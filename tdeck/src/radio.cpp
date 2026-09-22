/**
 * Internet radio, ported from the Cardputer Advanced Radio app
 * (lordkeynes/cardputer_radio_project) to the T-Deck Plus.
 *
 * Station list: /radio/station_list.txt on SD, one per line:
 *   "Name, URL"
 * Falls back to a built-in list so the radio works without a card.
 *
 * Controls: u/d = station, click = tune/pause, l/r = volume,
 *           r = start/stop recording the stream to SD, Long = exit.
 * Audio: ESP32-audioI2S on the speaker I2S port (BCK 7 / WS 5 / DOUT 6).
 * Recordings: decoded mono 16-bit PCM written as .wav into /radio/rec/,
 *           playable from the Play app.
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

// ---- stream recording (tee of decoded PCM -> WAV on SD) ----
#define RADIO_REC_DIR "/radio/rec"
static File recFile;
static bool recActive = false;
static uint32_t recBytes = 0;
static uint32_t recSampleRate = 16000;
static uint32_t recChannels = 1;

static void radioRecWriteHeader(File &f, uint32_t dataLen, uint32_t rate, uint32_t ch) {
  uint32_t byteRate = rate * ch * 2;
  uint32_t riffLen = 36 + dataLen;
  uint8_t hdr[44];
  memcpy(hdr, "RIFF", 4);
  hdr[4] = riffLen; hdr[5] = riffLen >> 8; hdr[6] = riffLen >> 16; hdr[7] = riffLen >> 24;
  memcpy(hdr + 8, "WAVE", 4);
  memcpy(hdr + 12, "fmt ", 4);
  hdr[16] = 16; hdr[17] = 0; hdr[18] = 0; hdr[19] = 0;
  hdr[20] = 1; hdr[21] = 0;                      // PCM
  hdr[22] = (uint8_t)ch; hdr[23] = 0;
  hdr[24] = rate; hdr[25] = rate >> 8; hdr[26] = rate >> 16; hdr[27] = rate >> 24;
  hdr[28] = byteRate; hdr[29] = byteRate >> 8; hdr[30] = byteRate >> 16; hdr[31] = byteRate >> 24;
  hdr[32] = (uint8_t)(ch * 2); hdr[33] = 0;      // block align
  hdr[34] = 16; hdr[35] = 0;                     // bits per sample
  memcpy(hdr + 36, "data", 4);
  hdr[40] = dataLen; hdr[41] = dataLen >> 8; hdr[42] = dataLen >> 16; hdr[43] = dataLen >> 24;
  f.seek(0);
  f.write(hdr, 44);
}

static bool radioRecStart() {
  if (recActive) return true;
  if (sdOk && !SD.exists(RADIO_REC_DIR)) SD.mkdir(RADIO_REC_DIR);
  char name[48];
  uint32_t n = 1;
  do {
    snprintf(name, sizeof(name), RADIO_REC_DIR "/radio%lu.wav", (unsigned long)n);
    n++;
  } while (sdOk && SD.exists(name) && n < 1000);
  if (!sdOk) return false;
  recFile = SD.open(name, FILE_WRITE);
  if (!recFile) return false;
  // placeholder header; rewritten with real sizes on stop
  uint8_t z[44] = {0};
  recFile.write(z, 44);
  recBytes = 0;
  recActive = true;
  return true;
}

static void radioRecStop() {
  if (!recActive) return;
  recActive = false;
  if (recFile) {
    radioRecWriteHeader(recFile, recBytes, recSampleRate, recChannels);
    recFile.close();
  }
}

// ESP32-audioI2S hook: tee decoded PCM to the record file while playing.
void audio_process_i2s(int16_t *outBuff, uint16_t validSamples,
                       uint8_t bitsPerSample, uint8_t channels, bool *continueI2S) {
  *continueI2S = true;
  if (!recActive || !recFile) return;
  if (bitsPerSample != 16) return;
  recSampleRate = audio ? audio->getSampleRate() : recSampleRate;
  recChannels = channels > 2 ? 2 : channels;
  if (recChannels == 0) recChannels = 1;
  // keep up to ~10 MB per recording to protect the SD card
  if (recBytes + (uint32_t)validSamples * recChannels * 2 > 10 * 1024 * 1024) {
    radioRecStop();
    return;
  }
  size_t w = recFile.write((uint8_t *)outBuff, (size_t)validSamples * recChannels * 2);
  recBytes += w;
}

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
  if (recActive) radioRecStop();
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
  // the lib resets its gain on connect; re-apply our volume
  if (isPlaying) audio->setVolume(volume);
  return isPlaying;
}

// if the stream drops, try one silent re-tune of the same station
static void radioTick() {
  if (audio && isPlaying && !audio->isRunning()) {
    isPlaying = audio->connecttohost(stations[curStation].url.c_str());
    if (isPlaying) audio->setVolume(volume);
  }
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
    gfx->print("u/d=stn click=tune l/r=vol r=rec Long=off");
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
  int maxV = audio ? (int)audio->maxVolume() : 21;
  if (maxV < 1) maxV = 1;
  int step = maxV > 24 ? 4 : 6;
  for (int i = 0; i < maxV; i++)
    gfx->fillRect(40 + i * step, 104, 4, 8, i < volume ? TERM_GREEN : TERM_DIM);
  // REC indicator + elapsed
  if (recActive) {
    gfx->setTextColor(TERM_RED, BLACK);
    gfx->setCursor(10, 120);
    gfx->print("REC ");
    uint32_t sec = recBytes / (recSampleRate * recChannels * 2);
    gfx->printf("%02u:%02u", (unsigned)(sec / 60), (unsigned)(sec % 60));
  }
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
    radioTick();

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
    } else if (e.ev == PDA_EV_RIGHT &&
               (audio ? volume < (int)audio->maxVolume() : volume < 21)) {
      volume++;
      if (audio) audio->setVolume(volume);
      full = true;
    } else if (e.ev == PDA_EV_CHAR && (e.ch == 'r' || e.ch == 'R')) {
      if (recActive) radioRecStop();
      else if (isPlaying) radioRecStart();
      full = true;
    } else if (e.ev == PDA_EV_LONGSELECT || e.ev == PDA_EV_BACK) {
      radioRecStop();
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
