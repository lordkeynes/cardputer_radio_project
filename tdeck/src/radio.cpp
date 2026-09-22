/**
 * Internet radio, ported from the Cardputer Advanced Radio app
 * (lordkeynes/cardputer_radio_project) to the T-Deck Plus.
 *
 * Station list: /radio/station_list.txt on SD, one per line:
 *   "Name, URL"
 * Falls back to a built-in list so the radio works without a card.
 *
 * Controls: u/d = station, click = tune/pause, l/r = volume, Long = exit.
 * Audio: ESP32-audioI2S on the speaker I2S port (BCK 7 / WS 5 / DOUT 6).
 */
#include <Arduino.h>
#include <WiFi.h>
#include <SD.h>
#include <vector>
#include <Audio.h>
#include <Arduino_GFX_Library.h>
#include "utilities.h"
#include "pda.h"
#include "theme.h"
#include "radio.h"

extern Arduino_GFX *gfx;
extern bool sdOk;
bool wifiConnected();

#define RADIO_LIST_FILE "/radio/station_list.txt"
#define RADIO_MAX_STATIONS 24

struct Station {
  String name;
  String url;
};

static std::vector<Station> stations;
static int  curStation = 0;
static bool isPlaying = false;
static int  volume = 12;          // 0..21
static char streamTitle[96] = {0};
static Audio *audio = NULL;
static bool radioInited = false;

static const Station defaultStations[] = {
  {"WYSO 91.3 NPR",             "https://playerservices.streamtheworld.com/api/livestream-redirect/WYSOFM.mp3"},
  {"NPR News",                  "https://npr-ice.streamguys1.com/live.mp3"},
  {"KEXP 90.3 Seattle",         "https://kexp-mp3-128.streamguys1.com/kexp128.mp3"},
  {"WFMU 91.1 Freeform",        "http://stream0.wfmu.org/freeform-128k"},
  {"WWOZ 90.7 New Orleans",     "https://wwoz-sc.streamguys1.com/wwoz-hi.mp3"},
  {"KCRW 89.9 Santa Monica",    "https://streams.kcrw.com/kcrw_mp3"},
  {"KALW 91.7 San Francisco",   "https://kalw-live.streamguys1.com/kalw.aac"},
  {"WFPK 91.9 Louisville",      "https://lpm.streamguys1.com/wfpk-popup.aac"},
  {"WTJU 91.1 Charlottesville", "https://streams.wtju.net/wtju-live"},
};
#define N_DEFAULT (int)(sizeof(defaultStations)/sizeof(defaultStations[0]))

static void radioLoadStations() {
  stations.clear();
  if (sdOk && SD.exists(RADIO_LIST_FILE)) {
    File f = SD.open(RADIO_LIST_FILE, FILE_READ);
    if (f) {
      while (f.available() && (int)stations.size() < RADIO_MAX_STATIONS) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() == 0 || line.startsWith("#")) continue;
        int comma = line.indexOf(',');
        if (comma < 0) continue;
        Station s;
        s.name = line.substring(0, comma);
        s.url = line.substring(comma + 1);
        s.name.trim();
        s.url.trim();
        if (s.name.length() && s.url.length()) stations.push_back(s);
      }
      f.close();
    }
  }
  if (stations.empty())
    for (int i = 0; i < N_DEFAULT; i++) stations.push_back(defaultStations[i]);
}

static bool radioAudioInit() {
  if (radioInited) return true;
  audio = new Audio(false, 3, I2S_NUM_0);
  if (!audio) return false;
  audio->setPinout(BOARD_I2S_BCK, BOARD_I2S_WS, BOARD_I2S_DOUT);
  audio->setVolume(volume);
  radioInited = true;
  return true;
}

static void radioAudioDeinit() {
  if (audio) {
    audio->stopSong();
    delete audio;
    audio = NULL;
  }
  WiFi.setSleep(true);    // back to power saving when not streaming
  radioInited = false;
  isPlaying = false;
}

static void radioStop() {
  if (audio) audio->stopSong();
  isPlaying = false;
  streamTitle[0] = 0;
}

static bool radioTune(int idx) {
  if (!radioAudioInit()) return false;
  radioStop();
  curStation = idx;
  WiFi.setSleep(false);   // keep radio responsive; modem sleep drops streams
  isPlaying = audio->connecttohost(stations[idx].url.c_str());
  return isPlaying;
}

static void radioDraw(bool full) {
  if (full) {
    gfx->fillScreen(BLACK);
    gfx->setTextSize(1);
    gfx->setTextColor(TERM_GREEN, BLACK);
    gfx->setCursor(4, 7);
    gfx->print("Radio");
    gfx->drawFastHLine(0, 18, SCREEN_W, TERM_DIM);
    gfx->setTextColor(TERM_DIM, BLACK);
    gfx->setCursor(4, SCREEN_H - 12);
    gfx->print("u/d=stn click=tune l/r=vol Long=off");
    gfx->drawRect(6, 34, SCREEN_W - 12, 60, TERM_DIM);
  }
  // Station name
  gfx->setTextSize(2);
  gfx->setTextColor(isPlaying ? TERM_BRIGHT : TERM_DIM, BLACK);
  gfx->fillRect(10, 40, SCREEN_W - 20, 20, BLACK);
  gfx->setCursor(10, 40);
  String nm = stations[curStation].name;
  if (nm.length() > 18) nm = nm.substring(0, 17) + ".";
  gfx->print(nm);
  // Status line
  gfx->setTextSize(1);
  gfx->fillRect(10, 66, SCREEN_W - 20, 12, BLACK);
  gfx->setTextColor(isPlaying ? TERM_GREEN : TERM_ACCENT, BLACK);
  gfx->setCursor(10, 66);
  if (isPlaying) {
    gfx->print("PLAY ");
    gfx->print(streamTitle[0] ? streamTitle : "stream");
  } else gfx->print("stopped");
  // Volume bar
  gfx->setTextColor(TERM_DIM, BLACK);
  gfx->setCursor(10, 104);
  gfx->print("vol ");
  for (int i = 0; i < 21; i++)
    gfx->fillRect(40 + i * 6, 104, 4, 8, i < volume ? TERM_GREEN : TERM_DIM);
}

void radioApp() {
  radioLoadStations();
  int n = stations.size();
  if (n == 0) return;
  if (curStation >= n) curStation = 0;

  bool full = true;
  uint32_t lastFrame = 0;
  while (true) {
    if (millis() - lastFrame > 500) {
      radioDraw(full);
      full = false;
      lastFrame = millis();
    }
    if (audio) audio->loop();

    InputEventP e;
    if (!pdaGetInput(e, 20)) continue;
    if (e.ev == PDA_EV_UP) {
      radioTune((curStation + n - 1) % n);
      full = true;
    } else if (e.ev == PDA_EV_DOWN) {
      radioTune((curStation + 1) % n);
      full = true;
    } else if (e.ev == PDA_EV_SELECT || e.ev == PDA_EV_NEWLINE) {
      if (isPlaying) radioStop();
      else radioTune(curStation);
      full = true;
    } else if (e.ev == PDA_EV_LEFT && volume > 0) {
      volume--;
      if (audio) audio->setVolume(volume);
      full = true;
    } else if (e.ev == PDA_EV_RIGHT && volume < 21) {
      volume++;
      if (audio) audio->setVolume(volume);
      full = true;
    } else if (e.ev == PDA_EV_LONGSELECT || e.ev == PDA_EV_BACK) {
      radioAudioDeinit();
      return;
    }
  }
}

// ESP32-audioI2S callbacks: capture ICY metadata as the stream title.
void audio_showstreamtitle(const char *info) {
  strncpy(streamTitle, info ? info : "", sizeof(streamTitle) - 1);
  streamTitle[sizeof(streamTitle) - 1] = 0;
}
void audio_eof_mp3(const char *info) {
  (void)info;
  isPlaying = false;
  streamTitle[0] = 0;
}
