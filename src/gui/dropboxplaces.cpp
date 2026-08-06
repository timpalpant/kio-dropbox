/*
    SPDX-FileCopyrightText: 2026 Timothy Palpant <tim@palpant.us>
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "dropboxplaces.h"

#include <KFilePlacesModel>
#include <KLocalizedString>

#include <QUrl>

using namespace Qt::StringLiterals;

namespace
{
QUrl placeUrl()
{
    return QUrl("dropbox:/"_L1);
}

/*! The row holding our entry, or an invalid index if there isn't one. */
QModelIndex findPlace(const KFilePlacesModel &model)
{
    for (int row = 0; row < model.rowCount(); ++row) {
        const QModelIndex index = model.index(row, 0);
        if (model.url(index) == placeUrl()) {
            return index;
        }
    }
    return {};
}
} // namespace

bool DropboxPlaces::isShown()
{
    KFilePlacesModel model;
    return findPlace(model).isValid();
}

void DropboxPlaces::setShown(bool shown)
{
    KFilePlacesModel model;
    const QModelIndex existing = findPlace(model);

    if (shown && !existing.isValid()) {
        // An empty appName makes the entry visible in every application rather
        // than only the one that created it.
        model.addPlace(i18n("Dropbox"), placeUrl(), "folder-dropbox"_L1, QString());
    } else if (!shown && existing.isValid()) {
        model.removePlace(existing);
    }
}
