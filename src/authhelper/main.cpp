/*
    SPDX-FileCopyrightText: 2026 Timothy Palpant <tim@palpant.us>
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "dropboxplaces.h"
#include "setupwidget.h"

#include <KLocalizedString>

#include <QApplication>
#include <QCommandLineParser>
#include <QDialog>
#include <QDialogButtonBox>
#include <QIcon>
#include <QTextStream>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

namespace
{
/*! The same UI as the System Settings module, in a window of its own. */
QDialog *buildDialog()
{
    auto *dialog = new QDialog;
    dialog->setWindowTitle(i18n("Link a Dropbox Account"));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);

    auto *layout = new QVBoxLayout(dialog);
    layout->addWidget(new SetupWidget(dialog));
    layout->addWidget(buttons);

    return dialog;
}
} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QApplication::setApplicationName("kio-dropbox-auth"_L1);
    QApplication::setApplicationVersion("0.1.0"_L1);
    QApplication::setWindowIcon(QIcon::fromTheme("folder-dropbox"_L1));

    KLocalizedString::setApplicationDomain("kio6_dropbox");

    QCommandLineParser parser;
    parser.setApplicationDescription(i18n("Links a Dropbox account for use with the dropbox:/ protocol in Dolphin."));
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption statusOption("status"_L1, i18n("Print whether an account is linked and exit."));
    QCommandLineOption sidebarOption("sidebar"_L1,
                                     i18n("Add or remove the Dropbox entry in the file manager sidebar, then exit."),
                                     "show|hide"_L1);
    parser.addOption(statusOption);
    parser.addOption(sidebarOption);
    parser.process(app);

    if (parser.isSet(sidebarOption)) {
        const QString mode = parser.value(sidebarOption);
        if (mode != "show"_L1 && mode != "hide"_L1) {
            QTextStream(stderr) << i18n("--sidebar takes either 'show' or 'hide'.") << Qt::endl;
            return 2;
        }
        DropboxPlaces::setShown(mode == "show"_L1);
        return 0;
    }

    if (parser.isSet(statusOption)) {
        DropboxAccount account;
        QTextStream out(stdout);
        out << (DropboxPlaces::isShown() ? i18n("Shown in the file manager sidebar.") : i18n("Not in the file manager sidebar."))
            << Qt::endl;
        if (account.isConfigured()) {
            const QString who = account.accountEmail().isEmpty() ? account.accountName() : account.accountEmail();
            out << i18n("Linked to %1.", who) << Qt::endl;
            return 0;
        }
        out << i18n("No Dropbox account is linked.") << Qt::endl;
        return 1;
    }

    QDialog *dialog = buildDialog();
    dialog->show();
    QObject::connect(dialog, &QDialog::finished, &app, &QApplication::quit);
    return app.exec();
}
