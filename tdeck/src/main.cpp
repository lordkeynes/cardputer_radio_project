/**
 * T-Deck Plus — Notes + Audio Recorder firmware
 * Notes are saved as .txt files on the SD card; recordings are saved as .wav.
 * Menu: trackball or arrow keys. Select: trackball click (BOOT button) or Enter.
 */
#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <SD.h>
#include <driver/i2s.h>
#include <Arduino_GFX_Library.h>
#include <TinyGPS++.h>
#include "es7210.h"
#include <WiFi.h>
#include "utilities.h"
#include "mapapp.h"
#include "pda.h"
#include "theme.h"
#include "apps.h"
#include "games.h"
#include "media.h"
#include "settings.h"
#include "terminal.h"
#include "sports.h"
#include "email.h"
#include "radio.h"
#include <Audio.h>

// Shared single-instance audio player (ESP32-audioI2S): used by playback app.
static Audio *sharedAudioPlayer = NULL;
static bool audioPlayerInit() {
  if (sharedAudioPlayer) return true;
  sharedAudioPlayer = new Audio(false, 3, I2S_NUM_0);
  return sharedAudioPlayer != NULL;
}
static Audio *audioPlayerGet() { return sharedAudioPlayer; }
static void audioPlayerDeinit() {
  if (sharedAudioPlayer) {
    sharedAudioPlayer->stopSong();
    delete sharedAudioPlayer;
    sharedAudioPlayer = NULL;
  }
}
static int audioPlayerDefaultVolume() { return 12; }

#define SCREEN_W 320
#define SCREEN_H 240
#define LINE_H 12

#define NOTE_DIR "/notes"
#define REC_DIR  "/recordings"

#define BOARD_GPS_RX_PIN 44
#define BOARD_GPS_TX_PIN 43
#define GPS_BAUD 9600
#define MAP_TILE_DIR "/map"
#define TILE_STEP 64

#define MIC_SAMPLE_RATE 16000
#define MIC_I2S_PORT I2S_NUM_1
#define SPK_I2S_PORT I2S_NUM_0

#define MAX_NOTE_SIZE 8192
#define MAX_FILES 32

Arduino_DataBus *bus = new Arduino_HWSPI(BOARD_TFT_DC, BOARD_TFT_CS);
Arduino_GFX *gfx = new Arduino_ST7789(bus, GFX_NOT_DEFINED /* RST */, 1 /* rotation */, false /* IPS */);

enum AppEvent { EV_NONE, EV_UP, EV_DOWN, EV_LEFT, EV_RIGHT, EV_SELECT, EV_LONGSELECT, EV_NEWLINE, EV_BACK, EV_SPACE, EV_CHAR, EV_DELETE };

struct InputEvent;
static bool getInput(InputEvent &e, unsigned long waitMs);


// Bridge declarations for pda.cpp / wifiapp.cpp
void wifiApp();
void wifiBandsApp();
bool wifiAutoConnect();
bool wifiConnected();

struct InputEvent {
  AppEvent ev;
  char ch;
  unsigned long ts;
};

// Bridge for pda.cpp / wifiapp.cpp: same event codes as AppEvent (minus EV_NONE).
bool pdaGetInput(InputEventP &pe, uint32_t waitMs) {
  InputEvent e;
  if (!getInput(e, waitMs)) return false;
  pe.ev = (uint8_t)e.ev;
  pe.ch = e.ch;
  pe.ts = e.ts;
  return true;
}


static QueueHandle_t inputQueue;
bool sdOk = false;   // non-static: used by pda.cpp/wifiapp.cpp

// ---------- Input: keyboard (I2C @0x55) + trackball ----------
#define LILYGO_KB_SLAVE_ADDRESS 0x55
#define LILYGO_KB_BRIGHTNESS_CMD 0x01

static bool kbAvailable = false;

// Keyboard backlight (keyboard MCU command 0x01, duty 0-255). Runs on the
// keyboard task's Wire bus to avoid two I2C masters fighting over it.
static volatile uint8_t kbWantBacklight = 128;
static volatile bool kbBacklightDirty = false;

void kbSetBacklight(uint8_t duty) {
  kbWantBacklight = duty;
  kbBacklightDirty = true;
}

static void keyboardTask(void *pv) {
  Wire.begin(BOARD_I2C_SDA, BOARD_I2C_SCL);
  delay(300);
  Wire.requestFrom(LILYGO_KB_SLAVE_ADDRESS, 1);
  kbAvailable = (Wire.read() != -1);
  if (kbAvailable) {
    // turn the keyboard backlight on at boot
    Wire.beginTransmission(LILYGO_KB_SLAVE_ADDRESS);
    Wire.write(LILYGO_KB_BRIGHTNESS_CMD);
    Wire.write(kbWantBacklight);
    Wire.endTransmission();
  }
  if (!kbAvailable) return;
  while (true) {
    if (kbBacklightDirty) {
      kbBacklightDirty = false;
      Wire.beginTransmission(LILYGO_KB_SLAVE_ADDRESS);
      Wire.write(LILYGO_KB_BRIGHTNESS_CMD);
      Wire.write(kbWantBacklight);
      Wire.endTransmission();
    }
    char keyValue = 0;
    Wire.requestFrom(LILYGO_KB_SLAVE_ADDRESS, 1);
    while (Wire.available() > 0) {
      keyValue = Wire.read();
      if (keyValue == 0x00) continue;
      InputEvent e = {};
      e.ts = millis();
      if (keyValue == '\n' || keyValue == '\r') {
        e.ev = EV_NEWLINE;
      } else if (keyValue == 0x08 || keyValue == 0x7F) {
        e.ev = EV_DELETE;
      } else if (keyValue == 0x1B) {
        e.ev = EV_BACK;
      } else {
        e.ev = EV_CHAR;
        e.ch = keyValue;
      }
      xQueueSend(inputQueue, &e, 0);
    }
    vTaskDelay(pdMS_TO_TICKS(15));
  }
}

#define TB_PIN_UP    BOARD_TBOX_G02
#define TB_PIN_RIGHT  BOARD_TBOX_G01
#define TB_PIN_LEFT  BOARD_TBOX_G04
#define TB_PIN_DOWN  BOARD_TBOX_G03

// The AN48841B optical sensors emit very short active-low pulses; a 10ms polling
// loop can miss them entirely. Count edges in ISRs, drain the counts in the task.
static volatile uint32_t tbPulse[4] = {0, 0, 0, 0};
static void IRAM_ATTR tbIsr0() { tbPulse[0]++; }
static void IRAM_ATTR tbIsr1() { tbPulse[1]++; }
static void IRAM_ATTR tbIsr2() { tbPulse[2]++; }
static void IRAM_ATTR tbIsr3() { tbPulse[3]++; }

static void screenSleepTick();

static void trackballTask(void *pv) {
  const uint8_t dir_pins[4] = {TB_PIN_RIGHT, TB_PIN_UP, TB_PIN_LEFT, TB_PIN_DOWN};
  const AppEvent dir_ev[4] = {EV_RIGHT, EV_UP, EV_LEFT, EV_DOWN};
  const char *dir_name[4] = {"RIGHT", "UP", "LEFT", "DOWN"};
  void (*const isrs[4])() = {tbIsr0, tbIsr1, tbIsr2, tbIsr3};
  uint32_t lastCount[4] = {0, 0, 0, 0};
  pinMode(BOARD_BOOT_PIN, INPUT_PULLUP);
  for (int i = 0; i < 4; i++) {
    pinMode(dir_pins[i], INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(dir_pins[i]), isrs[i], FALLING);
  }
  Serial.println("[tb] trackball ready (FALLING interrupts, pins R/U/L/D = 3/2/1/15)");
  bool lastBoot = true;
  unsigned long bootDownAt = 0;
  while (true) {
    for (int i = 0; i < 4; i++) {
      uint32_t n = tbPulse[i];
      if (n != lastCount[i]) {
        uint32_t newPulses = n - lastCount[i];
        lastCount[i] = n;
        Serial.printf("[tb] %s (pin %d) x%lu\n", dir_name[i], dir_pins[i], (unsigned long)newPulses);
        InputEvent e = {};
        e.ts = millis();
        e.ev = dir_ev[i];
        xQueueSend(inputQueue, &e, 0);
      }
    }
    bool boot = digitalRead(BOARD_BOOT_PIN);
    if (boot == false && lastBoot == true) {
      bootDownAt = millis();
    } else if (boot == false && bootDownAt != 0 && millis() - bootDownAt > 600) {
      InputEvent e = {};
      e.ts = millis();
      e.ev = EV_LONGSELECT;
      xQueueSend(inputQueue, &e, 0);
      bootDownAt = 0;
    } else if (boot == true && lastBoot == false && bootDownAt != 0) {
      InputEvent e = {};
      e.ts = millis();
      e.ev = EV_SELECT;
      xQueueSend(inputQueue, &e, 0);
      bootDownAt = 0;
    }
    lastBoot = boot;
    screenSleepTick();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

static bool getInput(InputEvent &e, unsigned long waitMs) {
  bool got = xQueueReceive(inputQueue, &e, pdMS_TO_TICKS(waitMs)) == pdTRUE;
  if (got) pdaNoteActivity();
  return got;
}

// Periodic screen-sleep check; called from the trackball task (10ms loop)
static void screenSleepTick() {
  static uint32_t lastTick = 0;
  if (millis() - lastTick < 500) return;
  lastTick = millis();
  pdaScreenTick();
}

// ---------- UI helpers ----------
static void uiInit() {
  pinMode(BOARD_SDCARD_CS, OUTPUT);
  pinMode(RADIO_CS_PIN, OUTPUT);
  pinMode(BOARD_TFT_CS, OUTPUT);
  digitalWrite(BOARD_SDCARD_CS, HIGH);
  digitalWrite(RADIO_CS_PIN, HIGH);
  digitalWrite(BOARD_TFT_CS, HIGH);
  pinMode(BOARD_SPI_MISO, INPUT_PULLUP);
  SPI.begin(BOARD_SPI_SCK, BOARD_SPI_MISO, BOARD_SPI_MOSI);
  gfx->begin(40000000);
  pinMode(BOARD_BL_PIN, OUTPUT);
  digitalWrite(BOARD_BL_PIN, HIGH);
  gfx->fillScreen(BLACK);
  gfx->setTextColor(WHITE, BLACK);
  gfx->setTextSize(2);
}

// Status indicators in the title bar, right side: [WiFi] [Batt%]
static void drawTitleBarIndicators() {
  gfx->setTextSize(1);
  int x = SCREEN_W - 4;
  // Battery: icon (outline + fill proportional) + percent
  int pct = batteryPercent();
  uint16_t battColor = pct > 50 ? TERM_GREEN : pct > 20 ? TERM_ACCENT : TERM_RED;
  char b[8];
  snprintf(b, sizeof(b), "%d%%", pct);
  x -= strlen(b) * 6;
  gfx->setTextColor(battColor, BLACK);
  gfx->setCursor(x, 6);
  gfx->print(b);
  x -= 3;   // gap to icon
  gfx->drawRect(x - 14, 5, 12, 8, TERM_DIM);
  gfx->fillRect(x - 13, 6, (10 * pct) / 100 + ((pct > 0) ? 1 : 0), 6, battColor);
  gfx->fillRect(x - 2, 7, 2, 4, TERM_DIM);
  x -= 18;
  // WiFi: signal-strength bars (3 bars, tallest when strong)
  if (wifiConnected()) {
    int rssi = WiFi.RSSI();
    int bars = rssi >= -55 ? 4 : rssi >= -67 ? 3 : rssi >= -78 ? 2 : 1;
    for (int i = 0; i < 4; i++) {
      int h = 2 + i * 3;
      gfx->drawFastVLine(x - 4 - i * 4, 13 - h, h,
                         i < bars ? TERM_CYAN : RGB565(40, 40, 40));
    }
    x -= 20;
  }
}
static void drawTitle(const char *title) {
  gfx->fillScreen(BLACK);
  gfx->setTextSize(1);
  gfx->setTextColor(TERM_GREEN, BLACK);
  gfx->setCursor(4, 7);
  gfx->println(title);
  drawTitleBarIndicators();
  gfx->drawFastHLine(0, 18, SCREEN_W, TERM_DIM);
  gfx->setTextColor(WHITE, BLACK);
  gfx->setTextSize(1);
}

// Scrolling menu list geometry: compact size-1 rows to match other apps.
#define MENU_ROW_H 16
#define MENU_TOP 26
static int menuVisibleRows() {
  int area = SCREEN_H - 18 - MENU_TOP;
  int rows = area / MENU_ROW_H;
  return rows < 1 ? 1 : rows;
}
static int itemY(int i, int n) {
  (void)n;
  return MENU_TOP + i * MENU_ROW_H + 4;
}
static int itemH(int n) {
  (void)n;
  return MENU_ROW_H - 4;
}


static void drawStatus(const char *msg) {
  gfx->setTextSize(1);
  gfx->setTextColor(TERM_ACCENT, BLACK);
  gfx->setCursor(4, SCREEN_H - 10);
  gfx->print("                               ");
  gfx->setCursor(4, SCREEN_H - 10);
  gfx->print(msg);
  gfx->setTextColor(WHITE, BLACK);
}

static uint32_t uiGen = 0;

static void drawMenuList(const char *title, const char *const *items, int n, int sel, const char *status) {
  static int lastSel = -1;
  static int lastTop = 0;          // first visible item index
  static const char *lastTitle = NULL;
  static const char *const *lastItems = NULL;
  static int lastN = 0;
  static uint32_t lastGen = 0;
  int visible = menuVisibleRows();
  // Keep selection inside the visible window; scroll when it leaves.
  int top = lastTop;
  if (sel < top) top = sel;
  if (sel >= top + visible) top = sel - visible + 1;
  if (top > n - 1) top = n > 0 ? n - 1 : 0;
  if (top < 0) top = 0;
  bool fullRepaint = (uiGen != lastGen || title != lastTitle || lastSel < 0 ||
                      items != lastItems || n != lastN || top != lastTop);
  if (fullRepaint) {
    lastGen = uiGen;
    drawTitle(title);
    lastTitle = title;
    lastItems = items;
    lastN = n;
    lastSel = -1;
    // Clear the list area (title stays)
    gfx->fillRect(0, MENU_TOP - 4, SCREEN_W, SCREEN_H - 18 - (MENU_TOP - 4), BLACK);
    for (int i = top; i < n && i < top + visible; i++) {
      int y = itemY(i - top, n);
      if (i == sel) {
        gfx->fillRect(0, y - 4, SCREEN_W, itemH(n), TERM_SEL_BG);
        gfx->setTextColor(BLACK, TERM_SEL_BG);
      } else {
        gfx->setTextColor(WHITE, BLACK);
      }
      gfx->setTextSize(1);
      gfx->setCursor(8, y + 2);
      gfx->println(items[i]);
    }
    // Scroll indicators
    gfx->setTextSize(1);
    gfx->setTextColor(RGB565(150, 150, 150), BLACK);
    if (top > 0) {
      gfx->setCursor(SCREEN_W - 12, MENU_TOP + 2);
      gfx->print("^");
    }
    if (top + visible < n) {
      gfx->setCursor(SCREEN_W - 12, MENU_TOP + (visible - 1) * MENU_ROW_H + 8);
      gfx->print("v");
    }
    if (status) {
      gfx->setTextSize(1);
      gfx->setTextColor(WHITE, BLACK);
      gfx->setCursor(4, SCREEN_H - 10);
      gfx->println(status);
    }
  } else if (sel != lastSel) {
    int yPrev = itemY(lastSel - top, n);
    int yNew = itemY(sel - top, n);
    gfx->fillRect(0, yPrev - 4, SCREEN_W, itemH(n), BLACK);
    gfx->setTextSize(1);
    gfx->setTextColor(WHITE, BLACK);
    gfx->setCursor(8, yPrev + 2);
    gfx->println(items[lastSel]);
    gfx->fillRect(0, yNew - 4, SCREEN_W, itemH(n), RGB565(0, 120, 255));
    gfx->setTextColor(BLACK, TERM_SEL_BG);
    gfx->setCursor(8, yNew + 2);
    gfx->println(items[sel]);
  }
  lastTop = top;
  lastSel = sel;
}

static void uiScreenChanged() { uiGen++; }


// ---------- SD ----------
static bool sdInit() {
  if (!SD.begin(BOARD_SDCARD_CS, SPI, 8000000)) return false;
  if (!SD.exists(NOTE_DIR)) SD.mkdir(NOTE_DIR);
  if (!SD.exists(REC_DIR)) SD.mkdir(REC_DIR);
  return true;
}

// ---------- GPS ----------
TinyGPSPlus gps;   // non-static: used by media.cpp wardrive
static HardwareSerial &gpsSerial = Serial1;
static uint32_t gpsCharsSeen = 0;

static bool gpsSetup() {
  gpsSerial.begin(GPS_BAUD, SERIAL_8N1, BOARD_GPS_RX_PIN, BOARD_GPS_TX_PIN);
  delay(100);
  // L76K init: NMEA on, GPS+GLONASS, vehicle mode.
  // Harmless if the module is a u-blox variant (ignores $PCAS sentences).
  gpsSerial.write("$PCAS03,1,1,1,1,1,1,1,1,1,1,,,0,0*02\r\n");
  delay(250);
  gpsSerial.write("$PCAS04,5*1C\r\n");
  delay(250);
  gpsSerial.write("$PCAS11,3*1E\r\n");
  return true;
}

// Feed the parser whatever NMEA bytes have arrived; non-blocking.
void gpsPoll() {
  while (gpsSerial.available()) { gps.encode(gpsSerial.read()); gpsCharsSeen++; }
}

// ---------- Map app ----------
static int mapZoom = 14;
static double mapCenterLat = 47.6062;   // fallback default: Seattle
static double mapCenterLon = -122.3321;
static bool mapFollowGps = false;   // start on the downloaded tiles, not raw GPS
static bool mapTilesScanned = false;

// On first map entry, find tiles on the SD and center/zoom on them so the app
// shows something useful even before a GPS fix. Picks the deepest zoom level
// present, then centers on the middle of that level's x/y tile range.
static void mapScanTiles() {
  if (!sdOk) return;
  int bestZoom = -1;
  uint32_t bestX = 0, bestY = 0;
  for (int z = 18; z >= 1; z--) {
    String zdir = String(MAP_TILE_DIR) + "/z" + String(z);
    if (!SD.exists(zdir)) continue;
    File zfl = SD.open(zdir);
    if (!zfl) continue;
    long minX = -1, maxX = -1, minY = -1, maxY = -1;
    File xdir;
    while ((xdir = zfl.openNextFile())) {
      if (!xdir.isDirectory()) { xdir.close(); continue; }
      char *end = nullptr;
      long xv = strtol(xdir.name(), &end, 10);
      if (!end || *end != '\0' || xv < 0) { xdir.close(); continue; }
      if (minX < 0 || xv < minX) minX = xv;
      if (maxX < 0 || xv > maxX) maxX = xv;
      File ydir;
      while ((ydir = xdir.openNextFile())) {
        if (ydir.isDirectory()) { ydir.close(); continue; }
        if (!String(ydir.name()).endsWith(".bin")) { ydir.close(); continue; }
        String yname = ydir.name();
        yname.remove(yname.length() - 4);
        char *yend = nullptr;
        long yv = strtol(yname.c_str(), &yend, 10);
        if (yend && *yend == '\0' && yv >= 0) {
          if (minY < 0 || yv < minY) minY = yv;
          if (maxY < 0 || yv > maxY) maxY = yv;
        }
        ydir.close();
      }
      xdir.close();
    }
    zfl.close();
    if (minX < 0 || minY < 0) continue;
    bestZoom = z;
    bestX = (uint32_t)((minX + maxX) / 2);
    bestY = (uint32_t)((minY + maxY) / 2);
    break;
  }
  if (bestZoom > 0) {
    mapZoom = bestZoom;
    mapCenterLat = tileYToLat(bestY, bestZoom);
    mapCenterLon = tileXToLon(bestX, bestZoom);
    Serial.printf("[map] tiles found: z%d x%u y%u -> %.5f,%.5f\n",
                  bestZoom, (unsigned)bestX, (unsigned)bestY, mapCenterLat, mapCenterLon);
  } else {
    Serial.println("[map] no tiles found on SD");
  }
  mapTilesScanned = true;
}

static void mapApp() {
  const int step = TILE_STEP;
  bool needsRedraw = true;
  static char statusLine[64];
  if (!mapTilesScanned) mapScanTiles();
  // Center in global pixel space at the current zoom (kept as double so
  // panning moves in smooth pixel steps instead of snapping to tile edges).
  double n = pow(2, mapZoom);
  double latRad = mapCenterLat * M_PI / 180.0;
  double centerPixelXf = (mapCenterLon + 180.0) / 360.0 * n * 256.0;
  double centerPixelYf = (1.0 - log(tan(latRad) + 1.0 / cos(latRad)) / M_PI) / 2.0 * n * 256.0;
  uint32_t lastRedrawMs = 0;
  while (true) {
    gpsPoll();
    bool hasFix = gps.location.isValid();
    if (hasFix && gps.time.isValid() && gps.date.isValid() && !pdaTimeSynced()) {
      pdaApplyGpsTime(gps.date.year(), gps.date.month(), gps.date.day(),
                     gps.time.hour(), gps.time.minute(), gps.time.second());
    }
    if (mapFollowGps && hasFix) {
      // Recenter only if GPS moved meaningfully (~2px at this zoom) or 2s passed,
      // so position jitter doesn't trigger constant full redraws.
      double nn = pow(2, mapZoom);
      double lr = gps.location.lat() * M_PI / 180.0;
      double gpx = (gps.location.lng() + 180.0) / 360.0 * nn * 256.0;
      double gpy = (1.0 - log(tan(lr) + 1.0 / cos(lr)) / M_PI) / 2.0 * nn * 256.0;
      if (fabs(gpx - centerPixelXf) > 2 || fabs(gpy - centerPixelYf) > 2) {
        centerPixelXf = gpx;
        centerPixelYf = gpy;
        needsRedraw = true;
      }
    }
    if (needsRedraw && millis() - lastRedrawMs > 250) {
      needsRedraw = false;
      lastRedrawMs = millis();
      mapCenterLon = (centerPixelXf / (n * 256.0)) * 360.0 - 180.0;
      double nnlat = M_PI - 2.0 * M_PI * centerPixelYf / (n * 256.0);
      mapCenterLat = 180.0 / M_PI * atan(0.5 * (exp(nnlat) - exp(-nnlat)));
      int centerPixelX = (int)centerPixelXf;
      int centerPixelY = (int)centerPixelYf;
      mapAppRender(gfx, &gps, centerPixelX, centerPixelY, mapZoom, SCREEN_W, SCREEN_H - 16);
      // marker at center
      int mx = SCREEN_W / 2, my = (SCREEN_H - 16) / 2;
      gfx->fillCircle(mx, my, 3, RED);
      gfx->drawCircle(mx, my, 6, RED);
      // GPS position marker (blue dot) when fixed, if not centered
      if (hasFix && !mapFollowGps) {
        double nn = pow(2, mapZoom);
        double lr = gps.location.lat() * M_PI / 180.0;
        double gpx = (gps.location.lng() + 180.0) / 360.0 * nn * 256.0;
        double gpy = (1.0 - log(tan(lr) + 1.0 / cos(lr)) / M_PI) / 2.0 * nn * 256.0;
        int dx = (int)lround(gpx - centerPixelXf) + mx;
        int dy = (int)lround(gpy - centerPixelYf) + my;
        if (dx >= 3 && dx < SCREEN_W - 3 && dy >= 3 && dy < SCREEN_H - 19) {
          gfx->fillCircle(dx, dy, 4, RGB565(0, 120, 255));
          gfx->drawCircle(dx, dy, 6, WHITE);
        }
      }
      // status bar
      gfx->fillRect(0, SCREEN_H - 16, SCREEN_W, 16, BLACK);
      gfx->setTextSize(1);
      gfx->setTextColor(TERM_ACCENT, BLACK);
      gfx->setCursor(2, SCREEN_H - 12);
      snprintf(statusLine, sizeof(statusLine), "z%d %s %.5f,%.5f  sats:%d",
               mapZoom, hasFix ? "GPS" : "no-fix",
               mapCenterLat, mapCenterLon,
               gps.satellites.isValid() ? gps.satellites.value() : 0);
      gfx->print(statusLine);
    }
    InputEvent e;
    if (!getInput(e, 100)) continue;
    switch (e.ev) {
      case EV_UP:    mapFollowGps = false; centerPixelYf -= step; needsRedraw = true; break;
      case EV_DOWN:  mapFollowGps = false; centerPixelYf += step; needsRedraw = true; break;
      case EV_LEFT:  mapFollowGps = false; centerPixelXf -= step; needsRedraw = true; break;
      case EV_RIGHT: mapFollowGps = false; centerPixelXf += step; needsRedraw = true; break;
      case EV_SELECT: {
        mapFollowGps = true;
        if (hasFix) {
          double nn = pow(2, mapZoom);
          double lr = gps.location.lat() * M_PI / 180.0;
          centerPixelXf = (gps.location.lng() + 180.0) / 360.0 * nn * 256.0;
          centerPixelYf = (1.0 - log(tan(lr) + 1.0 / cos(lr)) / M_PI) / 2.0 * nn * 256.0;
        }
        needsRedraw = true; break;
      }
      case EV_LONGSELECT: return;
      case EV_CHAR:
        if (e.ch == '+' || e.ch == '=') {
          if (mapZoom < 18) {
            mapZoom++;
            centerPixelXf *= 2;
            centerPixelYf *= 2;
            n = pow(2, mapZoom);
            needsRedraw = true;
          }
        } else if (e.ch == '-') {
          if (mapZoom > 1) {
            mapZoom--;
            centerPixelXf /= 2;
            centerPixelYf /= 2;
            n = pow(2, mapZoom);
            needsRedraw = true;
          }
        }
        break;
      default: break;
    }
  }
}

// ---------- Notes app ----------
static char noteBuf[MAX_NOTE_SIZE];

static void listFiles(const char *dir, String names[], int &count, const char *ext) {
  count = 0;
  File root = SD.open(dir);
  if (!root || !root.isDirectory()) return;
  File f = root.openNextFile();
  while (f && count < MAX_FILES) {
    if (!f.isDirectory() && String(f.name()).endsWith(ext)) {
      names[count++] = String(dir) + "/" + String(f.name());
    }
    f = root.openNextFile();
  }
  root.close();
}

static String baseName(const String &path) {
  int slash = path.lastIndexOf('/');
  String n = (slash >= 0) ? path.substring(slash + 1) : path;
  int dot = n.lastIndexOf('.');
  return (dot > 0) ? n.substring(0, dot) : n;
}

static int pickFile(const char *title, const char *dir, const char *ext, String &outPath) {
  String names[MAX_FILES];
  int count = 0;
  listFiles(dir, names, count, ext);
  const int visible = 13;
  int sel = -1;
  int scroll = -1;
  bool needsRedraw = true;
  while (true) {
    if (needsRedraw) {
      drawTitle(title);
      gfx->setTextSize(1);
      if (sel == -1) {
        gfx->fillRect(0, 28, SCREEN_W, 13, RGB565(0, 120, 255));
        gfx->setTextColor(BLACK, TERM_SEL_BG);
      } else {
        gfx->setTextColor(TERM_ACCENT, BLACK);
      }
      gfx->setCursor(6, 30);
      gfx->println("< Back");
      gfx->setTextColor(WHITE, BLACK);
      int maxScroll = count > visible ? count - visible : 0;
      if (scroll > maxScroll) scroll = maxScroll;
      if (scroll < -1) scroll = -1;
      for (int i = 0; i < visible && scroll + 1 + i < count; i++) {
        int y = 44 + i * 14;
        int idx = scroll + 1 + i;
        if (idx == sel) {
          gfx->fillRect(0, y - 2, SCREEN_W, 13, RGB565(0, 120, 255));
          gfx->setTextColor(BLACK, TERM_SEL_BG);
        } else {
          gfx->setTextColor(WHITE, BLACK);
        }
        gfx->setCursor(6, y);
        gfx->println(baseName(names[idx]));
      }
      gfx->setTextColor(WHITE, BLACK);
      gfx->setCursor(4, SCREEN_H - 10);
      gfx->println("Click = select  Long-click = back");
      needsRedraw = false;
    }
    InputEvent e;
    if (!getInput(e, 50)) continue;
    if (e.ev == EV_UP && sel > -1) { sel--; needsRedraw = true; }
    else if (e.ev == EV_DOWN && sel < count - 1) { sel++; needsRedraw = true; }
    else if (e.ev == EV_SELECT || e.ev == EV_NEWLINE) {
      if (sel == -1 || count == 0) return -1;
      outPath = names[sel];
      return sel;
    } else if (e.ev == EV_BACK || e.ev == EV_LEFT || e.ev == EV_LONGSELECT) return -1;
    if (sel == -1) { if (scroll != -1) { scroll = -1; needsRedraw = true; } }
    else if (sel < scroll + 1) { scroll = sel - 1; needsRedraw = true; }
    else if (sel >= scroll + 1 + visible) { scroll = sel - visible; needsRedraw = true; }
  }
}

static int textEditor(const String &path, char *buf, size_t bufSize, bool isNew) {
  size_t len = 0;
  if (!isNew) {
    File f = SD.open(path, FILE_READ);
    if (f) {
      len = f.readBytes(buf, bufSize - 1);
      f.close();
    }
  } else {
    buf[0] = '\0';
  }
  buf[len] = '\0';
  int cursor = len;
  int scroll = 0;
  const int visibleChars = 50;
  const int visibleLines = 14;
  bool needsRedraw = true;
  uint16_t redrawHash = 0;
  while (true) {
    uint16_t curLine = 0;
    for (size_t i = 0; i < cursor && i < len; i++) if (buf[i] == '\n') curLine++;
    if (curLine < scroll) scroll = curLine;
    if (curLine >= scroll + visibleLines) scroll = curLine - visibleLines + 1;
    if (scroll < 0) scroll = 0;
    uint16_t hash = (uint16_t)((len & 0xFF) ^ (scroll << 8) ^ (cursor & 0xFF));
    if (needsRedraw || hash != redrawHash) {
      redrawHash = hash;
      needsRedraw = false;
      drawTitle(baseName(path).c_str());
      gfx->setTextSize(1);
      int lineStarts[40];
      int nLines = 1;
      lineStarts[0] = 0;
      for (size_t i = 0; i < len && nLines < 40; i++) {
        if (buf[i] == '\n') lineStarts[nLines++] = i + 1;
      }
      if (scroll >= nLines) scroll = nLines - 1;
      for (int i = 0; i < visibleLines && scroll + i < nLines; i++) {
        int start = lineStarts[scroll + i];
        int end = (scroll + i + 1 < nLines) ? lineStarts[scroll + i + 1] - 1 : len;
        if (end - start > visibleChars) end = start + visibleChars;
        gfx->setCursor(6, 30 + i * 14);
        gfx->setTextColor(WHITE, BLACK);
        for (int j = start; j < end; j++) gfx->print(buf[j]);
      }
      drawStatus("Click=save  Enter=newline  Bksp=del  Long-click=exit");
    }
    InputEvent e;
    if (!getInput(e, 50)) continue;
    switch (e.ev) {
      case EV_CHAR:
        if (len < bufSize - 2 && cursor <= (int)len) {
          memmove(buf + cursor + 1, buf + cursor, len - cursor);
          buf[cursor++] = e.ch;
          len++;
          buf[len] = '\0';
          needsRedraw = true;
        }
        break;
      case EV_SPACE: break;
      case EV_DELETE:
        if (cursor > 0) {
          memmove(buf + cursor - 1, buf + cursor, len - cursor);
          cursor--;
          len--;
          buf[len] = '\0';
          needsRedraw = true;
        }
        break;
      case EV_LEFT:
        if (cursor > 0) cursor--;
        else return -1;
        break;
      case EV_RIGHT: if (cursor < (int)len) cursor++; break;
      case EV_UP: while (cursor > 0 && buf[cursor - 1] != '\n') cursor--; if (cursor > 0) cursor--; break;
      case EV_DOWN: while (cursor < (int)len && buf[cursor] != '\n') cursor++; if (cursor < (int)len) cursor++; break;
      case EV_SELECT: {
        File f = SD.open(path, FILE_WRITE);
        if (f) {
          f.print(buf);
          f.close();
          return 0;
        }
        return -1;
      }
      case EV_NEWLINE:
        if (len < bufSize - 2 && cursor <= (int)len) {
          memmove(buf + cursor + 1, buf + cursor, len - cursor);
          buf[cursor++] = '\n';
          len++;
          buf[len] = '\0';
          needsRedraw = true;
        }
        break;
      case EV_BACK: return -1;
      case EV_LONGSELECT: return -1;
      default: break;
    }
  }
}

static void notesApp() {
  const char *items[] = {"New note", "Open note", "Delete note", "To-do list", "Back"};
  const int nItems = 5;
  int sel = 0;
  while (true) {
    drawMenuList("Notes", items, nItems, sel, NULL);
    InputEvent e;
    if (!getInput(e, 50)) continue;
    if (e.ev == EV_UP) sel = (sel + nItems - 1) % nItems;
    else if (e.ev == EV_DOWN) sel = (sel + 1) % nItems;
    else if (e.ev == EV_BACK || (e.ev == EV_LEFT) || e.ev == EV_LONGSELECT) return;
    else if (e.ev == EV_SELECT || e.ev == EV_NEWLINE) {
      if (sel == 4) return;
      if (sel == 0) {
        String names[MAX_FILES];
        int count = 0;
        listFiles(NOTE_DIR, names, count, ".txt");
        int idx = 1;
        String path;
        do {
          path = String(NOTE_DIR) + "/note" + String(idx) + ".txt";
          bool exists = false;
          for (int i = 0; i < count; i++) if (names[i] == path) { exists = true; break; }
          if (!exists) break;
          idx++;
        } while (idx < 999);
        textEditor(path, noteBuf, MAX_NOTE_SIZE, true);
      } else if (sel == 1) {
        String path;
        if (pickFile("Open note", NOTE_DIR, ".txt", path) >= 0) {
          textEditor(path, noteBuf, MAX_NOTE_SIZE, false);
        }
      } else if (sel == 2) {
        String path;
        if (pickFile("Delete note", NOTE_DIR, ".txt", path) >= 0) {
          SD.remove(path);
        }
      } else if (sel == 3) {
        todoApp();
      }
      uiScreenChanged();
    }
  }
}

// ---------- Audio: mic (ES7210 via I2S) ----------
static bool micSetup() {
  Wire.begin(BOARD_I2C_SDA, BOARD_I2C_SCL);
  delay(50);
  audio_hal_codec_config_t cfg = {
    .adc_input = AUDIO_HAL_ADC_INPUT_ALL,
    .codec_mode = AUDIO_HAL_CODEC_MODE_ENCODE,
    .i2s_iface = {
      .mode = AUDIO_HAL_MODE_SLAVE,
      .fmt = AUDIO_HAL_I2S_NORMAL,
      .samples = AUDIO_HAL_16K_SAMPLES,
      .bits = AUDIO_HAL_BIT_LENGTH_16BITS,
    },
  };
  uint32_t ret = ESP_OK;
  ret |= es7210_adc_init(&Wire, &cfg);
  ret |= es7210_adc_config_i2s(cfg.codec_mode, &cfg.i2s_iface);
  ret |= es7210_adc_set_gain((es7210_input_mics_t)(ES7210_INPUT_MIC1 | ES7210_INPUT_MIC2), (es7210_gain_value_t)GAIN_30DB);
  ret |= es7210_adc_ctrl_state(cfg.codec_mode, AUDIO_HAL_CTRL_START);
  if (ret != ESP_OK) return false;

  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = MIC_SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 256,
    .use_apll = false,
    .tx_desc_auto_clear = true,
    .fixed_mclk = 0,
    .mclk_multiple = I2S_MCLK_MULTIPLE_256,
    .bits_per_chan = I2S_BITS_PER_CHAN_16BIT,
  };
  i2s_pin_config_t pins = {};
  pins.mck_io_num = BOARD_ES7210_MCLK;
  pins.bck_io_num = BOARD_ES7210_SCK;
  pins.ws_io_num = BOARD_ES7210_LRCK;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num = BOARD_ES7210_DIN;
  if (i2s_driver_install(MIC_I2S_PORT, &i2s_config, 0, NULL) != ESP_OK) return false;
  i2s_set_pin(MIC_I2S_PORT, &pins);
  i2s_zero_dma_buffer(MIC_I2S_PORT);
  return true;
}

struct WavHeader {
  char riff[4] = {'R', 'I', 'F', 'F'};
  uint32_t chunkSize = 0;
  char wave[4] = {'W', 'A', 'V', 'E'};
  char fmt[4] = {'f', 'm', 't', ' '};
  uint32_t fmtSize = 16;
  uint16_t audioFormat = 1;
  uint16_t numChannels = 1;
  uint32_t sampleRate = MIC_SAMPLE_RATE;
  uint32_t byteRate = MIC_SAMPLE_RATE * 2;
  uint16_t blockAlign = 2;
  uint16_t bitsPerSample = 16;
  char data[4] = {'d', 'a', 't', 'a'};
  uint32_t dataSize = 0;
} __attribute__((packed));

static void recorderApp() {
  drawTitle("Recorder");
  gfx->setTextSize(2);
  gfx->setCursor(8, 40);
  gfx->println("Click/Enter = start REC");
  gfx->setTextSize(1);
  gfx->setCursor(8, 70);
  gfx->println("Long-click = back");
  drawStatus("Ready");
  InputEvent e;
  while (getInput(e, portMAX_DELAY)) {
    if (e.ev == EV_SELECT || e.ev == EV_NEWLINE) break;
    if (e.ev == EV_BACK || e.ev == EV_LEFT || e.ev == EV_LONGSELECT) return;
  }

  String names[MAX_FILES];
  int count = 0;
  listFiles(REC_DIR, names, count, ".wav");
  int idx = 1;
  String path;
  do {
    path = String(REC_DIR) + "/rec" + String(idx) + ".wav";
    bool exists = false;
    for (int i = 0; i < count; i++) if (names[i] == path) { exists = true; break; }
    if (!exists) break;
    idx++;
  } while (idx < 999);

  File f = SD.open(path, FILE_WRITE);
  if (!f) { drawStatus("SD write error"); delay(1500); return; }

  WavHeader hdr;
  f.write((uint8_t *)&hdr, sizeof(hdr));

  const int bufSamples = 1600;
  static int16_t audioBuf[bufSamples * 2];
  size_t bytesRead = 0;
  uint32_t totalSamples = 0;
  uint32_t recStart = millis();
  bool stop = false;

  drawTitle("REC");
  gfx->setTextSize(2);
  gfx->setTextColor(RED, BLACK);
  gfx->setCursor(8, 40);
  gfx->println("RECORDING...");
  gfx->setTextColor(WHITE, BLACK);

  while (!stop) {
    i2s_read(MIC_I2S_PORT, (char *)audioBuf, bufSamples * sizeof(int16_t), &bytesRead, portMAX_DELAY);
    int samples = bytesRead / 2;
    int32_t peak = 1;
    for (int i = 0; i < samples; i++) {
      int32_t v = audioBuf[i];
      if (v < 0) v = -v;
      if (v > peak) peak = v;
    }
    f.write((uint8_t *)audioBuf, bytesRead);
    totalSamples += samples;
    while (xQueueReceive(inputQueue, &e, 0) == pdTRUE) {
      if (e.ev == EV_BACK || e.ev == EV_SELECT || e.ev == EV_LEFT || e.ev == EV_LONGSELECT) { stop = true; }
    }
    uint32_t secs = (millis() - recStart) / 1000;
    gfx->setTextSize(2);
    gfx->setCursor(8, 70);
    gfx->printf("%02u:%02u\n", (unsigned)(secs / 60), (unsigned)(secs % 60));
    gfx->setTextSize(1);
    gfx->setCursor(8, 100);
    gfx->printf("samples: %u  peak: %d\n", (unsigned)totalSamples, (int)peak);
    gfx->setCursor(8, 114);
    int bars = (peak * 40) / 32768;
    if (bars > 40) bars = 40;
    gfx->print("[");
    for (int i = 0; i < 40; i++) gfx->print(i < bars ? "#" : " ");
    gfx->println("]");
    gfx->setCursor(8, 128);
    gfx->println("Click/Esc = stop & save");
  }
  f.flush();
  uint32_t dataBytes = totalSamples * 2;
  f.seek(40);
  f.write((uint8_t *)&dataBytes, 4);
  uint32_t riffSize = 36 + dataBytes;
  f.seek(4);
  f.write((uint8_t *)&riffSize, 4);
  f.close();
  drawStatus("Saved to SD");
  delay(1200);
}

// Simple beep for the timer alarm; uses its own short-lived I2S session.
void spkBeep(int ms) {
  i2s_config_t i2s_config = {};
  i2s_config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  i2s_config.sample_rate = 16000;
  i2s_config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  i2s_config.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  i2s_config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  i2s_config.dma_buf_count = 4;
  i2s_config.dma_buf_len = 256;
  i2s_config.use_apll = false;
  i2s_pin_config_t pins = {};
  pins.bck_io_num = BOARD_I2S_BCK;
  pins.ws_io_num = BOARD_I2S_WS;
  pins.data_out_num = BOARD_I2S_DOUT;
  pins.data_in_num = I2S_PIN_NO_CHANGE;
  if (i2s_driver_install(SPK_I2S_PORT, &i2s_config, 0, NULL) != ESP_OK) return;
  i2s_set_pin(SPK_I2S_PORT, &pins);
  i2s_zero_dma_buffer(SPK_I2S_PORT);
  // 880 Hz square wave
  static int16_t beepBuf[1600];  // 100ms worth
  for (int i = 0; i < 1600; i++) {
    int16_t v = ((i * 880) / 16000) % 2 ? 6000 : -6000;
    beepBuf[i] = v;
  }
  uint32_t start = millis();
  size_t written;
  while ((int32_t)(millis() - start) < (int32_t)ms) {
    i2s_write(SPK_I2S_PORT, beepBuf, sizeof(beepBuf), &written, pdMS_TO_TICKS(200));
  }
  i2s_zero_dma_buffer(SPK_I2S_PORT);
  i2s_driver_uninstall(SPK_I2S_PORT);
}

// pick an audio file from any folder: shows /recordings + /radio first
static bool pickAudioFile(String &outPath) {
  const char *folders[] = {REC_DIR, "/radio/rec"};
  const char *labels[] = {"Recordings", "Radio recordings"};
  String names[2][MAX_FILES];
  int counts[2] = {0, 0};
  for (int d = 0; d < 2; d++) {
    File root = SD.open(folders[d]);
    if (root && root.isDirectory()) {
      File f = root.openNextFile();
      while (f && counts[d] < MAX_FILES) {
        String nm = f.name();
        if (!f.isDirectory() && (nm.endsWith(".wav") || nm.endsWith(".mp3"))) {
          String shortNm = nm;
          int slash = shortNm.lastIndexOf('/');
          if (slash >= 0) shortNm = shortNm.substring(slash + 1);
          names[d][counts[d]++] = String(folders[d]) + "/" + shortNm;
        }
        f.close();
        f = root.openNextFile();
      }
      root.close();
    }
  }
  int total = counts[0] + counts[1];
  if (total == 0) return false;
  // flat picker across both folders (section headers skipped; simple list)
  const int visible = 12;
  int sel = 0, scroll = 0;
  bool needsRedraw = true;
  while (true) {
    if (needsRedraw) {
      needsRedraw = false;
      drawTitle("Play");
      gfx->setTextSize(1);
      int y = 30;
      for (int d = 0; d < 2; d++) {
        if (counts[d] == 0) continue;
        gfx->setTextColor(TERM_DIM, BLACK);
        gfx->setCursor(6, y);
        gfx->println(labels[d]);
        y += 13;
        int startIdx = (d == 0) ? 0 : counts[0];
        for (int i = 0; i < counts[d]; i++) {
          int idx = startIdx + i;
          if (idx < scroll || idx >= scroll + visible) continue;
          int row = idx - scroll;
          int yy = 30 + 14 + row * 14;   // approximate; fine for <24 items
          if (idx == sel) {
            gfx->fillRect(0, yy - 2, SCREEN_W, 13, RGB565(0, 120, 255));
            gfx->setTextColor(BLACK, TERM_SEL_BG);
          } else gfx->setTextColor(WHITE, BLACK);
          gfx->setCursor(6, yy);
          gfx->println(baseName(names[d][i]));
        }
        y += counts[d] * 14 + 4;
      }
      gfx->setTextColor(WHITE, BLACK);
      gfx->setCursor(4, SCREEN_H - 10);
      gfx->println("Click = play  Long-click = back");
    }
    InputEvent e;
    if (!getInput(e, 50)) continue;
    if (e.ev == EV_UP && sel > 0) { sel--; needsRedraw = true; }
    else if (e.ev == EV_DOWN && sel < total - 1) { sel++; needsRedraw = true; }
    else if (e.ev == EV_SELECT || e.ev == EV_NEWLINE) {
      int d = (sel < counts[0]) ? 0 : 1;
      int i = (d == 0) ? sel : sel - counts[0];
      if (i >= 0 && i < counts[d]) {
        outPath = names[d][i];
        return true;
      }
    } else if (e.ev == EV_BACK || e.ev == EV_LEFT || e.ev == EV_LONGSELECT) return false;
    // keep selection visible
    if (sel < scroll) scroll = sel;
    if (sel >= scroll + visible) scroll = sel - visible + 1;
  }
}

static void playbackApp() {
  String path;
  if (!pickAudioFile(path)) return;
  if (!audioPlayerInit()) {
    drawTitle("Play");
    gfx->setTextSize(1);
    gfx->setTextColor(TERM_RED, BLACK);
    gfx->setCursor(8, 40);
    gfx->println("Audio init failed");
    delay(1000);
    return;
  }
  Audio *a = audioPlayerGet();
  a->setPinout(BOARD_I2S_BCK, BOARD_I2S_WS, BOARD_I2S_DOUT);
  int vol = audioPlayerDefaultVolume();
  a->setVolume(vol);
  if (!a->connecttoFS(SD, path.c_str())) {
    drawTitle("Play");
    gfx->setTextSize(1);
    gfx->setTextColor(TERM_RED, BLACK);
    gfx->setCursor(8, 40);
    gfx->println("Cannot open file");
    audioPlayerDeinit();
    delay(1000);
    return;
  }
  bool playing = true, paused = false;
  bool needsRedraw = true;
  uint32_t lastDraw = 0;
  while (playing) {
    a->loop();
    if (millis() - lastDraw > 250 || needsRedraw) {
      needsRedraw = false;
      lastDraw = millis();
      uint32_t dur = a->getAudioFileDuration();
      uint32_t cur = a->getAudioCurrentTime();
      if (cur >= dur) { playing = false; break; }
      gfx->fillScreen(BLACK);
      drawTitleBarIndicators();
      gfx->setTextSize(1);
      gfx->setTextColor(TERM_GREEN, BLACK);
      gfx->setCursor(4, 7);
      gfx->print("Play");
      gfx->drawFastHLine(0, 18, SCREEN_W, TERM_DIM);
      gfx->setTextSize(1);
      gfx->setTextColor(WHITE, BLACK);
      gfx->setCursor(8, 30);
      gfx->println(baseName(path));
      // time mm:ss / mm:ss
      char tbuf[24];
      snprintf(tbuf, sizeof(tbuf), "%02u:%02u / %02u:%02u",
               (unsigned)(cur / 60), (unsigned)(cur % 60),
               (unsigned)(dur / 60), (unsigned)(dur % 60));
      gfx->setTextColor(TERM_BRIGHT, BLACK);
      gfx->setCursor(8, 46);
      gfx->println(tbuf);
      // progress bar
      int barW = SCREEN_W - 32;
      int frac = dur ? (int)(cur * barW / dur) : 0;
      if (frac > barW) frac = barW;
      gfx->drawRect(14, 64, barW + 2, 12, TERM_DIM);
      gfx->fillRect(16, 66, frac, 8, TERM_GREEN);
      // volume bar
      gfx->setTextColor(TERM_DIM, BLACK);
      gfx->setCursor(8, 88);
      gfx->print("vol ");
      for (int i = 0; i < 21; i++)
        gfx->fillRect(40 + i * 6, 88, 4, 8, i < vol ? TERM_GREEN : TERM_DIM);
      gfx->setTextColor(paused ? TERM_ACCENT : TERM_GREEN, BLACK);
      gfx->setCursor(8, 106);
      gfx->println(paused ? "PAUSED" : "PLAYING");
      gfx->setTextColor(TERM_DIM, BLACK);
      gfx->setCursor(4, SCREEN_H - 10);
      gfx->println("l/r=vol u=pause Esc=stop");
    }
    InputEvent e;
    while (xQueueReceive(inputQueue, &e, 0) == pdTRUE) {
      if (e.ev == EV_BACK || e.ev == EV_LONGSELECT) playing = false;
      else if (e.ev == EV_LEFT && vol > 0) { vol--; a->setVolume(vol); needsRedraw = true; }
      else if (e.ev == EV_RIGHT && vol < 21) { vol++; a->setVolume(vol); needsRedraw = true; }
      else if (e.ev == EV_UP || e.ev == EV_SELECT || e.ev == EV_NEWLINE) {
        paused = !paused;
        a->pauseResume();
        needsRedraw = true;
      }
    }
  }
  audioPlayerDeinit();
}

// ---------- Icon launcher ----------

static void batteryScreen() {
  gfx->fillScreen(BLACK);
  gfx->setTextSize(2);
  gfx->setTextColor(TERM_GREEN, BLACK);
  gfx->setCursor(20, 30);
  gfx->print("Battery");
  gfx->setTextSize(2);
  gfx->setTextColor(WHITE, BLACK);
  gfx->setCursor(20, 70);
  gfx->printf("%d%%  %d mV", batteryPercent(), batteryMillivolts());
  gfx->setTextSize(1);
  gfx->setTextColor(RGB565(150, 150, 150), BLACK);
  gfx->setCursor(20, 110);
  gfx->print("ADC raw sample; percent is an estimate from");
  gfx->setCursor(20, 122);
  gfx->print("voltage, not a calibrated fuel gauge.");
  gfx->setCursor(4, SCREEN_H - 10);
  gfx->setTextColor(WHITE, BLACK);
  gfx->print("Any key = back");
  InputEvent w;
  while (!getInput(w, 50)) {}
}

// Vector icons in terminal green, drawn inside a 24x24 box at (x,y).
static void drawAppIcon(int idx, int x, int y) {
  uint16_t c = TERM_GREEN, d = TERM_DIM;
  switch (idx) {
    case 0:  // Notes: lined page
      gfx->drawRect(x + 5, y + 2, 14, 20, c);
      for (int i = 0; i < 5; i++) gfx->drawFastHLine(x + 8, y + 6 + i * 3, 8, d);
      break;
    case 1:  // Recorder: microphone
      gfx->fillRoundRect(x + 9, y + 1, 6, 11, 3, c);
      gfx->drawRect(x + 7, y + 5, 10, 8, d);
      gfx->drawFastVLine(x + 12, y + 13, 4, c);
      gfx->drawFastHLine(x + 8, y + 17, 9, c);
      break;
    case 2:  // Play recs: speaker + waves
      gfx->fillTriangle(x + 4, y + 9, x + 9, y + 5, x + 9, y + 14, c);
      gfx->fillRect(x + 4, y + 9, 5, 6, c);
      gfx->drawCircle(x + 12, y + 10, 5, d);
      gfx->drawCircle(x + 12, y + 10, 9, d);
      break;
    case 3:  // Map: folded map with route
      gfx->drawRect(x + 3, y + 4, 18, 16, c);
      gfx->drawFastVLine(x + 9, y + 4, 16, d);
      gfx->drawFastVLine(x + 15, y + 4, 16, d);
      gfx->drawLine(x + 4, y + 16, x + 8, y + 10, TERM_ACCENT);
      gfx->drawLine(x + 8, y + 10, x + 14, y + 14, TERM_ACCENT);
      gfx->drawLine(x + 14, y + 14, x + 20, y + 7, TERM_ACCENT);
      break;
    case 4:  // Clock
      gfx->drawCircle(x + 12, y + 12, 10, c);
      gfx->drawLine(x + 12, y + 12, x + 12, y + 5, c);
      gfx->drawLine(x + 12, y + 12, x + 18, y + 15, TERM_BRIGHT);
      break;
    case 5:  // Calendar
      gfx->drawRect(x + 3, y + 4, 18, 16, c);
      gfx->drawFastHLine(x + 3, y + 9, 18, c);
      gfx->fillRect(x + 6, y + 12, 4, 4, d);
      gfx->drawFastVLine(x + 6, y + 1, 4, c);
      gfx->drawFastVLine(x + 18, y + 1, 4, c);
      break;
    case 6:  // WiFi: nested arcs + dot
      gfx->drawArc(x + 12, y + 15, 8, 9, 240, 300, c);
      gfx->drawArc(x + 12, y + 15, 5, 6, 240, 300, c);
      gfx->drawArc(x + 12, y + 15, 2, 3, 240, 300, c);
      gfx->fillCircle(x + 12, y + 15, 1, TERM_BRIGHT);
      break;
    case 7:  // Battery
      gfx->drawRect(x + 3, y + 7, 16, 10, c);
      gfx->fillRect(x + 19, y + 10, 2, 4, c);
      gfx->fillRect(x + 5, y + 9, 10, 6, TERM_BRIGHT);
      break;
    case 8:  // Calculator
      gfx->drawRect(x + 4, y + 2, 16, 20, c);
      gfx->fillRect(x + 6, y + 4, 12, 5, d);
      for (int r = 0; r < 3; r++)
        for (int q = 0; q < 3; q++)
          gfx->fillRect(x + 6 + q * 4, y + 11 + r * 4, 3, 3, (r + q) % 2 ? d : c);
      break;
    case 9:  // Search: magnifier
      gfx->drawCircle(x + 10, y + 10, 7, c);
      gfx->drawLine(x + 15, y + 15, x + 21, y + 21, c);
      gfx->drawLine(x + 16, y + 14, x + 22, y + 20, c);
      break;
    case 10:  // Contacts: person card
      gfx->drawRect(x + 3, y + 3, 18, 18, c);
      gfx->fillCircle(x + 10, y + 9, 3, TERM_BRIGHT);
      gfx->fillCircle(x + 10, y + 18, 6, TERM_BRIGHT);
      gfx->fillRect(x + 14, y + 12, 4, 2, d);
      gfx->fillRect(x + 14, y + 16, 4, 2, d);
      break;
    case 11:  // Convert: two-way arrows
      gfx->drawFastHLine(x + 5, y + 8, 14, c);
      gfx->fillTriangle(x + 3, y + 8, x + 8, y + 5, x + 8, y + 11, c);
      gfx->drawFastHLine(x + 5, y + 16, 14, c);
      gfx->fillTriangle(x + 21, y + 16, x + 16, y + 13, x + 16, y + 19, c);
      break;
    case 12:  // Files: folder
      gfx->drawFastHLine(x + 3, y + 5, 7, c);
      gfx->drawFastVLine(x + 3, y + 5, 2, c);
      gfx->drawRect(x + 3, y + 7, 18, 13, c);
      gfx->drawFastHLine(x + 6, y + 12, 12, d);
      gfx->drawFastHLine(x + 6, y + 16, 8, d);
      break;
    case 13:  // Ebook: open book
      gfx->drawLine(x + 12, y + 5, x + 12, y + 20, c);
      gfx->drawRect(x + 3, y + 4, 9, 14, c);
      gfx->drawRect(x + 12, y + 4, 9, 14, c);
      gfx->fillTriangle(x + 12, y + 20, x + 3, y + 18, x + 12, y + 23, c);
      gfx->fillTriangle(x + 12, y + 20, x + 21, y + 18, x + 12, y + 23, c);
      break;
    case 14:  // Images: mountain frame
      gfx->drawRect(x + 3, y + 4, 18, 16, c);
      gfx->fillCircle(x + 9, y + 9, 2, TERM_ACCENT);
      gfx->fillTriangle(x + 5, y + 18, x + 11, y + 10, x + 16, y + 18, d);
      gfx->fillTriangle(x + 10, y + 18, x + 15, y + 13, x + 19, y + 18, TERM_BRIGHT);
      break;
    case 15:  // Wardrive: antenna + waves
      gfx->drawFastVLine(x + 12, y + 8, 12, c);
      gfx->fillTriangle(x + 9, y + 20, x + 15, y + 20, x + 12, y + 23, c);
      for (int r = 4; r <= 7; r += 3)
        for (int a = 0; a < 5; a++) {
          float ang = (90 - a * 45) * 3.14159f / 180.0f;
          int px = x + 12 + (int)(r * cosf(ang));
          int py = y + 8 - (int)(r * sinf(ang));
          gfx->drawPixel(px, py, d);
        }
      break;
    case 16:  // Chess: pawn
      gfx->fillCircle(x + 12, y + 6, 4, c);
      gfx->fillTriangle(x + 9, y + 20, x + 15, y + 20, x + 13, y + 10, c);
      gfx->fillTriangle(x + 9, y + 20, x + 15, y + 20, x + 11, y + 10, c);
      gfx->fillRect(x + 7, y + 19, 10, 3, c);
      break;
    case 17:  // Go: board + stones
      gfx->drawRect(x + 3, y + 3, 18, 18, c);
      for (int i = 1; i <= 2; i++) {
        gfx->drawFastHLine(x + 3, y + 3 + i * 6, 18, d);
        gfx->drawFastVLine(x + 3 + i * 6, y + 3, 18, d);
      }
      gfx->fillCircle(x + 9, y + 9, 2, TERM_BRIGHT);
      gfx->drawCircle(x + 15, y + 15, 2, TERM_BRIGHT);
      break;
    case 18:  // Solitaire: three cards
      gfx->fillRect(x + 3, y + 6, 8, 11, d);
      gfx->fillRect(x + 8, y + 4, 8, 11, c);
      gfx->fillRect(x + 13, y + 8, 8, 11, TERM_BRIGHT);
      break;
    case 19:  // Checkers: board + men
      for (int r = 0; r < 4; r++)
        for (int q = 0; q < 4; q++)
          if ((r + q) % 2) gfx->fillRect(x + 3 + q * 5, y + 3 + r * 5, 5, 5, d);
      gfx->drawRect(x + 3, y + 3, 20, 20, c);
      gfx->drawCircle(x + 13, y + 8, 2, TERM_BRIGHT);
      gfx->fillCircle(x + 8, y + 18, 2, TERM_BRIGHT);
      break;
    case 20:  // Email: envelope
      gfx->drawRect(x + 3, y + 6, 18, 12, c);
      gfx->drawLine(x + 3, y + 6, x + 12, y + 13, d);
      gfx->drawLine(x + 12, y + 13, x + 21, y + 6, d);
      break;
    case 21:  // Radio: tower + waves
      gfx->fillTriangle(x + 10, y + 20, x + 14, y + 20, x + 12, y + 8, c);
      gfx->fillCircle(x + 12, y + 6, 2, TERM_BRIGHT);
      gfx->drawArc(x + 12, y + 6, 6, 7, 230, 310, d);
      gfx->drawArc(x + 12, y + 6, 10, 11, 235, 305, d);
      break;
    default:
      gfx->drawRect(x + 6, y + 6, 12, 12, c);
      break;
  }
}

static const char *const launcherLabels[] = {
  "Notes", "Record", "Play", "Map", "Clock", "Cal", "WiFi", "Batt",
  "Calc", "Search", "Contcts", "Convrt",
  "Files", "Book", "Image", "Wardrv",
  "Chess", "Go", "Solit", "Chkrs",
  "Email", "Radio",
};
static void (*const launcherRun[])() = {
  notesApp, recorderApp, playbackApp, mapApp,
  clockApp, calendarApp, wifiApp, batteryScreen,
  calcApp, searchApp, contactsApp, convertApp,
  filesApp, ebookApp, imageApp, wardriveApp,
  chessApp, goApp, solitaireApp, checkersApp,
  emailApp, radioApp,
};
static const int LAUNCHER_N = 22;

// ---- Category home screen ----
// Each category is a list of launcher-app indexes above (plus hubs).
struct Category {
  const char *name;
  const int *apps;     // indexes into launcherLabels/Run, or -1 = hub entry
  int n;
};

static const int catProductivity[] = {0, 5, -2, 20};

static const int catTools[]         = {8, 9, 10, 11, 12};
static const int catMedia[]         = {13, 14, 1, 2, 21};
static const int catNetwork[]      = {6, 15, -4};
static const int catSystem[]        = {7, -1, -3};

static void runSettings() { settingsApp(); }
static void runTerminal() { terminalApp(); }

// Home screen: 7 entries. Sports (-5) is its own tile, not buried in Network.
// A category with apps != NULL opens a grid; SPECIAL entries run directly.
#define CAT_SPORTS -100
static const Category categories[] = {
  {"Work",     catProductivity, 4},
  {"Tools",    catTools,        5},
  {"Media",    catMedia,        5},
  {"Network",  catNetwork,      3},
  {"Games",    NULL,            0},   // gamesApp hub
  {"Sports",    (const int *)CAT_SPORTS, 0},  // sportsApp direct
  {"System",   catSystem,       3},
};
#define N_CATS (int)(sizeof(categories)/sizeof(categories[0]))

// Category icons (drawn with primitives, 24x24 at x,y)
static void drawCategoryIcon(int cat, int x, int y) {
  uint16_t c = TERM_GREEN, d = TERM_DIM;
  switch (cat) {
    case 0:  // Work: clipboard
      gfx->drawRect(x + 4, y + 3, 16, 18, c);
      gfx->fillRect(x + 8, y + 1, 8, 4, c);
      for (int i = 0; i < 3; i++) gfx->drawFastHLine(x + 7, y + 9 + i * 4, 10, d);
      break;
    case 1:  // Tools: wrench-ish
      gfx->drawCircle(x + 8, y + 8, 5, c);
      gfx->drawLine(x + 12, y + 12, x + 20, y + 20, c);
      gfx->drawLine(x + 13, y + 11, x + 21, y + 19, c);
      gfx->drawRect(x + 3, y + 15, 6, 6, d);
      break;
    case 2:  // Media: play button in screen
      gfx->drawRect(x + 2, y + 4, 20, 16, c);
      gfx->fillTriangle(x + 8, y + 8, x + 8, y + 16, x + 17, y + 12, TERM_BRIGHT);
      break;
    case 3:  // Network: globe
      gfx->drawCircle(x + 12, y + 12, 10, c);
      gfx->drawCircle(x + 12, y + 12, 5, d);
      gfx->drawFastVLine(x + 12, y + 2, 20, d);
      gfx->drawFastHLine(x + 2, y + 12, 20, d);
      break;
    case 4:  // Games: dice
      gfx->drawRect(x + 3, y + 3, 18, 18, c);
      gfx->fillCircle(x + 8, y + 8, 2, TERM_BRIGHT);
      gfx->fillCircle(x + 16, y + 16, 2, TERM_BRIGHT);
      gfx->fillCircle(x + 16, y + 8, 2, d);
      gfx->fillCircle(x + 8, y + 16, 2, d);
      break;
    case 5:  // Sports: scoreboard
      gfx->drawRect(x + 2, y + 3, 20, 18, c);
      gfx->drawFastHLine(x + 2, y + 8, 20, c);
      gfx->setTextSize(1);
      gfx->setTextColor(TERM_BRIGHT, BLACK);
      gfx->setCursor(x + 5, y + 10);
      gfx->print("88");
      gfx->setCursor(x + 13, y + 10);
      gfx->print("88");
      gfx->setTextColor(TERM_DIM, BLACK);
      gfx->setCursor(x + 5, y + 16);
      gfx->print("LIVE");
      break;
    case 6:  // System: gear
      gfx->drawCircle(x + 12, y + 12, 6, c);
      gfx->drawCircle(x + 12, y + 12, 9, d);
      for (int a = 0; a < 8; a++) {
        float ang = a * 3.14159f / 4.0f;
        gfx->drawLine(x + 12 + 10 * cosf(ang), y + 12 + 10 * sinf(ang),
                      x + 12 + 13 * cosf(ang), y + 12 + 13 * sinf(ang), d);
      }
      break;
    default:
      gfx->drawRect(x + 6, y + 6, 12, 12, c);
      break;
  }
}

#define LAUNCHER_COLS 4
#define ICON_BOX 24
#define CELL_W (SCREEN_W / LAUNCHER_COLS)
#define CELL_H ((SCREEN_H - 18 - MENU_TOP) / 3)

// Draw one cell of a grid page; iconIdx indexes launcher icon drawers for
// apps, or -1/-2/-3 for settings/terminal/games-hub special icons.
static void drawCategoryCellIcon(int idx, int x, int y) {
  if (idx >= 100) { drawCategoryIcon(idx - 100, x, y); return; }
  if (idx >= 0) { drawAppIcon(idx, x, y); return; }
  uint16_t c = TERM_GREEN, d = TERM_DIM;
  if (idx == -1) {  // Settings: sliders
    gfx->drawFastVLine(x + 6, y + 4, 16, c);
    gfx->drawFastVLine(x + 12, y + 4, 16, c);
    gfx->drawFastVLine(x + 18, y + 4, 16, c);
    gfx->fillCircle(x + 6, y + 9, 3, TERM_BRIGHT);
    gfx->fillCircle(x + 12, y + 14, 3, TERM_BRIGHT);
    gfx->fillCircle(x + 18, y + 19, 3, TERM_BRIGHT);
  } else if (idx == -2) {  // To-do hub: check list
    gfx->drawRect(x + 3, y + 3, 18, 18, c);
    gfx->drawLine(x + 6, y + 8, x + 9, y + 11, TERM_BRIGHT);
    gfx->drawLine(x + 9, y + 11, x + 13, y + 6, TERM_BRIGHT);
    gfx->drawLine(x + 6, y + 14, x + 9, y + 17, d);
    gfx->drawLine(x + 9, y + 17, x + 13, y + 12, d);
  } else if (idx == -4) {  // WiFi bands: bars
    for (int i = 0; i < 4; i++)
      gfx->drawFastVLine(x + 5 + i * 5, y + 18 - (4 + i * 4), 4 + i * 4,
                         i == 3 ? TERM_BRIGHT : c);
  } else if (idx == -5) {  // Sports: scoreboard board
    gfx->drawRect(x + 2, y + 4, 20, 16, c);
    gfx->drawFastHLine(x + 5, y + 9, 5, TERM_BRIGHT);
    gfx->drawFastHLine(x + 5, y + 13, 8, TERM_BRIGHT);
    gfx->drawFastHLine(x + 5, y + 17, 4, TERM_DIM);
  } else {  // Terminal: prompt box
    gfx->drawRect(x + 2, y + 4, 20, 16, c);
    gfx->setCursor(x + 5, y + 9);
    gfx->setTextColor(TERM_BRIGHT, BLACK);
    gfx->setTextSize(1);
    gfx->print(">");
    gfx->fillRect(x + 13, y + 16, 6, 2, TERM_BRIGHT);
  }
}

static void drawCategoryCell(int cx, int cy, const char *label, int iconIdx,
                             bool selected) {
  int x = cx * CELL_W, y = MENU_TOP + cy * CELL_H;
  uint16_t bg = selected ? TERM_SEL_BG : BLACK;
  gfx->fillRect(x, y, CELL_W, CELL_H, bg);
  drawCategoryCellIcon(iconIdx, x + (CELL_W - ICON_BOX) / 2,
                       y + (CELL_H - ICON_BOX - 10) / 2);
  gfx->setTextSize(1);
  gfx->setTextColor(selected ? BLACK : TERM_DIM, bg);
  int tw = strlen(label) * 6;
  gfx->setCursor(x + (CELL_W - tw) / 2, y + CELL_H - 12);
  gfx->print(label);
}

// Run one app from a category page by its icon idx (>=0 launcher idx,
// -1 settings, -2 to-do hub, -3 terminal, -4 wifi bands, -5 sports).
static void runCategoryApp(int idx) {
  if (idx >= 0) launcherRun[idx]();
  else if (idx == -1) settingsApp();
  else if (idx == -2) todoApp();
  else if (idx == -3) terminalApp();
  else if (idx == -4) wifiBandsApp();
  else if (idx == -5) sportsApp();
}

static const char *categoryAppLabel(int idx) {
  if (idx >= 0) return launcherLabels[idx];
  if (idx == -1) return "Setngs";
  if (idx == -2) return "To-do";
  if (idx == -3) return "Term";
  if (idx == -4) return "Bands";
  if (idx == -5) return "Sports";
  return "?";
}

// A generic grid page: sel in [0,n), apps[] gives icon indexes, back returns.
static void runGridPage(const char *title, const int *apps, int n) {
  int sel = 0;
  static uint32_t lastGen = 0;
  bool full = true;
  int lastSel = -1;
  while (true) {
    bool fullRepaint = (full || uiGen != lastGen);
    if (fullRepaint) {
      lastGen = uiGen;
      full = false;
      drawTitle(title);
      gfx->fillRect(0, MENU_TOP - 4, SCREEN_W, SCREEN_H - 18 - (MENU_TOP - 4), BLACK);
      for (int i = 0; i < n; i++) {
        drawCategoryCell(i % LAUNCHER_COLS, i / LAUNCHER_COLS,
                         categoryAppLabel(apps[i]), apps[i], i == sel);
      }
      lastSel = sel;
    } else if (sel != lastSel) {
      drawCategoryCell(lastSel % LAUNCHER_COLS, lastSel / LAUNCHER_COLS,
                       categoryAppLabel(apps[lastSel]), apps[lastSel], false);
      drawCategoryCell(sel % LAUNCHER_COLS, sel / LAUNCHER_COLS,
                       categoryAppLabel(apps[sel]), apps[sel], true);
      lastSel = sel;
    }
    InputEvent e;
    if (!getInput(e, 50)) continue;
    int cols = LAUNCHER_COLS;
    if (e.ev == EV_UP) sel = (sel + n - cols) % n;
    else if (e.ev == EV_DOWN) sel = (sel + cols) % n;
    else if (e.ev == EV_LEFT) sel = (sel + n - 1) % n;
    else if (e.ev == EV_RIGHT) sel = (sel + 1) % n;
    else if (e.ev == EV_SELECT || e.ev == EV_NEWLINE) {
      runCategoryApp(apps[sel]);
      uiScreenChanged();
      full = true;
    } else if (e.ev == EV_LONGSELECT || e.ev == EV_BACK) return;
  }
}

static void mainMenu() {
  int sel = 0;
  static uint32_t lastGen = 0;
  static int lastSel = -1;
  while (true) {
    // Feed GPS parser while idle so time syncs at boot (first fix of the day).
    gpsPoll();
    if (!pdaTimeSynced() && gps.location.isValid() &&
        gps.time.isValid() && gps.date.isValid()) {
      pdaApplyGpsTime(gps.date.year(), gps.date.month(), gps.date.day(),
                      gps.time.hour(), gps.time.minute(), gps.time.second());
      Serial.println("[gps] time synced at boot");
    }
    bool fullRepaint = (uiGen != lastGen || lastSel < 0);
    if (fullRepaint) {
      lastGen = uiGen;
      drawTitle("T-Deck Plus");
      gfx->fillRect(0, MENU_TOP - 4, SCREEN_W, SCREEN_H - 18 - (MENU_TOP - 4), BLACK);
      for (int i = 0; i < N_CATS; i++) {
        const Category &cat = categories[i];
        drawCategoryCell(i % LAUNCHER_COLS, i / LAUNCHER_COLS,
                         cat.name, /*dummy*/ 100 + i, i == sel);
      }
      lastSel = sel;
    } else if (sel != lastSel) {
      drawCategoryCell(lastSel % LAUNCHER_COLS, lastSel / LAUNCHER_COLS,
                       categories[lastSel].name, 100 + lastSel, false);
      drawCategoryCell(sel % LAUNCHER_COLS, sel / LAUNCHER_COLS,
                       categories[sel].name, 100 + sel, true);
      lastSel = sel;
    }
    InputEvent e;
    if (!getInput(e, 50)) continue;
    int n = N_CATS;
    if (e.ev == EV_UP) sel = (sel + n - LAUNCHER_COLS) % n;
    else if (e.ev == EV_DOWN) sel = (sel + LAUNCHER_COLS) % n;
    else if (e.ev == EV_LEFT) sel = (sel + n - 1) % n;
    else if (e.ev == EV_RIGHT) sel = (sel + 1) % n;
    else if (e.ev == EV_SELECT || e.ev == EV_NEWLINE) {
      if ((intptr_t)categories[sel].apps == CAT_SPORTS) sportsApp();
      else if (categories[sel].apps == NULL) gamesApp();
      else {
        runGridPage(categories[sel].name, categories[sel].apps, categories[sel].n);
      }
      uiScreenChanged();
      lastSel = -1;
    }
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(BOARD_POWERON, OUTPUT);
  digitalWrite(BOARD_POWERON, HIGH);
  delay(300);

  inputQueue = xQueueCreate(16, sizeof(InputEvent));

  Serial.println("[boot] SPI bus init...");
  uiInit();
  Serial.println("[boot] display init done");

  drawTitle("T-Deck Plus");
  gfx->setTextSize(1);
  gfx->setCursor(8, 40);
  gfx->println("Mounting SD card...");

  sdOk = sdInit();
  Serial.printf("[boot] SD: %s\n", sdOk ? "OK" : "FAIL");
  themeInit();   // load saved theme (needs SD mounted)
  settingsSleepMs();  // eager-load sleep timeout here too (same reason)
  if (sdOk) {
    Serial.printf("[boot] SD type: %s, size: %lu MB\n",
                  SD.cardType() == CARD_SDHC ? "SDHC" : SD.cardType() == CARD_SD ? "SDSC" : "?",
                  (unsigned long)(SD.cardSize() / (1024UL * 1024UL)));
  }

  xTaskCreatePinnedToCore(keyboardTask, "kb", 4096, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(trackballTask, "tb", 2048, NULL, 1, NULL, 0);

  drawStatus(sdOk ? "SD OK" : "No SD card");
  delay(600);

  gfx->fillRect(0, 34, SCREEN_W, 14, BLACK);
  gfx->setTextSize(1);
  gfx->setTextColor(TERM_BRIGHT, BLACK);
  gfx->setCursor(8, 40);
  gfx->println("Starting mic + GPS...");
  Serial.printf("[boot] mic init: %s\n", micSetup() ? "OK" : "FAIL");
  gpsSetup();
  Serial.println("[boot] GPS serial started (9600, RX=44 TX=43)");
  for (int i = 0; i < 30; i++) { gpsPoll(); delay(100); }
  Serial.printf("[boot] GPS: %lu NMEA bytes in first 3s (0 = check antenna/module)\n",
                (unsigned long)gpsCharsSeen);
  Serial.printf("[boot] battery: %d%% (%d mV)\n", batteryPercent(), batteryMillivolts());
  gfx->fillRect(0, 34, SCREEN_W, 14, BLACK);
  gfx->setCursor(8, 40);
  gfx->println("Connecting WiFi...");
  if (wifiAutoConnect()) {
    Serial.println("[boot] WiFi connected");
  } else {
    Serial.println("[boot] WiFi: no known network in range");
  }
  pdaNoteActivity();
  mainMenu();
}

void loop() {
  vTaskDelay(portMAX_DELAY);
}
