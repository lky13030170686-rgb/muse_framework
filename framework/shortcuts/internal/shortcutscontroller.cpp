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
#include "shortcutscontroller.h"

#include "log.h"

using namespace muse::shortcuts;
using namespace muse::actions;

void ShortcutsController::init()
{
    interactive()->currentUri().ch.onReceive(this, [this](const Uri&) {
        //! NOTE: enable process shortcuts only for non-widget objects
        shortcutsRegister()->setActive(!interactive()->topWindowIsWidget());
    });
}

void ShortcutsController::activate(const std::string& sequence)
{
    LOGD() << sequence;

    ActionCode actionCode = resolveAction(sequence);

    //! NOTE: 排查快捷键“按了没反应”时，用 `MuseScoreStudio5.exe -d` 启动即可看到这一行
    //! （`-d` 把日志级别设为 Debug）—— `action` 为空就说明按键被静默丢弃了。
    //! ⚠️ 更多细节（含 Qt 自己的匹配过程）用环境变量 `QT_LOGGING_RULES=qt.gui.shortcutmap=true`。
    LOGD() << "resolved action: " << actionCode;

    if (!actionCode.empty()) {
        dispatcher()->dispatch(actionCode);
    }
}

bool ShortcutsController::isRegistered(const std::string& sequence) const
{
    return shortcutsRegister()->isRegistered(sequence);
}

static bool defaultHasLowerPriorityThan(const std::string& ctx1, const std::string& ctx2)
{
    static const std::array<std::string, 7> CONTEXTS_BY_INCREASING_PRIORITY {
        CTX_ANY,

        CTX_PROJECT_OPENED,
        CTX_NOT_PROJECT_FOCUSED,
        CTX_PROJECT_FOCUSED,
    };

    size_t index1 = muse::indexOf(CONTEXTS_BY_INCREASING_PRIORITY, ctx1);
    size_t index2 = muse::indexOf(CONTEXTS_BY_INCREASING_PRIORITY, ctx2);

    return index1 < index2;
}

ActionCode ShortcutsController::resolveAction(const std::string& sequence) const
{
    ShortcutList shortcutsForSequence = shortcutsRegister()->shortcutsForSequence(sequence);

    IF_ASSERT_FAILED(!shortcutsForSequence.empty()) {
        return ActionCode();
    }

    ShortcutList allowedShortcuts;

    for (const Shortcut& sc : shortcutsForSequence) {
        //! NOTE Check if the shortcut itself is allowed
        const bool ctxAllowed = uiContextResolver()->isShortcutContextAllowed(sc.context);

        //! NOTE Check if the action is allowed
        muse::ui::UiActionState st = aregister()->actionState(sc.action);

        LOGD() << "candidate action: " << sc.action
               << ", ctx: " << sc.context
               << ", ctxAllowed: " << ctxAllowed
               << ", enabled: " << st.enabled;

        if (!ctxAllowed) {
            continue;
        }

        if (!st.enabled) {
            continue;
        }

        allowedShortcuts.push_back(sc);
    }

    if (!shortcutContextPriority()) {
        LOGW() << "Not found implementation of IShortcutContextPriority, will be used default priority";
    }

    allowedShortcuts.sort([this](const Shortcut& f, const Shortcut& s) {
        if (shortcutContextPriority()) {
            return shortcutContextPriority()->hasLowerPriorityThan(f.context, s.context);
        } else {
            return defaultHasLowerPriorityThan(f.context, s.context);
        }
    });

    return !allowedShortcuts.empty() ? allowedShortcuts.back().action : ActionCode();
}
