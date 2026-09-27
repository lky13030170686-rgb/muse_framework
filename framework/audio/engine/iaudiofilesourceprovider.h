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
#ifndef MUSE_AUDIO_IAUDIOFILESOURCEPROVIDER_H
#define MUSE_AUDIO_IAUDIOFILESOURCEPROVIDER_H

#include <memory>
#include <string>

#include "global/modularity/imoduleinterface.h"
#include "iaudiosource.h"

namespace muse::audio::engine {
//! Creates an IAudioSource that streams an audio file.
//!
//! Why this indirection exists: the audio engine (muse submodule) must not depend on
//! application code (src/), and the decoder that knows how to read WAV/FLAC/OGG lives in
//! src/audiotrack. The app layer registers an implementation of this interface, so the
//! engine can turn "a file path arrived over RPC" into a playable source without
//! knowing anything about libsndfile or file formats.
//!
//! This mirrors how the engine already resolves synths and fx through resolver
//! interfaces rather than constructing concrete types.
class IAudioFileSourceProvider : MODULE_GLOBAL_INTERFACE
{
    INTERFACE_ID(IAudioFileSourceProvider)

public:
    virtual ~IAudioFileSourceProvider() = default;

    //! Returns nullptr if the file cannot be opened or the format is unsupported.
    virtual IAudioSourcePtr createSource(const std::string& filePath) const = 0;

    //! Whether the given path looks like an audio format we can decode. Lets callers
    //! reject obviously wrong files before attempting to open them.
    virtual bool isSupportedFile(const std::string& filePath) const = 0;
};
} // namespace muse::audio::engine

#endif // MUSE_AUDIO_IAUDIOFILESOURCEPROVIDER_H
