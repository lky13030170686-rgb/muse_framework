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
            //! `WindowShortcut` 只在"该 Shortcut 所属的那个窗口"激活时匹配
            //! （`qquickshortcut.cpp` 的 `qQuickShortcutContextMatcher`：要一路找到 owner 的
            //! window 再与 `QGuiApplication::focusWindow()` 比对）；`ApplicationShortcut`
            //! **无条件 `return true`**，跨窗口、跨宿主都匹配，覆盖面最宽。
            //!
            //! ⛔ **这一层不是 Ctrl+Z 失效的原因** —— 2026-10-05 用 Qt 自己的
            //! `qt.gui.shortcutmap` 日志实测：本组件注册的 Ctrl+Z 一直**在 map 里且 context
            //! 判定通过**（它出现在 Qt 打印的 ambiguous 候选列表里，说明 `correctContext()`
            //! 为 true）。真正的根因是**别处又注册了一个 Ctrl+Z**
            //! （`MidiEditorView.qml` 里页面级的 `Shortcut`，已删除）→ Qt 判定 ambiguous →
            //! 只发 `activatedAmbiguously()` → **所有同键的 `onActivated` 一起作废**。
            //! 详见 `MidiEditorView.qml` 里 `Keys.onPressed` 上方那段注释与 `进度快照.md` §60。
            //!
            //! ⚠️ 因此：**不要为了"抢键"再注册第二个 `Shortcut`** —— Qt 没有"优先级更高者胜出"
            //! 这回事，多一个注册者只会让双方一起失效。
            //!
            //! 模态对话框那种场景另有保护（`interactive.cpp` 打开颜色对话框时会
            //! `shortcutsRegister()->setActive(false)` → 这里是 `enabled: false`）。
            context: Qt.ApplicationShortcut
            enabled: shortcutsModel.active
            onActivated: shortcutsModel.activate(sequence)
        }
    }
}
