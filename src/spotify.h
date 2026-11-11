#pragma once
#include <Arduino.h>

// What the display needs to know about the current playback
struct NowPlaying {
  bool isPlaying = false;
  String trackId;       // used to detect song changes
  String title;
  String artist;
  String album;
  String imageUrl;      // ~300x300 cover
  uint32_t progressMs = 0;
  uint32_t durationMs = 0;
};

enum class PollResult {
  Ok,              // NowPlaying filled
  NothingPlaying,  // no active device / nothing loaded
  NeedLogin,       // no (valid) refresh token -> user must log in again
  RateLimited,     // HTTP 429, wait retryAfterMs()
  NetError         // temporary problem, just try again later
};

class SpotifyClient {
 public:
  void begin();  // loads the saved refresh token from flash
  bool hasRefreshToken() const { return _refresh.length() > 0; }

  String authorizeUrl() const;  // URL the user opens to log in
  // Exchange the ?code=... from the redirect for tokens (stored in flash)
  bool exchangeCode(const String& code, String& err);
  void logout();

  PollResult getNowPlaying(NowPlaying& out);

  // Player commands, e.g. command("POST", "/me/player/next").
  // Returns the HTTP status (200/204 = ok, 404 = no active device, <0 = network)
  int command(const char* method, const char* path);

  uint32_t retryAfterMs() const { return _retryAfterMs; }

 private:
  bool ensureAccessToken();
  bool tokenRequest(const String& body, String& err);
  void saveRefreshToken(const String& token);

  String _refresh;
  String _access;
  uint32_t _accessExpiresAt = 0;
  uint32_t _retryAfterMs = 5000;
};
