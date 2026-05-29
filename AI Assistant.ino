#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <vector>
#include <stdarg.h>

#include "Arduino_GFX_Library.h"
#include "Arduino_DriveBus_Library.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "XPowersLib.h"
#include "HWCDC.h"

// ==========================================================
// AIWatch Buddy UI No-LVGL V1
// - No LVGL: direct Arduino_GFX drawing
// - Safe-area UI for rounded AMOLED corners
// - Simple text buddy face
// - Full-screen custom keyboard
// - Deepgram + AssemblyAI STT
// - OpenRouter with 3 AI models
// ==========================================================

// ===================== BOARD PINS =====================
#define LCD_SDIO0   4
#define LCD_SDIO1   5
#define LCD_SDIO2   6
#define LCD_SDIO3   7
#define LCD_RESET   8
#define TP_RESET    9
#define PWR_BUTTON  10
#define LCD_SCLK    11
#define LCD_CS      12
#define LCD_TE      13

#define IIC_SDA     15
#define IIC_SCL     14

#define I2S_MCLK    16
#define SD_CS       17

#define BOOT_BTN    0
#define IMU_INT     21
#define RTC_INT     39
#define TP_INT      38

#define I2S_DSDIN   42
#define I2S_SCLK    41
#define I2S_ASDOUT  40
#define I2S_LRCK    45
#define PA_CTRL     46

#define LCD_WIDTH   410
#define LCD_HEIGHT  502
#define FT3168_DEVICE_ADDRESS 0x38

// Safe area for rounded display corners
#define SAFE_X      24
#define SAFE_Y      30
#define SAFE_W      362
#define SAFE_H      440
#define SAFE_RIGHT  (SAFE_X + SAFE_W)
#define SAFE_BOTTOM (SAFE_Y + SAFE_H)

// ===================== APP INFO =====================
#define FIRMWARE_VERSION "0.5.0-side-counter-only"

// ===================== CODEC ADDRESSES =====================
#define ES7210_ADDR  0x40
#define ES8311_ADDR  0x18

// ===================== DEBUG CDC =====================
HWCDC USBSerial;

// ===================== GLOBAL DISPLAY / TOUCH =====================
Arduino_DataBus *bus = nullptr;
// Use the concrete CO5300 type, not Arduino_GFX*, because brightness is
// a panel-specific method and is not exposed on the Arduino_GFX base class.
Arduino_CO5300 *gfx = nullptr;

std::shared_ptr<Arduino_IIC_DriveBus> IICBus;
std::unique_ptr<Arduino_IIC> touch;

XPowersAXP2101 power;
bool pmuReady = false;

volatile uint32_t te_count = 0;
volatile uint32_t te_timeout_count = 0;

// ===================== WIFI / CONFIG =====================
Preferences prefs;
WebServer server(80);
DNSServer dnsServer;
bool portalRunning = false;
bool portalSaved = false;
unsigned long portalStartMs = 0;
const unsigned long PORTAL_TIMEOUT_MS = 10UL * 60UL * 1000UL;

// ===================== SETTINGS STORAGE =====================
const char *NS_CFG           = "cfg";
const char *NS_SETTINGS      = "settings";

const char *KEY_WIFI1SSID    = "wifi1ssid";
const char *KEY_WIFI1PASS    = "wifi1pass";
const char *KEY_WIFI2SSID    = "wifi2ssid";
const char *KEY_WIFI2PASS    = "wifi2pass";
const char *KEY_WIFI3SSID    = "wifi3ssid";
const char *KEY_WIFI3PASS    = "wifi3pass";

const char *KEY_OR_API       = "orapi";
const char *KEY_OR_MODEL1    = "ormodel1";
const char *KEY_OR_MODEL2    = "ormodel2";
const char *KEY_OR_MODEL3    = "ormodel3";

const char *KEY_ASSEMBLY     = "assembly";
const char *KEY_DEEPGRAM     = "deepgram";

const char *KEY_AI_MODE      = "aimode";
const char *KEY_STT_MODE     = "sttmode";
const char *KEY_AI_FALLBACK  = "aifb";
const char *KEY_MAX_TOKENS    = "maxtok";
const char *KEY_STT_FALLBACK = "sttfb";
const char *KEY_AUTO_SEND    = "autosend";
const char *KEY_RECORD_SEC   = "recsec";
const char *KEY_THEME        = "theme";
const char *KEY_FONT_SIZE    = "fontsize";
const char *KEY_KB_MODE      = "kbmode";
const char *KEY_DEBUG_LOGS   = "dbglogs";
const char *KEY_BRIGHTNESS   = "bright";
const char *KEY_WIFI_SAVER   = "wifisaver";
const char *KEY_SLEEP_MODE   = "sleepmode";
const char *KEY_BOOT_SLEEP   = "bootsleep";
const char *KEY_LASTNET      = "lastnet";
const char *KEY_SYSTEMPROMPT = "persona";
const char *KEY_ASSISTANT_NAME = "asstname";

// ===================== RUNTIME CONFIG =====================
String WIFI1SSID, WIFI1PASS;
String WIFI2SSID, WIFI2PASS;
String WIFI3SSID, WIFI3PASS;

String OPENROUTER_API_KEY;
String OR_MODEL1 = "openai/gpt-4.1-mini";
String OR_MODEL2 = "deepseek/deepseek-chat";
String OR_MODEL3 = "meta-llama/llama-3.1-8b-instruct";

String ASSEMBLY_API_KEY;
String DEEPGRAM_API_KEY;

String SYSTEM_PROMPT = "You are a friendly wearable AI buddy. Be concise, helpful, and natural.";
String ASSISTANT_NAME = "Buddy";

enum AIMode {
  AI_MODE_SMART = 0,
  AI_MODE_FAST = 1,
  AI_MODE_CREATIVE = 2
};

enum STTMode {
  STT_MODE_AUTO = 0,
  STT_MODE_DEEPGRAM = 1,
  STT_MODE_ASSEMBLY = 2
};

enum ThemeMode {
  THEME_BUDDY_DARK = 0,   // Claude Buddy warm dark
  THEME_OLED_BLACK = 1,
  THEME_SOFT_LIGHT = 2,
  THEME_CYBER_MINT = 3,
  THEME_ROSE_NIGHT = 4
};

const int THEME_COUNT = 5;

enum KeyboardMode {
  KB_MODE_FULLSCREEN = 0,
  KB_MODE_OFF = 1
};

AIMode currentAI = AI_MODE_SMART;
STTMode currentSTT = STT_MODE_AUTO;
bool aiFallbackEnabled = true;
int maxOutputTokens = 800;
bool sttFallbackEnabled = true;
bool autoSendAfterSTT = false;
bool debugLogsEnabled = true;
int brightnessValue = 180;
bool wifiSaverEnabled = true;
int sleepTimeoutMode = 2; // 0 off, 1 30s, 2 1m, 3 3m, 4 5m
bool bootButtonSleepEnabled = true;
int themeMode = THEME_BUDDY_DARK;
int fontSizeMode = 1; // 0 small, 1 medium, 2 large
int keyboardMode = KB_MODE_FULLSCREEN;
int lastSuccessfulNetwork = 1;
uint32_t recordSeconds = 3;

// ===================== APP STATE =====================
enum AppState {
  STATE_CHAT = 0,
  STATE_SETTINGS = 1,
  STATE_RECORDING = 2,
  STATE_TRANSCRIBING = 3,
  STATE_STREAMING = 4,
  STATE_PORTAL = 5
};

volatile AppState appState = STATE_CHAT;
bool gBusy = false;
volatile bool cancelRequested = false;
uint32_t lastRunHeapKB = 0;
uint32_t lastRunPsramKB = 0;
String lastRunMemNote = "No run yet";
bool isRecording = false;
bool networkReady = false;

// Header-visible WiFi state. WiFi.status() alone cannot show the in-between
// states the user cares about, like auto-connecting or a recent failed attempt.
volatile bool wifiConnecting = false;
unsigned long wifiErrorUntilMs = 0;

// ===================== BATTERY SAVER / SLEEP =====================
const unsigned long TRANSCRIPT_WIFI_HOLD_MS = 10000UL; // keep WiFi briefly only after transcript preview
bool transcriptWifiHoldActive = false;
unsigned long transcriptWifiHoldUntilMs = 0;

bool screenSleeping = false;
unsigned long lastInteractionMs = 0;
unsigned long lastBootButtonMs = 0;
bool bootButtonWasDown = false;
const unsigned long BOOT_BUTTON_DEBOUNCE_MS = 320;

// ===================== UI SCREENS =====================
enum UIScreen {
  UI_HOME = 0,
  UI_CHAT,
  UI_KEYBOARD,
  UI_SETTINGS_HOME,
  UI_SETTINGS_VOICE,
  UI_SETTINGS_AI,
  UI_SETTINGS_DISPLAY,
  UI_SETTINGS_WIFI,
  UI_SETTINGS_DEBUG,
  UI_SETTINGS_ABOUT,
  UI_LISTENING,
  UI_TRANSCRIBING,
  UI_THINKING,
  UI_ERROR_SCREEN
};

UIScreen uiScreen = UI_HOME;
UIScreen lastScreen = UI_HOME;
volatile bool uiDirty = true;          // full-screen redraw
volatile bool uiProgressDirty = false;  // progress/work-card redraw only
SemaphoreHandle_t ui_mutex = nullptr;

// ===================== CONVERSATION =====================
struct ConversationPair {
  String user;
  String assistant;
};

std::vector<ConversationPair> convHistory;
const size_t MAX_CONVERSATIONS = 8;
String inputDraft = "";
String keyboardDraft = "";
String lastTranscript = "";
String lastError = "";
String liveStatus = "Booting...";
String workNotice = "";

// ===================== ON-SCREEN LIVE STEP FEED =====================
// This mirrors useful USBSerial/debug milestones onto the AMOLED display.
// It is intentionally NOT a fake progress bar. It is a rolling step feed:
// ACTIVE / OK / NOTE / ERROR, with exact technical messages when possible.
enum WorkLogState {
  WORKLOG_ACTIVE = 0,
  WORKLOG_DONE   = 1,
  WORKLOG_NOTE   = 2,
  WORKLOG_ERROR  = 3
};

const int WORK_LOG_MAX = 7;
String workLogTitle = "";
String workLogLines[WORK_LOG_MAX];
uint8_t workLogStates[WORK_LOG_MAX];
int workLogCount = 0;
String lastWorkLogLine = "";

// Extra robust UI copy of the latest exchange.
// This avoids the chat screen appearing blank if vector/String state gets disturbed
// by a long network response or redraw timing.
String visibleUserText = "";
String visibleAssistantText = "";
bool visibleAiPending = false;
int chatScroll = 0;

// Single-conversation history navigation.
// 0 = latest conversation, 1 = previous, etc. This replaces the old on-screen
// ^ / v text-scroll buttons with BACK / NEXT style conversation browsing.
int chatHistoryOffset = 0;
int chatHistoryMaxOffsetCached = 0;

// Cached wrapping for the single-conversation chat view.
// Text wrapping allocates many small String objects; rebuilding it on every
// animation frame causes avoidable heap churn and stutter. Rebuild only when
// the selected conversation text, width, or text size changes.
bool chatWrapCacheValid = false;
String chatWrapCacheText = "";
int chatWrapCacheW = 0;
uint8_t chatWrapCacheSize = 0;
std::vector<String> chatWrapCacheLines;
uint32_t chatWrapCacheBuilds = 0;


// ===================== AUDIO =====================
static int16_t *audioBuffer = nullptr;
static size_t audioBufferSamples = 0;
const uint32_t RECORD_SAMPLE_RATE = 16000;
const uint32_t STT_TASK_STACK_BYTES = 24576;
const uint32_t REC_TASK_STACK_BYTES = 16384;

i2s_chan_handle_t rx_chan = nullptr;
i2s_chan_handle_t tx_chan = nullptr;
bool audioReady = false;

// ===================== MIC DEBUG =====================
bool micDebugEnabled = true;
volatile int16_t g_lastPeakL = 0;
volatile int16_t g_lastPeakR = 0;
volatile int g_lastAvgL = 0;
volatile int g_lastAvgR = 0;
unsigned long g_lastMicDebugPrint = 0;

// ===================== CUSTOM UI BUTTONS =====================
struct UIButton {
  String id;
  int x;
  int y;
  int w;
  int h;
  String label;
};

std::vector<UIButton> uiButtons;
int keyboardPage = 0; // 0 ABC, 1 numbers/symbols
unsigned long lastTouchMs = 0;
unsigned long lastAnimMs = 0;
int animStep = 0;
bool touchHeld = false;
const unsigned long TOUCH_DEBOUNCE_MS = 320;


// ===================== ANDROID-STYLE CHAT SCROLL =====================
// Phase 1 scrolling: drag-scroll the existing chat text area.
// This does not change ConversationPair storage yet and keeps ^ / v as fallback.
bool chatTouchScrollActive = false;
bool chatTouchScrollMoved = false;
int chatTouchStartY = 0;
int chatTouchLastY = 0;
int chatTouchAccumPx = 0;

bool chatScrollAreaValid = false;
int chatScrollAreaX = 0;
int chatScrollAreaY = 0;
int chatScrollAreaW = 0;
int chatScrollAreaH = 0;
int chatMaxScrollCached = 0;

const int CHAT_DRAG_THRESHOLD_PX = 10;
const int CHAT_SCROLL_LINE_PX = 26; // chat text size=2 uses 10*2+6 line height

// ===================== LIVE UX / ANIMATION STATE =====================
// Kept separate from chat storage so we can add polish without changing
// ConversationPair, chatScroll, or the current ^ / v scrolling system.
enum WorkStage {
  WORK_IDLE = 0,
  WORK_RECORDING,
  WORK_AUDIO_READY,
  WORK_STT_PREPARE,
  WORK_STT_DEEPGRAM_CONNECT,
  WORK_STT_DEEPGRAM_UPLOAD,
  WORK_STT_DEEPGRAM_WAIT,
  WORK_STT_ASSEMBLY_BUILD,
  WORK_STT_ASSEMBLY_UPLOAD,
  WORK_STT_ASSEMBLY_WAIT,
  WORK_STT_DONE,
  WORK_AI_WIFI,
  WORK_AI_BUILD,
  WORK_AI_PRIMARY,
  WORK_AI_WAIT,
  WORK_AI_FALLBACK,
  WORK_AI_DONE,
  WORK_ERROR
};

volatile WorkStage workStage = WORK_IDLE;

// Tracks when the current visible loading stage started. This is UI-only;
// it does not change STT/AI/network logic.
static unsigned long uiPhaseStartMs = 0;
static WorkStage uiPhaseStage = WORK_IDLE;

void markUiPhaseStart(WorkStage stage) {
  if (uiPhaseStartMs == 0 || uiPhaseStage != stage) {
    uiPhaseStage = stage;
    uiPhaseStartMs = millis();
  }
}

uint32_t uiElapsedSec() {
  if (uiPhaseStartMs == 0) return 0;
  return (millis() - uiPhaseStartMs) / 1000;
}

String formatMMSS(uint32_t sec) {
  uint32_t mm = sec / 60;
  uint32_t ss = sec % 60;
  char buf[16];
  snprintf(buf, sizeof(buf), "%lu:%02lu", (unsigned long)mm, (unsigned long)ss);
  return String(buf);
}


void requestRedraw();
void requestProgressRedraw();
bool canUseProgressOnlyRedraw();
void drawProgressOnly();
void resetSmoothProgress(int kind, int startTarget);
void setSmoothProgress(int target, int cap, bool crawl);
void setSmoothProgressByBytes(int base, int span, size_t sent, size_t total, int cap);
void tickSmoothProgressAnimation();
int smoothProgressPercent();
void applySmoothProgressForStage(WorkStage stage);
void setWorkStage(WorkStage stage, const String &statusText);
bool shouldCancelWork();
void requestCancelWork();
void captureRunMemory(const String &note);
void invalidateChatWrapCache();
void markUserActivity();
void enterScreenSleep();
void enterScreenSleep(bool forceManual);
void wakeScreen();
void checkAutoScreenSleep();
void handleBootButtonSleepWake();
void cancelTranscriptWifiHold();
void startTranscriptWifiHold();
void wifiSaverDisconnectNow(const String &reason);
void maybeAutoDisconnectWiFi();
String sleepTimeoutName();
uint32_t sleepTimeoutMs();
void cycleSleepTimeout();
void saveRuntimeSettings();
void disconnectWiFi();

// ===================== SEGMENTED PROGRESS UI =====================
// One clean segmented bar for small watch screens. This replaces the old
// user-facing live-log card. USBSerial/debug logs still remain unchanged.
enum SegProgressKind {
  SEG_PROGRESS_NONE = 0,
  SEG_PROGRESS_STT,
  SEG_PROGRESS_AI
};

volatile SegProgressKind activeSegProgressKind = SEG_PROGRESS_NONE;
volatile int activeSegProgressStep = 1;

// ===================== GAME-STYLE SMOOTH PROGRESS =====================
// The old segmented bar only showed which step was active. This layer gives
// the watch a real loading-screen feel: real milestones push the target
// forward, uploads use actual bytes sent, and long waits crawl toward a safe
// cap without ever reaching 100% until the result is actually ready.
volatile int smoothProgressVisual = 0;      // user-visible value, 0-1000
volatile int smoothProgressTarget = 0;      // easing target, 0-1000
volatile int smoothProgressCap    = 1000;   // maximum allowed during this stage
volatile bool smoothProgressCrawl = false;  // slow movement while waiting
volatile unsigned long smoothProgressLastCrawlMs = 0; // throttle crawl so it cannot fake-race to 100%

const int PROGRESS_FULL = 1000;

const int STT_SEGMENT_COUNT = 7;
const int AI_SEGMENT_COUNT  = 7;

bool shouldCancelWork() {
  return cancelRequested;
}

void captureRunMemory(const String &note) {
  lastRunHeapKB = ESP.getFreeHeap() / 1024;
  lastRunPsramKB = ESP.getFreePsram() / 1024;
  lastRunMemNote = note + " | H:" + String(lastRunHeapKB) + "KB P:" + String(lastRunPsramKB) + "KB";
  USBSerial.println("[MEMRUN] " + lastRunMemNote);
}

void requestCancelWork() {
  if (!gBusy) return;
  cancelRequested = true;
  liveStatus = "Canceling...";
  setWorkStage(WORK_ERROR, "Canceling...");
}

void invalidateChatWrapCache() {
  chatWrapCacheValid = false;
}


void resetSmoothProgress(int kind, int startTarget) {
  activeSegProgressKind = (SegProgressKind)kind;
  activeSegProgressStep = 1;
  smoothProgressVisual = constrain(startTarget, 0, PROGRESS_FULL);
  smoothProgressTarget = smoothProgressVisual;
  smoothProgressCap = PROGRESS_FULL;
  smoothProgressCrawl = false;
  smoothProgressLastCrawlMs = millis();
  uiPhaseStartMs = 0;
  requestProgressRedraw();
}

void setSmoothProgress(int target, int cap, bool crawl) {
  target = constrain(target, 0, PROGRESS_FULL);
  cap = constrain(cap, 0, PROGRESS_FULL);
  if (target > cap) target = cap;

  // Normal loading bars should not jump backwards. Fallbacks/retries may reuse
  // earlier stage names, but the user experience should still move forward.
  if (target < smoothProgressTarget) target = smoothProgressTarget;

  smoothProgressTarget = target;
  if (target >= PROGRESS_FULL) {
    // Completion should look complete immediately. Without this, a finished
    // transcript/reply can briefly show 85-95% because the visual easing has
    // not caught up yet.
    smoothProgressVisual = PROGRESS_FULL;
  }
  // Avoid std::max()/Arduino max() with volatile ints on ESP32 core 3.x.
  // Copy the volatile target to a plain int so C++ template deduction cannot fail.
  int targetSnapshot = smoothProgressTarget;
  smoothProgressCap = (cap > targetSnapshot) ? cap : targetSnapshot;
  smoothProgressCrawl = crawl;
  requestProgressRedraw();
}

void setSmoothProgressByBytes(int base, int span, size_t sent, size_t total, int cap) {
  int target = base;
  if (total > 0) {
    target = base + (int)(((uint64_t)sent * (uint64_t)span) / (uint64_t)total);
  }
  setSmoothProgress(target, cap, false);
}

void tickSmoothProgressAnimation() {
  int target = smoothProgressTarget;

  // Slow, capped crawl during server wait time. The earlier version advanced
  // the target on every redraw, which made the bar hit the high 90s even when
  // the device was still waiting on TLS/model/STT. This throttled crawl keeps
  // it alive without lying.
  if (smoothProgressCrawl && target < smoothProgressCap) {
    unsigned long now = millis();
    if (smoothProgressLastCrawlMs == 0 || now - smoothProgressLastCrawlMs >= 950) {
      smoothProgressLastCrawlMs = now;
      int room = smoothProgressCap - target;
      int bump = (room > 180) ? 3 : ((room > 70) ? 2 : 1);
      target += bump;
      if (target > smoothProgressCap) target = smoothProgressCap;
      smoothProgressTarget = target;
    }
  }

  int visual = smoothProgressVisual;
  if (visual < target) {
    int delta = target - visual;
    visual += max(2, delta / 7);
    if (visual > target) visual = target;
  }

  if (visual > smoothProgressCap) visual = smoothProgressCap;
  smoothProgressVisual = constrain(visual, 0, PROGRESS_FULL);
}

int smoothProgressPercent() {
  return constrain((smoothProgressVisual + 5) / 10, 0, 100);
}

void applySmoothProgressForStage(WorkStage stage) {
  switch (stage) {
    case WORK_RECORDING: setSmoothProgress(30, 110, true); break;
    case WORK_AUDIO_READY: setSmoothProgress(100, 160, false); break;
    case WORK_STT_PREPARE: setSmoothProgress(160, 250, true); break;
    case WORK_STT_DEEPGRAM_CONNECT: setSmoothProgress(280, 380, true); break;
    case WORK_STT_DEEPGRAM_UPLOAD: setSmoothProgress(340, 620, true); break;
    case WORK_STT_DEEPGRAM_WAIT: setSmoothProgress(700, 970, true); break;
    case WORK_STT_ASSEMBLY_BUILD: setSmoothProgress(260, 360, true); break;
    case WORK_STT_ASSEMBLY_UPLOAD: setSmoothProgress(340, 620, true); break;
    case WORK_STT_ASSEMBLY_WAIT: setSmoothProgress(700, 970, true); break;
    case WORK_STT_DONE: setSmoothProgress(PROGRESS_FULL, PROGRESS_FULL, false); break;

    case WORK_AI_WIFI: setSmoothProgress(80, 140, true); break;
    case WORK_AI_BUILD: setSmoothProgress(160, 240, true); break;
    case WORK_AI_PRIMARY: setSmoothProgress(280, 380, true); break;
    case WORK_AI_FALLBACK: setSmoothProgress(500, 970, true); break;
    case WORK_AI_WAIT: setSmoothProgress(700, 970, true); break;
    case WORK_AI_DONE: setSmoothProgress(PROGRESS_FULL, PROGRESS_FULL, false); break;

    case WORK_ERROR:
      smoothProgressCrawl = false;
      break;
    default:
      break;
  }
}

bool isSttWorkStage(WorkStage stage) {
  return stage == WORK_AUDIO_READY ||
         stage == WORK_STT_PREPARE ||
         stage == WORK_STT_DEEPGRAM_CONNECT ||
         stage == WORK_STT_DEEPGRAM_UPLOAD ||
         stage == WORK_STT_DEEPGRAM_WAIT ||
         stage == WORK_STT_ASSEMBLY_BUILD ||
         stage == WORK_STT_ASSEMBLY_UPLOAD ||
         stage == WORK_STT_ASSEMBLY_WAIT ||
         stage == WORK_STT_DONE;
}

bool isAiWorkStage(WorkStage stage) {
  return stage == WORK_AI_WIFI ||
         stage == WORK_AI_BUILD ||
         stage == WORK_AI_PRIMARY ||
         stage == WORK_AI_WAIT ||
         stage == WORK_AI_FALLBACK ||
         stage == WORK_AI_DONE;
}

int sttSegmentForStage(WorkStage stage) {
  switch (stage) {
    case WORK_AUDIO_READY: return 1;          // captured
    case WORK_STT_PREPARE: return 2;          // prepare audio
    case WORK_STT_DEEPGRAM_CONNECT:
    case WORK_STT_ASSEMBLY_BUILD: return 3;   // choose/connect STT
    case WORK_STT_DEEPGRAM_UPLOAD:
    case WORK_STT_ASSEMBLY_UPLOAD: return 4;  // send audio / create request
    case WORK_STT_DEEPGRAM_WAIT:
    case WORK_STT_ASSEMBLY_WAIT: return 5;    // waiting/polling transcript
    case WORK_STT_DONE: return 7;             // ready
    default: return activeSegProgressStep > 0 ? activeSegProgressStep : 1;
  }
}

int aiSegmentForStage(WorkStage stage) {
  switch (stage) {
    case WORK_AI_WIFI: return 1;       // check WiFi
    case WORK_AI_BUILD: return 2;      // build request
    case WORK_AI_PRIMARY:
    case WORK_AI_FALLBACK: return 3;   // model selected / connect
    case WORK_AI_WAIT: return 5;       // send/wait/parse shares this stage
    case WORK_AI_DONE: return 7;       // ready
    default: return activeSegProgressStep > 0 ? activeSegProgressStep : 1;
  }
}

void updateSegmentProgressFromStage(WorkStage stage) {
  if (isSttWorkStage(stage)) {
    int stageStep = sttSegmentForStage(stage);
    if (activeSegProgressKind == SEG_PROGRESS_STT &&
        activeSegProgressStep > stageStep &&
        stage != WORK_AUDIO_READY &&
        stage != WORK_STT_PREPARE &&
        stage != WORK_STT_DONE) {
      stageStep = activeSegProgressStep; // preserve manual steps like Read result
    }
    activeSegProgressKind = SEG_PROGRESS_STT;
    activeSegProgressStep = constrain(stageStep, 1, STT_SEGMENT_COUNT);
  } else if (isAiWorkStage(stage)) {
    int stageStep = aiSegmentForStage(stage);
    if (activeSegProgressKind == SEG_PROGRESS_AI &&
        activeSegProgressStep > stageStep &&
        stage != WORK_AI_WIFI &&
        stage != WORK_AI_BUILD &&
        stage != WORK_AI_DONE) {
      stageStep = activeSegProgressStep; // preserve manual steps like Send/Wait/Parse
    }
    activeSegProgressKind = SEG_PROGRESS_AI;
    activeSegProgressStep = constrain(stageStep, 1, AI_SEGMENT_COUNT);
  } else if (stage == WORK_ERROR) {
    // Keep previous kind/step so the red segment shows where the task failed.
    if (activeSegProgressKind == SEG_PROGRESS_NONE) {
      activeSegProgressKind = SEG_PROGRESS_AI;
      activeSegProgressStep = 4;
    }
  } else if (stage == WORK_IDLE) {
    activeSegProgressKind = SEG_PROGRESS_NONE;
    activeSegProgressStep = 1;
  }
}

void setSegmentProgress(int kind, int step) {
  activeSegProgressKind = (SegProgressKind)kind;
  int total = (kind == SEG_PROGRESS_STT) ? STT_SEGMENT_COUNT : AI_SEGMENT_COUNT;
  activeSegProgressStep = constrain(step, 1, total);

  // Manual network phases are more precise than WorkStage alone. Keep the
  // smooth bar in sync with Send / Wait / Parse so the screen tells the truth.
  if (kind == SEG_PROGRESS_AI) {
    if (activeSegProgressStep == 4) setSmoothProgress(340, 520, true);
    else if (activeSegProgressStep == 5) setSmoothProgress(700, 970, true);
    else if (activeSegProgressStep == 6) setSmoothProgress(970, 990, true);
    else if (activeSegProgressStep == 7) setSmoothProgress(PROGRESS_FULL, PROGRESS_FULL, false);
  } else if (kind == SEG_PROGRESS_STT) {
    if (activeSegProgressStep == 4) setSmoothProgress(340, 620, true);
    else if (activeSegProgressStep == 5) setSmoothProgress(700, 970, true);
    else if (activeSegProgressStep == 6) setSmoothProgress(970, 990, true);
    else if (activeSegProgressStep == 7) setSmoothProgress(PROGRESS_FULL, PROGRESS_FULL, false);
  }

  requestProgressRedraw();
}

String sttSegmentLabel(int step) {
  switch (step) {
    case 1: return "Audio captured";
    case 2: return "Prepare audio";
    case 3: return "Select STT";
    case 4: return "Send audio";
    case 5: return "Transcribe";
    case 6: return "Read result";
    case 7: return "Transcript ready";
    default: return "Voice step";
  }
}

String aiSegmentLabel(int step) {
  switch (step) {
    case 1: return "Check WiFi";
    case 2: return "Build request";
    case 3: return "Connect model";
    case 4: return "Send prompt";
    case 5: return "Wait reply";
    case 6: return "Parse reply";
    case 7: return "Reply ready";
    default: return "AI step";
  }
}

String progressHeadline(bool isVoiceWork, int step, WorkStage ws) {
  if (isVoiceWork) {
    switch (step) {
      case 1: return "Audio captured";
      case 2: return "Preparing audio";
      case 3: return (ws == WORK_STT_ASSEMBLY_BUILD) ? "Preparing fallback" : "Connecting STT";
      case 4: return "Uploading audio";
      case 5: return "Waiting transcript";
      case 6: return "Reading result";
      case 7: return "Transcript ready";
      default: return "Voice working";
    }
  }

  switch (step) {
    case 1: return "Checking WiFi";
    case 2: return "Building request";
    case 3: return (ws == WORK_AI_FALLBACK) ? "Trying fallback" : "Connecting model";
    case 4: return "Sending prompt";
    case 5: return "Waiting reply";
    case 6: return "Reading reply";
    case 7: return "Reply ready";
    default: return "AI working";
  }
}

String latestWorkLineForDisplay() {
  if (workLogCount <= 0) return "";
  String line = workLogLines[workLogCount - 1];
  line.trim();
  return line;
}


// ===================== UTILS =====================
struct HttpResult {
  int code;
  String body;
};

// Manual prototypes for functions returning HttpResult.
// Arduino's .ino preprocessor can generate prototypes before this struct,
// which makes the compiler say: "HttpResult does not name a type".
// Declaring them here prevents bad auto-prototypes.
HttpResult postJson(const char *url, const String &json);
HttpResult postJsonWithAuth(const char *url, const String &json, const String &apiKey);
HttpResult postBinary(const char *url, const uint8_t *data, size_t len, const char *contentType);
HttpResult postJsonAssembly(const char *url, const String &json, const String &apiKey);
HttpResult postBinaryAssemblyManualOnce(const uint8_t *data, size_t len, const String &apiKey, const char *contentType);
HttpResult postBinaryAssembly(const char *url, const uint8_t *data, size_t len, const String &apiKey, const char *contentType);
HttpResult getAssembly(const char *url, const String &apiKey);
HttpResult postOpenRouterManual(const String &json, const String &apiKey);

String htmlEscape(const String &s) {
  String out = s;
  out.replace("&", "&amp;");
  out.replace("<", "&lt;");
  out.replace(">", "&gt;");
  out.replace("\"", "&quot;");
  return out;
}

DynamicJsonDocument makeDocPsram(size_t cap) {
  return DynamicJsonDocument(cap);
}

void requestRedraw();

String trimForDisplay(String s, int maxLen = 86) {
  s.replace("\r", " ");
  s.replace("\n", " ");
  while (s.indexOf("  ") >= 0) s.replace("  ", " ");
  s.trim();
  if (s.length() > maxLen) s = s.substring(0, maxLen - 3) + "...";
  return s;
}

void resetWorkLog(const String &title) {
  workLogTitle = title;
  workLogCount = 0;
  lastWorkLogLine = "";
  for (int i = 0; i < WORK_LOG_MAX; i++) {
    workLogLines[i] = "";
    workLogStates[i] = WORKLOG_NOTE;
  }
  requestProgressRedraw();
}

void appendWorkLog(uint8_t state, const String &text) {
  String line = trimForDisplay(text);
  if (line.isEmpty()) return;

  // Do not spam identical lines. For active progress updates, refresh the last row.
  if (workLogCount > 0 && line == lastWorkLogLine) {
    workLogStates[workLogCount - 1] = state;
    requestProgressRedraw();
    return;
  }

  if (workLogCount > 0 && state == WORKLOG_ACTIVE && workLogStates[workLogCount - 1] == WORKLOG_ACTIVE) {
    String prev = workLogLines[workLogCount - 1];
    bool sameFamily = false;
    if (prev.startsWith("Assembly sending payload") && line.startsWith("Assembly sending payload")) sameFamily = true;
    if (prev.startsWith("Deepgram sending audio") && line.startsWith("Deepgram sending audio")) sameFamily = true;
    if (prev.startsWith("OpenRouter sending payload") && line.startsWith("OpenRouter sending payload")) sameFamily = true;
    if (prev.startsWith("Assembly polling transcript") && line.startsWith("Assembly polling transcript")) sameFamily = true;
    if (sameFamily) {
      workLogLines[workLogCount - 1] = line;
      workLogStates[workLogCount - 1] = state;
      lastWorkLogLine = line;
      requestProgressRedraw();
      return;
    }
  }

  if (workLogCount >= WORK_LOG_MAX) {
    for (int i = 1; i < WORK_LOG_MAX; i++) {
      workLogLines[i - 1] = workLogLines[i];
      workLogStates[i - 1] = workLogStates[i];
    }
    workLogCount = WORK_LOG_MAX - 1;
  }

  workLogLines[workLogCount] = line;
  workLogStates[workLogCount] = state;
  workLogCount++;
  lastWorkLogLine = line;
  requestProgressRedraw();
}

void activeWorkLog(const String &text) { appendWorkLog(WORKLOG_ACTIVE, text); }
void doneWorkLog(const String &text)   { appendWorkLog(WORKLOG_DONE, text); }
void noteWorkLog(const String &text)   { appendWorkLog(WORKLOG_NOTE, text); }
void errorWorkLog(const String &text)  { appendWorkLog(WORKLOG_ERROR, text); }

void requestRedraw() {
  uiDirty = true;
  uiProgressDirty = false;
}

bool canUseProgressOnlyRedraw() {
  if (!gfx) return false;
  if (uiDirty) return false;
  if (uiScreen == UI_TRANSCRIBING || uiScreen == UI_THINKING) return true;
  if (uiScreen == UI_CHAT && (appState == STATE_STREAMING || workStage == WORK_ERROR)) return true;
  return false;
}

void requestProgressRedraw() {
  if (canUseProgressOnlyRedraw()) uiProgressDirty = true;
  else requestRedraw();
}

void setStatus(const String &s) {
  liveStatus = s;
  requestRedraw();
}

void setWorkStage(WorkStage stage, const String &statusText) {
  updateSegmentProgressFromStage(stage);
  applySmoothProgressForStage(stage);
  workStage = stage;
  liveStatus = statusText;
  if (statusText.length()) {
    if (stage == WORK_ERROR) errorWorkLog(statusText);
    else if (stage == WORK_STT_DONE || stage == WORK_AI_DONE || stage == WORK_AUDIO_READY) doneWorkLog(statusText);
    else activeWorkLog(statusText);
  }

  // Stage changes during STT/AI are usually loader updates. Do not force
  // full-screen redraws here, or progress animation will still flicker.
  // requestProgressRedraw() safely falls back to full redraw when the current
  // screen/state cannot support a progress-only update.
  requestProgressRedraw();
}

void clearWorkNotice() {
  workNotice = "";
}

void setWorkNotice(const String &noticeText) {
  workNotice = noticeText;
  if (noticeText.length()) noteWorkLog(noticeText);
  requestProgressRedraw();
}

void addConversation(const String &user, const String &assistant) {
  if (convHistory.size() >= MAX_CONVERSATIONS) {
    convHistory.erase(convHistory.begin());
  }
  convHistory.push_back({user, assistant});
}

// ===================== UI STATE HELPERS =====================
// These helpers keep the common chat/result state updates in one place.
// Worker tasks should call them while holding ui_mutex. That avoids the old
// pattern where success/error/cancel paths each updated visible text, history,
// scroll, and cache slightly differently.
void setLatestChatViewLocked(const String &userText, const String &assistantText, bool pending) {
  visibleUserText = userText;
  visibleAssistantText = assistantText;
  visibleAiPending = pending;
  chatScroll = 0;
  chatHistoryOffset = 0;
  invalidateChatWrapCache();
}

bool replaceLastPendingConversationLocked(const String &userText, const String &assistantText) {
  if (!convHistory.empty()) {
    ConversationPair &last = convHistory.back();
    if (last.user == userText && last.assistant.length() == 0) {
      last.assistant = assistantText;
      return true;
    }
  }
  return false;
}

void upsertConversationLocked(const String &userText, const String &assistantText) {
  if (!replaceLastPendingConversationLocked(userText, assistantText)) {
    addConversation(userText, assistantText);
  }
}

void applyFinalChatTurnLocked(const String &userText, const String &assistantText) {
  setLatestChatViewLocked(userText, assistantText, false);
  upsertConversationLocked(userText, assistantText);
  uiScreen = UI_CHAT;
}

void applyTransientChatStatusLocked(const String &userText, const String &assistantText, bool pending) {
  setLatestChatViewLocked(userText, assistantText, pending);
  uiScreen = UI_CHAT;
}

String currentAiName() {
  if (currentAI == AI_MODE_SMART) return "Smart";
  if (currentAI == AI_MODE_FAST) return "Fast";
  return "Creative";
}

String modelForMode(AIMode mode) {
  if (mode == AI_MODE_SMART) return OR_MODEL1;
  if (mode == AI_MODE_FAST) return OR_MODEL2;
  return OR_MODEL3;
}

String currentSttName() {
  if (currentSTT == STT_MODE_AUTO) return "Auto";
  if (currentSTT == STT_MODE_ASSEMBLY) return "AssemblyAI";
  return "Deepgram";
}

String aiModeName(AIMode mode) {
  if (mode == AI_MODE_SMART) return "Smart";
  if (mode == AI_MODE_FAST) return "Fast";
  return "Creative";
}

String currentWorkStageName() {
  switch (workStage) {
    case WORK_RECORDING: return "Listening";
    case WORK_AUDIO_READY: return "Voice captured";
    case WORK_STT_PREPARE: return "Preparing voice";
    case WORK_STT_DEEPGRAM_CONNECT: return "Connecting STT";
    case WORK_STT_DEEPGRAM_UPLOAD: return "Uploading voice";
    case WORK_STT_DEEPGRAM_WAIT: return "Reading transcript";
    case WORK_STT_ASSEMBLY_BUILD: return "Preparing fallback";
    case WORK_STT_ASSEMBLY_UPLOAD: return "Uploading fallback";
    case WORK_STT_ASSEMBLY_WAIT: return "Waiting fallback";
    case WORK_STT_DONE: return "Transcript ready";
    case WORK_AI_WIFI: return "Checking WiFi";
    case WORK_AI_BUILD: return "Preparing request";
    case WORK_AI_PRIMARY: return "Asking primary";
    case WORK_AI_WAIT: return "Waiting for AI";
    case WORK_AI_FALLBACK: return "Trying fallback";
    case WORK_AI_DONE: return "Reply ready";
    case WORK_ERROR: return "Error found";
    default: return "Ready";
  }
}


String stageSubtitle() {
  switch ((WorkStage)workStage) {
    case WORK_RECORDING: return "Listening to mic";
    case WORK_AUDIO_READY: return "Audio captured";
    case WORK_STT_PREPARE: return "Preparing captured voice";
    case WORK_STT_DEEPGRAM_CONNECT: return "Deepgram manual TLS connect";
    case WORK_STT_DEEPGRAM_UPLOAD: return "Deepgram sending audio";
    case WORK_STT_DEEPGRAM_WAIT: return "Deepgram waiting response";
    case WORK_STT_ASSEMBLY_BUILD: return "Assembly preparing fallback";
    case WORK_STT_ASSEMBLY_UPLOAD: return "Assembly MANUAL upload";
    case WORK_STT_ASSEMBLY_WAIT: return "Assembly polling transcript";
    case WORK_STT_DONE: return "Transcript ready";
    case WORK_AI_WIFI: return "Checking WiFi";
    case WORK_AI_BUILD: return "OpenRouter build request";
    case WORK_AI_PRIMARY: return "OpenRouter primary model";
    case WORK_AI_WAIT: return "OpenRouter waiting response";
    case WORK_AI_FALLBACK: return "OpenRouter fallback model";
    case WORK_AI_DONE: return "AI reply ready";
    case WORK_ERROR: return lastError.length() ? trimForDisplay(lastError, 42) : "Error found";
    default: return "Working";
  }
}

String themeName() {
  switch (themeMode) {
    case THEME_OLED_BLACK: return "OLED Black";
    case THEME_SOFT_LIGHT: return "Soft Paper";
    case THEME_CYBER_MINT: return "Cyber Mint";
    case THEME_ROSE_NIGHT: return "Rose Night";
    default: return "Claude Buddy";
  }
}

String onOff(bool v) { return v ? "ON" : "OFF"; }

uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  if (!gfx) return 0;
  // Software brightness fallback for AMOLED.
  // Some Arduino_GFX/CO5300 installs do not expose Display_Brightness(),
  // so we scale UI colors instead. This compiles on the standard CO5300 class.
  uint16_t scale = constrain(brightnessValue, 20, 255);
  r = (uint8_t)(((uint16_t)r * scale) / 255);
  g = (uint8_t)(((uint16_t)g * scale) / 255);
  b = (uint8_t)(((uint16_t)b * scale) / 255);
  return gfx->color565(r, g, b);
}

// ===================== THEME LIBRARY V2 =====================
// Semantic colors. Screens should use these instead of raw RGB so every
// theme has real hierarchy: title/body/muted/card/accent/progress/error.
uint16_t cBg() {
  switch (themeMode) {
    case THEME_OLED_BLACK: return rgb(0, 0, 0);
    case THEME_SOFT_LIGHT: return rgb(245, 238, 224);
    case THEME_CYBER_MINT: return rgb(4, 14, 24);
    case THEME_ROSE_NIGHT: return rgb(22, 10, 22);
    default: return rgb(10, 9, 7); // Claude Buddy warm black
  }
}

uint16_t cBgElevated() {
  switch (themeMode) {
    case THEME_OLED_BLACK: return rgb(8, 8, 8);
    case THEME_SOFT_LIGHT: return rgb(252, 246, 236);
    case THEME_CYBER_MINT: return rgb(8, 25, 38);
    case THEME_ROSE_NIGHT: return rgb(34, 16, 34);
    default: return rgb(16, 13, 10);
  }
}

uint16_t cCard() {
  switch (themeMode) {
    case THEME_OLED_BLACK: return rgb(13, 13, 13);
    case THEME_SOFT_LIGHT: return rgb(255, 252, 246);
    case THEME_CYBER_MINT: return rgb(10, 32, 48);
    case THEME_ROSE_NIGHT: return rgb(42, 22, 42);
    default: return rgb(20, 17, 13);
  }
}

uint16_t cCard2() {
  switch (themeMode) {
    case THEME_OLED_BLACK: return rgb(28, 28, 28);
    case THEME_SOFT_LIGHT: return rgb(232, 222, 207);
    case THEME_CYBER_MINT: return rgb(18, 52, 68);
    case THEME_ROSE_NIGHT: return rgb(62, 32, 58);
    default: return rgb(42, 34, 25);
  }
}

uint16_t cCardActive() {
  switch (themeMode) {
    case THEME_OLED_BLACK: return rgb(42, 30, 22);
    case THEME_SOFT_LIGHT: return rgb(242, 223, 199);
    case THEME_CYBER_MINT: return rgb(18, 72, 74);
    case THEME_ROSE_NIGHT: return rgb(78, 36, 66);
    default: return rgb(54, 39, 27);
  }
}

uint16_t cLine() {
  switch (themeMode) {
    case THEME_OLED_BLACK: return rgb(48, 48, 48);
    case THEME_SOFT_LIGHT: return rgb(211, 198, 179);
    case THEME_CYBER_MINT: return rgb(34, 83, 95);
    case THEME_ROSE_NIGHT: return rgb(94, 52, 82);
    default: return rgb(61, 50, 38);
  }
}

uint16_t cDivider() { return cLine(); }

uint16_t cAccent() {
  switch (themeMode) {
    case THEME_OLED_BLACK: return rgb(255, 142, 76);
    case THEME_SOFT_LIGHT: return rgb(175, 89, 48);
    case THEME_CYBER_MINT: return rgb(62, 220, 190);
    case THEME_ROSE_NIGHT: return rgb(255, 118, 154);
    default: return rgb(226, 107, 64); // Claude orange
  }
}

uint16_t cAccent2() {
  switch (themeMode) {
    case THEME_OLED_BLACK: return rgb(255, 205, 96);
    case THEME_SOFT_LIGHT: return rgb(134, 83, 37);
    case THEME_CYBER_MINT: return rgb(124, 244, 216);
    case THEME_ROSE_NIGHT: return rgb(255, 180, 142);
    default: return rgb(245, 181, 82);
  }
}

uint16_t cProgress() { return cAccent(); }

uint16_t cProgressTrack() {
  switch (themeMode) {
    case THEME_OLED_BLACK: return rgb(32, 32, 32);
    case THEME_SOFT_LIGHT: return rgb(222, 211, 195);
    case THEME_CYBER_MINT: return rgb(20, 62, 72);
    case THEME_ROSE_NIGHT: return rgb(62, 34, 58);
    default: return rgb(49, 39, 29);
  }
}

uint16_t cText() {
  switch (themeMode) {
    case THEME_SOFT_LIGHT: return rgb(44, 35, 26);
    case THEME_CYBER_MINT: return rgb(220, 255, 245);
    case THEME_ROSE_NIGHT: return rgb(255, 231, 226);
    default: return rgb(245, 238, 225);
  }
}

uint16_t cTextStrong() {
  switch (themeMode) {
    case THEME_SOFT_LIGHT: return rgb(24, 20, 16);
    case THEME_CYBER_MINT: return rgb(240, 255, 252);
    case THEME_ROSE_NIGHT: return rgb(255, 244, 238);
    default: return rgb(255, 250, 240);
  }
}

uint16_t cTextSoft() {
  switch (themeMode) {
    case THEME_SOFT_LIGHT: return rgb(74, 61, 47);
    case THEME_CYBER_MINT: return rgb(182, 232, 220);
    case THEME_ROSE_NIGHT: return rgb(236, 205, 210);
    default: return rgb(224, 211, 193);
  }
}

uint16_t cMuted() {
  switch (themeMode) {
    case THEME_SOFT_LIGHT: return rgb(114, 98, 78);
    case THEME_CYBER_MINT: return rgb(116, 170, 166);
    case THEME_ROSE_NIGHT: return rgb(184, 136, 154);
    default: return rgb(158, 140, 118);
  }
}

uint16_t cDim() {
  switch (themeMode) {
    case THEME_SOFT_LIGHT: return rgb(162, 142, 116);
    case THEME_CYBER_MINT: return rgb(68, 114, 118);
    case THEME_ROSE_NIGHT: return rgb(120, 82, 104);
    default: return rgb(96, 82, 65);
  }
}

uint16_t cButton() {
  switch (themeMode) {
    case THEME_OLED_BLACK: return rgb(24, 24, 24);
    case THEME_SOFT_LIGHT: return rgb(236, 225, 209);
    case THEME_CYBER_MINT: return rgb(16, 46, 60);
    case THEME_ROSE_NIGHT: return rgb(58, 28, 52);
    default: return rgb(33, 27, 20);
  }
}

uint16_t cOnAccent() {
  switch (themeMode) {
    case THEME_CYBER_MINT: return rgb(2, 20, 20);
    case THEME_SOFT_LIGHT: return rgb(255, 250, 240);
    default: return rgb(24, 17, 10);
  }
}

uint16_t cGood() {
  switch (themeMode) {
    case THEME_SOFT_LIGHT: return rgb(64, 136, 91);
    case THEME_CYBER_MINT: return rgb(80, 230, 150);
    case THEME_ROSE_NIGHT: return rgb(174, 224, 122);
    default: return rgb(163, 215, 107);
  }
}

uint16_t cWarn() { return cAccent2(); }

uint16_t cError() {
  switch (themeMode) {
    case THEME_SOFT_LIGHT: return rgb(183, 61, 61);
    case THEME_CYBER_MINT: return rgb(255, 98, 128);
    case THEME_ROSE_NIGHT: return rgb(255, 100, 112);
    default: return rgb(255, 102, 96);
  }
}

uint16_t cReplyRailTrack() { return cProgressTrack(); }
uint16_t cReplyRailThumb() { return cAccent2(); }
String getIpString() {
  if (WiFi.status() == WL_CONNECTED) return WiFi.localIP().toString();
  return "Offline";
}

void audioLog(const String &s) {
  USBSerial.println(s);
}

void audioLogf(const char *fmt, ...) {
  char buf[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  USBSerial.println(buf);
}

void logMem(const char *tag) {
  if (!debugLogsEnabled) return;
  USBSerial.printf("[MEM] %s | heap=%u minHeap=%u largest=%u psram=%u minPsram=%u\n",
                   tag,
                   (unsigned)ESP.getFreeHeap(),
                   (unsigned)ESP.getMinFreeHeap(),
                   (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                   (unsigned)ESP.getFreePsram(),
                   (unsigned)ESP.getMinFreePsram());
}

void updateMicStatsFromStereo(const int16_t *stereoBuf, size_t stereoFrames) {
  int16_t peakL = 0, peakR = 0;
  int32_t sumL = 0, sumR = 0;

  for (size_t i = 0; i < stereoFrames; i++) {
    int16_t l = stereoBuf[i * 2];
    int16_t r = stereoBuf[i * 2 + 1];

    if (abs(l) > abs(peakL)) peakL = l;
    if (abs(r) > abs(peakR)) peakR = r;

    sumL += abs(l);
    sumR += abs(r);
  }

  g_lastPeakL = peakL;
  g_lastPeakR = peakR;
  g_lastAvgL = stereoFrames ? (sumL / stereoFrames) : 0;
  g_lastAvgR = stereoFrames ? (sumR / stereoFrames) : 0;
}

String micStatsShort() {
  return "Mic L:" + String((int)g_lastPeakL) +
         " R:" + String((int)g_lastPeakR) +
         " AL:" + String(g_lastAvgL) +
         " AR:" + String(g_lastAvgR);
}


// ===================== TOUCH / DISPLAY =====================
void ArduinoIICTouchInterrupt(void) {
  if (touch) touch->IIC_Interrupt_Flag = true;
}

bool initDisplayAndTouch() {
  bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
  gfx = new Arduino_CO5300(bus, LCD_RESET, 0, LCD_WIDTH, LCD_HEIGHT, 22, 0, 6, 6);

  if (!gfx->begin()) return false;
  gfx->fillScreen(rgb(0, 0, 0));

  Wire.begin(IIC_SDA, IIC_SCL);
  Wire.setClock(400000);

  pinMode(TP_RESET, OUTPUT);
  digitalWrite(TP_RESET, LOW);
  delay(10);
  digitalWrite(TP_RESET, HIGH);
  delay(50);

  IICBus = std::make_shared<Arduino_HWIIC>(IIC_SDA, IIC_SCL, &Wire);
  touch = std::unique_ptr<Arduino_IIC>(
    new Arduino_FT3x68(IICBus, FT3168_DEVICE_ADDRESS, 0xFF, TP_INT, ArduinoIICTouchInterrupt)
  );
  if (!touch->begin()) return false;

  pinMode(TP_INT, INPUT_PULLUP);
  return true;
}

bool readTouchPoint(int &x, int &y) {
  if (!touch) return false;

  bool hasInterrupt = touch->IIC_Interrupt_Flag;
  bool pinActive = (digitalRead(TP_INT) == LOW);
  if (!hasInterrupt && !pinActive) return false;
  touch->IIC_Interrupt_Flag = false;

  int32_t rawX = touch->IIC_Read_Device_Value(
    Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_X);
  int32_t rawY = touch->IIC_Read_Device_Value(
    Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_Y);

  if (rawX < 0 || rawY < 0) return false;

  int mx = map(rawX, 0, 390, 0, LCD_WIDTH - 1);
  int my = map(rawY, 0, 490, 0, LCD_HEIGHT - 1);
  x = constrain(mx, 0, LCD_WIDTH - 1);
  y = constrain(my, 0, LCD_HEIGHT - 1);
  return true;
}

// ===================== PMU =====================
bool initPMU() {
  if (!power.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL)) {
    return false;
  }

  power.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
  power.clearIrqStatus();
  power.setChargingLedMode(XPOWERS_CHG_LED_CTRL_CHG);
  power.setDC3Voltage(3300);
  power.enableDC3();
  power.setALDO1Voltage(1800);
  power.enableALDO1();
  power.setBLDO1Voltage(3300);
  power.enableBLDO1();
  power.setSysPowerDownVoltage(2600);

  return true;
}

void applyBrightness(int val) {
  brightnessValue = constrain(val, 20, 255);

  // Compile-safe brightness handling:
  // This Arduino_GFX/CO5300 install does not expose Display_Brightness().
  // The UI brightness is handled by rgb() scaling all drawn colors.
  // If you later install the Waveshare-modified GFX library, we can switch
  // this back to the hardware panel brightness command.

  prefs.begin(NS_SETTINGS, false);
  prefs.putInt(KEY_BRIGHTNESS, brightnessValue);
  prefs.putBool(KEY_WIFI_SAVER, wifiSaverEnabled);
  prefs.putInt(KEY_SLEEP_MODE, sleepTimeoutMode);
  prefs.putBool(KEY_BOOT_SLEEP, bootButtonSleepEnabled);
  prefs.end();
}

int getBatteryPercent() {
  if (!pmuReady) return -1;
  if (power.isBatteryConnect()) return power.getBatteryPercent();
  return -1;
}


// ===================== BATTERY SAVER HELPERS =====================
uint32_t sleepTimeoutMs() {
  switch (sleepTimeoutMode) {
    case 1: return 30UL * 1000UL;
    case 2: return 60UL * 1000UL;
    case 3: return 3UL * 60UL * 1000UL;
    case 4: return 5UL * 60UL * 1000UL;
    default: return 0;
  }
}

String sleepTimeoutName() {
  switch (sleepTimeoutMode) {
    case 1: return "30 sec";
    case 2: return "1 min";
    case 3: return "3 min";
    case 4: return "5 min";
    default: return "Off";
  }
}

void cycleSleepTimeout() {
  sleepTimeoutMode = (sleepTimeoutMode + 1) % 5;
  saveRuntimeSettings();
}

bool canScreenSleepNow() {
  if (screenSleeping) return false;
  if (gBusy || portalRunning) return false;
  if (uiScreen == UI_KEYBOARD || uiScreen == UI_LISTENING || uiScreen == UI_TRANSCRIBING || uiScreen == UI_THINKING) return false;
  if (appState == STATE_RECORDING || appState == STATE_TRANSCRIBING || appState == STATE_STREAMING || appState == STATE_PORTAL) return false;
  return true;
}

void cancelTranscriptWifiHold() {
  transcriptWifiHoldActive = false;
  transcriptWifiHoldUntilMs = 0;
}

void wifiSaverDisconnectNow(const String &reason) {
  cancelTranscriptWifiHold();
  if (!wifiSaverEnabled || portalRunning) return;
  if (WiFi.status() == WL_CONNECTED || networkReady) {
    USBSerial.println("[PWR] WiFi saver disconnect: " + reason);
    disconnectWiFi();
    if (reason.length()) liveStatus = reason;
    requestRedraw();
  }
}

void startTranscriptWifiHold() {
  if (!wifiSaverEnabled || portalRunning) return;
  if (WiFi.status() == WL_CONNECTED) {
    transcriptWifiHoldActive = true;
    transcriptWifiHoldUntilMs = millis() + TRANSCRIPT_WIFI_HOLD_MS;
    liveStatus = "Transcript ready - WiFi 10s";
    USBSerial.println("[PWR] WiFi saver: transcript hold 10s");
    requestRedraw();
  }
}

void maybeAutoDisconnectWiFi() {
  if (!wifiSaverEnabled || portalRunning || gBusy) return;
  if (WiFi.status() != WL_CONNECTED) return;

  if (transcriptWifiHoldActive) {
    if ((long)(millis() - transcriptWifiHoldUntilMs) >= 0) {
      wifiSaverDisconnectNow("WiFi off");
    }
    return;
  }

  // If something left WiFi on while idle, shut it down. Network work will
  // reconnect through ensureWiFiConnection() and still use the existing
  // WiFi 1/2/3 fallback order.
  if (appState == STATE_CHAT && uiScreen != UI_SETTINGS_WIFI) {
    wifiSaverDisconnectNow("WiFi off");
  }
}

void markUserActivity() {
  lastInteractionMs = millis();
}

void enterScreenSleep(bool forceManual) {
  if (screenSleeping) return;

  // Auto-sleep stays conservative, but a BOOT-button/manual sleep is allowed
  // during STT/AI work. The background task keeps running; we only black the
  // AMOLED and pause UI drawing. Do not sleep the config portal because AP/DNS
  // portal screens need to stay visible and interactive.
  if (portalRunning || appState == STATE_PORTAL) return;
  if (!forceManual && !canScreenSleepNow()) return;

  screenSleeping = true;
  cancelTranscriptWifiHold();

  // Only shut WiFi off when truly idle. If the user manually sleeps during
  // transcribing/AI, disconnecting here would break the active request.
  if (wifiSaverEnabled && !gBusy &&
      appState != STATE_TRANSCRIBING && appState != STATE_STREAMING && appState != STATE_RECORDING) {
    wifiSaverDisconnectNow("Sleep - WiFi off");
  }

  uiDirty = false;
  uiProgressDirty = false;
  if (gfx) gfx->fillScreen(rgb(0, 0, 0));
  USBSerial.println(forceManual ? "[PWR] Manual screen sleep" : "[PWR] Auto screen sleep");
}

void enterScreenSleep() {
  enterScreenSleep(false);
}

void wakeScreen() {
  if (!screenSleeping) return;
  screenSleeping = false;
  markUserActivity();
  USBSerial.println("[PWR] Screen wake");
  requestRedraw();
}

void checkAutoScreenSleep() {
  uint32_t timeout = sleepTimeoutMs();
  if (timeout == 0 || screenSleeping) return;
  if (!canScreenSleepNow()) {
    markUserActivity();
    return;
  }
  if (lastInteractionMs == 0) markUserActivity();
  if (millis() - lastInteractionMs >= timeout) {
    enterScreenSleep();
  }
}

void handleBootButtonSleepWake() {
  bool down = (digitalRead(BOOT_BTN) == LOW);
  unsigned long now = millis();

  if (down && !bootButtonWasDown && now - lastBootButtonMs > BOOT_BUTTON_DEBOUNCE_MS) {
    lastBootButtonMs = now;
    bootButtonWasDown = true;

    if (screenSleeping) {
      wakeScreen();
      return;
    }

    markUserActivity();
    if (bootButtonSleepEnabled) {
      enterScreenSleep(true);
    }
    return;
  }

  if (!down) bootButtonWasDown = false;
}

// ===================== CONFIG LOAD/SAVE =====================
void loadConfig() {
  prefs.begin(NS_CFG, true);
  WIFI1SSID = prefs.getString(KEY_WIFI1SSID, "");
  WIFI1PASS = prefs.getString(KEY_WIFI1PASS, "");
  WIFI2SSID = prefs.getString(KEY_WIFI2SSID, "");
  WIFI2PASS = prefs.getString(KEY_WIFI2PASS, "");
  WIFI3SSID = prefs.getString(KEY_WIFI3SSID, "");
  WIFI3PASS = prefs.getString(KEY_WIFI3PASS, "");

  OPENROUTER_API_KEY = prefs.getString(KEY_OR_API, "");
  OR_MODEL1 = prefs.getString(KEY_OR_MODEL1, OR_MODEL1);
  OR_MODEL2 = prefs.getString(KEY_OR_MODEL2, OR_MODEL2);
  OR_MODEL3 = prefs.getString(KEY_OR_MODEL3, OR_MODEL3);

  ASSEMBLY_API_KEY = prefs.getString(KEY_ASSEMBLY, "");
  DEEPGRAM_API_KEY = prefs.getString(KEY_DEEPGRAM, "");

  SYSTEM_PROMPT = prefs.getString(KEY_SYSTEMPROMPT, SYSTEM_PROMPT);
  ASSISTANT_NAME = prefs.getString(KEY_ASSISTANT_NAME, ASSISTANT_NAME);
  currentAI = (AIMode)prefs.getInt(KEY_AI_MODE, AI_MODE_SMART);
  currentSTT = (STTMode)prefs.getInt(KEY_STT_MODE, STT_MODE_AUTO);
  aiFallbackEnabled = prefs.getBool(KEY_AI_FALLBACK, true);
  maxOutputTokens = prefs.getInt(KEY_MAX_TOKENS, 800);
  sttFallbackEnabled = prefs.getBool(KEY_STT_FALLBACK, true);
  autoSendAfterSTT = prefs.getBool(KEY_AUTO_SEND, false);
  debugLogsEnabled = prefs.getBool(KEY_DEBUG_LOGS, true);
  recordSeconds = prefs.getUInt(KEY_RECORD_SEC, 3);
  themeMode = prefs.getInt(KEY_THEME, THEME_BUDDY_DARK);
  fontSizeMode = prefs.getInt(KEY_FONT_SIZE, 1);
  keyboardMode = prefs.getInt(KEY_KB_MODE, KB_MODE_FULLSCREEN);
  lastSuccessfulNetwork = prefs.getInt(KEY_LASTNET, 1);
  prefs.end();

  prefs.begin(NS_SETTINGS, true);
  brightnessValue = prefs.getInt(KEY_BRIGHTNESS, 180);
  wifiSaverEnabled = prefs.getBool(KEY_WIFI_SAVER, true);
  sleepTimeoutMode = prefs.getInt(KEY_SLEEP_MODE, 2);
  bootButtonSleepEnabled = prefs.getBool(KEY_BOOT_SLEEP, true);
  prefs.end();

  recordSeconds = constrain(recordSeconds, (uint32_t)2, (uint32_t)6);
  if ((int)currentAI < 0 || (int)currentAI > 2) currentAI = AI_MODE_SMART;
  if ((int)currentSTT < 0 || (int)currentSTT > 2) currentSTT = STT_MODE_AUTO;
  themeMode = constrain(themeMode, 0, THEME_COUNT - 1);
  maxOutputTokens = constrain(maxOutputTokens, 300, 1600);
  fontSizeMode = constrain(fontSizeMode, 0, 2);
  keyboardMode = constrain(keyboardMode, 0, 1);
  sleepTimeoutMode = constrain(sleepTimeoutMode, 0, 4);
}


void saveConfig() {
  prefs.begin(NS_CFG, false);
  prefs.putString(KEY_WIFI1SSID, WIFI1SSID);
  prefs.putString(KEY_WIFI1PASS, WIFI1PASS);
  prefs.putString(KEY_WIFI2SSID, WIFI2SSID);
  prefs.putString(KEY_WIFI2PASS, WIFI2PASS);
  prefs.putString(KEY_WIFI3SSID, WIFI3SSID);
  prefs.putString(KEY_WIFI3PASS, WIFI3PASS);

  prefs.putString(KEY_OR_API, OPENROUTER_API_KEY);
  prefs.putString(KEY_OR_MODEL1, OR_MODEL1);
  prefs.putString(KEY_OR_MODEL2, OR_MODEL2);
  prefs.putString(KEY_OR_MODEL3, OR_MODEL3);

  prefs.putString(KEY_ASSEMBLY, ASSEMBLY_API_KEY);
  prefs.putString(KEY_DEEPGRAM, DEEPGRAM_API_KEY);

  prefs.putString(KEY_SYSTEMPROMPT, SYSTEM_PROMPT);
  prefs.putString(KEY_ASSISTANT_NAME, ASSISTANT_NAME);
  prefs.putInt(KEY_AI_MODE, (int)currentAI);
  prefs.putInt(KEY_STT_MODE, (int)currentSTT);
  prefs.putBool(KEY_AI_FALLBACK, aiFallbackEnabled);
  prefs.putInt(KEY_MAX_TOKENS, maxOutputTokens);
  prefs.putBool(KEY_STT_FALLBACK, sttFallbackEnabled);
  prefs.putBool(KEY_AUTO_SEND, autoSendAfterSTT);
  prefs.putBool(KEY_DEBUG_LOGS, debugLogsEnabled);
  prefs.putUInt(KEY_RECORD_SEC, recordSeconds);
  prefs.putInt(KEY_THEME, themeMode);
  prefs.putInt(KEY_FONT_SIZE, fontSizeMode);
  prefs.putInt(KEY_KB_MODE, keyboardMode);
  prefs.putInt(KEY_LASTNET, lastSuccessfulNetwork);
  prefs.end();

  prefs.begin(NS_SETTINGS, false);
  prefs.putInt(KEY_BRIGHTNESS, brightnessValue);
  prefs.putBool(KEY_WIFI_SAVER, wifiSaverEnabled);
  prefs.putInt(KEY_SLEEP_MODE, sleepTimeoutMode);
  prefs.putBool(KEY_BOOT_SLEEP, bootButtonSleepEnabled);
  prefs.end();
}

// ===================== WIFI =====================
void disconnectWiFi() {
  wifiConnecting = false;
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  networkReady = false;
}

void getNetworkCredentials(int idx, String &ssid, String &pass) {
  switch (idx) {
    case 1: ssid = WIFI1SSID; pass = WIFI1PASS; break;
    case 2: ssid = WIFI2SSID; pass = WIFI2PASS; break;
    case 3: ssid = WIFI3SSID; pass = WIFI3PASS; break;
    default: ssid = ""; pass = ""; break;
  }
}

bool ensureWiFiConnection() {
  markUserActivity();
  cancelTranscriptWifiHold();
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnecting = false;
    wifiErrorUntilMs = 0;
    networkReady = true;
    return true;
  }

  wifiConnecting = true;
  wifiErrorUntilMs = 0;
  requestRedraw();

  String ssid, pass;
  int order[3] = { lastSuccessfulNetwork, 1, 2 };
  if (lastSuccessfulNetwork == 1) { order[0] = 1; order[1] = 2; order[2] = 3; }
  if (lastSuccessfulNetwork == 2) { order[0] = 2; order[1] = 1; order[2] = 3; }
  if (lastSuccessfulNetwork == 3) { order[0] = 3; order[1] = 1; order[2] = 2; }

  for (int i = 0; i < 3; i++) {
    getNetworkCredentials(order[i], ssid, pass);
    if (ssid.isEmpty()) continue;

    setStatus("Connecting " + ssid + "...");
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());

    int tries = 0;
    while (WiFi.status() != WL_CONNECTED && tries < 20) {
      delay(500);
      tries++;
    }

    if (WiFi.status() == WL_CONNECTED) {
      lastSuccessfulNetwork = order[i];
      saveConfig();
      wifiConnecting = false;
      wifiErrorUntilMs = 0;
      networkReady = true;
      setStatus("WiFi " + WiFi.localIP().toString());
      return true;
    }

    WiFi.disconnect(true);
    delay(200);
  }

  wifiConnecting = false;
  wifiErrorUntilMs = millis() + 8000UL;
  setStatus("WiFi failed");
  networkReady = false;
  return false;
}

// ===================== HTTP HELPERS =====================
HttpResult postJson(const char *url, const String &json) {
  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();

  HttpResult r{-1, ""};
  if (!http.begin(client, url)) { r.body = "begin failed"; return r; }

  http.addHeader("Content-Type", "application/json");
  http.setReuse(false);
  http.setTimeout(45000);
  r.code = http.POST((uint8_t *)json.c_str(), json.length());
  r.body = (r.code > 0) ? http.getString() : http.errorToString(r.code);
  http.end();
  return r;
}

HttpResult postJsonWithAuth(const char *url, const String &json, const String &apiKey) {
  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();

  HttpResult r{-1, ""};
  if (!http.begin(client, url)) { r.body = "begin failed"; return r; }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + apiKey);
  http.setReuse(false);
  http.setTimeout(45000);
  r.code = http.POST((uint8_t *)json.c_str(), json.length());
  r.body = (r.code > 0) ? http.getString() : http.errorToString(r.code);
  http.end();
  return r;
}

HttpResult postBinary(const char *url, const uint8_t *data, size_t len, const char *contentType) {
  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();

  HttpResult r{-1, ""};
  if (!http.begin(client, url)) { r.body = "begin failed"; return r; }

  http.addHeader("Content-Type", contentType);
  http.setReuse(false);
  http.setTimeout(45000);
  USBSerial.printf("[HTTP] POST binary len=%u type=%s\n", (unsigned)len, contentType);
  logMem("before binary POST");
  r.code = http.POST((uint8_t*)data, len);
  r.body = (r.code > 0) ? http.getString() : http.errorToString(r.code);
  logMem("after binary POST");
  http.end();
  return r;
}

HttpResult postJsonAssembly(const char *url, const String &json, const String &apiKey) {
  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();

  HttpResult r{-1, ""};
  if (!http.begin(client, url)) { r.body = "begin failed"; return r; }

  http.addHeader("authorization", apiKey);
  http.addHeader("Content-Type", "application/json");
  http.setReuse(false);
  http.setTimeout(45000);
  r.code = http.POST((uint8_t *)json.c_str(), json.length());
  r.body = (r.code > 0) ? http.getString() : http.errorToString(r.code);
  http.end();
  return r;
}

String readTlsLineYielding(WiFiClientSecure &client, uint32_t timeoutMs);

String dechunkHttpBodyIfNeeded(const String &raw) {
  if (raw.length() < 5) return raw;

  int pos = 0;
  String out;
  out.reserve(raw.length());

  while (pos < (int)raw.length()) {
    int lineEnd = raw.indexOf("\r\n", pos);
    if (lineEnd < 0) return raw;

    String sizeLine = raw.substring(pos, lineEnd);
    sizeLine.trim();
    int semi = sizeLine.indexOf(';');
    if (semi >= 0) sizeLine = sizeLine.substring(0, semi);
    if (sizeLine.length() == 0) return raw;

    char *endp = nullptr;
    long chunkSize = strtol(sizeLine.c_str(), &endp, 16);
    if (endp == sizeLine.c_str()) return raw;
    pos = lineEnd + 2;

    if (chunkSize == 0) {
      return out.length() ? out : raw;
    }

    if (chunkSize < 0 || pos + chunkSize > (int)raw.length()) {
      return raw;
    }

    out += raw.substring(pos, pos + chunkSize);
    pos += chunkSize;

    if (pos + 2 <= (int)raw.length() && raw.substring(pos, pos + 2) == "\r\n") {
      pos += 2;
    } else {
      return raw;
    }
  }

  return out.length() ? out : raw;
}

HttpResult postBinaryAssemblyManualOnce(const uint8_t *data, size_t len,
                                        const String &apiKey, const char *contentType) {
  HttpResult r{-1, ""};

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(45000);

  USBSerial.printf("[HTTP] Assembly MANUAL upload len=%u type=%s\n",
                   (unsigned)len, contentType);
  setSegmentProgress(SEG_PROGRESS_STT, 4);
  setSmoothProgress(350, 440, true);
  activeWorkLog("Assembly MANUAL upload len=" + String((unsigned)len));
  logMem("before Assembly manual upload");

  if (shouldCancelWork()) { r.code = -9; r.body = "Canceled"; return r; }
  if (!client.connect("api.assemblyai.com", 443)) {
    r.body = "Assembly TLS connect failed";
    errorWorkLog("Assembly TLS connect failed");
    return r;
  }
  doneWorkLog("Assembly TLS connected");
  setSmoothProgress(330, 430, false);

  String header;
  header.reserve(512);
  header += "POST /v2/upload HTTP/1.1\r\n";
  header += "Host: api.assemblyai.com\r\n";
  header += "authorization: " + apiKey + "\r\n";
  header += "Content-Type: ";
  header += contentType;
  header += "\r\n";
  header += "Accept: application/json\r\n";
  header += "Connection: close\r\n";
  header += "Content-Length: " + String(len) + "\r\n\r\n";

  client.print(header);
  delay(1);
  yield();
  setSmoothProgress(340, 620, true);
  activeWorkLog("Assembly sending payload 0/" + String((unsigned)len));

  size_t sent = 0;
  unsigned long lastProgress = millis();
  while (sent < len) {
    if (shouldCancelWork()) { r.code = -9; r.body = "Canceled"; client.stop(); return r; }
    size_t chunk = min((size_t)1024, len - sent);
    size_t wrote = client.write(data + sent, chunk);

    if (wrote == 0) {
      r.code = -3;
      r.body = "Assembly send payload failed at " + String(sent) + "/" + String(len);
      errorWorkLog("Assembly failed payload at " + String((unsigned)sent) + "/" + String((unsigned)len));
      client.stop();
      return r;
    }

    sent += wrote;
    setSmoothProgressByBytes(340, 280, sent, len, 620);

    if (millis() - lastProgress > 1000) {
      lastProgress = millis();
      USBSerial.printf("[HTTP] Assembly sent %u/%u bytes\n",
                       (unsigned)sent, (unsigned)len);
      activeWorkLog("Assembly sending payload " + String((unsigned)sent) + "/" + String((unsigned)len));
    }

    delay(1);
    yield();
  }

  USBSerial.println("[HTTP] Assembly payload sent. Waiting response...");
  doneWorkLog("Assembly sent " + String((unsigned)sent) + " bytes");
  setSegmentProgress(SEG_PROGRESS_STT, 5);
  setSmoothProgress(700, 970, true);
  activeWorkLog("Assembly waiting response");

  if (shouldCancelWork()) { r.code = -9; r.body = "Canceled"; client.stop(); return r; }
  String status = readTlsLineYielding(client, 20000);
  USBSerial.print("[HTTP] Assembly status line: ");
  USBSerial.println(status);

  int code = -1;
  int sp1 = status.indexOf(' ');
  if (sp1 >= 0 && status.length() >= sp1 + 4) {
    code = status.substring(sp1 + 1, sp1 + 4).toInt();
  }
  r.code = code;
  doneWorkLog("Assembly HTTP status " + String(code));

  bool chunked = false;
  int contentLength = -1;

  while (client.connected()) {
    String line = readTlsLineYielding(client, 12000);
    if (line.length() == 0) break;
    String lower = line;
    lower.toLowerCase();
    if (lower.indexOf("transfer-encoding:") >= 0 && lower.indexOf("chunked") >= 0) {
      chunked = true;
    }
    if (lower.startsWith("content-length:")) {
      String n = line.substring(line.indexOf(':') + 1);
      n.trim();
      contentLength = n.toInt();
    }
    delay(1);
    yield();
  }

  String rawBody;
  rawBody.reserve(contentLength > 0 ? min(contentLength + 16, 8192) : 2048);

  unsigned long lastData = millis();
  while ((client.connected() || client.available()) && millis() - lastData < 45000) {
    if (shouldCancelWork()) { r.code = -9; r.body = "Canceled"; client.stop(); return r; }
    while (client.available()) {
      char c = (char)client.read();
      if (rawBody.length() < 8192) rawBody += c;
      lastData = millis();
    }
    delay(1);
    yield();
  }

  client.stop();

  r.body = chunked ? dechunkHttpBodyIfNeeded(rawBody) : rawBody;
  setSegmentProgress(SEG_PROGRESS_STT, 5);
  setSmoothProgress(900, 970, false);
  doneWorkLog("Assembly manual upload done bodyLen=" + String((unsigned)r.body.length()));

  USBSerial.printf("[HTTP] Assembly manual done code=%d rawBodyLen=%u bodyLen=%u chunked=%d\n",
                   r.code, (unsigned)rawBody.length(), (unsigned)r.body.length(), (int)chunked);
  logMem("after Assembly manual upload");

  if (r.body.length() == 0 && r.code > 0) {
    r.body = "Assembly empty body";
  }

  return r;
}

HttpResult postBinaryAssembly(const char *url, const uint8_t *data, size_t len,
                               const String &apiKey, const char *contentType) {
  (void)url;

  HttpResult r = postBinaryAssemblyManualOnce(data, len, apiKey, contentType);

  if (r.code == -3 || r.body.indexOf("send payload failed") >= 0) {
    USBSerial.println("[HTTP] Assembly upload failed while sending. Retrying once...");
    noteWorkLog("Assembly retry after payload failure");
    delay(500);
    yield();
    if (WiFi.status() == WL_CONNECTED) {
      r = postBinaryAssemblyManualOnce(data, len, apiKey, contentType);
    }
  }

  return r;
}

HttpResult getAssembly(const char *url, const String &apiKey) {
  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();

  HttpResult r{-1, ""};
  if (!http.begin(client, url)) { r.body = "begin failed"; return r; }

  http.addHeader("authorization", apiKey);
  http.setReuse(false);
  http.setTimeout(45000);
  r.code = http.GET();
  r.body = (r.code > 0) ? http.getString() : http.errorToString(r.code);
  http.end();
  return r;
}


String readTlsLineYielding(WiFiClientSecure &client, uint32_t timeoutMs = 12000) {
  String line;
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    while (client.available()) {
      char c = (char)client.read();
      if (c == '\n') return line;
      if (c != '\r') line += c;
      if (line.length() > 600) return line;
    }
    delay(1);
    yield();
  }
  return line;
}

HttpResult postOpenRouterManual(const String &json, const String &apiKey) {
  HttpResult r{-1, ""};

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(45000);

  USBSerial.printf("[AI] Manual OpenRouter connect. bodyLen=%u heap=%u psram=%u\n",
                   (unsigned)json.length(), (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());
  setSegmentProgress(SEG_PROGRESS_AI, 3);
  setSmoothProgress(280, 380, true);
  activeWorkLog("OpenRouter connect bodyLen=" + String((unsigned)json.length()));

  if (shouldCancelWork()) { r.code = -9; r.body = "Canceled"; return r; }
  if (!client.connect("openrouter.ai", 443)) {
    r.body = "OpenRouter TLS connect failed";
    errorWorkLog("OpenRouter TLS connect failed");
    return r;
  }
  doneWorkLog("OpenRouter TLS connected");
  setSmoothProgress(330, 430, false);

  String header;
  header.reserve(512);
  header += "POST /api/v1/chat/completions HTTP/1.1\r\n";
  header += "Host: openrouter.ai\r\n";
  header += "Authorization: Bearer " + apiKey + "\r\n";
  header += "Content-Type: application/json\r\n";
  header += "Accept: application/json\r\n";
  header += "Connection: close\r\n";
  header += "Content-Length: " + String(json.length()) + "\r\n\r\n";

  client.print(header);
  delay(1);
  yield();
  setSegmentProgress(SEG_PROGRESS_AI, 4);
  setSmoothProgress(340, 520, true);
  activeWorkLog("OpenRouter sending payload 0/" + String((unsigned)json.length()));

  const uint8_t *data = (const uint8_t *)json.c_str();
  size_t total = json.length();
  size_t sent = 0;
  unsigned long lastProgress = millis();

  while (sent < total) {
    if (shouldCancelWork()) { r.code = -9; r.body = "Canceled"; client.stop(); return r; }
    size_t chunk = min((size_t)768, total - sent);
    size_t wrote = client.write(data + sent, chunk);
    if (wrote == 0) {
      r.code = -3;
      r.body = "OpenRouter send payload failed at " + String(sent) + "/" + String(total);
      errorWorkLog("OpenRouter failed payload at " + String((unsigned)sent) + "/" + String((unsigned)total));
      client.stop();
      return r;
    }
    sent += wrote;
    setSmoothProgressByBytes(340, 180, sent, total, 520);

    if (millis() - lastProgress > 1000) {
      lastProgress = millis();
      USBSerial.printf("[AI] OpenRouter sent %u/%u bytes\n", (unsigned)sent, (unsigned)total);
      activeWorkLog("OpenRouter sending payload " + String((unsigned)sent) + "/" + String((unsigned)total));
    }

    delay(1);
    yield();
  }

  USBSerial.println("[AI] OpenRouter payload sent. Waiting response...");
  doneWorkLog("OpenRouter payload sent");
  setSegmentProgress(SEG_PROGRESS_AI, 5);
  setSmoothProgress(700, 970, true);
  activeWorkLog("OpenRouter waiting response");

  if (shouldCancelWork()) { r.code = -9; r.body = "Canceled"; client.stop(); return r; }
  String status = readTlsLineYielding(client, 20000);
  USBSerial.print("[AI] OpenRouter status line: ");
  USBSerial.println(status);

  int code = -1;
  int sp1 = status.indexOf(' ');
  if (sp1 >= 0 && status.length() >= sp1 + 4) {
    code = status.substring(sp1 + 1, sp1 + 4).toInt();
  }
  r.code = code;
  doneWorkLog("OpenRouter HTTP status " + String(code));
  setSmoothProgress(820, 970, true);

  // Skip response headers. Keep yielding so CPU0 idle watchdog is not starved.
  while (client.connected()) {
    String line = readTlsLineYielding(client, 12000);
    if (line.length() == 0) break;
    delay(1);
    yield();
  }

  setSegmentProgress(SEG_PROGRESS_AI, 6);
  setSmoothProgress(970, 990, true);
  activeWorkLog("OpenRouter reading body");
  String body;
  body.reserve(8192);
  unsigned long lastData = millis();
  while ((client.connected() || client.available()) && millis() - lastData < 45000) {
    if (shouldCancelWork()) { r.code = -9; r.body = "Canceled"; client.stop(); return r; }
    while (client.available()) {
      char c = (char)client.read();
      if (body.length() < 32768) body += c;
      lastData = millis();
    }
    delay(1);
    yield();
  }
  client.stop();

  r.body = body;
  setSmoothProgress(980, 990, false);
  doneWorkLog("OpenRouter done bodyLen=" + String((unsigned)r.body.length()));
  USBSerial.printf("[AI] Manual OpenRouter done code=%d bodyLen=%u heap=%u psram=%u\n",
                   r.code, (unsigned)r.body.length(), (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());

  if (r.body.length() == 0 && r.code > 0) {
    r.body = "OpenRouter empty body";
    errorWorkLog("OpenRouter empty body");
  }

  return r;
}

// ===================== AI REQUEST BODIES =====================
String openrouterBodyWithContext(const String &userText, const String &modelId) {
  DynamicJsonDocument d = makeDocPsram(32768);

  d["model"] = modelId;
  JsonArray msgs = d.createNestedArray("messages");

  JsonObject sys = msgs.createNestedObject();
  sys["role"] = "system";
  sys["content"] = SYSTEM_PROMPT;

  for (size_t i = 0; i < convHistory.size(); i++) {
    JsonObject u = msgs.createNestedObject();
    u["role"] = "user";
    u["content"] = convHistory[i].user;

    JsonObject a = msgs.createNestedObject();
    a["role"] = "assistant";
    a["content"] = convHistory[i].assistant;
  }

  JsonObject cur = msgs.createNestedObject();
  cur["role"] = "user";
  cur["content"] = userText;

  d["max_tokens"] = maxOutputTokens;
  d["temperature"] = (currentAI == AI_MODE_CREATIVE) ? 0.85 : 0.65;

  String out;
  serializeJson(d, out);
  return out;
}

// ===================== RESPONSE PARSERS =====================
String unescapeJsonString(const String &s) {
  String out;
  out.reserve(s.length());
  bool esc = false;
  for (int i = 0; i < (int)s.length(); i++) {
    char c = s[i];
    if (esc) {
      if (c == 'n') out += '\n';
      else if (c == 'r') out += '\r';
      else if (c == 't') out += '\t';
      else if (c == '"') out += '"';
      else if (c == '\\') out += '\\';
      else if (c == '/') out += '/';
      else out += c;
      esc = false;
    } else if (c == '\\') {
      esc = true;
    } else {
      out += c;
    }
  }
  return out;
}

String extractJsonStringValue(const String &body, const char *key) {
  String pattern = String("\"") + key + "\"";
  int p = body.indexOf(pattern);
  while (p >= 0) {
    int colon = body.indexOf(':', p + pattern.length());
    if (colon < 0) return "";
    int i = colon + 1;
    while (i < (int)body.length() && (body[i] == ' ' || body[i] == '\r' || body[i] == '\n' || body[i] == '\t')) i++;
    if (i >= (int)body.length()) return "";
    if (body.startsWith("null", i)) {
      p = body.indexOf(pattern, i + 4);
      continue;
    }
    if (body[i] != '"') {
      p = body.indexOf(pattern, i + 1);
      continue;
    }
    i++;
    String raw;
    raw.reserve(128);
    bool esc = false;
    for (; i < (int)body.length(); i++) {
      char c = body[i];
      if (esc) {
        raw += '\\';
        raw += c;
        esc = false;
      } else if (c == '\\') {
        esc = true;
      } else if (c == '"') {
        return unescapeJsonString(raw);
      } else {
        raw += c;
      }
    }
    return "";
  }
  return "";
}

String parseOpenRouter(const String &resp) {
  if (resp.isEmpty()) return "";

  DynamicJsonDocument d = makeDocPsram(min((size_t)65536, (size_t)resp.length() + 8192));
  DeserializationError e = deserializeJson(d, resp);
  if (!e) {
    JsonVariant v = d["choices"][0]["message"]["content"];
    if (!v.isNull()) return v.as<String>();
  } else {
    USBSerial.printf("[AI] OpenRouter JSON parse failed: %s\n", e.c_str());
  }

  // Fallback: extract the content field without parsing the whole JSON.
  // This is safer on ESP32 when providers return deep metadata.
  String content = extractJsonStringValue(resp, "content");
  content.trim();
  return content;
}

String parseSTTText(const String &resp) {
  // AssemblyAI responses include deep `words` arrays, so full JSON parsing can fail
  // with TooDeep. Extract text first, then try JSON as a fallback.
  String t = extractJsonStringValue(resp, "text");
  t.trim();
  if (t.length()) return t;

  DynamicJsonDocument d = makeDocPsram(min((size_t)16384, (size_t)resp.length() + 2048));
  DeserializationError e = deserializeJson(d, resp);
  if (!e && d.containsKey("text")) return d["text"].as<String>();
  if (e) USBSerial.printf("[STT] generic STT JSON parse failed: %s\n", e.c_str());
  return "";
}

// ===================== WAV BUILDER =====================
void buildWavFromPCM(const int16_t *pcm, size_t samples, std::vector<uint8_t> &outWav,
                     uint32_t sampleRate = 16000) {
  const uint32_t H = 44;
  uint32_t pcmBytes = samples * sizeof(int16_t);

  outWav.clear();
  outWav.resize(H + pcmBytes);

  uint8_t *w = outWav.data();
  memcpy(w,      "RIFF", 4);
  uint32_t chunk = 36 + pcmBytes;
  memcpy(w + 4,  &chunk,    4);
  memcpy(w + 8,  "WAVE",    4);
  memcpy(w + 12, "fmt ",    4);
  uint32_t sub1 = 16;
  memcpy(w + 16, &sub1,     4);
  uint16_t fmt = 1;
  memcpy(w + 20, &fmt,      2);
  uint16_t ch = 1;
  memcpy(w + 22, &ch,       2);
  memcpy(w + 24, &sampleRate, 4);
  uint32_t byteRate = sampleRate * 2;
  memcpy(w + 28, &byteRate, 4);
  uint16_t blockAlign = 2;
  memcpy(w + 32, &blockAlign, 2);
  uint16_t bps = 16;
  memcpy(w + 34, &bps,      2);
  memcpy(w + 36, "data",    4);
  memcpy(w + 40, &pcmBytes, 4);
  memcpy(w + 44, pcm, pcmBytes);
}

// Same WAV builder, but the WAV bytes live in PSRAM instead of normal heap.
// This is important because WiFi/TLS needs internal heap during HTTPS upload.
bool buildWavFromPCMToPsram(const int16_t *pcm, size_t samples, uint8_t **outWav,
                            size_t *outLen, uint32_t sampleRate = 16000) {
  if (!pcm || !outWav || !outLen) return false;

  const uint32_t H = 44;
  uint32_t pcmBytes = samples * sizeof(int16_t);
  size_t totalBytes = H + pcmBytes;

  uint8_t *w = (uint8_t *)heap_caps_malloc(totalBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!w) {
    audioLogf("[STT] PSRAM WAV alloc failed. bytes=%u", (unsigned)totalBytes);
    return false;
  }

  memcpy(w,      "RIFF", 4);
  uint32_t chunk = 36 + pcmBytes;
  memcpy(w + 4,  &chunk,    4);
  memcpy(w + 8,  "WAVE",    4);
  memcpy(w + 12, "fmt ",    4);
  uint32_t sub1 = 16;
  memcpy(w + 16, &sub1,     4);
  uint16_t fmt = 1;
  memcpy(w + 20, &fmt,      2);
  uint16_t ch = 1;
  memcpy(w + 22, &ch,       2);
  memcpy(w + 24, &sampleRate, 4);
  uint32_t byteRate = sampleRate * 2;
  memcpy(w + 28, &byteRate, 4);
  uint16_t blockAlign = 2;
  memcpy(w + 32, &blockAlign, 2);
  uint16_t bps = 16;
  memcpy(w + 34, &bps,      2);
  memcpy(w + 36, "data",    4);
  memcpy(w + 40, &pcmBytes, 4);
  memcpy(w + 44, pcm, pcmBytes);

  *outWav = w;
  *outLen = totalBytes;
  return true;
}

// ===================== CODEC I2C HELPERS =====================
static uint8_t codec_read(uint8_t addr, uint8_t reg) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom((uint8_t)addr, (uint8_t)1);
  return Wire.available() ? Wire.read() : 0xFF;
}

static void codec_write(uint8_t addr, uint8_t reg, uint8_t val) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

static void codec_update(uint8_t addr, uint8_t reg, uint8_t mask, uint8_t val) {
  uint8_t cur = codec_read(addr, reg);
  codec_write(addr, reg, (cur & ~mask) | (val & mask));
}

void scanAudioI2C() {
  audioLog("[I2C] Scanning...");
  int found = 0;
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      audioLogf("[I2C] Found device at 0x%02X", a);
      found++;
    }
  }
  audioLogf("[I2C] Scan done. Found=%d", found);
}

void dumpCodecRegs() {
  audioLogf("[CHK] ES7210 reg 0x00 = 0x%02X", codec_read(ES7210_ADDR, 0x00));
  audioLogf("[CHK] ES7210 reg 0x01 = 0x%02X", codec_read(ES7210_ADDR, 0x01));
  audioLogf("[CHK] ES7210 reg 0x11 = 0x%02X", codec_read(ES7210_ADDR, 0x11));
  audioLogf("[CHK] ES7210 reg 0x43 = 0x%02X", codec_read(ES7210_ADDR, 0x43));
  audioLogf("[CHK] ES7210 reg 0x44 = 0x%02X", codec_read(ES7210_ADDR, 0x44));
  audioLogf("[CHK] ES8311 reg 0x09 = 0x%02X", codec_read(ES8311_ADDR, 0x09));
  audioLogf("[CHK] ES8311 reg 0x0A = 0x%02X", codec_read(ES8311_ADDR, 0x0A));
}

// ===================== ES8311 INIT =====================
static void initES8311() {
  audioLog("[ES8311] Init...");
  codec_write(ES8311_ADDR, 0x32, 0x00);
  codec_write(ES8311_ADDR, 0x17, 0x00);
  codec_write(ES8311_ADDR, 0x0E, 0xFF);
  codec_write(ES8311_ADDR, 0x12, 0x02);
  codec_write(ES8311_ADDR, 0x14, 0x00);
  codec_write(ES8311_ADDR, 0x0D, 0xFA);
  codec_write(ES8311_ADDR, 0x15, 0x00);
  codec_write(ES8311_ADDR, 0x02, 0x10);
  codec_write(ES8311_ADDR, 0x00, 0x00);
  codec_write(ES8311_ADDR, 0x00, 0x1F);
  codec_write(ES8311_ADDR, 0x01, 0x30);
  codec_write(ES8311_ADDR, 0x01, 0x00);
  codec_write(ES8311_ADDR, 0x45, 0x00);
  codec_write(ES8311_ADDR, 0x0D, 0xFC);
  codec_write(ES8311_ADDR, 0x02, 0x00);
  delay(20);
  codec_write(ES8311_ADDR, 0x00, 0x80);
  codec_write(ES8311_ADDR, 0x01, 0x3F);
  codec_update(ES8311_ADDR, 0x09, 0x40, 0x00);
  codec_update(ES8311_ADDR, 0x0A, 0x40, 0x40);

  codec_update(ES8311_ADDR, 0x09, 0x1C, 0x0C);
  codec_update(ES8311_ADDR, 0x0A, 0x1C, 0x0C);

  codec_write(ES8311_ADDR, 0x17, 0xBF);
  codec_write(ES8311_ADDR, 0x0E, 0x02);
  codec_write(ES8311_ADDR, 0x12, 0x00);
  codec_write(ES8311_ADDR, 0x14, 0x1A);
  codec_update(ES8311_ADDR, 0x14, 0x40, 0x00);
  codec_write(ES8311_ADDR, 0x0D, 0x01);
  codec_write(ES8311_ADDR, 0x15, 0x40);
  codec_write(ES8311_ADDR, 0x37, 0x08);
  codec_write(ES8311_ADDR, 0x45, 0x00);
  codec_write(ES8311_ADDR, 0x32, 0x00);
  audioLog("[ES8311] Done");
}

// ===================== ES7210 INIT =====================
static void initES7210() {
  audioLog("[ES7210] Init v2...");

  // Full reset
  codec_write(ES7210_ADDR, 0x00, 0xFF);
  delay(20);
  codec_write(ES7210_ADDR, 0x00, 0x32);  // release reset, keep ADC off
  delay(10);

  // Clock: set MCLK divider for 256*Fs
  codec_write(ES7210_ADDR, 0x01, 0x20);  // MCLK=256Fs, BCLK/LRCK from master
  codec_write(ES7210_ADDR, 0x02, 0xC1);  // 16-bit I2S, stereo

  // Enable MCLK output to ADC core
  codec_write(ES7210_ADDR, 0x06, 0x00);  // clk enable
  codec_write(ES7210_ADDR, 0x07, 0x20);  // OSR=32

  // I2S format: standard I2S, 16-bit
  codec_update(ES7210_ADDR, 0x11, 0xE0, 0x60);  // I2S mode
  codec_update(ES7210_ADDR, 0x11, 0x03, 0x00);  // 16-bit word length

  // TDM: use normal I2S (not TDM), output on SDOUT1
  codec_write(ES7210_ADDR, 0x09, 0x30);
  codec_write(ES7210_ADDR, 0x0A, 0x30);

  // Analog: power on MIC1 & MIC2 bias + PGA
  codec_write(ES7210_ADDR, 0x40, 0x43);  // ref voltage on
  codec_write(ES7210_ADDR, 0x41, 0x70);  // MIC1 bias on
  codec_write(ES7210_ADDR, 0x42, 0x70);  // MIC2 bias on
  delay(50);                              // let bias stabilize

  // PGA gain: +30dB on MIC1 and MIC2
  codec_write(ES7210_ADDR, 0x43, 0x1E);  // MIC1: PGA=+30dB, boost on
  codec_write(ES7210_ADDR, 0x44, 0x1E);  // MIC2: PGA=+30dB, boost on
  codec_write(ES7210_ADDR, 0x45, 0x00);  // MIC3: off
  codec_write(ES7210_ADDR, 0x46, 0x00);  // MIC4: off

  // Digital volume: 0dB (no attenuation)
  codec_write(ES7210_ADDR, 0x47, 0x00);
  codec_write(ES7210_ADDR, 0x48, 0x00);
  codec_write(ES7210_ADDR, 0x49, 0x00);
  codec_write(ES7210_ADDR, 0x4A, 0x00);

  // HPF on (remove DC offset)
  codec_write(ES7210_ADDR, 0x4B, 0x00);
  codec_write(ES7210_ADDR, 0x4C, 0x00);

  // ADC power: enable CH1 + CH2, power down CH3 + CH4
  codec_write(ES7210_ADDR, 0x01, 0x14);  // CH3+CH4 powered down, CH1+CH2 active
  delay(50);

  // Start ADC — release from standby
  codec_write(ES7210_ADDR, 0x00, 0x71);  // normal operation
  delay(30);
  codec_write(ES7210_ADDR, 0x00, 0x41);  // clear init flag, keep running
  delay(50);

  audioLog("[ES7210] Done v2");
  audioLogf("[ES7210] reg 0x00 = 0x%02X (expect 0x41)", codec_read(ES7210_ADDR, 0x00));
  audioLogf("[ES7210] reg 0x01 = 0x%02X (expect 0x14)", codec_read(ES7210_ADDR, 0x01));
  audioLogf("[ES7210] reg 0x43 = 0x%02X (expect 0x1E)", codec_read(ES7210_ADDR, 0x43));
  audioLogf("[ES7210] reg 0x44 = 0x%02X (expect 0x1E)", codec_read(ES7210_ADDR, 0x44));
}

// ===================== AUDIO INIT =====================
bool initAudioInput() {
  pinMode(PA_CTRL, OUTPUT);
  digitalWrite(PA_CTRL, HIGH);
  audioLog("[Audio] PA_CTRL HIGH");

  scanAudioI2C();

  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
  chan_cfg.auto_clear    = true;
  chan_cfg.dma_desc_num  = 8;
  chan_cfg.dma_frame_num = 512;

  if (i2s_new_channel(&chan_cfg, &tx_chan, &rx_chan) != ESP_OK) {
    audioLog("[I2S] Channel create failed");
    return false;
  }

  i2s_std_config_t std_cfg = {
    .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(RECORD_SAMPLE_RATE),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                    I2S_DATA_BIT_WIDTH_16BIT,
                    I2S_SLOT_MODE_STEREO),
    .gpio_cfg = {
      .mclk = (gpio_num_t)I2S_MCLK,
      .bclk = (gpio_num_t)I2S_SCLK,
      .ws   = (gpio_num_t)I2S_LRCK,
      .dout = (gpio_num_t)I2S_ASDOUT,
      .din  = (gpio_num_t)I2S_DSDIN,
      .invert_flags = { false, false, false }
    }
  };
  std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

  if (i2s_channel_init_std_mode(tx_chan, &std_cfg) != ESP_OK) {
    audioLog("[I2S] TX init failed");
    return false;
  }
  if (i2s_channel_init_std_mode(rx_chan, &std_cfg) != ESP_OK) {
    audioLog("[I2S] RX init failed");
    return false;
  }
  if (i2s_channel_enable(tx_chan) != ESP_OK) {
    audioLog("[I2S] TX enable failed");
    return false;
  }
  if (i2s_channel_enable(rx_chan) != ESP_OK) {
    audioLog("[I2S] RX enable failed");
    return false;
  }

  audioLog("[I2S] Full-duplex enabled");
  delay(100);

  initES8311();
  delay(50);
  initES7210();
  delay(50);

  dumpCodecRegs();

  audioReady = true;
  audioLog("[Audio] Ready");
  return true;
}

// ===================== MIC DEBUG TEST =====================
bool runMicDebugTest(uint32_t testMs = 3000) {
  if (!audioReady || !rx_chan) {
    audioLog("[MICDBG] Audio not ready");
    return false;
  }

  audioLog("[MICDBG] Starting live mic debug test...");
  audioLog("[MICDBG] Speak or tap near the mic.");

  uint8_t discard[4096];
  for (int i = 0; i < 8; i++) {
    size_t br = 0;
    i2s_channel_read(rx_chan, discard, sizeof(discard), &br, pdMS_TO_TICKS(200));
  }

  unsigned long start = millis();
  int hitCount = 0;

  while (millis() - start < testMs) {
    int16_t buf[1024];
    size_t br = 0;
    esp_err_t err = i2s_channel_read(rx_chan, buf, sizeof(buf), &br, pdMS_TO_TICKS(250));

    if (err != ESP_OK) {
      audioLogf("[MICDBG] i2s read error=%d", err);
      continue;
    }

    if (br < 4) continue;

    size_t frames = br / 4;
    updateMicStatsFromStereo(buf, frames);

    if (millis() - g_lastMicDebugPrint > 250) {
      g_lastMicDebugPrint = millis();
      audioLogf("[MICDBG] bytes=%u frames=%u peakL=%d peakR=%d avgL=%d avgR=%d",
                (unsigned)br, (unsigned)frames,
                (int)g_lastPeakL, (int)g_lastPeakR,
                g_lastAvgL, g_lastAvgR);
    }

    if (g_lastAvgL > 120 || g_lastAvgR > 120 || abs(g_lastPeakL) > 1000 || abs(g_lastPeakR) > 1000) {
      hitCount++;
    }

    delay(10);
  }

  audioLogf("[MICDBG] Done. hitCount=%d final %s", hitCount, micStatsShort().c_str());
  return hitCount > 2;
}

// ===================== AUDIO RECORD =====================
bool recordAudioExperimental(int16_t *pcm, size_t samplesNeeded) {
  if (!audioReady || !rx_chan || !pcm) return false;

  size_t stereoBytesNeeded = samplesNeeded * sizeof(int16_t) * 2;
  int16_t *stereoBuf = (int16_t *)heap_caps_malloc(
      stereoBytesNeeded, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!stereoBuf) {
    audioLog("[Audio] Stereo buffer alloc failed");
    return false;
  }

  uint8_t discard[4096];
  for (int i = 0; i < 8; i++) {
    size_t br = 0;
    i2s_channel_read(rx_chan, discard, sizeof(discard), &br, pdMS_TO_TICKS(200));
  }

  size_t totalRead = 0;
  unsigned long start = millis();
  unsigned long lastDbg = 0;

  while (totalRead < stereoBytesNeeded && millis() - start < 12000) {
    if (shouldCancelWork()) {
      heap_caps_free(stereoBuf);
      audioLog("[REC] canceled");
      return false;
    }
    size_t br = 0;
    esp_err_t err = i2s_channel_read(
        rx_chan,
        (uint8_t *)stereoBuf + totalRead,
        min((size_t)4096, stereoBytesNeeded - totalRead),
        &br, pdMS_TO_TICKS(200));

    if (err == ESP_OK && br > 0) {
      br = (br / 4) * 4;
      size_t justFrames = br / 4;

      updateMicStatsFromStereo((int16_t *)((uint8_t *)stereoBuf + totalRead), justFrames);
      totalRead += br;

      if (micDebugEnabled && millis() - lastDbg > 300) {
        lastDbg = millis();
        audioLogf("[REC] %u/%u bytes | peakL=%d peakR=%d avgL=%d avgR=%d",
                  (unsigned)totalRead, (unsigned)stereoBytesNeeded,
                  (int)g_lastPeakL, (int)g_lastPeakR,
                  g_lastAvgL, g_lastAvgR);
      }
    }
  }

  size_t stereoFrames = totalRead / 4;
  for (size_t i = 0; i < stereoFrames && i < samplesNeeded; i++) {
    int32_t mixed = ((int32_t)stereoBuf[i * 2] + (int32_t)stereoBuf[i * 2 + 1]) / 2;
    mixed *= 4;
    if (mixed >  32767) mixed =  32767;
    if (mixed < -32768) mixed = -32768;
    pcm[i] = (int16_t)mixed;
  }

  heap_caps_free(stereoBuf);

  audioLogf("[REC] Done. stereoFrames=%u final %s",
            (unsigned)stereoFrames, micStatsShort().c_str());

  return stereoFrames >= samplesNeeded / 2;
}

// ===================== STT =====================
static void dumpWavDebugRaw(const uint8_t *wav, size_t wavLen) {
  if (!wav || wavLen < 44) {
    USBSerial.printf("[STT] WAV debug: too small (%u)\n", (unsigned)wavLen);
    return;
  }
  USBSerial.printf("[STT] WAV debug: size=%u bytes\n", (unsigned)wavLen);

  USBSerial.print("[STT] WAV header magic: ");
  USBSerial.write((const uint8_t*)wav, 4);
  USBSerial.println();

  if (wavLen >= 44 + 32) {
    USBSerial.println("[STT] WAV first 8 int16 samples (mono PCM):");
    for (int i = 0; i < 8; i++) {
      int idx = 44 + i * 2;
      int16_t sample = (int16_t)(wav[idx] | (wav[idx + 1] << 8));
      USBSerial.printf("  s[%d]=%d\n", i, sample);
    }
  }
}

static void dumpPcmDebug(const int16_t *pcm, size_t samples) {
  if (!pcm || samples < 8) {
    USBSerial.println("[STT] PCM debug: too small");
    return;
  }
  USBSerial.println("[STT] PCM first 8 int16 samples:");
  for (int i = 0; i < 8; i++) {
    USBSerial.printf("  s[%d]=%d\n", i, pcm[i]);
  }
}

String transcribeWithAssemblyAI(const uint8_t *wav, size_t wavLen, String &err) {
  if (ASSEMBLY_API_KEY.isEmpty()) { err = "Assembly key missing"; return ""; }
  if (shouldCancelWork()) { err = "Canceled"; return ""; }

  USBSerial.printf("[STT] AssemblyAI: WiFi=%d IP=%s WAVbytes=%u\n",
                (int)WiFi.status(), WiFi.localIP().toString().c_str(), (unsigned)wavLen);
  logMem("Assembly before upload");
  setWorkStage(WORK_STT_ASSEMBLY_UPLOAD, "Uploading voice to AssemblyAI...");
  activeWorkLog("Assembly upload request");

  if (WiFi.status() != WL_CONNECTED) { err = "WiFi dropped"; return ""; }
  delay(200);
  dumpWavDebugRaw(wav, wavLen);

  HttpResult up = postBinaryAssembly(
    "https://api.assemblyai.com/v2/upload",
    wav, wavLen, ASSEMBLY_API_KEY, "application/octet-stream"
  );

  USBSerial.printf("[STT] Assembly upload code=%d bodyLen=%u\n", up.code, (unsigned)up.body.length());
  if (up.body.length() > 0) {
    USBSerial.println("[STT] Assembly upload body snippet:");
    USBSerial.println(up.body.substring(0, 700));
    doneWorkLog("Assembly upload body snippet: " + up.body.substring(0, 72));
  }

  if (up.code != 200) { err = "Upload " + String(up.code) + ": " + up.body.substring(0, 120); errorWorkLog("Assembly upload failed: " + err); return ""; }

  setSmoothProgress(900, 970, false);
  DynamicJsonDocument upDoc(4096);
  if (deserializeJson(upDoc, up.body)) { err = "Upload parse fail"; errorWorkLog("Assembly upload parse fail"); return ""; }

  String uploadUrl = upDoc["upload_url"].as<String>();
  if (uploadUrl.isEmpty()) { err = "No upload URL"; errorWorkLog("Assembly no upload URL"); return ""; }
  doneWorkLog("Assembly upload URL ready");
  setSmoothProgress(920, 980, false);

  DynamicJsonDocument req(1024);
  req["audio_url"]   = uploadUrl;
  req["punctuate"]   = true;
  req["format_text"] = true;

  String body;
  serializeJson(req, body);

  USBSerial.printf("[STT] Assembly create transcript (audio_url len=%u)\n", (unsigned)uploadUrl.length());
  setSegmentProgress(SEG_PROGRESS_STT, 5);
  setSmoothProgress(720, 970, true);
  setWorkStage(WORK_STT_ASSEMBLY_WAIT, "Creating AssemblyAI transcript...");
  activeWorkLog("Assembly create transcript");

  HttpResult tr = postJsonAssembly("https://api.assemblyai.com/v2/transcript", body, ASSEMBLY_API_KEY);
  USBSerial.printf("[STT] Assembly create code=%d bodyLen=%u\n", tr.code, (unsigned)tr.body.length());
  if (tr.body.length() > 0) {
    USBSerial.println("[STT] Assembly create body snippet:");
    USBSerial.println(tr.body.substring(0, 700));
    doneWorkLog("Assembly create body snippet: " + tr.body.substring(0, 72));
  }

  if (tr.code != 200) { err = "Create " + String(tr.code) + ": " + tr.body.substring(0, 120); errorWorkLog("Assembly create failed: " + err); return ""; }

  DynamicJsonDocument trDoc(4096);
  if (deserializeJson(trDoc, tr.body)) { err = "Create parse fail"; errorWorkLog("Assembly create parse fail"); return ""; }

  String id = trDoc["id"].as<String>();
  if (id.isEmpty()) { err = "No transcript ID"; errorWorkLog("Assembly no transcript ID"); return ""; }
  doneWorkLog("Assembly transcript ID ready");

  for (int i = 0; i < 25; i++) {
    if (shouldCancelWork()) { err = "Canceled"; return ""; }
    setSegmentProgress(SEG_PROGRESS_STT, 5);
    setWorkStage(WORK_STT_ASSEMBLY_WAIT, "Waiting for AssemblyAI " + String(i + 1) + "/25...");
    setSmoothProgress(820 + min(i * 6, 140), 970, true);
    activeWorkLog("Assembly polling transcript " + String(i + 1) + "/25");
    delay(800);
    String url = "https://api.assemblyai.com/v2/transcript/" + id;
    HttpResult g = getAssembly(url.c_str(), ASSEMBLY_API_KEY);

    USBSerial.printf("[STT] Assembly poll %d code=%d bodyLen=%u\n", i, g.code, (unsigned)g.body.length());
    if (g.body.length() > 0 && i < 2) {
      USBSerial.println("[STT] Assembly poll body snippet:");
      USBSerial.println(g.body.substring(0, 600));
      noteWorkLog("Assembly poll body snippet: " + g.body.substring(0, 72));
    }

    if (g.code != 200) { err = "Poll " + String(g.code) + ": " + g.body.substring(0, 120); errorWorkLog("Assembly poll failed: " + err); return ""; }

    DynamicJsonDocument pd(4096);
    if (deserializeJson(pd, g.body)) { err = "Poll parse fail"; errorWorkLog("Assembly poll parse fail"); return ""; }

    String status = pd["status"].as<String>();
    if (status == "completed") { setSegmentProgress(SEG_PROGRESS_STT, 6); setSmoothProgress(980, 990, false); doneWorkLog("Assembly transcript ready"); return pd["text"].as<String>(); }
    if (status == "error")     { err = pd["error"].as<String>(); errorWorkLog("Assembly transcript error: " + err); return ""; }
  }

  err = "Assembly timeout";
  errorWorkLog("Assembly timeout");
  return "";
}

String readHttpBodyFromSecureClient(WiFiClientSecure &client, int &httpCode, String &err) {
  httpCode = -1;
  err = "";

  unsigned long startWait = millis();
  while (client.connected() && !client.available() && millis() - startWait < 15000) {
    if (shouldCancelWork()) { err = "Canceled"; return ""; }
    delay(10);
    yield();
  }

  if (!client.available()) {
    err = "No HTTP response";
    return "";
  }

  String statusLine = client.readStringUntil('\n');
  statusLine.trim();
  USBSerial.println("[STT] HTTP status line: " + statusLine);

  if (statusLine.startsWith("HTTP/")) {
    int firstSpace = statusLine.indexOf(' ');
    if (firstSpace > 0 && statusLine.length() >= firstSpace + 4) {
      httpCode = statusLine.substring(firstSpace + 1, firstSpace + 4).toInt();
    }
  }

  int contentLength = -1;
  bool chunked = false;

  while (client.connected()) {
    String h = client.readStringUntil('\n');
    h.trim();
    if (h.length() == 0) break;

    String lower = h;
    lower.toLowerCase();
    if (lower.startsWith("content-length:")) {
      contentLength = h.substring(15).toInt();
    }
    if (lower.startsWith("transfer-encoding:") && lower.indexOf("chunked") >= 0) {
      chunked = true;
    }
  }

  String body;
  unsigned long lastData = millis();

  if (contentLength >= 0) {
    body.reserve(min(contentLength + 8, 4096));
    while ((int)body.length() < contentLength && millis() - lastData < 20000) {
      if (shouldCancelWork()) { err = "Canceled"; return ""; }
      while (client.available()) {
        char c = (char)client.read();
        body += c;
        lastData = millis();
        if ((int)body.length() >= contentLength) break;
      }
      delay(1);
      yield();
    }
  } else if (chunked) {
    // Simple chunked-response reader. Deepgram usually returns a small JSON body.
    while (client.connected() && millis() - lastData < 20000) {
      String sizeLine = client.readStringUntil('\n');
      sizeLine.trim();
      if (sizeLine.length() == 0) {
        delay(1);
        yield();
        continue;
      }
      int chunkSize = (int)strtol(sizeLine.c_str(), nullptr, 16);
      if (chunkSize <= 0) break;
      for (int i = 0; i < chunkSize && millis() - lastData < 20000; i++) {
        while (!client.available() && millis() - lastData < 20000) {
          delay(1);
          yield();
        }
        if (client.available()) {
          body += (char)client.read();
          lastData = millis();
        }
      }
      // consume CRLF after chunk
      if (client.available()) client.read();
      if (client.available()) client.read();
    }
  } else {
    while ((client.connected() || client.available()) && millis() - lastData < 20000) {
      if (shouldCancelWork()) { err = "Canceled"; return ""; }
      while (client.available()) {
        body += (char)client.read();
        lastData = millis();
      }
      delay(1);
      yield();
    }
  }

  return body;
}

String transcribeWithDeepgramRawPCM(const int16_t *pcm, size_t samples, String &err) {
  if (DEEPGRAM_API_KEY.isEmpty()) { err = "Deepgram key missing"; return ""; }

  size_t pcmBytes = samples * sizeof(int16_t);
  USBSerial.printf("[STT] Deepgram MANUAL RAW PCM: WiFi=%d IP=%s samples=%u bytes=%u\n",
                (int)WiFi.status(), WiFi.localIP().toString().c_str(),
                (unsigned)samples, (unsigned)pcmBytes);
  logMem("Deepgram before manual raw PCM upload");
  setWorkStage(WORK_STT_DEEPGRAM_CONNECT, "Connecting to Deepgram...");
  setSmoothProgress(280, 380, true);
  activeWorkLog("Deepgram MANUAL RAW PCM bytes=" + String((unsigned)pcmBytes));

  if (shouldCancelWork()) { err = "Canceled"; return ""; }
  if (WiFi.status() != WL_CONNECTED) { err = "WiFi dropped"; errorWorkLog("Deepgram WiFi dropped"); return ""; }
  delay(100);
  dumpPcmDebug(pcm, samples);

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(45000);

  const char *host = "api.deepgram.com";
  const uint16_t port = 443;
  const char *path =
    "/v1/listen?model=nova-2-general"
    "&encoding=linear16"
    "&sample_rate=16000"
    "&channels=1"
    "&smart_format=true"
    "&punctuate=true";

  USBSerial.println("[STT] Deepgram manual TLS connect...");
  activeWorkLog("Deepgram manual TLS connect");
  logMem("Deepgram before TLS connect");

  if (shouldCancelWork()) { err = "Canceled"; client.stop(); return ""; }
  if (!client.connect(host, port)) {
    err = "Deepgram TLS connect failed";
    errorWorkLog("Deepgram TLS connect failed");
    client.stop();
    logMem("Deepgram TLS connect failed");
    return "";
  }

  doneWorkLog("Deepgram TLS connected");
  setSmoothProgress(330, 430, false);
  logMem("Deepgram after TLS connect");

  client.print(String("POST ") + path + " HTTP/1.1\r\n");
  client.print(String("Host: ") + host + "\r\n");
  client.print("Authorization: Token ");
  client.print(DEEPGRAM_API_KEY);
  client.print("\r\n");
  client.print("Content-Type: audio/raw\r\n");
  client.print("Accept: application/json\r\n");
  client.print("Connection: close\r\n");
  client.print("Content-Length: ");
  client.print((unsigned)pcmBytes);
  client.print("\r\n\r\n");

  USBSerial.println("[STT] Deepgram manual chunked payload send starting...");
  setWorkStage(WORK_STT_DEEPGRAM_UPLOAD, "Uploading voice to Deepgram...");
  setSmoothProgress(340, 620, true);
  activeWorkLog("Deepgram sending audio 0/" + String((unsigned)pcmBytes));
  logMem("Deepgram right before manual payload");

  const uint8_t *payload = (const uint8_t *)pcm;
  size_t sent = 0;
  const size_t CHUNK = 1024; // small writes are more stable with WiFiClientSecure on ESP32
  unsigned long lastProgress = millis();

  while (sent < pcmBytes) {
    if (shouldCancelWork()) { err = "Canceled"; client.stop(); return ""; }
    size_t n = min(CHUNK, pcmBytes - sent);
    size_t totalThisChunk = 0;
    unsigned long chunkStart = millis();

    while (totalThisChunk < n && millis() - chunkStart < 8000) {
      size_t w = client.write(payload + sent + totalThisChunk, n - totalThisChunk);
      if (w > 0) {
        totalThisChunk += w;
        lastProgress = millis();
      } else {
        delay(5);
        yield();
      }
    }

    if (totalThisChunk == 0) {
      err = "Deepgram payload write failed at byte " + String((unsigned)sent);
      USBSerial.println("[STT] " + err);
      errorWorkLog(err);
      client.stop();
      logMem("Deepgram manual payload failed");
      return "";
    }

    sent += totalThisChunk;
    setSmoothProgressByBytes(350, 300, sent, pcmBytes, 650);

    if (sent % 16384 == 0 || sent >= pcmBytes) {
      USBSerial.printf("[STT] Deepgram sent %u/%u bytes\n", (unsigned)sent, (unsigned)pcmBytes);
      int pct = pcmBytes ? (int)((sent * 100) / pcmBytes) : 100;
      setWorkStage(WORK_STT_DEEPGRAM_UPLOAD, "Uploading voice to Deepgram " + String(pct) + "%...");
      activeWorkLog("Deepgram sending audio " + String((unsigned)sent) + "/" + String((unsigned)pcmBytes));
    }

    if (millis() - lastProgress > 15000) {
      err = "Deepgram payload send timeout";
      errorWorkLog("Deepgram payload send timeout");
      client.stop();
      logMem("Deepgram manual payload timeout");
      return "";
    }

    delay(1);
    yield();
  }

  USBSerial.println("[STT] Deepgram payload sent. Waiting response...");
  doneWorkLog("Deepgram payload sent");
  setWorkStage(WORK_STT_DEEPGRAM_WAIT, "Waiting for Deepgram transcript...");
  setSmoothProgress(700, 970, true);
  activeWorkLog("Deepgram waiting response");
  logMem("Deepgram after manual payload");

  int code = -1;
  String body = readHttpBodyFromSecureClient(client, code, err);
  client.stop();

  setSegmentProgress(SEG_PROGRESS_STT, 6);
  setSmoothProgress(980, 990, false);
  logMem("Deepgram after manual response");
  USBSerial.printf("[STT] Deepgram manual HTTP code=%d bodyLen=%u\n", code, (unsigned)body.length());
  if (body.length() > 0) {
    USBSerial.println("[STT] Deepgram body snippet:");
    USBSerial.println(body.substring(0, 1200));
    doneWorkLog("Deepgram body snippet: " + body.substring(0, 72));
  }

  if (code != 200) {
    if (err.isEmpty()) err = "Deepgram HTTP " + String(code) + ": " + body.substring(0, 160);
    errorWorkLog(err);
    return "";
  }

  // Deepgram can return deep metadata/words/paragraphs; avoid full JSON parse first.
  String transcript = extractJsonStringValue(body, "transcript");
  transcript.trim();
  if (transcript.length()) return transcript;

  DynamicJsonDocument d = makeDocPsram(body.length() + 4096);
  DeserializationError jerr = deserializeJson(d, body);
  if (!jerr) {
    JsonVariant v = d["results"]["channels"][0]["alternatives"][0]["transcript"];
    if (!v.isNull()) return v.as<String>();
  } else {
    USBSerial.printf("[STT] Deepgram JSON parse failed: %s\n", jerr.c_str());
  }

  err = "No transcript";
  return "";
}

// ===================== PORTAL HTML =====================
String maskedKey(const String &s) {
  if (s.length() <= 8) return s;
  return s.substring(0, 4) + "********" + s.substring(s.length() - 4);
}

void handlePortalRoot() {
  String html = R"rawliteral(
<!DOCTYPE html><html><head>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>AI Buddy Watch Setup</title>
<style>
body{font-family:Arial,sans-serif;background:#0d1020;color:#f5f7ff;padding:18px;line-height:1.35}
.card{background:#171b33;border-radius:18px;padding:16px;margin:12px 0;box-shadow:0 8px 28px rgba(0,0,0,.22)}
input,textarea{width:100%;box-sizing:border-box;padding:12px;margin:7px 0 12px;border-radius:12px;border:1px solid #394063;background:#0f1328;color:#fff;font-size:15px}
textarea{min-height:130px}
button{width:100%;padding:15px;border:0;border-radius:14px;background:#8a7cff;color:#fff;font-weight:800;font-size:16px}
small{color:#b8bedc}.hint{color:#cfd4ff;font-size:14px}.pill{display:inline-block;background:#23294d;border-radius:999px;padding:5px 10px;margin:4px 4px 4px 0}
</style></head><body>
<h2>AI Buddy Watch Setup</h2>
<p class='hint'>Use this portal for long text: WiFi, API keys, model names, and system prompt. Daily controls like brightness, theme, STT mode, and recording duration are on the watch Settings screen.</p>
<form action='/save' method='post'>
<div class='card'>
<h3>WiFi</h3>
<input name='wifi1ssid' placeholder='WiFi 1 SSID' value='%WIFI1SSID%'>
<input name='wifi1pass' placeholder='WiFi 1 Password' value='%WIFI1PASS%'>
<input name='wifi2ssid' placeholder='WiFi 2 SSID' value='%WIFI2SSID%'>
<input name='wifi2pass' placeholder='WiFi 2 Password' value='%WIFI2PASS%'>
<input name='wifi3ssid' placeholder='WiFi 3 SSID' value='%WIFI3SSID%'>
<input name='wifi3pass' placeholder='WiFi 3 Password' value='%WIFI3PASS%'>
</div>

<div class='card'>
<h3>OpenRouter AI</h3>
<input name='orapi' placeholder='OpenRouter API Key' value='%ORAPI%'>
<label>Smart Mode model</label>
<input name='ormodel1' placeholder='Smart → OpenRouter Model 1' value='%ORM1%'>
<label>Fast Mode model</label>
<input name='ormodel2' placeholder='Fast → OpenRouter Model 2' value='%ORM2%'>
<label>Creative Mode model</label>
<input name='ormodel3' placeholder='Creative → OpenRouter Model 3' value='%ORM3%'>
<small>These are the exact OpenRouter model IDs. On the watch, Settings → AI selects Smart / Fast / Creative and controls fallback.</small>
</div>

<div class='card'>
<h3>Voice APIs</h3>
<input name='deepgram' placeholder='Deepgram API Key' value='%DEEPGRAM%'>
<input name='assembly' placeholder='AssemblyAI API Key' value='%ASSEMBLY%'>
<small>STT provider, fallback and recording duration are changed on the watch.</small>
</div>

<div class='card'>
<h3>Assistant Personality</h3>
<input name='assistant' placeholder='Assistant name' value='%ASSISTANT%'>
<textarea name='persona'>%PERSONA%</textarea>
</div>

<button type='submit'>Save Configuration</button>
</form>
</body></html>
)rawliteral";

  html.replace("%WIFI1SSID%", htmlEscape(WIFI1SSID));
  html.replace("%WIFI1PASS%", htmlEscape(WIFI1PASS));
  html.replace("%WIFI2SSID%", htmlEscape(WIFI2SSID));
  html.replace("%WIFI2PASS%", htmlEscape(WIFI2PASS));
  html.replace("%WIFI3SSID%", htmlEscape(WIFI3SSID));
  html.replace("%WIFI3PASS%", htmlEscape(WIFI3PASS));
  html.replace("%ORAPI%",     htmlEscape(OPENROUTER_API_KEY));
  html.replace("%ORM1%",      htmlEscape(OR_MODEL1));
  html.replace("%ORM2%",      htmlEscape(OR_MODEL2));
  html.replace("%ORM3%",      htmlEscape(OR_MODEL3));
  html.replace("%ASSEMBLY%",  htmlEscape(ASSEMBLY_API_KEY));
  html.replace("%DEEPGRAM%",  htmlEscape(DEEPGRAM_API_KEY));
  html.replace("%ASSISTANT%", htmlEscape(ASSISTANT_NAME));
  html.replace("%PERSONA%",   htmlEscape(SYSTEM_PROMPT));

  server.send(200, "text/html", html);
}

void handlePortalSave() {
  WIFI1SSID = server.arg("wifi1ssid"); WIFI1SSID.trim();
  WIFI1PASS = server.arg("wifi1pass"); WIFI1PASS.trim();
  WIFI2SSID = server.arg("wifi2ssid"); WIFI2SSID.trim();
  WIFI2PASS = server.arg("wifi2pass"); WIFI2PASS.trim();
  WIFI3SSID = server.arg("wifi3ssid"); WIFI3SSID.trim();
  WIFI3PASS = server.arg("wifi3pass"); WIFI3PASS.trim();

  OPENROUTER_API_KEY = server.arg("orapi");    OPENROUTER_API_KEY.trim();
  OR_MODEL1          = server.arg("ormodel1"); OR_MODEL1.trim();
  OR_MODEL2          = server.arg("ormodel2"); OR_MODEL2.trim();
  OR_MODEL3          = server.arg("ormodel3"); OR_MODEL3.trim();

  ASSEMBLY_API_KEY = server.arg("assembly"); ASSEMBLY_API_KEY.trim();
  DEEPGRAM_API_KEY = server.arg("deepgram"); DEEPGRAM_API_KEY.trim();

  String assistant = server.arg("assistant"); assistant.trim();
  if (!assistant.isEmpty()) ASSISTANT_NAME = assistant;

  String persona = server.arg("persona"); persona.trim();
  if (!persona.isEmpty()) SYSTEM_PROMPT = persona;

  saveConfig();
  portalSaved = true;

  server.send(200, "text/html",
    "<html><body style='font-family:Arial;background:#0d1020;color:#fff;padding:20px'>"
    "<h2>Saved</h2><p>You can return to the watch now.</p></body></html>");
}

bool startConfigPortal() {
  disconnectWiFi();
  delay(200);

  WiFi.mode(WIFI_AP);
  bool ok = WiFi.softAP("AIWatch-Config", "12345678");
  if (!ok) return false;

  dnsServer.start(53, "*", WiFi.softAPIP());

  server.on("/", handlePortalRoot);
  server.on("/save", HTTP_POST, handlePortalSave);
  server.onNotFound([]() { handlePortalRoot(); });
  server.begin();

  portalRunning  = true;
  portalSaved    = false;
  portalStartMs  = millis();
  setStatus("Portal " + WiFi.softAPIP().toString());
  return true;
}

void stopConfigPortal() {
  dnsServer.stop();
  server.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  networkReady = false;
  portalRunning = false;
  setStatus("Portal closed");
}

// ===================== AI SEND =====================
String askOpenRouterModel(const String &userText, const String &model, String &err) {
  if (OPENROUTER_API_KEY.isEmpty()) { err = "OpenRouter key missing"; return ""; }
  if (model.isEmpty()) { err = "OpenRouter model missing"; return ""; }

  setWorkStage(WORK_AI_BUILD, "Building request for " + model.substring(0, min(26, (int)model.length())) + "...");
  activeWorkLog("OpenRouter build request");
  String body = openrouterBodyWithContext(userText, model);
  doneWorkLog("OpenRouter request built");
  setWorkStage(WORK_AI_PRIMARY, "Connecting to AI model...");

  // Use a manual yielding HTTPS POST instead of HTTPClient.POST here.
  // HTTPClient can block long enough on ESP32-S3 to starve IDLE0 and trigger
  // the task watchdog while the AIWorker is pinned to CPU0.
  HttpResult apiResult = postOpenRouterManual(body, OPENROUTER_API_KEY);

  USBSerial.printf("[AI] OpenRouter model=%s code=%d bodyLen=%u\n",
                   model.c_str(), apiResult.code, (unsigned)apiResult.body.length());
  if (apiResult.code != 200) {
    err = "OpenRouter " + String(apiResult.code) + ": " + apiResult.body.substring(0, 160);
    errorWorkLog(err);
    return "";
  }

  setSegmentProgress(SEG_PROGRESS_AI, 6);
  setWorkStage(WORK_AI_WAIT, "Parsing AI reply...");
  setSmoothProgress(980, 990, false);
  activeWorkLog("OpenRouter parse reply");
  String out = parseOpenRouter(apiResult.body);
  out.trim();
  if (out.isEmpty()) {
    err = "OpenRouter empty response";
    USBSerial.println("[AI] OpenRouter body snippet:");
    USBSerial.println(apiResult.body.substring(0, 800));
    errorWorkLog("OpenRouter empty response");
  } else {
    USBSerial.printf("[AI] Parsed answer len=%u\n", (unsigned)out.length());
    USBSerial.println("[AI] Parsed answer snippet:");
    USBSerial.println(out.substring(0, 220));
    doneWorkLog("OpenRouter parsed answer len=" + String((unsigned)out.length()));
  }
  return out;
}

String askAI(const String &userText, String &err) {
  clearWorkNotice();
  if (shouldCancelWork()) { err = "Canceled"; return ""; }
  setWorkStage(WORK_AI_WIFI, "Checking WiFi...");
  if (!ensureWiFiConnection()) {
    err = "No WiFi connection. Open Settings > WiFi or start the config portal.";
    lastError = err;
    setWorkStage(WORK_ERROR, "WiFi check failed");
    return "";
  }

  String preferred = modelForMode(currentAI);
  setWorkStage(WORK_AI_PRIMARY, "Asking " + currentAiName() + " model...");
  if (shouldCancelWork()) { err = "Canceled"; return ""; }
  String answer = askOpenRouterModel(userText, preferred, err);
  if (!answer.isEmpty() || !aiFallbackEnabled) {
    if (!answer.isEmpty()) {
      clearWorkNotice();
      setWorkStage(WORK_AI_DONE, "AI reply ready");
    } else {
      lastError = err.length() ? err : "Primary AI model failed";
      setWorkNotice("Primary failed: " + lastError.substring(0, 90));
      setWorkStage(WORK_ERROR, "AI failed");
    }
    return answer;
  }

  USBSerial.println("[AI] Primary failed, trying fallback models...");
  String primaryErr = err.length() ? err : "Primary model did not return a reply";
  setWorkNotice("Primary failed: " + primaryErr.substring(0, 90));
  setWorkStage(WORK_AI_FALLBACK, "Trying fallback model...");

  AIMode order[3] = { AI_MODE_SMART, AI_MODE_FAST, AI_MODE_CREATIVE };
  for (int i = 0; i < 3; i++) {
    String m = modelForMode(order[i]);
    if (m == preferred || m.isEmpty()) continue;
    String e2;
    setWorkStage(WORK_AI_FALLBACK, "Trying " + aiModeName(order[i]) + " fallback...");
    if (shouldCancelWork()) { err = "Canceled"; return ""; }
    answer = askOpenRouterModel(userText, m, e2);
    if (!answer.isEmpty()) {
      err = "";
      clearWorkNotice();
      setWorkStage(WORK_AI_DONE, "AI reply ready");
      return answer;
    }
    err = e2;
    setWorkNotice("Fallback failed: " + err.substring(0, 90));
  }

  lastError = err.length() ? err : "All AI models failed";
  setWorkStage(WORK_ERROR, "AI failed");
  return "";
}


// ===================== ASYNC TASKS =====================
void sendCurrentTextToAI(const String &text);
void drawCurrentScreen();

void aiWorkerTask(void *param) {
  String *userPtr = (String *)param;
  String userText = *userPtr;
  delete userPtr;

  String err;
  if (workLogTitle.isEmpty()) resetWorkLog("AI reply loading");
  String answer = askAI(userText, err);
  bool wasCanceled = shouldCancelWork();
  bool answerEmpty = answer.isEmpty();
  String memNote;

  if (xSemaphoreTake(ui_mutex, portMAX_DELAY) == pdTRUE) {
    if (wasCanceled) {
      lastError = "";
      liveStatus = "AI canceled";
      setWorkStage(WORK_IDLE, "Canceled");
      applyFinalChatTurnLocked(userText, "AI canceled.");
      memNote = "AI canceled";
    } else if (answerEmpty) {
      String shownErr = err.length() ? err : "No response from AI";
      lastError = shownErr;
      setWorkStage(WORK_ERROR, "AI failed");
      applyFinalChatTurnLocked(userText, "AI error: " + shownErr);
      memNote = "AI failed";
    } else {
      lastError = "";
      setWorkStage(WORK_AI_DONE, "Done");
      applyFinalChatTurnLocked(userText, answer);
      USBSerial.printf("[AI] UI answer saved. history=%u visibleLen=%u\n",
                       (unsigned)convHistory.size(), (unsigned)visibleAssistantText.length());
      memNote = "AI done";
    }

    appState = STATE_CHAT;
    gBusy = false;
    cancelRequested = false;
    captureRunMemory(memNote);
    xSemaphoreGive(ui_mutex);
  }

  wifiSaverDisconnectNow(wasCanceled ? "AI canceled - WiFi off" : (answerEmpty ? "AI failed - WiFi off" : "Reply ready - WiFi off"));
  requestRedraw();
  vTaskDelete(NULL);
}

void sttWorkerTask(void *param) {
  (void)param;
  appState = STATE_TRANSCRIBING;
  uiScreen = UI_TRANSCRIBING;
  if (workLogTitle != "Transcribing voice") resetWorkLog("Transcribing voice");
  // Avoid max(int, volatile int) compile failure on ESP32 core 3.x.
  int progressStart = smoothProgressVisual;
  if (progressStart < 80) progressStart = 80;
  resetSmoothProgress(SEG_PROGRESS_STT, progressStart);
  setWorkStage(WORK_STT_PREPARE, "Preparing captured voice...");
  gBusy = true;

  USBSerial.println("[STT] Worker started...");
  logMem("STT worker start");

  String err;
  String text;

  bool tryDeepgram = (currentSTT == STT_MODE_AUTO || currentSTT == STT_MODE_DEEPGRAM);
  bool tryAssembly = (currentSTT == STT_MODE_AUTO || currentSTT == STT_MODE_ASSEMBLY);

  // WiFi Saver may leave WiFi fully off while idle/recording. STT is the first
  // point that actually needs internet, so connect here automatically using the
  // existing WiFi 1/2/3 fallback order. This prevents the "WiFi dropped" error
  // after recording when the watch booted with WiFi off.
  setWorkStage(WORK_STT_PREPARE, "Connecting WiFi for transcript...");
  if (!ensureWiFiConnection()) {
    err = "No saved WiFi connected. Check Settings > WiFi or config portal.";
    lastError = err;
    setWorkStage(WORK_ERROR, "WiFi check failed");
    tryDeepgram = false;
    tryAssembly = false;
  }

  if (tryDeepgram && !shouldCancelWork()) {
    USBSerial.println("[STT] Trying Deepgram RAW PCM...");
    setWorkStage(WORK_STT_DEEPGRAM_CONNECT, "Connecting to Deepgram...");
    text = transcribeWithDeepgramRawPCM(audioBuffer, audioBufferSamples, err);
    if (text.isEmpty() && err.length()) {
      setWorkNotice("Deepgram failed: " + err.substring(0, 90));
    }
  }

  if (text.isEmpty() && !shouldCancelWork() && tryAssembly && (currentSTT == STT_MODE_ASSEMBLY || sttFallbackEnabled)) {
    USBSerial.println("[STT] Trying AssemblyAI fallback...");
    setWorkStage(WORK_STT_ASSEMBLY_BUILD, "Preparing AssemblyAI fallback...");
    uint8_t *wav = nullptr;
    size_t wavLen = 0;

    logMem("before PSRAM WAV build for Assembly");
    bool wavOK = buildWavFromPCMToPsram(audioBuffer, audioBufferSamples, &wav, &wavLen, RECORD_SAMPLE_RATE);
    logMem("after PSRAM WAV build for Assembly");

    if (!wavOK || !wav || wavLen == 0) {
      err = "WAV PSRAM build failed";
      setWorkNotice(err);
    } else {
      USBSerial.printf("[STT] Built PSRAM WAV for Assembly: bytes=%u\n", (unsigned)wavLen);
      dumpWavDebugRaw(wav, wavLen);
      setWorkStage(WORK_STT_ASSEMBLY_UPLOAD, "Uploading to AssemblyAI...");
      String assemblyErr;
      String assemblyText = transcribeWithAssemblyAI(wav, wavLen, assemblyErr);
      if (!assemblyText.isEmpty()) {
        text = assemblyText;
        err = "";
      } else {
        err = assemblyErr;
        if (err.length()) setWorkNotice("AssemblyAI failed: " + err.substring(0, 90));
      }
    }

    if (wav) {
      heap_caps_free(wav);
      wav = nullptr;
      logMem("after Assembly WAV free");
    }
  }

  bool launchAI = false;
  bool transcriptReadyForSend = false;
  String aiText;

  if (xSemaphoreTake(ui_mutex, portMAX_DELAY) == pdTRUE) {
    if (shouldCancelWork()) {
      lastError = "";
      liveStatus = "Voice canceled";
      setWorkStage(WORK_IDLE, "Canceled");
      applyTransientChatStatusLocked("Voice message", "Voice canceled.", false);
    } else if (text.isEmpty()) {
      lastError = "Voice failed: " + err;
      errorWorkLog(lastError);
      setWorkStage(WORK_ERROR, "Voice failed");
      applyTransientChatStatusLocked("Voice message", lastError, false);
    } else {
      text.trim();
      if (text.isEmpty()) {
        lastError = "No speech detected. Try speaking closer to the mic.";
        errorWorkLog(lastError);
        setWorkStage(WORK_ERROR, "No speech detected");
        applyTransientChatStatusLocked("Voice message", lastError, false);
      } else {
        clearWorkNotice();
        doneWorkLog("Transcript ready: " + text.substring(0, 60));
        lastTranscript = text;
        inputDraft = text;
        chatScroll = 0; chatHistoryOffset = 0; invalidateChatWrapCache();
        setWorkStage(WORK_STT_DONE, autoSendAfterSTT ? "Transcript ready. Sending to AI..." : "Transcript ready");
        uiScreen = UI_CHAT;

        // Always show the fresh transcript immediately. Without this, an older
        // AI answer can stay visible and make it look like STT stopped working.
        setLatestChatViewLocked(text, autoSendAfterSTT ? "" : "Press SEND to ask AI, or TYPE to edit.", autoSendAfterSTT);
        transcriptReadyForSend = !autoSendAfterSTT;

        if (autoSendAfterSTT) {
          launchAI = true;
          aiText = text;
          // Show user's transcript immediately in conversation while AI works.
          addConversation(text, "");
          inputDraft = "";
          lastTranscript = "";
          appState = STATE_STREAMING;
          resetWorkLog("AI reply loading");
          resetSmoothProgress(SEG_PROGRESS_AI, 40);
          setWorkStage(WORK_AI_WIFI, "Checking WiFi before AI...");
          uiScreen = UI_CHAT;
        }
      }
    }

    if (!launchAI) {
      appState = STATE_CHAT;
      isRecording = false;
      gBusy = false;
      cancelRequested = false;
      captureRunMemory("STT done");
    }
    xSemaphoreGive(ui_mutex);
  }

  requestRedraw();

  if (transcriptReadyForSend) {
    startTranscriptWifiHold();
  } else if (!launchAI) {
    wifiSaverDisconnectNow(shouldCancelWork() ? "Voice canceled - WiFi off" : "Voice done - WiFi off");
  }

  if (launchAI) {
    String *heapText = new String(aiText);
    xTaskCreatePinnedToCore(aiWorkerTask, "AIWorker", 16384, heapText, 1, NULL, 0);
  }

  logMem("STT worker end");
  vTaskDelete(NULL);
}

void recordThenSttWorkerTask(void *param) {
  (void)param;

  USBSerial.println("[MIC] One-tap recording started immediately...");
  cancelRequested = false;
  clearWorkNotice();
  resetWorkLog("Recording voice");
  setWorkStage(WORK_RECORDING, "Listening...");
  logMem("record worker start");

  bool ok = recordAudioExperimental(audioBuffer, audioBufferSamples);

  if (shouldCancelWork()) {
    if (xSemaphoreTake(ui_mutex, portMAX_DELAY) == pdTRUE) {
      lastError = "";
      liveStatus = "Recording canceled";
      setWorkStage(WORK_IDLE, "Canceled");
      applyTransientChatStatusLocked("Voice message", "Recording canceled.", false);
      appState = STATE_CHAT;
      isRecording = false;
      gBusy = false;
      cancelRequested = false;
      captureRunMemory("Recording canceled");
      xSemaphoreGive(ui_mutex);
    }
    requestRedraw();
    vTaskDelete(NULL);
    return;
  }

  if (!ok) {
    audioLogf("[MIC] Capture failed. %s", micStatsShort().c_str());

    if (xSemaphoreTake(ui_mutex, portMAX_DELAY) == pdTRUE) {
      lastError = "Audio capture failed. Check codec USBSerial log.";
      errorWorkLog(lastError);
      setWorkStage(WORK_ERROR, "Audio capture failed");
      uiScreen = UI_ERROR_SCREEN;
      appState = STATE_CHAT;
      isRecording = false;
      gBusy = false;
      xSemaphoreGive(ui_mutex);
    }

    requestRedraw();
    logMem("record worker capture failed");
    vTaskDelete(NULL);
    return;
  }

  audioLogf("[MIC] Capture OK. %s", micStatsShort().c_str());

  if (xSemaphoreTake(ui_mutex, portMAX_DELAY) == pdTRUE) {
    isRecording = false;
    appState = STATE_TRANSCRIBING;
    resetWorkLog("Transcribing voice");
    resetSmoothProgress(SEG_PROGRESS_STT, 80);
    doneWorkLog("Audio captured " + String(recordSeconds) + " sec");
    setWorkStage(WORK_AUDIO_READY, "Capture OK. Transcribing...");
    setLatestChatViewLocked("Voice message", "Understanding your voice...", false);
    uiScreen = UI_TRANSCRIBING;
    xSemaphoreGive(ui_mutex);
  }

  requestRedraw();
  logMem("record worker before STT task");
  xTaskCreatePinnedToCore(sttWorkerTask, "STTWorker", STT_TASK_STACK_BYTES, NULL, 1, NULL, 0);
  vTaskDelete(NULL);
}

// ===================== CUSTOM BUDDY UI =====================
void clearButtons() {
  uiButtons.clear();
}

bool hitRect(const UIButton &b, int x, int y) {
  // The FT3168 mapping on this rounded AMOLED can be a little optimistic near
  // the bottom edge. Use larger vertical tolerance for normal buttons, but keep
  // keyboard keys tighter so neighbouring letters do not overlap too much.
  int padX = b.id.startsWith("key:") ? 7 : 14;
  int padY = b.id.startsWith("key:") ? 9 : 18;
  return x >= b.x - padX && x <= b.x + b.w + padX &&
         y >= b.y - padY && y <= b.y + b.h + padY;
}


bool pointInChatScrollArea(int x, int y) {
  if (uiScreen != UI_CHAT) return false;
  if (!chatScrollAreaValid || chatMaxScrollCached <= 0) return false;
  return x >= chatScrollAreaX && x <= chatScrollAreaX + chatScrollAreaW &&
         y >= chatScrollAreaY && y <= chatScrollAreaY + chatScrollAreaH;
}

void beginChatTouchScroll(int x, int y) {
  chatTouchScrollActive = true;
  chatTouchScrollMoved = false;
  chatTouchStartY = y;
  chatTouchLastY = y;
  chatTouchAccumPx = 0;
}

void updateChatTouchScroll(int x, int y) {
  (void)x;
  if (!chatTouchScrollActive) return;

  int totalMove = y - chatTouchStartY;
  if (abs(totalMove) >= CHAT_DRAG_THRESHOLD_PX) {
    chatTouchScrollMoved = true;
  }

  int delta = chatTouchLastY - y; // finger up => positive => scroll down
  chatTouchLastY = y;

  if (!chatTouchScrollMoved) return;

  chatTouchAccumPx += delta;
  int oldScroll = chatScroll;

  while (chatTouchAccumPx >= CHAT_SCROLL_LINE_PX) {
    if (chatScroll < chatMaxScrollCached) chatScroll++;
    chatTouchAccumPx -= CHAT_SCROLL_LINE_PX;
  }
  while (chatTouchAccumPx <= -CHAT_SCROLL_LINE_PX) {
    if (chatScroll > 0) chatScroll--;
    chatTouchAccumPx += CHAT_SCROLL_LINE_PX;
  }

  chatScroll = constrain(chatScroll, 0, chatMaxScrollCached);
  if (chatScroll != oldScroll) {
    liveStatus = "Scroll " + String(chatScroll + 1) + "/" + String(chatMaxScrollCached + 1);
    requestRedraw();
  }
}

void endChatTouchScroll() {
  chatTouchScrollActive = false;
  chatTouchScrollMoved = false;
  chatTouchAccumPx = 0;
}

void drawText(int x, int y, const String &s, uint16_t color, uint8_t size = 2) {
  gfx->setTextColor(color);
  gfx->setTextSize(size);
  gfx->setCursor(x, y);
  gfx->print(s);
}

void drawCenteredText(int x, int y, int w, const String &s, uint16_t color, uint8_t size = 2) {
  int charW = 6 * size;
  int tw = s.length() * charW;
  int tx = x + max(0, (w - tw) / 2);
  drawText(tx, y, s, color, size);
}

void drawRightText(int rightX, int y, const String &s, uint16_t color, uint8_t size = 2) {
  int charW = 6 * size;
  int tw = s.length() * charW;
  drawText(rightX - tw, y, s, color, size);
}

String batteryHeaderText() {
  int b = getBatteryPercent();
  if (b < 0) return "--%";
  b = constrain(b, 0, 100);
  return String(b) + "%";
}

uint16_t batteryHeaderColor() {
  int b = getBatteryPercent();
  if (b < 0) return cDim();
  if (b <= 15) return cError();
  if (b <= 35) return cWarn();
  return cTextStrong();
}

String wifiHeaderText() {
  if (portalRunning || appState == STATE_PORTAL) return "AP ON";
  if (wifiConnecting) return "WiFi...";
  if (WiFi.status() == WL_CONNECTED) {
    if (transcriptWifiHoldActive) return "WiFi 10s";
    return "WiFi ON";
  }
  if ((long)(millis() - wifiErrorUntilMs) < 0) return "WiFi ERR";
  return "WiFi OFF";
}

// Big, short WiFi text for the header badge. The full "WiFi OFF" string was
// too small on the AMOLED, especially with dim theme colors. This keeps the
// important state readable at a glance.
String wifiHeaderShortText() {
  if (portalRunning || appState == STATE_PORTAL) return "AP";
  if (wifiConnecting) return "...";
  if (WiFi.status() == WL_CONNECTED) {
    if (transcriptWifiHoldActive) return "10s";
    return "ON";
  }
  if ((long)(millis() - wifiErrorUntilMs) < 0) return "ERR";
  return "OFF";
}

uint16_t wifiHeaderColor() {
  if (portalRunning || appState == STATE_PORTAL) return cAccent2();
  if (wifiConnecting) return cWarn();
  if (WiFi.status() == WL_CONNECTED) return transcriptWifiHoldActive ? cAccent2() : cGood();
  if ((long)(millis() - wifiErrorUntilMs) < 0) return cError();
  return cTextSoft();
}

void drawWifiHeaderBadge() {
  const int x = SAFE_RIGHT - 126;
  const int y = SAFE_Y + 23;
  const int w = 112;
  const int h = 24;
  uint16_t col = wifiHeaderColor();

  // Filled pill + colored outline makes the indicator visible in every theme.
  // WiFi + ON/OFF are both size 2 now, so give them separate, aligned columns.
  gfx->fillRoundRect(x, y, w, h, 12, cBgElevated());
  gfx->drawRoundRect(x, y, w, h, 12, col);
  gfx->fillCircle(x + 11, y + h / 2, 4, col);

  drawText(x + 21, y + 5, "WiFi", cMuted(), 2);
  drawText(x + 72, y + 5, wifiHeaderShortText(), col, 2);
}

int wrapText(const String &text, int x, int y, int w, uint16_t color, uint8_t size, int maxLines = 99) {
  gfx->setTextColor(color);
  gfx->setTextSize(size);
  int charW = 6 * size;
  int lineH = 9 * size + 4;
  int maxChars = max(1, w / charW);
  String remaining = text;
  int lines = 0;

  while (remaining.length() > 0 && lines < maxLines) {
    remaining.trim();
    if (remaining.length() == 0) break;

    String line;
    if ((int)remaining.length() <= maxChars) {
      line = remaining;
      remaining = "";
    } else {
      int cut = maxChars;
      int lastSpace = remaining.lastIndexOf(' ', cut);
      if (lastSpace > 4) cut = lastSpace;
      line = remaining.substring(0, cut);
      remaining = remaining.substring(cut);
    }

    gfx->setCursor(x, y + lines * lineH);
    gfx->print(line);
    lines++;
  }
  return y + lines * lineH;
}


String screenClean(String s) {
  s.replace("\r", " ");
  s.replace("\n", " ");
  s.replace("\t", " ");
  while (s.indexOf("  ") >= 0) s.replace("  ", " ");
  s.trim();
  return s;
}

int drawTextBlockVisible(String text, int x, int y, int w, uint16_t color, uint8_t size, int maxLines) {
  text = screenClean(text);
  gfx->setTextColor(color);
  gfx->setTextSize(size);
  gfx->setTextWrap(false);

  const int charW = 6 * size;
  const int lineH = 10 * size + 6;
  const int maxChars = max(1, w / charW);
  int lines = 0;

  while (text.length() > 0 && lines < maxLines) {
    String line;
    if ((int)text.length() <= maxChars) {
      line = text;
      text = "";
    } else {
      int cut = maxChars;
      int lastSpace = text.lastIndexOf(' ', cut);
      if (lastSpace > 6) cut = lastSpace;
      line = text.substring(0, cut);
      text = text.substring(cut);
      text.trim();
    }

    if (lines == maxLines - 1 && text.length() > 0) {
      if (line.length() > 3) line = line.substring(0, line.length() - 3) + "...";
    }

    gfx->setCursor(x, y + lines * lineH);
    gfx->print(line);
    lines++;
  }

  if (lines == 0) {
    gfx->setCursor(x, y);
    gfx->print("-");
    lines = 1;
  }

  return y + lines * lineH;
}


void buildWrappedLines(String text, int w, uint8_t size, std::vector<String> &lines) {
  lines.clear();
  text = screenClean(text);
  const int charW = 6 * size;
  const int maxChars = max(1, w / charW);

  while (text.length() > 0) {
    text.trim();
    if (text.length() == 0) break;

    String line;
    if ((int)text.length() <= maxChars) {
      line = text;
      text = "";
    } else {
      int cut = maxChars;
      int lastSpace = text.lastIndexOf(' ', cut);
      if (lastSpace > 6) cut = lastSpace;
      line = text.substring(0, cut);
      text = text.substring(cut);
      text.trim();
    }
    lines.push_back(line);
  }

  if (lines.empty()) lines.push_back("-");
}

int drawScrollableTextBlock(String text, int x, int y, int w, uint16_t color,
                            uint8_t size, int maxLines, int scroll, int &totalLinesOut) {
  std::vector<String> lines;
  buildWrappedLines(text, w, size, lines);
  totalLinesOut = (int)lines.size();

  if (scroll < 0) scroll = 0;
  int maxScroll = max(0, totalLinesOut - maxLines);
  if (scroll > maxScroll) scroll = maxScroll;

  gfx->setTextColor(color);
  gfx->setTextSize(size);
  gfx->setTextWrap(false);

  const int lineH = 10 * size + 6;
  int drawn = 0;
  for (int i = scroll; i < totalLinesOut && drawn < maxLines; i++) {
    gfx->setCursor(x, y + drawn * lineH);
    gfx->print(lines[i]);
    drawn++;
  }
  return y + drawn * lineH;
}



String shortModelName(String m) {
  m.trim();
  if (m.length() == 0) return "<unset>";
  if (m.length() <= 30) return m;
  return m.substring(0, 27) + "...";
}

void wrapOneParagraph(String p, int w, uint8_t size, std::vector<String> &lines) {
  p.replace("\t", " ");
  while (p.indexOf("  ") >= 0) p.replace("  ", " ");
  p.trim();
  if (p.length() == 0) {
    lines.push_back("");
    return;
  }

  const int charW = 6 * size;
  const int maxChars = max(1, w / charW);
  while (p.length() > 0) {
    String line;
    if ((int)p.length() <= maxChars) {
      line = p;
      p = "";
    } else {
      int cut = maxChars;
      int lastSpace = p.lastIndexOf(' ', cut);
      if (lastSpace > 6) cut = lastSpace;
      line = p.substring(0, cut);
      p = p.substring(cut);
      p.trim();
    }
    lines.push_back(line);
  }
}

void buildWrappedLinesKeepBreaks(String text, int w, uint8_t size, std::vector<String> &lines) {
  lines.clear();
  text.replace("\r", "");
  String paragraph;
  for (int i = 0; i <= (int)text.length(); i++) {
    char c = (i < (int)text.length()) ? text[i] : '\n';
    if (c == '\n') {
      wrapOneParagraph(paragraph, w, size, lines);
      paragraph = "";
    } else {
      paragraph += c;
    }
  }
  while (!lines.empty() && lines.back().length() == 0) lines.pop_back();
  if (lines.empty()) lines.push_back("-");
}

int drawScrollableTextView(String text, int x, int y, int w, int h, uint16_t color,
                           uint8_t size, int &scroll, int &totalLinesOut) {
  std::vector<String> lines;
  buildWrappedLinesKeepBreaks(text, w, size, lines);
  totalLinesOut = (int)lines.size();

  const int lineH = 10 * size + 6;
  int maxLines = max(1, h / lineH);
  int maxScroll = max(0, totalLinesOut - maxLines);
  if (scroll < 0) scroll = 0;
  if (scroll > maxScroll) scroll = maxScroll;

  gfx->setTextColor(color);
  gfx->setTextSize(size);
  gfx->setTextWrap(false);
  int drawn = 0;
  for (int i = scroll; i < totalLinesOut && drawn < maxLines; i++) {
    gfx->setCursor(x, y + drawn * lineH);
    gfx->print(lines[i]);
    drawn++;
  }
  return maxScroll;
}

int drawCachedChatTextView(const String &text, int x, int y, int w, int h, uint16_t color,
                           uint8_t size, int &scroll, int &totalLinesOut) {
  if (!chatWrapCacheValid || chatWrapCacheText != text || chatWrapCacheW != w || chatWrapCacheSize != size) {
    chatWrapCacheText = text;
    chatWrapCacheW = w;
    chatWrapCacheSize = size;
    buildWrappedLinesKeepBreaks(chatWrapCacheText, w, size, chatWrapCacheLines);
    chatWrapCacheValid = true;
    chatWrapCacheBuilds++;
    USBSerial.printf("[CHAT_CACHE] rebuilt lines=%u builds=%u\n",
                     (unsigned)chatWrapCacheLines.size(), (unsigned)chatWrapCacheBuilds);
  }

  totalLinesOut = (int)chatWrapCacheLines.size();
  const int lineH = 10 * size + 6;
  int maxLines = max(1, h / lineH);
  int maxScroll = max(0, totalLinesOut - maxLines);
  if (scroll < 0) scroll = 0;
  if (scroll > maxScroll) scroll = maxScroll;

  gfx->setTextColor(color);
  gfx->setTextSize(size);
  gfx->setTextWrap(false);
  int drawn = 0;
  for (int i = scroll; i < totalLinesOut && drawn < maxLines; i++) {
    gfx->setCursor(x, y + drawn * lineH);
    gfx->print(chatWrapCacheLines[i]);
    drawn++;
  }
  return maxScroll;
}

String loadingDots() {
  int n = animStep % 4;
  if (n == 0) return "";
  if (n == 1) return ".";
  if (n == 2) return "..";
  return "...";
}

String buildChatViewText(String shownUser, String shownAI, bool shownPending) {
  String out;
  if (shownUser.length() == 0 && shownAI.length() == 0) {
    out = "No messages yet.\n\nTap TALK to speak, or TYPE to write a short message.";
    return out;
  }
  if (shownUser.length()) {
    out += "you\n";
    out += shownUser;
    out += "\n\n";
  }
  out += ASSISTANT_NAME;
  out += "\n";
  if (shownPending) {
    out += "Thinking";
    out += loadingDots();
    if (liveStatus.length()) {
      out += "\n";
      out += liveStatus;
    }
  } else if (shownAI.length()) {
    out += shownAI;
  } else {
    out += "Draft ready. Tap EDIT to fix transcript, or SEND to ask AI.";
  }
  return out;
}

void drawCard(int x, int y, int w, int h, uint16_t fill = 0, uint16_t border = 0) {
  if (fill == 0) fill = cCard();
  if (border == 0) border = cLine();
  gfx->fillRoundRect(x, y, w, h, 22, fill);
  gfx->drawRoundRect(x, y, w, h, 22, border);
}

void addButton(const String &id, int x, int y, int w, int h, const String &label,
               uint16_t fill = 0, uint16_t text = 0, uint8_t size = 2) {
  if (fill == 0) fill = cButton();
  if (text == 0) text = cTextStrong();
  bool primary = (fill == cAccent() || fill == cAccent2());
  gfx->fillRoundRect(x, y, w, h, 16, fill);
  gfx->drawRoundRect(x, y, w, h, 16, primary ? cAccent2() : cLine());
  drawCenteredText(x, y + (h - (8 * size)) / 2, w, label, text, size);
  uiButtons.push_back({id, x, y, w, h, label});
}

void drawHeader(const String &title) {
  drawCard(SAFE_X, SAFE_Y, SAFE_W, 48, cCard(), cLine());
  // Tiny status dot mirrors the big WiFi badge.
  gfx->fillCircle(SAFE_X + 16, SAFE_Y + 24, 4, wifiHeaderColor());
  drawText(SAFE_X + 28, SAFE_Y + 15, title, cTextStrong(), 2);

  // Battery remains large at the top-right.
  drawRightText(SAFE_RIGHT - 14, SAFE_Y + 7, batteryHeaderText(), batteryHeaderColor(), 2);

  // WiFi is now a visible badge instead of tiny text. The short ON/OFF/AP/ERR
  // state is intentionally size 2 so it is readable on the watch.
  drawWifiHeaderBadge();
}

String buddyFaceForState() {
  if (appState == STATE_RECORDING) return "(o_o)";
  if (appState == STATE_TRANSCRIBING) return "(-_-)";
  if (appState == STATE_STREAMING) return "(?_?)";
  if (uiScreen == UI_ERROR_SCREEN) return "(._.)";
  // WiFi Saver keeps WiFi off while idle, so offline is normal now.
  return "(^_^)";
}

String buddySubForState() {
  if (appState == STATE_RECORDING) return "I am listening...";
  if (appState == STATE_TRANSCRIBING) return "Understanding your voice...";
  if (appState == STATE_STREAMING) return "Thinking about it...";
  if (uiScreen == UI_ERROR_SCREEN) return "Something needs attention.";
  return "Hey, ready when you are.";
}

void drawBuddyCenter(const String &subtitle = "") {
  drawCard(SAFE_X, SAFE_Y + 62, SAFE_W, 210, cCard(), cCard2());
  drawCenteredText(SAFE_X, SAFE_Y + 96, SAFE_W, buddyFaceForState(), cAccent2(), 5);
  String sub = subtitle.length() ? subtitle : buddySubForState();
  wrapText(sub, SAFE_X + 24, SAFE_Y + 170, SAFE_W - 48, cMuted(), 2, 3);
}

void drawDotLoader(int x, int y, int w) {
  int cx = x + (w / 2) - 28;
  for (int i = 0; i < 3; i++) {
    bool active = ((animStep + i) % 3) == 0;
    uint16_t col = active ? cAccent() : cCard2();
    int r = active ? 8 : 5;
    gfx->fillCircle(cx + (i * 28), y, r, col);
  }
}

int micVisualLevel() {
  int avg = max(g_lastAvgL, g_lastAvgR);
  int peak = max(abs((int)g_lastPeakL), abs((int)g_lastPeakR));
  return constrain(max(avg * 3, peak / 16), 0, 3200);
}

void drawVoiceAnalyzer(int x, int y, int w, int h) {
  drawCard(x, y, w, h, cCard(), cLine());

  int bars = 18;
  int gap = 4;
  int bw = max(4, (w - 32 - (gap * (bars - 1))) / bars);
  int startX = x + 16;
  int midY = y + h / 2;
  int loud = micVisualLevel();
  int dynamicH = map(loud, 0, 3200, 10, h - 22);

  for (int i = 0; i < bars; i++) {
    int wave = abs(((i * 13 + animStep * 9) % 42) - 21);
    int bh = constrain((dynamicH * (55 + wave * 2)) / 100, 8, h - 18);
    int bx = startX + i * (bw + gap);
    gfx->fillRoundRect(bx, midY - bh / 2, bw, bh, max(2, bw / 2), cAccent());
  }

  String levelText = "voice level " + String(map(loud, 0, 3200, 0, 100)) + "%";
  drawCenteredText(x, y + h - 26, w, levelText, cMuted(), 2);
}

void drawProgressStep(int x, int y, int w, const String &label, bool active, bool done) {
  uint16_t fill = active ? cAccent() : (done ? cCard2() : cCard());
  uint16_t text = active ? cOnAccent() : cMuted();
  gfx->fillRoundRect(x, y, w, 28, 14, fill);
  gfx->drawRoundRect(x, y, w, 28, 14, active ? cAccent() : cCard2());
  drawCenteredText(x, y + 5, w, label, text, 2);
}

void drawStageCard(const String &title, const String &detail, bool showModel = false) {
  int x = SAFE_X + 22;
  int y = SAFE_Y + 286;
  int w = SAFE_W - 44;
  int h = 108;
  drawCard(x, y, w, h, cCard(), cLine());
  drawCenteredText(x, y + 16, w, title + loadingDots(), cTextStrong(), 2);
  wrapText(detail.length() ? detail : liveStatus, x + 18, y + 48, w - 36, cMuted(), 2, 2);
  if (showModel) {
    drawCenteredText(x, y + 78, w, "Mode: " + currentAiName(), cMuted(), 2);
  } else {
    drawDotLoader(x, y + 88, w);
  }
}


void drawHome() {
  clearButtons();
  gfx->fillScreen(cBg());
  drawHeader(ASSISTANT_NAME);
  drawBuddyCenter();

  addButton("speak", SAFE_X + 30, SAFE_Y + 295, SAFE_W - 60, 58, "TALK TO ME", cAccent(), cOnAccent(), 2);
  addButton("chat", SAFE_X + 20, SAFE_Y + 370, 100, 50, "CHAT", cCard2(), cText(), 2);
  addButton("type", SAFE_X + 132, SAFE_Y + 370, 100, 50, "TYPE", cCard2(), cText(), 2);
  addButton("settings", SAFE_X + 244, SAFE_Y + 370, 100, 50, "SET", cCard2(), cText(), 2);

  drawCenteredText(SAFE_X, SAFE_BOTTOM - 30, SAFE_W, liveStatus, cMuted(), 2);
}

void drawListening() {
  clearButtons();
  gfx->fillScreen(cBg());
  drawHeader("Listening");
  drawBuddyCenter("Speak naturally. I am listening" + loadingDots());

  String sec = String(recordSeconds) + " sec recording";
  drawCenteredText(SAFE_X, SAFE_Y + 284, SAFE_W, sec, cAccent(), 2);
  drawVoiceAnalyzer(SAFE_X + 20, SAFE_Y + 316, SAFE_W - 40, 104);
}

bool isSttDeepgramStage() {
  return workStage == WORK_STT_DEEPGRAM_CONNECT || workStage == WORK_STT_DEEPGRAM_UPLOAD || workStage == WORK_STT_DEEPGRAM_WAIT;
}

bool isSttAssemblyStage() {
  return workStage == WORK_STT_ASSEMBLY_BUILD || workStage == WORK_STT_ASSEMBLY_UPLOAD || workStage == WORK_STT_ASSEMBLY_WAIT;
}

bool isAiPrimaryStage() {
  return workStage == WORK_AI_PRIMARY || workStage == WORK_AI_BUILD || workStage == WORK_AI_WAIT;
}

bool isSttUploadOrConnectStage() {
  return workStage == WORK_STT_DEEPGRAM_CONNECT ||
         workStage == WORK_STT_DEEPGRAM_UPLOAD ||
         workStage == WORK_STT_ASSEMBLY_BUILD ||
         workStage == WORK_STT_ASSEMBLY_UPLOAD;
}

bool isSttWaitStage() {
  return workStage == WORK_STT_DEEPGRAM_WAIT ||
         workStage == WORK_STT_ASSEMBLY_WAIT ||
         workStage == WORK_STT_DONE;
}

bool isAiWaitStage() {
  return workStage == WORK_AI_PRIMARY ||
         workStage == WORK_AI_WAIT ||
         workStage == WORK_AI_FALLBACK;
}

bool isAiBuildStage() {
  return workStage == WORK_AI_WIFI || workStage == WORK_AI_BUILD;
}

void drawWorkWaveLoader(int x, int y, int w, int h) {
  gfx->fillRoundRect(x, y, w, h, 18, cCard());
  gfx->drawRoundRect(x, y, w, h, 18, cCard2());

  int bars = 24;
  int gap = 3;
  int bw = max(3, (w - 28 - gap * (bars - 1)) / bars);
  int startX = x + 14;
  int midY = y + h / 2;

  for (int i = 0; i < bars; i++) {
    int phase = (i * 11 + animStep * 7) % 48;
    int wave = abs(phase - 24);
    int bh = 8 + ((24 - wave) * (h - 26)) / 24;
    int bx = startX + i * (bw + gap);
    gfx->fillRoundRect(bx, midY - bh / 2, bw, bh, max(2, bw / 2), cAccent());
  }
}

void drawWorkShimmerLoader(int x, int y, int w, int h) {
  gfx->fillRoundRect(x, y, w, h, 18, cCard());
  gfx->drawRoundRect(x, y, w, h, 18, cCard2());

  // Three soft moving packets. This is not a fake percentage bar.
  int laneH = 10;
  int gapY = 10;
  int baseY = y + 18;
  int maxTravel = max(1, w - 72);

  for (int i = 0; i < 3; i++) {
    int yy = baseY + i * (laneH + gapY);
    gfx->fillRoundRect(x + 16, yy, w - 32, laneH, laneH / 2, cCard2());
    int segW = 34 + i * 12;
    int sx = x + 16 + ((animStep * (5 + i) + i * 31) % maxTravel);
    if (sx + segW > x + w - 16) sx = x + w - 16 - segW;
    gfx->fillRoundRect(sx, yy, segW, laneH, laneH / 2, cAccent());
  }
}

void drawWorkPulseDots(int x, int y, int w, int h, uint16_t col) {
  gfx->fillRoundRect(x, y, w, h, 18, cCard());
  gfx->drawRoundRect(x, y, w, h, 18, col);

  int cy = y + h / 2;
  int startX = x + w / 2 - 48;
  for (int i = 0; i < 4; i++) {
    bool active = ((animStep + i) % 4) == 0;
    int r = active ? 12 : 7;
    gfx->fillCircle(startX + i * 32, cy, r, active ? col : cCard2());
  }
}

void drawAnimatedWorkHero(const String &title, bool isVoiceScreen) {
  WorkStage ws = (WorkStage)workStage;
  markUiPhaseStart(ws);

  int x = SAFE_X;
  int y = SAFE_Y + 58;
  int w = SAFE_W;
  int h = 154;
  bool isErr = (ws == WORK_ERROR || lastError.length());

  drawCard(x, y, w, h, cCard(), isErr ? cError() : cCard2());

  drawText(x + 18, y + 16, title, isErr ? cError() : cAccent(), 2);
  String elapsed = formatMMSS(uiElapsedSec());
  String line = stageSubtitle() + " - " + elapsed;
  wrapText(line, x + 18, y + 44, w - 36, isErr ? cError() : cText(), 2, 2);

  int lx = x + 18;
  int ly = y + 76;
  int lw = w - 36;
  int lh = 62;

  if (isErr) {
    drawWorkPulseDots(lx, ly, lw, lh, cError());
  } else if (isVoiceScreen && (ws == WORK_RECORDING || isSttWaitStage())) {
    drawWorkWaveLoader(lx, ly, lw, lh);
  } else if (isVoiceScreen && isSttUploadOrConnectStage()) {
    drawWorkShimmerLoader(lx, ly, lw, lh);
  } else if (!isVoiceScreen && isAiBuildStage()) {
    drawWorkPulseDots(lx, ly, lw, lh, cAccent());
  } else {
    drawWorkShimmerLoader(lx, ly, lw, lh);
  }
}

void drawLiveDetailCardAt(int x, int y, int w, int h) {
  bool isErr = (workStage == WORK_ERROR || lastError.length());
  drawCard(x, y, w, h, cCard(), isErr ? cError() : cCard2());

  drawText(x + 14, y + 10, isErr ? "ERROR" : "LIVE", isErr ? cError() : cAccent(), 2);
  String detail = isErr ? (lastError.length() ? lastError : liveStatus) : liveStatus;
  wrapText(trimForDisplay(detail, 72), x + 96, y + 10, w - 110, isErr ? cError() : cText(), 2, 2);

  if (!isErr && workNotice.length()) {
    drawText(x + 14, y + h - 17, trimForDisplay(workNotice, 50), cWarn(), 2);
  }
}

void drawMiniWorkHero(const String &title, const String &subtitle) {
  int x = SAFE_X;
  int y = SAFE_Y + 58;
  int w = SAFE_W;
  int h = 92;
  drawCard(x, y, w, h, cCard(), cLine());
  drawCenteredText(x + 10, y + 20, 82, buddyFaceForState(), cAccent(), 4);
  drawText(x + 104, y + 18, title + loadingDots(), cText(), 2);
  wrapText(subtitle, x + 104, y + 46, w - 122, cMuted(), 2, 2);
}

void drawLiveDetailCard() {
  int x = SAFE_X;
  int y = SAFE_Y + 158;
  int w = SAFE_W;
  int h = 74;
  bool isErr = (workStage == WORK_ERROR || lastError.length());
  drawCard(x, y, w, h, cCard(), isErr ? cError() : cCard2());

  drawText(x + 16, y + 12, isErr ? "ERROR" : "LIVE", isErr ? cError() : cAccent(), 2);
  String detail = isErr ? (lastError.length() ? lastError : liveStatus) : liveStatus;
  wrapText(detail, x + 96, y + 12, w - 112, isErr ? cError() : cText(), 2, 2);

  if (!isErr && workNotice.length()) {
    String note = trimForDisplay(workNotice, 46);
    drawText(x + 16, y + 54, note, cWarn(), 2);
  }
}

String workLogStateLabel(uint8_t state) {
  if (state == WORKLOG_DONE) return "OK";
  if (state == WORKLOG_ERROR) return "ERR";
  if (state == WORKLOG_NOTE) return "NOTE";
  return ">>";
}

uint16_t workLogStateColor(uint8_t state) {
  if (state == WORKLOG_DONE) return cGood();
  if (state == WORKLOG_ERROR) return cError();
  if (state == WORKLOG_NOTE) return cWarn();
  return cAccent();
}

void drawWorkLogFeed(int x, int y, int w, int h) {
  bool hasErr = (workStage == WORK_ERROR || lastError.length());
  drawCard(x, y, w, h, cCard(), hasErr ? cError() : cCard2());

  String title = workLogTitle.length() ? workLogTitle : "Live steps";
  drawText(x + 14, y + 10, title, cAccent(), 2);
  drawText(x + w - 78, y + 13, hasErr ? "ERROR" : "LIVE", hasErr ? cError() : cMuted(), 2);

  int rowY = y + 40;
  int rowH = 28;
  int gap = 4;
  int availableH = h - 48;
  int maxRows = min(5, constrain(availableH / (rowH + gap), 1, WORK_LOG_MAX));
  int visible = min(workLogCount, maxRows);
  int start = workLogCount > visible ? workLogCount - visible : 0;

  if (workLogCount == 0) {
    drawText(x + 14, rowY + 8, "Waiting for first display step...", cMuted(), 2);
    return;
  }

  for (int i = 0; i < visible; i++) {
    int idx = start + i;
    uint8_t st = workLogStates[idx];
    String line = workLogLines[idx];
    bool active = (st == WORKLOG_ACTIVE);
    uint16_t col = workLogStateColor(st);
    int yy = rowY + i * (rowH + gap);

    gfx->fillRoundRect(x + 10, yy, w - 20, rowH, 10, cCard());
    gfx->fillRoundRect(x + 10, yy, 5, rowH, 2, col);

    gfx->setTextColor(col);
    gfx->setTextSize(2);
    gfx->setCursor(x + 22, yy + 9);
    gfx->print(workLogStateLabel(st));

    if (active) line += loadingDots();
    line = trimForDisplay(line, 50);
    drawText(x + 54, yy + 9, line, st == WORKLOG_ERROR ? cError() : cText(), 2);
  }
}

void drawInlineWorkLogRows(int x, int y, int w, int h, int maxRowsWanted) {
  int rowH = 22;
  int gap = 3;
  int maxRows = min(maxRowsWanted, constrain(h / (rowH + gap), 1, WORK_LOG_MAX));
  int visible = min(workLogCount, maxRows);
  int start = workLogCount > visible ? workLogCount - visible : 0;

  if (workLogCount == 0) {
    drawText(x, y + 6, "Waiting for first step...", cMuted(), 2);
    return;
  }

  for (int i = 0; i < visible; i++) {
    int idx = start + i;
    uint8_t st = workLogStates[idx];
    String line = workLogLines[idx];
    bool active = (st == WORKLOG_ACTIVE);
    uint16_t col = workLogStateColor(st);
    int yy = y + i * (rowH + gap);

    gfx->fillRoundRect(x, yy, w, rowH, 8, cCard());
    gfx->fillRoundRect(x, yy, 4, rowH, 2, col);

    gfx->setTextColor(col);
    gfx->setTextSize(2);
    gfx->setCursor(x + 10, yy + 6);
    gfx->print(workLogStateLabel(st));

    if (active) line += loadingDots();
    line = trimForDisplay(line, 43);
    drawText(x + 42, yy + 6, line, st == WORKLOG_ERROR ? cError() : cText(), 2);
  }
}

void drawAiThinkingLoader(int x, int y, int w, int h) {
  gfx->fillRoundRect(x, y, w, h, 16, cCard());
  gfx->drawRoundRect(x, y, w, h, 16, cCard2());

  int cx = x + w / 2;
  int cy = y + h / 2;
  for (int i = 0; i < 6; i++) {
    float angle = (i * 60 + animStep * 18) * 3.14159f / 180.0f;
    int dx = (int)(cos(angle) * 22);
    int dy = (int)(sin(angle) * 12);
    bool bright = (i == (animStep % 6));
    gfx->fillCircle(cx + dx, cy + dy, bright ? 7 : 4, bright ? cAccent() : cCard2());
  }
}

void drawSegmentedProgressBar(int x, int y, int w, int h, int totalSteps,
                              int currentStep, bool finished, bool errorState) {
  totalSteps = constrain(totalSteps, 2, 10);
  currentStep = constrain(currentStep, 1, totalSteps);

  int gap = 4;
  int segW = max(8, (w - gap * (totalSteps - 1)) / totalSteps);
  int usedW = segW * totalSteps + gap * (totalSteps - 1);
  int startX = x + max(0, (w - usedW) / 2);

  for (int i = 1; i <= totalSteps; i++) {
    int sx = startX + (i - 1) * (segW + gap);
    bool done = finished || (i < currentStep);
    bool active = (!finished && i == currentStep);
    uint16_t col = errorState && active ? cError() : (active ? cAccent2() : (done ? cAccent() : cCard2()));
    gfx->fillRoundRect(sx, y, segW, h, h / 2, cCard2());
    if (done || active) gfx->fillRoundRect(sx, y, segW, h, h / 2, col);
  }
}

void drawSmoothProgressBar(int x, int y, int w, int h, bool errorState) {
  int visual = constrain(smoothProgressVisual, 0, PROGRESS_FULL);
  int fillW = ((w - 4) * visual) / PROGRESS_FULL;
  fillW = constrain(fillW, 0, w - 4);

  gfx->fillRoundRect(x, y, w, h, h / 2, cProgressTrack());
  gfx->drawRoundRect(x, y, w, h, h / 2, errorState ? cError() : cAccent2());

  if (fillW > 0) {
    gfx->fillRoundRect(x + 2, y + 2, fillW, h - 4, max(2, (h - 4) / 2), errorState ? cError() : cProgress());

    // Moving bright packet inside the filled part. Cheap, but it feels like a
    // game loader instead of a dead status bar.
    if (!errorState && visual < PROGRESS_FULL && fillW > 28) {
      int shimmerW = min(42, max(14, fillW / 3));
      int travel = max(1, fillW - shimmerW);
      int sx = x + 2 + ((animStep * 11) % travel);
      gfx->fillRoundRect(sx, y + 5, shimmerW, max(4, h - 10), max(2, (h - 10) / 2), rgb(255, 255, 255));
    }
  }
}

void drawSegmentedProgressCard(int x, int y, int w, int h, bool isVoiceWork) {
  WorkStage ws = (WorkStage)workStage;
  markUiPhaseStart(ws);

  bool errorState = (ws == WORK_ERROR || lastError.length());
  int totalSteps = isVoiceWork ? STT_SEGMENT_COUNT : AI_SEGMENT_COUNT;
  int stageStep = isVoiceWork ? sttSegmentForStage(ws) : aiSegmentForStage(ws);
  int activeStepSnapshot = activeSegProgressStep;
  SegProgressKind kindSnapshot = (SegProgressKind)activeSegProgressKind;
  int step = stageStep;
  if (((isVoiceWork && kindSnapshot == SEG_PROGRESS_STT) ||
       (!isVoiceWork && kindSnapshot == SEG_PROGRESS_AI)) &&
      activeStepSnapshot >= 1 && activeStepSnapshot <= totalSteps) {
    step = activeStepSnapshot;
    if (stageStep > step) step = stageStep;
  }
  if (errorState) step = constrain(activeStepSnapshot, 1, totalSteps);

  bool finished = (!errorState) &&
                  ((isVoiceWork && ws == WORK_STT_DONE) ||
                   (!isVoiceWork && ws == WORK_AI_DONE));

  String title;
  if (errorState) title = isVoiceWork ? "Voice error" : "Buddy error";
  else title = isVoiceWork ? "Voice to text" : "AI reply loading";

  String stepLabel = progressHeadline(isVoiceWork, step, ws);
  String detail;
  if (errorState) {
    detail = lastError.length() ? lastError : liveStatus;
  } else {
    // Prefer the human-readable live status. The rolling debug line is still
    // useful as backup, but should not be the main visible stage text.
    detail = liveStatus;
    if (detail.length() == 0) detail = latestWorkLineForDisplay();
  }

  String subDetail;
  if (isVoiceWork) {
    subDetail = "Step " + String(step) + "/" + String(totalSteps) + " - " + currentSttName();
  } else {
    subDetail = "Step " + String(step) + "/" + String(totalSteps) + " - " + currentAiName() + " mode";
  }

  int pct = errorState ? smoothProgressPercent() : (finished ? 100 : smoothProgressPercent());

  drawCard(x, y, w, h, cCard(), errorState ? cError() : cCard2());

  drawText(x + 14, y + 12, title + (finished || errorState ? "" : loadingDots()), errorState ? cError() : cTextStrong(), 2);
  drawText(x + w - 56, y + 13, String(pct) + "%", errorState ? cError() : cAccent2(), 2);
  drawText(x + 14, y + 38, formatMMSS(uiElapsedSec()), cMuted(), 2);

  int barY = y + 58;
  drawSmoothProgressBar(x + 14, barY, w - 28, 28, errorState);

  int miniY = barY + 40;
  drawSegmentedProgressBar(x + 14, miniY, w - 28, 7, totalSteps, step, finished, errorState);

  int textY = miniY + 20;
  drawText(x + 14, textY, subDetail, cDim(), 2);
  drawText(x + 14, textY + 20, errorState ? "ERROR" : stepLabel, errorState ? cError() : cTextStrong(), 2);

  uint16_t detailColor = errorState ? cError() : cMuted();
  int detailMaxLines = (h >= 220) ? 3 : 2;
  wrapText(trimForDisplay(detail, 100), x + 14, textY + 48, w - 28, detailColor, 2, detailMaxLines);

  if (!errorState && workNotice.length() && h >= 190) {
    int noticeY = y + h - 32;
    gfx->fillRoundRect(x + 14, noticeY, w - 28, 24, 10, cWarn());
    drawText(x + 24, noticeY + 7, trimForDisplay("NOTICE: " + workNotice, 48), rgb(20, 18, 10), 2);
  }
}

void drawInlineWorkCard(int x, int y, int w, int h, bool isVoiceWork) {
  // Inline chat loader has much less vertical room than the full STT/AI
  // screens. The full segmented card became too tall after making all text
  // size 2, so its detail lines could spill into the footer/buttons. Keep
  // this compact card self-contained and readable.
  WorkStage ws = (WorkStage)workStage;
  markUiPhaseStart(ws);

  bool errorState = (ws == WORK_ERROR || lastError.length());
  int totalSteps = isVoiceWork ? STT_SEGMENT_COUNT : AI_SEGMENT_COUNT;
  int stageStep = isVoiceWork ? sttSegmentForStage(ws) : aiSegmentForStage(ws);
  int activeStepSnapshot = activeSegProgressStep;
  SegProgressKind kindSnapshot = (SegProgressKind)activeSegProgressKind;

  int step = stageStep;
  if (((isVoiceWork && kindSnapshot == SEG_PROGRESS_STT) ||
       (!isVoiceWork && kindSnapshot == SEG_PROGRESS_AI)) &&
      activeStepSnapshot >= 1 && activeStepSnapshot <= totalSteps) {
    step = max(step, activeStepSnapshot);
  }
  if (errorState) step = constrain(activeStepSnapshot, 1, totalSteps);

  bool finished = (!errorState) &&
                  ((isVoiceWork && ws == WORK_STT_DONE) ||
                   (!isVoiceWork && ws == WORK_AI_DONE));

  int pct = errorState ? smoothProgressPercent() : (finished ? 100 : smoothProgressPercent());
  String title = errorState ? (isVoiceWork ? "Voice error" : "Buddy error")
                            : (isVoiceWork ? "Voice to text" : "AI reply");
  String stepLabel = progressHeadline(isVoiceWork, step, ws);
  String detail = errorState ? (lastError.length() ? lastError : liveStatus) : liveStatus;
  if (detail.length() == 0) detail = stepLabel;

  drawCard(x, y, w, h, cCard(), errorState ? cError() : cCard2());

  drawText(x + 14, y + 12, title + (finished || errorState ? "" : loadingDots()),
           errorState ? cError() : cTextStrong(), 2);
  drawText(x + w - 62, y + 12, String(pct) + "%", errorState ? cError() : cAccent2(), 2);

  int barY = y + 42;
  drawSmoothProgressBar(x + 14, barY, w - 28, 24, errorState);

  int segY = barY + 34;
  drawSegmentedProgressBar(x + 14, segY, w - 28, 7, totalSteps, step, finished, errorState);

  String sub = "Step " + String(step) + "/" + String(totalSteps) + " - " +
               (isVoiceWork ? currentSttName() : currentAiName());
  drawText(x + 14, segY + 18, sub, cDim(), 2);

  // One status line only, so it cannot collide with the bottom controls.
  drawText(x + 14, segY + 42, trimForDisplay(detail, 30),
           errorState ? cError() : cTextSoft(), 2);
}

void drawStepRow(int x, int y, int w, const String &label, const String &sub,
                 bool done, bool active, bool errorState) {
  uint16_t border = errorState ? cError() : (active ? cAccent() : cCard2());
  uint16_t iconFill = errorState ? cError() : (done ? cGood() : (active ? cAccent() : cCard2()));
  uint16_t labelColor = errorState ? cError() : (active ? cText() : (done ? cText() : cMuted()));

  gfx->fillRoundRect(x, y, w, 32, 16, cCard());
  gfx->drawRoundRect(x, y, w, 32, 16, border);
  gfx->fillCircle(x + 18, y + 16, 9, iconFill);

  gfx->setTextSize(2);
  gfx->setTextColor(cTextStrong());
  gfx->setCursor(x + 14, y + 12);
  if (errorState) gfx->print("!");
  else if (done) gfx->print("*");
  else if (active) gfx->print(">");
  else gfx->print("-");

  drawText(x + 36, y + 7, label, labelColor, 2);
  if (sub.length()) drawText(x + 178, y + 7, sub, cMuted(), 2);
  if (active && !errorState) drawDotLoader(x + w - 76, y + 17, 70);
}

void drawTranscribing() {
  clearButtons();
  gfx->fillScreen(cBg());
  drawHeader("Voice to Text");

  drawSegmentedProgressCard(SAFE_X, SAFE_Y + 68, SAFE_W, 238, true);

  int previewY = SAFE_Y + 320;
  drawCard(SAFE_X, previewY, SAFE_W, 96, cCard(), cLine());
  drawText(SAFE_X + 16, previewY + 12, "Transcript preview", cAccent2(), 2);

  String preview;
  if (lastTranscript.length()) preview = lastTranscript;
  else if (workStage == WORK_ERROR) preview = lastError.length() ? lastError : "Voice failed.";
  else preview = "Waiting for your words...";

  wrapText(trimForDisplay(preview, 150), SAFE_X + 16, previewY + 34, SAFE_W - 32,
           (workStage == WORK_ERROR) ? cError() : (lastTranscript.length() ? cText() : cMuted()), 1, 3);

  if (gBusy) {
    addButton("cancel_work", SAFE_X + 80, SAFE_BOTTOM - 58, SAFE_W - 160, 46, "CANCEL", cError(), cText(), 2);
  } else {
    drawCenteredText(SAFE_X, SAFE_BOTTOM - 50, SAFE_W,
                     "When ready, you can EDIT before SEND", cMuted(), 1);
  }
}


void drawThinking() {
  // Normal AI loading is shown inline inside drawChat().
  // This fallback screen remains for manual/debug paths only.
  clearButtons();
  gfx->fillScreen(cBg());
  drawHeader("AI loading");
  drawSegmentedProgressCard(SAFE_X, SAFE_Y + 86, SAFE_W, 250, false);
  if (gBusy) {
    addButton("cancel_work", SAFE_X + 80, SAFE_BOTTOM - 58, SAFE_W - 160, 46, "CANCEL", cError(), cText(), 2);
  } else {
    drawCenteredText(SAFE_X, SAFE_BOTTOM - 50, SAFE_W,
                     "AI loading also appears inside chat", cMuted(), 1);
  }
}


void drawErrorScreen() {
  clearButtons();
  gfx->fillScreen(cBg());
  drawHeader("Error");
  drawMiniWorkHero("Needs attention", "The process stopped. I kept the exact reason below so you can debug it.");
  drawLiveDetailCard();

  String details;
  if (lastError.length()) details += lastError;
  if (workNotice.length()) {
    if (details.length()) details += "\n";
    details += workNotice;
  }
  if (details.isEmpty()) details = liveStatus.length() ? liveStatus : "Unknown error";
  wrapText(details, SAFE_X + 16, SAFE_Y + 248, SAFE_W - 32, cMuted(), 2, 8);

  addButton("chat", SAFE_X + 24, SAFE_BOTTOM - 58, 146, 46, "CHAT", cCard2(), cText(), 2);
  addButton("back_home", SAFE_X + 194, SAFE_BOTTOM - 58, 146, 46, "HOME", cAccent(), cOnAccent(), 2);
}


void drawReplyLengthRail(int x, int y, int h, int totalLines, int visibleLines, int scroll, int maxScroll) {
  int trackH = max(24, h);
  gfx->fillRoundRect(x, y, 7, trackH, 4, cReplyRailTrack());

  int barH;
  if (totalLines <= visibleLines || maxScroll <= 0) {
    barH = trackH;
  } else {
    barH = max(18, (trackH * visibleLines) / max(visibleLines, totalLines));
  }

  int barY = y;
  if (maxScroll > 0 && trackH > barH) {
    barY = y + ((trackH - barH) * constrain(scroll, 0, maxScroll)) / maxScroll;
  }

  gfx->fillRoundRect(x, barY, 7, barH, 4, cReplyRailThumb());
}

String replySizeLabel(int totalLines, int visibleLines, int maxScroll) {
  if (totalLines <= visibleLines) return "short reply";
  if (totalLines <= visibleLines * 2) return "medium reply";
  return "long reply";
}

void drawChat() {
  clearButtons();
  gfx->fillScreen(cBg());
  drawHeader("Chat");

  int historyCount = (int)convHistory.size();
  chatHistoryMaxOffsetCached = max(0, historyCount - 1);
  chatHistoryOffset = constrain(chatHistoryOffset, 0, chatHistoryMaxOffsetCached);

  bool viewingLatest = (chatHistoryOffset == 0);
  bool inlineSTT = false; // STT uses its own segmented Voice-to-Text screen.
  bool inlineAI = viewingLatest && ((appState == STATE_STREAMING) ||
                  (workStage == WORK_AI_WIFI ||
                   workStage == WORK_AI_BUILD ||
                   workStage == WORK_AI_PRIMARY ||
                   workStage == WORK_AI_WAIT ||
                   workStage == WORK_AI_FALLBACK));

  bool inlineErr = viewingLatest && (workStage == WORK_ERROR && lastError.length());
  bool showInlineWork = inlineAI || inlineErr;
  bool inlineIsVoice = inlineErr && (activeSegProgressKind == SEG_PROGRESS_STT);

  String shownUser = "";
  String shownAI = "";
  bool shownPending = false;
  bool showingDraft = false;
  int selectedHistoryIndex = -1;

  // Priority order:
  // 1) Editable draft/transcript waiting to be sent.
  // 2) Current visible error/status if a worker failed before adding history.
  // 3) Selected conversation from history using BACK / NEXT.
  // 4) Visible fallback state.
  if (inputDraft.length() > 0 && !gBusy && appState != STATE_STREAMING) {
    showingDraft = true;
    shownUser = inputDraft;
    shownAI = "Draft ready. Tap EDIT to fix transcript, or SEND to ask AI.";
    shownPending = false;
  } else if (inlineErr && visibleAssistantText.length() > 0 && historyCount == 0) {
    shownUser = visibleUserText.length() ? visibleUserText : "Voice message";
    shownAI = visibleAssistantText;
    shownPending = false;
  } else if (historyCount > 0) {
    selectedHistoryIndex = constrain(historyCount - 1 - chatHistoryOffset, 0, historyCount - 1);
    ConversationPair selected = convHistory[selectedHistoryIndex];
    shownUser = selected.user;
    shownAI = selected.assistant;
    shownPending = (selectedHistoryIndex == historyCount - 1 && shownAI.length() == 0 && gBusy && appState == STATE_STREAMING);
  } else {
    shownUser = visibleUserText;
    shownAI = visibleAssistantText;
    shownPending = visibleAiPending;
  }

  if (showInlineWork && shownUser.length() == 0) {
    shownUser = inlineIsVoice ? "Voice message" : "Message";
  }
  if (showInlineWork && shownAI.length() == 0 && !shownPending) {
    shownAI = inlineIsVoice ? "Voice step paused..." : "Preparing reply...";
  }

  USBSerial.printf("[CHAT] inline=%d stt=%d ai=%d err=%d userLen=%u aiLen=%u draftLen=%u pending=%d history=%u histOffset=%d scroll=%d\n",
                   showInlineWork ? 1 : 0,
                   inlineSTT ? 1 : 0,
                   inlineAI ? 1 : 0,
                   inlineErr ? 1 : 0,
                   (unsigned)shownUser.length(),
                   (unsigned)shownAI.length(),
                   (unsigned)inputDraft.length(),
                   shownPending ? 1 : 0,
                   (unsigned)convHistory.size(),
                   chatHistoryOffset,
                   chatScroll);

  String chatText = buildChatViewText(shownUser, shownAI, shownPending && !showInlineWork);

  int viewX = SAFE_X;
  int viewY = SAFE_Y + 58;
  int viewW = SAFE_W;
  int viewH = SAFE_H - 132;
  drawCard(viewX, viewY, viewW, viewH, cCard(), cLine());

  int sideX = SAFE_RIGHT - 52;
  bool showHistoryNav = (!showingDraft && historyCount > 1);

  int textX = viewX + 16;
  int textY = viewY + 16;
  int textW = showHistoryNav ? (viewW - 86) : (viewW - 50);
  int metaH = showInlineWork ? 0 : 24;
  int textH = showInlineWork ? 108 : (viewH - 36 - metaH);
  int totalLines = 0;
  int maxScroll = drawCachedChatTextView(chatText, textX, textY, textW, textH, cTextSoft(), 2, chatScroll, totalLines);

  int replyLines = 0;
  if (shownAI.length() > 0 && !shownPending) {
    std::vector<String> replyOnlyLines;
    buildWrappedLinesKeepBreaks(shownAI, textW, 2, replyOnlyLines);
    replyLines = (int)replyOnlyLines.size();
  }

  const int chatLineH = 10 * 2 + 6;
  int visibleLines = max(1, textH / chatLineH);

  // Touch-drag still scrolls long answers inside the selected conversation.
  // The old visible ^ / v controls are gone; right-side buttons now browse conversations.
  chatMaxScrollCached = maxScroll;
  chatScrollAreaValid = (maxScroll > 0);
  chatScrollAreaX = textX;
  chatScrollAreaY = textY;
  chatScrollAreaW = textW;
  chatScrollAreaH = textH;

  if (!showInlineWork) {
    int railX = showHistoryNav ? (sideX - 15) : (viewX + viewW - 23);
    drawReplyLengthRail(railX, textY + 2, max(24, textH - 4), totalLines, visibleLines, chatScroll, maxScroll);
    // No extra scroll/reply metadata here. The right-side rail + history 2/2
    // counter are enough and keep the chat page cleaner.
  }

  if (showHistoryNav) {
    uint16_t olderFill = (chatHistoryOffset < chatHistoryMaxOffsetCached) ? cCard2() : cCard();
    uint16_t newerFill = (chatHistoryOffset > 0) ? cCard2() : cCard();
    addButton("conv_prev", sideX, viewY + 18, 42, 44, "<", olderFill, cText(), 2);
    addButton("conv_next", sideX, viewY + 76, 42, 44, ">", newerFill, cText(), 2);

    String histLabel = String(historyCount - chatHistoryOffset) + "/" + String(historyCount);
    drawCenteredText(sideX - 4, viewY + 132, 50, histLabel, cMuted(), 2);

  }

  if (showInlineWork) {
    int cardX = viewX + 12;
    int cardY = viewY + 132;
    int cardW = viewW - 24;
    int cardH = viewH - 144;
    drawInlineWorkCard(cardX, cardY, cardW, cardH, inlineIsVoice);
  }

  String footer;
  if (showInlineWork) footer = ""; // inline progress card already explains the running task
  else if (showingDraft) footer = "Draft ready";
  else if (gBusy && appState == STATE_STREAMING && viewingLatest) footer = (liveStatus.length() ? liveStatus : "AI is replying") + loadingDots();
  else if (historyCount > 1) footer = "";
  else if (maxScroll > 0) footer = "Drag text to scroll";
  else footer = liveStatus;
  if (footer.length()) {
    drawCenteredText(SAFE_X, SAFE_BOTTOM - 88, SAFE_W, footer, inlineErr ? cError() : cMuted(), 2);
  }

  String typeLabel = (inputDraft.length() && !gBusy) ? "EDIT" : "TYPE";
  bool canCancel = gBusy && (appState == STATE_STREAMING || appState == STATE_TRANSCRIBING || appState == STATE_RECORDING);
  uint16_t sendFill = canCancel ? cError() : ((inputDraft.length() && !gBusy) ? cAccent() : cCard2());
  addButton("speak", SAFE_X + 4, SAFE_BOTTOM - 62, 86, 50, "TALK", cAccent(), cOnAccent(), 2);
  addButton("type", SAFE_X + 96, SAFE_BOTTOM - 62, 76, 50, typeLabel, cCard2(), cText(), 2);
  addButton(canCancel ? "cancel_work" : "send", SAFE_X + 178, SAFE_BOTTOM - 62, 80, 50, canCancel ? "STOP" : "SEND", sendFill, cOnAccent(), 2);
  addButton("back_home", SAFE_X + 264, SAFE_BOTTOM - 62, 94, 50, "HOME", cCard2(), cText(), 2);
}


void drawKeyboardKey(const String &id, int x, int y, int w, int h, const String &label) {
  addButton(id, x, y, w, h, label, cCard2(), cText(), 2);
}

void drawKeyboard() {
  clearButtons();
  gfx->fillScreen(cBg());
  drawHeader("Type");

  drawCard(SAFE_X, SAFE_Y + 58, SAFE_W, 116, cCard(), cLine());
  drawText(SAFE_X + 16, SAFE_Y + 72, "Message", cAccent2(), 2);
  String show = keyboardDraft.length() ? keyboardDraft : "Type your message...";
  drawTextBlockVisible(show, SAFE_X + 16, SAFE_Y + 96, SAFE_W - 32,
                       keyboardDraft.length() ? cText() : cMuted(), 2, 3);
  drawText(SAFE_RIGHT - 76, SAFE_Y + 72, String(keyboardDraft.length()) + " ch", cMuted(), 2);

  int keyY = SAFE_Y + 194;
  int keyH = 40;
  int gap = 4;

  const char *rowsABC[] = {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};
  const char *rowsNUM[] = {"1234567890", "@#&-_.,?!", "+*/=:;()"};
  const char **rows = keyboardPage == 0 ? rowsABC : rowsNUM;

  for (int r = 0; r < 3; r++) {
    String row = rows[r];
    int count = row.length();
    int x = SAFE_X + (r == 1 ? 10 : (r == 2 ? 28 : 0));
    int usableW = SAFE_W - (r == 1 ? 20 : (r == 2 ? 56 : 0));
    int keyW = (usableW - (count - 1) * gap) / count;
    for (int i = 0; i < count; i++) {
      String ch = row.substring(i, i + 1);
      drawKeyboardKey("key:" + ch, x + i * (keyW + gap), keyY + r * (keyH + 8), keyW, keyH, ch);
    }
  }

  int by = SAFE_BOTTOM - 102;
  drawKeyboardKey("key:PAGE", SAFE_X, by, 68, 46, keyboardPage == 0 ? "123" : "ABC");
  drawKeyboardKey("key:SPACE", SAFE_X + 74, by, 130, 46, "SPACE");
  drawKeyboardKey("key:DEL", SAFE_X + 210, by, 66, 46, "DEL");
  drawKeyboardKey("key:DONE", SAFE_X + 282, by, 80, 46, "DONE");
  addButton("chat", SAFE_X + 40, SAFE_BOTTOM - 48, SAFE_W - 80, 42, "BACK", cCard2(), cText(), 2);

  USBSerial.printf("[KEYBOARD] draw draftLen=%u buttons=%u page=%d\n", (unsigned)keyboardDraft.length(), (unsigned)uiButtons.size(), keyboardPage);
}

void drawSettingsHome() {
  clearButtons();
  gfx->fillScreen(cBg());
  drawHeader("Settings");

  int y = SAFE_Y + 76;
  int h = 52;
  int gap = 12;
  addButton("settings_voice", SAFE_X + 20, y, SAFE_W - 40, h, "Voice", cCard2(), cText(), 2); y += h + gap;
  addButton("settings_ai", SAFE_X + 20, y, SAFE_W - 40, h, "AI Models", cCard2(), cText(), 2); y += h + gap;
  addButton("settings_display", SAFE_X + 20, y, SAFE_W - 40, h, "Display", cCard2(), cText(), 2); y += h + gap;
  addButton("settings_wifi", SAFE_X + 20, y, SAFE_W - 40, h, "WiFi", cCard2(), cText(), 2); y += h + gap;
  addButton("settings_debug", SAFE_X + 20, y, SAFE_W - 40, h, "Debug", cCard2(), cText(), 2); y += h + gap;
  addButton("settings_about", SAFE_X + 20, y, SAFE_W - 40, h, "About", cCard2(), cText(), 2);
  addButton("back_home", SAFE_X + 70, SAFE_BOTTOM - 54, SAFE_W - 140, 44, "BACK", cAccent(), cOnAccent(), 2);
}

void drawSettingRow(const String &title, const String &value, int y, const String &minusId, const String &plusId) {
  drawCard(SAFE_X, y, SAFE_W, 76, cCard(), cLine());
  drawText(SAFE_X + 16, y + 12, title, cTextStrong(), 2);
  drawText(SAFE_X + 18, y + 42, value, cAccent2(), 2);
  if (minusId.length()) addButton(minusId, SAFE_RIGHT - 112, y + 16, 44, 40, "-", cCard2(), cText(), 3);
  if (plusId.length()) addButton(plusId, SAFE_RIGHT - 58, y + 16, 44, 40, "+", cAccent(), cOnAccent(), 3);
}

void drawCompactSettingRow(const String &title, const String &value, int y, const String &minusId, const String &plusId) {
  drawCard(SAFE_X, y, SAFE_W, 54, cCard(), cLine());
  drawText(SAFE_X + 16, y + 8, title, cTextStrong(), 2);
  drawText(SAFE_X + 16, y + 28, value, cAccent2(), 2);
  if (minusId.length()) addButton(minusId, SAFE_RIGHT - 104, y + 8, 42, 38, "-", cCard2(), cText(), 3);
  if (plusId.length()) addButton(plusId, SAFE_RIGHT - 54, y + 8, 42, 38, "+", cAccent(), cOnAccent(), 3);
}

void drawSettingsVoice() {
  clearButtons();
  gfx->fillScreen(cBg());
  drawHeader("Voice");
  drawSettingRow("STT Mode", currentSttName(), SAFE_Y + 70, "voice_stt", "voice_stt");
  drawSettingRow("Record Time", String(recordSeconds) + " sec", SAFE_Y + 154, "rec_minus", "rec_plus");
  drawSettingRow("Fallback", onOff(sttFallbackEnabled), SAFE_Y + 238, "stt_fb", "stt_fb");
  drawSettingRow("Auto Send", onOff(autoSendAfterSTT), SAFE_Y + 322, "autosend", "autosend");
  addButton("settings", SAFE_X + 70, SAFE_BOTTOM - 46, SAFE_W - 140, 40, "BACK", cAccent(), cOnAccent(), 2);
}

void drawSettingsAI() {
  clearButtons();
  gfx->fillScreen(cBg());
  drawHeader("AI");

  drawSettingRow("AI Mode", currentAiName(), SAFE_Y + 62, "ai_mode", "ai_mode");
  drawSettingRow("Fallback", onOff(aiFallbackEnabled), SAFE_Y + 144, "ai_fb", "ai_fb");
  drawSettingRow("Max Tokens", String(maxOutputTokens), SAFE_Y + 226, "tokens_minus", "tokens_plus");

  drawCard(SAFE_X, SAFE_Y + 310, SAFE_W, 112, cCard(), cLine());
  drawText(SAFE_X + 16, SAFE_Y + 322, "Portal Models", cTextStrong(), 2);
  drawText(SAFE_X + 16, SAFE_Y + 344, "Smart: " + shortModelName(OR_MODEL1), currentAI == AI_MODE_SMART ? cAccent() : cMuted(), 2);
  drawText(SAFE_X + 16, SAFE_Y + 364, "Fast: " + shortModelName(OR_MODEL2), currentAI == AI_MODE_FAST ? cAccent() : cMuted(), 2);
  drawText(SAFE_X + 16, SAFE_Y + 384, "Creative: " + shortModelName(OR_MODEL3), currentAI == AI_MODE_CREATIVE ? cAccent() : cMuted(), 2);
  drawText(SAFE_X + 16, SAFE_Y + 404, "Edit IDs in Config Portal", cMuted(), 2);

  addButton("settings", SAFE_X + 70, SAFE_BOTTOM - 44, SAFE_W - 140, 38, "BACK", cAccent(), cOnAccent(), 2);
}

void drawThemePreviewCard(int y) {
  drawCard(SAFE_X, y, SAFE_W, 58, cCard(), cLine());
  drawText(SAFE_X + 16, y + 10, "Theme preview", cTextStrong(), 2);
  int sx = SAFE_X + 18;
  int sy = y + 32;
  int sw = 42;
  gfx->fillRoundRect(sx, sy, sw, 12, 6, cBgElevated()); sx += sw + 8;
  gfx->fillRoundRect(sx, sy, sw, 12, 6, cCard2()); sx += sw + 8;
  gfx->fillRoundRect(sx, sy, sw, 12, 6, cAccent()); sx += sw + 8;
  gfx->fillRoundRect(sx, sy, sw, 12, 6, cAccent2()); sx += sw + 8;
  gfx->fillRoundRect(sx, sy, sw, 12, 6, cGood()); sx += sw + 8;
  gfx->fillRoundRect(sx, sy, sw, 12, 6, cError());
  drawText(SAFE_RIGHT - 116, y + 10, String(themeMode + 1) + "/" + String(THEME_COUNT), cMuted(), 2);
}

void drawSettingsDisplay() {
  clearButtons();
  gfx->fillScreen(cBg());
  drawHeader("Display");
  drawCompactSettingRow("Brightness", String(brightnessValue), SAFE_Y + 58, "bright_minus", "bright_plus");
  drawCompactSettingRow("Theme", themeName(), SAFE_Y + 118, "theme", "theme");
  drawThemePreviewCard(SAFE_Y + 178);
  String fs = fontSizeMode == 0 ? "Small" : (fontSizeMode == 2 ? "Large" : "Medium");
  drawCompactSettingRow("Font Size", fs, SAFE_Y + 246, "font", "font");
  drawCompactSettingRow("Sleep Timeout", sleepTimeoutName(), SAFE_Y + 306, "sleep_timeout", "sleep_timeout");
  addButton("settings", SAFE_X + 70, SAFE_BOTTOM - 46, SAFE_W - 140, 40, "BACK", cAccent(), cOnAccent(), 2);
}

void drawSettingsWifi() {
  clearButtons();
  gfx->fillScreen(cBg());
  drawHeader("WiFi");
  drawCard(SAFE_X, SAFE_Y + 70, SAFE_W, 116, cCard(), cLine());
  drawText(SAFE_X + 16, SAFE_Y + 88, "Current", cTextStrong(), 2);
  String wifiLine = wifiHeaderText();
  if (WiFi.status() == WL_CONNECTED) wifiLine += " / " + WiFi.SSID() + " / " + WiFi.localIP().toString();
  wrapText(wifiLine, SAFE_X + 16, SAFE_Y + 124, SAFE_W - 32, wifiHeaderColor(), 2, 2);
  drawCompactSettingRow("WiFi Saver", onOff(wifiSaverEnabled), SAFE_Y + 204, "wifi_saver", "wifi_saver");
  drawText(SAFE_X + 18, SAFE_Y + 264, "STT/AI only - transcript hold 10s", cDim(), 2);
  addButton("wifi_test", SAFE_X + 30, SAFE_Y + 286, SAFE_W - 60, 46, "TEST WIFI", cCard2(), cText(), 2);
  addButton("portal", SAFE_X + 30, SAFE_Y + 342, SAFE_W - 60, 46, "OPEN PORTAL", cAccent(), cOnAccent(), 2);
  addButton("settings", SAFE_X + 70, SAFE_BOTTOM - 46, SAFE_W - 140, 40, "BACK", cCard2(), cText(), 2);
}

void drawSettingsDebug() {
  clearButtons();
  gfx->fillScreen(cBg());
  drawHeader("Debug");
  drawSettingRow("Debug Logs", onOff(debugLogsEnabled), SAFE_Y + 58, "dbg_logs", "dbg_logs");
  drawSettingRow("Mic Debug", onOff(micDebugEnabled), SAFE_Y + 142, "mic_dbg_toggle", "mic_dbg_toggle");

  drawCard(SAFE_X, SAFE_Y + 224, SAFE_W, 86, cCard(), cLine());
  String mem;
  mem += "Heap now: " + String(ESP.getFreeHeap() / 1024) + " KB\n";
  mem += "PSRAM now: " + String(ESP.getFreePsram() / 1024) + " KB\n";
  mem += "Last run: " + lastRunMemNote + "\n";
  mem += "Chat wrap cache: " + String((unsigned)chatWrapCacheLines.size()) + " lines / " + String((unsigned)chatWrapCacheBuilds) + " builds";
  wrapText(mem, SAFE_X + 16, SAFE_Y + 238, SAFE_W - 32, cTextSoft(), 2, 5);

  addButton("mic_test", SAFE_X + 18, SAFE_Y + 326, 154, 46, "MIC TEST", cCard2(), cText(), 2);
  addButton("clear_chat", SAFE_X + 190, SAFE_Y + 326, 154, 46, "CLEAR", cCard2(), cText(), 2);
  addButton("settings", SAFE_X + 70, SAFE_BOTTOM - 54, SAFE_W - 140, 44, "BACK", cAccent(), cOnAccent(), 2);
}

void drawSettingsAbout() {
  clearButtons();
  gfx->fillScreen(cBg());
  drawHeader("About");
  drawCard(SAFE_X, SAFE_Y + 70, SAFE_W, 330, cCard(), cLine());
  String info;
  info += "FW: " + String(FIRMWARE_VERSION) + "\n";
  info += "WiFi: " + String(WiFi.status() == WL_CONNECTED ? WiFi.SSID() : "Offline") + "\n";
  info += "IP: " + getIpString() + "\n";
  info += "AI: " + currentAiName() + "\n";
  info += "STT: " + currentSttName() + "\n";
  int b = getBatteryPercent();
  info += "Batt: " + String(b >= 0 ? String(b) + "%" : "n/a") + "\n";
  info += "Heap: " + String(ESP.getFreeHeap() / 1024) + " KB\n";
  info += "PSRAM: " + String(ESP.getFreePsram() / 1024) + " KB\n";
  info += "Audio: " + String(audioReady ? "Ready" : "NotReady") + "\n";
  info += micStatsShort();
  wrapText(info, SAFE_X + 18, SAFE_Y + 92, SAFE_W - 36, cTextSoft(), 2, 18);
  addButton("settings", SAFE_X + 70, SAFE_BOTTOM - 54, SAFE_W - 140, 44, "BACK", cAccent(), cOnAccent(), 2);
}

void drawProgressOnly() {
  if (!gfx) return;

  if (uiScreen == UI_TRANSCRIBING) {
    drawSegmentedProgressCard(SAFE_X, SAFE_Y + 68, SAFE_W, 238, true);
    uiProgressDirty = false;
    return;
  }

  if (uiScreen == UI_THINKING) {
    drawSegmentedProgressCard(SAFE_X, SAFE_Y + 86, SAFE_W, 250, false);
    uiProgressDirty = false;
    return;
  }

  if (uiScreen == UI_CHAT) {
    int historyCount = (int)convHistory.size();
    int maxOffset = max(0, historyCount - 1);
    int offsetSnapshot = constrain(chatHistoryOffset, 0, maxOffset);
    bool viewingLatest = (offsetSnapshot == 0);

    bool inlineAI = viewingLatest && ((appState == STATE_STREAMING) ||
                    (workStage == WORK_AI_WIFI ||
                     workStage == WORK_AI_BUILD ||
                     workStage == WORK_AI_PRIMARY ||
                     workStage == WORK_AI_WAIT ||
                     workStage == WORK_AI_FALLBACK));
    bool inlineErr = viewingLatest && (workStage == WORK_ERROR && lastError.length());

    if (inlineAI || inlineErr) {
      bool inlineIsVoice = inlineErr && (activeSegProgressKind == SEG_PROGRESS_STT);
      int viewX = SAFE_X;
      int viewY = SAFE_Y + 58;
      int viewW = SAFE_W;
      int viewH = SAFE_H - 132;
      int cardX = viewX + 12;
      int cardY = viewY + 132;
      int cardW = viewW - 24;
      int cardH = viewH - 144;
      drawInlineWorkCard(cardX, cardY, cardW, cardH, inlineIsVoice);
      uiProgressDirty = false;
      return;
    }
  }

  // If the progress-only path is not valid for the current screen/state,
  // fall back safely to a full redraw rather than leaving stale pixels behind.
  uiProgressDirty = false;
  requestRedraw();
}


void drawCurrentScreen() {
  if (!gfx) return;
  switch (uiScreen) {
    case UI_HOME: drawHome(); break;
    case UI_CHAT: drawChat(); break;
    case UI_KEYBOARD: drawKeyboard(); break;
    case UI_SETTINGS_HOME: drawSettingsHome(); break;
    case UI_SETTINGS_VOICE: drawSettingsVoice(); break;
    case UI_SETTINGS_AI: drawSettingsAI(); break;
    case UI_SETTINGS_DISPLAY: drawSettingsDisplay(); break;
    case UI_SETTINGS_WIFI: drawSettingsWifi(); break;
    case UI_SETTINGS_DEBUG: drawSettingsDebug(); break;
    case UI_SETTINGS_ABOUT: drawSettingsAbout(); break;
    case UI_LISTENING: drawListening(); break;
    case UI_TRANSCRIBING: drawTranscribing(); break;
    case UI_THINKING: drawThinking(); break;
    case UI_ERROR_SCREEN: drawErrorScreen(); break;
    default: drawHome(); break;
  }
  uiDirty = false;
  uiProgressDirty = false;
}

bool prepareAudioBufferForSeconds(uint32_t seconds) {
  size_t samples = RECORD_SAMPLE_RATE * seconds;
  if (audioBuffer && audioBufferSamples == samples) return true;
  if (audioBuffer) {
    heap_caps_free(audioBuffer);
    audioBuffer = nullptr;
    audioBufferSamples = 0;
  }
  audioBuffer = (int16_t *)heap_caps_malloc(samples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!audioBuffer) return false;
  audioBufferSamples = samples;
  return true;
}

void startRecordingFlow() {
  markUserActivity();
  if (wifiSaverEnabled && !gBusy && !portalRunning) wifiSaverDisconnectNow("Recording - WiFi off");
  if (gBusy) {
    USBSerial.println("[MIC] TALK ignored because device is busy");
    liveStatus = "Please wait...";
    requestRedraw();
    return;
  }
  if (!audioReady) {
    lastError = "Audio not ready. Check USBSerial codec logs.";
    uiScreen = UI_ERROR_SCREEN;
    requestRedraw();
    return;
  }
  if (!prepareAudioBufferForSeconds(recordSeconds)) {
    lastError = "Audio buffer allocation failed.";
    uiScreen = UI_ERROR_SCREEN;
    requestRedraw();
    return;
  }

  cancelRequested = false;
  clearWorkNotice();
  resetWorkLog("Recording voice");
  resetSmoothProgress(SEG_PROGRESS_STT, 0);
  lastError = "";
  gBusy = true;
  isRecording = true;
  appState = STATE_RECORDING;
  uiScreen = UI_LISTENING;
  setWorkStage(WORK_RECORDING, "Listening...");
  xTaskCreatePinnedToCore(recordThenSttWorkerTask, "RECWorker", REC_TASK_STACK_BYTES, NULL, 1, NULL, 0);
}

void sendCurrentTextToAI(const String &text) {
  markUserActivity();
  cancelTranscriptWifiHold();
  String clean = text;
  clean.trim();
  if (gBusy) return;
  if (clean.isEmpty()) {
    liveStatus = "Nothing to send";
    uiScreen = UI_CHAT;
    requestRedraw();
    return;
  }

  cancelRequested = false;
  // Show the user's message immediately so the chat is never blank while AI works.
  addConversation(clean, "");
  setLatestChatViewLocked(clean, "", true);
  inputDraft = "";
  lastTranscript = "";

  gBusy = true;
  appState = STATE_STREAMING;
  lastError = "";
  // Keep the user in the chat and show the loading/status card inline.
  // Force one clean full redraw on this transition. Without this, the dirty
  // progress-only path can draw the AI loading card over the old transcript
  // preview/text after pressing SEND. After this first clean frame, progress
  // ticks go back to progress-only redraw for anti-flicker.
  uiScreen = UI_CHAT;
  requestRedraw();
  clearWorkNotice();
  lastError = "";
  resetWorkLog("AI reply loading");
  resetSmoothProgress(SEG_PROGRESS_AI, 40);
  setWorkStage(WORK_AI_BUILD, "Preparing " + currentAiName() + " mode...");
  String *heapText = new String(clean);
  xTaskCreatePinnedToCore(aiWorkerTask, "AIWorker", 16384, heapText, 1, NULL, 0);
}
void saveRuntimeSettings() {
  saveConfig();
}

void cycleSttMode() {
  currentSTT = (STTMode)(((int)currentSTT + 1) % 3);
  saveRuntimeSettings();
}

void cycleAiMode() {
  currentAI = (AIMode)(((int)currentAI + 1) % 3);
  saveRuntimeSettings();
}

void cycleTheme() {
  themeMode = (themeMode + 1) % THEME_COUNT;
  saveRuntimeSettings();
}

void handleButton(const String &id) {
  if (id == "speak") { startRecordingFlow(); return; }
  if (id == "chat") { uiScreen = UI_CHAT; requestRedraw(); return; }
  if (id == "cancel_work") { requestCancelWork(); return; }
  if (id == "type") {
    if (keyboardMode == KB_MODE_OFF || gBusy) return;
    keyboardDraft = inputDraft;
    keyboardPage = 0;
    lastScreen = uiScreen;
    uiScreen = UI_KEYBOARD;
    liveStatus = "Typing...";
    requestRedraw();
    return;
  }
  if (id == "settings") { uiScreen = UI_SETTINGS_HOME; requestRedraw(); return; }
  if (id == "back_home") { uiScreen = UI_HOME; requestRedraw(); return; }
  if (id == "send") { sendCurrentTextToAI(inputDraft); return; }

  if (id == "conv_prev") {
    if (chatHistoryOffset < chatHistoryMaxOffsetCached) {
      chatHistoryOffset++;
      chatScroll = 0;
      invalidateChatWrapCache();
    }
    requestRedraw();
    return;
  }
  if (id == "conv_next") {
    if (chatHistoryOffset > 0) {
      chatHistoryOffset--;
      chatScroll = 0;
      invalidateChatWrapCache();
    }
    requestRedraw();
    return;
  }

  if (id == "settings_voice") { uiScreen = UI_SETTINGS_VOICE; requestRedraw(); return; }
  if (id == "settings_ai") { uiScreen = UI_SETTINGS_AI; requestRedraw(); return; }
  if (id == "settings_display") { uiScreen = UI_SETTINGS_DISPLAY; requestRedraw(); return; }
  if (id == "settings_wifi") { uiScreen = UI_SETTINGS_WIFI; requestRedraw(); return; }
  if (id == "settings_debug") { uiScreen = UI_SETTINGS_DEBUG; requestRedraw(); return; }
  if (id == "settings_about") { uiScreen = UI_SETTINGS_ABOUT; requestRedraw(); return; }

  if (id == "voice_stt") { cycleSttMode(); requestRedraw(); return; }
  if (id == "rec_minus") { if (recordSeconds > 1) recordSeconds--; saveRuntimeSettings(); requestRedraw(); return; }
  if (id == "rec_plus") { if (recordSeconds < 6) recordSeconds++; saveRuntimeSettings(); requestRedraw(); return; }
  if (id == "stt_fb") { sttFallbackEnabled = !sttFallbackEnabled; saveRuntimeSettings(); requestRedraw(); return; }
  if (id == "autosend") { autoSendAfterSTT = !autoSendAfterSTT; saveRuntimeSettings(); requestRedraw(); return; }

  if (id == "ai_mode") { cycleAiMode(); requestRedraw(); return; }
  if (id == "ai_fb") { aiFallbackEnabled = !aiFallbackEnabled; saveRuntimeSettings(); requestRedraw(); return; }
  if (id == "tokens_minus") { maxOutputTokens = max(300, maxOutputTokens - 100); saveRuntimeSettings(); requestRedraw(); return; }
  if (id == "tokens_plus") { maxOutputTokens = min(1600, maxOutputTokens + 100); saveRuntimeSettings(); requestRedraw(); return; }

  if (id == "bright_minus") { brightnessValue = max(20, brightnessValue - 20); applyBrightness(brightnessValue); requestRedraw(); return; }
  if (id == "bright_plus") { brightnessValue = min(255, brightnessValue + 20); applyBrightness(brightnessValue); requestRedraw(); return; }
  if (id == "theme") { cycleTheme(); requestRedraw(); return; }
  if (id == "font") { fontSizeMode = (fontSizeMode + 1) % 3; saveRuntimeSettings(); requestRedraw(); return; }
  if (id == "sleep_timeout") { cycleSleepTimeout(); requestRedraw(); return; }

  if (id == "wifi_saver") {
    wifiSaverEnabled = !wifiSaverEnabled;
    saveRuntimeSettings();
    if (wifiSaverEnabled) wifiSaverDisconnectNow("WiFi saver on");
    requestRedraw();
    return;
  }

  if (id == "wifi_test") {
    liveStatus = "Testing WiFi...";
    requestRedraw();
    bool ok = ensureWiFiConnection();
    liveStatus = ok ? "WiFi OK" : "WiFi failed";
    requestRedraw();
    return;
  }
  if (id == "portal") {
    bool ok = startConfigPortal();
    uiScreen = UI_HOME;
    liveStatus = ok ? "Portal: 192.168.4.1" : "Portal failed";
    requestRedraw();
    return;
  }

  if (id == "dbg_logs") { debugLogsEnabled = !debugLogsEnabled; saveRuntimeSettings(); requestRedraw(); return; }
  if (id == "mic_dbg_toggle") { micDebugEnabled = !micDebugEnabled; saveRuntimeSettings(); requestRedraw(); return; }
  if (id == "mic_test") {
    uiScreen = UI_LISTENING;
    liveStatus = "Mic test...";
    requestRedraw();
    bool ok = runMicDebugTest(2500);
    lastError = ok ? "Mic looks alive. " + micStatsShort() : "Mic weak/no signal. " + micStatsShort();
    uiScreen = ok ? UI_SETTINGS_DEBUG : UI_ERROR_SCREEN;
    requestRedraw();
    return;
  }
  if (id == "clear_chat") { convHistory.clear(); inputDraft = ""; setLatestChatViewLocked("", "", false); liveStatus = "Chat cleared"; requestRedraw(); return; }

  if (id.startsWith("key:")) {
    String k = id.substring(4);
    if (k == "PAGE") {
      keyboardPage = 1 - keyboardPage;
    } else if (k == "SPACE") {
      keyboardDraft += " ";
    } else if (k == "DEL") {
      if (keyboardDraft.length() > 0) keyboardDraft.remove(keyboardDraft.length() - 1);
    } else if (k == "DONE") {
      inputDraft = keyboardDraft;
      lastTranscript = inputDraft;
      chatScroll = 0; chatHistoryOffset = 0; invalidateChatWrapCache();
      if (inputDraft.length()) {
        setLatestChatViewLocked(inputDraft, "Press SEND to ask AI, or TYPE to edit.", false);
      }
      uiScreen = UI_CHAT;
      liveStatus = inputDraft.length() ? "Draft ready" : "Draft empty";
      requestRedraw();
      return;
    } else {
      keyboardDraft += k;
      USBSerial.println("[KEYBOARD] typed: " + k + " draft=" + screenClean(keyboardDraft).substring(0, 80));
    }
    liveStatus = "Typing " + String(keyboardDraft.length()) + " chars";
    requestRedraw();
    return;
  }
}


void handleKeyboardFallbackTouch(int x, int y) {
  if (uiScreen != UI_KEYBOARD) return;

  // Coordinate fallback for the custom keyboard. If a button hit misses due
  // to small touch calibration differences, this still types the key.
  int keyY = SAFE_Y + 194;
  int keyH = 40;
  int gap = 4;
  const char *rowsABC[] = {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};
  const char *rowsNUM[] = {"1234567890", "@#&-_.,?!", "+*/=:;()"};
  const char **rows = keyboardPage == 0 ? rowsABC : rowsNUM;

  for (int r = 0; r < 3; r++) {
    String row = rows[r];
    int count = row.length();
    int x0 = SAFE_X + (r == 1 ? 10 : (r == 2 ? 28 : 0));
    int usableW = SAFE_W - (r == 1 ? 20 : (r == 2 ? 56 : 0));
    int keyW = (usableW - (count - 1) * gap) / count;
    int y0 = keyY + r * (keyH + 8);
    if (y >= y0 - 10 && y <= y0 + keyH + 10 && x >= x0 - 10 && x <= x0 + usableW + 10) {
      int idx = (x - x0) / (keyW + gap);
      if (idx >= 0 && idx < count) {
        keyboardDraft += row.substring(idx, idx + 1);
        liveStatus = "Typing " + String(keyboardDraft.length()) + " chars";
        USBSerial.println("[KEYBOARD] fallback typed: " + row.substring(idx, idx + 1));
        requestRedraw();
        return;
      }
    }
  }
}

void handleTouch() {
  int x, y;
  bool down = readTouchPoint(x, y);

  if (screenSleeping) {
    // Screen sleep now wakes ONLY from the physical BOOT button.
    // Touch is ignored while sleeping so accidental taps do not wake the watch.
    touchHeld = false;
    return;
  }

  if (down) markUserActivity();

  // Release: end any active chat drag. Normal button debounce can reset too.
  if (!down) {
    if (chatTouchScrollActive) {
      endChatTouchScroll();
      lastTouchMs = millis();
    }
    touchHeld = false;
    return;
  }

  // If a chat drag already started, keep tracking it even while the finger is held.
  if (chatTouchScrollActive) {
    updateChatTouchScroll(x, y);
    touchHeld = true;
    return;
  }

  // Start drag-scroll when the finger begins inside the current chat text area.
  // This consumes the touch so it does not accidentally press SEND/HOME/etc.
  if (!touchHeld && pointInChatScrollArea(x, y)) {
    beginChatTouchScroll(x, y);
    touchHeld = true;
    lastTouchMs = millis();
    USBSerial.printf("[TOUCH_SCROLL] start x=%d y=%d scroll=%d max=%d\n", x, y, chatScroll, chatMaxScrollCached);
    return;
  }

  // Trigger only on a new press, not while the same finger is still held down.
  // This fixes: TYPE opening keyboard and immediately going back, accidental SEND,
  // and repeated key presses from one physical tap.
  if (touchHeld) return;
  if (millis() - lastTouchMs < TOUCH_DEBOUNCE_MS) return;

  touchHeld = true;
  lastTouchMs = millis();

  USBSerial.printf("[TOUCH] x=%d y=%d screen=%d buttons=%u\n", x, y, (int)uiScreen, (unsigned)uiButtons.size());

  for (int i = (int)uiButtons.size() - 1; i >= 0; i--) {
    if (hitRect(uiButtons[i], x, y)) {
      USBSerial.println("[TOUCH] hit: " + uiButtons[i].id);
      handleButton(uiButtons[i].id);
      return;
    }
  }

  USBSerial.println("[TOUCH] no hit");
  handleKeyboardFallbackTouch(x, y);
}

// ===================== SETUP =====================
void setup() {
  Serial.begin(115200);
  USBSerial.begin(115200);
  delay(1500);

  USBSerial.println("\n=== ESP32-S3 Touch AMOLED AI Buddy No-LVGL ===");
  pinMode(BOOT_BTN, INPUT_PULLUP);
  markUserActivity();

  loadConfig();

  if (!initDisplayAndTouch()) {
    USBSerial.println("Display/touch init failed");
    while (1) delay(100);
  }

  ui_mutex = xSemaphoreCreateMutex();
  drawCurrentScreen();

  pmuReady = initPMU();
  USBSerial.printf("PMU init: %s\n", pmuReady ? "OK" : "FAILED");
  applyBrightness(brightnessValue);
  if (wifiSaverEnabled) disconnectWiFi();

  size_t samples = RECORD_SAMPLE_RATE * recordSeconds;
  logMem("before initial audioBuffer alloc");
  audioBuffer = (int16_t *)heap_caps_malloc(
      samples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (audioBuffer) audioBufferSamples = samples;
  logMem("after initial audioBuffer alloc");

  audioReady = initAudioInput();
  liveStatus = audioReady ? "Ready" : "Audio init failed";
  uiScreen = UI_HOME;
  requestRedraw();
}

// ===================== LOOP =====================
void loop() {
  handleBootButtonSleepWake();

  if (portalRunning) {
    dnsServer.processNextRequest();
    server.handleClient();

    if (portalSaved) {
      stopConfigPortal();
      loadConfig();
      liveStatus = "Config saved";
      requestRedraw();
    } else if (millis() - portalStartMs > PORTAL_TIMEOUT_MS) {
      stopConfigPortal();
      liveStatus = "Portal timeout";
      requestRedraw();
    }
  }

  handleTouch();

  if (!screenSleeping) checkAutoScreenSleep();
  maybeAutoDisconnectWiFi();

  // Clear the temporary WiFi ERR header state after a few seconds.
  if (wifiErrorUntilMs && (long)(millis() - wifiErrorUntilMs) >= 0) {
    wifiErrorUntilMs = 0;
    if (!screenSleeping) requestRedraw();
  }

  if (screenSleeping) {
    delay(30);
    return;
  }

  // Lightweight animation/status refresh. Recording needs faster refresh for voice bars;
  // network states use slower redraws to avoid wasting cycles during HTTPS work.
  // Progress ticking happens here only. Draw functions render the current state;
  // they do not advance animation state. This prevents double-advance/jumpy loaders.
  if (appState == STATE_RECORDING || appState == STATE_TRANSCRIBING || appState == STATE_STREAMING) {
    unsigned long interval = (appState == STATE_RECORDING) ? 140 : 260;
    if (millis() - lastAnimMs > interval) {
      lastAnimMs = millis();
      animStep++;
      tickSmoothProgressAnimation();
      if (appState == STATE_RECORDING) requestRedraw();
      else requestProgressRedraw();
    }
  }

  if ((uiDirty || uiProgressDirty) && xSemaphoreTake(ui_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
    if (uiDirty) {
      drawCurrentScreen();
      uiProgressDirty = false;
    } else if (uiProgressDirty) {
      drawProgressOnly();
    }
    xSemaphoreGive(ui_mutex);
  }

  static unsigned long lastDiag = 0;
  if (millis() - lastDiag > 3000) {
    int batt = getBatteryPercent();
    USBSerial.printf("WiFi:%s | IP:%s | Batt:%d | PMU:%s | Heap:%u | PSRAM:%u | %s\n",
                  WiFi.status() == WL_CONNECTED ? "ON" : "OFF",
                  getIpString().c_str(),
                  batt,
                  pmuReady ? "OK" : "FAIL",
                  ESP.getFreeHeap() / 1024,
                  ESP.getFreePsram() / 1024,
                  micStatsShort().c_str());
    lastDiag = millis();
  }

  delay(10);
}
