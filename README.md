# Zombies Ate My Neighbors Original Xbox Native Port

Unofficial native port of **Zombies Ate My Neighbors** for the Original Xbox.

## Features

- Native translated game runtime with compatibility fallbacks
- Original Xbox rendering and audio through RXDK
- Enhanced / Wide audio by default
- Direct Connect and Public Rooms online co-op
- Online Solo play
- Server-backed profiles, World Chat and lobby chat
- Global Solo and Co-op leaderboards with best-time-only records
- Normal and Hardcore modes, with Savable and No-Save variants
- Synchronized online Save/Load
- Fast cached title return and responsive asynchronous online menus
- Modern Controls: LT previous weapon, RT next weapon, Black radar/map

<p align="center">
  <img src="https://i.imgur.com/cZhHFlY.png" width="700">
</p>

<p align="center">
  <img src="https://i.imgur.com/6uUCvYL.png" width="700">
</p>

<p align="center">
  <img src="https://i.imgur.com/vDXLcD7.png" width="700">
</p>



## Build requirements

- Windows
- [Team Resurgent RXDK](https://github.com/Team-Resurgent) installed at `C:\ProgramData\RXDK`
- A legally obtained USA **Zombies Ate My Neighbors** SNES ROM (`.sfc` or `.smc`)

ROMs, Xbox SDK/toolchain files, generated XBE/ISO files, runtime logs, title-cache
files, original manual scans, and the private online-server implementation are not included.

## Build

Place the ROM in the repository root, open PowerShell in that folder, and run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\BUILD_XBOX_RELEASE.ps1
```

Default output:

```text
out\Zombies Ate My Neighbors!\
```

If RXDK's `xdvdfs.exe` is installed, the build script also creates an XISO.

The build produces only the logging-free public Release version. The saved RXDK
Release configuration also disables runtime logging and diagnostic profiling.

Copy the entire **Zombies Ate My Neighbors!** folder to your console's games
location, for example `E:\Games\Zombies Ate My Neighbors!`, and launch
`default.xbe`. The folder contains:

```text
Zombies Ate My Neighbors!/
  default.xbe
  Zombies Ate My Neighbors.sfc
  audio_enhanced.flag
  manual/
    spread00.rgb565
    ...
    spread09.rgb565
```

The ROM is copied from the file you supply locally. `audio_enhanced.flag` is
created for the default Enhanced audio build. Optional Remastered builds also
copy the supplied `zamn-remastered.zsb` bank. The `manual/` folder is copied only
when all ten valid user-supplied or generated spreads are present. See the optional manual
instructions below.

## Optional official game manual

Original manual scans are not distributed with this source. Building and
playing the game does not require a manual. If any required spread is missing
or has an incorrect size, the build omits the manual, and selecting **GAME
MANUAL** stays on the main menu without opening anything.

### Automatic PDF or EPUB conversion

The reference manual is the SNES instruction booklet named **`Zombies Ate My
Neighbors.pdf`**: 11 scanned PDF pages, containing a front cover, nine inside
spreads, and a back cover. This is the PDF used to check the converter against
the existing manual layout. Place your own copy at this exact location:

```text
manual/Zombies Ate My Neighbors.pdf
```

The filename includes spaces. Put it beside `manual/README.md`, not in the
repository root or the build output. Do not upload your copy to GitHub.

For automatic conversion, install Python 3.10 or newer and these packages once:

```powershell
python -m pip install -r .\tools\manual-requirements.txt
```

Then run `BUILD_XBOX_RELEASE.ps1` normally. It automatically creates the ten
`spreadNN.rgb565` files and copies them into the console-ready game folder.
The front and back covers share the first spread, followed by the nine inside
spreads. Unchanged documents are cached so subsequent builds skip conversion.

A **scan-based EPUB** can alternatively be placed at:

```text
manual/Zombies Ate My Neighbors.epub
```

This is an accepted input filename, not a claim that every EPUB with that name
contains the correct manual. EPUB scan images must appear in reading order in
the EPUB spine. Supported layouts are 11 images (front cover, nine landscape
spreads, back cover), 20 single pages (front cover first and back cover last),
or 10 prepared landscape spreads. PDFs also accept these layouts. Text-only
or OCR-only EPUBs are unsupported. If both files exist, the PDF takes priority.
A different game edition or unrelated PDF renamed to this filename is not a
compatible manual.

No PDF/EPUB is included or downloaded by the build. These documents, generated
spreads, and conversion cache files are ignored by Git. If conversion fails,
dependencies are missing, or the document layout is unsupported, the build
continues **without the manual**, even if older spreads remain locally. If no
PDF/EPUB is supplied, a complete raw spread set can still be used as below.
Python and its packages are not needed when building without a document.

### Preconverted files

Alternatively, supply these exact files in the repository's `manual/`
folder, beside `manual/README.md`, before running the build:

```text
manual/spread00.rgb565
manual/spread01.rgb565
manual/spread02.rgb565
manual/spread03.rgb565
manual/spread04.rgb565
manual/spread05.rgb565
manual/spread06.rgb565
manual/spread07.rgb565
manual/spread08.rgb565
manual/spread09.rgb565
```

Each spread must be **1024 x 760 pixels**, uncompressed **RGB565** with
little-endian 16-bit pixels, row by row from the top left, with **no file header**.
Each file must be exactly **1,556,480 bytes**. A PDF, JPEG or PNG renamed to
`.rgb565` will not work. Supply the ten spreads in reading order.

The build copies a complete valid set to:

```text
out/Zombies Ate My Neighbors!/manual/spread00.rgb565
...
out/Zombies Ate My Neighbors!/manual/spread09.rgb565
```

You may also place the complete set in a `manual/` folder beside the console's
`default.xbe` after building. These local assets are ignored by Git.

## Controller update (R79)

- **LT** — previous weapon
- **RT** — next weapon
- **Black** — radar/map
- **X** — shoot/use weapon
- Main menu navigation and A selection use the default controls.

R79 corrects the direct-page address used by previous-weapon requests and the
runtime-to-controller player numbering. It includes the native runtime and
sprite/renderer updates accumulated since the previous published source.
Use matching builds on both consoles for online co-op.

## Online

```text
PUBLIC ROOMS
DIRECT CONNECT
SOLO PLAY
LEADERBOARDS
PROFILE
```

A signed-in profile is required for online play, chat, and leaderboards.

Gameplay modes:

- **Normal / Savable**  +1 life every 5 completed levels; Save/Load enabled
- **Normal / No Save**  same Normal rules; Save/Load disabled
- **Hardcore / Savable**  original gameplay; Save/Load enabled
- **Hardcore / No Save**  original gameplay; Save/Load disabled

Leaderboards are split into **Solo Online** and **Co-op Online** categories. Times
are ranked by deterministic gameplay frames and displayed as `MM:SS.mmm` with the
server record date.

Host lobby controls:

- **RS Click**  toggle **ROLLBACK: ENABLED / DISABLED**
- **Y** ” Lobby Chat in Public Rooms

During an online session:

- **START + BACK**  synchronized Save/Load menu
- **LT + RT + Right Stick Click**  leave the session and return to the ZAMN title

## Repository layout

- `src/`  native runtime, Xbox platform code, rendering/audio, and online client
- `third_party/lakesnes/` ” LakeSnes-derived compatibility/reference components
- `tools/`  validation and development utilities
- `LICENSES/`  third-party license notices
- `BUILD_XBOX_RELEASE.ps1`  public Release build entry point

## Credits / licensing

See [`CREDITS.md`](CREDITS.md) and [`LICENSING.md`](LICENSING.md).

## Disclaimer

Unofficial fan/homebrew project. Not affiliated with or endorsed by LucasArts /
Lucasfilm Games, Konami, Microsoft, Team Resurgent, or the upstream projects.
No game ROM is included.
