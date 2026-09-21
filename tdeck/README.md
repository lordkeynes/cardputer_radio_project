# T-Deck Notes · Recorder · Maps

Offline-first PDA firmware for the **LilyGO T-Deck Plus** (ESP32-S3, 2.8" ST7789
320x240, ES7210 mic, BBQ10-style I2C keyboard, trackball, microSD, L76K GPS).
Apps: notes, to-do list, audio recording/playback, offline OSM map, clock,
calendar, WiFi, and battery status. The screen sleeps after 45 s of inactivity;
any key or trackball event wakes it.

## Build & flash

```bash
cd tdeck
pio run                # build
pio run -t upload      # build + flash over USB
pio device monitor     # 115200 serial console
```

## Apps

### Notes
Browse/create/delete notes saved to `/notes/*.txt` on the SD card. In the
editor: type to write, **Enter** starts a new line, trackball click or **BOOT**
saves, long-press exits without saving.

### To-do
Checklist app stored at `/todo/todo.txt` on the SD card. Trackball up/down picks
a task, click/Enter toggles done, **n** adds a new task (type it, Enter to
finish), **d** deletes the selected task, long-click exits and saves.

### Clock & calendar
Clock shows time from GPS once a fix is acquired (the T-Deck has no RTC
battery; time is lost on power-off). Calendar is a month grid; up/down
changes month, click returns to today.

### Battery
Voltage estimate from the battery ADC (GPIO 4). Shown in the main-menu status
bar and in a dedicated Battery screen (percent + millivolts). It is a voltage
curve estimate, not a calibrated fuel gauge — treat percentages as approximate,
especially between 30–90%.

### WiFi
Scans networks, joins with on-device password entry via the keyboard, stores
credentials in `/wifi/known.txt` on SD, and auto-connects at boot to any known
network in range. No cloud dependency; passwords live on the SD card.

### Recorder
Records mono WAV files to `/rec/`. A live peak meter shows input level while
recording. Files can be played back from the "Play recordings" menu item.

### Map
Shows pre-rendered OpenStreetMap tiles from the SD card, centered on the GPS
position once a fix is acquired.

**Controls:** trackball pans, click re-centers on GPS, `+`/`-` zoom, long-click
exits.

**Preparing tiles:**

```bash
python3 tools/download_tiles.py \
  --sd /media/$USER/SDCARD \
  --lat 47.6062 --lon -122.3321 \
  --zooms 10-16 --radius-tiles 6
```

Downloads OSM PNG tiles around the given lat/lon, converts them to raw
little-endian RGB565, and writes them to the SD card as
`/map/z<zoom>/<x>/<y>.bin` (256x256 pixels, 128 KB each). Existing tiles are
skipped, so the command is safe to re-run to extend coverage or add zoom
levels.

Without tiles on the card, the map still works — missing tiles render as dark
placeholders, and GPS position/status is still shown.

Note: tile downloads require internet on the PC running the script; the T-Deck
itself never needs a network connection.

## Layout

```
tdeck/
├── boards/T-Deck.json     vendored board definition
├── include/
│   ├── mapapp.h           slippy-tile math + tile renderer API
│   └── utilities.h        T-Deck pin map
├── lib/es7210/            vendored ES7210 mic codec driver
├── src/
│   ├── main.cpp           app shell: input, UI, notes, recorder, map
│   └── mapapp.cpp         tile math + SD tile rendering
└── tools/download_tiles.py  OSM tile downloader / RGB565 converter
```

## Debugging

Serial (115200) prints `[boot]` diagnostics during init and `[tb]` lines for
each trackball pulse — direction and GPIO — to help diagnose trackball issues.
Long-press **BOOT** (>600 ms) is a universal back/exit from any screen.
