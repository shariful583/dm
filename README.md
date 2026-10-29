# Lanes Download Manager

A native C++ download manager. Each file is downloaded over several parallel connections
("lanes") with dynamic re-splitting, and downloads survive crashes. Lanes also supports
pause/resume, queues, a scheduler, a speed limiter, HLS/DASH streams, video sites via yt-dlp,
and a Chrome/Edge extension.

**Status:** early development. The download engine (parallel connections, dynamic splitting,
crash-safe resume, retries, speed limits), queues and scheduler, HLS/DASH streams, the `lanes`
command line, the Windows app and the Chrome/Edge extension work; video sites (yt-dlp) are still
to come.

## Windows app

`lanes-gui.exe` is the main program: the download list with categories and queues, an Add
window that checks the link first (name, size, resume support, folder), a progress window with
the connection map and a live speed graph, queues with start/stop times, a speed-limit menu,
options, a notification-area icon and taskbar progress. It follows the Windows light/dark theme
and uses Mica on Windows 11. Paste a link with Ctrl+V or drop it on the window.

While the app runs, `lanes` commands and the browser extension use its engine; a second
`lanes-gui.exe <url>` hands the link to the running window. `lanes-gui.exe --tray` starts hidden
in the notification area.

## Browser extension (Chrome, Edge)

The extension in [`extension/`](extension) hands browser downloads to Lanes (by file type, with
the page's cookies, so logged-in downloads work), lists the videos and streams a page plays,
puts a "Download with Lanes" button over videos, and adds "Download with Lanes" / "Download all
links" to the right-click menu. Hold Alt while clicking to let the browser download instead,
Insert to send anything to Lanes. If Lanes is not running, the extension starts it; if Lanes is
not installed, downloads stay in the browser.

Until the store versions exist, load it unpacked (development only):

```powershell
.\build\windows-debug\bin\lanes-bridge.exe --register   # once: lets Chrome and Edge start Lanes
```

Then open `chrome://extensions` (or `edge://extensions`), turn on Developer mode, choose
**Load unpacked** and pick the `extension` folder. `lanes-bridge.exe --unregister` removes the
registration.

## Command line

```
lanes get <url> [-o FILE|DIR\] [-c 8] [--limit 2M] [--sha256 HEX] [-H "Name: value"]
                [--cookie ..] [--referer ..] [-A agent] [--user name:pass] [--proxy url]
                [--max-height 720]        # HLS (.m3u8) / DASH (.mpd): best quality up to 720p
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

HLS (`.m3u8`) and DASH (`.mpd`) links are recognised automatically: Lanes picks the best quality
(or the best up to `--max-height`), downloads the segments in parallel (AES-128 encrypted HLS
included), and joins video and audio into one `.mp4` with ffmpeg, without re-encoding.
`lanes info <url>` lists the qualities a stream offers. Live streams and DRM-protected streams
(Widevine, PlayReady, FairPlay, SAMPLE-AES) are not supported. Run `tools\fetch-third-party.ps1`
once to get `ffmpeg.exe`.

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

The Windows app also needs Qt 6.8 or newer, the MSVC 64-bit kit (from the Qt online installer;
the free LGPL version is enough). CMake finds a kit under `C:\Qt` by itself; otherwise set
`CMAKE_PREFIX_PATH` to it. Without Qt, everything except `lanes-gui.exe` still builds. The build
copies the Qt files next to `lanes-gui.exe`, so `.\build\windows-debug\bin\lanes-gui.exe` runs as
is.

## License

GPL-3.0-or-later. See [`LICENSE`](LICENSE) and [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).
