# Lanes Download Manager

A native C++ download manager. Each file is downloaded over several parallel connections
("lanes") with dynamic re-splitting, and downloads survive crashes. Lanes also supports
pause/resume, queues, a scheduler, a speed limiter, HLS/DASH streams, video sites via yt-dlp,
and a Chrome/Edge extension.

**Status:** early development. The download engine (parallel connections, dynamic splitting,
crash-safe resume, retries, speed limits) and the `lanes` command line work; queues, video sites,
the GUI and the browser extension are still to come.

## Command line

```
lanes get <url> [-o FILE|DIR\] [-c 8] [--limit 2M] [--sha256 HEX] [-H "Name: value"]
                [--cookie ..] [--referer ..] [-A agent] [--user name:pass] [--proxy url]
lanes info <url>
```

`lanes get` shows a live progress line with a connection map (`█` done, `▒` partly, `░` not
yet). Ctrl+C pauses; running the same command again resumes where it stopped. If a link expired,
run `lanes get <new-url> -o <same file>` to continue with the new address.

## Building (Windows)

Requirements: Visual Studio 2022 or 2026 with "Desktop development with C++" (it includes
MSVC, CMake, Ninja and vcpkg).

```powershell
. .\tools\dev-shell.ps1          # loads the MSVC x64 environment and sets VCPKG_ROOT if unset
cmake --preset windows-debug     # first run builds dependencies through vcpkg
cmake --build --preset windows-debug
ctest --preset windows-debug
.\build\windows-debug\bin\lanes.exe --version
```

Use `windows-release` for an optimized build. To use your own vcpkg clone, set `VCPKG_ROOT`
before running the dev-shell script.

## License

GPL-3.0-or-later. See [`LICENSE`](LICENSE) and [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).
