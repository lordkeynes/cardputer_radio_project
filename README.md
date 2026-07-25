# CardPuter Radio — Release 1.1

An internet radio player and stream recorder for the **M5Stack Cardputer-Adv**.

Plays internet radio through the Adv's ES8311 codec, records stations to SD with
timestamped filenames, remembers up to five WiFi networks, and resumes on its own
after a power loss. Tested stable across multi-hour unattended runs.

---

## Hardware

**Requires a Cardputer-Adv.** The original Cardputer has no I²S audio hardware and
will not produce sound with this firmware.

| | |
|---|---|
| Module | Stamp-S3A (ESP32-S3FN8) |
| Flash | 8 MB |
| **PSRAM** | **None** — this constrains the whole design; see *Design notes* |
| Audio | ES8311 codec → NS4150B amp → speaker / 3.5 mm jack |
| Keyboard | TCA8418 (I²C) |
| SD card | own SPI bus (SPI3), pins SCK 40 / MISO 39 / MOSI 14 / CS 12 |

---

## Controls

| Key | Action |
|---|---|
| **Space** / **Enter** | Play / pause |
| **N** | Next station |
| **P** | Previous station |
| **L** | Station list (`;` up, `.` down, **Enter** to tune, **L** to cancel) |
| **S** | Start / stop recording |
| **R** | Re-dial the current stream |
| **W** | Add a new WiFi network on the spot (blank name cancels) |
| **+** / **-** | Volume (0–21) |

Any keypress on a dimmed or dark screen **only wakes it** — it never fires an
action. Deliberate: glancing at the screen must not be able to kill a recording.

### Hidden: diagnostics / soak log

**Ctrl + D** cycles three states: off → on-screen overlay → overlay + serial soak
log → off. The overlay's uptime field turns green and reads `LOG` when soak
logging is armed. Serial output is otherwise near-silent in normal use.

```
BUF ████████████░░░░  94%   2h05m       <- buffer fill, uptime (LOG when armed)
gap:9ms dec:8 drw:35 rdl:0               <- audio timing, stream redials
heap:55k big:45k min:43k                 <- memory health
```

| Field | Meaning | Healthy |
|---|---|---|
| `BUF` | decoder input buffer fill | high and steady |
| `gap` | worst delay feeding the fetch loop | **< 20 ms** |
| `dec` | worst decode-loop time | small |
| `drw` | worst UI frame time | 30–60 ms is fine |
| `rdl` | stream watchdog re-dials | 0, or occasional on weak streams |
| `heap` | free heap | — |
| `big` | **largest contiguous free block** | tracks `heap`; must not decay |
| `min` | lowest heap since boot | **> 25 k** |

`gap`/`dec`/`drw` are high-water marks that reset when the overlay is re-opened
and are cleared automatically ~3 s after each station change (so a connect stall
doesn't mask later readings). `min` is a since-boot watermark and never resets.

**`big` matters most.** Free heap can look fine while the largest usable block
collapses — that is fragmentation, and it is what makes a later allocation fail.

---

## Stations

The SD card is **authoritative**: if `/station_list.txt` exists and parses, it
*replaces* the built-in list. Otherwise the nine built-in stations load. Editing
the card needs no rebuild.

```
# Format:  Station Name, Stream URL
# Lines starting with # are ignored.

WYSO 91.3 NPR, https://playerservices.streamtheworld.com/api/livestream-redirect/WYSOFM.mp3
WFMU 91.1 Freeform, http://stream0.wfmu.org/freeform-128k
```

If you edit the card and the original built-ins reappear, that is the tell that
**your file failed to parse**.

Two format notes:
- **MP3 and AAC streams both work.** In testing, MP3 stations held a full buffer
  for hours; some AAC feeds (e.g. WFPK) run slightly below realtime and get
  re-dialled every 30–60 min. For rock-solid unattended playback, prefer MP3.
- **HLS (`.m3u8`) will not record** — it is a playlist of segment files, not a
  byte stream. Playback may work; recording would save the playlist text. This
  rules out the BBC, which dropped its direct MP3 streams in 2023.

---

## Recording

Press **S**. Files land in `/rec/` with start *and* end times:

```
/rec/KEXP903Seatt_0724_1250-1418.mp3
```

Recording writes to `..._1250_rec.mp3` and renames on stop. If power is lost
mid-recording, the `_rec` file survives and plays — it just never got renamed.

- The **compressed stream is copied byte-for-byte**, no re-encoding (~16 KB/s).
- Recording uses its **own connection**, so you can record one station while
  listening to another, and an SD stall cannot glitch playback.
- A dropped connection **does not end the recording**: it reconnects and keeps
  appending to the same file (`REC 42:17 r3` = 3 reconnects; expect a 1–3 s gap
  at each seam).
- Short SD writes are retried and recovered rather than aborting.
- Recording is **plain HTTP only** (`https://` is rewritten) — a TLS handshake
  needs ~40 KB of free heap this board doesn't have while decoding.
- Refuses to start below ~10 KB contiguous heap (`REC FAIL -106`).
- **Known limit:** recording continuously for well over an hour slowly fragments
  the heap. It degrades gracefully (a *new* recording is refused; the running one
  is never corrupted), but very long single recordings are best avoided.

---

## WiFi

First boot prompts for SSID and password (masked). Up to **5** networks are
stored. On boot it scans and connects to whichever saved network is in range,
strongest first — so it does the right thing moving between locations. Hidden
SSIDs get a blind retry.

If saved networks exist but none are reachable, the radio **boots offline**
instead of blocking, and retries saved networks in the background (rotating
through all of them) every 30 s. Press **W** any time to add a network.

---

## Reliability

- **Auto-resume.** Volume, last station, and play state persist. If the device
  was playing when power was lost, it reconnects and resumes on boot.
- **Stream watchdog (two-tier).** Re-dials if the buffer craters (<15 % for 20 s)
  *or* stays chronically low (<45 % for 60 s). The second tier catches a stream
  that degrades without disconnecting.
- **Stream-format guard.** A live station never changes sample rate mid-stream;
  if the decoder resyncs onto a bogus header (a corruption symptom), the firmware
  re-dials rather than feed it garbage.

---

## Design notes

Choices that look odd but are load-bearing — easy to "fix" back into bugs:

**No off-screen framebuffer.** A 240×135 sprite costs 32 KB of internal RAM. With
no PSRAM, that RAM is worth far more as audio buffer. The UI draws directly and
avoids flicker by painting text over an opaque background.

**The draw path allocates nothing.** No `String` anywhere that runs per frame. An
earlier version rebuilt several `String`s per frame for the marquee; over an hour
that fragmented the heap badly enough to starve the network stack.

**Audio decode runs in the library's own task, with an enlarged stack.** The
library's internal `PeriodicTask` ships with a 3300-byte stack; the deep
floating-point filter path overflowed it and panicked after ~8 h. A pre-build
script (`patch_audio_stack.py`) raises it to 8192 bytes before compiling.

**The SD card is on its own SPI controller (SPI3); the display is on SPI2.** On
one bus, display DMA and SD writes corrupted each other. Same physical pins — the
ESP32-S3 routes any peripheral to any pin.

**`SPI.begin()` is never called on the global `SPI` object.** Its default MOSI is
GPIO11, which on the Adv is the keyboard's interrupt line. Claiming that pin
silently kills the keyboard.

---

## Build

```bash
pio run --target upload
pio device monitor -f esp32_exception_decoder    # 115200; auto-decodes panics
```

Watch for `[audio-stack] PeriodicTask stack 3300 -> 8192 bytes` in the build
output — that confirms the stack patch applied. If it ever prints
`WARNING: task creation line not found`, the audio library changed and the patch
needs revisiting.

A `pio system prune` notice and the `IRremote` wildcard warning during the build
are both harmless.

---

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| Keyboard dead | Something claimed GPIO11. Check nothing calls `SPI.begin()`. |
| No audio at all | Original Cardputer, not an Adv. |
| Stutter, `BUF` high | Not the network. Check `gap`; the buffer is full. |
| Stutter, `BUF` low | Network/server. Check `rssi`; the watchdog will re-dial. |
| `big` decaying over hours | Fragmentation — check whether a long recording is running. |
| Frequent `rdl` on one station | That station's feed is marginal (common on AAC). |
| `REC FAIL -101` | Couldn't connect (station may be HTTPS-only). |
| `REC FAIL -106` | Not enough contiguous heap to start. |
| `REC FAIL -108` | SD write failed after retries. |
| Built-ins reappear after editing SD | `station_list.txt` failed to parse. |

---

*Release 1.1 — verified across multiple multi-hour soak tests: no crashes, flat
memory in playback, corrupt-frame recovery, and clean auto-resume.*
