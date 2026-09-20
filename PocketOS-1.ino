/*
  PocketOS 1.0
  ---------------------------------------------------------------------------
  A small, touch-first operating environment for an ESP32-32E with a
  480x320 TFT (portrait base rotation 320x480), resistive TFT_eSPI touch and
  an SD card.  This is intentionally a new implementation; the supplied
  Minecraft sketch was used only to retain the board wiring and boot order.

  Required Arduino libraries:
    - TFT_eSPI       (configure the display controller and touch pins in
                      User_Setup.h for this exact panel)
    - TJpg_Decoder   (JPEG viewing)

  Hardware retained from the working example:
    SD card: CS=5, SCK=18, MISO=19, MOSI=23 on HSPI
    backlight: GPIO 27 (5 kHz, 8-bit PWM)

  Notes:
    * Bluetooth is deliberately not used.
    * SD is used for persistent documents, browser cache, drawings and apps;
      it is not treated as RAM, because flash latency/wear make that unsafe.
    * The first boot calibrates the resistive touch panel and stores exactly
      five calibration values in SPIFFS.
*/

#define FS_NO_GLOBALS
#include <Arduino.h>
#include <pgmspace.h>
#include <esp_arduino_version.h>
#include <SPI.h>
#include <FS.h>
#include <SD.h>
#include <SPIFFS.h>
#include <Preferences.h>
#include <TFT_eSPI.h>
#include <TJpg_Decoder.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include <esp_sleep.h>
#include <AudioTools.h>
#include <AudioTools/AudioCodecs/CodecMP3Helix.h>

// ---------------------------------------------------------------------------
// Hardware and storage constants
// ---------------------------------------------------------------------------
static constexpr uint8_t SD_CS_PIN = 5;
static constexpr uint8_t SD_SCK_PIN = 18;
static constexpr uint8_t SD_MISO_PIN = 19;
static constexpr uint8_t SD_MOSI_PIN = 23;
static constexpr uint8_t BACKLIGHT_PIN = 27;
static constexpr uint8_t AUDIO_ENABLE_PIN = 4;
static constexpr uint8_t BATTERY_ADC_PIN = 34;
static constexpr uint8_t TOUCH_IRQ_PIN = 36;
static constexpr uint8_t RGB_RED_PIN = 22;
static constexpr uint8_t RGB_GREEN_PIN = 16;
static constexpr uint8_t RGB_BLUE_PIN = 17;
static constexpr uint8_t BACKLIGHT_CHANNEL = 7;

static const char *const CONFIG_FILE = "/pocketos.cfg";
static const char *const TOUCH_FILE = "/PocketOS_TouchCal_v1";
static const char *const TOUCH_FILE_LANDSCAPE = "/PocketOS_TouchCal_landscape_v1";
static const char *const BROWSER_CACHE_FILE = "/PocketOS/Cache/last_page.txt";
static const char *const ROOT_POCKETOS = "/PocketOS";
static const char *const ROOT_APPS = "/Apps";
static const char *const ROOT_PICTURES = "/Pictures";
static const char *const ROOT_MUSIC = "/Music";
static const char *const ROOT_DOCUMENTS = "/Documents";
static const char *const ROOT_DRAWINGS = "/Drawings";

static constexpr size_t MAX_EDITOR_BYTES = 8192;
static constexpr size_t MAX_BROWSER_BYTES = 12000;
static constexpr uint8_t MAX_FILES = 64;
static constexpr uint8_t MAX_MEDIA = 48;
static constexpr uint8_t MAX_APPS = 16;
static constexpr uint8_t MAX_APP_BUTTONS = 20;

TFT_eSPI tft = TFT_eSPI();
SPIClass sdSPI(HSPI);
bool sdMounted = false;
bool spiffsMounted = false;
uint16_t touchCalibrationData[5] = {};
bool touchCalibrationValid = false;
Preferences touchPreferences;
uint8_t batteryPercent = 0;
float batteryVoltage = 0.0f;
uint32_t lastBatteryReadAt = 0;
bool ultraPowerSave = false;
uint32_t sleepTimeoutSeconds = 0;
uint32_t lastActivityAt = 0;

// ---------------------------------------------------------------------------
// Theme, profile, screen and interaction state
// ---------------------------------------------------------------------------
struct Palette {
  uint16_t background;
  uint16_t panel;
  uint16_t card;
  uint16_t cardAlt;
  uint16_t text;
  uint16_t muted;
  uint16_t accent;
  uint16_t accent2;
  uint16_t danger;
  uint16_t wall1;
  uint16_t wall2;
};

Palette colors;
uint8_t themeIndex = 0;
uint8_t wallpaperIndex = 0;
uint8_t brightnessPercent = 86;
bool landscape = false;
bool wifiEnabled = true;
String wifiSsid = "";
String wifiPassword = "";

String profileName = "Besitzer";
bool profileConfigured = false;
bool profileUsesPattern = false;
String profileLockHash = "";
bool profilePatternEdit = false;
String profilePatternCandidate = "";
String lockPinInput = "";
String lockPatternInput = "";
bool lockPatternDrawing = false;
bool profilePatternDrawing = false;

enum Screen : uint8_t {
  SC_LOCK,
  SC_HOME,
  SC_RECENTS,
  SC_SETTINGS,
  SC_WIFI,
  SC_FILES,
  SC_GALLERY,
  SC_MUSIC,
  SC_MESSAGES,
  SC_EDITOR,
  SC_BROWSER,
  SC_PROFILE,
  SC_APPS,
  SC_APP_STORE,
  SC_APP_RUNNER
};

Screen screen = SC_HOME;
Screen historyStack[8];
uint8_t historyCount = 0;
Screen recentScreens[6] = {SC_FILES, SC_BROWSER, SC_EDITOR, SC_SETTINGS, SC_GALLERY, SC_MUSIC};
uint8_t recentCount = 0;

bool redrawRequested = true;
bool partialFrameRequested = false;
enum PartialRegion : uint8_t { PARTIAL_NONE, PARTIAL_STATUS };
PartialRegion partialRegion = PARTIAL_NONE;
uint32_t lastFrameAt = 0;
uint32_t lastClockAt = 0;
String toastText = "";
uint32_t toastUntil = 0;

// A single eased scroll state is deliberately used instead of an off-screen
// framebuffer.  That keeps the UI smooth while staying comfortable on a
// no-PSRAM ESP32.
float scrollPosition = 0.0f;
float scrollTarget = 0.0f;
float touchStartScroll = 0.0f;

bool quickOpen = false;
bool quickOverlayActive = false;
float quickProgress = 0.0f;
float quickTarget = 0.0f;

struct TouchGesture {
  bool down = false;
  bool moved = false;
  uint16_t startX = 0;
  uint16_t startY = 0;
  uint16_t lastX = 0;
  uint16_t lastY = 0;
  uint32_t beganAt = 0;
};
TouchGesture touch;

// ---------------------------------------------------------------------------
// File, media and app metadata (fixed-size to avoid heap churn)
// ---------------------------------------------------------------------------
struct FileItem {
  char path[128];
  char name[54];
  bool directory;
  uint32_t size;
};
FileItem fileItems[MAX_FILES];
uint8_t fileCount = 0;
bool filesTruncated = false;
char currentDirectory[128] = "/";
char deleteCandidate[128] = "";
bool deleteConfirmVisible = false;

struct MediaItem {
  char path[128];
  char name[54];
};
MediaItem photoItems[MAX_MEDIA];
uint8_t photoCount = 0;
MediaItem musicItems[MAX_MEDIA];
uint8_t musicCount = 0;
char selectedPicture[128] = "";
bool galleryDetail = false;
char selectedMusic[128] = "";
bool musicPlaying = false;
File *mp3File = nullptr;
AnalogAudioStream *audioOutput = nullptr;
EncodedAudioStream *mp3Decoder = nullptr;
StreamCopy *mp3Copier = nullptr;

struct AddonApp {
  char path[128];
  char title[34];
  char accent[18];
};
struct StoreItem {
  char title[34];
  char path[96];
  char description[54];
  char accent[18];
};
AddonApp addonApps[MAX_APPS];
uint8_t addonCount = 0;
StoreItem storeItems[MAX_APPS];
uint8_t storeCount = 0;
bool storeLoading = false;
String storeMessage = "";
int8_t activeAddon = -1;

struct AppButton {
  int16_t x;
  int16_t y;
  int16_t w;
  int16_t h;
  char label[34];
  char action[92];
};
AppButton appButtons[MAX_APP_BUTTONS];

// ---------------------------------------------------------------------------
// Generic SD app VM
// ---------------------------------------------------------------------------
// App source and persistent state stay on SD. Only the small hot-variable
// cache and the currently visible buttons live in RAM. This makes the format
// extensible without giving every app a firmware-specific code path.
static constexpr uint8_t APP_VM_CACHE = 32;
static constexpr uint8_t APP_VM_VERTICES = 32;
static constexpr uint8_t APP_VM_EDGES = 48;
struct AppVmVar { char name[18]; int32_t value; bool dirty; };
struct AppVmVertex { int16_t x, y, z; };
struct AppVmEdge { uint8_t a, b; };
AppVmVar appVmVars[APP_VM_CACHE];
uint8_t appVmVarCount = 0;
AppVmVertex appVmVertices[APP_VM_VERTICES];
AppVmEdge appVmEdges[APP_VM_EDGES];
uint8_t appVmVertexCount = 0, appVmEdgeCount = 0;
struct AppVmTrail { char name[14]; uint8_t x[32], y[32], length; };
AppVmTrail appVmTrails[4]; uint8_t appVmTrailCount = 0;
String appVmStatePath = "";
uint32_t appVmLastTimer = 0, appVmStateDirtyAt = 0;
bool appVmLoaded = false, appVmStateDirty = false;

void appVmReset();
void appVmLoad(const char *path);
void appVmRender();
void appVmRunTimers();
void appVmAction(const String &action);
uint8_t appButtonCount = 0;
String appInput = "";
String appRuntimeMessage = "";
String calculatorExpression = "";
float cubeAngleX = -0.35f;
float cubeAngleY = 0.55f;
float cubeAngleZ = 0.0f;

// ---------------------------------------------------------------------------
// Editor and browser state
// ---------------------------------------------------------------------------
char editorPath[128] = "";
String editorText = "";
bool editorDirty = false;
uint16_t editorTopLine = 0;
uint16_t editorTotalLines = 0;

String browserUrl = "http://example.com";
String browserTitle = "PocketOS Browser";
String browserText = "Tippe oben eine Adresse ein. Der Browser liest nur HTML-Text; JavaScript und CSS werden weder geladen noch ausgefuehrt.";
String browserLinks[6];
String browserLinkLabels[6];
uint8_t browserLinkCount = 0;
bool browserLoading = false;

// Messages is a built-in PocketOS application, not an SD add-on.
static const char *const MESSAGES_SERVER_URL = "https://3000-iufuml5bz0whsrvgmda34-f98f3dbb.us1.manus.computer";
String messagesServerUrl = MESSAGES_SERVER_URL;
String messagesToken = "";
String messagesUsername = "";
String messagesPassword = "";
uint32_t messagesAccountId = 0;
String messagesRecipient = "";
String messagesBody = "";
String messagesInbox = "Noch keine Nachrichten geladen.";
bool messagesLoggedIn = false;
bool messagesLoading = false;
uint32_t lastMessagesPollAt = 0;

static constexpr uint8_t MAX_MESSAGE_CHATS = 6;
static constexpr uint8_t MAX_MESSAGE_BUBBLES = 8;
struct MessageChat {
  char username[34];
  char preview[82];
};
MessageChat messageChats[MAX_MESSAGE_CHATS];
uint8_t messageChatCount = 0;
String messageBubbleBodies[MAX_MESSAGE_BUBBLES];
bool messageBubbleMine[MAX_MESSAGE_BUBBLES] = {};
uint8_t messageBubbleCount = 0;
uint8_t messageChatScroll = 0;
int browserTotalLines = 0;

// JPEG rendering is clipped to the current content area so photos never draw
// over PocketOS' status/navigation chrome.
int jpegClipTop = 0;
int jpegClipBottom = 0;
static uint8_t bmpRow[1440];
static uint16_t bmpLine[480];

// ---------------------------------------------------------------------------
// On-screen keyboard
// ---------------------------------------------------------------------------
enum KeyboardPurpose : uint8_t {
  KB_NONE,
  KB_EDITOR,
  KB_BROWSER_URL,
  KB_WIFI_SSID,
  KB_WIFI_PASSWORD,
  KB_PROFILE_NAME,
  KB_PROFILE_PIN,
  KB_NEW_FOLDER,
  KB_NEW_FILE,
  KB_APP_INPUT,
  KB_MESSAGES_USERNAME,
  KB_MESSAGES_PASSWORD,
  KB_MESSAGES_RECIPIENT,
  KB_MESSAGES_BODY
};

bool keyboardOpen = false;
bool keyboardUpper = false;
bool keyboardSymbols = false;
uint8_t keyboardSymbolPage = 0;
bool keyboardDigitsOnly = false;
KeyboardPurpose keyboardPurpose = KB_NONE;
String keyboardText = "";

// Explicit declarations keep the Arduino preprocessor from generating
// malformed prototypes for functions that use default arguments.
void showToast(const String &message, uint32_t duration = 2200);
void drawText(const String &text, int x, int y, uint8_t size = 1, uint16_t color = TFT_WHITE);
void drawCentered(const String &text, int x, int y, uint8_t size = 1, uint16_t color = TFT_WHITE);
void drawRight(const String &text, int x, int y, uint8_t size = 1, uint16_t color = TFT_WHITE);
void drawWrapped(const String &source, int x, int y, int maximumWidth, int lineHeight,
                 int maximumLines, uint16_t color, uint8_t size = 1, int skipLines = 0,
                 int *totalLinesOut = nullptr);
void drawHeader(const String &title, const String &subtitle = "");
void drawAppFrame(const String &title, const String &subtitle = "");
enum UiGlyph : uint8_t { GLYPH_FILES, GLYPH_IMAGE, GLYPH_MUSIC, GLYPH_BROWSER, GLYPH_NOTE, GLYPH_BRUSH, GLYPH_SETTINGS, GLYPH_USER, GLYPH_APPS, GLYPH_BACK, GLYPH_HOME, GLYPH_BATTERY };
static constexpr uint8_t HOME_APP_COUNT = 9;
extern const char *homeLabels[];
static const char *const STORE_MANIFEST_URL = "https://raw.githubusercontent.com/arduinodude456/PocketOS-Apps/main/apps.txt";
static const char *const STORE_RAW_BASE = "https://raw.githubusercontent.com/arduinodude456/PocketOS-Apps/main/";
uint8_t uiFontForSize(uint8_t size);
void drawAddonRunner();
void drawThemeGlyph(UiGlyph glyph, int cx, int cy, uint8_t scale, uint16_t color);
void drawBatteryIcon(int cx, int cy, int w, int h, uint16_t color, uint8_t percent);
void drawRoundCard(int x, int y, int w, int h, uint16_t fill, uint16_t border = 0);
void openScreen(Screen target, bool pushCurrent = true);
void drawPatternGrid(const String &sequence, int top, int spacing, int currentX = -1, int currentY = -1);
void beginKeyboard(const String &initial, KeyboardPurpose purpose, bool digitsOnly = false);
void stopMp3Playback();
void drawMessages();
void handleMessagesTap(int x, int y);
void messagesLogin();
void pollMessages();
void sendMessageFromPocket();
void clearMessagesCredentials();

// The Arduino preprocessor is less reliable when many functions appear
// before their definitions. Keep the core PocketOS declarations explicit.
int screenW();
int screenH();
int statusHeight();
int headerHeight();
int navHeight();
int contentTop();
int contentBottom();
int contentHeight();
void drawStatusBar();
void drawToast();
void drawHome();
void drawSettings();
void drawWifi();
void drawFiles();
void drawGallery();
void drawMusic();
void drawEditor();
void drawProfile();
void drawLockScreen();
void drawRecents();
void drawApps();
void drawAppStore();
void saveConfig();
void loadConfig();
void initialiseStorage();
void loadDirectory(const char *directory);
void scanAddonApps();
void scanMediaDirectory(const char *directory, MediaItem *items, uint8_t &count, bool pictures);
void chooseDefaultEditorFile();
void openTextFile(const char *path);
void saveEditorFile();
void runTouchCalibration();
void applyBrightness();
void applyOrientation();
void setTheme();
void updateBatteryStatus();
void serviceAutomaticSleep();
void serviceMp3Playback();
void enterUltraPowerSave();
void enterDeepSleep();
void goHome();
void goBack();
void openRecents();
void openAppStore();
void downloadStoreItem(uint8_t index);
bool startMp3Playback(const char *path);
bool loadBrowserCache();
String hashLock(const String &secret);
String lowerCopy(String value);
String baseName(const String &path);
String humanBytes(uint32_t value);
bool storageReady();
bool isAddonFile(const String &path);
bool isTextFile(const String &path);
bool isPictureFile(const String &path);
bool isMusicFile(const String &path);
void safeCopy(char *destination, size_t destinationSize, const String &source);
void calibrateTouchIfNeeded();
bool jpegOutput(int16_t x, int16_t y, uint16_t width, uint16_t height, uint16_t *bitmap);

int keyboardTop() {
  return screenH() - (landscape ? 166 : 224);
}

void drawQuickPanel() {
  if (quickProgress <= 0.01f) return;
  const int panelH = landscape ? 170 : 224;
  const int panelY = -panelH + static_cast<int>(panelH * quickProgress);
  const int w = screenW();
  tft.fillRoundRect(0, panelY, w, panelH + 10, 10, colors.panel);
  tft.drawRoundRect(1, panelY, w - 2, panelH + 8, 10, colors.accent);
  drawText("Schnelleinstellungen", 14, panelY + 13, landscape ? 1 : 2, colors.text);
  drawRight("v", w - 14, panelY + 14, 1, colors.muted);
  const char *labels[] = {"WLAN", "Helligkeit", "Theme", "Drehen", "Sperren", "Cache"};
  String values[] = {wifiEnabled ? (WiFi.status() == WL_CONNECTED ? "AN" : "SUCHE") : "AUS",
                     String(brightnessPercent) + "%", String(themeIndex + 1),
                     landscape ? "QUER" : "HOCH", profileConfigured ? "JETZT" : "PROFIL", "HTML"};
  const int gap = 8;
  const int columns = 2;
  const int tileW = (w - gap * 3) / columns;
  const int tileH = landscape ? 34 : 48;
  const int top = panelY + (landscape ? 33 : 42);
  for (uint8_t i = 0; i < 6; ++i) {
    int col = i % 2;
    int row = i / 2;
    int x = gap + col * (tileW + gap);
    int y = top + row * (tileH + gap);
    uint16_t fill = (i == 0 && wifiEnabled) || i == 2 ? colors.accent : colors.card;
    drawRoundCard(x, y, tileW, tileH, fill, colors.cardAlt);
    drawText(labels[i], x + 9, y + (landscape ? 6 : 8), 1, fill == colors.accent ? TFT_WHITE : colors.text);
    drawRight(values[i], x + tileW - 9, y + (landscape ? 6 : 8), 1, fill == colors.accent ? TFT_WHITE : colors.muted);
  }
}

void drawKeyboard() {
  if (!keyboardOpen) return;
  const int top = keyboardTop();
  const int w = screenW();
  const int h = screenH() - top;
  tft.fillRect(0, top, w, h, colors.panel);
  tft.drawFastHLine(0, top, w, colors.accent);
  String shown = keyboardText;
  if (keyboardPurpose == KB_WIFI_PASSWORD || keyboardPurpose == KB_PROFILE_PIN || keyboardPurpose == KB_MESSAGES_PASSWORD) {
    shown = "";
    for (uint8_t i = 0; i < min(static_cast<size_t>(32), keyboardText.length()); ++i) shown += '*';
  }
  if (shown.length() > 36) shown = "..." + shown.substring(shown.length() - 33);
  drawText(shown.length() ? shown : "Eingabe", 10, top + 7, 1, shown.length() ? colors.text : colors.muted);
  const char *row1 = keyboardSymbols ? (keyboardSymbolPage ? "+-*/=_%" : "1234567890") : "qwertyuiop";
  const char *row2 = keyboardSymbols ? (keyboardSymbolPage ? "()[]{}" : "!@#$%^&*()") : "asdfghjkl";
  const char *row3 = keyboardSymbols ? (keyboardSymbolPage ? ":;,.!?" : "[]{}<>?/\\") : "zxcvbnm";
  String rows[] = {String(row1), String(row2), String(row3)};
  if (!keyboardSymbols && keyboardUpper) {
    for (uint8_t i = 0; i < 3; ++i) rows[i].toUpperCase();
  }
  int rowsTop = top + 27;
  int bottomH = landscape ? 33 : 37;
  int rowsBottom = screenH() - bottomH - 6;
  int rowH = max(23, (rowsBottom - rowsTop - 6) / 3);
  for (uint8_t row = 0; row < 3; ++row) {
    int count = rows[row].length();
    int gap = 3;
    int keyW = (w - gap * (count + 1)) / count;
    int x = (w - (keyW * count + gap * (count - 1))) / 2;
    int y = rowsTop + row * (rowH + 3);
    for (int key = 0; key < count; ++key) {
      drawRoundCard(x + key * (keyW + gap), y, keyW, rowH, colors.card, colors.cardAlt);
      drawCentered(String(rows[row][key]), x + key * (keyW + gap) + keyW / 2, y + rowH / 2, 1, colors.text);
    }
  }
  const int bottom = screenH() - bottomH - 3;
  const float fractions[] = {0.13f, 0.13f, 0.30f, 0.13f, 0.13f, 0.18f};
  const char *labels[] = {keyboardSymbols ? (keyboardSymbolPage ? "SYM1" : "SYM2") : "123", keyboardSymbols ? "ABC" : "SHIFT", "LEER", "ZEILE", "<", "OK"};
  int x = 3;
  for (uint8_t i = 0; i < 6; ++i) {
    int width = i == 5 ? w - x - 3 : static_cast<int>(w * fractions[i]) - 3;
    uint16_t fill = i == 5 ? colors.accent2 : (i == 4 ? colors.danger : colors.cardAlt);
    drawRoundCard(x, bottom, width, bottomH, fill);
    drawCentered(labels[i], x + width / 2, bottom + bottomH / 2, 1, i == 5 || i == 4 ? TFT_WHITE : colors.text);
    x += width + 3;
  }
}

void beginQuickOverlay() {
  quickOverlayActive = false;
  redraw();
  quickOverlayActive = true;
  redrawRequested = true;
}

void redraw() {
  if (partialFrameRequested && partialRegion == PARTIAL_STATUS) { drawStatusBar(); partialFrameRequested = false; redrawRequested = false; return; }
  partialFrameRequested = false;
  if (quickOverlayActive) {
    drawQuickPanel();
    drawToast();
    redrawRequested = false;
    return;
  }
  switch (screen) {
    case SC_LOCK: drawLockScreen(); break;
    case SC_HOME: drawHome(); break;
    case SC_RECENTS: drawRecents(); break;
    case SC_SETTINGS: drawSettings(); break;
    case SC_WIFI: drawWifi(); break;
    case SC_FILES: drawFiles(); break;
    case SC_GALLERY: drawGallery(); break;
    case SC_MUSIC: drawMusic(); break;
    case SC_MESSAGES: drawMessages(); break;
    case SC_EDITOR: drawEditor(); break;
    case SC_BROWSER: drawBrowser(); break;
    case SC_PROFILE: drawProfile(); break;
    case SC_APPS: drawApps(); break;
    case SC_APP_STORE: drawAppStore(); break;
    case SC_APP_RUNNER: drawAddonRunner(); break;
  }
  if (screen != SC_LOCK) drawQuickPanel();
  drawKeyboard();
  if (deleteConfirmVisible) {
    const int width = min(290, screenW() - 24);
    const int height = 104;
    const int x = (screenW() - width) / 2;
    const int y = (screenH() - height) / 2;
    drawRoundCard(x, y, width, height, colors.panel, colors.danger);
    drawCentered("Datei wirklich loeschen?", screenW() / 2, y + 21, 1, colors.text);
    drawCentered(baseName(deleteCandidate), screenW() / 2, y + 40, 1, colors.muted);
    drawRoundCard(x + 12, y + 60, (width - 36) / 2, 30, colors.cardAlt);
    drawCentered("ABBRECHEN", x + 12 + (width - 36) / 4, y + 75, 1, colors.text);
    drawRoundCard(x + 24 + (width - 36) / 2, y + 60, (width - 36) / 2, 30, colors.danger);
    drawCentered("LOESCHEN", x + 24 + (width - 36) * 3 / 4, y + 75, 1, TFT_WHITE);
  }
  drawToast();
  redrawRequested = false;
}

// ---------------------------------------------------------------------------
// Actions: Wi-Fi, profile lock, keyboard and the safe SD-app command set
// ---------------------------------------------------------------------------
void beginWifiConnection() {
  if (!wifiEnabled) {
    showToast("WLAN ist ausgeschaltet");
    return;
  }
  if (!wifiSsid.length()) {
    showToast("Netzwerkname fehlt");
    return;
  }
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
  saveConfig();
  showToast("WLAN verbindet...");
}

void setWifiEnabled(bool enabled) {
  wifiEnabled = enabled;
  if (!enabled) {
    WiFi.disconnect();
    WiFi.mode(WIFI_OFF);
    showToast("WLAN ausgeschaltet");
  } else {
    WiFi.mode(WIFI_STA);
    if (wifiSsid.length()) WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
    showToast("WLAN eingeschaltet");
  }
  saveConfig();
  redrawRequested = true;
}

void lockDevice() {
  if (!profileConfigured || !profileLockHash.length()) {
    showToast("Bitte zuerst PIN oder Muster setzen");
    return;
  }
  keyboardOpen = false;
  quickTarget = 0;
  lockPinInput = "";
  lockPatternInput = "";
  screen = SC_LOCK;
  redrawRequested = true;
}

bool unlockWith(const String &secret) {
  if (secret.length() && hashLock(secret) == profileLockHash) {
    lockPinInput = "";
    lockPatternInput = "";
    screen = SC_HOME;
    redrawRequested = true;
    showToast("Entsperrt", 900);
    return true;
  }
  showToast("Nicht korrekt");
  return false;
}

void beginKeyboard(const String &initial, KeyboardPurpose purpose, bool digitsOnly) {
  keyboardText = initial;
  keyboardPurpose = purpose;
  keyboardDigitsOnly = digitsOnly;
  keyboardSymbols = digitsOnly;
  keyboardSymbolPage = 0;
  keyboardUpper = false;
  keyboardOpen = true;
  redrawRequested = true;
}

String cleanFileName(String name) {
  name.trim();
  name.replace("/", "_");
  name.replace("\\", "_");
  name.replace("..", "_");
  name.replace("\n", " ");
  if (name.length() > 42) name = name.substring(0, 42);
  return name;
}

void createFolderFromKeyboard() {
  String name = cleanFileName(keyboardText);
  if (!name.length()) name = "Neuer Ordner";
  String path = String(currentDirectory) + (String(currentDirectory).endsWith("/") ? "" : "/") + name;
  if (!storageReady() || !SD.mkdir(path)) showToast("Ordner konnte nicht angelegt werden");
  else {
    loadDirectory(currentDirectory);
    showToast("Ordner angelegt");
  }
}

void createFileFromKeyboard() {
  String name = cleanFileName(keyboardText);
  if (!name.length()) name = "Neue Datei.txt";
  if (name.indexOf('.') < 0) name += ".txt";
  String path = String(currentDirectory) + (String(currentDirectory).endsWith("/") ? "" : "/") + name;
  if (!storageReady() || SD.exists(path)) {
    showToast("Dateiname existiert bereits");
    return;
  }
  File file = SD.open(path, "w");
  if (!file) {
    showToast("Datei konnte nicht angelegt werden");
    return;
  }
  file.close();
  openTextFile(path.c_str());
  showToast("Neue Textdatei");
}

void finishKeyboard() {
  String completed = keyboardText;
  KeyboardPurpose completedPurpose = keyboardPurpose;
  if (completedPurpose == KB_PROFILE_PIN && completed.length() < 4) {
    showToast("PIN braucht mindestens 4 Ziffern");
    return;
  }
  keyboardOpen = false;
  keyboardPurpose = KB_NONE;
  keyboardDigitsOnly = false;
  switch (completedPurpose) {
    case KB_EDITOR:
      editorText = completed;
      editorDirty = true;
      break;
    case KB_BROWSER_URL:
      browserUrl = completed;
      fetchBrowserPage();
      break;
    case KB_WIFI_SSID:
      wifiSsid = completed;
      saveConfig();
      break;
    case KB_WIFI_PASSWORD:
      wifiPassword = completed;
      saveConfig();
      break;
    case KB_PROFILE_NAME:
      if (completed.length()) profileName = completed.substring(0, 28);
      saveConfig();
      break;
    case KB_PROFILE_PIN:
      profileUsesPattern = false;
      profileLockHash = hashLock(completed);
      profileConfigured = true;
      saveConfig();
      showToast("PIN gespeichert");
      break;
    case KB_NEW_FOLDER:
      createFolderFromKeyboard();
      break;
    case KB_NEW_FILE:
      createFileFromKeyboard();
      break;
    case KB_APP_INPUT:
      appInput = completed;
      appRuntimeMessage = "Eingabe uebernommen";
      break;
    case KB_MESSAGES_USERNAME:
      messagesUsername = completed;
      break;
    case KB_MESSAGES_PASSWORD:
      messagesPassword = completed;
      break;
    case KB_MESSAGES_RECIPIENT:
      messagesRecipient = completed;
      messagesRecipient.trim();
      break;
    case KB_MESSAGES_BODY:
      messagesBody = completed;
      break;
    default:
      break;
  }
  redrawRequested = true;
}

void appendKeyboardCharacter(char character) {
  if (keyboardDigitsOnly && (character < '0' || character > '9')) return;
  size_t maximum = keyboardPurpose == KB_EDITOR ? MAX_EDITOR_BYTES : (keyboardPurpose == KB_MESSAGES_BODY ? 180 : 96);
  if (keyboardText.length() >= maximum) {
    showToast("Eingabe ist voll");
    return;
  }
  keyboardText += character;
  redrawRequested = true;
}

void keyboardTap(int x, int y) {
  const int top = keyboardTop();
  if (y < top) return;
  const int w = screenW();
  const int bottomH = landscape ? 33 : 37;
  const int bottom = screenH() - bottomH - 3;
  if (y >= bottom) {
    const float fractions[] = {0.13f, 0.13f, 0.30f, 0.13f, 0.13f, 0.18f};
    int cursor = 3;
    for (uint8_t index = 0; index < 6; ++index) {
      int width = index == 5 ? w - cursor - 3 : static_cast<int>(w * fractions[index]) - 3;
      if (x >= cursor && x < cursor + width) {
        if (index == 0) {
          if (keyboardDigitsOnly) {
            keyboardSymbols = true;
          } else if (!keyboardSymbols) {
            keyboardSymbols = true;
            keyboardSymbolPage = 0;
          } else {
            keyboardSymbolPage = (keyboardSymbolPage + 1) % 2;
          }
        } else if (index == 1) {
          if (!keyboardDigitsOnly) {
            if (!keyboardSymbols) keyboardUpper = !keyboardUpper;
            else { keyboardSymbols = false; keyboardSymbolPage = 0; }
          }
        } else if (index == 2) {
          appendKeyboardCharacter(' ');
        } else if (index == 3) {
          if (keyboardPurpose == KB_EDITOR) appendKeyboardCharacter('\n');
        } else if (index == 4) {
          if (keyboardText.length()) keyboardText.remove(keyboardText.length() - 1);
        } else {
          finishKeyboard();
        }
        redrawRequested = true;
        return;
      }
      cursor += width + 3;
    }
    return;
  }
  int rowsTop = top + 27;
  int rowsBottom = screenH() - bottomH - 6;
  int rowH = max(23, (rowsBottom - rowsTop - 6) / 3);
  int row = (y - rowsTop) / (rowH + 3);
  if (row < 0 || row > 2 || y > rowsTop + row * (rowH + 3) + rowH) return;
  const char *rawRows[] = {keyboardSymbols ? (keyboardSymbolPage ? "+-*/=_%" : "1234567890") : "qwertyuiop",
                           keyboardSymbols ? (keyboardSymbolPage ? "()[]{}" : "!@#$%^&*()") : "asdfghjkl",
                           keyboardSymbols ? (keyboardSymbolPage ? ":;,.!?" : "[]{}<>?/\\") : "zxcvbnm"};
  String keys = rawRows[row];
  if (!keyboardSymbols && keyboardUpper) keys.toUpperCase();
  int count = keys.length();
  int gap = 3;
  int keyW = (w - gap * (count + 1)) / count;
  int startX = (w - (keyW * count + gap * (count - 1))) / 2;
  int column = (x - startX) / (keyW + gap);
  if (column >= 0 && column < count && x <= startX + column * (keyW + gap) + keyW) appendKeyboardCharacter(keys[column]);
}

bool evaluateCalculator(const String &source, float &result) {
  float values[16];
  char operators[16];
  uint8_t valueCount = 0, operatorCount = 0;
  String number = "";
  for (uint16_t i = 0; i <= source.length(); ++i) {
    char c = i < source.length() ? source[i] : '\0';
    if ((c >= '0' && c <= '9') || c == '.') {
      number += c;
    } else if (c == '+' || c == '-' || c == '*' || c == '/') {
      if (!number.length() || valueCount >= 16) return false;
      values[valueCount++] = number.toFloat();
      number = "";
      if (operatorCount >= 16) return false;
      operators[operatorCount++] = c;
    } else if (c == '\0') {
      if (!number.length() || valueCount >= 16) return false;
      values[valueCount++] = number.toFloat();
      number = "";
    } else if (c != ' ') {
      return false;
    }
  }
  if (number.length() || valueCount != operatorCount + 1) return false;
  for (uint8_t i = 0; i < operatorCount;) {
    if (operators[i] == '*' || operators[i] == '/') {
      if (operators[i] == '/' && values[i + 1] == 0) return false;
      values[i] = operators[i] == '*' ? values[i] * values[i + 1] : values[i] / values[i + 1];
      for (uint8_t j = i + 1; j + 1 < valueCount; ++j) values[j] = values[j + 1];
      for (uint8_t j = i; j + 1 < operatorCount; ++j) operators[j] = operators[j + 1];
      --valueCount; --operatorCount;
    } else ++i;
  }
  result = values[0];
  for (uint8_t i = 0; i < operatorCount; ++i)
    result = operators[i] == '+' ? result + values[i + 1] : result - values[i + 1];
  return true;
}

void applyLedAction(const String &value) {
  String color = lowerCopy(value);
  digitalWrite(RGB_RED_PIN, HIGH);
  digitalWrite(RGB_GREEN_PIN, HIGH);
  digitalWrite(RGB_BLUE_PIN, HIGH);
  if (color == "red" || color == "yellow" || color == "magenta" || color == "white") digitalWrite(RGB_RED_PIN, LOW);
  if (color == "green" || color == "yellow" || color == "cyan" || color == "white") digitalWrite(RGB_GREEN_PIN, LOW);
  if (color == "blue" || color == "cyan" || color == "magenta" || color == "white") digitalWrite(RGB_BLUE_PIN, LOW);
  appRuntimeMessage = color == "off" ? "LED aus" : "LED: " + color;
}

void executeAddonAction(String action) {
  action.trim(); String lower = lowerCopy(action);
  if (lower.startsWith("calc:")) {
    String key = action.substring(5); key.trim();
    if (key == "C") { calculatorExpression = ""; appInput = ""; appRuntimeMessage = ""; }
    else if (key == "=") { float result=0; if(evaluateCalculator(calculatorExpression,result)){String formatted=String(result,4);calculatorExpression=formatted;appInput=formatted;appRuntimeMessage="Ergebnis: "+formatted;} else appRuntimeMessage="Ungueltiger Ausdruck"; }
    else if (calculatorExpression.length() < 80) { calculatorExpression += key; appInput = calculatorExpression; appRuntimeMessage = ""; }
  } else if (lower.startsWith("open:")) {
    String target=lower.substring(5); if(target=="files")openScreen(SC_FILES); else if(target=="settings")openScreen(SC_SETTINGS); else if(target=="browser")openScreen(SC_BROWSER); else if(target=="gallery")openScreen(SC_GALLERY); else if(target=="music")openScreen(SC_MUSIC); else if(target=="profile")openScreen(SC_PROFILE); else showToast("Unbekanntes App-Ziel");
  } else if (lower.startsWith("url:")) { browserUrl=action.substring(4); openScreen(SC_BROWSER); fetchBrowserPage(); }
  else if (lower.startsWith("input:")) beginKeyboard(action.substring(6), KB_APP_INPUT);
  else if (lower.startsWith("toggle:wifi")) setWifiEnabled(!wifiEnabled);
  else if (lower.startsWith("led:")) applyLedAction(action.substring(4));
  else if (lower.startsWith("toast:")) showToast(action.substring(6));
  else if (lower.startsWith("write:") || lower.startsWith("read:") || lower.startsWith("set:")) { appRuntimeMessage = action.substring(action.indexOf(':') + 1); }
  else appVmAction(action);
  redrawRequested = true;
}


// ---------------------------------------------------------------------------
// Built-in Messages app: HTTPS client for the Manus-hosted PocketOS server.
// The server accepts a bearer token, so the firmware never needs browser cookies.
// ---------------------------------------------------------------------------
String messageJsonValue(const String &json, const String &key, int from)
{
  String needle = "\"" + key + "\":\"";
  int begin = json.indexOf(needle, from);
  if (begin < 0) return "";
  begin += needle.length();
  String value = "";
  bool escaped = false;
  for (int i = begin; i < static_cast<int>(json.length()); ++i) {
    char c = json[i];
    if (escaped) { value += c; escaped = false; continue; }
    if (c == '\\') { escaped = true; continue; }
    if (c == '"') break;
    value += c;
  }
  value.replace("\\n", "\n");
  value.replace("\\r", "\r");
  value.replace("\\\"", "\"");
  return value;
}

String messageJsonNumber(const String &json, const String &key, int from)
{
  String needle = "\"" + key + "\":";
  int begin = json.indexOf(needle, from);
  if (begin < 0) return "";
  begin += needle.length();
  int end = begin;
  while (end < static_cast<int>(json.length()) && json[end] >= '0' && json[end] <= '9') ++end;
  return json.substring(begin, end);
}

String messageJsonEscape(String value)
{
  value.replace("\\", "\\\\");
  value.replace("\"", "\\\"");
  value.replace("\n", "\\n");
  value.replace("\r", "\\r");
  return value;
}

bool messageHttpReady()
{
  return wifiEnabled && WiFi.status() == WL_CONNECTED && messagesServerUrl.length();
}

void messagesLogin()
{
  if (!messageHttpReady()) { showToast("WLAN nicht verbunden"); return; }
  if (!messagesUsername.length() || !messagesPassword.length()) { showToast("Nutzername und Passwort fehlen"); return; }
  messagesLoading = true;
  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();
  if (!http.begin(client, messagesServerUrl + "/api/device/login")) { messagesLoading = false; showToast("Messages-Server nicht erreichbar"); return; }
  http.addHeader("Content-Type", "application/json");
  String payload = "{\"username\":\"" + messageJsonEscape(messagesUsername) + "\",\"password\":\"" + messageJsonEscape(messagesPassword) + "\"}";
  int status = http.POST(payload);
  String response = http.getString();
  http.end();
  messagesLoading = false;
  if (status >= 200 && status < 300) {
    messagesToken = messageJsonValue(response, "token", 0);
    messagesAccountId = messageJsonNumber(response, "id", 0).toInt();
    messagesLoggedIn = messagesToken.length() > 0;
    saveConfig();
    showToast(messagesLoggedIn ? "Messages angemeldet" : "Serverantwort unvollstaendig");
    if (messagesLoggedIn) pollMessages();
  } else {
    showToast("Login fehlgeschlagen");
  }
  redrawRequested = true;
}

void pollMessages()
{
  if (!messagesLoggedIn || !messageHttpReady()) return;
  messagesLoading = true;
  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();
  String endpoint = messagesServerUrl + "/api/device/messages";
  if (messagesRecipient.length()) endpoint += "?username=" + messagesRecipient;
  if (!http.begin(client, endpoint)) { messagesLoading = false; return; }
  http.addHeader("Authorization", "Bearer " + messagesToken);
  int status = http.GET();
  String response = http.getString();
  http.end();
  messagesLoading = false;
  lastMessagesPollAt = millis();
  if (status == HTTP_CODE_UNAUTHORIZED) { messagesLoggedIn = false; messagesToken = ""; showToast("Messages-Login abgelaufen"); redrawRequested = true; return; }
  if (status != HTTP_CODE_OK) { messagesInbox = "Serverfehler " + String(status); redrawRequested = true; return; }
  messageChatCount = 0;
  messageBubbleCount = 0;
  int cursor = 0;
  if (!messagesRecipient.length()) {
    while (messageChatCount < MAX_MESSAGE_CHATS) {
      String username = messageJsonValue(response, "username", cursor);
      if (!username.length()) break;
      int usernameAt = response.indexOf("\"username\":\"", cursor);
      int bodyAt = response.indexOf("\"body\":\"", usernameAt);
      String preview = bodyAt >= 0 ? messageJsonValue(response, "body", bodyAt) : "Neue Unterhaltung";
      username.toCharArray(messageChats[messageChatCount].username, sizeof(messageChats[messageChatCount].username));
      preview.replace("\n", " ");
      messageChats[messageChatCount].preview[0] = '\0';
      preview.substring(0, sizeof(messageChats[messageChatCount].preview) - 1).toCharArray(messageChats[messageChatCount].preview, sizeof(messageChats[messageChatCount].preview));
      ++messageChatCount;
      cursor = usernameAt + 12;
    }
    messagesInbox = messageChatCount ? "Tippe einen Chat an" : "Noch keine Chats. Starte eine neue Unterhaltung.";
  } else {
    cursor = response.indexOf("\"messages\":");
    if (cursor < 0) cursor = 0;
    while (messageBubbleCount < MAX_MESSAGE_BUBBLES) {
      String body = messageJsonValue(response, "body", cursor);
      if (!body.length()) break;
      int bodyAt = response.indexOf("\"body\":\"", cursor);
      int senderAt = response.indexOf("\"senderId\":", cursor);
      String sender = senderAt >= 0 ? messageJsonNumber(response, "senderId", senderAt) : "0";
      messageBubbleBodies[messageBubbleCount] = body;
      messageBubbleMine[messageBubbleCount] = messagesAccountId > 0 && sender.toInt() == messagesAccountId;
      ++messageBubbleCount;
      cursor = bodyAt + 8;
    }
    messagesInbox = messageBubbleCount ? "" : "Noch keine Nachrichten in diesem Chat.";
  }
  redrawRequested = true;
}

void clearMessagesCredentials()
{
  messagesUsername = "";
  messagesPassword = "";
  messagesToken = "";
  messagesRecipient = "";
  messagesLoggedIn = false;
  messageChatCount = 0;
  messageBubbleCount = 0;
  saveConfig();
  showToast("Messages-Anmeldung geloescht");
  redrawRequested = true;
}

void sendMessageFromPocket()
{
  if (!messagesLoggedIn) { messagesLogin(); return; }
  if (!messageHttpReady()) { showToast("WLAN nicht verbunden"); return; }
  if (!messagesRecipient.length() || !messagesBody.length()) { showToast("Empfaenger und Text fehlen"); return; }
  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();
  if (!http.begin(client, messagesServerUrl + "/api/device/message")) { showToast("Server nicht erreichbar"); return; }
  http.addHeader("Authorization", "Bearer " + messagesToken);
  http.addHeader("Content-Type", "application/json");
  String payload = "{\"recipientUsername\":\"" + messageJsonEscape(messagesRecipient) + "\",\"body\":\"" + messageJsonEscape(messagesBody) + "\"}";
  int status = http.POST(payload);
  http.end();
  if (status >= 200 && status < 300) { messagesBody = ""; showToast("Nachricht gesendet"); pollMessages(); }
  else showToast("Senden fehlgeschlagen");
  redrawRequested = true;
}

void handleMessagesTap(int x, int y)
{
  int top = contentTop() + 8;
  int rowH = landscape ? 30 : 38;
  if (!messagesLoggedIn) {
    const int userFieldY = top + (landscape ? 72 : 82);
    const int passwordFieldY = top + (landscape ? 120 : 141);
    const int loginButtonY = top + (landscape ? 156 : 188);
    if (y >= userFieldY && y < userFieldY + rowH) beginKeyboard(messagesUsername, KB_MESSAGES_USERNAME);
    else if (y >= passwordFieldY && y < passwordFieldY + rowH) beginKeyboard(messagesPassword, KB_MESSAGES_PASSWORD);
    else if (y >= loginButtonY && y < loginButtonY + (landscape ? 34 : 38)) messagesLogin();
    return;
  }

  if (messagesRecipient.length()) {
    const int recipientY = top + (landscape ? 64 : 74);
    if (y >= top && y < top + (landscape ? 47 : 55) && x > screenW() / 2) {
      messagesRecipient = "";
      messageBubbleCount = 0;
      scrollPosition = scrollTarget = 0;
      pollMessages();
      redrawRequested = true;
      return;
    }
    if (y >= recipientY && y < recipientY + rowH) {
      beginKeyboard(messagesRecipient, KB_MESSAGES_RECIPIENT);
      return;
    }
    const int composerY = contentBottom() - (landscape ? 65 : 78);
    if (y >= composerY && y < composerY + (landscape ? 34 : 40)) {
      const int sendX = screenW() - (landscape ? 56 : 68);
      if (x >= sendX) sendMessageFromPocket();
      else beginKeyboard(messagesBody, KB_MESSAGES_BODY);
    }
    return;
  }

  const int recipientY = top + (landscape ? 64 : 74);
  if (y >= recipientY && y < recipientY + rowH) {
    beginKeyboard(messagesRecipient, KB_MESSAGES_RECIPIENT);
    return;
  }
  const int listTop = top + (landscape ? 104 : 122);
  for (uint8_t i = 0; i < messageChatCount; ++i) {
    int chatY = listTop + 16 + i * (rowH + 5);
    if (y >= chatY && y < chatY + rowH) {
      messagesRecipient = String(messageChats[i].username);
      scrollPosition = scrollTarget = 0;
      pollMessages();
      redrawRequested = true;
      return;
    }
  }
}

void drawMessages()
{
  drawAppFrame("Messages", messagesLoggedIn ? "CHATS" : "ANMELDUNG");
  const int w = screenW();
  const int top = contentTop() + 8;
  const int side = landscape ? 8 : 10;
  const int rowH = landscape ? 30 : 38;

  if (!messagesLoggedIn) {
    drawRoundCard(side, top, w - side * 2, landscape ? 46 : 54, colors.card, colors.cardAlt);
    tft.fillRoundRect(side, top, 4, landscape ? 46 : 54, 3, colors.accent);
    drawText("Messages-Konto", side + 14, top + 10, landscape ? 1 : 2, colors.text);
    drawText("Anmeldedaten bleiben auf dem Geraet gespeichert", side + 14, top + (landscape ? 27 : 35), 1, colors.muted);
    const int userFieldY = top + (landscape ? 72 : 82);
    drawText("NUTZERNAME", side + 2, userFieldY - 15, 1, colors.muted);
    drawRoundCard(side, userFieldY, w - side * 2, rowH, colors.card, colors.cardAlt);
    drawText(messagesUsername.length() ? messagesUsername : "z. B. luna_7", side + 10, userFieldY + 11, 1, messagesUsername.length() ? colors.text : colors.muted);
    const int passwordFieldY = top + (landscape ? 120 : 141);
    drawText("PASSWORT", side + 2, passwordFieldY - 15, 1, colors.muted);
    drawRoundCard(side, passwordFieldY, w - side * 2, rowH, colors.card, colors.cardAlt);
    String hidden = ""; for (uint8_t i = 0; i < min(static_cast<size_t>(18), messagesPassword.length()); ++i) hidden += '*';
    drawText(hidden.length() ? hidden : "mindestens 8 Zeichen", side + 10, passwordFieldY + 11, 1, hidden.length() ? colors.text : colors.muted);
    const int buttonY = top + (landscape ? 156 : 188);
    drawRoundCard(side, buttonY, w - side * 2, landscape ? 34 : 38, colors.accent, colors.accent);
    drawCentered(messagesLoading ? "VERBINDE..." : "ANMELDEN", w / 2, buttonY + (landscape ? 17 : 19), 1, TFT_WHITE);
    drawWrapped("Du kannst die gespeicherten Daten jederzeit unter Einstellungen > Messages-Anmeldung loeschen.", side + 3,
                buttonY + (landscape ? 46 : 52), w - side * 2 - 6, 14, 3, colors.muted);
    return;
  }

  const int recipientY = top + (landscape ? 64 : 74);
  drawRoundCard(side, top, w - side * 2, landscape ? 39 : 47, colors.card, colors.cardAlt);
  drawText(messagesRecipient.length() ? "CHAT MIT" : "DEINE CHATS", side + 10, top + 9, 1, colors.accent);
  if (messagesRecipient.length()) {
    drawText("@" + messagesRecipient, side + 10, top + (landscape ? 24 : 31), 1, colors.text);
    drawRight("< CHATS", w - side - 10, top + (landscape ? 24 : 31), 1, colors.muted);
    drawRoundCard(side, recipientY, w - side * 2, rowH, colors.card, colors.cardAlt);
    drawText("@" + messagesRecipient, side + 10, recipientY + 11, 1, colors.text);

    const int bubbleTop = recipientY + rowH + 8;
    const int bubbleBottom = contentBottom() - (landscape ? 72 : 86);
    const int bubbleOffset = static_cast<int>(scrollPosition * (landscape ? 20.0f : 22.0f));
    int bubbleY = bubbleTop - bubbleOffset;
    for (uint8_t i = 0; i < messageBubbleCount && bubbleY < bubbleBottom - 16; ++i) {
      if (bubbleY + (landscape ? 25 : 31) < bubbleTop - 2) {
        bubbleY += (landscape ? 25 + 5 : 31 + 7);
        continue;
      }
      String body = messageBubbleBodies[i];
      int bubbleW = min(w - 38, max(92, static_cast<int>(body.length() * 6 + 28)));
      if (bubbleW > w - 24) bubbleW = w - 24;
      int bubbleH = landscape ? 25 : 31;
      int bubbleX = messageBubbleMine[i] ? w - side - bubbleW : side;
      uint16_t fill = messageBubbleMine[i] ? colors.accent : colors.cardAlt;
      drawRoundCard(bubbleX, bubbleY, bubbleW, bubbleH, fill, fill);
      drawWrapped(body, bubbleX + 9, bubbleY + (landscape ? 6 : 8), bubbleW - 18, 13, 2, messageBubbleMine[i] ? TFT_WHITE : colors.text);
      bubbleY += bubbleH + (landscape ? 5 : 7);
    }
    if (!messageBubbleCount) drawCentered("Noch keine Nachrichten", w / 2, bubbleTop + 24, 1, colors.muted);

    const int composerY = contentBottom() - (landscape ? 65 : 78);
    drawRoundCard(side, composerY, w - side * 2 - (landscape ? 55 : 66), landscape ? 34 : 40, colors.card, colors.cardAlt);
    drawText(messagesBody.length() ? messagesBody : "Nachricht schreiben...", side + 10, composerY + (landscape ? 11 : 14), 1, messagesBody.length() ? colors.text : colors.muted);
    int sendX = w - side - (landscape ? 48 : 58);
    drawRoundCard(sendX, composerY, landscape ? 48 : 58, landscape ? 34 : 40, colors.accent, colors.accent);
    drawCentered("->", sendX + (landscape ? 24 : 29), composerY + (landscape ? 17 : 20), 1, TFT_WHITE);
    return;
  }

  drawText("NEUER CHAT", side + 3, top + (landscape ? 51 : 60), 1, colors.muted);
  drawRoundCard(side, top + (landscape ? 64 : 74), w - side * 2, rowH, colors.card, colors.cardAlt);
  drawText("@nutzername eingeben", side + 10, top + (landscape ? 75 : 85), 1, colors.muted);
  const int listTop = top + (landscape ? 104 : 122);
  drawText("LETZTE UNTERHALTUNGEN", side + 3, listTop, 1, colors.accent);
  if (!messageChatCount) {
    drawRoundCard(side, listTop + 16, w - side * 2, 52, colors.card, colors.cardAlt);
    drawWrapped("Noch keine Chats vorhanden. Gib oben einen exakten Nutzernamen ein.", side + 10, listTop + 27, w - side * 2 - 20, 14, 2, colors.muted);
  } else {
    for (uint8_t i = 0; i < messageChatCount; ++i) {
      int y = listTop + 16 + i * (rowH + 5);
      if (y + rowH > contentBottom()) break;
      drawRoundCard(side, y, w - side * 2, rowH, colors.card, colors.cardAlt);
      tft.fillCircle(side + 19, y + rowH / 2, landscape ? 9 : 11, colors.accent);
      drawCentered(String(messageChats[i].username[0]).substring(0, 1), side + 19, y + rowH / 2, 1, TFT_WHITE);
      drawText(messageChats[i].username, side + 36, y + 7, 1, colors.text);
      drawText(messageChats[i].preview, side + 36, y + (landscape ? 19 : 24), 1, colors.muted);
    }
  }
}

// ---------------------------------------------------------------------------
// Touch routing and gesture-based scrolling
// ---------------------------------------------------------------------------
void addPatternNode(String &sequence, int node) {
  if (node < 0 || node > 8) return;
  char character = static_cast<char>('0' + node);
  if (sequence.indexOf(character) < 0) sequence += character;
}

void savePatternFromProfile() {
  if (profilePatternCandidate.length() < 4) {
    showToast("Muster braucht mindestens 4 Punkte");
    profilePatternCandidate = "";
    return;
  }
  profileUsesPattern = true;
  profileLockHash = hashLock(profilePatternCandidate);
  profileConfigured = true;
  profilePatternCandidate = "";
  profilePatternEdit = false;
  saveConfig();
  showToast("Muster gespeichert");
}

void handleProfilePatternTouch(bool pressed, uint16_t x, uint16_t y) {
  int spacing = landscape ? 54 : 72;
  int top = contentTop() + (landscape ? 92 : 115);
  if (!pressed) {
    if (!touch.down) return;
    if (!touch.moved && touch.startX < 100 && touch.startY >= contentTop() + 50 && touch.startY < contentTop() + 82) {
      profilePatternEdit = false;
      profilePatternCandidate = "";
      profilePatternDrawing = false;
      touch.down = false;
      redrawRequested = true;
      return;
    }
    if (profilePatternDrawing) savePatternFromProfile();
    profilePatternDrawing = false;
    touch.down = false;
    redrawRequested = true;
    return;
  }
  if (!touch.down) {
    touch.down = true; touch.moved = false; touch.startX = touch.lastX = x; touch.startY = touch.lastY = y; touch.beganAt = millis();
    profilePatternCandidate = "";
    profilePatternDrawing = true;
  }
  touch.lastX = x; touch.lastY = y;
  addPatternNode(profilePatternCandidate, patternNodeAt(x, y, top, spacing));
  redrawRequested = true;
}

void handleLockTouch(bool pressed, uint16_t x, uint16_t y) {
  if (!pressed) {
    if (!touch.down) return;
    uint32_t held = millis() - touch.beganAt;
    if (profileUsesPattern && lockPatternDrawing) {
      lockPatternDrawing = false;
      if (lockPatternInput.length() >= 4) {
        if (!unlockWith(lockPatternInput)) lockPatternInput = "";
      } else {
        lockPatternInput = "";
        showToast("Mindestens 4 Punkte");
      }
    } else if (!touch.moved) {
      (void)held;
      // The PIN keypad is handled through the normal coordinate calculation.
      int padTop = contentTop() + (landscape ? 67 : 93);
      int gap = 8;
      int buttonW = min(58, (screenW() - gap * 4) / 3);
      int buttonH = landscape ? 28 : 38;
      int originX = (screenW() - (buttonW * 3 + gap * 2)) / 2;
      int selected = -1;
      for (uint8_t number = 0; number < 10; ++number) {
        int col = number == 9 ? 1 : number % 3;
        int row = number == 9 ? 3 : number / 3;
        int bx = originX + col * (buttonW + gap);
        int by = padTop + row * (buttonH + gap);
        if (touch.lastX >= bx && touch.lastX < bx + buttonW && touch.lastY >= by && touch.lastY < by + buttonH) selected = number;
      }
      int bottom = padTop + 3 * (buttonH + gap);
      if (selected >= 0 && lockPinInput.length() < 16) lockPinInput += String(selected);
      else if (touch.lastY >= bottom && touch.lastY < bottom + buttonH && touch.lastX >= originX && touch.lastX < originX + buttonW) {
        if (lockPinInput.length()) lockPinInput.remove(lockPinInput.length() - 1);
      } else if (touch.lastY >= bottom && touch.lastY < bottom + buttonH &&
                 touch.lastX >= originX + 2 * (buttonW + gap) && touch.lastX < originX + 2 * (buttonW + gap) + buttonW) {
        if (!unlockWith(lockPinInput)) lockPinInput = "";
      }
    }
    touch.down = false;
    redrawRequested = true;
    return;
  }
  if (!touch.down) {
    touch.down = true; touch.moved = false; touch.startX = touch.lastX = x; touch.startY = touch.lastY = y; touch.beganAt = millis();
    if (profileUsesPattern) {
      lockPatternInput = "";
      lockPatternDrawing = true;
    }
  }
  touch.lastX = x; touch.lastY = y;
  if (abs(static_cast<int>(x) - touch.startX) > 7 || abs(static_cast<int>(y) - touch.startY) > 7) touch.moved = true;
  if (profileUsesPattern) {
    int spacing = landscape ? 48 : 62;
    int top = contentTop() + (landscape ? 52 : 80);
    addPatternNode(lockPatternInput, patternNodeAt(x, y, top, spacing));
    redrawRequested = true;
  }
}

float maximumScroll() {
  int visible = 1;
  switch (screen) {
    case SC_FILES: {
      int actionH = landscape ? 32 : 38;
      visible = max(1, (contentHeight() - actionH - 9) / (landscape ? 31 : 40));
      return max(0, static_cast<int>(fileCount) - visible);
    }
    case SC_SETTINGS:
      visible = max(1, (contentHeight() - 8) / (landscape ? 34 : 43));
      return max(0, 13 - visible);
    case SC_GALLERY: {
      if (galleryDetail) return 0;
      int columns = landscape ? 5 : 3;
      int tileH = landscape ? 62 : 86;
      int rows = (photoCount + columns - 1) / columns;
      visible = max(1, (contentHeight() - 10) / (tileH + 8));
      return max(0, rows - visible);
    }
    case SC_MUSIC: {
      int topUsed = landscape ? 63 : 84;
      visible = max(1, (contentHeight() - topUsed) / (landscape ? 28 : 36));
      return max(0, static_cast<int>(musicCount) - visible);
    }
    case SC_MESSAGES: {
      if (!messagesLoggedIn || !messagesRecipient.length() || messageBubbleCount == 0) return 0;
      const int bubbleH = landscape ? 25 : 31;
      const int bubbleGap = landscape ? 5 : 7;
      const int bubbleTop = contentTop() + (landscape ? 64 : 74) + (landscape ? 30 : 38) + 8;
      const int bubbleBottom = contentBottom() - (landscape ? 72 : 86);
      const int contentPixels = static_cast<int>(messageBubbleCount) * (bubbleH + bubbleGap);
      const int visiblePixels = max(1, bubbleBottom - bubbleTop);
      const int overflowPixels = max(0, contentPixels - visiblePixels);
      const int unit = landscape ? 20 : 22;
      return (overflowPixels + unit - 1) / unit;
    }
    case SC_APPS:
      visible = max(1, (contentHeight() - 8) / (landscape ? 34 : 43));
      return max(0, static_cast<int>(addonCount) - visible);
    case SC_BROWSER: {
      int linksH = min(static_cast<int>(browserLinkCount) * (landscape ? 20 : 24) + (browserLinkCount ? 17 : 0), landscape ? 90 : 125);
      visible = max(1, (contentHeight() - linksH - 48) / (landscape ? 13 : 15));
      return max(0, browserTotalLines - visible);
    }
    case SC_EDITOR: {
      int controlsH = landscape ? 30 : 38;
      visible = max(1, (contentHeight() - controlsH - 15) / (landscape ? 13 : 15));
      return max(0, static_cast<int>(editorTotalLines) - visible);
    }
    default:
      return 0;
  }
}

float scrollPixelUnit() {
  switch (screen) {
    case SC_FILES: return landscape ? 31.0f : 40.0f;
    case SC_SETTINGS: return landscape ? 34.0f : 43.0f;
    case SC_GALLERY: return landscape ? 70.0f : 94.0f;
    case SC_MUSIC: return landscape ? 28.0f : 36.0f;
    case SC_MESSAGES: return landscape ? 20.0f : 22.0f;
    case SC_APPS: return landscape ? 34.0f : 43.0f;
    case SC_BROWSER: return landscape ? 13.0f : 15.0f;
    case SC_EDITOR: return landscape ? 13.0f : 15.0f;
    default: return 36.0f;
  }
}

bool canScrollThisScreen() {
  return maximumScroll() > 0.0f;
}

void moveToParentDirectory() {
  String path = currentDirectory;
  if (path == "/") return;
  while (path.endsWith("/") && path.length() > 1) path.remove(path.length() - 1);
  int slash = path.lastIndexOf('/');
  path = slash <= 0 ? "/" : path.substring(0, slash);
  loadDirectory(path.c_str());
}

void openFileItem(uint8_t index) {
  if (index >= fileCount) return;
  FileItem &item = fileItems[index];
  String path = item.path;
  if (item.directory) {
    loadDirectory(item.path);
    return;
  }
  if (isAddonFile(path)) {
    scanAddonApps();
    for (uint8_t i = 0; i < addonCount; ++i) {
      if (path == addonApps[i].path) {
        activeAddon = i;
        appVmLoad(addonApps[i].path);
        openScreen(SC_APP_RUNNER);
        return;
      }
    }
    showToast("App nicht gefunden");
  } else if (isTextFile(path)) {
    openTextFile(item.path);
  } else if (isPictureFile(path)) {
    safeCopy(selectedPicture, sizeof(selectedPicture), path);
    galleryDetail = true;
    openScreen(SC_GALLERY);
  } else if (isMusicFile(path)) {
    safeCopy(selectedMusic, sizeof(selectedMusic), path);
    musicPlaying = false;
    openScreen(SC_MUSIC);
    if (!startMp3Playback(selectedMusic)) showToast("MP3 konnte nicht gestartet werden");
  } else {
    showToast("Dateityp nicht unterstuetzt");
  }
}

void requestDeleteFile(uint8_t index) {
  if (index >= fileCount) return;
  safeCopy(deleteCandidate, sizeof(deleteCandidate), fileItems[index].path);
  deleteConfirmVisible = true;
  redrawRequested = true;
}

void handleDeleteDialogTap(int x, int y) {
  const int width = min(290, screenW() - 24);
  const int height = 104;
  const int left = (screenW() - width) / 2;
  const int top = (screenH() - height) / 2;
  if (y >= top + 60 && y < top + 90 && x >= left + 24 + (width - 36) / 2) {
    File target = SD.open(deleteCandidate);
    bool directory = target && target.isDirectory();
    if (target) target.close();
    bool removed = directory ? SD.rmdir(deleteCandidate) : SD.remove(deleteCandidate);
    showToast(removed ? "Entfernt" : "Ordner ist nicht leer oder Fehler");
    loadDirectory(currentDirectory);
  }
  deleteConfirmVisible = false;
  redrawRequested = true;
}

void tapQuickPanel(int x, int y) {
  const int panelH = landscape ? 170 : 224;
  const int panelY = -panelH + static_cast<int>(panelH * quickProgress);
  const int top = panelY + (landscape ? 33 : 42);
  const int gap = 8;
  const int tileW = (screenW() - gap * 3) / 2;
  const int tileH = landscape ? 34 : 48;
  if (y < panelY || y > panelY + panelH) {
    quickTarget = 0;
    redrawRequested = true;
    return;
  }
  int column = (x - gap) / (tileW + gap);
  int row = (y - top) / (tileH + gap);
  if (column < 0 || column > 1 || row < 0 || row > 2) return;
  int index = row * 2 + column;
  if (index == 0) setWifiEnabled(!wifiEnabled);
  else if (index == 1) {
    brightnessPercent = brightnessPercent >= 100 ? 20 : brightnessPercent + 10;
    applyBrightness(); saveConfig(); showToast("Helligkeit " + String(brightnessPercent) + "%");
  } else if (index == 2) {
    themeIndex = (themeIndex + 1) % 4; setTheme(); saveConfig();
  } else if (index == 3) {
    landscape = !landscape; applyOrientation(); saveConfig();
  } else if (index == 4) {
    lockDevice();
  } else if (index == 5) {
    if (loadBrowserCache()) { openScreen(SC_BROWSER); showToast("Browser-Cache geoeffnet"); }
    else showToast("Kein Browser-Cache");
  }
  quickTarget = 0;
  redrawRequested = true;
}



void handleHomeTap(int x, int y) {
  const int columns = landscape ? 5 : 3;
  const int gap = landscape ? 9 : 12;
  const int startY = statusHeight() + (landscape ? 18 : 26);
  const int top = startY + (landscape ? 40 : 43);
  const int tileW = (screenW() - gap * (columns + 1)) / columns;
  const int tileH = landscape ? 70 : 68;
  int column = (x - gap) / (tileW + gap);
  int row = (y - top) / (tileH + gap);
  if (column < 0 || column >= columns || row < 0) return;
  int index = row * columns + column;
  if (index < 0 || index >= HOME_APP_COUNT) return;
  int tx = gap + column * (tileW + gap), ty = top + row * (tileH + gap);
  if (x < tx || y < ty || x > tx + tileW || y > ty + tileH) return;
  switch (index) {
    case 0: loadDirectory("/"); openScreen(SC_FILES); break;
    case 1: scanMediaDirectory(ROOT_PICTURES, photoItems, photoCount, true); galleryDetail = false; openScreen(SC_GALLERY); break;
    case 2: scanMediaDirectory(ROOT_MUSIC, musicItems, musicCount, false); openScreen(SC_MUSIC); break;
    case 3: openScreen(SC_BROWSER); break;
    case 4: openScreen(SC_MESSAGES); break;
    case 5: chooseDefaultEditorFile(); break;
    case 6: openScreen(SC_SETTINGS); break;
    case 7: openScreen(SC_PROFILE); break;
    case 8: scanAddonApps(); openScreen(SC_APPS); break;
  }
}

void handleSettingsTap(int x, int y) {
  const int rowH = landscape ? 34 : 43;
  const int start = contentTop() + 5;
  if (y < start || y >= contentBottom()) return;
  int index = static_cast<int>(scrollPosition) + (y - start) / rowH;
  if (index < 0 || index > 12) return;
  if (index == 0) {
    themeIndex = (themeIndex + 1) % 4; setTheme(); saveConfig();
  } else if (index == 1) {
    wallpaperIndex = (wallpaperIndex + 1) % 4; saveConfig();
  } else if (index == 2) {
    int sliderX = screenW() / 2 - 6;
    int sliderW = screenW() / 2 - 32;
    if (x >= sliderX) brightnessPercent = constrain((x - sliderX) * 100 / max(1, sliderW), 10, 100);
    else brightnessPercent = brightnessPercent >= 100 ? 20 : brightnessPercent + 10;
    applyBrightness(); saveConfig();
  } else if (index == 3) {
    landscape = !landscape; applyOrientation(); saveConfig();
  } else if (index == 4) {
    openScreen(SC_WIFI);
  } else if (index == 5) {
    openScreen(SC_PROFILE);
  } else if (index == 6) {
    if (storageReady() && SD.exists(BROWSER_CACHE_FILE)) {
      SD.remove(BROWSER_CACHE_FILE);
      showToast("Browser-Cache entfernt");
    } else showToast("Kein Browser-Cache");
  } else if (index == 7) {
    runTouchCalibration();
  } else if (index == 8) {
    sleepTimeoutSeconds = sleepTimeoutSeconds == 0 ? 10 : sleepTimeoutSeconds == 10 ? 30 : sleepTimeoutSeconds == 30 ? 60 : 0;
    saveConfig();
    showToast(sleepTimeoutSeconds == 0 ? "Automatischer Schlaf aus" : "Schlaf: " + String(sleepTimeoutSeconds) + " s");
  } else if (index == 9) {
    enterUltraPowerSave();
  } else if (index == 10) {
    enterDeepSleep();
  } else if (index == 11) {
    String info = String("Heap ") + humanBytes(ESP.getFreeHeap());
    if (storageReady()) info += " | SD " + humanBytes(SD.totalBytes() - SD.usedBytes()) + " frei";
    showToast(info, 3500);
  } else if (index == 12) {
    clearMessagesCredentials();
  }
  redrawRequested = true;
}

void handleWifiTap(int x, int y) {
  int base = contentTop() + 12;
  if (y >= base + 17 && y < base + 53) {
    beginKeyboard(wifiSsid, KB_WIFI_SSID);
    return;
  }
  base += landscape ? 63 : 78;
  if (y >= base + 17 && y < base + 53) {
    beginKeyboard(wifiPassword, KB_WIFI_PASSWORD);
    return;
  }
  base += landscape ? 62 : 76;
  int buttonW = (screenW() - 38) / 2;
  if (y >= base && y < base + 38) {
    if (x < 12 + buttonW) {
      if (!wifiEnabled) setWifiEnabled(true);
      beginWifiConnection();
    } else if (x >= 26 + buttonW) {
      setWifiEnabled(!wifiEnabled);
    }
  }
}

void handleFilesTap(int x, int y, uint32_t held) {
  if (!storageReady()) return;
  const int start = contentTop() + 4;
  const int actionH = landscape ? 32 : 38;
  const int actionY = contentBottom() - actionH;
  if (y >= actionY) {
    int bw = (screenW() - 28) / 3;
    if (x < 7 + bw) moveToParentDirectory();
    else if (x < 14 + bw * 2) beginKeyboard("", KB_NEW_FOLDER);
    else beginKeyboard("Neue Datei.txt", KB_NEW_FILE);
    return;
  }
  if (y < start || y >= actionY) return;
  const int rowH = landscape ? 31 : 40;
  int index = static_cast<int>(scrollPosition + (y - start) / static_cast<float>(rowH));
  if (index < 0 || index >= fileCount) return;
  if (held >= 700) requestDeleteFile(index);
  else openFileItem(index);
}

void handleGalleryTap(int x, int y) {
  if (galleryDetail) return;
  int columns = landscape ? 5 : 3;
  int gap = 8;
  int tileW = (screenW() - gap * (columns + 1)) / columns;
  int tileH = landscape ? 62 : 86;
  int start = contentTop() + 8;
  if (y < start || y >= contentBottom()) return;
  int column = (x - gap) / (tileW + gap);
  int row = static_cast<int>(scrollPosition + (y - start) / static_cast<float>(tileH + gap));
  int index = row * columns + column;
  if (column < 0 || column >= columns || index < 0 || index >= photoCount) return;
  int px = gap + column * (tileW + gap);
  int py = start + static_cast<int>((row - scrollPosition) * (tileH + gap));
  if (x < px || y < py || x > px + tileW || y > py + tileH) return;
  safeCopy(selectedPicture, sizeof(selectedPicture), photoItems[index].path);
  galleryDetail = true;
  redrawRequested = true;
}

void handleMusicTap(int x, int y) {
  const int top = contentTop() + 7;
  const int listTop = top + (landscape ? 63 : 84);
  const int rowH = landscape ? 28 : 36;
  if (y < listTop || y >= contentBottom()) return;
  int index = static_cast<int>(scrollPosition + (y - listTop) / static_cast<float>(rowH));
  if (index < 0 || index >= musicCount) return;
  if (String(selectedMusic) == String(musicItems[index].path) && musicPlaying) {
    stopMp3Playback();
  } else {
    safeCopy(selectedMusic, sizeof(selectedMusic), musicItems[index].path);
    if (!startMp3Playback(selectedMusic)) showToast("MP3 konnte nicht gestartet werden");
  }
  redrawRequested = true;
}

void handleEditorTap(int x, int y) {
  const int controlsH = landscape ? 30 : 38;
  const int controlsY = contentBottom() - controlsH;
  if (y < controlsY) {
    beginKeyboard(editorText, KB_EDITOR);
    return;
  }
  int half = (screenW() - 24) / 2;
  if (x < 8 + half) beginKeyboard(editorText, KB_EDITOR);
  else saveEditorFile();
}



void handleBrowserTap(int x, int y) {
  const int top = contentTop() + 5;
  int buttonW = landscape ? 48 : 58;
  if (y >= top && y < top + 31) {
    if (x < screenW() - buttonW - 4) beginKeyboard(browserUrl, KB_BROWSER_URL);
    else fetchBrowserPage();
    return;
  }
  int linksH = min(static_cast<int>(browserLinkCount) * (landscape ? 20 : 24) + (browserLinkCount ? 17 : 0), landscape ? 90 : 125);
  if (browserLinkCount && y >= contentBottom() - linksH + 13) {
    int rowH = landscape ? 19 : 23;
    int index = (y - (contentBottom() - linksH + 13)) / rowH;
    if (index >= 0 && index < browserLinkCount) {
      browserUrl = browserLinks[index];
      fetchBrowserPage();
    }
  }
}

void handleProfileTap(int x, int y) {
  if (profilePatternEdit) return;
  int base = contentTop() + 12 + (landscape ? 68 : 88);
  int rowH = landscape ? 33 : 42;
  if (y < base || y >= base + rowH * 4) return;
  int index = (y - base) / rowH;
  if (index < 0 || index > 3) return;
  if (index == 0) {
    beginKeyboard(profileName, KB_PROFILE_NAME);
  } else if (index == 1) {
    profileUsesPattern = !profileUsesPattern;
    profileConfigured = false;
    profileLockHash = "";
    saveConfig();
    showToast(profileUsesPattern ? "Jetzt Muster setzen" : "Jetzt PIN setzen");
  } else if (index == 2) {
    if (profileUsesPattern) {
      profilePatternEdit = true;
      profilePatternCandidate = "";
    } else beginKeyboard("", KB_PROFILE_PIN, true);
  } else {
    lockDevice();
  }
  redrawRequested = true;
}

void handleAppsTap(int x, int y) {
  if (y >= contentBottom() - 36) { openAppStore(); return; }
  int rowH = landscape ? 34 : 43;
  int start = contentTop() + 7;
  if (y < start || y >= contentBottom()) return;
  int index = static_cast<int>(scrollPosition + (y - start) / static_cast<float>(rowH));
  if (index < 0 || index >= addonCount) return;
  activeAddon = index;
  appInput = "";
  appRuntimeMessage = "";
  appVmLoad(addonApps[index].path);
  openScreen(SC_APP_RUNNER);
}

void handleAppStoreTap(int x, int y) {
  const int rowH = landscape ? 42 : 52;
  int index = (y - contentTop() - 6) / rowH;
  if (index >= 0 && index < storeCount) downloadStoreItem(index);
}

void handleRunnerTap(int x, int y) {
  for (uint8_t i = 0; i < appButtonCount; ++i) {
    AppButton &button = appButtons[i];
    if (x >= button.x && x < button.x + button.w && y >= button.y && y < button.y + button.h) {
      executeAddonAction(button.action);
      return;
    }
  }
}

void handleRecentsTap(int x, int y) {
  const int columns = landscape ? 3 : 2;
  const int gap = 10;
  const int cardW = (screenW() - gap * (columns + 1)) / columns;
  const int cardH = landscape ? 67 : 96;
  int column = (x - gap) / (cardW + gap);
  int row = (y - (contentTop() + 10)) / (cardH + gap);
  int index = row * columns + column;
  if (column < 0 || column >= columns || index < 0 || index >= recentCount) return;
  int px = gap + column * (cardW + gap);
  int py = contentTop() + 10 + row * (cardH + gap);
  if (x < px || y < py || x > px + cardW || y > py + cardH) return;
  openScreen(recentScreens[index], false);
}

void processTap(int x, int y, uint32_t held) {
  if (deleteConfirmVisible) {
    handleDeleteDialogTap(x, y);
    return;
  }
  if (keyboardOpen) {
    keyboardTap(x, y);
    return;
  }
  if (quickProgress > 0.05f || quickTarget > 0.0f) {
    tapQuickPanel(x, y);
    return;
  }
  if (screen != SC_LOCK && y < statusHeight()) {
    quickOpen = true;
    quickTarget = 1.0f;
    if (!quickOverlayActive) beginQuickOverlay();
    redrawRequested = true;
    return;
  }
  if (screen != SC_HOME && screen != SC_LOCK && y >= statusHeight() && y < contentTop() && x < 36) {
    if (screen == SC_GALLERY && galleryDetail) {
      galleryDetail = false;
      redrawRequested = true;
    } else goBack();
    return;
  }
  if (screen != SC_LOCK && y >= screenH() - navHeight()) {
    int third = screenW() / 3;
    if (x < third) goHome();
    else if (x < third * 2) openRecents();
    else goBack();
    return;
  }
  switch (screen) {
    case SC_HOME: handleHomeTap(x, y); break;
    case SC_RECENTS: handleRecentsTap(x, y); break;
    case SC_SETTINGS: handleSettingsTap(x, y); break;
    case SC_WIFI: handleWifiTap(x, y); break;
    case SC_FILES: handleFilesTap(x, y, held); break;
    case SC_GALLERY: handleGalleryTap(x, y); break;
    case SC_MUSIC: handleMusicTap(x, y); break;
    case SC_MESSAGES: handleMessagesTap(x, y); break;
    case SC_EDITOR: handleEditorTap(x, y); break;
    case SC_BROWSER: handleBrowserTap(x, y); break;
    case SC_PROFILE: handleProfileTap(x, y); break;
    case SC_APPS: handleAppsTap(x, y); break;
    case SC_APP_STORE: handleAppStoreTap(x, y); break;
    case SC_APP_RUNNER: handleRunnerTap(x, y); break;
    default: break;
  }
  redrawRequested = true;
}

void handleTouch(bool pressed, uint16_t x, uint16_t y) {
  if (pressed) lastActivityAt = millis();
  if (screen == SC_LOCK) {
    handleLockTouch(pressed, x, y);
    return;
  }
  if (screen == SC_PROFILE && profilePatternEdit) {
    handleProfilePatternTouch(pressed, x, y);
    return;
  }
  if (!pressed) {
    if (!touch.down) return;
    uint32_t held = millis() - touch.beganAt;
    if (!touch.moved) processTap(touch.lastX, touch.lastY, held);
    touch.down = false;
    return;
  }
  if (!touch.down) {
    touch.down = true;
    touch.moved = false;
    touch.startX = touch.lastX = x;
    touch.startY = touch.lastY = y;
    touch.beganAt = millis();
    touchStartScroll = scrollTarget;
  }
  uint16_t previousX = touch.lastX;
  uint16_t previousY = touch.lastY;
  touch.lastX = x;
  touch.lastY = y;
  int dx = static_cast<int>(x) - touch.startX;
  int dy = static_cast<int>(y) - touch.startY;
  if (abs(dx) > 7 || abs(dy) > 7) touch.moved = true;
  if (touch.startY < statusHeight() + 5 && dy > 22) {
    quickOpen = true;
    quickTarget = 1.0f;
    if (!quickOverlayActive) beginQuickOverlay();
    touch.moved = true;
    redrawRequested = true;
    return;
  }

  if (touch.moved && canScrollThisScreen()) {
    float candidate = touchStartScroll + static_cast<float>(touch.startY - y) / scrollPixelUnit();
    float maximum = maximumScroll();
    if (candidate < 0) candidate = 0;
    if (candidate > maximum) candidate = maximum;
    scrollTarget = candidate;
    redrawRequested = true;
  }
}

// ---------------------------------------------------------------------------
// Boot and cooperative main loop
// ---------------------------------------------------------------------------
void drawBootScreen() {
  tft.fillScreen(TFT_BLACK);
  drawCentered("PocketOS", screenW() / 2, screenH() / 2 - 24, 3, TFT_WHITE);
  drawCentered("ESP32 Touch Environment", screenW() / 2, screenH() / 2 + 7, 1, TFT_LIGHTGREY);
  tft.fillRoundRect(screenW() / 2 - 80, screenH() / 2 + 29, 160, 7, 4, TFT_DARKGREY);
  tft.fillRoundRect(screenW() / 2 - 80, screenH() / 2 + 29, 112, 7, 4, TFT_CYAN);
  delay(350);
}

void updateAnimations() {
  bool changed = false;
  partialRegion = PARTIAL_NONE;
  uint8_t previousBattery = batteryPercent;
  updateBatteryStatus();
  if (batteryPercent != previousBattery && partialRegion == PARTIAL_NONE) {
    partialFrameRequested = true;
    partialRegion = PARTIAL_STATUS;
    changed = true;
  }
  appVmRunTimers();
  if (screen == SC_APP_RUNNER && appVmStateDirty) changed = true;
  if (screen == SC_MESSAGES && messagesLoggedIn && millis() - lastMessagesPollAt > 15000UL) { pollMessages(); changed = true; }
  float scrollDifference = scrollTarget - scrollPosition;
  if (abs(scrollDifference) > 0.02f) {
    scrollPosition += scrollDifference * 0.28f;
    changed = true;
  } else if (scrollPosition != scrollTarget) {
    scrollPosition = scrollTarget;
    changed = true;
  }
  float quickDifference = quickTarget - quickProgress;
  if (abs(quickDifference) > 0.015f) {
    quickProgress += quickDifference * 0.30f;
    changed = true;
  } else if (quickProgress != quickTarget) {
    quickProgress = quickTarget;
    changed = true;
  }
  if (quickProgress <= 0.01f && quickTarget <= 0.01f) {
    quickOpen = false;
    if (quickOverlayActive) {
      quickOverlayActive = false;
      changed = true;
    }
  }
  if (toastText.length() && static_cast<int32_t>(millis() - toastUntil) >= 0) {
    toastText = "";
    changed = true;
  }
  // Die Statusleiste zeigt nur Minuten; ein sekündlicher Voll-Refresh war daher
  // unnötig und besonders bei statischen SD-Apps sichtbar belastend.
  if (millis() - lastClockAt > 60000UL) {
    lastClockAt = millis();
    if (partialRegion == PARTIAL_NONE) { partialFrameRequested = true; partialRegion = PARTIAL_STATUS; }
    changed = true;
  }
  if (changed) redrawRequested = true;
}

void setup() {
  Serial.begin(115200);
  delay(50);
  Serial.println("[PocketOS] Start...");
  pinMode(AUDIO_ENABLE_PIN, OUTPUT);
  digitalWrite(AUDIO_ENABLE_PIN, HIGH);
  pinMode(RGB_RED_PIN, OUTPUT); digitalWrite(RGB_RED_PIN, HIGH);
  pinMode(RGB_GREEN_PIN, OUTPUT); digitalWrite(RGB_GREEN_PIN, HIGH);
  pinMode(RGB_BLUE_PIN, OUTPUT); digitalWrite(RGB_BLUE_PIN, HIGH);
  pinMode(BATTERY_ADC_PIN, INPUT);
  pinMode(TOUCH_IRQ_PIN, INPUT);
  analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_11db);
  updateBatteryStatus();

  // Erst Display und Bootscreen initialisieren, damit ein langsames oder
  // fehlendes SD-Modul den Start nicht unsichtbar blockieren kann.
  tft.init();
  tft.setRotation(0);  // Portrait is the calibration/base orientation.
  tft.setTextFont(1);
  drawBootScreen();

  // Kein automatisches Formatieren beim Boot: Nach einem Flash-Löschen kann
  // begin(true) minutenlang im Formatierer hängen und den Bootscreen einfrieren.
  // PocketOS läuft ohne persistente Einstellungen weiter; Formatierung kann
  // später gezielt über die Systemverwaltung erfolgen.
  spiffsMounted = SPIFFS.begin(false);
  Serial.println(spiffsMounted ? "[PocketOS] SPIFFS bereit." : "[PocketOS] SPIFFS nicht verfuegbar - starte ohne Cache.");
  loadConfig();
  setTheme();
  applyBrightness();
  calibrateTouchIfNeeded();  // Stores sizeof(uint16_t[5]), never an unsafe size.
  applyOrientation();
  TJpgDec.setCallback(jpegOutput);
  TJpgDec.setSwapBytes(true);

  // SD erst nach Display, SPIFFS und Orientierung starten. Audio ist in diesem
  // A/B-Test vollständig aus dem Sketch entfernt.
  Serial.println("[PocketOS] SD-Initialisierung...");
  initialiseStorage();
  Serial.println(sdMounted ? "[PocketOS] SD bereit." : "[PocketOS] Keine SD-Karte - starte weiter.");

  if (wifiEnabled) {
    WiFi.mode(WIFI_STA);
    if (wifiSsid.length()) WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
    configTime(3600, 3600, "pool.ntp.org", "time.nist.gov");
  } else {
    WiFi.mode(WIFI_OFF);
  }

  loadDirectory("/");
  scanMediaDirectory(ROOT_PICTURES, photoItems, photoCount, true);
  scanMediaDirectory(ROOT_MUSIC, musicItems, musicCount, false);
  scanAddonApps();
  screen = profileConfigured && profileLockHash.length() ? SC_LOCK : SC_HOME;
  lastActivityAt = millis();
  redrawRequested = true;
  redraw();
}

void loop() {
  // Resistive touch is deliberately sampled before any drawing or SD work.
  serviceMp3Playback();
  uint16_t x = 0, y = 0;
  bool pressed = tft.getTouch(&x, &y);
  handleTouch(pressed, x, y);
  serviceMp3Playback();
  serviceAutomaticSleep();

  updateAnimations();
  // Vollbild-Rendering auf dem kleinen TFT nicht schneller als ca. 30 FPS;
  // Touch-Samples bleiben davon ungedrosselt, Animationen wirken dadurch aber
  // ruhiger und erzeugen deutlich weniger sichtbares Flimmern.
  if (redrawRequested && millis() - lastFrameAt >= 33) {
    lastFrameAt = millis();
    redraw();
  }
  // Während MP3-Wiedergabe den Decoder nicht künstlich ausbremsen.
  if (!musicPlaying) delay(2);
}


// ---------------------------------------------------------------------------
// PocketOS generic SD App VM
// ---------------------------------------------------------------------------
int32_t appVmGet(const String &name, int32_t fallback) { for(uint8_t i=0;i<appVmVarCount;++i) if(name==appVmVars[i].name) return appVmVars[i].value; return fallback; }
void appVmSet(const String &name,int32_t value,bool persist){ for(uint8_t i=0;i<appVmVarCount;++i) if(name==appVmVars[i].name){appVmVars[i].value=value;appVmVars[i].dirty=true;appVmStateDirty|=persist;return;} if(appVmVarCount<APP_VM_CACHE){safeCopy(appVmVars[appVmVarCount].name,sizeof(appVmVars[appVmVarCount].name),name);appVmVars[appVmVarCount].value=value;appVmVars[appVmVarCount].dirty=false;++appVmVarCount;} }
String appVmField(const String &v,uint8_t i){return csvField(v,i);}
void appVmSaveState(){if(!storageReady()||!appVmStatePath.length()||!appVmStateDirty)return;File f=SD.open(appVmStatePath.c_str(),"w");if(!f)return;for(uint8_t i=0;i<appVmVarCount;++i)f.printf("%s=%ld\n",appVmVars[i].name,(long)appVmVars[i].value);f.close();appVmStateDirty=false;}
void appVmLoadState(){if(!storageReady()||!SD.exists(appVmStatePath))return;File f=SD.open(appVmStatePath.c_str(),"r");if(!f)return;while(f.available()){String l=f.readStringUntil('\n');l.trim();int e=l.indexOf('=');if(e>0)appVmSet(l.substring(0,e),l.substring(e+1).toInt(),false);}f.close();}
void appVmReset(){appVmSaveState();appVmVarCount=0;appVmVertexCount=0;appVmEdgeCount=0;appVmTrailCount=0;appVmStateDirty=false;appVmLastTimer=millis();appVmLoaded=false;}
void appVmLoad(const char *path){appVmReset();String n=baseName(path);n.replace('.','_');appVmStatePath=String("/PocketOS/AppData/")+n+".state";if(!storageReady())return;File f=SD.open(path,"r");if(!f)return;while(f.available()){String l=f.readStringUntil('\n');l.trim();if(l.startsWith("var=")){String v=l.substring(4);appVmSet(appVmField(v,0),appVmField(v,1).toInt(),false);}}f.close();appVmLoadState();appVmLoaded=true;}
void appVmAction(const String &action){String v=action;v.trim();int sequence=v.indexOf(';');if(sequence>=0){appVmAction(v.substring(0,sequence));appVmAction(v.substring(sequence+1));return;}String c=lowerCopy(appVmField(v,0));if(c=="set")appVmSet(appVmField(v,1),appVmField(v,2).toInt(),true);else if(c=="add"){String a=appVmField(v,2);int32_t d=(a.length()&&((a[0]>='0'&&a[0]<='9')||a[0]=='-'))?a.toInt():appVmGet(a,0);appVmSet(appVmField(v,1),appVmGet(appVmField(v,1),0)+d,true);}else if(c=="toggle")appVmSet(appVmField(v,1),!appVmGet(appVmField(v,1),0),true);else if(c=="wrap"){String n=appVmField(v,1);int32_t x=appVmGet(n,0),m=appVmField(v,2).toInt();if(m>0){if(x<0)x=m-1;if(x>=m)x=0;appVmSet(n,x,true);}}else if(c=="random"){uint32_t seed=millis();int m=max(1, static_cast<int>(appVmField(v,2).toInt()));appVmSet(appVmField(v,1),(seed*1103515245UL+12345UL)%m,true);}else if(c=="trail"){String n=appVmField(v,1);uint8_t len=constrain(appVmField(v,2).toInt(),1,32);for(uint8_t j=len-1;j>0;--j){appVmSet(n+"_x"+String(j),appVmGet(n+"_x"+String(j-1),0),false);appVmSet(n+"_y"+String(j),appVmGet(n+"_y"+String(j-1),0),false);}appVmSet(n+"_x0",appVmGet(appVmField(v,3),0),false);appVmSet(n+"_y0",appVmGet(appVmField(v,4),0),false);appVmSet(n+"_length",len,false);}else if(c=="message")appRuntimeMessage=appVmField(v,1);else if(c=="reset")appVmLoad(addonApps[activeAddon].path);else if(c=="redraw")redrawRequested=true;else showToast("Unbekannter VM-Befehl");appVmStateDirty=true;redrawRequested=true;}
void appVmExecuteEvent(const String &name){if(!storageReady()||activeAddon<0||activeAddon>=addonCount)return;File f=SD.open(addonApps[activeAddon].path,"r");if(!f)return;while(f.available()){String l=f.readStringUntil('\n');l.trim();if(l.startsWith("event=")){String v=l.substring(6);if(appVmField(v,0)==name)appVmAction(v.substring(v.indexOf(',')+1));}else if(l.startsWith("event-if=")){String v=l.substring(9);if(appVmField(v,0)==name&&appVmGet(appVmField(v,1),0)==appVmGet(appVmField(v,2),0)&&appVmGet(appVmField(v,3),0)==appVmGet(appVmField(v,4),0)){int cut=-1;for(uint8_t ci=0;ci<5;++ci)cut=v.indexOf(',',cut+1);appVmAction(v.substring(cut+1));}}}f.close();appVmSaveState();}
void appVmRunTimers(){if(!appVmLoaded||screen!=SC_APP_RUNNER||activeAddon<0||activeAddon>=addonCount)return;static uint32_t marks[APP_VM_CACHE]={};File f=SD.open(addonApps[activeAddon].path,"r");if(!f)return;while(f.available()){String l=f.readStringUntil('\n');l.trim();if(!l.startsWith("timer="))continue;String v=l.substring(6),event=v.substring(v.indexOf(',')+1);uint8_t h=0;for(uint8_t i=0;i<event.length();++i)h=h*33+event[i];h%=APP_VM_CACHE;uint32_t interval=max(16, static_cast<int>(v.substring(0,v.indexOf(',')).toInt()));if(millis()-marks[h]>=interval){marks[h]=millis();f.close();appVmExecuteEvent(event);return;}}f.close();}
void appVmRender(){if(!appVmLoaded||activeAddon<0||activeAddon>=addonCount)return;drawAppFrame(addonApps[activeAddon].title,"SD-App VM");appButtonCount=0;appVmVertexCount=0;appVmEdgeCount=0;appVmTrailCount=0;int trailCell=0,trailX=0,trailY=0;File f=SD.open(addonApps[activeAddon].path,"r");if(!f)return;while(f.available()){String l=f.readStringUntil('\n');l.trim();if(l.startsWith("label=")){String v=l.substring(6);int x=appVmField(v,0).toInt();drawWrapped(appVmField(v,3),x,contentTop()+appVmField(v,1).toInt(),screenW()-x-8,15,3,colors.text,constrain(appVmField(v,2).toInt(),1,2));}else if(l.startsWith("value=")){String v=l.substring(6);int vx=appVmField(v,0).toInt(),vy=contentTop()+appVmField(v,1).toInt(),vs=constrain(appVmField(v,2).toInt(),1,2);String vv=appVmField(v,3);String prefix=appVmField(v,4);drawText(prefix+String(appVmGet(vv,0)),vx,vy,vs,colors.accent2);
}else if(l.startsWith("input=")){String v=l.substring(6);int ix=appVmField(v,0).toInt(),iy=contentTop()+appVmField(v,1).toInt(),iw=appVmField(v,2).toInt(),ih=appVmField(v,3).toInt();drawRoundCard(ix,iy,iw,ih,colors.card,colors.cardAlt);drawText(appInput.length()?appInput:appVmField(v,4),ix+8,iy+ih/2-7,1,appInput.length()?colors.text:colors.muted);}
else if(l.startsWith("button=")){String v=l.substring(7);int comma=-1;for(uint8_t i=0;i<5;++i)comma=v.indexOf(',',comma+1);addRuntimeButton(appVmField(v,0).toInt(),contentTop()+appVmField(v,1).toInt(),appVmField(v,2).toInt(),appVmField(v,3).toInt(),appVmField(v,4),comma>=0?v.substring(comma+1):String(),colors.accent);}else if(l.startsWith("grid=")){String v=l.substring(5);int x=appVmField(v,0).toInt(),y=contentTop()+appVmField(v,1).toInt(),cols=appVmField(v,2).toInt(),rows=appVmField(v,3).toInt(),cell=appVmField(v,4).toInt();for(int gy=0;gy<rows;++gy)for(int gx=0;gx<cols;++gx)tft.drawRect(x+gx*cell,y+gy*cell,cell,cell,colors.cardAlt);}else if(l.startsWith("cell=")){String v=l.substring(5);int x=appVmGet(appVmField(v,1),0),y=appVmGet(appVmField(v,2),0),cell=appVmField(v,3).toInt();tft.fillRect(appVmField(v,4).toInt()+x*cell+1,contentTop()+appVmField(v,5).toInt()+y*cell+1,cell-2,cell-2,colors.accent2);}else if(l.startsWith("trail=")){String v=l.substring(6);if(appVmTrailCount<4){safeCopy(appVmTrails[appVmTrailCount].name,sizeof(appVmTrails[appVmTrailCount].name),appVmField(v,0));appVmTrails[appVmTrailCount].length=constrain(appVmGet(appVmField(v,1),1),1,32);trailCell=appVmField(v,2).toInt();trailX=appVmField(v,3).toInt();trailY=appVmField(v,4).toInt();++appVmTrailCount;}}else if(l.startsWith("vertex=")){String v=l.substring(7);if(appVmVertexCount<APP_VM_VERTICES)appVmVertices[appVmVertexCount++]={static_cast<int16_t>(appVmField(v,1).toInt()),static_cast<int16_t>(appVmField(v,2).toInt()),static_cast<int16_t>(appVmField(v,3).toInt())};}else if(l.startsWith("edge=")){String v=l.substring(5);if(appVmEdgeCount<APP_VM_EDGES)appVmEdges[appVmEdgeCount++]={static_cast<uint8_t>(appVmField(v,0).toInt()),static_cast<uint8_t>(appVmField(v,1).toInt())};}}f.close();for(uint8_t ti=0;ti<appVmTrailCount;++ti)for(uint8_t j=0;j<appVmTrails[ti].length;++j){appVmTrails[ti].x[j]=constrain(appVmGet(String(appVmTrails[ti].name)+"_x"+String(j),appVmGet("head_x",0)),0,31);appVmTrails[ti].y[j]=constrain(appVmGet(String(appVmTrails[ti].name)+"_y"+String(j),appVmGet("head_y",0)),0,31);tft.fillRect(trailX+appVmTrails[ti].x[j]*trailCell+1,contentTop()+trailY+appVmTrails[ti].y[j]*trailCell+1,trailCell-2,trailCell-2,j?colors.accent2:colors.accent);}float rx=appVmGet("rot_x",0)*0.0174532925f,ry=appVmGet("rot_y",0)*0.0174532925f,sx=sinf(rx),ax=cosf(rx),sy=sinf(ry),ay=cosf(ry);int16_t pr[APP_VM_VERTICES][2];int cx=screenW()/2,cy=contentTop()+contentHeight()/2-20;for(uint8_t i=0;i<appVmVertexCount;++i){float x=appVmVertices[i].x,y=appVmVertices[i].y,z=appVmVertices[i].z,tx=ay*x+sy*z,tz=-sy*x+ay*z,ty=ax*y-sx*tz;pr[i][0]=cx+static_cast<int16_t>(tx*60);pr[i][1]=cy+static_cast<int16_t>(ty*60);}for(uint8_t i=0;i<appVmEdgeCount;++i)if(appVmEdges[i].a<appVmVertexCount&&appVmEdges[i].b<appVmVertexCount)tft.drawLine(pr[appVmEdges[i].a][0],pr[appVmEdges[i].a][1],pr[appVmEdges[i].b][0],pr[appVmEdges[i].b][1],colors.accent2);if(appRuntimeMessage.length())drawCentered(appRuntimeMessage,screenW()/2,contentBottom()-12,1,colors.muted);}
void drawAddonRunner(){appVmRender();}

uint8_t uiFontForSize(uint8_t size) { if (size >= 3) return 4; if (size == 2) return 2; return 1; }

// Restored top-level PocketOS UI/system functions from the last stable sketch.
// ---------------------------------------------------------------------------
// Basic helpers
// ---------------------------------------------------------------------------
int screenW()
{ return tft.width(); }

int screenH()
{ return tft.height(); }

int statusHeight()
{ return landscape ? 22 : 26; }

int headerHeight()
{ return landscape ? 30 : 40; }

int navHeight()
{ return landscape ? 36 : 44; }

int contentTop()
{ return statusHeight() + headerHeight(); }

int contentBottom()
{ return screenH() - navHeight(); }

int contentHeight()
{ return contentBottom() - contentTop(); }

void safeCopy(char *destination, size_t destinationSize, const String &source)
{
  if (destinationSize == 0) return;
  size_t copyCount = source.length();
  if (copyCount >= destinationSize) copyCount = destinationSize - 1;
  memcpy(destination, source.c_str(), copyCount);
  destination[copyCount] = '\0';
}

String lowerCopy(String value)
{
  value.toLowerCase();
  return value;
}

String baseName(const String &path)
{
  int slash = path.lastIndexOf('/');
  return slash >= 0 ? path.substring(slash + 1) : path;
}

bool hasExtension(const String &path, const char *const extension)
{
  return lowerCopy(path).endsWith(extension);
}

bool isTextFile(const String &path)
{
  String value = lowerCopy(path);
  return value.endsWith(".txt") || value.endsWith(".md") || value.endsWith(".log") ||
         value.endsWith(".cfg") || value.endsWith(".ini") || value.endsWith(".csv") ||
         value.endsWith(".papp") || value.endsWith(".app");
}

bool isPictureFile(const String &path)
{
  String value = lowerCopy(path);
  return value.endsWith(".jpg") || value.endsWith(".jpeg") || value.endsWith(".bmp");
}

bool isMusicFile(const String &path)
{
  String value = lowerCopy(path);
  return value.endsWith(".mp3") || value.endsWith(".wav") || value.endsWith(".ogg");
}

bool isAddonFile(const String &path)
{
  String value = lowerCopy(path);
  return value.endsWith(".papp") || value.endsWith(".app");
}

String humanBytes(uint32_t value)
{
  if (value < 1024) return String(value) + " B";
  if (value < 1024UL * 1024UL) return String(value / 1024UL) + " KB";
  return String(value / (1024UL * 1024UL)) + " MB";
}

String hashLock(const String &secret)
{
  // A salted FNV-1a hash avoids leaving the PIN/pattern readable in SPIFFS.
  uint32_t hash = 2166136261UL;
  const String material = String("PocketOS-v1|") + secret + "|ESP32";
  for (size_t i = 0; i < material.length(); ++i) {
    hash ^= static_cast<uint8_t>(material[i]);
    hash *= 16777619UL;
  }
  return String(hash, HEX);
}

void showToast(const String &message, uint32_t duration)
{
  toastText = message;
  toastUntil = millis() + duration;
  redrawRequested = true;
}

void setTheme()
{
  switch (themeIndex % 4) {
    case 1: // Paper
      colors = {tft.color565(238, 242, 248), TFT_WHITE, tft.color565(220, 228, 240),
                tft.color565(205, 216, 234), tft.color565(24, 34, 52),
                tft.color565(92, 105, 126), tft.color565(52, 104, 220),
                tft.color565(36, 163, 126), tft.color565(210, 55, 74),
                tft.color565(210, 224, 255), tft.color565(237, 245, 255)};
      break;
    case 2: // Forest
      colors = {tft.color565(8, 28, 26), tft.color565(14, 49, 43), tft.color565(20, 68, 58),
                tft.color565(25, 87, 73), TFT_WHITE, tft.color565(163, 210, 196),
                tft.color565(37, 185, 137), tft.color565(109, 220, 169),
                tft.color565(228, 81, 92), tft.color565(19, 80, 65),
                tft.color565(7, 43, 42)};
      break;
    case 3: // Sunset
      colors = {tft.color565(40, 25, 45), tft.color565(65, 39, 70), tft.color565(91, 53, 90),
                tft.color565(116, 67, 106), TFT_WHITE, tft.color565(236, 192, 225),
                tft.color565(238, 102, 168), tft.color565(255, 168, 91),
                tft.color565(244, 85, 103), tft.color565(102, 55, 109),
                tft.color565(47, 27, 66)};
      break;
    default: // Midnight
      colors = {tft.color565(9, 14, 26), tft.color565(19, 28, 46), tft.color565(29, 40, 63),
                tft.color565(39, 55, 82), TFT_WHITE, tft.color565(164, 179, 205),
                tft.color565(91, 116, 255), tft.color565(50, 205, 171),
                tft.color565(235, 83, 106), tft.color565(29, 44, 79),
                tft.color565(10, 17, 35)};
      break;
  }
}

void applyBrightness()
{
  uint8_t duty = static_cast<uint8_t>((static_cast<uint16_t>(brightnessPercent) * 255U) / 100U);
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(BACKLIGHT_PIN, 5000, 8);
  ledcWrite(BACKLIGHT_PIN, duty);
#else
  ledcSetup(BACKLIGHT_CHANNEL, 5000, 8);
  ledcAttachPin(BACKLIGHT_PIN, BACKLIGHT_CHANNEL);
  ledcWrite(BACKLIGHT_CHANNEL, duty);
#endif
}

void selectUiFont(uint8_t size)
{
  tft.setTextFont(uiFontForSize(size));
  tft.setTextSize(1);
}

void drawText(const String &text, int x, int y, uint8_t size, uint16_t color)
{
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(color);
  selectUiFont(size);
  tft.drawString(text, x, y);
  selectUiFont(1);
}

void drawCentered(const String &text, int x, int y, uint8_t size, uint16_t color)
{
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(color);
  selectUiFont(size);
  tft.drawString(text, x, y);
  tft.setTextDatum(TL_DATUM);
  selectUiFont(1);
}

void drawRight(const String &text, int x, int y, uint8_t size, uint16_t color)
{
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(color);
  selectUiFont(size);
  tft.drawString(text, x, y);
  tft.setTextDatum(TL_DATUM);
  selectUiFont(1);
}

void drawWrapped(const String &source, int x, int y, int maximumWidth, int lineHeight,
                 int maximumLines, uint16_t color, uint8_t size, int skipLines,
                 int *totalLinesOut)
{
  selectUiFont(size);
  String line = "";
  String word = "";
  int lineNumber = 0;
  int drawn = 0;
  auto emitLine = [&]() {
    if (lineNumber >= skipLines && drawn < maximumLines) {
      drawText(line, x, y + drawn * lineHeight, size, color);
      ++drawn;
    }
    ++lineNumber;
    line = "";
  };
  for (size_t i = 0; i <= source.length(); ++i) {
    char c = i < source.length() ? source[i] : ' ';
    if (c == '\r') continue;
    if (c == '\n' || c == ' ' || i == source.length()) {
      if (c == '\n' && word.length() == 0) {
        emitLine();
        continue;
      }
      if (word.length()) {
        String candidate = line.length() ? line + " " + word : word;
        if (line.length() && tft.textWidth(candidate) > maximumWidth) {
          emitLine();
          line = word;
        } else {
          line = candidate;
        }
        word = "";
      }
      if (c == '\n') emitLine();
    } else {
      word += c;
    }
  }
  if (line.length()) emitLine();
  if (totalLinesOut) *totalLinesOut = lineNumber;
  selectUiFont(1);
}

void drawBackground()
{
  tft.fillScreen(colors.background);
  if (wallpaperIndex % 4 == 0) return;
  const int w = screenW();
  const int h = screenH();
  if (wallpaperIndex % 4 == 1) {
    for (int y = 32; y < h; y += landscape ? 42 : 54) {
      int x = ((y / 7) % 2) ? 22 : w - 22;
      tft.fillCircle(x, y, landscape ? 16 : 22, colors.wall1);
      tft.drawCircle(x, y, landscape ? 21 : 29, colors.wall2);
    }
  } else if (wallpaperIndex % 4 == 2) {
    for (int x = -h; x < w; x += 34) {
      tft.drawLine(x, 0, x + h, h, colors.wall1);
      tft.drawLine(x + 9, 0, x + h + 9, h, colors.wall2);
    }
  } else {
    for (int x = 18; x < w; x += 44) {
      for (int y = 42; y < h; y += 44) {
        tft.fillCircle(x + ((y / 44) % 2) * 12, y, 3, colors.wall1);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Persistent settings and first-boot calibration
// ---------------------------------------------------------------------------
void saveConfig()
{
  if (!spiffsMounted) return;
  File file = SPIFFS.open(CONFIG_FILE, "w");
  if (!file) return;
  file.print("theme="); file.println(themeIndex);
  file.print("wallpaper="); file.println(wallpaperIndex);
  file.print("brightness="); file.println(brightnessPercent);
  file.print("sleepTimeout="); file.println(sleepTimeoutSeconds);
  file.print("landscape="); file.println(landscape ? 1 : 0);
  file.print("wifiEnabled="); file.println(wifiEnabled ? 1 : 0);
  file.print("wifiSsid="); file.println(wifiSsid);
  file.print("wifiPassword="); file.println(wifiPassword);
  file.print("profileName="); file.println(profileName);
  file.print("profileConfigured="); file.println(profileConfigured ? 1 : 0);
  file.print("profilePattern="); file.println(profileUsesPattern ? 1 : 0);
  file.print("profileHash="); file.println(profileLockHash);
  file.print("messagesUsername="); file.println(messagesUsername);
  file.print("messagesPassword="); file.println(messagesPassword);
  file.close();
}

void loadConfig()
{
  if (!spiffsMounted || !SPIFFS.exists(CONFIG_FILE)) return;
  File file = SPIFFS.open(CONFIG_FILE, "r");
  if (!file) return;
  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    int separator = line.indexOf('=');
    if (separator <= 0) continue;
    String key = line.substring(0, separator);
    String value = line.substring(separator + 1);
    if (key == "theme") themeIndex = static_cast<uint8_t>(value.toInt() % 4);
    else if (key == "wallpaper") wallpaperIndex = static_cast<uint8_t>(value.toInt() % 4);
    else if (key == "brightness") brightnessPercent = constrain(value.toInt(), 10, 100);
    else if (key == "sleepTimeout") {
      uint32_t valueSeconds = static_cast<uint32_t>(value.toInt());
      sleepTimeoutSeconds = (valueSeconds == 10 || valueSeconds == 30 || valueSeconds == 60) ? valueSeconds : 0;
    }
    else if (key == "landscape") landscape = value.toInt() != 0;
    else if (key == "wifiEnabled") wifiEnabled = value.toInt() != 0;
    else if (key == "wifiSsid") wifiSsid = value;
    else if (key == "wifiPassword") wifiPassword = value;
    else if (key == "profileName") profileName = value;
    else if (key == "profileConfigured") profileConfigured = value.toInt() != 0;
    else if (key == "profilePattern") profileUsesPattern = value.toInt() != 0;
    else if (key == "profileHash") profileLockHash = value;
    else if (key == "messagesUsername") messagesUsername = value;
    else if (key == "messagesPassword") messagesPassword = value;
  }
  file.close();
}

void calibrateTouchIfNeeded()
{
  Serial.println("[PocketOS] Touch-Kalibrierung pruefen...");
  const char *calibrationFile = landscape ? TOUCH_FILE_LANDSCAPE : TOUCH_FILE;
  bool haveCalibration = false;
  const char *touchNamespace = landscape ? "touch_land" : "touch_port";
  if (touchPreferences.begin(touchNamespace, true)) {
    haveCalibration = touchPreferences.getBytesLength("data") == sizeof(touchCalibrationData) &&
                      touchPreferences.getBytes("data", touchCalibrationData, sizeof(touchCalibrationData)) == sizeof(touchCalibrationData);
    touchPreferences.end();
  }
  if (spiffsMounted && SPIFFS.exists(calibrationFile)) {
    File file = SPIFFS.open(calibrationFile, "r");
    if (file && file.size() == sizeof(touchCalibrationData)) {
      size_t read = file.read(reinterpret_cast<uint8_t *>(touchCalibrationData), sizeof(touchCalibrationData));
      haveCalibration = read == sizeof(touchCalibrationData);
    }
    if (file) file.close();
  }
  if (haveCalibration) {
    touchCalibrationValid = true;
    tft.setTouch(touchCalibrationData);
    Serial.println("[PocketOS] Touch-Kalibrierung geladen.");
    return;
  }

  Serial.println("[PocketOS] Keine Touch-Kalibrierung - starte trotzdem weiter.");
  // calibrateTouch() ist synchron und wartet unbegrenzt auf vier Punkte.
  // Nach einem Flash-Erase darf dieser Pfad den Boot nicht blockieren.
  touchCalibrationValid = false;
}

void runTouchCalibration()
{
  const char *calibrationFile = landscape ? TOUCH_FILE_LANDSCAPE : TOUCH_FILE;
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextFont(2);
  tft.drawString("Touch kalibrieren", screenW() / 2, 34);
  tft.setTextFont(1);
  tft.drawString("Bitte alle Markierungen antippen", screenW() / 2, 62);
  delay(250);
  tft.calibrateTouch(touchCalibrationData, TFT_MAGENTA, TFT_BLACK, 15);
  touchCalibrationValid = true;
  tft.setTouch(touchCalibrationData);
  bool saved = false;
  const char *touchNamespace = landscape ? "touch_land" : "touch_port";
  if (touchPreferences.begin(touchNamespace, false)) {
    saved = touchPreferences.putBytes("data", touchCalibrationData, sizeof(touchCalibrationData)) == sizeof(touchCalibrationData);
    touchPreferences.end();
  }
  if (saved && spiffsMounted) {
    File file = SPIFFS.open(calibrationFile, "w");
    if (file) {
      file.write(reinterpret_cast<const uint8_t *>(touchCalibrationData), sizeof(touchCalibrationData));
      file.close();
    }
  }
  setTheme();
  showToast(saved ? "Touch-Kalibrierung gespeichert" : "Speichern fehlgeschlagen", 3000);
  Serial.println(saved ? "[PocketOS] Touch-Kalibrierung gespeichert." : "[PocketOS] Touch-Kalibrierung konnte nicht gespeichert werden.");
  redrawRequested = true;
}

void applyOrientation()
{
  tft.setRotation(landscape ? 1 : 0);
  touchCalibrationValid = false;
  if (spiffsMounted) calibrateTouchIfNeeded();
  scrollPosition = scrollTarget = 0;
  redrawRequested = true;
}

// ---------------------------------------------------------------------------
// SD card support and persistent cache
// ---------------------------------------------------------------------------
bool storageReady()
{
  return sdMounted && SD.cardType() != CARD_NONE;
}

void ensureDirectory(const char *path)
{
  if (storageReady() && !SD.exists(path)) SD.mkdir(path);
}

void writeWelcomeAppIfMissing()
{
  if (!storageReady()) return;
  const char *path = "/Apps/Welcome.papp";
  if (SD.exists(path)) return;
  File file = SD.open(path, "w");
  if (!file) return;
  file.println("title=Willkommen");
  file.println("accent=indigo");
  file.println("label=16,18,2,PocketOS App Runtime");
  file.println("label=16,58,1,Apps liegen als .papp-Dateien auf der SD-Karte.");
  file.println("button=16,102,288,40,Dateien oeffnen,open:files");
  file.println("button=16,154,288,40,Nachricht,toast:Hallo von der SD-Karte");
  file.println("button=16,206,288,40,Eingabe testen,input:Notiz");
  file.close();
}

void writeCalculatorAppIfMissing()
{
  if (!storageReady()) return;
  const char *path = "/Apps/Taschenrechner.papp";
  if (SD.exists(path)) return;
  File file = SD.open(path, "w");
  if (!file) return;
  file.println("title=Taschenrechner");
  file.println("accent=green");
  file.println("label=16,10,2,Taschenrechner");
  file.println("label=16,42,1,+ - * / und Punkt verwenden");
  file.println("input=16,66,288,32,Ausdruck");
  const char *keys[] = {"7","8","9","/","4","5","6","*","1","2","3","-","0",".","=","+","C"};
  for (uint8_t i = 0; i < 17; ++i) {
    int col = i % 4, row = i / 4;
    String action = String("calc:") + keys[i];
    file.printf("button=%d,%d,66,30,%s,%s\n", 16 + col * 70, 108 + row * 32, keys[i], action.c_str());
  }
  file.close();
}

void initialiseStorage()
{
  pinMode(SD_CS_PIN, OUTPUT);
  digitalWrite(SD_CS_PIN, HIGH);
  sdSPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN);
  sdMounted = SD.begin(SD_CS_PIN, sdSPI);
  if (!sdMounted) return;
  ensureDirectory(ROOT_POCKETOS);
  ensureDirectory("/PocketOS/Cache");
  ensureDirectory("/PocketOS/AppData");
  ensureDirectory(ROOT_APPS);
  ensureDirectory(ROOT_PICTURES);
  ensureDirectory(ROOT_MUSIC);
  ensureDirectory(ROOT_DOCUMENTS);
  ensureDirectory(ROOT_DRAWINGS);
  writeWelcomeAppIfMissing();
  writeCalculatorAppIfMissing();
}

void saveBrowserCache()
{
  if (!storageReady()) return;
  File file = SD.open(BROWSER_CACHE_FILE, "w");
  if (!file) return;
  file.println(browserTitle);
  file.println(browserUrl);
  file.print(browserText);
  file.close();
}

bool loadBrowserCache()
{
  if (!storageReady() || !SD.exists(BROWSER_CACHE_FILE)) return false;
  File file = SD.open(BROWSER_CACHE_FILE, "r");
  if (!file) return false;
  browserTitle = file.readStringUntil('\n'); browserTitle.trim();
  browserUrl = file.readStringUntil('\n'); browserUrl.trim();
  browserText = "";
  while (file.available() && browserText.length() < MAX_BROWSER_BYTES) {
    browserText += static_cast<char>(file.read());
  }
  file.close();
  return browserText.length() > 0;
}

// ---------------------------------------------------------------------------
// Display chrome and navigation
// ---------------------------------------------------------------------------
String screenTitle(Screen value)
{
  switch (value) {
    case SC_FILES: return "Dateien";
    case SC_GALLERY: return "Bilder";
    case SC_MUSIC: return "Musik";
    case SC_MESSAGES: return "Messages";
    case SC_EDITOR: return "Texteditor";
    case SC_BROWSER: return "Browser";
    case SC_SETTINGS: return "Einstellungen";
    case SC_WIFI: return "WLAN";
    case SC_PROFILE: return "Profil";
    case SC_APPS: return "SD-Apps";
    case SC_APP_STORE: return "App-Store";
    case SC_APP_RUNNER: return activeAddon >= 0 ? String(addonApps[activeAddon].title) : "SD-App";
    case SC_RECENTS: return "Geoeffnete Apps";
    default: return "PocketOS";
  }
}

void updateBatteryStatus()
{
  if (millis() - lastBatteryReadAt < 30000UL && lastBatteryReadAt != 0) return;
  lastBatteryReadAt = millis();
  uint32_t millivolts = analogReadMilliVolts(BATTERY_ADC_PIN);
  // LCDWiki führt BAT_ADC über einen 1:1-Teiler an GPIO34 heraus.
  batteryVoltage = (millivolts * 2.0f) / 1000.0f;
  batteryPercent = static_cast<uint8_t>(constrain((batteryVoltage - 3.30f) * 100.0f / 0.90f, 0.0f, 100.0f));
}

void enterUltraPowerSave()
{
  ultraPowerSave = true;
  stopMp3Playback();
  digitalWrite(AUDIO_ENABLE_PIN, HIGH);
  digitalWrite(BACKLIGHT_PIN, LOW);
  if (wifiEnabled) WiFi.mode(WIFI_OFF);
  while (digitalRead(TOUCH_IRQ_PIN) == LOW) delay(5);
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_36, 0);
  tft.writecommand(0x10); // ST7796 sleep-in
  esp_light_sleep_start();
  tft.writecommand(0x11); // ST7796 sleep-out
  delay(120);
  digitalWrite(BACKLIGHT_PIN, HIGH);
  ultraPowerSave = false;
  redrawRequested = true;
}

void enterDeepSleep()
{
  stopMp3Playback();
  digitalWrite(AUDIO_ENABLE_PIN, HIGH);
  digitalWrite(BACKLIGHT_PIN, LOW);
  WiFi.mode(WIFI_OFF);
  tft.writecommand(0x10); // ST7796 sleep-in
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_36, 0);
  esp_deep_sleep_start();
}

void serviceAutomaticSleep()
{
  if (sleepTimeoutSeconds == 0 || musicPlaying || ultraPowerSave) return;
  if (millis() - lastActivityAt >= sleepTimeoutSeconds * 1000UL) {
    enterUltraPowerSave();
    lastActivityAt = millis();
  }
}

void drawStatusBar()
{
  const int h = statusHeight();
  tft.fillRect(0, 0, screenW(), h, colors.panel);
  String timeText = "--:--";
  time_t now = time(nullptr);
  if (now > 100000) {
    struct tm localTime;
    localtime_r(&now, &localTime);
    char formatted[8];
    strftime(formatted, sizeof(formatted), "%H:%M", &localTime);
    timeText = formatted;
  }
  drawText(timeText, 7, landscape ? 4 : 6, 1, colors.text);
  String network = "WLAN aus";
  if (wifiEnabled) {
    network = WiFi.status() == WL_CONNECTED ? "WLAN" : "WLAN...";
  }
  drawRight(network, screenW() - (landscape ? 70 : 82), landscape ? 4 : 6, 1,
            WiFi.status() == WL_CONNECTED ? colors.accent2 : colors.muted);
  const int bx = screenW() - (landscape ? 42 : 48);
  const int by = landscape ? 5 : 7;
  const int bw = landscape ? 25 : 29;
  const int bh = landscape ? 12 : 14;
  drawBatteryIcon(bx + bw / 2, by + bh / 2, bw, bh, colors.muted, batteryPercent);
  drawRight(String(batteryPercent) + "%", bx - 5, landscape ? 4 : 6, 1,
            batteryPercent < 15 ? colors.danger : colors.muted);
}

void drawNavigation()
{
  const int y = screenH() - navHeight();
  const int w = screenW();
  const int third = w / 3;
  tft.fillRect(0, y, w, navHeight(), colors.panel);
  tft.drawFastHLine(0, y, w, colors.cardAlt);
  const int centers[] = {third / 2, third + third / 2, third * 2 + (w - third * 2) / 2};
  const UiGlyph glyphs[] = {GLYPH_HOME, GLYPH_APPS, GLYPH_BACK};
  const char *labels[] = {"START", "APPS", "ZURUECK"};
  for (uint8_t i = 0; i < 3; ++i) {
    int left = i == 0 ? 5 : (i == 1 ? third + 5 : third * 2 + 5);
    int width = i == 2 ? w - third * 2 - 10 : third - 10;
    bool active = (i == 0 && screen == SC_HOME) || (i == 1 && (screen == SC_APPS || screen == SC_APP_STORE || screen == SC_APP_RUNNER));
    if (active) drawRoundCard(left, y + 4, width, navHeight() - 8, colors.cardAlt, colors.accent);
    drawThemeGlyph(glyphs[i], centers[i], y + (landscape ? 13 : 15), 1, active ? colors.accent : colors.muted);
    drawCentered(labels[i], centers[i], y + navHeight() - 8, 1, active ? colors.text : colors.muted);
  }
}

void drawHeader(const String &title, const String &subtitle)
{
  const int y = statusHeight();
  const int h = headerHeight();
  tft.fillRect(0, y, screenW(), h, colors.panel);
  tft.drawFastHLine(0, y + h - 1, screenW(), colors.cardAlt);
  drawRoundCard(8, y + 7, landscape ? 30 : 34, h - 14, colors.cardAlt, colors.cardAlt);
  drawThemeGlyph(GLYPH_BACK, 8 + (landscape ? 15 : 17), y + h / 2, 1, colors.accent);
  drawText(title, landscape ? 46 : 50, y + (landscape ? 7 : 8), landscape ? 1 : 2, colors.text);
  if (subtitle.length()) {
    int pillW = min(screenW() / 3, max(48, static_cast<int>(subtitle.length() * 6 + 18)));
    drawRoundCard(screenW() - pillW - 8, y + 8, pillW, h - 16, colors.card, colors.cardAlt);
    drawCentered(subtitle, screenW() - pillW / 2 - 8, y + h / 2, 1, colors.muted);
  }
}

void drawAppFrame(const String &title, const String &subtitle)
{
  drawBackground();
  drawStatusBar();
  drawHeader(title, subtitle);
  drawNavigation();
}

void drawRoundCard(int x, int y, int w, int h, uint16_t fill, uint16_t border)
{
  if (w <= 0 || h <= 0) return;
  tft.fillRoundRect(x, y, w, h, landscape ? 6 : 9, fill);
  if (border) tft.drawRoundRect(x, y, w, h, landscape ? 6 : 9, border);
}

void drawToast()
{
  if (!toastText.length() || static_cast<int32_t>(millis() - toastUntil) >= 0) return;
  int width = min(screenW() - 24, max(120, static_cast<int>(toastText.length() * 7 + 22)));
  int x = (screenW() - width) / 2;
  int y = keyboardOpen ? contentTop() + 8 : screenH() - navHeight() - 42;
  drawRoundCard(x, y, width, 30, colors.cardAlt, colors.accent);
  drawCentered(toastText, screenW() / 2, y + 15, 1, colors.text);
}

void addRecent(Screen value)
{
  if (value == SC_HOME || value == SC_LOCK || value == SC_RECENTS) return;
  for (uint8_t i = 0; i < recentCount; ++i) {
    if (recentScreens[i] == value) {
      for (uint8_t j = i; j > 0; --j) recentScreens[j] = recentScreens[j - 1];
      recentScreens[0] = value;
      return;
    }
  }
  if (recentCount < 6) ++recentCount;
  for (uint8_t i = recentCount - 1; i > 0; --i) recentScreens[i] = recentScreens[i - 1];
  recentScreens[0] = value;
}

void openScreen(Screen target, bool pushCurrent)
{
  if (target == screen) return;
  if (pushCurrent && screen != SC_HOME && screen != SC_LOCK && screen != SC_RECENTS && historyCount < 8) {
    historyStack[historyCount++] = screen;
  }
  if (screen != SC_LOCK) addRecent(screen);
  screen = target;
  scrollPosition = scrollTarget = 0;
  redrawRequested = true;
}

void goHome()
{
  keyboardOpen = false;
  quickTarget = 0;
  screen = SC_HOME;
  historyCount = 0;
  scrollPosition = scrollTarget = 0;
  redrawRequested = true;
}

void goBack()
{
  if (keyboardOpen) {
    keyboardOpen = false;
    redrawRequested = true;
    return;
  }
  if (quickOpen || quickProgress > 0.01f) {
    quickTarget = 0;
    redrawRequested = true;
    return;
  }
  if (screen == SC_HOME) return;
  if (historyCount) {
    screen = historyStack[--historyCount];
  } else {
    screen = SC_HOME;
  }
  scrollPosition = scrollTarget = 0;
  redrawRequested = true;
}

void openRecents()
{
  if (screen != SC_RECENTS) addRecent(screen);
  screen = SC_RECENTS;
  keyboardOpen = false;
  redrawRequested = true;
}

// ---------------------------------------------------------------------------
// SD browsing, photo/music scanning and add-on discovery
// ---------------------------------------------------------------------------
void loadDirectory(const char *directory)
{
  fileCount = 0;
  filesTruncated = false;
  if (!storageReady()) {
    redrawRequested = true;
    return;
  }
  File folder = SD.open(directory);
  if (!folder || !folder.isDirectory()) {
    if (folder) folder.close();
    safeCopy(currentDirectory, sizeof(currentDirectory), "/");
    redrawRequested = true;
    return;
  }
  File entry = folder.openNextFile();
  while (entry) {
    if (fileCount < MAX_FILES) {
      String path = entry.name();
      if (!path.startsWith("/")) {
        path = String(directory) + (String(directory).endsWith("/") ? "" : "/") + path;
      }
      safeCopy(fileItems[fileCount].path, sizeof(fileItems[fileCount].path), path);
      safeCopy(fileItems[fileCount].name, sizeof(fileItems[fileCount].name), baseName(path));
      fileItems[fileCount].directory = entry.isDirectory();
      fileItems[fileCount].size = static_cast<uint32_t>(entry.size());
      ++fileCount;
    } else {
      filesTruncated = true;
    }
    entry.close();
    entry = folder.openNextFile();
  }
  folder.close();

  // Compact stable sort: folders first, then alphabetical file names.
  for (uint8_t i = 0; i < fileCount; ++i) {
    for (uint8_t j = i + 1; j < fileCount; ++j) {
      String left = lowerCopy(String(fileItems[i].name));
      String right = lowerCopy(String(fileItems[j].name));
      bool swap = fileItems[j].directory && !fileItems[i].directory;
      if (fileItems[j].directory == fileItems[i].directory && right < left) swap = true;
      if (swap) {
        FileItem temporary = fileItems[i];
        fileItems[i] = fileItems[j];
        fileItems[j] = temporary;
      }
    }
  }
  safeCopy(currentDirectory, sizeof(currentDirectory), directory);
  scrollPosition = scrollTarget = 0;
  redrawRequested = true;
}

void scanMediaDirectory(const char *directory, MediaItem *items, uint8_t &count, bool pictures)
{
  count = 0;
  if (!storageReady()) return;
  File folder = SD.open(directory);
  if (!folder || !folder.isDirectory()) {
    if (folder) folder.close();
    return;
  }
  File entry = folder.openNextFile();
  while (entry) {
    String path = entry.name();
    if (!entry.isDirectory() && (pictures ? isPictureFile(path) : isMusicFile(path)) && count < MAX_MEDIA) {
      safeCopy(items[count].path, sizeof(items[count].path), path);
      safeCopy(items[count].name, sizeof(items[count].name), baseName(path));
      ++count;
    }
    entry.close();
    entry = folder.openNextFile();
  }
  folder.close();
}

String configValue(const String &line, const String &key)
{
  String prefix = key + "=";
  return line.startsWith(prefix) ? line.substring(prefix.length()) : String();
}

void fetchStoreManifest()
{
  storeCount = 0; storeLoading = true; storeMessage = "Lade App-Liste...";
  if (!wifiEnabled || WiFi.status() != WL_CONNECTED) { storeLoading = false; storeMessage = "WLAN nicht verbunden"; redrawRequested = true; return; }
  HTTPClient http;
  WiFiClientSecure client; client.setInsecure();
  if (!http.begin(client, STORE_MANIFEST_URL) || http.GET() != HTTP_CODE_OK) { http.end(); storeLoading = false; storeMessage = "App-Liste nicht erreichbar"; redrawRequested = true; return; }
  String manifest = http.getString(); http.end();
  int start = 0;
  while (start < static_cast<int>(manifest.length()) && storeCount < MAX_APPS) {
    int end = manifest.indexOf('\n', start); if (end < 0) end = manifest.length();
    String line = manifest.substring(start, end); line.trim(); start = end + 1;
    if (!line.length() || line.startsWith("#")) continue;
    StoreItem &item = storeItems[storeCount];
    safeCopy(item.title, sizeof(item.title), csvField(line, 0));
    safeCopy(item.path, sizeof(item.path), csvField(line, 1));
    safeCopy(item.description, sizeof(item.description), csvField(line, 2));
    safeCopy(item.accent, sizeof(item.accent), csvField(line, 3));
    ++storeCount;
  }
  storeLoading = false; storeMessage = storeCount ? "App antippen zum Herunterladen" : "Keine Apps gefunden"; redrawRequested = true;
}

void downloadStoreItem(uint8_t index)
{
  if (index >= storeCount || !storageReady()) { showToast("SD-Karte fehlt"); return; }
  String remote = String(STORE_RAW_BASE) + storeItems[index].path;
  String destination = String(ROOT_APPS) + "/" + baseName(storeItems[index].path);
  HTTPClient http;
  WiFiClientSecure client; client.setInsecure();
  if (!wifiEnabled || WiFi.status() != WL_CONNECTED || !http.begin(client, remote)) { showToast("WLAN nicht verfuegbar"); return; }
  int status = http.GET();
  if (status != HTTP_CODE_OK) { http.end(); showToast("Download fehlgeschlagen"); return; }
  File file = SD.open(destination, "w");
  if (!file) { http.end(); showToast("SD-Datei nicht schreibbar"); return; }
  WiFiClient *stream = http.getStreamPtr(); uint8_t buffer[128]; int remaining = http.getSize();
  while (http.connected() && (remaining > 0 || remaining == -1)) {
    size_t available = stream->available();
    if (!available) { delay(1); continue; }
    size_t count = stream->readBytes(buffer, min(available, sizeof(buffer)));
    file.write(buffer, count); if (remaining > 0) remaining -= count;
  }
  file.close(); http.end(); scanAddonApps(); showToast(baseName(storeItems[index].path) + " installiert");
}

void openAppStore()
{ openScreen(SC_APP_STORE); fetchStoreManifest(); }

void scanAddonApps()
{
  addonCount = 0;
  if (!storageReady()) return;
  File directory = SD.open(ROOT_APPS);
  if (!directory || !directory.isDirectory()) {
    if (directory) directory.close();
    return;
  }
  File entry = directory.openNextFile();
  while (entry) {
    String path = entry.name();
    // Je nach SD/FAT-Version liefert openNextFile() nur den Dateinamen.
    // Der Runtime-Loader braucht jedoch einen vollständigen SD-Pfad.
    if (!path.startsWith("/")) path = String(ROOT_APPS) + "/" + path;
    if (!entry.isDirectory() && isAddonFile(path) && addonCount < MAX_APPS) {
      AddonApp &app = addonApps[addonCount];
      safeCopy(app.path, sizeof(app.path), path);
      safeCopy(app.title, sizeof(app.title), baseName(path));
      safeCopy(app.accent, sizeof(app.accent), "indigo");
      File source = SD.open(path, "r");
      uint8_t lines = 0;
      while (source && source.available() && lines++ < 12) {
        String line = source.readStringUntil('\n');
        line.trim();
        String title = configValue(line, "title");
        String accent = configValue(line, "accent");
        if (title.length()) safeCopy(app.title, sizeof(app.title), title);
        if (accent.length()) safeCopy(app.accent, sizeof(app.accent), accent);
      }
      if (source) source.close();
      ++addonCount;
    }
    entry.close();
    entry = directory.openNextFile();
  }
  directory.close();
}

void chooseDefaultEditorFile()
{
  if (!storageReady()) {
    showToast("Keine SD-Karte");
    return;
  }
  const char *path = "/Documents/Notiz.txt";
  if (!SD.exists(path)) {
    File file = SD.open(path, "w");
    if (file) {
      file.println("Willkommen bei PocketOS.");
      file.println("Diese Notiz liegt auf deiner SD-Karte.");
      file.close();
    }
  }
  safeCopy(editorPath, sizeof(editorPath), path);
  editorText = "";
  File file = SD.open(editorPath, "r");
  while (file && file.available() && editorText.length() < MAX_EDITOR_BYTES) editorText += static_cast<char>(file.read());
  if (file) file.close();
  editorDirty = false;
  editorTopLine = 0;
  openScreen(SC_EDITOR);
}

void openTextFile(const char *path)
{
  if (!storageReady()) return;
  File file = SD.open(path, "r");
  if (!file) {
    showToast("Datei nicht lesbar");
    return;
  }
  editorText = "";
  while (file.available() && editorText.length() < MAX_EDITOR_BYTES) editorText += static_cast<char>(file.read());
  bool cut = file.available();
  file.close();
  safeCopy(editorPath, sizeof(editorPath), path);
  editorDirty = false;
  editorTopLine = 0;
  openScreen(SC_EDITOR);
  if (cut) showToast("Datei auf 8 KB begrenzt");
}

void saveEditorFile()
{
  if (!storageReady() || !strlen(editorPath)) {
    showToast("SD oder Dateipfad fehlt");
    return;
  }
  File file = SD.open(editorPath, "w");
  if (!file) {
    showToast("Speichern fehlgeschlagen");
    return;
  }
  file.print(editorText);
  file.close();
  editorDirty = false;
  showToast("Auf SD gespeichert");
}

String uniquePath(const char *directory, const String &stem, const String &extension)
{
  if (!storageReady()) return "";
  for (uint16_t number = 0; number < 1000; ++number) {
    String candidate = String(directory) + (String(directory).endsWith("/") ? "" : "/") +
                       stem + (number ? String(number) : "") + extension;
    if (!SD.exists(candidate)) return candidate;
  }
  return "";
}

// ---------------------------------------------------------------------------
// Home, settings, file manager and media UI
// ---------------------------------------------------------------------------
uint16_t shortcutColor(uint8_t index)
{
  static const uint8_t red[]   = {78, 45, 45, 72, 52, 106, 90, 160, 32};
  static const uint8_t green[] = {111, 179, 134, 95, 164, 83, 107, 92, 158};
  static const uint8_t blue[]  = {235, 154, 92, 220, 174, 189, 255, 204, 220};
  return tft.color565(red[index % 9], green[index % 9], blue[index % 9]);
}


// Theme-neutral 16x16 monochrome UI art. One bit is a foreground pixel;
// transparent pixels reveal the card/panel underneath. The masks live in
// program flash and are tinted at draw time, so every theme can reuse them.

static const uint16_t uiGlyphs[][16] PROGMEM = {
  {0x07E0,0x0FF0,0x1818,0x1818,0x1818,0x1818,0x1FF8,0x1818,0x1818,0x1818,0x1818,0x1FF8,0x0000,0x0000,0x0000,0x0000},
  {0x0000,0x0FF0,0x1818,0x1C38,0x1FF8,0x1FF8,0x1FF8,0x1FF8,0x1FF8,0x1FF8,0x0FF0,0x0000,0x0000,0x0000,0x0000,0x0000},
  {0x0060,0x00E0,0x01E0,0x03E0,0x07E0,0x0FE0,0x1FE0,0x1FE0,0x1FE0,0x1FE0,0x1FE0,0x0C60,0x0C60,0x0C60,0x07C0,0x0000},
  {0x07E0,0x1818,0x300C,0x6006,0x6006,0x6006,0x6006,0x6006,0x6006,0x6006,0x300C,0x1818,0x07E0,0x0000,0x0000,0x0000},
  {0x1FF8,0x1818,0x1818,0x1818,0x1FF8,0x1818,0x1818,0x1818,0x1818,0x1818,0x1FF8,0x0000,0x0000,0x0000,0x0000,0x0000},
  {0x0000,0x0018,0x0038,0x0070,0x00E0,0x01C0,0x0380,0x0700,0x0E00,0x1C00,0x1800,0x1000,0x0000,0x0000,0x0000,0x0000},
  {0x03C0,0x0FF0,0x1C38,0x381C,0x300C,0x7FFE,0x6006,0x6006,0x7FFE,0x300C,0x381C,0x1C38,0x0FF0,0x03C0,0x0000,0x0000},
  {0x03C0,0x0FF0,0x1FF8,0x3FFC,0x3FFC,0x1FF8,0x0FF0,0x0FF0,0x1FF8,0x3FFC,0x3FFC,0x1FF8,0x0FF0,0x03C0,0x0000,0x0000},
  {0x07E0,0x1818,0x300C,0x6006,0x6006,0x6006,0x6006,0x6006,0x6006,0x6006,0x300C,0x1818,0x07E0,0x0000,0x0000,0x0000},
  {0x0000,0x0000,0x3000,0x3800,0x3C00,0x3E00,0x3F00,0x3F80,0x3F00,0x3E00,0x3C00,0x3800,0x3000,0x0000,0x0000,0x0000},
  {0x0000,0x03C0,0x0FF0,0x1FF8,0x3FFC,0x3FFC,0x300C,0x300C,0x300C,0x300C,0x300C,0x300C,0x300C,0x0000,0x0000,0x0000},
  {0x7FFE,0x6006,0x6006,0x6006,0x6006,0x6006,0x6006,0x6006,0x6006,0x6006,0x6006,0x6006,0x7FFE,0x0000,0x0000,0x0000}
};
const char *homeLabels[] = {"Dateien", "Bilder", "Musik", "Browser", "Messages", "Notizen", "Einstellungen", "Profil", "SD-Apps"};

void drawBatteryIcon(int cx, int cy, int w, int h, uint16_t color, uint8_t percent) {
  int left=cx-w/2, top=cy-h/2;
  tft.drawRoundRect(left,top,w-3,h,3,color);
  tft.fillRoundRect(left+w-2,top+h/3,3,max(2,h/3),1,color);
  int inner=max(1,(w-9)*percent/100);
  uint16_t fill=percent<15?colors.danger:colors.accent2;
  tft.fillRoundRect(left+3,top+3,inner,max(1,h-6),2,fill);
}

void drawThemeGlyph(UiGlyph glyph, int cx, int cy, uint8_t scale, uint16_t color) {
  if (glyph >= sizeof(uiGlyphs) / sizeof(uiGlyphs[0])) return;
  const uint16_t *rows = uiGlyphs[glyph];
  for (uint8_t y = 0; y < 16; ++y) {
    uint16_t bits = pgm_read_word(&rows[y]);
    for (uint8_t x = 0; x < 16; ++x) if (bits & (0x8000 >> x))
      tft.fillRect(cx - 8 * scale + x * scale, cy - 8 * scale + y * scale, scale, scale, color);
  }
}

void drawHome()
{
  drawBackground();
  drawStatusBar();
  const int w = screenW();
  const int h = screenH();
  const int gap = landscape ? 8 : 8;
  const int columns = landscape ? 5 : 3;
  const int startY = statusHeight() + (landscape ? 8 : 12);
  const int top = startY + (landscape ? 40 : 43);
  const int tileW = (w - gap * (columns + 1)) / columns;
  const int tileH = landscape ? 70 : 68;
  drawRoundCard(10, startY, w - 20, landscape ? 33 : 36, colors.card, colors.cardAlt);
  drawText("PocketOS", 18, startY + (landscape ? 8 : 9), landscape ? 2 : 2, colors.text);
  String greeting = profileConfigured ? ("Hallo, " + profileName) : "Profil einrichten";
  drawRight(greeting, w - 18, startY + (landscape ? 10 : 11), 1, colors.muted);
  static const UiGlyph homeGlyphs[] = {GLYPH_FILES, GLYPH_IMAGE, GLYPH_MUSIC, GLYPH_BROWSER, GLYPH_NOTE, GLYPH_NOTE, GLYPH_SETTINGS, GLYPH_USER, GLYPH_APPS};
  for (uint8_t i = 0; i < HOME_APP_COUNT; ++i) {
    int col = i % columns;
    int row = i / columns;
    int x = gap + col * (tileW + gap);
    int y = top + row * (tileH + gap);
    if (y + tileH > h - navHeight() - 4) continue;
    drawRoundCard(x, y, tileW, tileH, colors.card, colors.cardAlt);
    const int iconBox = landscape ? 34 : 32;
    drawRoundCard(x + tileW / 2 - iconBox / 2, y + 7, iconBox, iconBox, shortcutColor(i), 0);
    drawThemeGlyph(homeGlyphs[i], x + tileW / 2, y + 7 + iconBox / 2, 1, TFT_WHITE);
    drawCentered(homeLabels[i], x + tileW / 2, y + tileH - 13, 1, colors.text);
  }
  if (!storageReady()) {
    drawRoundCard(12, h - navHeight() - 32, w - 24, 24, colors.danger, colors.danger);
    drawCentered("SD-KARTE FEHLT", w / 2, h - navHeight() - 20, 1, TFT_WHITE);
  }
  drawNavigation();
}

void drawSettings()
{
  drawAppFrame("Einstellungen", "System");
  const char *labels[] = {"Theme", "Hintergrund", "Helligkeit", "Ausrichtung", "WLAN", "Profil & Sperre", "Browser-Cache", "Touch kalibrieren", "Automatischer Schlaf", "Ultraenergiesparmodus", "Ausschalten", "Systeminfo", "Messages-Anmeldung"};
  String values[] = {
    themeIndex == 0 ? "Midnight" : themeIndex == 1 ? "Paper" : themeIndex == 2 ? "Forest" : "Sunset",
    String("Design ") + String(wallpaperIndex + 1), String(brightnessPercent) + "%",
    landscape ? "Querformat" : "Hochformat", wifiEnabled ? (WiFi.status() == WL_CONNECTED ? "verbunden" : "ein") : "aus",
    profileConfigured ? (profileUsesPattern ? "Muster" : "PIN") : "einrichten", "letzte Seite", "starten",
    sleepTimeoutSeconds == 10 ? "10 Sekunden" : sleepTimeoutSeconds == 30 ? "30 Sekunden" : sleepTimeoutSeconds == 60 ? "1 Minute" : "aus",
    "Touch IRQ", "Deep Sleep", "ESP32-32E", messagesUsername.length() ? "gespeichert" : "nicht gespeichert"};
  const int rowH = landscape ? 34 : 43;
  const int start = contentTop() + 5;
  const int count = 13;
  int first = static_cast<int>(scrollPosition);
  float fraction = scrollPosition - first;
  for (int i = 0; i < count; ++i) {
    int y = start + static_cast<int>((i - first - fraction) * rowH);
    if (y + rowH < contentTop() || y > contentBottom()) continue;
    drawRoundCard(8, y, screenW() - 16, rowH - 5, colors.card, i == 2 ? colors.accent : 0);
    drawText(labels[i], 18, y + (landscape ? 8 : 10), 1, colors.text);
    drawRight(values[i], screenW() - 19, y + (landscape ? 8 : 10), 1, colors.muted);
    if (i == 2) {
      int sliderX = screenW() / 2 - 6;
      int sliderW = screenW() / 2 - 32;
      tft.drawFastHLine(sliderX, y + rowH - 12, sliderW, colors.muted);
      tft.fillCircle(sliderX + sliderW * brightnessPercent / 100, y + rowH - 12, 4, colors.accent);
    }
  }
}

void drawWifi()
{
  drawAppFrame("WLAN", wifiEnabled ? "ein" : "aus");
  int y = contentTop() + 12;
  drawText("Netzwerkname", 16, y, 1, colors.muted);
  drawRoundCard(12, y + 17, screenW() - 24, 36, colors.card, colors.cardAlt);
  drawText(wifiSsid.length() ? wifiSsid : "Tippen zum Eingeben", 21, y + 28, 1,
           wifiSsid.length() ? colors.text : colors.muted);
  y += landscape ? 63 : 78;
  drawText("Passwort", 16, y, 1, colors.muted);
  drawRoundCard(12, y + 17, screenW() - 24, 36, colors.card, colors.cardAlt);
  String hidden = "";
  for (uint8_t i = 0; i < min(static_cast<size_t>(18), wifiPassword.length()); ++i) hidden += '*';
  drawText(hidden.length() ? hidden : "Tippen zum Eingeben", 21, y + 28, 1,
           hidden.length() ? colors.text : colors.muted);
  y += landscape ? 62 : 76;
  int buttonW = (screenW() - 38) / 2;
  drawRoundCard(12, y, buttonW, 38, wifiEnabled ? colors.accent : colors.cardAlt);
  drawCentered(wifiEnabled ? "Verbinden" : "WLAN einschalten", 12 + buttonW / 2, y + 19, 1, TFT_WHITE);
  drawRoundCard(26 + buttonW, y, buttonW, 38, colors.cardAlt);
  drawCentered("Ein/Aus", 26 + buttonW + buttonW / 2, y + 19, 1, colors.text);
  String state = wifiEnabled ? (WiFi.status() == WL_CONNECTED ? "Verbunden: " + WiFi.localIP().toString() : "Nicht verbunden") : "WLAN ist deaktiviert";
  drawWrapped(state, 16, y + 54, screenW() - 32, 15, 3, colors.muted);
}

void drawFileRow(const FileItem &item, int y, int rowH)
{
  int w = screenW();
  drawRoundCard(7, y, w - 14, rowH - 4, colors.card);
  uint16_t marker = item.directory ? colors.accent2 : colors.accent;
  tft.fillCircle(23, y + (rowH - 4) / 2, landscape ? 8 : 10, marker);
  drawCentered(item.directory ? "D" : "F", 23, y + (rowH - 4) / 2, 1, TFT_WHITE);
  drawText(item.name, 39, y + (landscape ? 8 : 10), 1, colors.text);
  String meta = item.directory ? "Ordner" : humanBytes(item.size);
  drawRight(meta, w - 15, y + (landscape ? 8 : 10), 1, colors.muted);
}

void drawFiles()
{
  drawAppFrame("Dateien", currentDirectory);
  if (!storageReady()) {
    drawCentered("Keine SD-Karte", screenW() / 2, contentTop() + 65, 2, colors.danger);
    drawCentered("Karte einlegen und neu starten", screenW() / 2, contentTop() + 93, 1, colors.muted);
    return;
  }
  const int start = contentTop() + 4;
  const int actionH = landscape ? 32 : 38;
  const int listBottom = contentBottom() - actionH - 4;
  const int rowH = landscape ? 31 : 40;
  int first = static_cast<int>(scrollPosition);
  float fraction = scrollPosition - first;
  for (uint8_t i = 0; i < fileCount; ++i) {
    int y = start + static_cast<int>((static_cast<int>(i) - first - fraction) * rowH);
    if (y + rowH < start || y > listBottom) continue;
    drawFileRow(fileItems[i], y, rowH);
  }
  if (!fileCount) drawCentered("Dieser Ordner ist leer", screenW() / 2, start + 44, 1, colors.muted);
  if (filesTruncated) drawCentered("Weitere Dateien nicht angezeigt", screenW() / 2, listBottom - 10, 1, colors.muted);
  int y = contentBottom() - actionH;
  int bw = (screenW() - 28) / 3;
  drawRoundCard(7, y, bw, actionH - 4, colors.cardAlt);
  drawCentered("HOCH", 7 + bw / 2, y + (actionH - 4) / 2, 1, colors.text);
  drawRoundCard(14 + bw, y, bw, actionH - 4, colors.accent);
  drawCentered("+ORDNER", 14 + bw + bw / 2, y + (actionH - 4) / 2, 1, TFT_WHITE);
  drawRoundCard(21 + bw * 2, y, bw, actionH - 4, colors.accent2);
  drawCentered("+TEXT", 21 + bw * 2 + bw / 2, y + (actionH - 4) / 2, 1, TFT_WHITE);
}

bool jpegOutput(int16_t x, int16_t y, uint16_t width, uint16_t height, uint16_t *bitmap)
{
  if (y < jpegClipTop || y + height > jpegClipBottom) return true;
  if (x < 0 || x + width > screenW()) return true;
  tft.pushImage(x, y, width, height, bitmap);
  return true;
}

uint16_t read16(File &file)
{
  uint16_t value = file.read();
  value |= static_cast<uint16_t>(file.read()) << 8;
  return value;
}

uint32_t read32(File &file)
{
  uint32_t value = file.read();
  value |= static_cast<uint32_t>(file.read()) << 8;
  value |= static_cast<uint32_t>(file.read()) << 16;
  value |= static_cast<uint32_t>(file.read()) << 24;
  return value;
}

bool drawBmpFile(const char *path, int x, int y)
{
  File file = SD.open(path, "r");
  if (!file || file.read() != 'B' || file.read() != 'M') {
    if (file) file.close();
    return false;
  }
  read32(file); read32(file);
  uint32_t dataOffset = read32(file);
  read32(file);
  int32_t width = static_cast<int32_t>(read32(file));
  int32_t height = static_cast<int32_t>(read32(file));
  if (read16(file) != 1 || read16(file) != 24 || width <= 0 || height == 0 || width > 480 || abs(height) > 320) {
    file.close();
    return false;
  }
  bool bottomUp = height > 0;
  int32_t actualHeight = abs(height);
  uint32_t rowBytes = (static_cast<uint32_t>(width) * 3U + 3U) & ~3U;
  int drawWidth = min(static_cast<int>(width), screenW() - x);
  int drawHeight = min(static_cast<int>(actualHeight), jpegClipBottom - y);
  for (int row = 0; row < drawHeight; ++row) {
    int sourceRow = bottomUp ? actualHeight - 1 - row : row;
    file.seek(dataOffset + static_cast<uint32_t>(sourceRow) * rowBytes);
    if (file.read(bmpRow, min(rowBytes, static_cast<uint32_t>(sizeof(bmpRow)))) < width * 3) break;
    for (int col = 0; col < drawWidth; ++col) {
      uint8_t b = bmpRow[col * 3];
      uint8_t g = bmpRow[col * 3 + 1];
      uint8_t r = bmpRow[col * 3 + 2];
      bmpLine[col] = tft.color565(r, g, b);
    }
    tft.pushImage(x, y + row, drawWidth, 1, bmpLine);
  }
  file.close();
  return true;
}

void drawPictureViewer()
{
  drawAppFrame("Bild", baseName(selectedPicture));
  const int top = contentTop() + 2;
  const int bottom = contentBottom() - 2;
  tft.fillRect(0, top, screenW(), bottom - top, TFT_BLACK);
  jpegClipTop = top;
  jpegClipBottom = bottom;
  bool succeeded = false;
  if (hasExtension(String(selectedPicture), ".bmp")) {
    succeeded = drawBmpFile(selectedPicture, 0, top);
  } else if (storageReady()) {
    TJpgDec.setJpgScale(1);
    succeeded = TJpgDec.drawSdJpg(0, top, selectedPicture);
  }
  if (!succeeded) {
    drawCentered("Bild kann nicht angezeigt werden", screenW() / 2, top + 45, 1, colors.danger);
    drawCentered("JPEG oder 24-bit BMP verwenden", screenW() / 2, top + 64, 1, colors.muted);
  }
}

void drawGallery()
{
  if (galleryDetail) {
    drawPictureViewer();
    return;
  }
  drawAppFrame("Bilder", String(photoCount) + " Dateien");
  const int columns = landscape ? 5 : 3;
  const int gap = 8;
  const int tileW = (screenW() - gap * (columns + 1)) / columns;
  const int tileH = landscape ? 62 : 86;
  const int start = contentTop() + 8;
  int firstRow = static_cast<int>(scrollPosition);
  float fraction = scrollPosition - firstRow;
  for (uint8_t i = 0; i < photoCount; ++i) {
    int row = i / columns;
    int col = i % columns;
    int x = gap + col * (tileW + gap);
    int y = start + static_cast<int>((row - firstRow - fraction) * (tileH + gap));
    if (y + tileH < start || y > contentBottom()) continue;
    drawRoundCard(x, y, tileW, tileH, colors.card, colors.cardAlt);
    tft.fillRect(x + 8, y + 8, tileW - 16, tileH - (landscape ? 27 : 34), shortcutColor(i + 1));
    drawCentered("BILD", x + tileW / 2, y + (tileH - (landscape ? 27 : 34)) / 2 + 8, 1, TFT_WHITE);
    drawCentered(photoItems[i].name, x + tileW / 2, y + tileH - (landscape ? 10 : 13), 1, colors.text);
  }
  if (!photoCount) {
    drawCentered("Keine Bilder in /Pictures", screenW() / 2, contentTop() + 55, 1, colors.muted);
    drawCentered("JPEG oder BMP per Dateimanager oeffnen", screenW() / 2, contentTop() + 76, 1, colors.muted);
  }
}

void drawMusic()
{
  drawAppFrame("Musik", musicPlaying ? "MP3 laeuft" : "MP3-Player");
  int top = contentTop() + 7;
  drawRoundCard(10, top, screenW() - 20, landscape ? 54 : 72, colors.card, colors.cardAlt);
  String track = strlen(selectedMusic) ? baseName(selectedMusic) : "Kein Titel ausgewaehlt";
  drawText(track, 22, top + 13, 1, colors.text);
  drawText("DAC GPIO26 / Lautsprecher GPIO4", 22, top + (landscape ? 31 : 38), 1, colors.muted);
  drawText(musicPlaying ? "Wiedergabe aktiv" : "Bereit", 22, top + (landscape ? 44 : 57), 1,
           musicPlaying ? colors.accent2 : colors.muted);
  int listTop = top + (landscape ? 63 : 84);
  int rowH = landscape ? 28 : 36;
  int first = static_cast<int>(scrollPosition);
  float fraction = scrollPosition - first;
  for (uint8_t i = 0; i < musicCount; ++i) {
    int y = listTop + static_cast<int>((static_cast<int>(i) - first - fraction) * rowH);
    if (y + rowH > contentBottom() || y + rowH < listTop) continue;
    drawRoundCard(8, y, screenW() - 16, rowH - 3, colors.card);
    drawText(musicItems[i].name, 19, y + (landscape ? 7 : 9), 1, colors.text);
    drawRight(String(musicItems[i].path) == String(selectedMusic) && musicPlaying ? "PAUSE" : "START", screenW() - 17,
              y + (landscape ? 7 : 9), 1, colors.accent2);
  }
  if (!musicCount) drawCentered("Keine Audiodateien in /Music", screenW() / 2, listTop + 35, 1, colors.muted);
}

void stopMp3Playback()
{
  if (mp3Copier) { delete mp3Copier; mp3Copier = nullptr; }
  if (mp3Decoder) { mp3Decoder->end(); delete mp3Decoder; mp3Decoder = nullptr; }
  if (mp3File) { mp3File->close(); delete mp3File; mp3File = nullptr; }
  if (audioOutput) { audioOutput->end(); delete audioOutput; audioOutput = nullptr; }
  pinMode(AUDIO_ENABLE_PIN, OUTPUT); digitalWrite(AUDIO_ENABLE_PIN, HIGH); musicPlaying = false;
}

bool startMp3Playback(const char *path)
{
  stopMp3Playback();
  if (!storageReady() || !path || !hasExtension(String(path), ".mp3")) return false;
  File opened = SD.open(path, FILE_READ); if (!opened) return false;
  mp3File = new File(opened); audioOutput = new AnalogAudioStream();
  auto config = audioOutput->defaultConfig(TX_MODE);
  config.sample_rate = 44100; config.channels = 2; config.bits_per_sample = 16;
  // Größerer Puffer verhindert hörbare Unterläufe, wenn SD und Touch kurz
  // dieselbe SPI-Schnittstelle beanspruchen.
  audioOutput->setWriteBufferSize(4096);
  if (!audioOutput->begin(config)) { stopMp3Playback(); return false; }
  mp3Decoder = new EncodedAudioStream(audioOutput, new MP3DecoderHelix());
  if (!mp3Decoder->begin()) { stopMp3Playback(); return false; }
  mp3Copier = new StreamCopy(*mp3Decoder, *mp3File);
  pinMode(AUDIO_ENABLE_PIN, OUTPUT); digitalWrite(AUDIO_ENABLE_PIN, LOW);
  if (!mp3Copier) { stopMp3Playback(); return false; }
  musicPlaying = true; return true;
}

void serviceMp3Playback()
{
  if (!mp3Decoder || !musicPlaying) return;
  if (!mp3File || !mp3File->available()) { stopMp3Playback(); redrawRequested = true; return; }
  // Mehrere kleine Kopierzyklen halten den Helix-Decoder und den DAC-Puffer
  // auch dann gefüllt, wenn die Touch-Abfrage kurz etwas Zeit benötigt.
  for (uint8_t i = 0; i < 12 && mp3File->available(); ++i) {
    if (!mp3Copier->copy()) break;
  }
}

// ---------------------------------------------------------------------------
// Editor and browser application
// ---------------------------------------------------------------------------
void drawEditor()
{
  String subtitle = strlen(editorPath) ? baseName(editorPath) : "Neue Notiz";
  drawAppFrame("Texteditor", subtitle);
  const int controlsH = landscape ? 30 : 38;
  const int textTop = contentTop() + 5;
  const int textBottom = contentBottom() - controlsH - 5;
  drawRoundCard(7, textTop, screenW() - 14, textBottom - textTop, colors.card, colors.cardAlt);
  int availableLines = max(1, (textBottom - textTop - 10) / (landscape ? 13 : 15));
  int total = 0;
  drawWrapped(editorText.length() ? editorText : "Tippe auf Tastatur, um Text einzugeben.", 15, textTop + 7,
              screenW() - 30, landscape ? 13 : 15, availableLines,
              editorText.length() ? colors.text : colors.muted, 1, editorTopLine, &total);
  editorTotalLines = total;
  const int y = contentBottom() - controlsH;
  int left = (screenW() - 24) / 2;
  drawRoundCard(8, y, left, controlsH - 3, colors.accent);
  drawCentered("TASTATUR", 8 + left / 2, y + (controlsH - 3) / 2, 1, TFT_WHITE);
  drawRoundCard(16 + left, y, left, controlsH - 3, editorDirty ? colors.accent2 : colors.cardAlt);
  drawCentered(editorDirty ? "SPEICHERN *" : "SPEICHERN", 16 + left + left / 2, y + (controlsH - 3) / 2, 1,
               editorDirty ? TFT_WHITE : colors.text);
}

String decodeEntities(String value)
{
  value.replace("&amp;", "&");
  value.replace("&lt;", "<");
  value.replace("&gt;", ">");
  value.replace("&quot;", "\"");
  value.replace("&#39;", "'");
  value.replace("&nbsp;", " ");
  return value;
}

String attributeValue(const String &tag, const String &attribute)
{
  String lower = lowerCopy(tag);
  int begin = lower.indexOf(attribute + "=");
  if (begin < 0) return "";
  begin += attribute.length() + 1;
  if (begin >= static_cast<int>(tag.length())) return "";
  char quote = tag[begin];
  if (quote == '\'' || quote == '\"') {
    int end = tag.indexOf(quote, begin + 1);
    return end < 0 ? tag.substring(begin + 1) : tag.substring(begin + 1, end);
  }
  int end = begin;
  while (end < static_cast<int>(tag.length()) && tag[end] != ' ' && tag[end] != '\t') ++end;
  return tag.substring(begin, end);
}

String resolveBrowserUrl(const String &href)
{
  if (href.startsWith("http://") || href.startsWith("https://")) return href;
  int scheme = browserUrl.indexOf("://");
  if (scheme < 0) return href;
  int domainEnd = browserUrl.indexOf('/', scheme + 3);
  String origin = domainEnd < 0 ? browserUrl : browserUrl.substring(0, domainEnd);
  if (href.startsWith("/")) return origin + href;
  int slash = browserUrl.lastIndexOf('/');
  String base = slash > scheme + 2 ? browserUrl.substring(0, slash + 1) : origin + "/";
  return base + href;
}

void addBrowserLink(const String &href, String label)
{
  if (!href.length() || browserLinkCount >= 6) return;
  label = decodeEntities(label);
  label.trim();
  if (!label.length()) label = href;
  browserLinks[browserLinkCount] = resolveBrowserUrl(href);
  browserLinkLabels[browserLinkCount] = label.substring(0, 42);
  ++browserLinkCount;
}

void appendBrowserChar(char character, bool &previousSpace, String &anchorText)
{
  if (browserText.length() >= MAX_BROWSER_BYTES) return;
  if (character == '\r') return;
  if (character == '\n') {
    if (browserText.length() && browserText[browserText.length() - 1] != '\n') browserText += '\n';
    if (anchorText.length() < 80) anchorText += ' ';
    previousSpace = true;
    return;
  }
  if (character == '\t') character = ' ';
  if (character == ' ') {
    if (previousSpace) return;
    previousSpace = true;
  } else {
    previousSpace = false;
  }
  browserText += character;
  if (anchorText.length() < 80) anchorText += character;
}

void parseHtmlStream(WiFiClient *stream, HTTPClient &http)
{
  browserText = "";
  browserTitle = "HTML-Seite";
  browserLinkCount = 0;
  String tag = "";
  String anchorHref = "";
  String anchorText = "";
  String titleText = "";
  bool insideTag = false;
  bool insideScript = false;
  bool insideStyle = false;
  bool insideTitle = false;
  bool previousSpace = true;
  uint32_t started = millis();
  size_t readBytes = 0;
  while ((http.connected() || stream->available()) && millis() - started < 12000 && readBytes < 18000) {
    if (!stream->available()) {
      delay(1);
      continue;
    }
    char c = static_cast<char>(stream->read());
    ++readBytes;
    if (insideTag) {
      if (c == '>') {
        String rawTag = tag;
        String clean = lowerCopy(rawTag);
        clean.trim();
        if (clean.startsWith("script")) insideScript = true;
        else if (clean.startsWith("/script")) insideScript = false;
        else if (clean.startsWith("style")) insideStyle = true;
        else if (clean.startsWith("/style")) insideStyle = false;
        else if (clean.startsWith("title")) { insideTitle = true; titleText = ""; }
        else if (clean.startsWith("/title")) { insideTitle = false; if (titleText.length()) browserTitle = decodeEntities(titleText); }
        else if (clean == "br" || clean.startsWith("br ") || clean == "p" || clean.startsWith("p ") ||
                 clean.startsWith("/p") || clean.startsWith("h1") || clean.startsWith("h2") ||
                 clean.startsWith("h3") || clean.startsWith("li") || clean.startsWith("/li") || clean.startsWith("div")) {
          appendBrowserChar('\n', previousSpace, anchorText);
        }
        if (clean == "a" || clean.startsWith("a ")) {
          anchorHref = attributeValue(rawTag, "href");
          anchorText = "";
        } else if (clean.startsWith("/a")) {
          addBrowserLink(anchorHref, anchorText);
          anchorHref = "";
          anchorText = "";
        }
        insideTag = false;
        tag = "";
      } else if (tag.length() < 240) {
        tag += c;
      }
      continue;
    }
    if (c == '<') {
      insideTag = true;
      tag = "";
      continue;
    }
    if (insideScript || insideStyle) continue;
    if (insideTitle) {
      if (titleText.length() < 100) titleText += c;
    }
    appendBrowserChar(c, previousSpace, anchorText);
  }
  browserText = decodeEntities(browserText);
  browserText.trim();
  if (!browserText.length()) browserText = "Die HTML-Seite enthielt keinen lesbaren Text.";
}

void fetchBrowserPage()
{
  browserUrl.trim();
  if (!browserUrl.length()) {
    showToast("Adresse fehlt");
    return;
  }
  if (!browserUrl.startsWith("http://") && !browserUrl.startsWith("https://")) browserUrl = "http://" + browserUrl;
  if (!wifiEnabled || WiFi.status() != WL_CONNECTED) {
    if (loadBrowserCache()) showToast("Offline: gespeicherte Seite");
    else showToast("WLAN nicht verbunden");
    redrawRequested = true;
    return;
  }
  browserLoading = true;
  redrawRequested = true;
  HTTPClient http;
  http.setTimeout(8500);
  http.setUserAgent("PocketOS/1.0 HTML");
  if (!http.begin(browserUrl)) {
    browserLoading = false;
    showToast("Adresse konnte nicht geoeffnet werden");
    return;
  }
  int status = http.GET();
  if (status == HTTP_CODE_OK) {
    String type = http.header("Content-Type");
    if (type.length() && type.indexOf("html") < 0 && type.indexOf("text") < 0) {
      browserText = "Diese Datei ist kein HTML/Text-Dokument.";
      browserTitle = "Nicht darstellbar";
      browserLinkCount = 0;
    } else {
      parseHtmlStream(http.getStreamPtr(), http);
      saveBrowserCache();
    }
  } else {
    String error = "HTTP-Fehler " + String(status) + ".";
    browserTitle = "Netzwerkfehler";
    if (loadBrowserCache()) {
      browserText = "Aktuelle Seite nicht erreichbar (" + error + ").\n\n--- Gespeicherte Seite ---\n" + browserText;
    } else {
      browserText = error;
    }
  }
  http.end();
  browserLoading = false;
  scrollPosition = scrollTarget = 0;
  redrawRequested = true;
}

void drawBrowser()
{
  drawAppFrame("Browser", "nur HTML");
  int top = contentTop() + 5;
  int buttonW = landscape ? 48 : 58;
  drawRoundCard(8, top, screenW() - buttonW - 18, 31, colors.card, colors.cardAlt);
  drawText(browserUrl, 15, top + 10, 1, colors.text);
  drawRoundCard(screenW() - buttonW - 4, top, buttonW - 4, 31, colors.accent);
  drawCentered(browserLoading ? "..." : "LOS", screenW() - buttonW / 2 - 6, top + 16, 1, TFT_WHITE);
  int linksH = min(static_cast<int>(browserLinkCount) * (landscape ? 20 : 24) + (browserLinkCount ? 17 : 0), landscape ? 90 : 125);
  int textTop = top + 39;
  int textBottom = contentBottom() - linksH - 4;
  drawRoundCard(8, textTop, screenW() - 16, textBottom - textTop, colors.card, colors.cardAlt);
  drawText(browserTitle, 15, textTop + 6, 1, colors.accent);
  int available = max(1, (textBottom - textTop - 24) / (landscape ? 13 : 15));
  drawWrapped(browserText, 15, textTop + 20, screenW() - 30, landscape ? 13 : 15, available,
              colors.text, 1, static_cast<int>(scrollPosition), &browserTotalLines);
  if (browserLinkCount) {
    int y = contentBottom() - linksH;
    drawText("Links", 12, y, 1, colors.muted);
    y += 13;
    for (uint8_t i = 0; i < browserLinkCount; ++i) {
      int rowH = landscape ? 19 : 23;
      drawRoundCard(9, y + i * rowH, screenW() - 18, rowH - 2, colors.cardAlt);
      drawText(browserLinkLabels[i], 16, y + i * rowH + 4, 1, colors.accent2);
    }
  }
}





















// ---------------------------------------------------------------------------
// Profile/security, app overview and SD app interpreter UI
// ---------------------------------------------------------------------------
void patternCenter(uint8_t node, int &x, int &y, int top, int spacing)
{
  x = screenW() / 2 + (static_cast<int>(node % 3) - 1) * spacing;
  y = top + (static_cast<int>(node / 3)) * spacing;
}

int patternNodeAt(int x, int y, int top, int spacing)
{
  for (uint8_t node = 0; node < 9; ++node) {
    int px, py;
    patternCenter(node, px, py, top, spacing);
    int dx = x - px, dy = y - py;
    if (dx * dx + dy * dy <= 22 * 22) return node;
  }
  return -1;
}

void drawPatternGrid(const String &sequence, int top, int spacing, int currentX, int currentY)
{
  for (uint8_t i = 1; i < sequence.length(); ++i) {
    int first = sequence[i - 1] - '0';
    int second = sequence[i] - '0';
    if (first < 0 || first > 8 || second < 0 || second > 8) continue;
    int x1, y1, x2, y2;
    patternCenter(first, x1, y1, top, spacing);
    patternCenter(second, x2, y2, top, spacing);
    tft.drawLine(x1, y1, x2, y2, colors.accent);
  }
  if (sequence.length() && currentX >= 0) {
    int last = sequence[sequence.length() - 1] - '0';
    if (last >= 0 && last <= 8) {
      int x, y;
      patternCenter(last, x, y, top, spacing);
      tft.drawLine(x, y, currentX, currentY, colors.accent);
    }
  }
  for (uint8_t node = 0; node < 9; ++node) {
    int x, y;
    patternCenter(node, x, y, top, spacing);
    bool active = sequence.indexOf(static_cast<char>('0' + node)) >= 0;
    tft.fillCircle(x, y, 13, active ? colors.accent : colors.cardAlt);
    tft.drawCircle(x, y, 16, colors.muted);
  }
}

void drawProfile()
{
  drawAppFrame("Profil", profileConfigured ? "gesichert" : "einrichten");
  if (profilePatternEdit) {
    drawCentered("Neues Muster zeichnen", screenW() / 2, contentTop() + 18, 2, colors.text);
    drawCentered("Mindestens vier Punkte", screenW() / 2, contentTop() + 43, 1, colors.muted);
    drawRoundCard(10, contentTop() + 54, 84, 24, colors.cardAlt);
    drawCentered("ABBRUCH", 52, contentTop() + 66, 1, colors.text);
    int spacing = landscape ? 54 : 72;
    int top = contentTop() + (landscape ? 92 : 115);
    drawPatternGrid(profilePatternCandidate, top, spacing);
    drawCentered("Beim Loslassen wird es gespeichert", screenW() / 2,
                 min(contentBottom() - 12, top + spacing * 3 + 4), 1, colors.muted);
    return;
  }
  int y = contentTop() + 12;
  drawRoundCard(12, y, screenW() - 24, landscape ? 57 : 75, colors.card, colors.cardAlt);
  tft.fillCircle(42, y + (landscape ? 28 : 37), 20, colors.accent);
  drawCentered("U", 42, y + (landscape ? 28 : 37), 2, TFT_WHITE);
  drawText(profileName, 75, y + 16, 2, colors.text);
  drawText(profileConfigured ? (profileUsesPattern ? "Entsperrmuster aktiv" : "PIN-Sperre aktiv") : "Noch keine Sperre eingerichtet",
           75, y + (landscape ? 38 : 48), 1, colors.muted);
  y += landscape ? 68 : 88;
  const int rowH = landscape ? 33 : 42;
  const char *labels[] = {"Name aendern", profileUsesPattern ? "Auf PIN umstellen" : "Auf Muster umstellen",
                          profileUsesPattern ? "Muster neu setzen" : "PIN neu setzen", "Jetzt sperren"};
  for (uint8_t i = 0; i < 4; ++i) {
    drawRoundCard(12, y + i * rowH, screenW() - 24, rowH - 5,
                  i == 3 ? colors.accent : colors.card, i == 3 ? 0 : colors.cardAlt);
    drawText(labels[i], 21, y + i * rowH + (landscape ? 8 : 11), 1, i == 3 ? TFT_WHITE : colors.text);
    if (i == 1) drawRight("ANTIPPEN", screenW() - 21, y + i * rowH + (landscape ? 8 : 11), 1, colors.muted);
  }
}

void drawLockScreen()
{
  drawBackground();
  drawStatusBar();
  const int w = screenW();
  drawCentered("PocketOS", w / 2, statusHeight() + 31, 2, colors.text);
  drawCentered(profileName, w / 2, statusHeight() + 55, 1, colors.muted);
  if (profileUsesPattern) {
    drawCentered("Muster zeichnen", w / 2, contentTop() + 8, 1, colors.muted);
    int spacing = landscape ? 48 : 62;
    int top = contentTop() + (landscape ? 52 : 80);
    drawPatternGrid(lockPatternInput, top, spacing, touch.down ? touch.lastX : -1, touch.down ? touch.lastY : -1);
    drawCentered("Mindestens vier Punkte", w / 2, min(contentBottom() - 15, top + spacing * 3 + 4), 1, colors.muted);
  } else {
    drawCentered("PIN eingeben", w / 2, contentTop() + 18, 1, colors.muted);
    String masked;
    for (uint8_t i = 0; i < lockPinInput.length(); ++i) masked += '*';
    drawCentered(masked.length() ? masked : "- - - -", w / 2, contentTop() + 47, 2, colors.text);
    int padTop = contentTop() + (landscape ? 67 : 93);
    int gap = 8;
    int buttonW = min(58, (w - gap * 4) / 3);
    int buttonH = landscape ? 28 : 38;
    int originX = (w - (buttonW * 3 + gap * 2)) / 2;
    for (uint8_t number = 0; number < 10; ++number) {
      int col = number == 9 ? 1 : number % 3;
      int row = number == 9 ? 3 : number / 3;
      int x = originX + col * (buttonW + gap);
      int y = padTop + row * (buttonH + gap);
      drawRoundCard(x, y, buttonW, buttonH, colors.card, colors.cardAlt);
      drawCentered(String(number), x + buttonW / 2, y + buttonH / 2, 2, colors.text);
    }
    int bottom = padTop + 3 * (buttonH + gap);
    drawRoundCard(originX, bottom, buttonW, buttonH, colors.danger);
    drawCentered("<", originX + buttonW / 2, bottom + buttonH / 2, 2, TFT_WHITE);
    drawRoundCard(originX + 2 * (buttonW + gap), bottom, buttonW, buttonH, colors.accent);
    drawCentered("OK", originX + 2 * (buttonW + gap) + buttonW / 2, bottom + buttonH / 2, 1, TFT_WHITE);
  }
}

void drawRecents()
{
  drawAppFrame("Geoeffnete Apps", "Uebersicht");
  if (!recentCount) {
    drawCentered("Noch keine Apps geoeffnet", screenW() / 2, contentTop() + 55, 1, colors.muted);
    return;
  }
  const int columns = landscape ? 3 : 2;
  const int gap = 10;
  const int cardW = (screenW() - gap * (columns + 1)) / columns;
  const int cardH = landscape ? 67 : 96;
  for (uint8_t i = 0; i < recentCount; ++i) {
    int col = i % columns;
    int row = i / columns;
    int x = gap + col * (cardW + gap);
    int y = contentTop() + 10 + row * (cardH + gap);
    if (y + cardH > contentBottom()) continue;
    drawRoundCard(x, y, cardW, cardH, colors.card, colors.cardAlt);
    tft.fillCircle(x + cardW / 2, y + (landscape ? 24 : 34), landscape ? 15 : 20, shortcutColor(i));
    drawCentered("APP", x + cardW / 2, y + (landscape ? 24 : 34), 1, TFT_WHITE);
    drawCentered(screenTitle(recentScreens[i]), x + cardW / 2, y + cardH - (landscape ? 14 : 18), 1, colors.text);
  }
}

void drawApps()
{
  drawAppFrame("SD-Apps", String(addonCount) + " installiert");
  const int rowH = landscape ? 34 : 43;
  const int start = contentTop() + 7;
  int first = static_cast<int>(scrollPosition);
  float fraction = scrollPosition - first;
  for (uint8_t i = 0; i < addonCount; ++i) {
    int y = start + static_cast<int>((static_cast<int>(i) - first - fraction) * rowH);
    if (y + rowH < start || y > contentBottom()) continue;
    drawRoundCard(9, y, screenW() - 18, rowH - 5, colors.card, colors.cardAlt);
    tft.fillCircle(27, y + (rowH - 5) / 2, landscape ? 10 : 13, shortcutColor(i + 3));
    drawCentered("+", 27, y + (rowH - 5) / 2, 2, TFT_WHITE);
    drawText(addonApps[i].title, 48, y + (landscape ? 8 : 11), 1, colors.text);
    drawRight(baseName(addonApps[i].path), screenW() - 16, y + (landscape ? 8 : 11), 1, colors.muted);
  }
  if (!addonCount) {
    drawCentered("Keine .papp-Dateien in /Apps", screenW() / 2, start + 40, 1, colors.muted);
    drawCentered("Ein Beispiel wird beim SD-Start angelegt.", screenW() / 2, start + 62, 1, colors.muted);
  }
  drawRoundCard(10, contentBottom() - 34, screenW() - 20, 28, colors.accent);
  drawCentered("APP-STORE (GITHUB)", screenW() / 2, contentBottom() - 20, 1, TFT_WHITE);
}

void drawAppStore()
{
  drawAppFrame("App-Store", "GitHub");
  if (storeLoading) { drawCentered("Lade App-Liste...", screenW() / 2, contentTop() + 48, 1, colors.muted); return; }
  const int rowH = landscape ? 42 : 52;
  for (uint8_t i = 0; i < storeCount; ++i) {
    int y = contentTop() + 6 + i * rowH;
    if (y + rowH > contentBottom()) continue;
    drawRoundCard(8, y, screenW() - 16, rowH - 5, colors.card, colors.cardAlt);
    drawText(storeItems[i].title, 18, y + 7, 1, colors.text);
    drawText(storeItems[i].description, 18, y + 24, 1, colors.muted);
    drawRight("LADEN", screenW() - 17, y + 15, 1, colors.accent2);
  }
  if (!storeCount) drawCentered(storeMessage, screenW() / 2, contentTop() + 50, 1, colors.muted);
}

uint16_t appAccentColor(const String &name)
{
  String value = lowerCopy(name);
  if (value == "green" || value == "mint") return colors.accent2;
  if (value == "red") return colors.danger;
  if (value == "orange") return tft.color565(244, 151, 65);
  if (value == "purple") return tft.color565(164, 100, 224);
  if (value == "blue" || value == "indigo") return colors.accent;
  return colors.accent;
}

String csvField(const String &source, uint8_t index)
{
  int start = 0;
  uint8_t current = 0;
  for (int i = 0; i <= static_cast<int>(source.length()); ++i) {
    if (i == static_cast<int>(source.length()) || source[i] == ',') {
      if (current == index) return source.substring(start, i);
      ++current;
      start = i + 1;
    }
  }
  return "";
}

void addRuntimeButton(int x, int y, int w, int h, const String &label, const String &action, uint16_t accent)
{
  if (appButtonCount < MAX_APP_BUTTONS) {
    AppButton &button = appButtons[appButtonCount++];
    button.x = x; button.y = y; button.w = w; button.h = h;
    safeCopy(button.label, sizeof(button.label), label);
    safeCopy(button.action, sizeof(button.action), action);
  }
  drawRoundCard(x, y, w, h, accent);
  drawCentered(label, x + w / 2, y + h / 2, 1, TFT_WHITE);
}
