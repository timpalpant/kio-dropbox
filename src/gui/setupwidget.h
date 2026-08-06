/*
    SPDX-FileCopyrightText: 2026 Timothy Palpant <tim@palpant.us>
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "dropboxaccount.h"

#include <QWidget>

class QCheckBox;
class QLabel;
class QLineEdit;
class QNetworkAccessManager;
class QPushButton;
class QStackedWidget;

/*!
 * The account-linking UI, hosted both by the standalone kio-dropbox-auth dialog
 * and by the System Settings module.
 *
 * Dropbox has no public client credentials to embed, so each user registers
 * their own app and pastes its key here. The OAuth exchange then uses PKCE with
 * no redirect URI, which is the one flow Dropbox offers that needs no redirect
 * registered against the app and no local web server: Dropbox simply shows the
 * authorization code on screen for the user to copy back.
 *
 * Everything here applies immediately rather than on an Apply button — linking
 * an account is an action, not a pending setting.
 */
class SetupWidget : public QWidget
{
    Q_OBJECT

public:
    explicit SetupWidget(QWidget *parent = nullptr);

    /*! Re-reads the stored account and shows the page that matches it. */
    void refresh();

private:
    QWidget *buildLinkedPage();
    QWidget *buildConnectPage();
    QWidget *buildAppKeyPage();
    QWidget *buildAuthorizePage();

    /*! The page to start on: Connect if a key is baked in, App Key otherwise. */
    int startPage() const;

    void showLinked();
    void openAuthorizationPage();
    void finishAuthorization();
    void unlink();

    /*! Confirms the new token works and records who it belongs to. */
    bool fetchAccountInfo(QString *errorText);

    DropboxAccount m_account;
    QNetworkAccessManager *m_nam;
    QString m_codeVerifier;

    QStackedWidget *m_pages;
    QLabel *m_linkedSummary;
    QCheckBox *m_showInSidebar;
    QLineEdit *m_appKeyEdit;
    QLineEdit *m_codeEdit;
    QPushButton *m_finishButton;
    QLabel *m_status;
};
