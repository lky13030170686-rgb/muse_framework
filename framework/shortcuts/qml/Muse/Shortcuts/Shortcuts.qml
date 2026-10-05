/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2021 MuseScore Limited and others
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
import QtQuick

import Muse.Shortcuts

QtObject {
    id: root

    Component.onCompleted: {
        shortcutsModel.init()
    }

    property var objects: []

    property ShortcutsInstanceModel model: ShortcutsInstanceModel {
        id: shortcutsModel

        onShortcutsChanged: {
            for (var s = 0; s < root.objects.length; ++s) {
                root.objects[s].enabled = false
                root.objects[s].destroy()
            }
            root.objects = []

            for (var key in shortcutsModel.shortcuts) {
                var autoRepeat = Boolean(shortcutsModel.shortcuts[key]);
                var obj = shortcutComponent.createObject(root, {sequence: key, autoRepeat: autoRepeat})
                root.objects.push(obj)
            }
        }
    }

    property Component component: Component {
        id: shortcutComponent
        Shortcut {
            //! ⚠️ `Qt::ApplicationShortcut` 而不是 `Qt::WindowShortcut`（2026-10-05 改）。
            //!
            //! `WindowShortcut` 只在"该 Shortcut 所属的那个窗口"激活时匹配。而 MIDI 页
            //! （`musescore://midi`）的 QML 跑在一个 **QWidget 宿主**里
            //! （实测 `Interactive::topWindowIsWidget()` 在那一页为 true），于是这些挂在
            //! `AppWindow` 上的快捷键在 MIDI 页**根本不触发** —— 日志里连
            //! `ShortcutsController::activate()` 都不会被调用，按键被静默吞掉
            //! （用户实测：**记谱页 Ctrl+Z 生效、MIDI 页毫无反应**）。
            //!
            //! `ApplicationShortcut` 只要应用活动就匹配，跨窗口，正好覆盖这种情况。
            //! 模态对话框那种场景另有保护（`interactive.cpp` 打开颜色对话框时会
            //! `shortcutsRegister()->setActive(false)`）。
            context: Qt.ApplicationShortcut
            enabled: shortcutsModel.active
            onActivated: shortcutsModel.activate(sequence)
        }
    }
}
