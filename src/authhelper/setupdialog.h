/*
    SPDX-FileCopyrightText: 2026 Timothy Palpant <tim@palpant.us>
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "dropboxaccount.h"

#include <QDialog>

class QLabel;
class QLineEdit;
class QNetworkAccessManager;
class QPushButton;
class QStackedWidget;

/*!
 * Walks the user through linking a Dropbox account.
 *
 * Dropbox has no public client credentials to embed, so each user registers
 * their own app and pastes its key here. The OAuth exchange then uses PKCE with
 * no redirect URI, which is the one flow Dropbox offers that needs no redirect
 * registered against the app and no local web server: Dropbox simply shows the
 * authorization code on screen for the user to copy back.
 */
class SetupDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SetupDialog(QWidget *parent = nullptr);

private:
    QWidget *buildLinkedPage();
    QWidget *buildAppKeyPage();
    QWidget *buildAuthorizePage();

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
    QLineEdit *m_appKeyEdit;
    QLineEdit *m_codeEdit;
    QPushButton *m_finishButton;
    QLabel *m_status;
};
