/*
    SPDX-FileCopyrightText: 2026 Timothy Palpant <tim@palpant.us>
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "setupdialog.h"

#include "dropboxapi.h"

#include <KLocalizedString>
#include <KMessageBox>

#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QGuiApplication>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QPushButton>
#include <QRandomGenerator>
#include <QStackedWidget>
#include <QUrl>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

namespace
{
enum Page {
    LinkedPage,
    AppKeyPage,
    AuthorizePage,
};

/*! A PKCE code verifier: 64 characters from the unreserved set, per RFC 7636. */
QString makeCodeVerifier()
{
    static constexpr QLatin1StringView Alphabet("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~");
    QString verifier;
    verifier.reserve(64);
    for (int i = 0; i < 64; ++i) {
        verifier.append(Alphabet[QRandomGenerator::system()->bounded(Alphabet.size())]);
    }
    return verifier;
}

QString makeCodeChallenge(const QString &verifier)
{
    const QByteArray digest = QCryptographicHash::hash(verifier.toUtf8(), QCryptographicHash::Sha256);
    return QString::fromLatin1(digest.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}
} // namespace

SetupDialog::SetupDialog(QWidget *parent)
    : QDialog(parent)
    , m_nam(new QNetworkAccessManager(this))
{
    setWindowTitle(i18n("Link a Dropbox Account"));
    // Wide enough that the numbered steps don't wrap into an unreadable column.
    setMinimumWidth(560);

    m_pages = new QStackedWidget(this);
    m_pages->insertWidget(LinkedPage, buildLinkedPage());
    m_pages->insertWidget(AppKeyPage, buildAppKeyPage());
    m_pages->insertWidget(AuthorizePage, buildAuthorizePage());

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    m_status->setTextFormat(Qt::PlainText);
    m_status->hide();

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_pages);
    layout->addWidget(m_status);
    layout->addWidget(buttons);

    if (m_account.isConfigured()) {
        showLinked();
    } else {
        m_pages->setCurrentIndex(AppKeyPage);
    }
}

QWidget *SetupDialog::buildLinkedPage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);

    m_linkedSummary = new QLabel(page);
    m_linkedSummary->setWordWrap(true);
    layout->addWidget(m_linkedSummary);

    layout->addWidget(new QLabel(i18n("Dropbox is available in Dolphin at <b>dropbox:/</b>."), page));

    auto *unlinkButton = new QPushButton(i18n("Unlink This Account"), page);
    connect(unlinkButton, &QPushButton::clicked, this, &SetupDialog::unlink);
    layout->addWidget(unlinkButton, 0, Qt::AlignLeft);
    layout->addStretch();

    return page;
}

QWidget *SetupDialog::buildAppKeyPage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);

    auto *explanation = new QLabel(i18n("<p>Dropbox requires every application to be registered, so this needs a key of your own. "
                                        "It takes about a minute:</p>"
                                        "<ol>"
                                        "<li>Open <a href=\"https://www.dropbox.com/developers/apps/create\">the Dropbox App Console</a>.</li>"
                                        "<li>Choose <b>Scoped access</b>, then <b>Full Dropbox</b>, and give the app any name.</li>"
                                        "<li>On the <b>Permissions</b> tab, tick <b>files.metadata.read</b>, <b>files.content.read</b>, "
                                        "<b>files.content.write</b> and <b>account_info.read</b>, then click Submit.</li>"
                                        "<li>Copy the <b>App key</b> from the Settings tab and paste it below.</li>"
                                        "</ol>"),
                                   page);
    explanation->setWordWrap(true);
    explanation->setOpenExternalLinks(true);
    layout->addWidget(explanation);

    layout->addWidget(new QLabel(i18n("App key:"), page));
    m_appKeyEdit = new QLineEdit(page);
    m_appKeyEdit->setText(m_account.appKey());
    layout->addWidget(m_appKeyEdit);

    auto *next = new QPushButton(i18n("Authorize in Browser…"), page);
    connect(next, &QPushButton::clicked, this, &SetupDialog::openAuthorizationPage);
    connect(m_appKeyEdit, &QLineEdit::returnPressed, this, &SetupDialog::openAuthorizationPage);
    layout->addWidget(next, 0, Qt::AlignRight);
    layout->addStretch();

    return page;
}

QWidget *SetupDialog::buildAuthorizePage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);

    auto *explanation = new QLabel(i18n("<p>Your browser should now be showing the Dropbox authorization page. "
                                        "Approve the request, then copy the code Dropbox displays and paste it here.</p>"),
                                   page);
    explanation->setWordWrap(true);
    layout->addWidget(explanation);

    layout->addWidget(new QLabel(i18n("Authorization code:"), page));
    m_codeEdit = new QLineEdit(page);
    layout->addWidget(m_codeEdit);

    m_finishButton = new QPushButton(i18n("Link Account"), page);
    connect(m_finishButton, &QPushButton::clicked, this, &SetupDialog::finishAuthorization);
    connect(m_codeEdit, &QLineEdit::returnPressed, this, &SetupDialog::finishAuthorization);
    layout->addWidget(m_finishButton, 0, Qt::AlignRight);
    layout->addStretch();

    return page;
}

void SetupDialog::showLinked()
{
    const QString who = m_account.accountEmail().isEmpty() ? m_account.accountName() : m_account.accountEmail();
    m_linkedSummary->setText(i18n("<p>Linked to <b>%1</b>.</p>", who.toHtmlEscaped()));
    m_pages->setCurrentIndex(LinkedPage);
}

void SetupDialog::openAuthorizationPage()
{
    const QString appKey = m_appKeyEdit->text().trimmed();
    if (appKey.isEmpty()) {
        KMessageBox::error(this, i18n("Please enter the app key from the Dropbox App Console."));
        return;
    }

    m_account.setAppKey(appKey);
    m_codeVerifier = makeCodeVerifier();

    const QString url = DropboxAccount::authorizationUrl(appKey, makeCodeChallenge(m_codeVerifier));
    if (!QDesktopServices::openUrl(QUrl(url))) {
        KMessageBox::error(this, i18n("Could not open a browser. Please visit this address manually:\n\n%1", url));
    }

    m_pages->setCurrentIndex(AuthorizePage);
    m_codeEdit->setFocus();
}

void SetupDialog::finishAuthorization()
{
    const QString code = m_codeEdit->text().trimmed();
    if (code.isEmpty()) {
        KMessageBox::error(this, i18n("Please paste the code Dropbox showed you."));
        return;
    }

    m_finishButton->setEnabled(false);
    m_status->setText(i18n("Contacting Dropbox…"));
    m_status->show();
    QGuiApplication::setOverrideCursor(Qt::WaitCursor);

    QString errorText;
    const bool redeemed = m_account.redeemAuthorizationCode(m_nam, code, m_codeVerifier, &errorText);
    const bool identified = redeemed && fetchAccountInfo(&errorText);

    QGuiApplication::restoreOverrideCursor();
    m_status->hide();
    m_finishButton->setEnabled(true);

    if (!identified) {
        KMessageBox::error(this, errorText);
        return;
    }

    QString warning;
    if (!m_account.save(&warning)) {
        KMessageBox::error(this, warning);
        return;
    }
    if (!warning.isEmpty()) {
        KMessageBox::information(this, warning, i18n("Dropbox"), "kio-dropbox-plaintext-token"_L1);
    }

    showLinked();
}

bool SetupDialog::fetchAccountInfo(QString *errorText)
{
    DropboxApi api(&m_account);
    QJsonObject result;
    DropboxError error;
    if (!api.rpc("users/get_current_account"_L1, QJsonValue::Null, &result, &error)) {
        *errorText = error.text;
        return false;
    }

    m_account.setAccountInfo(result.value("email"_L1).toString(), result.value("name"_L1).toObject().value("display_name"_L1).toString());
    return true;
}

void SetupDialog::unlink()
{
    if (KMessageBox::questionTwoActions(this,
                                        i18n("Forget the stored Dropbox credentials? Files in Dropbox are not affected."),
                                        i18n("Unlink Account"),
                                        KGuiItem(i18n("Unlink")),
                                        KStandardGuiItem::cancel())
        != KMessageBox::PrimaryAction) {
        return;
    }

    m_account.forget();
    m_appKeyEdit->clear();
    m_codeEdit->clear();
    m_pages->setCurrentIndex(AppKeyPage);
}
