#pragma once
#include <Arduino.h>

#include "spotify.h"

// Landscape layout, 320 x 240
//  +----------+-----------------+
//  |  album   | Title           |
//  |  cover   | Artist          |
//  | 150x150  | Album           |
//  +----------+-----------------+
//  | ======progress bar======== |
//  | 1:23                  3:45 |
//  |    |<      (>)      >|     |
//  +----------------------------+

enum class Button { None, Prev, PlayPause, Next };

void uiBegin();
void uiClear();
// Full-screen message, e.g. "Connecting to WiFi"
void uiMessage(const String& title, const String& l1 = "", const String& l2 = "",
               const String& l3 = "");

void uiDrawTrackText(const NowPlaying& np);  // title / artist / album
bool uiDrawArt(const String& url);           // downloads + decodes the JPEG cover
void uiDrawProgress(uint32_t progressMs, uint32_t durationMs);
void uiDrawControls(bool playing);
void uiToast(const String& msg);  // short message over the control row

Button uiHitTest(int x, int y);
