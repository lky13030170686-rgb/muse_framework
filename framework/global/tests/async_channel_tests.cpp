/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-CLA-applies
 *
 * MuseScore Studio
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

//! What this suite protects (progress entry 175):
//! `ChannelImpl::receiversCall` used to walk `receivers` **by iterator** while a
//! callback could add/remove receivers — also indirectly, through a nested
//! `send()` on the same channel. Adding reallocates the vector; removing erases
//! (and, in the old code, deletes) an element the running loop still
//! dereferences. Symptom in the real app: `execute of address 0x0` on the main
//! thread, inside `Channel<...>::send`, seconds after a panel/dock was opened or
//! closed while playback was running.
//!
//! These are *invariant* tests: a receiver is never called through a pointer the
//! loop no longer owns, and nobody subscribed during a send hears that send.

#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "async/channel.h"

using namespace kors::async;

namespace {
struct Sink : public Asyncable {
    std::vector<int> got;

    int count() const { return int(got.size()); }
    bool heard(int v) const
    {
        for (int x : got) {
            if (x == v) {
                return true;
            }
        }
        return false;
    }
};
}

//! Subscribing from inside a callback grows `receivers` (reallocating it once the
//! capacity is exceeded). The send already running must not be disturbed.
TEST(AsyncChannelReentrancyTests, SubscribingDuringSendDoesNotDisturbThatSend)
{
    Channel<int> ch;

    std::vector<std::unique_ptr<Sink>> sinks;
    for (int i = 0; i < 4; ++i) {
        sinks.emplace_back(std::make_unique<Sink>());
        Sink* s = sinks.back().get();
        ch.onReceive(s, [s](const int& v) { s->got.push_back(v); });
    }

    //! 64 new subscribers from inside the first callback: with 4 elements in the
    //! receiver vector this necessarily reallocates it.
    std::vector<std::unique_ptr<Sink>> added;
    bool expanded = false;
    Sink* first = sinks.front().get();
    ch.onReceive(first, [&](const int& v) {
        first->got.push_back(v);
        if (!expanded) {
            expanded = true;
            for (int k = 0; k < 64; ++k) {
                added.emplace_back(std::make_unique<Sink>());
                Sink* s = added.back().get();
                ch.onReceive(s, [s](const int& w) { s->got.push_back(w); });
            }
        }
    }, Asyncable::Mode::SetReplace);

    ch.send(1);

    //! every pre-existing receiver heard the send exactly once
    for (const auto& s : sinks) {
        EXPECT_EQ(s->count(), 1) << "a receiver that existed before send(1) was skipped or called twice";
        EXPECT_TRUE(s->heard(1));
    }
    //! a receiver subscribed during send(1) must not hear send(1)
    for (const auto& s : added) {
        EXPECT_EQ(s->count(), 0) << "a receiver subscribed during send(1) heard send(1)";
    }

    //! and the next send reaches everybody, the new ones included
    ch.send(2);
    for (const auto& s : sinks) {
        EXPECT_EQ(s->count(), 2);
        EXPECT_TRUE(s->heard(2));
    }
    for (const auto& s : added) {
        EXPECT_EQ(s->count(), 1) << "a receiver added during send(1) did not hear send(2)";
        EXPECT_TRUE(s->heard(2));
    }

    for (const auto& s : sinks) {
        ch.disconnect(s.get());
    }
    for (const auto& s : added) {
        ch.disconnect(s.get());
    }
}

//! The dangerous shape: a callback unsubscribes somebody **and** triggers a
//! nested send, so the nested call flushes `pendingToRemove` while the outer
//! loop still holds that receiver. The old code deleted it there (use-after-free
//! right under the outer loop -> call through a freed callback -> crash).
TEST(AsyncChannelReentrancyTests, UnsubscribingAndSendingAgainInsideACallbackIsSafe)
{
    Channel<int> ch;

    Sink a;
    Sink b;
    Sink c;

    bool nested = false;
    ch.onReceive(&a, [&](const int& v) {
        a.got.push_back(v);
        if (!nested && v == 1) {
            nested = true;
            ch.disconnect(&c);   //! -> pendingToRemove
            ch.send(2);          //! nested send flushes pendingToRemove while we are still looping
        }
    });
    ch.onReceive(&b, [&](const int& v) { b.got.push_back(v); });
    ch.onReceive(&c, [&](const int& v) { c.got.push_back(v); });

    ch.send(1);

    EXPECT_TRUE(a.heard(1));
    EXPECT_TRUE(b.heard(1));
    EXPECT_EQ(c.count(), 0) << "an unsubscribed receiver still heard the send";

    EXPECT_TRUE(a.heard(2));
    EXPECT_TRUE(b.heard(2));
    EXPECT_EQ(c.count(), 0);

    EXPECT_EQ(a.count(), 2);
    EXPECT_EQ(b.count(), 2);

    ch.disconnect(&a);
    ch.disconnect(&b);
}

//! Destroying a subscriber (its destructor unsubscribes) inside a callback is the
//! shape the real app hits when a panel/page is closed while a notification is
//! being delivered.
TEST(AsyncChannelReentrancyTests, DestroyingASubscriberDuringSendIsSafe)
{
    Channel<int> ch;

    auto victim = std::make_unique<Sink>();
    Sink keeper;
    Sink* v = victim.get();
    ch.onReceive(v, [v](const int& x) { v->got.push_back(x); });
    ch.onReceive(&keeper, [&](const int& x) {
        keeper.got.push_back(x);
        if (x == 1 && victim) {
            victim.reset();      //! ~Asyncable -> disconnect -> pendingToRemove
            ch.send(2);          //! nested send with a receiver already gone
        }
    });

    ch.send(1);

    EXPECT_EQ(keeper.count(), 2);
    EXPECT_TRUE(keeper.heard(1));
    EXPECT_TRUE(keeper.heard(2));

    ch.disconnect(&keeper);
}
