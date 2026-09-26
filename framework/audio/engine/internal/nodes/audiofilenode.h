/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
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
#ifndef MUSE_AUDIO_AUDIOFILENODE_H
#define MUSE_AUDIO_AUDIOFILENODE_H

#include "audiosourcenode.h"
#include "../../iaudiosource.h"

namespace muse::audio::engine {
//! A seekable audio source. Kept as a separate, narrow interface rather than a method
//! on IAudioSource so that existing sources (SineSource, NoiseSource) and any future
//! non-file sources stay untouched — see 开发管理细则.md §3 on minimizing edits to
//! upstream interfaces.
class ISeekableAudioSource
{
public:
    virtual ~ISeekableAudioSource() = default;
    //! Jumps to the given position. Called on the engine thread, so implementations
    //! must not allocate or block.
    virtual void seekTo(const TimePosition& position) = 0;
    //! The source's ACTUAL current position. Queried rather than shadowed, so a seek
    //! guard cannot drift out of sync with reality (cf. EventAudioNode::seek, which
    //! asks the synth for playbackPosition() for the same reason).
    virtual TimePosition position() const = 0;
    //! Length of the source, used to stop the playhead from running past the end.
    virtual TimePosition duration() const = 0;
};
//! Wraps a plain IAudioSource (a file reader) as an AudioSourceNode so it can join a
//! TrackChain. This is the missing piece for TrackType::Sound_track: AudioContext
//! already reserves that type, but had no node able to drive an IAudioSource-based
//! chain (see the "NOT IMPLEMENTED YET" note in audiocontext.cpp).
//!
//! Modelled on EventAudioNode, minus everything synth/MPE related: a file source has
//! no note events, no input processing, and no params to negotiate.
//!
//! Thread contract (same as EventAudioNode):
//!   - seek/flush/applyInputParams/prepareToPlay: engine thread, guarded below
//!   - doSelfProcess: audio processing (realtime) thread
class AudioFileNode : public AudioSourceNode
{
public:
    explicit AudioFileNode(TrackId trackId, IAudioSourcePtr source);
    ~AudioFileNode() override = default;

    IAudioSourcePtr source() const { return m_source; }

    void seek(const TimePosition& position, const bool flushSound = true) override;
    void flush() override;

    const AudioInputParams& inputParams() const override;
    void applyInputParams(const AudioInputParams& requiredParams) override;
    async::Channel<AudioInputParams> inputParamsChanged() const override;

    void prepareToPlay() override;
    bool readyToPlay() const override;
    async::Notification readyToPlayChanged() const override;

    bool hasPendingChunks() const override;
    void processInput() override;
    InputProcessingProgress inputProcessingProgress() const override;

    void clearCache() override;

private:
    void onModeChanged(const ProcessMode mode) override;
    void onOutputSpecChanged(const OutputSpec& spec) override;

    void doSelfProcess(float* buffer, samples_t samplesPerChannel) override;

    TrackId m_trackId = INVALID_TRACK_ID;
    IAudioSourcePtr m_source = nullptr;
    //! Same object as m_source when the source supports seeking; null otherwise.
    ISeekableAudioSource* m_seekable = nullptr;

    AudioInputParams m_params;
    async::Channel<AudioInputParams> m_paramsChanges;

    async::Notification m_readyToPlayChanged;

    bool m_ready = false;
};
} // namespace muse::audio::engine

#endif // MUSE_AUDIO_AUDIOFILENODE_H
