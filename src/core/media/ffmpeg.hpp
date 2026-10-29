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
// combined (video + separate audio), MPEG-TS is remuxed to MP4 with fast start. Writes to a
// temporary name and renames on success. Blocking; Cancelled on `stop`. Thread-safe.
Result<void> mux(const std::vector<std::vector<std::filesystem::path>> &tracks,
                 const std::filesystem::path &output, std::stop_token stop = {});

} // namespace lanes::core::media
