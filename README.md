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

lanes add <url> [-o DIR\] [--queue NAME] [--at "YYYY-MM-DD HH:MM"]   # background download
lanes add --batch urls.txt   |   lanes add --pattern "https://host/img[001-120].jpg"
          [--if-exists ask|rename|overwrite|resume]                 # same URL already listed
lanes categories   |   lanes category set Video --ext mp4,mkv [--folder D:\Films]
lanes login add <host> <user>   |   lanes login list   |   lanes login remove <host>
lanes list [--queue NAME]
lanes pause <id|all>     lanes resume <id|all> [--url NEW-URL]     lanes remove <id> [--delete-file]
lanes run [--queue main] [--max 3] [--until 07:00]                  # process a queue, live status
lanes config [key [value]]       # download_dir, connections, max_active, speed_limit (e.g. 2M)
```

`lanes get` downloads in the foreground and shows a live progress line with a connection map
(`█` done, `▒` partly, `░` not yet). Ctrl+C pauses; running the same command again resumes where
it stopped. If a link expired, run `lanes get <new-url> -o <same file>` to continue.

`lanes add` hands the download to a background Lanes engine (started automatically, it stops by
itself when idle). Downloads, queues and settings are kept in `%LOCALAPPDATA%\Lanes\lanes.db`;
cookies and passwords are never saved there. Without `-o`, files go to a folder for their type
(`Downloads\Video`, `Downloads\Music`, ...). Site logins are kept in Windows Credential Manager.

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
