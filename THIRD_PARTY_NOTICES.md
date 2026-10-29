# Third-party notices

Lanes is licensed under GPL-3.0-or-later (see `LICENSE`). It uses the following third-party
components. Every dependency must be GPLv3-compatible and listed here before it is added

## Currently used

| Component | Used for | License | Project |
|---|---|---|---|
| CLI11 | CLI argument parsing | BSD-3-Clause | https://github.com/CLIUtils/CLI11 |
| libcurl | HTTP/FTP transfers (Schannel TLS, WinIDN on Windows) | curl license (MIT-style) | https://curl.se |
| tl-expected | `Result` type in core | CC0-1.0 | https://github.com/TartanLlama/expected |
| nlohmann/json | Resume sidecar files, engine pipe protocol (JSON) | MIT | https://github.com/nlohmann/json |
| SQLite | Downloads, queues, settings database | Public domain | https://sqlite.org |
| pugixml | DASH manifests (XML) | MIT | https://pugixml.org |
| FFmpeg | Joining stream segments into MP4 (bundled ffmpeg.exe, LGPL build, fetched by tools/fetch-third-party.ps1) | LGPL-2.1-or-later | https://ffmpeg.org |
| yt-dlp | Reading video sites, its own downloader as a fallback (bundled yt-dlp.exe, updated by Lanes in %LOCALAPPDATA%\Lanes\bin) | Unlicense | https://github.com/yt-dlp/yt-dlp |
| Deno | JavaScript runtime yt-dlp needs for some sites (bundled deno.exe) | MIT | https://deno.com |
| Qt 6 (Core, Gui, Qml, Quick, Quick Controls with the FluentWinUI3 style, Quick Dialogs) | GUI, linked dynamically (DLLs shipped next to lanes-gui.exe) | LGPL-3.0 | https://www.qt.io |
| OpenSSL 3 | Checksums, AES-128 decryption of HLS segments, HMAC (test server) | Apache-2.0 | https://www.openssl.org |
| Catch2 v3 | Unit tests (not shipped) | BSL-1.0 | https://github.com/catchorg/Catch2 |
| cpp-httplib | Local test HTTP server (not shipped) | MIT | https://github.com/yhirose/cpp-httplib |

## Planned (added to the build in later steps)

| Component | Used for | License | Project |
|---|---|---|---|
| spdlog | Logging | MIT | https://github.com/gabime/spdlog |

Full license texts are shipped with the installer and shown in the app under
About → Open-source licenses.
