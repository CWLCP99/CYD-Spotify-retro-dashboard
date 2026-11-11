#include "spotify.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFiClientSecure.h>
#include <base64.h>

#include "secrets.h"

// read = see what's playing, modify = play/pause/skip (modify needs Premium which I have)
static const char* SCOPES =
    "user-read-currently-playing user-read-playback-state user-modify-playback-state";
static const char* API = "https://api.spotify.com/v1";

static String urlEncode(const String& s) {
  static const char* hex = "0123456789ABCDEF";
  String out;
  out.reserve(s.length() * 3);
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = s[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += (char)c;
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 0x0F];
    }
  }
  return out;
}

// NOTE: setInsecure() skips TLS certificate checks. Fine for a hobby display
// on your home network; for more security, use client.setCACert(<root CA>).
static void prepareClient(WiFiClientSecure& client) {
  client.setInsecure();
  client.setTimeout(10);
}

void SpotifyClient::begin() {
  Preferences prefs;
  prefs.begin("spotify", false);
  _refresh = prefs.getString("refresh", "");
  prefs.end();
}

void SpotifyClient::saveRefreshToken(const String& token) {
  _refresh = token;
  Preferences prefs;
  prefs.begin("spotify", false);
  if (token.length()) prefs.putString("refresh", token);
  else prefs.remove("refresh");
  prefs.end();
}

void SpotifyClient::logout() {
  saveRefreshToken("");
  _access = "";
}

String SpotifyClient::authorizeUrl() const {
  return String("https://accounts.spotify.com/authorize?response_type=code") +
         "&client_id=" + SPOTIFY_CLIENT_ID +
         "&redirect_uri=" + urlEncode(SPOTIFY_REDIRECT_URI) +
         "&scope=" + urlEncode(SCOPES) + "&show_dialog=true";
}

bool SpotifyClient::exchangeCode(const String& code, String& err) {
  String body = "grant_type=authorization_code&code=" + urlEncode(code) +
                "&redirect_uri=" + urlEncode(SPOTIFY_REDIRECT_URI);
  return tokenRequest(body, err);
}

bool SpotifyClient::tokenRequest(const String& body, String& err) {
  WiFiClientSecure client;
  prepareClient(client);
  HTTPClient http;
  if (!http.begin(client, "https://accounts.spotify.com/api/token")) {
    err = "could not connect";
    return false;
  }
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  http.addHeader("Authorization",
                 "Basic " + base64::encode(String(SPOTIFY_CLIENT_ID) + ":" +
                                           SPOTIFY_CLIENT_SECRET));
  int status = http.POST(body);
  String resp = http.getString();
  http.end();

  if (status < 0) {
    err = "network error " + String(status);
    return false;
  }

  JsonDocument doc;
  if (deserializeJson(doc, resp)) {
    err = "HTTP " + String(status) + ", unreadable response";
    return false;
  }

  if (status != 200) {
    const char* e = doc["error"] | "";
    const char* desc = doc["error_description"] | "";
    err = "HTTP " + String(status) + ": " + e + " " + desc;
    // invalid_grant = refresh token expired/revoked (they last ~6 months)
    if (status == 400 && String(e) == "invalid_grant" && body.startsWith("grant_type=refresh")) {
      logout();
    }
    return false;
  }

  _access = doc["access_token"].as<String>();
  uint32_t expiresIn = doc["expires_in"] | 3600;
  _accessExpiresAt = millis() + (expiresIn - 60) * 1000UL;  // refresh 1 min early

  // Spotify may hand out a new refresh token; always keep the newest one
  const char* newRefresh = doc["refresh_token"] | "";
  if (strlen(newRefresh) > 0 && _refresh != newRefresh) saveRefreshToken(newRefresh);
  return true;
}

bool SpotifyClient::ensureAccessToken() {
  if (!hasRefreshToken()) return false;
  if (_access.length() && (int32_t)(_accessExpiresAt - millis()) > 0) return true;
  String err;
  bool ok = tokenRequest("grant_type=refresh_token&refresh_token=" + urlEncode(_refresh), err);
  if (!ok) Serial.printf("[spotify] token refresh failed: %s\n", err.c_str());
  return ok;
}

// Pick the cover closest to 300 px (fits the 150 px box at 1/2 scale)
static String pickImage(JsonArrayConst images) {
  String best;
  int bestDiff = INT_MAX;
  for (JsonObjectConst img : images) {
    int w = img["width"] | 0;
    int diff = abs(w - 300);
    if (diff < bestDiff) {
      bestDiff = diff;
      best = img["url"].as<String>();
    }
  }
  return best;
}

PollResult SpotifyClient::getNowPlaying(NowPlaying& out) {
  if (!ensureAccessToken()) return hasRefreshToken() ? PollResult::NetError : PollResult::NeedLogin;

  WiFiClientSecure client;
  prepareClient(client);
  HTTPClient http;
  http.useHTTP10(true);  // no chunked encoding -> we can parse straight from the stream
  if (!http.begin(client, String(API) + "/me/player/currently-playing?additional_types=episode")) {
    return PollResult::NetError;
  }
  http.addHeader("Authorization", "Bearer " + _access);
  const char* headerKeys[] = {"Retry-After"};
  http.collectHeaders(headerKeys, 1);

  int status = http.GET();
  if (status == 204) {  // nothing playing
    http.end();
    return PollResult::NothingPlaying;
  }
  if (status == 401) {  // access token expired early -> refresh next time
    _access = "";
    http.end();
    return PollResult::NetError;
  }
  if (status == 429) {
    int s = http.header("Retry-After").toInt();
    _retryAfterMs = (s > 0 ? s : 5) * 1000UL;
    http.end();
    return PollResult::RateLimited;
  }
  if (status != 200) {
    Serial.printf("[spotify] currently-playing HTTP %d\n", status);
    http.end();
    return PollResult::NetError;
  }

  // Only keep the fields we need -> saves a lot of RAM
  JsonDocument filter;
  filter["is_playing"] = true;
  filter["progress_ms"] = true;
  filter["item"]["id"] = true;
  filter["item"]["name"] = true;
  filter["item"]["duration_ms"] = true;
  filter["item"]["artists"][0]["name"] = true;
  filter["item"]["album"]["name"] = true;
  filter["item"]["album"]["images"][0]["url"] = true;
  filter["item"]["album"]["images"][0]["width"] = true;
  filter["item"]["show"]["name"] = true;            // podcasts
  filter["item"]["images"][0]["url"] = true;        // podcast cover
  filter["item"]["images"][0]["width"] = true;

  JsonDocument doc;
  DeserializationError jerr =
      deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (jerr) {
    Serial.printf("[spotify] JSON error: %s\n", jerr.c_str());
    return PollResult::NetError;
  }

  JsonObjectConst item = doc["item"];
  if (item.isNull()) return PollResult::NothingPlaying;  // e.g. ad or private session

  out.isPlaying = doc["is_playing"] | false;
  out.progressMs = doc["progress_ms"] | 0;
  out.durationMs = item["duration_ms"] | 0;
  out.title = item["name"] | "";

  if (!item["show"].isNull()) {  // podcast episode
    out.artist = item["show"]["name"] | "";
    out.album = "Podcast";
    out.imageUrl = pickImage(item["images"]);
  } else {
    out.artist = "";
    for (JsonObjectConst a : item["artists"].as<JsonArrayConst>()) {
      if (out.artist.length()) out.artist += ", ";
      out.artist += a["name"].as<const char*>();
    }
    out.album = item["album"]["name"] | "";
    out.imageUrl = pickImage(item["album"]["images"]);
  }

  out.trackId = item["id"] | "";
  if (!out.trackId.length()) out.trackId = out.title + "|" + out.artist;  // local files have no id
  return PollResult::Ok;
}

int SpotifyClient::command(const char* method, const char* path) {
  for (int attempt = 0; attempt < 2; attempt++) {
    if (!ensureAccessToken()) return -1;
    WiFiClientSecure client;
    prepareClient(client);
    HTTPClient http;
    if (!http.begin(client, String(API) + path)) return -1;
    http.addHeader("Authorization", "Bearer " + _access);
    http.addHeader("Content-Length", "0");  // Spotify answers 411 without it
    int status = http.sendRequest(method, (uint8_t*)nullptr, 0);
    http.end();
    if (status == 401) {  // token expired -> refresh and retry once
      _access = "";
      continue;
    }
    return status;
  }
  return 401;
}
