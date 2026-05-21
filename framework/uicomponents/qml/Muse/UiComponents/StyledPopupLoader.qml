/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-CLA-applies
 *
 * MuseScore
 * Music Composition & Notation
 *
 * Copyright (C) 2026 MuseScore Limited and others
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
pragma ComponentBehavior: Bound

import QtQuick

import Muse.UiComponents

Loader {
    id: loader

    property StyledPopupView popup: loader.item as StyledPopupView
    property Item popupAnchorItem: null

    readonly property bool isOpened: Boolean(loader.popup) && loader.popup.isOpened

    signal opened()
    signal closed()

    function open() {
        if (loader.status === Loader.Ready) {
            loader.popup.open()
        } else {
            prv.pendingOpen = true
            loader.active = true
        }
    }

    function toggleOpened() {
        if (loader.isOpened || prv.pendingOpen) {
            close()
            return
        }

        open()
    }

    function close() {
        prv.pendingOpen = false

        if (loader.isOpened) {
            loader.popup.close()
        }
    }

    QtObject {
        id: prv

        property bool pendingOpen: false

        function unloadPopup() {
            if (loader.popup && loader.popup.isOpened) {
                return
            }

            loader.active = false
            Qt.callLater(loader.closed)
        }
    }

    active: false

    onLoaded: {
        var popup = loader.popup
        popup.parent = loader.parent

        if (loader.popupAnchorItem) {
            popup.anchorItem = loader.popupAnchorItem
        }

        popup.opened.connect(function() { Qt.callLater(loader.opened) })
        popup.closed.connect(function() { Qt.callLater(prv.unloadPopup) })

        if (prv.pendingOpen) {
            prv.pendingOpen = false
            popup.open()
        }
    }
}
