/*
    SPDX-FileCopyrightText: 2026 Timothy Palpant <tim@palpant.us>
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <QDateTime>
#include <QString>

class QNetworkAccessManager;

/*!
 * The credentials for the linked Dropbox account, and the machinery to turn the
 * long-lived refresh token into a usable bearer token.
 *
 * Three things are stored, in three different places, according to how secret
 * and how long-lived they are:
 *
 *  - the app key and the account's display info go in ~/.config/kio-dropboxrc,
 *  - the refresh token goes in KWallet (falling back to a 0600 file if there is
 *    no wallet available, e.g. on a headless box),
 *  - the access token is cached in $XDG_RUNTIME_DIR so that the short-lived
 *    worker processes Dolphin spawns don't each burn a token refresh.
 */
class DropboxAccount
{
public:
    DropboxAccount();

    /*! Loads whatever is on disk. Never fails; check isConfigured() afterwards. */
    void load();

    bool isConfigured() const;

    /*!
     * The key this account authenticates with: the user's own if they supplied
     * one, otherwise whatever was baked in at build time.
     */
    QString appKey() const;

    /*!
     * The key compiled in via -DDROPBOX_APP_KEY, or empty if the build didn't
     * set one. Empty means every user has to register an app of their own.
     *
     * This is not a secret. PKCE exists because native apps are public clients
     * that cannot hold one; the key only identifies which app is asking.
     */
    static QString builtInAppKey();

    /*! Whether the user supplied a key of their own, overriding the built-in. */
    bool hasCustomAppKey() const { return !m_appKey.isEmpty(); }
    QString refreshToken() const { return m_refreshToken; }
    QString accountEmail() const { return m_email; }
    QString accountName() const { return m_displayName; }

    void setAppKey(const QString &key) { m_appKey = key; }
    void setRefreshToken(const QString &token) { m_refreshToken = token; }
    void setAccountInfo(const QString &email, const QString &displayName);

    /*!
     * Writes the config file and stores the refresh token. On success \a errorText
     * may still be set, to warn that the token had to be stored unencrypted
     * because no wallet was available.
     */
    bool save(QString *errorText = nullptr);

    /*! Forgets every stored credential, including the cached access token. */
    void forget();

    /*!
     * Returns a bearer token that is valid right now, refreshing it against
     * Dropbox if the cached one has expired. Returns an empty string on
     * failure, with the reason in \a errorText.
     */
    QString accessToken(QNetworkAccessManager *nam, QString *errorText);

    /*! Drops the cached access token, so the next accessToken() refreshes. */
    void invalidateAccessToken();

    /*!
     * Exchanges an OAuth authorization code for a refresh token, and stores it
     * on this object (but does not save() it). Used by the setup helper.
     */
    bool redeemAuthorizationCode(QNetworkAccessManager *nam, const QString &code, const QString &codeVerifier, QString *errorText);

    /*! The URL the user has to visit to authorize a PKCE flow. */
    static QString authorizationUrl(const QString &appKey, const QString &codeChallenge);

private:
    void loadCachedAccessToken();
    void storeCachedAccessToken() const;
    static QString cachedAccessTokenPath();

    bool readRefreshTokenFromWallet();
    bool writeRefreshTokenToWallet();
    void readRefreshTokenFromFile();
    bool writeRefreshTokenToFile(QString *errorText);
    static QString refreshTokenFilePath();

    QString m_appKey;
    QString m_refreshToken;
    QString m_email;
    QString m_displayName;

    QString m_accessToken;
    QDateTime m_accessTokenExpiry;
};
