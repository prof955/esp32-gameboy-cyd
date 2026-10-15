/*
 * ESPboy GameBoy / Peanut-GB port for the standard CYD ESP32-2432S028R.
 *
 * The ESP8266-only ESPboy display, button expander, EEPROM, mmu_iram, nbSPI,
 * sigma-delta and timer APIs have been replaced with CYD display/touch/SD,
 * ESP32 DAC audio, and a two-page SD ROM cache.
 */
#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <Preferences.h>
#include <Arduino_GFX_Library.h>
#include <XPT2046_Touchscreen.h>
#include <driver/dac.h>
#include <esp_system.h>
#include <string.h>
#include <stdlib.h>

#include "sound.h"
#define ENABLE_SOUND 1
#define PEANUT_GB_12_COLOUR 0
#include "peanut_gb.h"

// -----------------------------------------------------------------------------
// CYD ESP32-2432S028R pin map
// Pin assignment follows the proven ESP32-2432S028R sketch: ILI9341 on HSPI;
// the default VSPI bus is re-mapped between the SD socket and XPT2046 touch.
// -----------------------------------------------------------------------------
static constexpr uint8_t TFT_BACKLIGHT_PIN = 21;
static constexpr uint8_t SPEAKER_DAC_PIN = 26;  // ESP32 DAC2; CYD P4 speaker amp input
static constexpr uint8_t SPEAKER_ENABLE_PIN = 4;  // This board revision: LOW enables the amp
static constexpr int8_t CYD_TFT_DC = 2;
static constexpr int8_t CYD_TFT_CS = 15;
static constexpr int8_t CYD_TFT_SCK = 14;
static constexpr int8_t CYD_TFT_MOSI = 13;
static constexpr int8_t CYD_TFT_MISO = 12;
static constexpr uint32_t TFT_SPI_HZ = 40000000;

static constexpr uint8_t SD_CS_PIN   = 5;
static constexpr uint8_t SD_SCK_PIN  = 18;
static constexpr uint8_t SD_MISO_PIN = 19;
static constexpr uint8_t SD_MOSI_PIN = 23;

static constexpr uint8_t TOUCH_CS_PIN   = 33;
static constexpr uint8_t TOUCH_IRQ_PIN  = 36;
static constexpr uint8_t TOUCH_SCK_PIN  = 25;
static constexpr uint8_t TOUCH_MISO_PIN = 39;
static constexpr uint8_t TOUCH_MOSI_PIN = 32;
static constexpr uint8_t TOUCH_ROTATION = 0;  // portrait UI

// Calibrated raw ranges from the known-good landscape CYD sketch. The mapping
// below rotates those axes into this portrait emulator layout.
static constexpr int TOUCH_LAND_X_MIN = 560;
static constexpr int TOUCH_LAND_X_MAX = 3720;
static constexpr int TOUCH_LAND_Y_MIN = 590;
static constexpr int TOUCH_LAND_Y_MAX = 3580;

static constexpr int SCREEN_W = 240;
static constexpr int SCREEN_H = 320;
static constexpr int GB_W = 160;
static constexpr int GB_H = 144;
static constexpr int LCD_W = 240;  // GB image scaled 3:2, no crop
static constexpr int LCD_H = 216;
static constexpr int LCD_TOP = 0;
static constexpr int STATUS_Y = LCD_TOP + LCD_H;
static constexpr int CONTROLS_Y = 240;

static constexpr char ROM_DIR[] = "/ROMS/GB";
static constexpr char SAVE_DIR[] = "/SAVES";
static constexpr uint32_t SD_SPI_HZ = 20000000;
static constexpr uint32_t ROM_PAGE_SIZE = 16U * 1024U;
static constexpr uint8_t ROM_CACHE_PAGES = 2;
static constexpr uint8_t MAX_ROMS = 32;
static constexpr uint8_t VISIBLE_ROMS = 6;
static constexpr uint32_t SAVE_DELAY_MS = 2000;

// RGB565 four-shade palettes. Index 0 is the lightest Game Boy shade.
static const uint16_t DMG_PALETTES[][4] = {
  {0xDFFB, 0xA6D5, 0x4D8D, 0x1928},  // classic green LCD
  {0xFFFF, 0xBDF7, 0x7BEF, 0x0000},  // grayscale
  {0x6ACE, 0x279D, 0xE46B, 0x613A},  // warm nostalgia
  {0x7FFF, 0x7EAC, 0x40C0, 0x0000},  // soft gray/blue
  {0x16F6, 0xC9D3, 0xC091, 0x8041},  // red LCD
  {0x49CD, 0x099B, 0x0549, 0x4320},  // gold LCD
};
static constexpr uint8_t PALETTE_COUNT = sizeof(DMG_PALETTES) / sizeof(DMG_PALETTES[0]);

// Peanut-GB's direct joypad bits are active-low. Keep the UI's latched keys
// active-high, then invert them just before running the emulated frame.
static constexpr uint8_t KEY_A      = JOYPAD_A;
static constexpr uint8_t KEY_B      = JOYPAD_B;
static constexpr uint8_t KEY_SELECT = JOYPAD_SELECT;
static constexpr uint8_t KEY_START  = JOYPAD_START;
static constexpr uint8_t KEY_RIGHT  = JOYPAD_RIGHT;
static constexpr uint8_t KEY_LEFT   = JOYPAD_LEFT;
static constexpr uint8_t KEY_UP     = JOYPAD_UP;
static constexpr uint8_t KEY_DOWN   = JOYPAD_DOWN;

// Arduino_GFX owns the TFT's dedicated HSPI bus; touch/SD use VSPI.

#ifndef TFT_BLACK
#define TFT_BLACK 0x0000
#endif
#ifndef TFT_WHITE
#define TFT_WHITE 0xFFFF
#endif
#ifndef TFT_RED
#define TFT_RED 0xF800
#endif
#ifndef TFT_YELLOW
#define TFT_YELLOW 0xFFE0
#endif
#ifndef TFT_CYAN
#define TFT_CYAN 0x07FF
#endif
#ifndef TFT_ORANGE
#define TFT_ORANGE 0xFDA0
#endif
#ifndef TFT_LIGHTGREY
#define TFT_LIGHTGREY 0xC618
#endif

Arduino_DataBus *displayBus = new Arduino_ESP32SPI(
  CYD_TFT_DC, CYD_TFT_CS, CYD_TFT_SCK, CYD_TFT_MOSI, CYD_TFT_MISO, HSPI);
Arduino_GFX *displayGfx = new Arduino_ILI9341(displayBus, GFX_NOT_DEFINED, 0, false);

class CydDisplayCompat {
public:
  void init() { displayGfx->begin(TFT_SPI_HZ); }
  void setRotation(uint8_t rotation) { displayGfx->setRotation(rotation); }
  void setSwapBytes(bool) {}  // Arduino_GFX accepts native RGB565 words.
  void fillScreen(uint16_t color) { displayGfx->fillScreen(color); }
  void fillRect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color) {
    displayGfx->fillRect(x, y, w, h, color);
  }
  void drawFastHLine(int32_t x, int32_t y, int32_t w, uint16_t color) {
    displayGfx->drawFastHLine(x, y, w, color);
  }
  void fillCircle(int32_t x, int32_t y, int32_t r, uint16_t color) {
    displayGfx->fillCircle(x, y, r, color);
  }
  void drawCircle(int32_t x, int32_t y, int32_t r, uint16_t color) {
    displayGfx->drawCircle(x, y, r, color);
  }
  void fillRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint16_t color) {
    displayGfx->fillRoundRect(x, y, w, h, r, color);
  }
  void drawRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint16_t color) {
    displayGfx->drawRoundRect(x, y, w, h, r, color);
  }
  void setTextColor(uint16_t foreground, uint16_t background) {
    textForeground = foreground;
    textBackground = background;
    displayGfx->setTextColor(foreground, background);
  }
  void drawString(const char *text, int32_t x, int32_t y, uint8_t font = 1) {
    drawStringAt(text, x, y, font);
  }
  void drawCentreString(const char *text, int32_t centerX, int32_t y, uint8_t font = 1) {
    const uint8_t scale = (font >= 2) ? 2 : 1;
    const int32_t estimatedWidth = static_cast<int32_t>(strlen(text)) * 6 * scale;
    drawStringAt(text, centerX - estimatedWidth / 2, y, font);
  }
  void pushImage(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t *pixels) {
    displayGfx->draw16bitRGBBitmap(x, y, pixels, w, h);
  }

private:
  uint16_t textForeground = TFT_WHITE;
  uint16_t textBackground = TFT_BLACK;
  void drawStringAt(const char *text, int32_t x, int32_t y, uint8_t font) {
    const uint8_t scale = (font >= 2) ? 2 : 1;
    displayGfx->setTextSize(scale);
    displayGfx->setTextColor(textForeground, textBackground);
    displayGfx->setCursor(x, y);
    displayGfx->print(text);
  }
};

CydDisplayCompat tft;
XPT2046_Touchscreen touchscreen(TOUCH_CS_PIN, TOUCH_IRQ_PIN);
Preferences preferences;
File romFile;

struct gb_s gb;

// The emulator writes a compact 160x144 shade frame. Two buffers let the
// display worker send one image while the loop prepares the next one.
static uint8_t *frameBuffers[2] = {nullptr, nullptr};
static uint16_t scaledLine[LCD_W * 2];
static uint8_t scaleX[LCD_W];

enum FrameBufferState : uint8_t {
  BUFFER_FREE,
  BUFFER_RENDERING,
  BUFFER_READY,
  BUFFER_PRESENTING
};
static volatile FrameBufferState frameBufferStates[2] = {BUFFER_FREE, BUFFER_FREE};
static portMUX_TYPE frameBufferMux = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t readyFrames = nullptr;
static int renderBufferIndex = -1;  // owned by the Arduino loop task
static TaskHandle_t displayTaskHandle = nullptr;
static uint32_t nextGameFrameUs = 0;
static constexpr uint32_t GAME_FRAME_PERIOD_US = 16743;  // 59.73 Hz
static volatile uint32_t emulatedFrameCount = 0;
static volatile uint32_t presentedFrameCount = 0;
static volatile uint32_t droppedFrameCount = 0;
static volatile uint32_t nonSilentAudioSamples = 0;

struct RomCachePage {
  uint32_t pageNumber;
  uint32_t lastUse;
  bool valid;
  uint8_t bytes[ROM_PAGE_SIZE];
};
static RomCachePage romCache[ROM_CACHE_PAGES];
static uint32_t cacheClock = 0;

static char romPaths[MAX_ROMS][96];
static char savePath[96];
static char gameTitle[18] = "";
static char browserMessage[72] = "Select a ROM from the SD card.";
static uint8_t romCount = 0;
static uint8_t selectedRom = 0;
static uint8_t pageStart = 0;
static uint8_t paletteIndex = 0;
static uint8_t virtualKeys = 0;
static uint8_t lastGbError = 0;
static uint8_t auxBusMode = 0;  // 0=not started, 1=SD wiring, 2=touch wiring
static uint32_t romSize = 0;
static size_t cartRamSize = 0;
static uint8_t *cartRam = nullptr;
static uint32_t cartWriteAt = 0;
static bool cartRamDirty = false;
static bool sdMounted = false;
static bool touchWasDown = false;
static volatile bool soundEnabled = true;
static volatile bool gameRunning = false;

static hw_timer_t *audioTimer = nullptr;

enum AppMode : uint8_t {
  MODE_BROWSER,
  MODE_GAME
};
static volatile AppMode appMode = MODE_BROWSER;

// Coherent, small UI snapshots keep all ILI9341 drawing on the display task.
// The main loop publishes a new snapshot after touch/menu state changes.
struct UiSnapshot {
  uint32_t generation;
  AppMode mode;
  bool sdMounted;
  bool soundEnabled;
  uint8_t paletteIndex;
  uint8_t virtualKeys;
  uint8_t romCount;
  uint8_t selectedRom;
  uint8_t pageStart;
  char gameTitle[18];
  char browserMessage[72];
  char visibleRomNames[VISIBLE_ROMS][34];
};
static UiSnapshot pendingUi = {};
static UiSnapshot displayUi = {};
static portMUX_TYPE uiMux = portMUX_INITIALIZER_UNLOCKED;

// -----------------------------------------------------------------------------
// Shared VSPI bus routing
// -----------------------------------------------------------------------------
static void setAuxBus(uint8_t targetMode) {
  if (auxBusMode == targetMode) return;

  if (auxBusMode != 0) {
    SPI.end();
  }

  if (targetMode == 1) {
    SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
  } else if (targetMode == 2) {
    SPI.begin(TOUCH_SCK_PIN, TOUCH_MISO_PIN, TOUCH_MOSI_PIN, TOUCH_CS_PIN);
  }
  auxBusMode = targetMode;
}

static void setSdBus()    { setAuxBus(1); }
static void setTouchBus() { setAuxBus(2); }

// -----------------------------------------------------------------------------
// SD card ROM list
// -----------------------------------------------------------------------------
static const char *baseNameOf(const char *path) {
  const char *slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

static bool hasRomExtension(const String &name) {
  String lower = name;
  lower.toLowerCase();
  return lower.endsWith(".gb") || lower.endsWith(".gbc");
}

static void scanRomDirectory() {
  romCount = 0;
  File dir = SD.open(ROM_DIR);
  if (!dir || !dir.isDirectory()) {
    snprintf(browserMessage, sizeof(browserMessage), "Create %s and copy .gb files there.", ROM_DIR);
    if (dir) dir.close();
    return;
  }

  File entry = dir.openNextFile();
  while (entry && romCount < MAX_ROMS) {
    if (!entry.isDirectory()) {
      String name = entry.name();
      const int slash = name.lastIndexOf('/');
      if (slash >= 0) name = name.substring(slash + 1);
      if (name.length() > 0 && hasRomExtension(name)) {
        snprintf(romPaths[romCount], sizeof(romPaths[romCount]), "%s/%s", ROM_DIR, name.c_str());
        ++romCount;
      }
    }
    entry.close();
    entry = dir.openNextFile();
  }
  dir.close();

  if (romCount == 0) {
    snprintf(browserMessage, sizeof(browserMessage), "No .gb ROMs found in %s.", ROM_DIR);
  } else {
    snprintf(browserMessage, sizeof(browserMessage), "%u ROM(s) found. Tap one, then PLAY.", romCount);
    if (selectedRom >= romCount) selectedRom = 0;
    pageStart = (selectedRom / VISIBLE_ROMS) * VISIBLE_ROMS;
  }
}

static void requestUiRedraw() {
  UiSnapshot snapshot = {};
  snapshot.mode = static_cast<AppMode>(appMode);
  snapshot.sdMounted = sdMounted;
  snapshot.soundEnabled = soundEnabled;
  snapshot.paletteIndex = paletteIndex;
  snapshot.virtualKeys = virtualKeys;
  snapshot.romCount = romCount;
  snapshot.selectedRom = selectedRom;
  snapshot.pageStart = pageStart;
  memcpy(snapshot.gameTitle, gameTitle, sizeof(snapshot.gameTitle));
  memcpy(snapshot.browserMessage, browserMessage, sizeof(snapshot.browserMessage));

  for (uint8_t row = 0; row < VISIBLE_ROMS; ++row) {
    const uint8_t index = pageStart + row;
    if (index < romCount) {
      snprintf(snapshot.visibleRomNames[row], sizeof(snapshot.visibleRomNames[row]),
               "%.33s", baseNameOf(romPaths[index]));
    }
  }

  portENTER_CRITICAL(&uiMux);
  snapshot.generation = pendingUi.generation + 1;
  pendingUi = snapshot;
  portEXIT_CRITICAL(&uiMux);
}

static bool updateDisplaySnapshot() {
  bool changed = false;
  portENTER_CRITICAL(&uiMux);
  if (displayUi.generation != pendingUi.generation) {
    displayUi = pendingUi;
    changed = true;
  }
  portEXIT_CRITICAL(&uiMux);
  return changed;
}

static bool mountAndScanCard(bool retry) {
  if (retry) {
    setSdBus();
    SD.end();
    sdMounted = false;
    // SD.end() may end the SPI bus internally; force a clean remap on next begin.
    if (auxBusMode != 0) SPI.end();
    auxBusMode = 0;
  }

  setSdBus();
  sdMounted = SD.begin(SD_CS_PIN, SPI, SD_SPI_HZ);
  if (sdMounted) {
    SD.mkdir(SAVE_DIR);
    scanRomDirectory();
  } else {
    romCount = 0;
    snprintf(browserMessage, sizeof(browserMessage), "SD card not found. Insert FAT32 card, then RETRY.");
  }
  setTouchBus();
  return sdMounted;
}

// -----------------------------------------------------------------------------
// SD-backed 16 KiB bank cache. The GB core can address banked ROMs much larger
// than available ESP32 SRAM without copying a whole cartridge into RAM.
// -----------------------------------------------------------------------------
static int findCachedPage(uint32_t pageNumber) {
  for (uint8_t i = 0; i < ROM_CACHE_PAGES; ++i) {
    if (romCache[i].valid && romCache[i].pageNumber == pageNumber) return i;
  }
  return -1;
}

static int chooseCacheVictim() {
  for (uint8_t i = 0; i < ROM_CACHE_PAGES; ++i) {
    if (!romCache[i].valid) return i;
  }
  return (romCache[0].lastUse <= romCache[1].lastUse) ? 0 : 1;
}

static bool fillRomPage(uint32_t pageNumber, uint8_t slot) {
  RomCachePage &page = romCache[slot];
  const uint32_t offset = pageNumber * ROM_PAGE_SIZE;
  memset(page.bytes, 0xFF, sizeof(page.bytes));
  if (!sdMounted || !romFile || offset >= romSize) {
    page.valid = false;
    return false;
  }

  setSdBus();
  bool ok = false;
  if (romFile.seek(offset)) {
    const uint32_t wanted = min(ROM_PAGE_SIZE, romSize - offset);
    const size_t got = romFile.read(page.bytes, wanted);
    ok = got == wanted;
  }
  setTouchBus();

  page.pageNumber = pageNumber;
  page.lastUse = ++cacheClock;
  page.valid = true;  // A short final page is padded with 0xFF above.
  return ok;
}

static uint8_t readRomByte(uint32_t address) {
  if (address >= romSize || !romFile) return 0xFF;

  const uint32_t pageNumber = address / ROM_PAGE_SIZE;
  int slot = findCachedPage(pageNumber);
  if (slot < 0) {
    slot = chooseCacheVictim();
    fillRomPage(pageNumber, static_cast<uint8_t>(slot));
  }
  romCache[slot].lastUse = ++cacheClock;
  return romCache[slot].bytes[address % ROM_PAGE_SIZE];
}

static uint8_t gbRomRead(struct gb_s *, const uint_fast32_t address) {
  return readRomByte(static_cast<uint32_t>(address));
}

// -----------------------------------------------------------------------------
// Cartridge RAM persistence
// -----------------------------------------------------------------------------
static void makeSavePath(const char *romPath) {
  const char *base = baseNameOf(romPath);
  char clean[52];
  size_t out = 0;
  for (size_t i = 0; base[i] && base[i] != '.' && out < sizeof(clean) - 1; ++i) {
    const char c = base[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '_' || c == '-') {
      clean[out++] = c;
    } else if (c == ' ' || c == '.') {
      clean[out++] = '_';
    }
  }
  if (out == 0) {
    memcpy(clean, "game", 4);
    out = 4;
  }
  clean[out] = '\0';
  snprintf(savePath, sizeof(savePath), "%s/%s.sav", SAVE_DIR, clean);
}

static void loadCartRam() {
  if (!sdMounted || !cartRam || cartRamSize == 0) return;
  memset(cartRam, 0, cartRamSize);

  setSdBus();
  File file = SD.open(savePath, FILE_READ);
  if (file) {
    const size_t got = file.read(cartRam, cartRamSize);
    if (got < cartRamSize) memset(cartRam + got, 0, cartRamSize - got);
    file.close();
  }
  setTouchBus();
  cartRamDirty = false;
}

static void saveCartRam() {
  if (!sdMounted || !cartRam || cartRamSize == 0 || !cartRamDirty) return;

  setSdBus();
  SD.mkdir(SAVE_DIR);
  File file = SD.open(savePath, FILE_WRITE);
  bool ok = false;
  if (file) {
    const size_t written = file.write(cartRam, cartRamSize);
    file.flush();
    file.close();
    ok = written == cartRamSize;
  }
  setTouchBus();

  if (ok) {
    cartRamDirty = false;
  } else {
    cartWriteAt = millis();  // avoid retrying a failed card write every frame
    Serial.printf("Save failed: %s\n", savePath);
  }
}

static uint8_t gbCartRamRead(struct gb_s *, const uint_fast32_t address) {
  if (cartRam && address < cartRamSize) return cartRam[address];
  return 0xFF;
}

static void gbCartRamWrite(struct gb_s *, const uint_fast32_t address, const uint8_t value) {
  if (cartRam && address < cartRamSize) {
    cartRam[address] = value;
    cartRamDirty = true;
    cartWriteAt = millis();
  }
}

static void gbError(struct gb_s *, const enum gb_error_e error, const uint16_t address) {
  lastGbError = static_cast<uint8_t>(error);
  Serial.printf("Peanut-GB error %u at 0x%04X\n", static_cast<unsigned>(error), address);
}

// -----------------------------------------------------------------------------
// 160x144 capture plus queued 240x216 RGB565 presentation
// -----------------------------------------------------------------------------
static void lcdDrawLine(struct gb_s *, const uint8_t *pixels, const uint_fast8_t line) {
  const int bufferIndex = renderBufferIndex;
  if (line >= GB_H || bufferIndex < 0) return;
  memcpy(frameBuffers[bufferIndex] + line * GB_W, pixels, GB_W);
}

static int acquireFreeFrameBuffer() {
  int bufferIndex = -1;
  portENTER_CRITICAL(&frameBufferMux);
  for (uint8_t index = 0; index < 2; ++index) {
    if (frameBufferStates[index] == BUFFER_FREE) {
      frameBufferStates[index] = BUFFER_RENDERING;
      bufferIndex = index;
      break;
    }
  }
  portEXIT_CRITICAL(&frameBufferMux);
  return bufferIndex;
}

static void releaseFrameBuffer(int bufferIndex) {
  if (bufferIndex < 0 || bufferIndex >= 2) return;
  portENTER_CRITICAL(&frameBufferMux);
  frameBufferStates[bufferIndex] = BUFFER_FREE;
  portEXIT_CRITICAL(&frameBufferMux);
}

static void discardQueuedFrames() {
  if (!readyFrames) return;
  int staleIndex = -1;
  while (xQueueReceive(readyFrames, &staleIndex, 0) == pdTRUE) {
    releaseFrameBuffer(staleIndex);
  }
}

static void publishFrameBuffer(int bufferIndex) {
  portENTER_CRITICAL(&frameBufferMux);
  frameBufferStates[bufferIndex] = BUFFER_READY;
  portEXIT_CRITICAL(&frameBufferMux);

  if (xQueueSend(readyFrames, &bufferIndex, 0) == pdTRUE) return;

  // Keep the newest frame if the LCD is behind; stale frames are expendable,
  // emulated CPU frames are not.
  int staleIndex = -1;
  if (xQueueReceive(readyFrames, &staleIndex, 0) == pdTRUE) {
    releaseFrameBuffer(staleIndex);
    ++droppedFrameCount;
  }
  if (xQueueSend(readyFrames, &bufferIndex, 0) != pdTRUE) {
    releaseFrameBuffer(bufferIndex);
    ++droppedFrameCount;
  }
}

static void presentFrame(const uint8_t *source, uint8_t activePalette) {
  const uint16_t *palette = DMG_PALETTES[activePalette % PALETTE_COUNT];
  for (uint16_t sourceY = 0; sourceY < GB_H; ++sourceY) {
    const int firstY = (sourceY * 3 + 1) / 2;
    const int endY = ((sourceY + 1) * 3 + 1) / 2;
    const uint8_t outputRows = static_cast<uint8_t>(endY - firstY);
    const uint8_t *sourceRow = source + sourceY * GB_W;

    for (uint16_t x = 0; x < LCD_W; ++x) {
      const uint16_t color = palette[sourceRow[scaleX[x]] & 0x03];
      scaledLine[x] = color;
      if (outputRows == 2) scaledLine[LCD_W + x] = color;
    }
    tft.pushImage(0, firstY, LCD_W, outputRows, scaledLine);
  }
  ++presentedFrameCount;
}

static void initializeFrameBuffers() {
  const size_t frameBytes = static_cast<size_t>(GB_W) * GB_H;
  frameBuffers[0] = static_cast<uint8_t *>(malloc(frameBytes));
  frameBuffers[1] = static_cast<uint8_t *>(malloc(frameBytes));
  if (frameBuffers[0]) memset(frameBuffers[0], 0, frameBytes);
  if (frameBuffers[1]) memset(frameBuffers[1], 0, frameBytes);
  for (uint16_t x = 0; x < LCD_W; ++x) scaleX[x] = static_cast<uint8_t>((x * 2) / 3);
}

// -----------------------------------------------------------------------------
// ESP32 DAC audio at 16 kHz on GPIO26 (CYD speaker amplifier input)
// -----------------------------------------------------------------------------
static void IRAM_ATTR audioTimerISR() {
  const uint8_t sample = soundEnabled ? audio_update() : 128;
  if (sample != 128) ++nonSilentAudioSamples;
  dac_output_voltage(DAC_CHANNEL_2, sample);
}

static void initAudio() {
  // This hardware revision has an active-low speaker-amplifier enable.
  pinMode(SPEAKER_ENABLE_PIN, OUTPUT);
  digitalWrite(SPEAKER_ENABLE_PIN, HIGH);  // keep muted while the DAC/timer starts

  const esp_err_t dacResult = dac_output_enable(DAC_CHANNEL_2);
  dac_output_voltage(DAC_CHANNEL_2, 128);
  if (dacResult != ESP_OK) {
    Serial.printf("DAC2/GPIO26 init failed: %d\n", static_cast<int>(dacResult));
  }

  // 80 MHz APB / divider 5 = 16 MHz; alarm every 1000 ticks -> 16 kHz.
  audioTimer = timerBegin(0, 5, true);
  if (audioTimer) {
    timerAttachInterrupt(audioTimer, &audioTimerISR, true);
    timerAlarmWrite(audioTimer, 1000, true);
    timerAlarmEnable(audioTimer);
    digitalWrite(SPEAKER_ENABLE_PIN,
                 (dacResult == ESP_OK && soundEnabled) ? LOW : HIGH);
  } else {
    digitalWrite(SPEAKER_ENABLE_PIN, HIGH);
    Serial.println("Audio timer init failed; DAC audio will be silent.");
  }
}

// -----------------------------------------------------------------------------
// UI drawing (portrait orientation gives the full GB image plus touch controls)
// -----------------------------------------------------------------------------
static uint16_t activeButtonColor(uint8_t mask) {
  return (displayUi.virtualKeys & mask) ? 0x07E0 : 0x39E7;
}

static void drawButtonCircle(int x, int y, int radius, uint8_t mask, const char *label) {
  const uint16_t fill = activeButtonColor(mask);
  tft.fillCircle(x, y, radius, fill);
  tft.drawCircle(x, y, radius, TFT_WHITE);
  tft.setTextColor(TFT_WHITE, fill);
  tft.drawCentreString(label, x, y - 4, 1);
}

static void drawButtonPill(int x, int y, int w, int h, uint8_t mask, const char *label) {
  const uint16_t fill = activeButtonColor(mask);
  tft.fillRoundRect(x, y, w, h, 5, fill);
  tft.drawRoundRect(x, y, w, h, 5, TFT_WHITE);
  tft.setTextColor(TFT_WHITE, fill);
  tft.drawCentreString(label, x + w / 2, y + (h - 8) / 2, 1);
}

static void renderGamePanel() {
  const uint16_t panel = 0x1082;
  tft.fillRect(0, STATUS_Y, SCREEN_W, SCREEN_H - STATUS_Y, panel);
  tft.drawFastHLine(0, STATUS_Y, SCREEN_W, 0x7BEF);

  char shownTitle[22];
  snprintf(shownTitle, sizeof(shownTitle), "%.16s", displayUi.gameTitle[0] ? displayUi.gameTitle : "GameBoy");
  tft.setTextColor(TFT_CYAN, panel);
  tft.drawString(shownTitle, 4, STATUS_Y + 6, 1);

  tft.fillRoundRect(127, STATUS_Y + 2, 34, 20, 4, 0x39E7);
  tft.fillRoundRect(164, STATUS_Y + 2, 34, 20, 4, displayUi.soundEnabled ? 0x07E0 : 0x7800);
  tft.fillRoundRect(201, STATUS_Y + 2, 36, 20, 4, 0x001F);
  tft.setTextColor(TFT_WHITE, 0x39E7);
  tft.drawCentreString("PAL", 144, STATUS_Y + 8, 1);
  tft.setTextColor(TFT_WHITE, displayUi.soundEnabled ? 0x07E0 : 0x7800);
  tft.drawCentreString(displayUi.soundEnabled ? "SND" : "MUT", 181, STATUS_Y + 8, 1);
  tft.setTextColor(TFT_WHITE, 0x001F);
  tft.drawCentreString("MENU", 219, STATUS_Y + 8, 1);

  // Latching controls let a single-touch panel combine (for example) a held
  // direction with A/B. Tap a highlighted key again to release it.
  drawButtonCircle(46, 258, 14, KEY_UP, "^");
  drawButtonCircle(20, 282, 14, KEY_LEFT, "<");
  drawButtonCircle(72, 282, 14, KEY_RIGHT, ">");
  drawButtonCircle(46, 306, 14, KEY_DOWN, "v");

  drawButtonPill(105, 246, 50, 22, KEY_SELECT, "SEL");
  drawButtonPill(105, 273, 50, 22, KEY_START, "START");
  tft.fillRoundRect(105, 301, 50, 18, 5, 0x7800);
  tft.drawRoundRect(105, 301, 50, 18, 5, TFT_WHITE);
  tft.setTextColor(TFT_WHITE, 0x7800);
  tft.drawCentreString("CLR", 130, 306, 1);

  drawButtonCircle(178, 263, 18, KEY_A, "A");
  drawButtonCircle(215, 298, 18, KEY_B, "B");
}

static void renderBrowserScreen() {
  tft.fillScreen(TFT_BLACK);
  tft.fillRect(0, 0, SCREEN_W, 48, 0x0010);
  tft.setTextColor(TFT_YELLOW, 0x0010);
  tft.drawCentreString("ESPboy GameBoy", SCREEN_W / 2, 5, 2);
  tft.setTextColor(TFT_CYAN, 0x0010);
  tft.drawCentreString("CYD / ESP32-2432S028R", SCREEN_W / 2, 29, 1);

  if (!displayUi.sdMounted) {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawCentreString("SD card not ready", SCREEN_W / 2, 92, 2);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawCentreString("Insert a FAT32 microSD card.", SCREEN_W / 2, 130, 1);
    tft.drawCentreString("Copy legal .gb ROMs to /ROMS/.", SCREEN_W / 2, 148, 1);
    tft.drawCentreString("Tap RETRY below.", SCREEN_W / 2, 176, 1);
    tft.fillRoundRect(18, 278, 204, 38, 7, 0x001F);
    tft.drawRoundRect(18, 278, 204, 38, 7, TFT_WHITE);
    tft.setTextColor(TFT_WHITE, 0x001F);
    tft.drawCentreString("RETRY SD", SCREEN_W / 2, 291, 2);
    return;
  }

  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawCentreString(displayUi.browserMessage, SCREEN_W / 2, 51, 1);

  if (displayUi.romCount == 0) {
    tft.setTextColor(TFT_ORANGE, TFT_BLACK);
    tft.drawCentreString("No Game Boy ROMs found", SCREEN_W / 2, 102, 2);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawCentreString("Create /ROMS on the card and add .gb files.", SCREEN_W / 2, 142, 1);
    tft.fillRoundRect(18, 278, 204, 38, 7, 0x001F);
    tft.drawRoundRect(18, 278, 204, 38, 7, TFT_WHITE);
    tft.setTextColor(TFT_WHITE, 0x001F);
    tft.drawCentreString("RESCAN", SCREEN_W / 2, 291, 2);
    return;
  }

  for (uint8_t row = 0; row < VISIBLE_ROMS; ++row) {
    const uint8_t index = displayUi.pageStart + row;
    if (index >= displayUi.romCount) break;
    const int y = 72 + row * 33;
    const bool selected = index == displayUi.selectedRom;
    const uint16_t fill = selected ? 0x03EF : 0x2104;
    tft.fillRoundRect(7, y, 226, 29, 4, fill);
    tft.drawRoundRect(7, y, 226, 29, 4, selected ? TFT_YELLOW : 0x7BEF);
    tft.setTextColor(TFT_WHITE, fill);
    char label[34];
    snprintf(label, sizeof(label), "%02u  %.26s", static_cast<unsigned>(index + 1), displayUi.visibleRomNames[row]);
    tft.drawString(label, 14, y + 10, 1);
  }

  const uint8_t pageNumber = static_cast<uint8_t>(displayUi.pageStart / VISIBLE_ROMS + 1);
  const uint8_t totalPages = static_cast<uint8_t>((displayUi.romCount + VISIBLE_ROMS - 1) / VISIBLE_ROMS);
  char pageLabel[20];
  snprintf(pageLabel, sizeof(pageLabel), "Page %u / %u", pageNumber, totalPages);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawCentreString(pageLabel, SCREEN_W / 2, 272, 1);

  tft.fillRoundRect(0, 286, 66, 34, 4, 0x4208);
  tft.fillRoundRect(69, 286, 102, 34, 4, 0x07E0);
  tft.fillRoundRect(174, 286, 66, 34, 4, 0x4208);
  tft.setTextColor(TFT_WHITE, 0x4208);
  tft.drawCentreString("PREV", 33, 299, 1);
  tft.setTextColor(TFT_BLACK, 0x07E0);
  tft.drawCentreString("PLAY", 120, 295, 2);
  tft.setTextColor(TFT_WHITE, 0x4208);
  tft.drawCentreString("NEXT", 207, 299, 1);
}

// -----------------------------------------------------------------------------
// ROM start/stop and game browser
// -----------------------------------------------------------------------------
static void releaseCartRam() {
  if (cartRam) free(cartRam);
  cartRam = nullptr;
  cartRamSize = 0;
  cartRamDirty = false;
}

static void closeRomFile() {
  setSdBus();
  if (romFile) romFile.close();
  setTouchBus();
  romSize = 0;
  for (uint8_t i = 0; i < ROM_CACHE_PAGES; ++i) romCache[i].valid = false;
}

static void openBrowser() {
  if (gameRunning) {
    saveCartRam();
    closeRomFile();
  }
  discardQueuedFrames();
  virtualKeys = 0;
  gameRunning = false;
  nextGameFrameUs = 0;
  appMode = MODE_BROWSER;
  requestUiRedraw();
}

static uint32_t expectedRomBytes(uint8_t code) {
  static const uint16_t bankCounts[] = {2, 4, 8, 16, 32, 64, 128, 256, 512};
  if (code >= sizeof(bankCounts) / sizeof(bankCounts[0])) return 0;
  return static_cast<uint32_t>(bankCounts[code]) * ROM_PAGE_SIZE;
}

static void startSelectedRom() {
  if (!sdMounted || selectedRom >= romCount) return;
  discardQueuedFrames();

  if (gameRunning) {
    saveCartRam();
    closeRomFile();
    gameRunning = false;
  }
  releaseCartRam();
  virtualKeys = 0;
  lastGbError = 0;
  gameTitle[0] = '\0';

  setSdBus();
  romFile = SD.open(romPaths[selectedRom], FILE_READ);
  if (!romFile) {
    setTouchBus();
    snprintf(browserMessage, sizeof(browserMessage), "Could not open %s", baseNameOf(romPaths[selectedRom]));
    requestUiRedraw();
    return;
  }
  romSize = static_cast<uint32_t>(romFile.size());
  setTouchBus();

  if (romSize < 0x150) {
    closeRomFile();
    snprintf(browserMessage, sizeof(browserMessage), "ROM is too small (header missing)." );
    requestUiRedraw();
    return;
  }

  for (uint8_t i = 0; i < ROM_CACHE_PAGES; ++i) romCache[i].valid = false;
  cacheClock = 0;

  // Validate header size codes before calling Peanut-GB. Its lookup tables assume
  // standard ROM-size codes 0..8 and RAM-size codes 0..5.
  const uint8_t romSizeCode = readRomByte(0x0148);
  const uint8_t ramSizeCode = readRomByte(0x0149);
  const uint32_t expectedBytes = expectedRomBytes(romSizeCode);
  if (expectedBytes == 0 || ramSizeCode > 5 || romSize < expectedBytes) {
    closeRomFile();
    snprintf(browserMessage, sizeof(browserMessage), "Unsupported or incomplete GB ROM header." );
    requestUiRedraw();
    return;
  }

  memset(&gb, 0, sizeof(gb));
  const enum gb_init_error_e result = gb_init(&gb, gbRomRead, gbCartRamRead, gbCartRamWrite, gbError, nullptr);
  if (result != GB_INIT_NO_ERROR) {
    closeRomFile();
    if (result == GB_INIT_INVALID_CHECKSUM) {
      snprintf(browserMessage, sizeof(browserMessage), "Header checksum failed. ROM may be damaged." );
    } else {
      snprintf(browserMessage, sizeof(browserMessage), "Cartridge mapper is not supported by Peanut-GB." );
    }
    requestUiRedraw();
    return;
  }

  gb_init_lcd(&gb, lcdDrawLine);
  gb.direct.interlace = false;
  gb.direct.frame_skip = false;  // double buffering handles display pacing without skipping LCD callbacks

  if (gb.cart_ram) {
    size_t requestedRam = 0;
    if (gb_get_save_size_s(&gb, &requestedRam) == 0 && requestedRam > 0) {
      cartRam = static_cast<uint8_t *>(malloc(requestedRam));
      if (cartRam) {
        cartRamSize = requestedRam;
        makeSavePath(romPaths[selectedRom]);
        loadCartRam();
      } else {
        snprintf(browserMessage, sizeof(browserMessage), "Game RAM allocation failed; save RAM disabled." );
        Serial.println(browserMessage);
      }
    }
  }

  gb_get_rom_name(&gb, gameTitle);
  if (gameTitle[0] == '\0') {
    snprintf(gameTitle, sizeof(gameTitle), "%.16s", baseNameOf(romPaths[selectedRom]));
  }

  cartRamDirty = false;
  gameRunning = true;
  appMode = MODE_GAME;
  nextGameFrameUs = micros();
  requestUiRedraw();
  Serial.printf("Running %s (%lu bytes), cart RAM %u bytes\n", gameTitle,
                static_cast<unsigned long>(romSize), static_cast<unsigned>(cartRamSize));
}

// -----------------------------------------------------------------------------
// Touch controls. Buttons are toggled/latching because XPT2046 is single-touch.
// This allows a held direction and A/B to be combined without multitouch.
// -----------------------------------------------------------------------------
static void toggleKey(uint8_t key) {
  virtualKeys ^= key;
  requestUiRedraw();
}

static bool insideCircle(int x, int y, int cx, int cy, int r) {
  const int dx = x - cx;
  const int dy = y - cy;
  return (dx * dx + dy * dy) <= (r * r);
}

static void handleGameTap(int x, int y) {
  if (y >= STATUS_Y && y < CONTROLS_Y) {
    if (x >= 127 && x < 163) {
      paletteIndex = (paletteIndex + 1) % PALETTE_COUNT;
      preferences.putUChar("palette", paletteIndex);
      requestUiRedraw();
      return;
    }
    if (x >= 164 && x < 200) {
      soundEnabled = !soundEnabled;
      digitalWrite(SPEAKER_ENABLE_PIN, soundEnabled ? LOW : HIGH);
      preferences.putBool("sound", soundEnabled);
      requestUiRedraw();
      return;
    }
    if (x >= 201) {
      openBrowser();
      return;
    }
    return;
  }
  if (y < CONTROLS_Y) return;

  if (insideCircle(x, y, 46, 258, 17)) { toggleKey(KEY_UP); return; }
  if (insideCircle(x, y, 20, 282, 17)) { toggleKey(KEY_LEFT); return; }
  if (insideCircle(x, y, 72, 282, 17)) { toggleKey(KEY_RIGHT); return; }
  if (insideCircle(x, y, 46, 306, 17)) { toggleKey(KEY_DOWN); return; }
  if (insideCircle(x, y, 178, 263, 21)) { toggleKey(KEY_A); return; }
  if (insideCircle(x, y, 215, 298, 21)) { toggleKey(KEY_B); return; }

  if (x >= 101 && x <= 159 && y >= 242 && y <= 270) { toggleKey(KEY_SELECT); return; }
  if (x >= 101 && x <= 159 && y >= 270 && y <= 300) { toggleKey(KEY_START); return; }
  if (x >= 101 && x <= 159 && y >= 300 && y <= 320) {
    virtualKeys = 0;
    requestUiRedraw();
  }
}

static void handleBrowserTap(int x, int y) {
  if (!sdMounted) {
    if (y >= 270) {
      mountAndScanCard(true);
      requestUiRedraw();
    }
    return;
  }
  if (romCount == 0) {
    if (y >= 270) {
      mountAndScanCard(true);
      requestUiRedraw();
    }
    return;
  }

  const int listTop = 72;
  const int listBottom = listTop + VISIBLE_ROMS * 33;
  if (y >= listTop && y < listBottom) {
    const uint8_t row = static_cast<uint8_t>((y - listTop) / 33);
    const uint8_t index = pageStart + row;
    if (index < romCount) {
      selectedRom = index;
      requestUiRedraw();
    }
    return;
  }

  if (y >= 286) {
    if (x < 67) {
      if (pageStart >= VISIBLE_ROMS) pageStart -= VISIBLE_ROMS;
      selectedRom = pageStart;
      requestUiRedraw();
    } else if (x < 173) {
      startSelectedRom();
    } else {
      if (pageStart + VISIBLE_ROMS < romCount) pageStart += VISIBLE_ROMS;
      selectedRom = pageStart;
      requestUiRedraw();
    }
  }
}

static void mapTouchToScreen(const TS_Point &p, int &x, int &y) {
  // XPT2046 rotation 0 maps the verified landscape sample as:
  // xPortrait = yLandscape; yPortrait = 4095 - xLandscape.
  x = map(p.x, TOUCH_LAND_Y_MIN, TOUCH_LAND_Y_MAX, 0, SCREEN_W - 1);
  y = map(p.y, 4095 - TOUCH_LAND_X_MAX, 4095 - TOUCH_LAND_X_MIN, 0, SCREEN_H - 1);
  x = constrain(x, 0, SCREEN_W - 1);
  y = constrain(y, 0, SCREEN_H - 1);
}

static void pollTouch() {
  const bool down = touchscreen.touched();
  if (down && !touchWasDown) {
    const TS_Point p = touchscreen.getPoint();
    int x = 0;
    int y = 0;
    mapTouchToScreen(p, x, y);
    if (appMode == MODE_GAME) handleGameTap(x, y);
    else handleBrowserTap(x, y);
  }
  touchWasDown = down;
}

static void setJoypadFromVirtualKeys() {
  gb.direct.joypad_bits.a = (virtualKeys & KEY_A) ? 0 : 1;
  gb.direct.joypad_bits.b = (virtualKeys & KEY_B) ? 0 : 1;
  gb.direct.joypad_bits.select = (virtualKeys & KEY_SELECT) ? 0 : 1;
  gb.direct.joypad_bits.start = (virtualKeys & KEY_START) ? 0 : 1;
  gb.direct.joypad_bits.right = (virtualKeys & KEY_RIGHT) ? 0 : 1;
  gb.direct.joypad_bits.left = (virtualKeys & KEY_LEFT) ? 0 : 1;
  gb.direct.joypad_bits.up = (virtualKeys & KEY_UP) ? 0 : 1;
  gb.direct.joypad_bits.down = (virtualKeys & KEY_DOWN) ? 0 : 1;
}

static void displayTask(void *) {
  AppMode lastDrawnMode = static_cast<AppMode>(0xFF);

  for (;;) {
    int bufferIndex = -1;
    const bool gotFrame = xQueueReceive(readyFrames, &bufferIndex, pdMS_TO_TICKS(1)) == pdTRUE;
    const bool uiChanged = updateDisplaySnapshot();

    if (uiChanged) {
      if (displayUi.mode != lastDrawnMode) {
        if (displayUi.mode == MODE_BROWSER) {
          renderBrowserScreen();
        } else {
          tft.fillRect(0, 0, SCREEN_W, LCD_H, TFT_BLACK);
          renderGamePanel();
        }
        lastDrawnMode = displayUi.mode;
      } else if (displayUi.mode == MODE_BROWSER) {
        renderBrowserScreen();
      } else {
        renderGamePanel();
      }
    }

    if (gotFrame && bufferIndex >= 0 && bufferIndex < 2) {
      portENTER_CRITICAL(&frameBufferMux);
      if (frameBufferStates[bufferIndex] == BUFFER_READY) {
        frameBufferStates[bufferIndex] = BUFFER_PRESENTING;
      }
      portEXIT_CRITICAL(&frameBufferMux);

      if (displayUi.mode == MODE_GAME) {
        presentFrame(frameBuffers[bufferIndex], displayUi.paletteIndex);
      }
      releaseFrameBuffer(bufferIndex);
    } else if (!uiChanged) {
      vTaskDelay(pdMS_TO_TICKS(1));
    }
  }
}

static void reportFrameStats() {
  static uint32_t previousMs = 0;
  static uint32_t previousEmulated = 0;
  static uint32_t previousPresented = 0;
  static uint32_t previousDropped = 0;
  static uint32_t previousNonSilentAudio = 0;
  const uint32_t now = millis();
  if (previousMs == 0) {
    previousMs = now;
    return;
  }
  const uint32_t elapsed = now - previousMs;
  if (elapsed < 5000) return;

  const uint32_t emulated = emulatedFrameCount;
  const uint32_t presented = presentedFrameCount;
  const uint32_t dropped = droppedFrameCount;
  const uint32_t nonSilentAudio = nonSilentAudioSamples;
  const uint32_t emuFps = (emulated - previousEmulated) * 1000U / elapsed;
  const uint32_t lcdFps = (presented - previousPresented) * 1000U / elapsed;
  Serial.printf("FPS: emulation=%lu display=%lu dropped=%lu audioActive=%lu freeHeap=%u\n",
                static_cast<unsigned long>(emuFps),
                static_cast<unsigned long>(lcdFps),
                static_cast<unsigned long>(dropped - previousDropped),
                static_cast<unsigned long>(nonSilentAudio - previousNonSilentAudio),
                ESP.getFreeHeap());
  previousMs = now;
  previousEmulated = emulated;
  previousPresented = presented;
  previousDropped = dropped;
  previousNonSilentAudio = nonSilentAudio;
}

void cydGameBoySetup() {
  Serial.begin(115200);
  delay(150);

  pinMode(TFT_BACKLIGHT_PIN, OUTPUT);
  digitalWrite(TFT_BACKLIGHT_PIN, HIGH);

  // Same explicit Arduino_GFX HSPI wiring and 40 MHz transfer rate as the
  // known-working ESP32-2432S028R example.
  tft.init();
  tft.setRotation(0);  // portrait layout: 240 x 320
  tft.fillScreen(TFT_BLACK);

  preferences.begin("cyd-gb", false);
  paletteIndex = preferences.getUChar("palette", 0) % PALETTE_COUNT;
  soundEnabled = preferences.getBool("sound", true);

  initAudio();

  // VSPI is switched to the SD pins for card access, then handed to XPT2046.
  mountAndScanCard(false);
  setTouchBus();
  touchscreen.begin();
  touchscreen.setRotation(TOUCH_ROTATION);

  initializeFrameBuffers();
  if (!frameBuffers[0] || !frameBuffers[1]) {
    Serial.println("Not enough RAM for the two Game Boy frame buffers.");
    for (;;) delay(1000);
  }

  readyFrames = xQueueCreate(1, sizeof(int));
  if (!readyFrames) {
    Serial.println("Could not create the display frame queue.");
    for (;;) delay(1000);
  }

  requestUiRedraw();
  if (xTaskCreatePinnedToCore(displayTask, "cyd-display", 8192, nullptr, 1,
                              &displayTaskHandle, 0) != pdPASS) {
    Serial.println("Could not create the CYD display task.");
    for (;;) delay(1000);
  }

  Serial.printf("CYD GameBoy ready. Free heap: %u bytes\n", ESP.getFreeHeap());
}

void cydGameBoyLoop() {
  pollTouch();

  if (appMode == MODE_GAME && gameRunning) {
    const uint32_t nowUs = micros();
    if (nextGameFrameUs == 0) nextGameFrameUs = nowUs;

    if (static_cast<int32_t>(nowUs - nextGameFrameUs) >= 0) {
      const int bufferIndex = acquireFreeFrameBuffer();
      if (bufferIndex >= 0) {
        memset(frameBuffers[bufferIndex], 0, static_cast<size_t>(GB_W) * GB_H);
        renderBufferIndex = bufferIndex;
      } else {
        renderBufferIndex = -1;
        ++droppedFrameCount;
      }

      setJoypadFromVirtualKeys();
      gb_run_frame(&gb);
      renderBufferIndex = -1;
      ++emulatedFrameCount;
      if (bufferIndex >= 0) publishFrameBuffer(bufferIndex);

      nextGameFrameUs += GAME_FRAME_PERIOD_US;
      const uint32_t afterFrameUs = micros();
      if (static_cast<int32_t>(afterFrameUs - nextGameFrameUs) >
          static_cast<int32_t>(GAME_FRAME_PERIOD_US)) {
        nextGameFrameUs = afterFrameUs;
      }
    } else {
      const uint32_t waitUs = nextGameFrameUs - nowUs;
      if (waitUs > 1500) delay(1);
      else if (waitUs > 0) delayMicroseconds(waitUs);
    }

    if (cartRamDirty && (millis() - cartWriteAt) > SAVE_DELAY_MS) {
      saveCartRam();
    }
  } else {
    nextGameFrameUs = 0;
    delay(2);
  }

  reportFrameStats();
}
