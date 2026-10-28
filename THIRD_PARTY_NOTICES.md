# Third-party notices

Lanes is licensed under GPL-3.0-or-later (see `LICENSE`). It uses the following third-party
components. Every dependency must be GPLv3-compatible and listed here before it is added

## Currently used

| Component | Used for | License | Project |
|---|---|---|---|
| CLI11 | CLI argument parsing | BSD-3-Clause | https://github.com/CLIUtils/CLI11 |
| tl-expected | `Result` type in core | CC0-1.0 | https://github.com/TartanLlama/expected |
| Catch2 v3 | Unit tests (not shipped) | BSL-1.0 | https://github.com/catchorg/Catch2 |

## Planned (added to the build in later steps)

| Component | Used for | License | Project |
|---|---|---|---|
| libcurl | HTTP/FTP transfers | curl license (MIT-style) | https://curl.se |
| OpenSSL 3 | AES-128 (HLS), MD5/SHA-256 | Apache-2.0 | https://www.openssl.org |
| SQLite | Queue, history, settings | Public domain | https://sqlite.org |
| nlohmann/json | JSON | MIT | https://github.com/nlohmann/json |
| spdlog | Logging | MIT | https://github.com/gabime/spdlog |
| Qt 6 | GUI (dynamically linked) | LGPL-3.0 | https://www.qt.io |
| yt-dlp | Video extraction (bundled binary) | Unlicense | https://github.com/yt-dlp/yt-dlp |
| Deno | JS runtime for yt-dlp (bundled binary) | MIT | https://deno.com |
| FFmpeg | Muxing (bundled binary) | LGPL-2.1+ / GPL-2.0+ | https://ffmpeg.org |

Full license texts are shipped with the installer and shown in the app under
About → Open-source licenses.
