// Mini Spotify display for the ESP32-2432S028R ("Cheap Yellow Display")
//
// Two things run in parallel on the ESP32's two cores:
//   core 0: netTask()  - talks to Spotify (polling, play/pause/skip, cover download)
//   core 1: loop()     - screen, touch, animations, login web page
// so the screen never freezes while waiting for the internet.

#include <Arduino.h>
#include <ESPmDNS.h>
#include <SPI.h>
#include <WebServer.h>
#include <WiFi.h>
#include <XPT2046_Touchscreen.h>

#include "config.h"
#include "secrets.h"
#include "spotify.h"
#include "ui.h"

// ---------- touch (the CYD's touch controller has its own SPI bus) ----------
#define XPT2046_IRQ 36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK 25
#define XPT2046_CS 33
// Raw -> pixel calibration. If taps land in the wrong place, open the serial
// monitor, tap the corners and adjust these numbers.
#define TOUCH_X_MIN 200
#define TOUCH_X_MAX 3700
#define TOUCH_Y_MIN 240
#define TOUCH_Y_MAX 3800

static const char* HOSTNAME = "spotify-display";  // -> http://spotify-display.local

// ---------- timing ----------
static const uint32_t POLL_PLAYING_MS = 3000;
static const uint32_t POLL_PAUSED_MS = 5000;
static const uint32_t POLL_IDLE_MS = 10000;

SPIClass touchSPI(VSPI);
XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);
WebServer server(80);
SpotifyClient spotify;

// ---------------------------------------------------------------------------
// Data shared between the network task (core 0) and the UI loop (core 1)
// ---------------------------------------------------------------------------
enum class Cmd : uint8_t { PlayPause, Play, Pause, Next, Prev };

struct Shared {
  SemaphoreHandle_t mtx;
  uint32_t version = 0;  // +1 after every poll
  PollResult result = PollResult::NetError;
  NowPlaying np;
  uint32_t fetchedAt = 0;
  uint32_t artVersion = 0;  // +1 when a new cover was downloaded
  uint8_t* art = nullptr;   // JPEG bytes (nullptr = no cover)
  size_t artLen = 0;
  uint32_t cmdSeq = 0;      // +1 after every finished command
  int cmdStatus = 0;
} shared;

QueueHandle_t cmdQueue;
volatile bool pollNow = false;

struct SharedLock {
  SharedLock() { xSemaphoreTake(shared.mtx, portMAX_DELAY); }
  ~SharedLock() { xSemaphoreGive(shared.mtx); }
};

static int runCommand(Cmd c) {
  switch (c) {
    case Cmd::Play: return spotify.command("PUT", "/me/player/play");
    case Cmd::Pause: return spotify.command("PUT", "/me/player/pause");
    case Cmd::Next: return spotify.command("POST", "/me/player/next");
    case Cmd::Prev: return spotify.command("POST", "/me/player/previous");
    default: return 0;
  }
}

void netTask(void*) {
  uint32_t nextPollAt = 0;
  String lastArtUrl;
  for (;;) {
    int32_t wait = (int32_t)(nextPollAt - millis());
    wait = constrain(wait, 0, 200);  // wake up regularly to check pollNow
    Cmd cmd;
    if (xQueueReceive(cmdQueue, &cmd, pdMS_TO_TICKS(wait)) == pdTRUE) {
      int status = runCommand(cmd);
      {
        SharedLock l;
        shared.cmdStatus = status;
        shared.cmdSeq++;
      }
      nextPollAt = millis() + 700;  // give Spotify a moment, then refresh
      continue;
    }
    if (pollNow) {
      pollNow = false;
      nextPollAt = millis();
      lastArtUrl = "";
    }
    if ((int32_t)(millis() - nextPollAt) < 0) continue;
    if (WiFi.status() != WL_CONNECTED || !spotify.hasRefreshToken()) {
      nextPollAt = millis() + 500;
      continue;
    }

    NowPlaying np;
    PollResult r = spotify.getNowPlaying(np);
    uint32_t now = millis();
    {
      SharedLock l;
      shared.result = r;
      if (r == PollResult::Ok) {
        shared.np = np;
        shared.fetchedAt = now;
      }
      shared.version++;
    }

    uint32_t interval = POLL_IDLE_MS;
    if (r == PollResult::Ok) {
      interval = np.isPlaying ? POLL_PLAYING_MS : POLL_PAUSED_MS;
      // song about to end -> check again right after it finishes
      if (np.isPlaying && np.durationMs > np.progressMs)
        interval = min<uint32_t>(interval, np.durationMs - np.progressMs + 800);
    } else if (r == PollResult::RateLimited) {
      interval = spotify.retryAfterMs();
    } else if (r == PollResult::NetError) {
      interval = 3000;
    }
    nextPollAt = millis() + interval;

    // album cover (only if the theme shows one)
    if (r == PollResult::Ok && uiWantsArt() && np.imageUrl != lastArtUrl) {
      lastArtUrl = np.imageUrl;
      size_t len = 0;
      uint8_t* buf = np.imageUrl.length() ? httpDownload(np.imageUrl, len, 120 * 1024) : nullptr;
      SharedLock l;
      free(shared.art);
      shared.art = buf;
      shared.artLen = len;
      shared.artVersion++;
    }
  }
}

// ---------------------------------------------------------------------------
// UI state (core 1 only)
// ---------------------------------------------------------------------------
enum class Screen { None, Login, Idle, Player, Message };
Screen screen = Screen::None;

NowPlaying np;             // what the screen shows
uint32_t npFetchedAt = 0;  // millis() when np was fetched (for smooth progress)
uint32_t seenVersion = 0, seenCmdSeq = 0, versionFloor = 0, drawnArtVersion = UINT32_MAX;
bool awaitingCmd = false;
uint32_t lastProgressDraw = 0;
uint32_t toastUntil = 0;
String shownTrackId;
int shownPlaying = -1;  // play/pause state currently drawn (-1 = needs redraw)

static void drawControls() {
  uiDrawControls(np.isPlaying);
  shownPlaying = np.isPlaying;
}

static uint32_t currentProgress() {
  uint32_t p = np.progressMs;
  if (np.isPlaying) p += millis() - npFetchedAt;
  return min(p, np.durationMs);
}

// ---------------------------------------------------------------------------
// Web page for logging in (open http://<display-ip>/ on your phone or PC)
// ---------------------------------------------------------------------------
static String htmlEscape(const String& s) {
  String o;
  for (char c : s) {
    if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '&') o += "&amp;";
    else if (c == '"') o += "&quot;";
    else o += c;
  }
  return o;
}

static String page(const String& body) {
  return "<!doctype html><html><head><meta charset='utf-8'>"
         "<meta name='viewport' content='width=device-width,initial-scale=1'>"
         "<title>Spotify Display</title><style>"
         "body{font-family:system-ui,sans-serif;background:#121212;color:#eee;max-width:560px;"
         "margin:auto;padding:20px;line-height:1.5}h1{color:#1ed760}"
         "a.btn,button{display:inline-block;background:#1ed760;color:#000;padding:10px 18px;"
         "border-radius:24px;border:0;font-weight:600;text-decoration:none;font-size:15px}"
         "textarea{width:100%;height:90px;background:#222;color:#eee;border:1px solid #444;"
         "border-radius:8px;padding:8px;box-sizing:border-box}"
         "code{background:#222;padding:2px 5px;border-radius:4px}.muted{color:#999}"
         "</style></head><body>" +
         body + "</body></html>";
}

static void handleRoot() {
  String b = "<h1>Spotify Display</h1>";
  if (spotify.hasRefreshToken()) {
    b += "<p>&#9989; Logged in. The display is showing what you play.</p>"
         "<form method='post' action='/logout'><button>Log out</button></form>";
  } else {
    b += "<p><b>Step 1.</b> Log in with Spotify:</p>"
         "<p><a class='btn' target='_blank' href='" + htmlEscape(spotify.authorizeUrl()) +
         "'>Log in with Spotify</a></p>"
         "<p><b>Step 2.</b> After you click <i>Agree</i>, the browser opens "
         "<code>" SPOTIFY_REDIRECT_URI "?code=...</code> and shows an error like "
         "<i>&quot;This site can&#39;t be reached&quot;</i>. That's expected. "
         "Copy the <b>whole address</b> from the address bar and paste it here:</p>"
         "<form method='post' action='/code'><textarea name='url' "
         "placeholder='http://127.0.0.1:8888/callback?code=...'></textarea>"
         "<p><button>Connect</button></p></form>"
         "<p class='muted'>The code is only valid for a few minutes, so paste it right away.</p>";
  }
  server.send(200, "text/html", page(b));
}

static void handleCode() {
  String url = server.arg("url");
  url.trim();
  if (url.indexOf("error=") >= 0) {
    server.send(400, "text/html",
                page("<h1>Login cancelled</h1><p>" + htmlEscape(url) +
                     "</p><p><a class='btn' href='/'>Try again</a></p>"));
    return;
  }
  String code;
  int i = url.indexOf("code=");
  if (i >= 0) {
    int end = url.indexOf('&', i);
    code = url.substring(i + 5, end < 0 ? url.length() : end);
  } else {
    code = url;  // user pasted only the code
  }

  String err;
  if (code.length() && spotify.exchangeCode(code, err)) {
    server.send(200, "text/html",
                page("<h1>Connected &#127881;</h1><p>Look at the display. "
                     "Play something on Spotify and it will show up.</p>"));
    screen = Screen::None;  // force a redraw
    pollNow = true;
  } else {
    server.send(400, "text/html",
                page("<h1>That didn't work</h1><p class='muted'>" + htmlEscape(err) +
                     "</p><p>Check that the Redirect URI in your Spotify app is exactly "
                     "<code>" SPOTIFY_REDIRECT_URI "</code>, then try again.</p>"
                     "<p><a class='btn' href='/'>Try again</a></p>"));
  }
}

static void handleLogout() {
  spotify.logout();
  screen = Screen::None;
  server.sendHeader("Location", "/");
  server.send(303);
}

// ---------------------------------------------------------------------------
// Screen updates
// ---------------------------------------------------------------------------
static void showLoginScreen() {
  uiMessage("Log in to Spotify", "On your phone or PC, open:",
            "http://" + WiFi.localIP().toString(), String("or http://") + HOSTNAME + ".local");
  screen = Screen::Login;
}

static void drawArtFromShared() {
  SharedLock l;
  uiDrawArtJpeg(shared.art, shared.artLen);
  drawnArtVersion = shared.artVersion;
}

static void renderPlayer() {
  if (screen != Screen::Player) {
    uiBeginPlayer();
    screen = Screen::Player;
    shownTrackId = "";
    shownPlaying = -1;
    drawnArtVersion = UINT32_MAX;  // redraw cover
  }
  if (np.trackId != shownTrackId) {
    uiDrawTrackText(np);
    shownTrackId = np.trackId;
  }
  if (!toastUntil) {
    if (shownPlaying != (int)np.isPlaying) drawControls();
    uiDrawProgress(currentProgress(), np.durationMs);
  }
}

// Pick up new results from the network task
static void syncFromNetwork() {
  bool gotPoll = false, gotCmd = false;
  PollResult result;
  NowPlaying fresh;
  uint32_t fetchedAt = 0;
  int cmdStatus = 0;
  {
    SharedLock l;
    if (shared.cmdSeq != seenCmdSeq) {
      seenCmdSeq = shared.cmdSeq;
      cmdStatus = shared.cmdStatus;
      gotCmd = true;
      awaitingCmd = false;
      versionFloor = shared.version;  // ignore polls that started before the command finished
    }
    if (!awaitingCmd && shared.version != seenVersion && shared.version > versionFloor) {
      seenVersion = shared.version;
      result = shared.result;
      fresh = shared.np;
      fetchedAt = shared.fetchedAt;
      gotPoll = true;
    }
  }

  if (gotCmd && !(cmdStatus >= 200 && cmdStatus < 300)) {
    if (cmdStatus == 404) uiToast("No active device");
    else if (cmdStatus == 403) uiToast("Not allowed (Premium?)");
    else if (cmdStatus == 429) uiToast("Too many requests");
    else uiToast("Error " + String(cmdStatus));
    toastUntil = millis() + 2000;
  }

  if (gotPoll) {
    switch (result) {
      case PollResult::Ok:
        np = fresh;
        npFetchedAt = fetchedAt;
        renderPlayer();
        break;
      case PollResult::NothingPlaying:
        if (screen != Screen::Idle) {
          uiMessage("Nothing playing", "Start Spotify on any device");
          screen = Screen::Idle;
        }
        break;
      case PollResult::NeedLogin:
        showLoginScreen();
        break;
      default:
        break;  // network hiccup / rate limit: keep showing the last state
    }
  }

  if (screen == Screen::Player && uiWantsArt()) {
    uint32_t av;
    {
      SharedLock l;
      av = shared.artVersion;
    }
    if (av != drawnArtVersion) drawArtFromShared();
  }
}

// ---------------------------------------------------------------------------
// Touch
// ---------------------------------------------------------------------------
static Button readTap() {
  if (!ts.tirqTouched() || !ts.touched()) return Button::None;
  TS_Point p = ts.getPoint();
  int x = constrain(map(p.x, TOUCH_X_MIN, TOUCH_X_MAX, 0, 320), 0, 319);
  int y = constrain(map(p.y, TOUCH_Y_MIN, TOUCH_Y_MAX, 0, 240), 0, 239);
  Serial.printf("touch raw=(%d,%d) z=%d -> (%d,%d)\n", p.x, p.y, p.z, x, y);
  if (screen != Screen::Player) return Button::None;

  Button b = uiHitTest(x, y);
  if (b != Button::None) uiPressFeedback(b);
  // wait for release so one tap = one action (keep animating meanwhile)
  uint32_t t0 = millis();
  while (ts.touched() && millis() - t0 < 1500) {
    uiAnimate(np.isPlaying, currentProgress(), np.durationMs);
    delay(10);
  }
  return b;
}

static void handleTouch() {
  Button b = readTap();
  if (b == Button::None) return;

  Cmd cmd;
  if (b == Button::PlayPause) {
    cmd = np.isPlaying ? Cmd::Pause : Cmd::Play;
    // optimistic update: show the new state right away
    np.progressMs = currentProgress();
    npFetchedAt = millis();
    np.isPlaying = !np.isPlaying;
  } else {
    cmd = (b == Button::Next) ? Cmd::Next : Cmd::Prev;
  }
  drawControls();
  awaitingCmd = xQueueSend(cmdQueue, &cmd, 0) == pdTRUE;
}

// ---------------------------------------------------------------------------
// Demo mode: fake songs, no WiFi needed (set UI_DEMO 1 in include/config.h)
// ---------------------------------------------------------------------------
#if UI_DEMO
static const char* DEMO[][3] = {
    {"Bohemian Rhapsody", "Queen", "A Night at the Opera"},
    {"Tití Me Preguntó", "Bad Bunny", "Un Verano Sin Ti"},
    {"A Really Long Song Title That Will Not Fit", "Some Artist With A Long Name",
     "And An Even Longer Album Name (Deluxe Edition)"},
};
static int demoIdx = 0;

static void demoLoad(int i) {
  demoIdx = (i + 3) % 3;
  np.title = DEMO[demoIdx][0];
  np.artist = DEMO[demoIdx][1];
  np.album = DEMO[demoIdx][2];
  np.trackId = String(demoIdx);
  np.durationMs = 95000 + demoIdx * 60000;
  np.progressMs = 0;
  np.isPlaying = true;
  npFetchedAt = millis();
  renderPlayer();
  uiDrawArtJpeg(nullptr, 0);  // placeholder cover
}

static void demoLoop() {
  Button b = readTap();
  if (b == Button::PlayPause) {
    np.progressMs = currentProgress();
    npFetchedAt = millis();
    np.isPlaying = !np.isPlaying;
    drawControls();
  } else if (b == Button::Next) {
    demoLoad(demoIdx + 1);
    drawControls();
  } else if (b == Button::Prev) {
    demoLoad(demoIdx - 1);
    drawControls();
  }
  if (np.isPlaying && currentProgress() >= np.durationMs) demoLoad(demoIdx + 1);
}
#endif

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  uiBegin();

  touchSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  ts.begin(touchSPI);
  ts.setRotation(1);

#if UI_DEMO
  demoLoad(0);
  return;
#endif

  uiMessage("Connecting to WiFi", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    if (millis() - t0 > 20000) {
      uiMessage("WiFi failed", "Check WIFI_SSID / WIFI_PASSWORD in secrets.h", "Retrying...");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      t0 = millis();
    }
  }
  Serial.printf("WiFi OK, IP %s\n", WiFi.localIP().toString().c_str());

  MDNS.begin(HOSTNAME);
  server.on("/", HTTP_GET, handleRoot);
  server.on("/code", HTTP_POST, handleCode);
  server.on("/logout", HTTP_POST, handleLogout);
  server.begin();
  MDNS.addService("http", "tcp", 80);

  spotify.begin();
  shared.mtx = xSemaphoreCreateMutex();
  cmdQueue = xQueueCreate(4, sizeof(Cmd));
  // 16 KB stack: TLS needs a lot. Core 0 = the "network" core.
  xTaskCreatePinnedToCore(netTask, "net", 16384, nullptr, 1, nullptr, 0);

  if (!spotify.hasRefreshToken()) showLoginScreen();
  else uiMessage("Spotify", "Loading...");
}

void loop() {
#if UI_DEMO
  demoLoop();
#else
  server.handleClient();

  if (WiFi.status() != WL_CONNECTED) {
    if (screen != Screen::Message) {
      uiMessage("WiFi lost", "Reconnecting...");
      screen = Screen::Message;
    }
    delay(200);
    return;
  }
  if (screen == Screen::Message) {
    screen = Screen::None;
    pollNow = true;
  }

  if (!spotify.hasRefreshToken()) {
    if (screen != Screen::Login) showLoginScreen();
    delay(5);
    return;
  }

  handleTouch();
  syncFromNetwork();
#endif

  if (toastUntil && (int32_t)(millis() - toastUntil) >= 0) {
    toastUntil = 0;
    if (screen == Screen::Player) {
      drawControls();
      uiDrawProgress(currentProgress(), np.durationMs);
    }
  }

  if (screen == Screen::Player) {
    // move the progress smoothly between polls
    if (!toastUntil && millis() - lastProgressDraw >= 500) {
      uiDrawProgress(currentProgress(), np.durationMs);
      lastProgressDraw = millis();
    }
    uiAnimate(np.isPlaying, currentProgress(), np.durationMs);
  }
  delay(5);
}
