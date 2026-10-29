// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <stop_token>
#include <vector>

#include "core/error.hpp"

namespace lanes::core::media {

// ffmpeg.exe to run: LANES_FFMPEG if set, else next to the running program (the installer and
// the build put it there). Never searched on PATH. NotFound with a hint otherwise. Thread-safe.
Result<std::filesystem::path> find_ffmpeg();

// Joins tracks into `output` without re-encoding: each track is the byte-wise concatenation of
// its files (ffmpeg's concatf protocol, so no joined copy is written), all tracks' streams are
// combined (video + separate audio) into the container `output`'s extension names (MP4 files get
// fast start). Writes to a
// temporary name and renames on success. Blocking; Cancelled on `stop`. Thread-safe.
Result<void> mux(const std::vector<std::vector<std::filesystem::path>> &tracks,
                 const std::filesystem::path &output, std::stop_token stop = {});

// Converts the audio of `input` to MP3 (LAME VBR quality 2, about 190 kbit/s) in `output`.
// Blocking; Cancelled on `stop`. Thread-safe.
Result<void> to_mp3(const std::filesystem::path &input, const std::filesystem::path &output,
                    std::stop_token stop = {});

} // namespace lanes::core::media
