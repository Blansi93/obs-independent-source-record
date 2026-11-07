# Record Source

Plugin for **OBS Studio 33.0+**

> This plugin was developed with the assistance of AI. that records a single source to its own file, with its own audio track and its own control dock. It does not share the output system with streaming, the main recording, or the replay buffer.

## Download

Prebuilt Windows (x64) releases are available on the [Releases](https://github.com/Blansi93/obs-independent-source-record/releases) page. No compilation is required.

- **Installer (recommended):** `obs-record-source-setup-0.2.1.exe` installs the plugin into `C:\ProgramData\obs-studio\plugins\obs-record-source` and includes an uninstaller.
- **ZIP:** extract it so that `obs-record-source/obs-record-source.dll` and `obs-record-source/data/manifest.json` end up under your OBS plugins folder (for example `C:\ProgramData\obs-studio\plugins\obs-record-source`).

Restart OBS after installing.
## Features

- Independent recording of a video source (scene, game, window, capture card, etc.).
- Its own audio track, captured from the selected source.
- Standalone file through `ffmpeg_muxer`.
- Small dock integrated in OBS (`Docks -> Record Source`).
- Video and audio encoder configuration with the same properties exposed by OBS.
- Per-recording color settings: color format (NV12, I420, I444, P010), color space (Rec. 709, Rec. 601, sRGB, Rec. 2100 PQ/HLG), and color range (limited or full).
- Automatic splitting of the recording into multiple files by time or size, like OBS.
- **Bilingual UI**: English by default for any OBS language except Spanish, where the UI is Spanish.
- Compatible with OBS Studio 33.0.

## Build

Requirements:

- Visual Studio 2022 Build Tools with C++.
- CMake 3.28 or higher.
- OBS Studio 33.0.1 sources.
- Qt 6 package from `obs-deps`.
- OBS Studio 33.0.1 installed.

Build:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build.ps1 -Config Release -Install
```

The script downloads dependencies if they are not available, generates the import libraries from `obs.dll` and `obs-frontend-api.dll`, configures CMake, builds, and, with `-Install`, copies the plugin to:

```text
C:\ProgramData\obs-studio\plugins\obs-record-source
```

## Development

To test without installing in `ProgramData`, define:

```powershell
$env:OBS_PLUGINS_PATH = "C:\path\to\parent\of\the\bundle"
```

OBS expects the plugin at:

```text
<parent>\obs-record-source\obs-record-source.dll
<parent>\obs-record-source\data\manifest.json
<parent>\obs-record-source\data\locale\en-US.ini
<parent>\obs-record-source\data\locale\es-ES.ini
```

## Usage

1. Open OBS Studio.
2. Open the `Record Source` dock.
3. Select the video source.
4. Choose the destination folder, format, and, if needed, color settings and file splitting.
5. Select and configure the video and audio encoders.
6. Click **Start recording**.
7. Click **Stop** to finish.

Settings are saved automatically in the plugin configuration.

### Logs

The plugin writes a `record-source.log` file next to its DLL (for example `C:\ProgramData\obs-studio\plugins\obs-record-source\record-source.log`). Errors and recording start/stop events are appended there, in addition to OBS' own log.
### Color settings

- **Color format**: `NV12` and `I420` are 8-bit 4:2:0 and the most compatible. `I444` preserves more color detail and `P010` uses 10 bits.
- **Color space**: `Rec. 709` is the SDR standard. `Rec. 2100 PQ/HLG` uses the `Rec. 2020` gamut (HDR) and requires a 10-bit format and a compatible encoder; OBS' x264 does not support it.
- **Color range**: `Limited` (16-235) is usual for YUV; `Full` (0-255) preserves the entire range.
- The **Automatic** option in each setting keeps the value OBS currently has configured, so the default behavior does not change.

### Audio

The plugin does not capture the audio source in isolation: when recording starts, it temporarily adds the selected source to the OBS track you choose and records that whole track. That is why, to record only that source, you must disable the other audio sources on the same track. The selected audio is effectively duplicated onto that track, so any filters added to the original OBS source are also recorded. When recording stops, the source's original tracks are restored.

### File splitting

The **Split file** setting makes the recording continue in another file when a limit is reached, just like OBS (`Settings -> Output -> Recording`):

- **Do not split**: a single file per recording.
- **By time**: each file lasts the specified number of minutes (15 by default).
- **By size**: each file is at most the specified number of MB (2048 by default, minimum 20 MB).

Splitting happens at the next keyframe, so each part may slightly exceed the limit. Parts are saved in the destination folder using the same naming pattern as the first file, with the date and time of when each one is created. The **File** field always shows the part currently being recorded.

### Language

The plugin UI follows OBS' language:

- If OBS is set to **Spanish**, the plugin uses Spanish (`es-ES`).
- With any other language, including English, the plugin uses **English** (`en-US`).

The texts live in `data/locale/en-US.ini` and `data/locale/es-ES.ini`. English is always loaded as the base and Spanish is overlaid when appropriate, so if a key is missing in a language the English text is shown. OBS does not allow translating `manifest.json` metadata, which is in English by default.

To add another language, create `data/locale/<locale>.ini`, add it to the `CMakeLists.txt` rules, and extend the language selection in `obs_module_set_locale` in `src/plugin-main.cpp`.
