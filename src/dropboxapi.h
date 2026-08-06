/*
    SPDX-FileCopyrightText: 2026 Timothy Palpant <tim@palpant.us>
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QJsonObject>
#include <QString>

#include <functional>

class DropboxAccount;
class QNetworkAccessManager;
class QNetworkRequest;

/*!
 * What went wrong, in terms this project cares about. The KIO worker maps these
 * onto KIO::Error; keeping them separate means the setup helper doesn't have to
 * link KIO.
 */
enum class DropboxErrorKind {
    None,
    Network,        //!< could not reach Dropbox, or the request timed out
    Authentication, //!< no linked account, or the refresh token was rejected
    NotFound,
    AlreadyExists,
    AccessDenied,
    InsufficientSpace,
    InvalidPath,
    RateLimited,
    Server, //!< Dropbox returned 5xx after we ran out of retries
    Unknown,
};

struct DropboxError {
    DropboxErrorKind kind = DropboxErrorKind::None;
    QString text;
    QString tag; //!< Dropbox's own error_summary, when there was one

    explicit operator bool() const { return kind != DropboxErrorKind::None; }
};

/*! One file or folder, as far as a file manager is concerned. */
struct DropboxEntry {
    bool isValid = false;
    bool isDir = false;
    QString name;        //!< the leaf name, in its display casing
    QString pathDisplay; //!< full Dropbox path, in its display casing
    qint64 size = 0;
    QDateTime modified;

    static DropboxEntry fromJson(const QJsonObject &json);
};

/*!
 * A blocking client for the bits of the Dropbox v2 API a file manager needs.
 *
 * Every call spins a nested event loop, which is the documented way to do
 * asynchronous work inside a KIO worker. Access-token refresh, rate-limit
 * backoff and 5xx retries are handled here so callers only see final outcomes.
 */
class DropboxApi
{
public:
    /*! \a account must outlive the DropboxApi. */
    explicit DropboxApi(DropboxAccount *account);
    ~DropboxApi();

    DropboxApi(const DropboxApi &) = delete;
    DropboxApi &operator=(const DropboxApi &) = delete;

    QNetworkAccessManager *networkAccessManager() const { return m_nam; }

    /*! Calls a JSON-in/JSON-out endpoint under https://api.dropboxapi.com/2/. */
    bool rpc(const QString &endpoint, const QJsonValue &args, QJsonObject *result, DropboxError *error);

    /*!
     * Streams a file's contents. \a onSize is called once with the total size
     * (or -1 if Dropbox didn't say), then \a onData is called repeatedly;
     * returning false from it aborts the transfer.
     */
    bool download(const QString &path,
                  const std::function<void(qint64)> &onSize,
                  const std::function<bool(const QByteArray &)> &onData,
                  DropboxError *error);

    /*! Calls a content endpoint under https://content.dropboxapi.com/2/, sending \a body. */
    bool contentUpload(const QString &endpoint, const QJsonObject &args, const QByteArray &body, QJsonObject *result, DropboxError *error);

    /*! Turns a KIO path ("/", "/a/b") into a Dropbox API path ("", "/a/b"). */
    static QString apiPath(const QString &kioPath);

    /*!
     * Serializes \a args for the Dropbox-API-Arg header. Dropbox requires that
     * header to be pure ASCII, so anything above U+007F is \\u-escaped.
     */
    static QByteArray asciiJson(const QJsonObject &args);

private:
    struct Response {
        int status = 0;
        QByteArray body;
        QByteArray apiResult; //!< the Dropbox-API-Result header, on downloads
        bool networkFailed = false;
        QString networkErrorText;
    };

    /*! Runs one request, retrying for token refresh, 429 and 5xx. */
    bool send(const QString &url,
              const QJsonObject *headerArgs,
              const QByteArray &body,
              const QByteArray &contentType,
              const std::function<void(qint64)> *onSize,
              const std::function<bool(const QByteArray &)> *onData,
              Response *response,
              DropboxError *error);

    bool authorize(QNetworkRequest *request, DropboxError *error);
    static DropboxError errorFromResponse(const Response &response);

    DropboxAccount *m_account;
    QNetworkAccessManager *m_nam;
};
