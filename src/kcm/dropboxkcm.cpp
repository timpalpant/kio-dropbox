/*
    SPDX-FileCopyrightText: 2026 Timothy Palpant <tim@palpant.us>
    SPDX-License-Identifier: GPL-2.0-or-later

    The System Settings page for the dropbox:/ protocol.
*/

#include "setupwidget.h"

#include <KCModule>
#include <KPluginFactory>

#include <QVBoxLayout>

/*!
 * Hosts SetupWidget in System Settings.
 *
 * There is no Apply button to wire up: linking an account and toggling the
 * sidebar entry both take effect the moment the user acts, so this module never
 * reports unsaved changes. load() just re-reads the stored account, which
 * matters when the standalone helper changed it while this page was open.
 */
class DropboxKcm : public KCModule
{
    Q_OBJECT

public:
    DropboxKcm(QObject *parent, const KPluginMetaData &data)
        : KCModule(parent, data)
        , m_setup(new SetupWidget(widget()))
    {
        auto *layout = new QVBoxLayout(widget());
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(m_setup);
    }

    void load() override { m_setup->refresh(); }

private:
    SetupWidget *m_setup;
};

K_PLUGIN_CLASS_WITH_JSON(DropboxKcm, "kcm_dropbox.json")

#include "dropboxkcm.moc"
