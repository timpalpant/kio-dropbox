/*
    SPDX-FileCopyrightText: 2026 Timothy Palpant <tim@palpant.us>
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "setupdialog.h"

#include <KLocalizedString>

#include <QApplication>
#include <QCommandLineParser>
#include <QTextStream>

using namespace Qt::StringLiterals;

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
    parser.addOption(statusOption);
    parser.process(app);

    if (parser.isSet(statusOption)) {
        DropboxAccount account;
        QTextStream out(stdout);
        if (account.isConfigured()) {
            const QString who = account.accountEmail().isEmpty() ? account.accountName() : account.accountEmail();
            out << i18n("Linked to %1.", who) << Qt::endl;
            return 0;
        }
        out << i18n("No Dropbox account is linked.") << Qt::endl;
        return 1;
    }

    SetupDialog dialog;
    dialog.show();
    return app.exec();
}
