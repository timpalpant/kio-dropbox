/*
    SPDX-FileCopyrightText: 2026 Timothy Palpant <tim@palpant.us>
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "dropboxapi.h"
#include "dropboxaccount.h"

#include <KLocalizedString>

#include <QEventLoop>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

using namespace Qt::StringLiterals;

namespace
{
constexpr int RequestTimeoutMs = 120'000;
constexpr int MaxRetries = 4;

/*! Sleeps without blocking the nested event loop the caller is already in. */
void pause(int milliseconds)
{
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}

/*!
 * How long to wait before retrying, honoring Retry-After when Dropbox sets it
 * and otherwise backing off exponentially.
 */
int retryDelayMs(QNetworkReply *reply, int attempt)
{
    bool ok = false;
    const int retryAfter = reply->rawHeader("Retry-After").toInt(&ok);
    if (ok && retryAfter > 0) {
        return qMin(retryAfter, 60) * 1000;
    }
    return 500 * (1 << attempt);
}

/*!
 * The API roots, overridable so the test suite can point the worker at a local
 * stand-in for Dropbox. Not meant to be set in normal use.
 */
QString apiBase()
{
    static const QString base = qEnvironmentVariable("KIO_DROPBOX_API_BASE", u"https://api.dropboxapi.com/2/"_s);
    return base;
}

QString contentBase()
{
    static const QString base = qEnvironmentVariable("KIO_DROPBOX_CONTENT_BASE", u"https://content.dropboxapi.com/2/"_s);
    return base;
}
} // namespace

// --- DropboxEntry ------------------------------------------------------------

DropboxEntry DropboxEntry::fromJson(const QJsonObject &json)
{
    DropboxEntry entry;
    const QString tag = json.value(".tag"_L1).toString();
    if (tag != "file"_L1 && tag != "folder"_L1) {
        return entry;
    }

    entry.isValid = true;
    entry.isDir = (tag == "folder"_L1);
    entry.name = json.value("name"_L1).toString();
    entry.pathDisplay = json.value("path_display"_L1).toString();

    if (!entry.isDir) {
        entry.size = json.value("size"_L1).toInteger();
        // server_modified is when Dropbox accepted the file; client_modified is
        // what the uploading client claimed. The latter is what users expect to
        // see, so prefer it and fall back.
        QString stamp = json.value("client_modified"_L1).toString();
        if (stamp.isEmpty()) {
            stamp = json.value("server_modified"_L1).toString();
        }
        entry.modified = QDateTime::fromString(stamp, Qt::ISODate);
    }
    return entry;
}

// --- DropboxApi --------------------------------------------------------------

DropboxApi::DropboxApi(DropboxAccount *account)
    : m_account(account)
    , m_nam(new QNetworkAccessManager)
{
    m_nam->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
}

DropboxApi::~DropboxApi()
{
    delete m_nam;
}

QString DropboxApi::apiPath(const QString &kioPath)
{
    QString path = kioPath;
    while (path.endsWith(u'/')) {
        path.chop(1);
    }
    if (!path.isEmpty() && !path.startsWith(u'/')) {
        path.prepend(u'/');
    }
    // Dropbox spells the root as the empty string, not "/".
    return path;
}

QByteArray DropboxApi::asciiJson(const QJsonObject &args)
{
    const QByteArray utf8 = QJsonDocument(args).toJson(QJsonDocument::Compact);
    const QString text = QString::fromUtf8(utf8);

    QByteArray ascii;
    ascii.reserve(utf8.size());
    for (const QChar ch : text) {
        if (ch.unicode() < 0x80) {
            ascii.append(static_cast<char>(ch.unicode()));
        } else {
            ascii.append(QByteArray::number(ch.unicode(), 16).rightJustified(4, '0').prepend("\\u"));
        }
    }
    return ascii;
}

bool DropboxApi::authorize(QNetworkRequest *request, DropboxError *error)
{
    QString reason;
    const QString token = m_account->accessToken(m_nam, &reason);
    if (token.isEmpty()) {
        if (error) {
            *error = {DropboxErrorKind::Authentication, reason, {}};
        }
        return false;
    }
    request->setRawHeader("Authorization", "Bearer " + token.toUtf8());
    return true;
}

DropboxError DropboxApi::errorFromResponse(const Response &response)
{
    if (response.networkFailed) {
        return {DropboxErrorKind::Network, response.networkErrorText, {}};
    }

    const QJsonObject json = QJsonDocument::fromJson(response.body).object();
    const QString summary = json.value("error_summary"_L1).toString();
    const QString detail = summary.isEmpty() ? QString::fromUtf8(response.body).trimmed() : summary;

    switch (response.status) {
    case 401:
        return {DropboxErrorKind::Authentication, i18n("Dropbox rejected the access token."), summary};
    case 403:
        return {DropboxErrorKind::AccessDenied, i18n("Dropbox denied access: %1", detail), summary};
    case 429:
        return {DropboxErrorKind::RateLimited, i18n("Dropbox is rate-limiting this account. Please try again shortly."), summary};
    default:
        break;
    }

    if (response.status >= 500) {
        return {DropboxErrorKind::Server, i18n("Dropbox reported a server error (HTTP %1).", response.status), summary};
    }

    // 409 is Dropbox's catch-all for "the request was well formed but the
    // operation cannot be performed"; the error_summary says which.
    if (summary.contains("not_found"_L1)) {
        return {DropboxErrorKind::NotFound, i18n("No such file or folder in Dropbox."), summary};
    }
    if (summary.contains("conflict"_L1)) {
        return {DropboxErrorKind::AlreadyExists, i18n("A Dropbox item with that name already exists."), summary};
    }
    if (summary.contains("insufficient_space"_L1)) {
        return {DropboxErrorKind::InsufficientSpace, i18n("There is not enough space left in this Dropbox account."), summary};
    }
    if (summary.contains("malformed_path"_L1) || summary.contains("disallowed_name"_L1)) {
        return {DropboxErrorKind::InvalidPath, i18n("Dropbox does not accept that name: %1", detail), summary};
    }
    if (summary.contains("no_write_permission"_L1) || summary.contains("team_folder"_L1) || summary.contains("restricted_content"_L1)) {
        return {DropboxErrorKind::AccessDenied, i18n("Dropbox denied access: %1", detail), summary};
    }

    return {DropboxErrorKind::Unknown, detail.isEmpty() ? i18n("Dropbox returned HTTP %1.", response.status) : detail, summary};
}

bool DropboxApi::send(const QString &url,
                      const QJsonObject *headerArgs,
                      const QByteArray &body,
                      const QByteArray &contentType,
                      const std::function<void(qint64)> *onSize,
                      const std::function<bool(const QByteArray &)> *onData,
                      Response *response,
                      DropboxError *error)
{
    bool refreshed = false;

    for (int attempt = 0; attempt < MaxRetries; ++attempt) {
        QNetworkRequest request{QUrl(url)};
        if (!authorize(&request, error)) {
            return false;
        }
        if (headerArgs) {
            request.setRawHeader("Dropbox-API-Arg", asciiJson(*headerArgs));
        }
        if (!contentType.isEmpty()) {
            request.setRawHeader("Content-Type", contentType);
        }

        QNetworkReply *reply = m_nam->post(request, body);

        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        QObject::connect(&timeout, &QTimer::timeout, reply, &QNetworkReply::abort);

        // Streaming downloads consume the body as it lands rather than letting
        // it pile up in memory; every chunk also refreshes the timeout, so a
        // slow-but-progressing transfer is never killed.
        bool abortedByCallback = false;
        int status = 0;
        bool announcedSize = false;

        if (onData) {
            QObject::connect(reply, &QNetworkReply::readyRead, reply, [&] {
                timeout.start(RequestTimeoutMs);

                status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                if (status != 200) {
                    return; // an error body; collect it below instead of streaming it
                }

                if (!announcedSize && onSize) {
                    qint64 total = -1;
                    const QJsonObject meta = QJsonDocument::fromJson(reply->rawHeader("Dropbox-API-Result")).object();
                    if (meta.contains("size"_L1)) {
                        total = meta.value("size"_L1).toInteger();
                    } else {
                        total = reply->header(QNetworkRequest::ContentLengthHeader).toLongLong(nullptr);
                    }
                    (*onSize)(total);
                    announcedSize = true;
                }

                const QByteArray chunk = reply->readAll();
                if (!chunk.isEmpty() && !(*onData)(chunk)) {
                    abortedByCallback = true;
                    reply->abort();
                }
            });
        }

        // Likewise for uploads: as long as bytes are moving, don't time out.
        QObject::connect(reply, &QNetworkReply::uploadProgress, reply, [&] {
            timeout.start(RequestTimeoutMs);
        });

        timeout.start(RequestTimeoutMs);
        loop.exec();

        status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 200 && onSize && !announcedSize) {
            // A zero-byte file never emits readyRead, so announce the size here.
            (*onSize)(0);
            announcedSize = true;
        }
        const bool timedOut = !timeout.isActive() && reply->error() == QNetworkReply::OperationCanceledError;

        response->status = status;
        response->apiResult = reply->rawHeader("Dropbox-API-Result");
        response->body = reply->readAll();
        response->networkFailed = false;
        response->networkErrorText.clear();

        if (abortedByCallback) {
            reply->deleteLater();
            return false; // the caller already knows why; no error to report
        }

        if (reply->error() != QNetworkReply::NoError && status == 0) {
            response->networkFailed = true;
            response->networkErrorText = timedOut ? i18n("The request to Dropbox timed out.") : reply->errorString();
        }

        // One silent retry after a token refresh: the cached access token may
        // have been revoked, or another process may have rotated it.
        if (status == 401 && !refreshed) {
            refreshed = true;
            m_account->invalidateAccessToken();
            reply->deleteLater();
            continue;
        }

        const bool retryable = (status == 429 || status >= 500 || response->networkFailed);
        if (retryable && attempt + 1 < MaxRetries) {
            const int delay = retryDelayMs(reply, attempt);
            reply->deleteLater();
            pause(delay);
            continue;
        }

        reply->deleteLater();

        if (status == 200) {
            return true;
        }
        if (error) {
            *error = errorFromResponse(*response);
        }
        return false;
    }

    if (error && !*error) {
        *error = {DropboxErrorKind::Network, i18n("Dropbox could not be reached."), {}};
    }
    return false;
}

bool DropboxApi::rpc(const QString &endpoint, const QJsonValue &args, QJsonObject *result, DropboxError *error)
{
    // Dropbox wants a JSON body here, and the literal `null` for the handful of
    // endpoints that take no arguments.
    QByteArray body;
    if (args.isNull() || args.isUndefined()) {
        body = "null";
    } else if (args.isObject()) {
        body = QJsonDocument(args.toObject()).toJson(QJsonDocument::Compact);
    } else {
        body = QJsonDocument::fromVariant(args.toVariant()).toJson(QJsonDocument::Compact);
    }

    Response response;
    if (!send(apiBase() + endpoint, nullptr, body, "application/json", nullptr, nullptr, &response, error)) {
        return false;
    }
    if (result) {
        *result = QJsonDocument::fromJson(response.body).object();
    }
    return true;
}

bool DropboxApi::download(const QString &path,
                          const std::function<void(qint64)> &onSize,
                          const std::function<bool(const QByteArray &)> &onData,
                          DropboxError *error)
{
    QJsonObject args;
    args["path"_L1] = path;

    Response response;
    // No Content-Type: the download endpoint takes an empty body and rejects
    // requests that claim to be sending JSON.
    return send(contentBase() + "files/download"_L1, &args, {}, {}, &onSize, &onData, &response, error);
}

bool DropboxApi::contentUpload(const QString &endpoint, const QJsonObject &args, const QByteArray &body, QJsonObject *result, DropboxError *error)
{
    Response response;
    if (!send(contentBase() + endpoint, &args, body, "application/octet-stream", nullptr, nullptr, &response, error)) {
        return false;
    }
    if (result) {
        *result = QJsonDocument::fromJson(response.body).object();
    }
    return true;
}
