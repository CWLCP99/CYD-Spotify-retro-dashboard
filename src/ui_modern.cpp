// Modern theme: album cover, Spotify-green progress bar, round buttons
#include "config.h"
#if !UI_THEME_RETRO

#include <TFT_eSPI.h>
#include <TJpg_Decoder.h>

#include "text_util.h"
#include "ui.h"

static TFT_eSPI tft;

// ---------- layout & colours ----------
static const int W = 320, H = 240;
static const int ART_X = 8, ART_Y = 8, ART_SIZE = 150;
static const int TEXT_X = 166, TEXT_W = W - TEXT_X - 6;
static const int BAR_X = 10, BAR_Y = 168, BAR_W = W - 20, BAR_H = 6;
static const int TIME_Y = 179;
static const int CTRL_Y = 216, CTRL_TOP = 196;
static const int CTRL_X[3] = {70, 160, 250};

static uint16_t BG, FG, GREEN, GREY, DARK;

// ---------- text helpers ----------
// Draws text word-wrapped inside width w, max `maxLines`, adds "..." if cut.
// Returns the y position below the last line.
static int drawWrapped(String text, int x, int y, int w, int maxLines, int lineH) {
  text.trim();
  int lines = 0;
  while (text.length() && lines < maxLines) {
    bool lastLine = (lines == maxLines - 1);
    int fit = 0, lastSpace = -1;
    for (int i = 1; i <= (int)text.length(); i++) {
      if (tft.textWidth(text.substring(0, i)) > w) break;
      fit = i;
      if (i < (int)text.length() && text[i] == ' ') lastSpace = i;
    }
    String line;
    if (fit >= (int)text.length()) {
      line = text;
      text = "";
    } else if (lastLine) {
      line = text.substring(0, fit);
      while (line.length() && tft.textWidth(line + "...") > w) line.remove(line.length() - 1);
      line.trim();
      line += "...";
      text = "";
    } else {
      int cut = lastSpace > 0 ? lastSpace : max(fit, 1);
      line = text.substring(0, cut);
      text = text.substring(cut);
      text.trim();
    }
    tft.drawString(line, x, y);
    y += lineH;
    lines++;
  }
  return y;
}

// ---------- JPEG ----------
static bool jpgOutput(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
  if (y >= H) return false;
  tft.pushImage(x, y, w, h, bitmap);
  return true;
}

static void drawArtPlaceholder() {
  tft.fillRoundRect(ART_X, ART_Y, ART_SIZE, ART_SIZE, 8, DARK);
  // simple music note
  int cx = ART_X + ART_SIZE / 2, cy = ART_Y + ART_SIZE / 2;
  tft.fillCircle(cx - 12, cy + 22, 12, GREY);
  tft.fillRect(cx - 2, cy - 30, 5, 52, GREY);
  tft.fillTriangle(cx + 3, cy - 30, cx + 3, cy - 12, cx + 24, cy - 18, GREY);
}

// ---------- public ----------
void uiBegin() {
  tft.init();
  tft.setRotation(1);  // landscape; use 3 if the picture is upside down (then also ts.setRotation(3))
#ifdef CYD_INVERT_COLORS
  tft.invertDisplay(true);
#endif
  BG = TFT_BLACK;
  FG = TFT_WHITE;
  GREEN = tft.color565(30, 215, 96);  // Spotify green
  GREY = tft.color565(150, 150, 150);
  DARK = tft.color565(45, 45, 45);
  tft.setSwapBytes(true);  // needed for JPEG pixels
  tft.fillScreen(BG);

  TJpgDec.setCallback(jpgOutput);
}

void uiBeginPlayer() { tft.fillScreen(BG); }

void uiMessage(const String& title, const String& l1, const String& l2, const String& l3) {
  tft.fillScreen(BG);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(GREEN, BG);
  tft.setFreeFont(&FreeSansBold12pt7b);
  tft.drawString(title, W / 2, 70);
  tft.setFreeFont(&FreeSans9pt7b);
  tft.setTextColor(FG, BG);
  if (l1.length()) tft.drawString(l1, W / 2, 120);
  if (l2.length()) tft.drawString(l2, W / 2, 150);
  tft.setTextColor(GREY, BG);
  if (l3.length()) tft.drawString(l3, W / 2, 180);
  tft.setTextDatum(TL_DATUM);
}

void uiDrawTrackText(const NowPlaying& np) {
  tft.fillRect(TEXT_X - 4, 0, W - TEXT_X + 4, BAR_Y - 4, BG);
  tft.setTextDatum(TL_DATUM);
  int y = 10;

  tft.setFreeFont(&FreeSansBold9pt7b);
  tft.setTextColor(FG, BG);
  y = drawWrapped(toDisplayText(np.title), TEXT_X, y, TEXT_W, 3, 20);

  y += 4;
  tft.setFreeFont(&FreeSans9pt7b);
  tft.setTextColor(GREEN, BG);
  y = drawWrapped(toDisplayText(np.artist), TEXT_X, y, TEXT_W, 2, 20);

  y += 4;
  tft.setTextFont(2);
  tft.setTextColor(GREY, BG);
  drawWrapped(toDisplayText(np.album), TEXT_X, y, TEXT_W, 2, 16);
}

bool uiWantsArt() { return true; }

void uiDrawArtJpeg(const uint8_t* jpg, size_t len) {
  if (!jpg || !len) {
    drawArtPlaceholder();
    return;
  }
  uint16_t w = 0, h = 0;
  TJpgDec.getJpgSize(&w, &h, jpg, len);
  uint8_t scale = 1;
  while ((w / scale > ART_SIZE || h / scale > ART_SIZE) && scale < 8) scale *= 2;
  TJpgDec.setJpgScale(scale);
  int dw = w / scale, dh = h / scale;
  if (dw < ART_SIZE || dh < ART_SIZE) tft.fillRect(ART_X, ART_Y, ART_SIZE, ART_SIZE, BG);
  TJpgDec.drawJpg(ART_X + (ART_SIZE - dw) / 2, ART_Y + (ART_SIZE - dh) / 2, jpg, len);
}

void uiDrawProgress(uint32_t progressMs, uint32_t durationMs) {
  int filled = 0;
  if (durationMs > 0) filled = (int)((uint64_t)BAR_W * min(progressMs, durationMs) / durationMs);
  // draw the two parts separately -> no flicker
  if (filled > 0) tft.fillRect(BAR_X, BAR_Y, filled, BAR_H, GREEN);
  if (filled < BAR_W) tft.fillRect(BAR_X + filled, BAR_Y, BAR_W - filled, BAR_H, DARK);

  tft.setTextFont(2);
  tft.setTextColor(GREY, BG);
  tft.setTextPadding(tft.textWidth("88:88"));
  tft.setTextDatum(TL_DATUM);
  tft.drawString(fmtTime(progressMs), BAR_X, TIME_Y);
  tft.setTextDatum(TR_DATUM);
  tft.drawString(fmtTime(durationMs), BAR_X + BAR_W, TIME_Y);
  tft.setTextDatum(TL_DATUM);
  tft.setTextPadding(0);
}

void uiDrawControls(bool playing) {
  tft.fillRect(0, CTRL_TOP, W, H - CTRL_TOP, BG);
  int cy = CTRL_Y;

  // previous: |<
  int cx = CTRL_X[0];
  tft.fillRect(cx - 11, cy - 10, 4, 20, FG);
  tft.fillTriangle(cx + 10, cy - 10, cx + 10, cy + 10, cx - 6, cy, FG);

  // play / pause
  cx = CTRL_X[1];
  tft.fillCircle(cx, cy, 20, GREEN);
  if (playing) {
    tft.fillRect(cx - 8, cy - 9, 6, 18, BG);
    tft.fillRect(cx + 2, cy - 9, 6, 18, BG);
  } else {
    tft.fillTriangle(cx - 6, cy - 11, cx - 6, cy + 11, cx + 11, cy, BG);
  }

  // next: >|
  cx = CTRL_X[2];
  tft.fillTriangle(cx - 10, cy - 10, cx - 10, cy + 10, cx + 6, cy, FG);
  tft.fillRect(cx + 7, cy - 10, 4, 20, FG);
}

void uiToast(const String& msg) {
  tft.fillRect(0, CTRL_TOP, W, H - CTRL_TOP, BG);
  tft.fillRoundRect(20, CTRL_TOP + 2, W - 40, 36, 8, DARK);
  tft.setTextDatum(MC_DATUM);
  tft.setFreeFont(&FreeSans9pt7b);
  tft.setTextColor(FG, DARK);
  tft.drawString(msg, W / 2, CTRL_TOP + 20);
  tft.setTextDatum(TL_DATUM);
}

Button uiHitTest(int x, int y) {
  if (y < CTRL_TOP - 10) return Button::None;
  if (x < 115) return Button::Prev;
  if (x < 205) return Button::PlayPause;
  return Button::Next;
}

void uiAnimate(bool, uint32_t, uint32_t) {}  // nothing animated in this theme

void uiPressFeedback(Button b) {
  int i = b == Button::Prev ? 0 : b == Button::PlayPause ? 1 : b == Button::Next ? 2 : -1;
  if (i >= 0) tft.drawCircle(CTRL_X[i], CTRL_Y, 23, GREY);
}

#endif  // !UI_THEME_RETRO
