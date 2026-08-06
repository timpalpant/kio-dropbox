/*
    SPDX-FileCopyrightText: 2026 Timothy Palpant <tim@palpant.us>
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "dropboxaccount.h"

#include <KLocalizedString>
#include <KWallet>

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

using namespace Qt::StringLiterals;

namespace
{
constexpr auto WalletFolder = "kio-dropbox";
constexpr auto WalletKey = "refresh-token";

// A refresh has to finish in a bounded time: the whole thing runs inside a
// nested event loop in a KIO worker, and a wedged socket would hang Dolphin.
constexpr int NetworkTimeoutMs = 30'000;

/*! Overridable so the test suite can stand in for Dropbox; not for normal use. */
QString tokenEndpoint()
{
    static const QString url = qEnvironmentVariable("KIO_DROPBOX_TOKEN_ENDPOINT", u"https://api.dropboxapi.com/oauth2/token"_s);
    return url;
}

/*!
 * Whether to use KWallet at all. Users on headless or wallet-less setups can
 * set KIO_DROPBOX_NO_WALLET=1 to keep the token in a 0600 file instead.
 */
bool walletUsable()
{
    if (qEnvironmentVariableIntValue("KIO_DROPBOX_NO_WALLET") != 0) {
        return false;
    }
    return KWallet::Wallet::isEnabled();
}

QString configFilePath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/kio-dropboxrc"_L1;
}

/*! Blocks on \a reply until it finishes or the timeout expires. */
void waitFor(QNetworkReply *reply)
{
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timeout, &QTimer::timeout, reply, &QNetworkReply::abort);
    timeout.start(NetworkTimeoutMs);
    loop.exec();
}

/*! Turns a token-endpoint failure into something worth showing a human. */
QString describeTokenError(QNetworkReply *reply, const QByteArray &body)
{
    const QJsonObject json = QJsonDocument::fromJson(body).object();
    const QString tag = json.value("error"_L1).toString();
    const QString description = json.value("error_description"_L1).toString();

    if (tag == "invalid_grant"_L1) {
        return i18n("Dropbox rejected the stored authorization. Please link the account again.");
    }
    if (tag == "invalid_client"_L1) {
        return i18n("Dropbox rejected the app key. Please check it and link the account again.");
    }
    if (!description.isEmpty()) {
        return description;
    }
    if (!tag.isEmpty()) {
        return tag;
    }
    if (reply->error() != QNetworkReply::NoError) {
        return reply->errorString();
    }
    return i18n("Unexpected reply from Dropbox while requesting an access token.");
}
} // namespace

DropboxAccount::DropboxAccount()
{
    load();
}

void DropboxAccount::load()
{
    QSettings config(configFilePath(), QSettings::IniFormat);
    config.beginGroup("Account"_L1);
    m_appKey = config.value("AppKey"_L1).toString();
    m_email = config.value("Email"_L1).toString();
    m_displayName = config.value("DisplayName"_L1).toString();
    config.endGroup();

    if (!readRefreshTokenFromWallet()) {
        readRefreshTokenFromFile();
    }

    loadCachedAccessToken();
}

QString DropboxAccount::builtInAppKey()
{
    return QString::fromLatin1(DROPBOX_BUILTIN_APP_KEY);
}

QString DropboxAccount::appKey() const
{
    return m_appKey.isEmpty() ? builtInAppKey() : m_appKey;
}

bool DropboxAccount::isConfigured() const
{
    return !appKey().isEmpty() && !m_refreshToken.isEmpty();
}

void DropboxAccount::setAccountInfo(const QString &email, const QString &displayName)
{
    m_email = email;
    m_displayName = displayName;
}

bool DropboxAccount::save(QString *errorText)
{
    QSettings config(configFilePath(), QSettings::IniFormat);
    config.beginGroup("Account"_L1);
    config.setValue("AppKey"_L1, m_appKey);
    config.setValue("Email"_L1, m_email);
    config.setValue("DisplayName"_L1, m_displayName);
    config.endGroup();
    config.sync();

    if (config.status() != QSettings::NoError) {
        if (errorText) {
            *errorText = i18n("Could not write %1.", configFilePath());
        }
        return false;
    }

    if (writeRefreshTokenToWallet()) {
        // Make sure a token left over from a previous wallet-less run doesn't
        // shadow the wallet copy on the next load().
        QFile::remove(refreshTokenFilePath());
        return true;
    }
    return writeRefreshTokenToFile(errorText);
}

void DropboxAccount::forget()
{
    m_appKey.clear();
    m_refreshToken.clear();
    m_email.clear();
    m_displayName.clear();
    invalidateAccessToken();

    QSettings config(configFilePath(), QSettings::IniFormat);
    config.remove("Account"_L1);
    config.sync();

    QFile::remove(refreshTokenFilePath());

    if (!walletUsable()) {
        return;
    }
    if (auto *wallet = KWallet::Wallet::openWallet(KWallet::Wallet::NetworkWallet(), 0, KWallet::Wallet::Synchronous)) {
        if (wallet->hasFolder(QLatin1String(WalletFolder))) {
            wallet->removeFolder(QLatin1String(WalletFolder));
        }
        delete wallet;
    }
}

// --- access tokens -----------------------------------------------------------

QString DropboxAccount::accessToken(QNetworkAccessManager *nam, QString *errorText)
{
    // A minute of slack so we don't hand out a token that expires mid-transfer.
    if (!m_accessToken.isEmpty() && m_accessTokenExpiry.isValid() && QDateTime::currentDateTimeUtc().addSecs(60) < m_accessTokenExpiry) {
        return m_accessToken;
    }

    if (!isConfigured()) {
        if (errorText) {
            *errorText = i18n("No Dropbox account has been linked yet.");
        }
        return {};
    }

    QUrlQuery form;
    form.addQueryItem("grant_type"_L1, "refresh_token"_L1);
    form.addQueryItem("refresh_token"_L1, m_refreshToken);
    form.addQueryItem("client_id"_L1, appKey());

    QNetworkRequest request{QUrl(tokenEndpoint())};
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded"_L1);

    QNetworkReply *reply = nam->post(request, form.toString(QUrl::FullyEncoded).toUtf8());
    waitFor(reply);

    const QByteArray body = reply->readAll();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    if (status != 200) {
        if (errorText) {
            *errorText = describeTokenError(reply, body);
        }
        reply->deleteLater();
        return {};
    }
    reply->deleteLater();

    const QJsonObject json = QJsonDocument::fromJson(body).object();
    m_accessToken = json.value("access_token"_L1).toString();
    const int expiresIn = json.value("expires_in"_L1).toInt(14400);

    if (m_accessToken.isEmpty()) {
        if (errorText) {
            *errorText = i18n("Dropbox returned an empty access token.");
        }
        return {};
    }

    m_accessTokenExpiry = QDateTime::currentDateTimeUtc().addSecs(expiresIn);
    storeCachedAccessToken();
    return m_accessToken;
}

void DropboxAccount::invalidateAccessToken()
{
    m_accessToken.clear();
    m_accessTokenExpiry = QDateTime();
    QFile::remove(cachedAccessTokenPath());
}

bool DropboxAccount::redeemAuthorizationCode(QNetworkAccessManager *nam, const QString &code, const QString &codeVerifier, QString *errorText)
{
    QUrlQuery form;
    form.addQueryItem("grant_type"_L1, "authorization_code"_L1);
    form.addQueryItem("code"_L1, code);
    form.addQueryItem("client_id"_L1, appKey());
    form.addQueryItem("code_verifier"_L1, codeVerifier);

    QNetworkRequest request{QUrl(tokenEndpoint())};
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded"_L1);

    QNetworkReply *reply = nam->post(request, form.toString(QUrl::FullyEncoded).toUtf8());
    waitFor(reply);

    const QByteArray body = reply->readAll();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    if (status != 200) {
        if (errorText) {
            *errorText = describeTokenError(reply, body);
        }
        reply->deleteLater();
        return false;
    }
    reply->deleteLater();

    const QJsonObject json = QJsonDocument::fromJson(body).object();
    m_refreshToken = json.value("refresh_token"_L1).toString();
    m_accessToken = json.value("access_token"_L1).toString();
    m_accessTokenExpiry = QDateTime::currentDateTimeUtc().addSecs(json.value("expires_in"_L1).toInt(14400));

    if (m_refreshToken.isEmpty()) {
        if (errorText) {
            // Happens when the authorize URL was built without token_access_type=offline.
            *errorText = i18n("Dropbox did not return a long-lived token. Please restart the authorization.");
        }
        return false;
    }
    return true;
}

QString DropboxAccount::authorizationUrl(const QString &appKey, const QString &codeChallenge)
{
    QUrlQuery query;
    query.addQueryItem("client_id"_L1, appKey);
    query.addQueryItem("response_type"_L1, "code"_L1);
    query.addQueryItem("code_challenge"_L1, codeChallenge);
    query.addQueryItem("code_challenge_method"_L1, "S256"_L1);
    // "offline" is what makes the token exchange hand back a refresh token.
    query.addQueryItem("token_access_type"_L1, "offline"_L1);

    QUrl url{"https://www.dropbox.com/oauth2/authorize"_L1};
    url.setQuery(query);
    return url.toString();
}

// --- storage -----------------------------------------------------------------

QString DropboxAccount::cachedAccessTokenPath()
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (dir.isEmpty()) {
        dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    }
    return dir + "/kio-dropbox-token.json"_L1;
}

void DropboxAccount::loadCachedAccessToken()
{
    QFile file(cachedAccessTokenPath());
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }

    const QJsonObject json = QJsonDocument::fromJson(file.readAll()).object();
    // Tie the cache to the key it was minted for, so relinking a different app
    // doesn't resurrect a stale token.
    if (json.value("app_key"_L1).toString() != appKey()) {
        return;
    }

    const QDateTime expiry = QDateTime::fromSecsSinceEpoch(json.value("expires_at"_L1).toInteger(), QTimeZone::UTC);
    if (expiry <= QDateTime::currentDateTimeUtc()) {
        return;
    }

    m_accessToken = json.value("access_token"_L1).toString();
    m_accessTokenExpiry = expiry;
}

void DropboxAccount::storeCachedAccessToken() const
{
    QFile file(cachedAccessTokenPath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return;
    }
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);

    QJsonObject json;
    json["app_key"_L1] = appKey();
    json["access_token"_L1] = m_accessToken;
    json["expires_at"_L1] = m_accessTokenExpiry.toSecsSinceEpoch();
    file.write(QJsonDocument(json).toJson(QJsonDocument::Compact));
}

QString DropboxAccount::refreshTokenFilePath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/kio-dropbox/refresh-token"_L1;
}

bool DropboxAccount::readRefreshTokenFromWallet()
{
    if (!walletUsable()) {
        return false;
    }

    std::unique_ptr<KWallet::Wallet> wallet{KWallet::Wallet::openWallet(KWallet::Wallet::NetworkWallet(), 0, KWallet::Wallet::Synchronous)};
    if (!wallet || !wallet->hasFolder(QLatin1String(WalletFolder)) || !wallet->setFolder(QLatin1String(WalletFolder))) {
        return false;
    }

    QString value;
    if (wallet->readPassword(QLatin1String(WalletKey), value) != 0 || value.isEmpty()) {
        return false;
    }
    m_refreshToken = value;
    return true;
}

bool DropboxAccount::writeRefreshTokenToWallet()
{
    if (!walletUsable()) {
        return false;
    }

    std::unique_ptr<KWallet::Wallet> wallet{KWallet::Wallet::openWallet(KWallet::Wallet::NetworkWallet(), 0, KWallet::Wallet::Synchronous)};
    if (!wallet) {
        return false;
    }
    if (!wallet->hasFolder(QLatin1String(WalletFolder)) && !wallet->createFolder(QLatin1String(WalletFolder))) {
        return false;
    }
    if (!wallet->setFolder(QLatin1String(WalletFolder))) {
        return false;
    }
    return wallet->writePassword(QLatin1String(WalletKey), m_refreshToken) == 0;
}

void DropboxAccount::readRefreshTokenFromFile()
{
    QFile file(refreshTokenFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }
    m_refreshToken = QString::fromUtf8(file.readAll()).trimmed();
}

bool DropboxAccount::writeRefreshTokenToFile(QString *errorText)
{
    const QString path = refreshTokenFilePath();
    QDir().mkpath(QFileInfo(path).absolutePath());

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorText) {
            *errorText = i18n("Could not store the Dropbox token in %1.", path);
        }
        return false;
    }
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    file.write(m_refreshToken.toUtf8());
    file.close();

    if (errorText) {
        *errorText = i18n("No KWallet is available, so the Dropbox token was stored unencrypted in %1.", path);
    }
    return true;
}
