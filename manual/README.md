# Optional manual input

Place your own SNES manual at **`manual/Zombies Ate My Neighbors.pdf`**.
The tested reference PDF has 11 scan pages: front cover, nine inside spreads,
and back cover. A scan-based **`manual/Zombies Ate My Neighbors.epub`** is
also supported; text-only EPUBs are not. If both exist, PDF takes priority.

Install Python 3.10+ and run from the repository root:

```powershell
python -m pip install -r .\tools\manual-requirements.txt
```

Run `BUILD_XBOX_RELEASE.ps1` normally to generate `spread00.rgb565` through
`spread09.rgb565` automatically. See the main README for supported layouts.

You can instead place all ten preconverted spreads here. Each contains
1024 x 760 raw RGB565 pixels, little-endian, without a header: exactly
1,556,480 bytes. Put the ten spreads in reading order.

Only a complete valid set is copied into
`out/Zombies Ate My Neighbors!/manual/`. Missing assets or failed conversion
leave GAME MANUAL inactive and do not prevent the game from building.
Original documents and generated scans are ignored by Git; none are included
in the repository or downloaded by the build.
