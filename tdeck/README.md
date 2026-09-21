# T-Deck PDA firmware

Offline-first PDA firmware for the **LilyGO T-Deck Plus** (ESP32-S3, 2.8" ST7789
320x240, ES7210 mic, BBQ10-style I2C keyboard, trackball, microSD, L76K GPS).

A full app suite in a category-based icon launcher (Work / Tools / Media /
Network / Games / System) with switchable terminal phosphor themes: notes, to-do, audio
recording/playback, offline OSM map, clock/stopwatch/timer, calendar, WiFi,
battery, calculator, search, contacts, unit converter, file manager, ebook
reader, image viewer, wardrive logger, chess, Go, solitaire, and checkers.

The screen sleeps after 45 s of inactivity; any key or trackball event wakes
it. Long-press **BOOT** (>600 ms) is a universal back/exit from any screen.

## Build & flash

```bash
git clone https://github.com/lordkeynes/cardputer_radio_project
cd cardputer_radio_project
git checkout vibe-tdeck-notes-recorder
cd tdeck
pio run                # build
pio run -t upload      # build + flash over USB
pio device monitor     # 115200 serial console
```

## Launcher

The home screen shows six categories: **Work, Tools, Media, Network, Games,
System**. Trackball moves the selection, click opens a category grid of app
icons, click again launches. Long-press BOOT always backs out one level.
The keyboard backlight turns on at boot and follows screen sleep.

## Apps

### Notes
Browse/create/delete notes saved to `/notes/*.txt` on the SD card. In the
editor: type to write, **Enter** starts a new line, trackball click or **BOOT**
saves, long-press exits without saving.

### To-do
Checklist stored at `/todo/todo.txt` on the SD card. Up/down picks a task,
click/Enter toggles done, **n** adds a task, **d** deletes. Press **t** on a
task to open the due-date picker: quick presets (Today, Tomorrow, In 3 days,
Next week, In 2 weeks, In a month), a full month calendar picker, or Clear.
Dated tasks also appear on the calendar day they are due (yellow dots in the
month grid) and in a task list under it.

### Clock, stopwatch & timer
Clock shows time from GPS once a fix is acquired (the T-Deck has no RTC
battery; time is lost on power-off). Time syncs automatically at boot while
sitting on the launcher with a GPS fix. Clock mode tabs: **Clock** (click
switches), **Stopwatch** (click start/stop, **r** resets), **Timer** (u/d
±10 s, +/- 1 min, click starts, beeps when done, long-click cancels).

### Calendar
Month grid with a day cursor (left/right moves within the month, up/down
changes month). Days with due to-do items show a yellow dot; selecting a day
lists that day's tasks below the grid.

### Battery
Voltage estimate from the battery ADC (GPIO 4). Shown in the title bar
(color-coded percent) and in the Battery app (percent + millivolts). It is a
voltage-curve estimate, not a calibrated fuel gauge — treat percentages as
approximate, especially between 30–90%.

### WiFi
Scans networks, joins with on-device password entry via the keyboard, stores
credentials in `/wifi/known.txt` on SD, and auto-connects at boot to any known
network in range. The title bar shows `~~` when connected. Passwords live only
on the SD card.

### Recorder
Records mono WAV files to `/rec/`. A live peak meter shows input level while
recording. Play them back from the **Play** app.

### Map
Shows pre-rendered OpenStreetMap tiles from the SD card, centered on the GPS
position once a fix is acquired. Trackball pans, click re-centers on GPS,
`+`/`-` zoom, long-click exits. Without tiles the map still shows GPS
position/status on a dark grid.

**Preparing tiles:**

```bash
python3 tools/download_tiles.py \
  --sd /run/media/$USER/TDECK \
  --lat 39.7589 --lon -84.1916 \
  --zooms 10-16 --radius-tiles 6
```

Downloads OSM tiles around the given lat/lon (with automatic mirror fallback
if a tile server throttles you), converts them to raw little-endian RGB565,
and writes them as `/map/z<zoom>/<x>/<y>.bin` (256x256, 128 KB each).
Existing tiles are skipped, so it is safe to re-run. Requires internet on the
PC; the T-Deck itself never needs a network connection for maps.

### Calculator
Basic arithmetic with a recursive-descent expression parser: `+ - * / ( )`
and decimal points. Type an expression, Enter evaluates, long-click exits.

### Search
Full-text search across all notes and to-do items on the SD card.

### Contacts
Contact cards stored at `/contacts/contacts.txt` (TAB-separated
name/phone/email). **n** adds a contact with prompted fields, **d** deletes,
click dials into the detail view.

### Unit converter
Seven categories: length, mass, temperature, volume, speed, area, and data.
Left/right picks category, up/down picks the unit pair, typing digits builds
the input value, Enter converts.

### Files
Browse the whole SD card, view or delete files and directories.

### Ebook reader
Reads plain-text books from `/books/*.txt` on the SD card (up to 400 KB per
book, loaded into PSRAM). Left/right pages through; long-click exits.

**EPUB support:** EPUBs are converted on your PC first:

```bash
python3 tools/epub2txt.py mybook.epub /run/media/$USER/TDECK/books/
```

The tool unzips the EPUB, follows the book's spine order, strips the XHTML
down to plain text with paragraph breaks, and writes `mybook.txt` onto the SD
card. Standard library only — no extra Python packages needed.

### Image viewer
Displays raw RGB565 `.bin` images from `/images` on the SD card
(little-endian, 320x240 or smaller). Convert photos on your PC:

```bash
pip install Pillow
python3 tools/img2rgb565.py photo.jpg /run/media/$USER/TDECK/images/
```

### Wardrive
Passive WiFi survey logger: scans every 8 s and appends new networks to
`/wardrive/wifi.csv` in WiGLE-style format (BSSID, SSID, frequency, signal,
GPS coordinates if available). Purely passive observation for mapping
network coverage — no attack tooling.

### Games
- **Chess** — play against a 2-ply minimax AI. Trackball moves the cursor,
  click picks up/drops a piece; the AI plays white's response.
- **Go** — 9x9 board with full capture and suicide rules against a greedy AI.
  Click places a stone; the AI responds.
- **Solitaire** — Klondike, draw-1. Click moves cards to foundations, **d**
  draws from the stock.
- **Checkers** — against a jump-preferring AI. Click picks and moves men;
  jumps are enforced.
- **Snake** — trackball steers, click pauses, **n** restarts.
- **Stats** — wins/losses per game and the Snake high score, persisted to
  `/games/stats.txt`; **r** resets.

### Sports

Live and past scores from ESPN's public site API (no API key, WiFi required).
Fifteen leagues: NFL, NCAAF, NBA, WNBA, NCAAM, MLB, NHL, EPL, LaLiga, Serie A,
Bundesliga, Ligue 1, MLS, Champions League, MMA. Pick a league for the daily
scoreboard; **left/right** steps day by day (past results included), click
opens the game detail: per-quarter/period line scores plus a box-score table
(team statistics). Long-click backs out to the league picker.

### Terminal

An on-device shell. Type commands, Enter runs:
`ls [dir]`, `cat <file>`, `rm <file>`, `df` (card + free heap/PSRAM), `batt`,
`gps` (fix/sats), `ip`, `scan` (WiFi), `date`, `beep`, `clear`, `help`.
Long-click exits.

### Settings

Theme picker (Phosphor, Amber, Ice, Crimson, Mono) with live preview —
applied immediately and saved to `/config/theme.txt`. Screen-sleep timeout
(Never/15 s/30 s/45 s/1 min/2 min/5 min), saved to `/config/sleep.txt`.
The theme also restyles every app; the keyboard backlight follows the
screen-sleep setting automatically.

## Layout

```
tdeck/
├── boards/T-Deck.json     vendored board definition
├── include/
│   ├── apps.h             calculator/search/contacts/convert/files protos
│   ├── gamestats.h        game win/loss + high-score APIs
│   ├── games.h            chess/go/solitaire/checkers/snake/hub protos
│   ├── media.h            ebook/image/wardrive protos
│   ├── mapapp.h           slippy-tile math + tile renderer API
│   ├── pda.h              shared event codes, battery/clock/todo APIs
│   ├── settings.h         settings app protos
│   ├── terminal.h         terminal app protos
│   ├── theme.h            runtime-switchable theme API
│   └── utilities.h        T-Deck pin map
├── lib/es7210/            vendored ES7210 mic codec driver
├── src/
│   ├── main.cpp           shell: input, category launcher, notes, recorder, map
│   ├── apps.cpp           scientific calculator, search, contacts, converter, files
│   ├── gamestats.cpp      game win/loss + high-score persistence
│   ├── games.cpp          chess, Go, solitaire, checkers, Snake, games hub
│   ├── media.cpp          ebook reader, image viewer, wardrive logger
│   ├── pda.cpp            battery, clock/stopwatch/timer, calendar, to-do, sleep
│   ├── settings.cpp       theme picker, sleep timeout
│   ├── sports.cpp         ESPN scores: league picker, scoreboard, box score
│   ├── terminal.cpp       on-device shell
│   ├── theme.cpp          runtime themes
│   ├── wifiapp.cpp        WiFi scan/join, bands view, auto-connect
│   └── mapapp.cpp         tile math + SD tile rendering
└── tools/
    ├── download_tiles.py  OSM tile downloader / RGB565 converter
    ├── epub2txt.py        EPUB → plain-text converter for the ebook reader
    └── img2rgb565.py      PNG/JPG → RGB565 .bin converter for image viewer
```

## Debugging

Serial (115200) prints `[boot]` diagnostics during init and `[tb]` lines for
each trackball pulse — direction and GPIO — to help diagnose trackball issues.
