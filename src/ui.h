#pragma once
#include <Arduino.h>

#include "spotify.h"

// Every theme (ui_modern.cpp, ui_retro.cpp) implements these functions.
// main.cpp only talks to the screen through them, so a new theme = a new file.
// Screen is 320 x 240, landscape.

enum class Button { None, Prev, PlayPause, Next };

void uiBegin();
// Full-screen message, e.g. "Connecting to WiFi"
void uiMessage(const String& title, const String& l1 = "", const String& l2 = "",
               const String& l3 = "");

void uiBeginPlayer();                         // draw the static parts of the player screen
void uiDrawTrackText(const NowPlaying& np);   // called when the song changes
bool uiWantsArt();                            // should the album cover be downloaded?
void uiDrawArtJpeg(const uint8_t* jpg, size_t len);  // nullptr = no cover available
void uiDrawProgress(uint32_t progressMs, uint32_t durationMs);  // ~2x per second
void uiDrawControls(bool playing);
void uiAnimate(bool playing, uint32_t progressMs, uint32_t durationMs);  // ~50x per second
void uiToast(const String& msg);              // short message (2 s)

Button uiHitTest(int x, int y);
void uiPressFeedback(Button b);               // finger is down on button b
