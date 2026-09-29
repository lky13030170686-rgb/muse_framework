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
#include "audiofilenode.h"

#include <algorithm>

#include "log.h"

using namespace muse;
using namespace muse::audio;
using namespace muse::audio::engine;

AudioFileNode::AudioFileNode(TrackId trackId, IAudioSourcePtr source)
    : m_trackId(trackId), m_source(std::move(source))
{
    ONLY_AUDIO_ENGINE_THREAD;

    // A file source can be positioned; the sine/noise sources cannot. Probe once here
    // rather than on every seek.
    m_seekable = dynamic_cast<ISeekableAudioSource*>(m_source.get());

    setName("Source[AudioFile]");
}

void AudioFileNode::onModeChanged(const ProcessMode mode)
{
    ONLY_AUDIO_ENGINE_THREAD;

    // Forwarded to the source, which decides for itself whether to produce sound: a file
    // source is audible only while the transport runs. The engine drives this from the
    // player's isActiveChanged, so ProcessMode::Playing means "the score is playing", and
    // ContextPlayer::seekAllTracks re-seeks every source on play, pause and seek -- which is
    // what makes the track follow the cursor.
    if (m_source) {
        m_source->setMode(mode);
    }
}

void AudioFileNode::onOutputSpecChanged(const OutputSpec& spec)
{
    ONLY_AUDIO_ENGINE_THREAD;

    if (m_source) {
        m_source->setOutputSpec(spec);
    }
}

void AudioFileNode::doSelfProcess(float* buffer, samples_t samplesPerChannel)
{
    ONLY_AUDIO_PROC_THREAD;

    // audioch_t is not unsigned int, so std::max cannot deduce a common type here.
    const audioch_t channels = m_outputSpec.audioChannelCount > 0 ? m_outputSpec.audioChannelCount : 1;

    if (!m_source) {
        std::fill(buffer, buffer + samplesPerChannel * channels, 0.f);
        return;
    }

    // The source produces silence past its own end and while the transport is stopped, so no
    // bound or transport check is needed here.
    m_source->process(buffer, samplesPerChannel);
}

void AudioFileNode::seek(const TimePosition& position, const bool flushSound)
{
    ONLY_AUDIO_ENGINE_THREAD;

    if (!m_seekable) {
        return;
    }

    // Ask the source where it actually is instead of trusting a shadow copy: a stale
    // cached position would silently skip a needed seek (e.g. after clearCache()).
    if (m_seekable->position() == position) {
        return;
    }

    m_seekable->seekTo(position);

    if (flushSound) {
        flush();
    }
}

void AudioFileNode::flush()
{
    ONLY_AUDIO_ENGINE_THREAD;

    // A file source has no scheduled events to drop; its position is authoritative.
}

const AudioInputParams& AudioFileNode::inputParams() const
{
    return m_params;
}

void AudioFileNode::applyInputParams(const AudioInputParams& requiredParams)
{
    // A file source has no settable resource, but the chain still expects params to be
    // negotiable. Echo them back so downstream (mixer/params UI) sees a valid state.
    m_params = requiredParams;
    m_paramsChanges.send(m_params);
}

async::Channel<AudioInputParams> AudioFileNode::inputParamsChanged() const
{
    return m_paramsChanges;
}

void AudioFileNode::prepareToPlay()
{
    ONLY_AUDIO_ENGINE_THREAD;

    m_ready = m_source != nullptr;
    m_readyToPlayChanged.notify();
}

bool AudioFileNode::readyToPlay() const
{
    return m_ready;
}

async::Notification AudioFileNode::readyToPlayChanged() const
{
    return m_readyToPlayChanged;
}

bool AudioFileNode::hasPendingChunks() const
{
    // A file source reads straight from disk on demand; nothing is pre-buffered in the
    // engine's sense, so there are never pending chunks to wait for.
    return false;
}

void AudioFileNode::processInput()
{
    // No offline analysis needed for a file source.
}

InputProcessingProgress AudioFileNode::inputProcessingProgress() const
{
    return {};
}

void AudioFileNode::clearCache()
{
    ONLY_AUDIO_ENGINE_THREAD;

    if (m_seekable) {
        m_seekable->seekTo(TimePosition());
    }
}
