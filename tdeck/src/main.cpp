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
#include "es7210.h"
#include "utilities.h"

#define SCREEN_W 320
#define SCREEN_H 240
#define LINE_H 12

#define NOTE_DIR "/notes"
#define REC_DIR  "/recordings"

#define MIC_SAMPLE_RATE 16000
#define MIC_I2S_PORT I2S_NUM_1
#define SPK_I2S_PORT I2S_NUM_0

#define MAX_NOTE_SIZE 8192
#define MAX_FILES 32

Arduino_DataBus *bus = new Arduino_HWSPI(BOARD_TFT_DC, BOARD_TFT_CS);
Arduino_GFX *gfx = new Arduino_ST7789(bus, GFX_NOT_DEFINED /* RST */, 1 /* rotation */, false /* IPS */, 320, 240);

enum AppEvent { EV_NONE, EV_UP, EV_DOWN, EV_LEFT, EV_RIGHT, EV_SELECT, EV_LONGSELECT, EV_BACK, EV_SPACE, EV_CHAR, EV_DELETE };

struct InputEvent {
  AppEvent ev;
  char ch;
  unsigned long ts;
};

static QueueHandle_t inputQueue;
static bool sdOk = false;

// ---------- Input: keyboard (I2C @0x55) + trackball ----------
#define LILYGO_KB_SLAVE_ADDRESS 0x55

static bool kbAvailable = false;

static void keyboardTask(void *pv) {
  Wire.begin(BOARD_I2C_SDA, BOARD_I2C_SCL);
  delay(300);
  Wire.requestFrom(LILYGO_KB_SLAVE_ADDRESS, 1);
  kbAvailable = (Wire.read() != -1);
  if (!kbAvailable) return;
  while (true) {
    char keyValue = 0;
    Wire.requestFrom(LILYGO_KB_SLAVE_ADDRESS, 1);
    while (Wire.available() > 0) {
      keyValue = Wire.read();
      if (keyValue == 0x00) continue;
      InputEvent e = {};
      e.ts = millis();
      if (keyValue == '\n' || keyValue == '\r') {
        e.ev = EV_SELECT;
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

static void trackballTask(void *pv) {
  const uint8_t dir_pins[4] = {BOARD_TBOX_G02, BOARD_TBOX_G01, BOARD_TBOX_G04, BOARD_TBOX_G03};
  bool last_dir[4] = {false};
  pinMode(BOARD_BOOT_PIN, INPUT_PULLUP);
  for (int i = 0; i < 4; i++) pinMode(dir_pins[i], INPUT_PULLUP);
  bool lastBoot = true;
  unsigned long bootDownAt = 0;
  while (true) {
    for (int i = 0; i < 4; i++) {
      bool dir = digitalRead(dir_pins[i]);
      if (dir != last_dir[i]) {
        last_dir[i] = dir;
        InputEvent e = {};
        e.ts = millis();
        e.ev = (i == 0) ? EV_RIGHT : (i == 1) ? EV_UP : (i == 2) ? EV_LEFT : EV_DOWN;
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
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

static bool getInput(InputEvent &e, unsigned long waitMs) {
  return xQueueReceive(inputQueue, &e, pdMS_TO_TICKS(waitMs)) == pdTRUE;
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

static void drawTitle(const char *title) {
  gfx->fillScreen(BLACK);
  gfx->setTextSize(2);
  gfx->setTextColor(RGB565(0, 120, 255), BLACK);
  gfx->setCursor(4, 4);
  gfx->println(title);
  gfx->drawFastHLine(0, 22, SCREEN_W, RGB565(0, 120, 255));
  gfx->setTextColor(WHITE, BLACK);
  gfx->setTextSize(1);
}

static int itemY(int i, int n) {
  int h = (n > 4) ? 34 : (SCREEN_H - 44) / n;
  return 34 + i * h;
}

static int itemH(int n) {
  return (n > 4) ? 30 : (SCREEN_H - 44) / n - 4;
}

static void drawStatus(const char *msg) {
  gfx->setTextSize(1);
  gfx->setTextColor(RGB565(255, 255, 0), BLACK);
  gfx->setCursor(4, SCREEN_H - 10);
  gfx->print("                               ");
  gfx->setCursor(4, SCREEN_H - 10);
  gfx->print(msg);
  gfx->setTextColor(WHITE, BLACK);
}

static uint32_t uiGen = 0;

static void drawMenuList(const char *title, const char *const *items, int n, int sel, const char *status) {
  static int lastSel = -1;
  static const char *lastTitle = NULL;
  static const char *const *lastItems = NULL;
  static int lastN = 0;
  static uint32_t lastGen = 0;
  if (uiGen != lastGen || title != lastTitle || lastSel < 0 || items != lastItems || n != lastN) {
    lastGen = uiGen;
    drawTitle(title);
    lastTitle = title;
    lastItems = items;
    lastN = n;
    lastSel = -1;
    for (int i = 0; i < n; i++) {
      int y = itemY(i, n);
      if (i == sel) {
        gfx->fillRect(0, y - 4, SCREEN_W, itemH(n), RGB565(0, 120, 255));
        gfx->setTextColor(BLACK, RGB565(0, 120, 255));
      } else {
        gfx->setTextColor(WHITE, BLACK);
      }
      gfx->setTextSize(2);
      gfx->setCursor(8, y);
      gfx->println(items[i]);
    }
    if (status) {
      gfx->setTextSize(1);
      gfx->setTextColor(WHITE, BLACK);
      gfx->setCursor(4, SCREEN_H - 10);
      gfx->println(status);
    }
  } else if (sel != lastSel) {
    int yPrev = itemY(lastSel, n);
    int yNew = itemY(sel, n);
    gfx->fillRect(0, yPrev - 4, SCREEN_W, itemH(n), BLACK);
    gfx->setTextSize(2);
    gfx->setTextColor(WHITE, BLACK);
    gfx->setCursor(8, yPrev);
    gfx->println(items[lastSel]);
    gfx->fillRect(0, yNew - 4, SCREEN_W, itemH(n), RGB565(0, 120, 255));
    gfx->setTextColor(BLACK, RGB565(0, 120, 255));
    gfx->setCursor(8, yNew);
    gfx->println(items[sel]);
  }
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
        gfx->setTextColor(BLACK, RGB565(0, 120, 255));
      } else {
        gfx->setTextColor(RGB565(255, 255, 0), BLACK);
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
          gfx->setTextColor(BLACK, RGB565(0, 120, 255));
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
    else if (e.ev == EV_SELECT) {
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
      drawStatus("Enter=save  Backspace=del  Long-click=exit");
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
      case EV_BACK: return -1;
      case EV_LONGSELECT: return -1;
      default: break;
    }
  }
}

static void notesApp() {
  const char *items[] = {"New note", "Open note", "Delete note", "Back"};
  int sel = 0;
  while (true) {
    drawMenuList("Notes", items, 4, sel, NULL);
    InputEvent e;
    if (!getInput(e, 50)) continue;
    if (e.ev == EV_UP) sel = (sel + 3) % 4;
    else if (e.ev == EV_DOWN) sel = (sel + 1) % 4;
    else if (e.ev == EV_BACK || (e.ev == EV_LEFT) || e.ev == EV_LONGSELECT) return;
    else if (e.ev == EV_SELECT) {
      if (sel == 3) return;
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
  gfx->println("Click = start REC");
  gfx->setTextSize(1);
  gfx->setCursor(8, 70);
  gfx->println("Long-click = back");
  drawStatus("Ready");
  InputEvent e;
  while (getInput(e, portMAX_DELAY)) {
    if (e.ev == EV_SELECT) break;
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

static void playbackApp() {
  String path;
  if (pickFile("Play recording", REC_DIR, ".wav", path) < 0) return;

  File f = SD.open(path, FILE_READ);
  if (!f) return;
  WavHeader hdr;
  f.readBytes((char *)&hdr, sizeof(hdr));

  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = MIC_SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_RIGHT,
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
  pins.bck_io_num = BOARD_I2S_BCK;
  pins.ws_io_num = BOARD_I2S_WS;
  pins.data_out_num = BOARD_I2S_DOUT;
  pins.data_in_num = I2S_PIN_NO_CHANGE;
  i2s_driver_install(SPK_I2S_PORT, &i2s_config, 0, NULL);
  i2s_set_pin(SPK_I2S_PORT, &pins);
  i2s_zero_dma_buffer(SPK_I2S_PORT);

  drawTitle("Play");
  gfx->setTextSize(2);
  gfx->setCursor(8, 40);
  gfx->println("Playing...");
  drawStatus("Esc = stop");

  static int16_t playBuf[2048];
  size_t bytesWritten = 0;
  bool stop = false;
  while (f.available() > 0 && !stop) {
    int n = f.read((uint8_t *)playBuf, sizeof(playBuf));
    if (n <= 0) break;
    i2s_write(SPK_I2S_PORT, playBuf, n, &bytesWritten, portMAX_DELAY);
    InputEvent e;
    while (xQueueReceive(inputQueue, &e, 0) == pdTRUE) {
      if (e.ev == EV_BACK || e.ev == EV_SELECT) stop = true;
    }
  }
  i2s_zero_dma_buffer(SPK_I2S_PORT);
  i2s_driver_uninstall(SPK_I2S_PORT);
  f.close();
  drawStatus("Done");
  delay(1000);
}

// ---------- Main menu ----------
static void mainMenu() {
  const char *items[] = {"Notes", "Recorder", "Play recordings"};
  int sel = 0;
  while (true) {
    drawMenuList("T-Deck Plus", items, 3, sel, sdOk ? "SD OK" : "NO SD CARD!");
    InputEvent e;
    if (!getInput(e, 50)) continue;
    if (e.ev == EV_UP) sel = (sel + 2) % 3;
    else if (e.ev == EV_DOWN) sel = (sel + 1) % 3;
    else if (e.ev == EV_SELECT) {
      if (sel == 0) notesApp();
      else if (sel == 1) recorderApp();
      else playbackApp();
      uiScreenChanged();
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
  if (sdOk) {
    Serial.printf("[boot] SD type: %s, size: %lu MB\n",
                  SD.cardType() == CARD_SDHC ? "SDHC" : SD.cardType() == CARD_SD ? "SDSC" : "?",
                  (unsigned long)(SD.cardSize() / (1024UL * 1024UL)));
  }

  xTaskCreatePinnedToCore(keyboardTask, "kb", 4096, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(trackballTask, "tb", 2048, NULL, 1, NULL, 0);

  drawStatus(sdOk ? "SD OK" : "No SD card");
  delay(600);

  Serial.printf("[boot] mic init: %s\n", micSetup() ? "OK" : "FAIL");
  mainMenu();
}

void loop() {
  vTaskDelay(portMAX_DELAY);
}
