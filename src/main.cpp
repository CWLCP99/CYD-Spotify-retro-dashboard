// Mini Spotify display for the ESP32-2432S028R ("Cheap Yellow Display")
// Shows the current song + cover, progress bar, and touch play/pause/skip.

#include <Arduino.h>
#include <ESPmDNS.h>
#include <SPI.h>
#include <WebServer.h>
#include <WiFi.h>
#include <XPT2046_Touchscreen.h>

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

enum class Screen { None, Login, Idle, Player, Message };
Screen screen = Screen::None;

NowPlaying np;              // last known playback state
uint32_t npFetchedAt = 0;   // millis() when np was fetched (for smooth progress)
uint32_t nextPollAt = 0;
uint32_t lastProgressDraw = 0;
uint32_t toastUntil = 0;
String shownTrackId, shownArtUrl;

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
  String code;
  if (url.indexOf("error=") >= 0) {
    server.send(400, "text/html",
                page("<h1>Login cancelled</h1><p>" + htmlEscape(url) +
                     "</p><p><a class='btn' href='/'>Try again</a></p>"));
    return;
  }
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
    nextPollAt = millis();
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

static uint32_t currentProgress() {
  uint32_t p = np.progressMs;
  if (np.isPlaying) p += millis() - npFetchedAt;
  return min(p, np.durationMs);
}

static void renderPlayer() {
  if (screen != Screen::Player) {
    uiClear();
    screen = Screen::Player;
    shownTrackId = "";
    shownArtUrl = "";
  }
  if (np.trackId != shownTrackId) {
    uiDrawTrackText(np);
    shownTrackId = np.trackId;
  }
  if (np.imageUrl != shownArtUrl) {
    uiDrawArt(np.imageUrl);
    shownArtUrl = np.imageUrl;
  }
  if (!toastUntil) uiDrawControls(np.isPlaying);
  uiDrawProgress(currentProgress(), np.durationMs);
}

static void poll() {
  NowPlaying fresh;
  PollResult r = spotify.getNowPlaying(fresh);
  uint32_t now = millis();
  uint32_t interval = POLL_IDLE_MS;

  switch (r) {
    case PollResult::Ok: {
      np = fresh;
      npFetchedAt = now;
      renderPlayer();
      interval = np.isPlaying ? POLL_PLAYING_MS : POLL_PAUSED_MS;
      // song about to end -> check again right after it finishes
      if (np.isPlaying && np.durationMs > np.progressMs) {
        interval = min<uint32_t>(interval, np.durationMs - np.progressMs + 800);
      }
      break;
    }
    case PollResult::NothingPlaying:
      if (screen != Screen::Idle) {
        uiMessage("Nothing playing", "Start Spotify on any device", "", "");
        screen = Screen::Idle;
      }
      break;
    case PollResult::NeedLogin:
      showLoginScreen();
      break;
    case PollResult::RateLimited:
      interval = spotify.retryAfterMs();
      break;
    case PollResult::NetError:
      interval = 3000;  // keep showing the last state, retry soon
      break;
  }
  nextPollAt = millis() + interval;
}

// ---------------------------------------------------------------------------
// Touch
// ---------------------------------------------------------------------------
static void showCommandResult(int status) {
  if (status >= 200 && status < 300) return;
  if (status == 404) uiToast("No active device");
  else if (status == 403) uiToast("Not allowed (Premium needed?)");
  else if (status == 429) uiToast("Too many requests, wait");
  else uiToast("Error " + String(status));
  toastUntil = millis() + 2000;
}

static void handleTouch() {
  if (!ts.tirqTouched() || !ts.touched()) return;
  TS_Point p = ts.getPoint();
  int x = constrain(map(p.x, TOUCH_X_MIN, TOUCH_X_MAX, 0, 320), 0, 319);
  int y = constrain(map(p.y, TOUCH_Y_MIN, TOUCH_Y_MAX, 0, 240), 0, 239);
  Serial.printf("touch raw=(%d,%d) z=%d -> (%d,%d)\n", p.x, p.y, p.z, x, y);

  // wait for release so one tap = one action
  uint32_t t0 = millis();
  while (ts.touched() && millis() - t0 < 1500) delay(10);

  if (screen != Screen::Player) return;

  int status = 0;
  switch (uiHitTest(x, y)) {
    case Button::PlayPause: {
      status = spotify.command("PUT", np.isPlaying ? "/me/player/pause" : "/me/player/play");
      if (status >= 200 && status < 300) {
        np.progressMs = currentProgress();
        npFetchedAt = millis();
        np.isPlaying = !np.isPlaying;  // optimistic update, confirmed by next poll
        uiDrawControls(np.isPlaying);
      }
      break;
    }
    case Button::Next:
      status = spotify.command("POST", "/me/player/next");
      break;
    case Button::Prev:
      status = spotify.command("POST", "/me/player/previous");
      break;
    case Button::None:
      return;
  }
  showCommandResult(status);
  nextPollAt = millis() + 700;  // give Spotify a moment, then refresh
}

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  uiBegin();

  touchSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  ts.begin(touchSPI);
  ts.setRotation(1);

  uiMessage("Connecting to WiFi", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    if (millis() - t0 > 20000) {
      uiMessage("WiFi failed", "Check WIFI_SSID / WIFI_PASSWORD", "in secrets.h", "Retrying...");
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
  if (!spotify.hasRefreshToken()) showLoginScreen();
  else uiMessage("Spotify", "Loading...");
}

void loop() {
  server.handleClient();
  uint32_t now = millis();

  if (WiFi.status() != WL_CONNECTED) {
    if (screen != Screen::Message) {
      uiMessage("WiFi lost", "Reconnecting...");
      screen = Screen::Message;
    }
    delay(200);
    return;
  }

  if (!spotify.hasRefreshToken()) {
    if (screen != Screen::Login) showLoginScreen();
    delay(5);
    return;
  }

  if (screen == Screen::Message) screen = Screen::None;

  handleTouch();

  if ((int32_t)(now - nextPollAt) >= 0) poll();

  if (toastUntil && (int32_t)(millis() - toastUntil) >= 0) {
    toastUntil = 0;
    if (screen == Screen::Player) uiDrawControls(np.isPlaying);
  }

  // move the progress bar smoothly between polls
  if (screen == Screen::Player && millis() - lastProgressDraw >= 500) {
    uiDrawProgress(currentProgress(), np.durationMs);
    lastProgressDraw = millis();
  }
  delay(5);
}
