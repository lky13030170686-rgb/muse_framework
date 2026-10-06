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

#include "contextplayer.h"

#include "audio/common/audiosanitizer.h"
#include "audio/common/audioerrors.h"

#include "log.h"

using namespace muse;
using namespace muse::audio;
using namespace muse::audio::engine;
using namespace muse::async;

ContextPlayer::ContextPlayer(IGetTrackSource* getTracks, IExecOperation* execOperation)
    : m_trackSource(getTracks)
    , m_execOperation(execOperation)
{
    m_status.set(PlaybackStatus::Stopped);
    m_isActive.set(false);

    m_status.ch.onReceive(this, [this](const PlaybackStatus status) {
        onStatusChanged(status);
    });

    // Forwarding events from the processing thread to the engine thread
    m_timeEvent.onReceive(this, [this](const TimeEvent event) {
        onTimeEvent(event);
    });
}

void ContextPlayer::exec(OperationType type, const Operation& func)
{
    ONLY_AUDIO_ENGINE_THREAD;
    if (m_execOperation) {
        m_execOperation->execOperation(type, func);
    } else {
        func();
    }
}

void ContextPlayer::onStatusChanged(const PlaybackStatus status)
{
    const bool active = status == PlaybackStatus::Running;
    if (active) {
        //! NOTE If there is no countdown, activate the mixer.
        //! Otherwise, it will become active when the countdown ends.
        if (m_countDown.is_zero()) {
            m_isActive.set(true);
        }
    } else {
        m_isActive.set(false);
        flushAllTracks();
    }
}

void ContextPlayer::forward(const TimePosition& delta)
{
    ONLY_AUDIO_PROC_THREAD;

    // Check: Active
    if (m_status.val != PlaybackStatus::Running) {
        return;
    }

    const TimePosition newTime = proc_onTimeChanged(delta);

    if (m_currentPosition == newTime) {
        return;
    }

    m_currentPosition = newTime;
    m_timeChanged.send(m_currentPosition.time());
}

TimePosition ContextPlayer::proc_onTimeChanged(const TimePosition& delta)
{
    ONLY_AUDIO_PROC_THREAD;

    // Check: Count down
    if (!m_countDown.is_zero()) {
        m_countDown -= delta.time();

        if (m_countDown > 0.) {
            return m_currentPosition; // no change
        }

        m_countDown = 0.;
        m_timeEvent.send(TimeEvent { TimeEventType::CountDownEnded, m_currentPosition }); // forwarding an event to the engine thread
    }

    // Check: Loop
    const TimePosition newTime = m_currentPosition.forwarded(delta);

    //! ⚠️ The wrap happens one chunk **early**, and that is deliberate.
    //!
    //! The tracks are rendered *before* the clock within a cycle: `AudioNode::process()` renders its input
    //! first and then itself, and this player's playhead node has the whole track chain as its input. So by
    //! the time the clock reaches the loop end, the chunk that crosses it has already been rendered - and
    //! with it the note sitting exactly on the loop end, i.e. the first note of the bar after the loop. That
    //! was audible as a stray attack at the loop wrap (user: "奇数次播放会播放到循环外小节的音头",
    //! 进度快照.md 第 66 条).
    //!
    //! Wrapping while the *next* chunk would cross the loop end keeps the tracks away from the loop end
    //! altogether: the seek below repositions them before that chunk is rendered, so nothing at or past the
    //! loop end is ever triggered. The cost is that up to one chunk (1024 samples ≈ 21 ms at 48 kHz) at the
    //! end of the loop is skipped - the flush that comes with the seek cuts the sound at the wrap anyway, so
    //! this is not a new loss, it just makes the boundary deterministic.
    //!
    //! NOTE: the loop end is *exclusive* - a note starting exactly on it belongs to the bar after the loop
    //! and must not sound while looping.
    const bool loopWillBeCrossed = m_timeLoopStart < m_timeLoopEnd
                                   && newTime.time() + delta.time() > m_timeLoopEnd;
    if (loopWillBeCrossed) {
        //! ⚠️ …and the wrap goes back to **exactly** the loop start - no overshoot.
        //!
        //! The sequencers are repositioned with `lower_bound(position)`, which *includes* the events
        //! sitting exactly on that position. Seeking to `loopStart + overshoot` therefore skipped every
        //! note in that little window - including the downbeat of the loop's first bar, which is exactly
        //! where the note is in most music. That was audible as "循环回来第一小节会丢音" (user, 2026-10-06,
        //! 进度快照.md 第 67 条): the loop's first pass was fine (playback starts *at* the loop start), only
        //! the wraps lost those notes.
        //!
        //! Wrapping the clock to the same place keeps the audio and the playhead in step. What is given up
        //! is the tail: the part of the loop between the last rendered chunk and the loop end (up to one
        //! chunk, ≈21 ms at 48 kHz) is not played, and the loop is that much shorter. A fully seamless wrap
        //! would need the sequencers to be loop-aware (render across the boundary in one chunk) - not done
        //! here; see the note in 维护手册.md §4.8.2.
        const TimePosition loopedTime = TimePosition::fromTime(m_timeLoopStart, delta.sampleRate());

        //! ⚠️ The tracks must be seeked to the position the clock wraps **to**, not to the position where
        //! the loop ended. `LoopEnded` used to carry `newTime` (= loop end + overshoot), so every wrap left
        //! the sequencers playing the music that comes **after** the loop while the clock - and therefore
        //! the playhead, the measure/beat display and everything else - was back at the loop start. What you
        //! hear then no longer matches what you see; when the loop ends at the end of the score it simply
        //! goes silent, because the tracks get seeked past the last note on every single wrap.
        //!
        //! NOTE: reported by the user as "循环后播放的声音与音符不符" on the MIDI page (which is where
        //! setting a loop became easy). See 进度快照.md 第 65 条 - the measurement was
        //! `clock wraps to 2.01067 but the tracks are seeked to 6.01067` for a loop of [2, 6].

        //! ⚠️ …and it has to happen **here**, not through an engine operation: an operation is delivered
        //! asynchronously, so until it lands the tracks keep rendering - and triggering - the events just
        //! past the loop end. The upstream TODO right here says the same thing:
        //! "Seek may be necessary to call this directly within the PROC thread."
        seekAllTracks(loopedTime);

        return loopedTime;
    }

    // Check: Duration
    if (newTime.time() >= m_timeDuration) {
        m_timeEvent.send(TimeEvent { TimeEventType::PlaybackEnded, newTime }); // forwarding an event to the engine thread
        return TimePosition::fromTime(m_timeDuration, delta.sampleRate());
    }

    return newTime;
}

const TimePosition& ContextPlayer::currentPosition() const
{
    return m_currentPosition;
}

void ContextPlayer::onTimeEvent(const TimeEvent event)
{
    ONLY_AUDIO_ENGINE_THREAD;

    switch (event.type) {
    case TimeEventType::CountDownEnded:
        exec(OperationType::QuickOperation, [this]() {
            m_isActive.set(m_status.val == PlaybackStatus::Running);
        });
        break;
    case TimeEventType::LoopEnded:
        exec(OperationType::QuickOperation, [this, event]() {
            seekAllTracks(event.position);
        });
        break;
    case TimeEventType::PlaybackEnded:
        exec(OperationType::QuickOperation, [this]() {
            pause();
        });
        break;
    default:
        break;
    }
}

async::Promise<Ret> ContextPlayer::prepareToPlay()
{
    ONLY_AUDIO_ENGINE_THREAD;

    return async::make_promise<Ret>([this](auto resolve, auto) {
        prepareAllTracksToPlay([resolve]() {
            (void)resolve(make_ok());
        });

        return Promise<Ret>::dummy_result();
    });
}

void ContextPlayer::play(const secs_t delay)
{
    ONLY_AUDIO_ENGINE_THREAD;
    ONLY_ON_OPERATION_EXEC;

    if (playbackStatus() == PlaybackStatus::Running) {
        return;
    }

    m_countDown = delay;
    m_status.set(PlaybackStatus::Running);
}

void ContextPlayer::seek(const TimePosition& position, const bool flushSound)
{
    ONLY_AUDIO_ENGINE_THREAD;
    ONLY_ON_OPERATION_EXEC;

    //! NOTE During export, the current time does not change, it remains at 0
    // but a seek operation is still required to reset the internal state of the sources (synthesizers).
    // if (newPosition == m_currentPosition.time()) {
    //     return;
    // }

    IF_ASSERT_FAILED(position.isValid()) {
        return;
    }

    IF_ASSERT_FAILED(m_trackSource) {
        return;
    }

    m_flushSoundOnSeek = flushSound;
    m_currentPosition = position;
    m_timeChanged.send(m_currentPosition.time());
    seekAllTracks(position);
    m_flushSoundOnSeek = true;
}

void ContextPlayer::stop()
{
    ONLY_AUDIO_ENGINE_THREAD;
    ONLY_ON_OPERATION_EXEC;

    if (playbackStatus() == PlaybackStatus::Stopped) {
        return;
    }

    m_status.set(PlaybackStatus::Stopped);
    m_countDown = 0.;
    seek(TimePosition::zero(m_currentPosition.sampleRate()));
    m_notYetReadyToPlayTracks.clear();
}

void ContextPlayer::pause()
{
    ONLY_AUDIO_ENGINE_THREAD;
    ONLY_ON_OPERATION_EXEC;

    if (playbackStatus() == PlaybackStatus::Paused) {
        return;
    }

    m_status.set(PlaybackStatus::Paused);
    m_notYetReadyToPlayTracks.clear();
}

void ContextPlayer::resume(const secs_t delay)
{
    ONLY_AUDIO_ENGINE_THREAD;
    ONLY_ON_OPERATION_EXEC;

    if (playbackStatus() == PlaybackStatus::Running) {
        return;
    }

    m_countDown = delay;
    seek(m_currentPosition);
    m_status.set(PlaybackStatus::Running);
}

secs_t ContextPlayer::duration() const
{
    ONLY_AUDIO_ENGINE_THREAD;
    return m_timeDuration;
}

void ContextPlayer::setDuration(const secs_t duration)
{
    ONLY_AUDIO_ENGINE_THREAD;
    m_timeDuration = duration;
}

Ret ContextPlayer::setLoop(const secs_t from, const secs_t to)
{
    ONLY_AUDIO_ENGINE_THREAD;
    ONLY_ON_OPERATION_EXEC;

    if (from >= to) {
        return make_ret(Err::InvalidTimeLoop);
    }

    m_timeLoopStart = from;
    m_timeLoopEnd = to;

    return Ret(Ret::Code::Ok);
}

void ContextPlayer::resetLoop()
{
    ONLY_AUDIO_ENGINE_THREAD;
    ONLY_ON_OPERATION_EXEC;
    m_timeLoopStart = 0;
    m_timeLoopEnd = 0;
}

secs_t ContextPlayer::playbackPosition() const
{
    ONLY_AUDIO_ENGINE_THREAD;
    return m_currentPosition.time();
}

Channel<secs_t> ContextPlayer::playbackPositionChanged() const
{
    ONLY_AUDIO_ENGINE_THREAD;
    return m_timeChanged;
}

PlaybackStatus ContextPlayer::playbackStatus() const
{
    ONLY_AUDIO_ENGINE_THREAD;
    return m_status.val;
}

Channel<PlaybackStatus> ContextPlayer::playbackStatusChanged() const
{
    ONLY_AUDIO_ENGINE_THREAD;
    return m_status.ch;
}

bool ContextPlayer::isActive() const
{
    ONLY_AUDIO_ENGINE_THREAD;
    return m_isActive.val;
}

Channel<bool> ContextPlayer::isActiveChanged() const
{
    ONLY_AUDIO_ENGINE_THREAD;
    return m_isActive.ch;
}

void ContextPlayer::seekAllTracks(const TimePosition& position)
{
    ONLY_AUDIO_ENGINE_THREAD;

    //! NOTE: deliberately **no** `ONLY_ON_OPERATION_EXEC` here. Besides the ordinary seeks (which do run
    //! inside an engine operation), this is also called from the loop wrap, which is detected while the
    //! clock is being forwarded - i.e. inside the processing cycle - and that is the whole point of the
    //! call there: an operation-based seek is delivered asynchronously, and until it lands the tracks keep
    //! rendering (and triggering) the events just past the loop end, so the note that starts exactly at
    //! the loop end - the downbeat of the bar after the loop - was audible as a stray attack.
    //! Safe because the processing thread **is** the engine thread (`AudioSanitizer::isEngineThread()`
    //! covers both, see audiosanitizer.h), so no cross-thread access happens here. See 进度快照.md 第 66 条.

    IF_ASSERT_FAILED(m_trackSource) {
        return;
    }

    for (const auto& source : m_trackSource->allTracksSources()) {
        source->seek(position, m_flushSoundOnSeek);
    }
}

void ContextPlayer::flushAllTracks()
{
    ONLY_AUDIO_ENGINE_THREAD;
    ONLY_ON_OPERATION_EXEC;

    IF_ASSERT_FAILED(m_trackSource) {
        return;
    }

    for (const auto& source : m_trackSource->allTracksSources()) {
        source->flush();
    }
}

void ContextPlayer::prepareAllTracksToPlay(AllTracksReadyCallback allTracksReadyCallback)
{
    ONLY_AUDIO_ENGINE_THREAD;

    IF_ASSERT_FAILED(m_trackSource) {
        return;
    }

    m_notYetReadyToPlayTracks.clear();

    for (const auto& source : m_trackSource->allTracksSources()) {
        IF_ASSERT_FAILED(source) {
            continue;
        }

        IF_ASSERT_FAILED(source->mode() == ProcessMode::Idle) {
            continue;
        }

        source->prepareToPlay();

        if (!source->readyToPlay()) {
            m_notYetReadyToPlayTracks.insert(source);
        }
    }

    if (m_notYetReadyToPlayTracks.empty()) {
        allTracksReadyCallback();
        return;
    }

    for (const auto& source : m_notYetReadyToPlayTracks) {
        std::weak_ptr<AudioSourceNode> weakPtr = source;
        source->readyToPlayChanged().onNotify(this, [this, weakPtr, allTracksReadyCallback]() {
            if (auto source = weakPtr.lock()) {
                muse::remove(m_notYetReadyToPlayTracks, source);

                if (m_notYetReadyToPlayTracks.empty()) {
                    allTracksReadyCallback();
                }

                source->readyToPlayChanged().disconnect(this);
            }
        }, Asyncable::Mode::SetReplace);
    }
}
