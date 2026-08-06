/*
    SPDX-FileCopyrightText: 2026 Timothy Palpant <tim@palpant.us>
    SPDX-License-Identifier: GPL-2.0-or-later

    A KIO worker that exposes a Dropbox account as dropbox:/ .
*/

#include "dropboxaccount.h"
#include "dropboxapi.h"

#include <KIO/WorkerBase>
#include <KLocalizedString>

#include <QCoreApplication>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QMimeDatabase>
#include <QProcess>
#include <QStandardPaths>
#include <QUrl>

#include <sys/stat.h>

using namespace Qt::StringLiterals;
using namespace KIO;

namespace {
/*! Upload chunk size. Dropbox requires session chunks to be a multiple of 4 MiB. */
constexpr qint64 UploadChunkSize = 8 * 1024 * 1024;

/*! Above this, Dropbox refuses a single-shot upload and a session is required. */
constexpr qint64 SingleShotUploadLimit = 150 * 1024 * 1024;
static_assert(UploadChunkSize <= SingleShotUploadLimit,
              "put() only uses a single-shot upload when the whole file fit in one chunk, "
              "so the chunk size must stay under what Dropbox accepts in one request");

int toKioError(DropboxErrorKind kind)
{
    switch (kind) {
    case DropboxErrorKind::None:
        return 0;
    case DropboxErrorKind::Network:
        return ERR_CANNOT_CONNECT;
    case DropboxErrorKind::Authentication:
        return ERR_CANNOT_LOGIN;
    case DropboxErrorKind::NotFound:
        return ERR_DOES_NOT_EXIST;
    case DropboxErrorKind::AlreadyExists:
        return ERR_FILE_ALREADY_EXIST;
    case DropboxErrorKind::AccessDenied:
        return ERR_ACCESS_DENIED;
    case DropboxErrorKind::InsufficientSpace:
        return ERR_DISK_FULL;
    case DropboxErrorKind::InvalidPath:
    case DropboxErrorKind::RateLimited:
    case DropboxErrorKind::Server:
    case DropboxErrorKind::Unknown:
        break;
    }
    return ERR_WORKER_DEFINED;
}

WorkerResult toWorkerResult(const DropboxError &error, const QUrl &url)
{
    const int code = toKioError(error.kind);
    // The errors KIO renders from a path read much better with the path.
    switch (code) {
    case ERR_DOES_NOT_EXIST:
    case ERR_FILE_ALREADY_EXIST:
    case ERR_ACCESS_DENIED:
        return WorkerResult::fail(code, url.toDisplayString());
    default:
        return WorkerResult::fail(code, error.text);
    }
}
} // namespace

class DropboxWorker : public WorkerBase
{
public:
    DropboxWorker(const QByteArray &pool, const QByteArray &app);

    Q_REQUIRED_RESULT WorkerResult listDir(const QUrl &url) override;
    Q_REQUIRED_RESULT WorkerResult stat(const QUrl &url) override;
    Q_REQUIRED_RESULT WorkerResult get(const QUrl &url) override;
    Q_REQUIRED_RESULT WorkerResult put(const QUrl &url, int permissions, JobFlags flags) override;
    Q_REQUIRED_RESULT WorkerResult mkdir(const QUrl &url, int permissions) override;
    Q_REQUIRED_RESULT WorkerResult del(const QUrl &url, bool isfile) override;
    Q_REQUIRED_RESULT WorkerResult rename(const QUrl &src, const QUrl &dest, JobFlags flags) override;
    Q_REQUIRED_RESULT WorkerResult copy(const QUrl &src, const QUrl &dest, int permissions, JobFlags flags) override;
    Q_REQUIRED_RESULT WorkerResult mimetype(const QUrl &url) override;
    Q_REQUIRED_RESULT WorkerResult fileSystemFreeSpace(const QUrl &url) override;

private:
    /*! Ensures there is a linked account, offering to run the setup helper if not. */
    Q_REQUIRED_RESULT WorkerResult requireAccount();

    UDSEntry toUdsEntry(const DropboxEntry &entry) const;
    UDSEntry rootEntry() const;

    /*! Server-side copy or move; \a endpoint is "files/copy_v2" or "files/move_v2". */
    Q_REQUIRED_RESULT WorkerResult transfer(const QString &endpoint, const QUrl &src, const QUrl &dest, JobFlags flags);

    Q_REQUIRED_RESULT WorkerResult uploadSingleShot(const QString &path, const QByteArray &data, JobFlags flags);
    Q_REQUIRED_RESULT WorkerResult uploadSession(const QString &path, const QByteArray &firstChunk, JobFlags flags);

    /*!
     * Pulls up to \a limit bytes of the file being written from the job, setting
     * \a atEnd when the source runs dry. Returns false if the source errored.
     */
    bool readChunk(QByteArray &chunk, qint64 limit, bool *atEnd);

    static QJsonObject commitInfo(const QString &path, JobFlags flags);

    DropboxAccount m_account;
    DropboxApi m_api;
    QMimeDatabase m_mimeDb;
    bool m_offeredSetup = false;
};

DropboxWorker::DropboxWorker(const QByteArray &pool, const QByteArray &app)
    : WorkerBase("dropbox", pool, app)
    , m_api(&m_account)
{}

// --- account -----------------------------------------------------------------

WorkerResult DropboxWorker::requireAccount()
{
    if (m_account.isConfigured()) {
        return WorkerResult::pass();
    }

    // Re-read: the helper may have linked an account since this worker started.
    m_account.load();
    if (m_account.isConfigured()) {
        return WorkerResult::pass();
    }

    if (!m_offeredSetup) {
        m_offeredSetup = true;
        const QString helper = QStandardPaths::findExecutable("kio-dropbox-auth"_L1);
        if (!helper.isEmpty()) {
            const int answer = messageBox(QuestionTwoActions,
                                          i18n("No Dropbox account is linked yet. Would you like to link one now?"),
                                          i18n("Dropbox"),
                                          i18n("Link Account…"),
                                          i18n("Cancel"));
            if (answer == PrimaryAction) {
                QProcess::startDetached(helper, {});
                return WorkerResult::fail(ERR_CANNOT_LOGIN,
                                          i18n("Finish linking the account in the window that just opened, then reload this folder."));
            }
        }
    }

    return WorkerResult::fail(ERR_CANNOT_LOGIN, i18n("No Dropbox account is linked. Run kio-dropbox-auth to link one."));
}

// --- entry conversion --------------------------------------------------------

UDSEntry DropboxWorker::rootEntry() const
{
    UDSEntry entry;
    entry.fastInsert(UDSEntry::UDS_NAME, "."_L1);
    entry.fastInsert(UDSEntry::UDS_DISPLAY_NAME, m_account.accountEmail().isEmpty() ? i18n("Dropbox") : m_account.accountEmail());
    entry.fastInsert(UDSEntry::UDS_FILE_TYPE, S_IFDIR);
    entry.fastInsert(UDSEntry::UDS_ACCESS, S_IRWXU);
    entry.fastInsert(UDSEntry::UDS_MIME_TYPE, "inode/directory"_L1);
    entry.fastInsert(UDSEntry::UDS_ICON_NAME, "io.github.timpalpant.kio-dropbox"_L1);
    return entry;
}

UDSEntry DropboxWorker::toUdsEntry(const DropboxEntry &item) const
{
    UDSEntry entry;
    entry.fastInsert(UDSEntry::UDS_NAME, item.name);

    if (item.isDir) {
        entry.fastInsert(UDSEntry::UDS_FILE_TYPE, S_IFDIR);
        entry.fastInsert(UDSEntry::UDS_ACCESS, S_IRWXU);
        entry.fastInsert(UDSEntry::UDS_MIME_TYPE, "inode/directory"_L1);
    } else {
        entry.fastInsert(UDSEntry::UDS_FILE_TYPE, S_IFREG);
        entry.fastInsert(UDSEntry::UDS_ACCESS, S_IRUSR | S_IWUSR);
        entry.fastInsert(UDSEntry::UDS_SIZE, item.size);
        // Dropbox has no per-file MIME type, so go by the name. It is a guess,
        // which lets KIO re-determine the real type when the file is opened.
        const QMimeType mime = m_mimeDb.mimeTypeForFile(item.name, QMimeDatabase::MatchExtension);
        entry.fastInsert(UDSEntry::UDS_GUESSED_MIME_TYPE, mime.name());
    }

    if (item.modified.isValid()) {
        entry.fastInsert(UDSEntry::UDS_MODIFICATION_TIME, item.modified.toSecsSinceEpoch());
    }
    return entry;
}

// --- listing and stat --------------------------------------------------------

WorkerResult DropboxWorker::listDir(const QUrl &url)
{
    if (const auto result = requireAccount(); !result.success()) {
        return result;
    }

    const QString path = DropboxApi::apiPath(url.path());

    QJsonObject args;
    args["path"_L1] = path;
    args["recursive"_L1] = false;
    args["include_deleted"_L1] = false;
    args["include_mounted_folders"_L1] = true;
    args["include_non_downloadable_files"_L1] = true;

    QString endpoint = "files/list_folder"_L1;
    uint count = 0;

    while (true) {
        QJsonObject result;
        DropboxError error;
        if (!m_api.rpc(endpoint, args, &result, &error)) {
            return toWorkerResult(error, url);
        }

        const QJsonArray entries = result.value("entries"_L1).toArray();
        UDSEntryList batch;
        batch.reserve(entries.size());
        for (const QJsonValue &value : entries) {
            const DropboxEntry item = DropboxEntry::fromJson(value.toObject());
            if (item.isValid) {
                batch.append(toUdsEntry(item));
            }
        }
        if (!batch.isEmpty()) {
            listEntries(batch);
            count += batch.size();
        }

        if (!result.value("has_more"_L1).toBool()) {
            break;
        }
        // Subsequent pages are fetched by cursor from a different endpoint.
        endpoint = "files/list_folder/continue"_L1;
        args = QJsonObject{{"cursor"_L1, result.value("cursor"_L1)}};
    }

    totalSize(count);
    return WorkerResult::pass();
}

WorkerResult DropboxWorker::stat(const QUrl &url)
{
    if (const auto result = requireAccount(); !result.success()) {
        return result;
    }

    const QString path = DropboxApi::apiPath(url.path());
    if (path.isEmpty()) {
        // get_metadata refuses the root, so describe it ourselves.
        statEntry(rootEntry());
        return WorkerResult::pass();
    }

    QJsonObject result;
    DropboxError error;
    if (!m_api.rpc("files/get_metadata"_L1, QJsonObject{{"path"_L1, path}}, &result, &error)) {
        return toWorkerResult(error, url);
    }

    const DropboxEntry item = DropboxEntry::fromJson(result);
    if (!item.isValid) {
        return WorkerResult::fail(ERR_DOES_NOT_EXIST, url.toDisplayString());
    }

    statEntry(toUdsEntry(item));
    return WorkerResult::pass();
}

WorkerResult DropboxWorker::mimetype(const QUrl &url)
{
    if (const auto result = requireAccount(); !result.success()) {
        return result;
    }

    const QString path = DropboxApi::apiPath(url.path());
    if (path.isEmpty()) {
        mimeType("inode/directory"_L1);
        return WorkerResult::pass();
    }

    QJsonObject result;
    DropboxError error;
    if (!m_api.rpc("files/get_metadata"_L1, QJsonObject{{"path"_L1, path}}, &result, &error)) {
        return toWorkerResult(error, url);
    }

    const DropboxEntry item = DropboxEntry::fromJson(result);
    if (!item.isValid) {
        return WorkerResult::fail(ERR_DOES_NOT_EXIST, url.toDisplayString());
    }

    mimeType(item.isDir ? "inode/directory"_L1 : m_mimeDb.mimeTypeForFile(item.name, QMimeDatabase::MatchExtension).name());
    return WorkerResult::pass();
}

// --- reading -----------------------------------------------------------------

WorkerResult DropboxWorker::get(const QUrl &url)
{
    if (const auto result = requireAccount(); !result.success()) {
        return result;
    }

    const QString path = DropboxApi::apiPath(url.path());
    if (path.isEmpty()) {
        return WorkerResult::fail(ERR_IS_DIRECTORY, url.toDisplayString());
    }

    // KIO wants the MIME type before the first data() call.
    mimeType(m_mimeDb.mimeTypeForFile(url.fileName(), QMimeDatabase::MatchExtension).name());

    qint64 received = 0;
    DropboxError error;
    const bool ok = m_api.download(
        path,
        [this](qint64 total) {
            if (total >= 0) {
                totalSize(total);
            }
        },
        [this, &received](const QByteArray &chunk) {
            data(chunk);
            received += chunk.size();
            processedSize(received);
            return true;
        },
        &error);

    if (!ok) {
        return toWorkerResult(error, url);
    }

    data(QByteArray()); // end of stream
    return WorkerResult::pass();
}

// --- writing -----------------------------------------------------------------

QJsonObject DropboxWorker::commitInfo(const QString &path, JobFlags flags)
{
    QJsonObject commit;
    commit["path"_L1] = path;
    commit["mode"_L1] = (flags & Overwrite) ? "overwrite"_L1 : "add"_L1;
    commit["autorename"_L1] = false;
    commit["mute"_L1] = false;
    // Without this, "add" silently renames on conflict instead of failing, and
    // KIO would never get the chance to ask the user about overwriting.
    commit["strict_conflict"_L1] = !(flags & Overwrite);
    return commit;
}

WorkerResult DropboxWorker::uploadSingleShot(const QString &path, const QByteArray &data, JobFlags flags)
{
    DropboxError error;
    if (!m_api.contentUpload("files/upload"_L1, commitInfo(path, flags), data, nullptr, &error)) {
        return toWorkerResult(error, QUrl("dropbox:"_L1 + path));
    }
    return WorkerResult::pass();
}

bool DropboxWorker::readChunk(QByteArray &chunk, qint64 limit, bool *atEnd)
{
    chunk.clear();
    chunk.reserve(limit);
    *atEnd = false;

    while (chunk.size() < limit) {
        // KIO's data pull is a handshake: ask, then read. Skipping dataReq()
        // leaves readData() waiting for a message that is never sent.
        dataReq();

        QByteArray part;
        const int read = readData(part);
        if (read < 0) {
            return false;
        }
        if (read == 0) {
            *atEnd = true;
            break;
        }
        chunk.append(part);
    }
    return true;
}

WorkerResult DropboxWorker::uploadSession(const QString &path, const QByteArray &firstChunk, JobFlags flags)
{
    QJsonObject result;
    DropboxError error;

    if (!m_api.contentUpload("files/upload_session/start"_L1, QJsonObject{{"close"_L1, false}}, firstChunk, &result, &error)) {
        return toWorkerResult(error, QUrl("dropbox:"_L1 + path));
    }

    const QString sessionId = result.value("session_id"_L1).toString();
    qint64 offset = firstChunk.size();
    processedSize(offset);

    while (true) {
        QByteArray chunk;
        bool isLast = false;
        if (!readChunk(chunk, UploadChunkSize, &isLast)) {
            return WorkerResult::fail(ERR_CANNOT_READ, QString());
        }

        QJsonObject cursor{{"session_id"_L1, sessionId}, {"offset"_L1, offset}};

        if (isLast) {
            QJsonObject args{{"cursor"_L1, cursor}, {"commit"_L1, commitInfo(path, flags)}};
            if (!m_api.contentUpload("files/upload_session/finish"_L1, args, chunk, nullptr, &error)) {
                return toWorkerResult(error, QUrl("dropbox:"_L1 + path));
            }
            offset += chunk.size();
            processedSize(offset);
            return WorkerResult::pass();
        }

        QJsonObject args{{"cursor"_L1, cursor}, {"close"_L1, false}};
        if (!m_api.contentUpload("files/upload_session/append_v2"_L1, args, chunk, nullptr, &error)) {
            return toWorkerResult(error, QUrl("dropbox:"_L1 + path));
        }
        offset += chunk.size();
        processedSize(offset);
    }
}

WorkerResult DropboxWorker::put(const QUrl &url, int permissions, JobFlags flags)
{
    Q_UNUSED(permissions)

    if (const auto result = requireAccount(); !result.success()) {
        return result;
    }

    const QString path = DropboxApi::apiPath(url.path());
    if (path.isEmpty()) {
        return WorkerResult::fail(ERR_IS_DIRECTORY, url.toDisplayString());
    }

    // Buffer the first chunk to find out whether this is small enough for a
    // one-shot upload; anything larger streams through an upload session, so
    // the whole file never has to fit in memory.
    QByteArray head;
    bool wholeFile = false;
    if (!readChunk(head, UploadChunkSize, &wholeFile)) {
        return WorkerResult::fail(ERR_CANNOT_READ, url.toDisplayString());
    }

    if (wholeFile) {
        const auto result = uploadSingleShot(path, head, flags);
        if (result.success()) {
            processedSize(head.size());
        }
        return result;
    }

    return uploadSession(path, head, flags);
}

WorkerResult DropboxWorker::mkdir(const QUrl &url, int permissions)
{
    Q_UNUSED(permissions)

    if (const auto result = requireAccount(); !result.success()) {
        return result;
    }

    const QString path = DropboxApi::apiPath(url.path());
    if (path.isEmpty()) {
        return WorkerResult::fail(ERR_FILE_ALREADY_EXIST, url.toDisplayString());
    }

    DropboxError error;
    if (!m_api.rpc("files/create_folder_v2"_L1, QJsonObject{{"path"_L1, path}, {"autorename"_L1, false}}, nullptr, &error)) {
        return toWorkerResult(error, url);
    }
    return WorkerResult::pass();
}

WorkerResult DropboxWorker::del(const QUrl &url, bool isfile)
{
    Q_UNUSED(isfile)

    if (const auto result = requireAccount(); !result.success()) {
        return result;
    }

    const QString path = DropboxApi::apiPath(url.path());
    if (path.isEmpty()) {
        return WorkerResult::fail(ERR_ACCESS_DENIED, url.toDisplayString());
    }

    DropboxError error;
    if (!m_api.rpc("files/delete_v2"_L1, QJsonObject{{"path"_L1, path}}, nullptr, &error)) {
        return toWorkerResult(error, url);
    }
    return WorkerResult::pass();
}

WorkerResult DropboxWorker::transfer(const QString &endpoint, const QUrl &src, const QUrl &dest, JobFlags flags)
{
    if (const auto result = requireAccount(); !result.success()) {
        return result;
    }

    if (src.scheme() != "dropbox"_L1 || dest.scheme() != "dropbox"_L1) {
        // Let KIO fall back to streaming the file through get()/put().
        return WorkerResult::fail(ERR_UNSUPPORTED_ACTION, QString());
    }

    const QString from = DropboxApi::apiPath(src.path());
    const QString to = DropboxApi::apiPath(dest.path());
    if (from.isEmpty() || to.isEmpty()) {
        return WorkerResult::fail(ERR_ACCESS_DENIED, src.toDisplayString());
    }

    // Dropbox has no overwriting move or copy, so clear the way first. KIO has
    // already confirmed the overwrite with the user by this point.
    if (flags & Overwrite) {
        DropboxError ignored;
        m_api.rpc("files/delete_v2"_L1, QJsonObject{{"path"_L1, to}}, nullptr, &ignored);
    }

    QJsonObject args;
    args["from_path"_L1] = from;
    args["to_path"_L1] = to;
    args["autorename"_L1] = false;

    DropboxError error;
    if (!m_api.rpc(endpoint, args, nullptr, &error)) {
        return toWorkerResult(error, dest);
    }
    return WorkerResult::pass();
}

WorkerResult DropboxWorker::rename(const QUrl &src, const QUrl &dest, JobFlags flags)
{
    return transfer("files/move_v2"_L1, src, dest, flags);
}

WorkerResult DropboxWorker::copy(const QUrl &src, const QUrl &dest, int permissions, JobFlags flags)
{
    Q_UNUSED(permissions)
    return transfer("files/copy_v2"_L1, src, dest, flags);
}

// --- misc --------------------------------------------------------------------

WorkerResult DropboxWorker::fileSystemFreeSpace(const QUrl &url)
{
    if (const auto result = requireAccount(); !result.success()) {
        return result;
    }

    QJsonObject result;
    DropboxError error;
    if (!m_api.rpc("users/get_space_usage"_L1, QJsonValue::Null, &result, &error)) {
        return toWorkerResult(error, url);
    }

    const qint64 used = result.value("used"_L1).toInteger();
    const QJsonObject allocation = result.value("allocation"_L1).toObject();
    // Team members report their allocation under a different key.
    qint64 total = allocation.value("allocated"_L1).toInteger();
    if (total == 0) {
        total = allocation.value("user_within_team_space_allocated"_L1).toInteger();
    }
    if (total == 0) {
        return WorkerResult::fail(ERR_UNSUPPORTED_ACTION, QString());
    }

    setMetaData("total"_L1, QString::number(total));
    setMetaData("available"_L1, QString::number(qMax<qint64>(0, total - used)));
    return WorkerResult::pass();
}

// --- plugin entry point ------------------------------------------------------

class KIOPluginForMetaData : public QObject
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.kde.kio.worker.dropbox" FILE "dropbox.json")
};

extern "C" int Q_DECL_EXPORT kdemain(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    app.setApplicationName("kio_dropbox"_L1);

    if (argc != 4) {
        fprintf(stderr, "Usage: kio_dropbox protocol domain-socket1 domain-socket2\n");
        return -1;
    }

    DropboxWorker worker(argv[2], argv[3]);
    worker.dispatchLoop();
    return 0;
}

#include "kio_dropbox.moc"
