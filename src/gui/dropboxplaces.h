/*
    SPDX-FileCopyrightText: 2026 Timothy Palpant <tim@palpant.us>
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

/*!
 * The Dropbox entry in the file manager's Places sidebar.
 *
 * Places are stored in a shared bookmark file that every KDE application
 * watches, so adding or removing the entry shows up in already-running Dolphin
 * windows without a restart. There is no separate setting for this: whether the
 * entry exists *is* the setting.
 */
namespace DropboxPlaces {
/*! Whether dropbox:/ currently has an entry in the Places sidebar. */
bool isShown();

/*! Adds or removes the dropbox:/ entry. Does nothing if already in that state. */
void setShown(bool shown);
} // namespace DropboxPlaces
