// Retro theme: a cassette tape whose reels spin while music plays.
// As the song progresses, tape moves from the left reel to the right one.
//
//  +--------------------------------+
//  | o [A| SONG TITLE           ] o |  <- cassette label
//  |    ===== stripes =====         |
//  |   (( @ ))          (( @ ))     |  <- window with spinning reels
//  | o       \__________/         o |
//  +--------------------------------+
//  | ARTIST - ALBUM (scrolls) <<<<  |  <- green LCD
//  | > PLAY              1:23/3:45  |
//  +--------------------------------+
//  [  <<  ]   [ > || ]   [  >>  ]       <- tape-deck keys
//
// Everything is plain shapes + the built-in pixel font, so it's easy to tweak:
// colours are in the PALETTE section, positions in the LAYOUT section.

#include "config.h"
#if UI_THEME_RETRO

#include <TFT_eSPI.h>

#include "text_util.h"
#include "ui.h"

static TFT_eSPI tft;
static TFT_eSprite reelSpr(&tft);   // off-screen buffer -> flicker-free reels
static TFT_eSprite marquee(&tft);   // off-screen buffer for the scrolling LCD line

// ---------- PALETTE ----------
static uint16_t BG, BODY, BODY_LIGHT, SCREW, LABEL, LABEL_INK, STRIPE1, STRIPE2, WINDOW,
    TAPE, HUB, LCD_BG, LCD_SCAN, LCD_FG, LCD_DIM, KEY_FACE, KEY_DOWN, KEY_SHADOW, KEY_HI,
    KEY_INK, ACCENT;

static void initPalette() {
  BG = tft.color565(22, 18, 15);           // dark brown background
  BODY = tft.color565(58, 54, 52);         // cassette shell
  BODY_LIGHT = tft.color565(78, 73, 69);
  SCREW = tft.color565(120, 115, 110);
  LABEL = tft.color565(240, 228, 200);     // cream paper label
  LABEL_INK = tft.color565(60, 40, 30);
  STRIPE1 = tft.color565(240, 140, 40);    // 70s orange
  STRIPE2 = tft.color565(215, 65, 45);     // 70s red
  WINDOW = tft.color565(26, 22, 20);       // smoked plastic window
  TAPE = tft.color565(95, 58, 36);         // brown magnetic tape
  HUB = tft.color565(235, 232, 225);       // white reel hub
  LCD_BG = tft.color565(8, 24, 12);
  LCD_SCAN = tft.color565(12, 32, 17);     // faint scanlines
  LCD_FG = tft.color565(90, 255, 130);     // phosphor green
  LCD_DIM = tft.color565(40, 120, 65);
  KEY_FACE = tft.color565(200, 198, 190);  // brushed-metal keys
  KEY_DOWN = tft.color565(150, 148, 140);
  KEY_SHADOW = tft.color565(70, 68, 64);
  KEY_HI = tft.color565(245, 245, 240);
  KEY_INK = tft.color565(40, 36, 34);
  ACCENT = tft.color565(245, 190, 60);     // 70s yellow
}

// ---------- LAYOUT ----------
static const int W = 320, H = 240;
// cassette
static const int CAS_X = 8, CAS_Y = 4, CAS_W = 304, CAS_H = 124;
static const int LBL_X = 27, LBL_Y = 10, LBL_W = 266, LBL_H = 38;
static const int WIN_X = 50, WIN_Y = 52, WIN_W = 220, WIN_H = 60;
static const int REEL_Y = 82, REEL_LX = 110, REEL_RX = 210;
static const int HUB_R = 13, R_MIN = 14, R_MAX = 29;  // tape radius range
static const int REEL_BOX = 60;
// LCD
static const int LCD_X = 8, LCD_Y = 134, LCD_W = 304, LCD_H = 42;
static const int LCD_L1_Y = 138, LCD_L2_Y = 156, LCD_TX = 14, LCD_TW = 292;
// keys
static const int KEY_Y = 182, KEY_W = 94, KEY_H = 54;
static const int KEY_X[3] = {8, 113, 218};

// ---------- state ----------
static bool gPlaying = false;
static float gAngleL = 0, gAngleR = 0;
static int gDrawnRL = -1, gDrawnRR = -1;
static float gDrawnAL = -99, gDrawnAR = -99;
static uint32_t gLastAnim = 0, gLastMarquee = 0;
static String gMarqueeText;
static int gMarqueeOff = 0;
static int gMarqueeW = 0;
static bool gInPlayer = false;

// ---------- helpers ----------
// pixel font = TFT_eSPI font 1 (5x7) scaled up; size 2 => 12 x 16 px per character
static void pixelFont(TFT_eSPI& g, uint8_t size, uint16_t color) {
  g.setTextFont(1);
  g.setTextSize(size);
  g.setTextColor(color);
}

static String fitChars(const String& s, int maxChars) {
  if ((int)s.length() <= maxChars) return s;
  return s.substring(0, maxChars - 2) + "..";
}

// LCD background with faint horizontal scanlines (on odd screen rows)
static void lcdFill(int x, int y, int w, int h) {
  tft.fillRect(x, y, w, h, LCD_BG);
  for (int yy = y; yy < y + h; yy++)
    if (yy & 1) tft.drawFastHLine(x, yy, w, LCD_SCAN);
}

static void drawReel(int cx, float rTape, float angle) {
  const int c = REEL_BOX / 2;
  reelSpr.fillSprite(WINDOW);
  reelSpr.fillCircle(c, c, (int)rTape, TAPE);
  reelSpr.drawCircle(c, c, (int)rTape, tft.color565(70, 42, 26));  // tape edge
  reelSpr.fillCircle(c, c, HUB_R, HUB);
  reelSpr.fillCircle(c, c, HUB_R - 4, WINDOW);
  // 6 teeth inside the hub -> this is what you see rotating
  for (int k = 0; k < 6; k++) {
    float a = angle + k * PI / 3;
    reelSpr.drawWideLine(c + cosf(a) * 3, c + sinf(a) * 3, c + cosf(a) * (HUB_R - 3),
                         c + sinf(a) * (HUB_R - 3), 3, HUB, WINDOW);
  }
  reelSpr.pushSprite(cx - c, REEL_Y - c);
}

// Tape is conserved: area on left + area on right stays constant
static void reelRadii(uint32_t progressMs, uint32_t durationMs, float& rl, float& rr) {
  float f = durationMs ? (float)min(progressMs, durationMs) / durationMs : 0;
  float a0 = R_MIN * R_MIN, a1 = R_MAX * R_MAX;
  rl = sqrtf(a0 + (a1 - a0) * (1 - f));
  rr = sqrtf(a0 + (a1 - a0) * f);
}

static void drawMarquee() {
  marquee.fillSprite(LCD_BG);
  for (int y = 1; y < 16; y += 2) marquee.drawFastHLine(0, y, LCD_TW, LCD_SCAN);  // row 138+odd = odd
  pixelFont(marquee, 2, LCD_FG);
  marquee.setTextDatum(TL_DATUM);
  int tw = marquee.textWidth(gMarqueeText);
  if (tw <= LCD_TW) {
    marquee.drawString(gMarqueeText, 0, 0);
  } else {
    const int gap = 48;
    marquee.drawString(gMarqueeText, -gMarqueeOff, 0);
    marquee.drawString(gMarqueeText, -gMarqueeOff + tw + gap, 0);
    gMarqueeOff = (gMarqueeOff + 1) % (tw + gap);
  }
  marquee.pushSprite(LCD_TX, LCD_L1_Y);
}

static void drawIcon(int i, int cx, int cy, uint16_t ink) {
  if (i == 0) {  // rewind <<
    tft.fillTriangle(cx - 14, cy, cx - 1, cy - 8, cx - 1, cy + 8, ink);
    tft.fillTriangle(cx - 1, cy, cx + 12, cy - 8, cx + 12, cy + 8, ink);
  } else if (i == 2) {  // fast forward >>
    tft.fillTriangle(cx + 14, cy, cx + 1, cy - 8, cx + 1, cy + 8, ink);
    tft.fillTriangle(cx + 1, cy, cx - 12, cy - 8, cx - 12, cy + 8, ink);
  } else {  // play / pause  > ||
    tft.fillTriangle(cx - 17, cy - 8, cx - 17, cy + 8, cx - 4, cy, ink);
    tft.fillRect(cx + 4, cy - 8, 4, 16, ink);
    tft.fillRect(cx + 11, cy - 8, 4, 16, ink);
  }
}

static void drawKey(int i, bool down) {
  static const char* labels[3] = {"REW", "PLAY/PAUSE", "FF"};
  int x = KEY_X[i];
  int off = down ? 4 : 0;  // pressed keys sink down
  tft.fillRect(x, KEY_Y, KEY_W, KEY_H, BG);
  tft.fillRoundRect(x, KEY_Y + 4, KEY_W, KEY_H - 4, 5, KEY_SHADOW);  // key "depth"
  tft.fillRoundRect(x, KEY_Y + off, KEY_W, KEY_H - 4, 5, down ? KEY_DOWN : KEY_FACE);
  tft.drawFastHLine(x + 4, KEY_Y + off + 1, KEY_W - 8, down ? KEY_FACE : KEY_HI);
  uint16_t ink = (i == 1 && gPlaying) ? STRIPE2 : KEY_INK;
  drawIcon(i, x + KEY_W / 2, KEY_Y + off + 18, ink);
  tft.setTextDatum(MC_DATUM);
  pixelFont(tft, 1, KEY_INK);
  tft.drawString(labels[i], x + KEY_W / 2, KEY_Y + off + 38);
  tft.setTextDatum(TL_DATUM);
}

static void drawStripes(int x, int y, int w) {
  tft.fillRect(x, y, w, 5, STRIPE1);
  tft.fillRect(x, y + 5, w, 5, STRIPE2);
}

// ---------- public ----------
void uiBegin() {
  tft.init();
  tft.setRotation(1);  // use 3 if upside down (then also ts.setRotation(3) in main.cpp)
#ifdef CYD_INVERT_COLORS
  tft.invertDisplay(true);
#endif
  initPalette();
  tft.fillScreen(BG);
  reelSpr.createSprite(REEL_BOX, REEL_BOX);
  marquee.createSprite(LCD_TW, 16);
}

void uiMessage(const String& title, const String& l1, const String& l2, const String& l3) {
  gInPlayer = false;
  tft.fillScreen(BG);
  drawStripes(0, 24, W);
  tft.fillRect(0, 34, W, 5, ACCENT);
  tft.setTextDatum(MC_DATUM);
  pixelFont(tft, 2, STRIPE1);
  tft.drawString(fitChars(title, 26), W / 2, 80);
  pixelFont(tft, 1, LABEL);
  if (l1.length()) tft.drawString(l1, W / 2, 118);
  pixelFont(tft, 2, LCD_FG);
  if (l2.length()) tft.drawString(fitChars(l2, 26), W / 2, 142);
  pixelFont(tft, 1, SCREW);
  if (l3.length()) tft.drawString(l3, W / 2, 170);
  tft.setTextDatum(TL_DATUM);
  drawStripes(0, H - 22, W);
}

void uiBeginPlayer() {
  gInPlayer = true;
  tft.fillScreen(BG);
  // cassette shell + screws
  tft.fillRoundRect(CAS_X, CAS_Y, CAS_W, CAS_H, 10, BODY);
  const int sx[2] = {CAS_X + 10, CAS_X + CAS_W - 11}, sy[2] = {CAS_Y + 10, CAS_Y + CAS_H - 11};
  for (int a = 0; a < 2; a++)
    for (int b = 0; b < 2; b++) {
      tft.fillCircle(sx[a], sy[b], 4, SCREW);
      tft.drawFastHLine(sx[a] - 2, sy[b], 5, BODY);
    }
  // paper label with "A" side marker and stripes
  tft.fillRoundRect(LBL_X, LBL_Y, LBL_W, LBL_H, 4, LABEL);
  drawStripes(LBL_X + 1, LBL_Y + 26, LBL_W - 2);
  tft.fillRect(LBL_X + 6, LBL_Y + 4, 16, 18, STRIPE2);
  tft.setTextDatum(MC_DATUM);
  pixelFont(tft, 2, LABEL);
  tft.drawString("A", LBL_X + 15, LBL_Y + 14);
  tft.setTextDatum(TL_DATUM);
  // window
  tft.fillRoundRect(WIN_X, WIN_Y, WIN_W, WIN_H, 10, WINDOW);
  tft.drawRoundRect(WIN_X, WIN_Y, WIN_W, WIN_H, 10, BODY_LIGHT);
  // head area at the bottom of the shell
  int y0 = CAS_Y + CAS_H - 14, y1 = CAS_Y + CAS_H - 1;
  tft.fillTriangle(80, y1, 92, y0, 92, y1, BODY_LIGHT);
  tft.fillRect(92, y0, 136, y1 - y0 + 1, BODY_LIGHT);
  tft.fillTriangle(240, y1, 228, y0, 228, y1, BODY_LIGHT);
  tft.fillCircle(120, y0 + 7, 3, WINDOW);
  tft.fillCircle(200, y0 + 7, 3, WINDOW);
  tft.fillRect(150, y0 + 4, 20, 7, WINDOW);
  // LCD panel
  tft.fillRoundRect(LCD_X, LCD_Y, LCD_W, LCD_H, 5, KEY_SHADOW);
  lcdFill(LCD_X + 2, LCD_Y + 2, LCD_W - 4, LCD_H - 4);
  // force reels to redraw
  gDrawnRL = gDrawnRR = -1;
}

void uiDrawTrackText(const NowPlaying& np) {
  // title on the label (static, 19 chars max)
  tft.fillRect(LBL_X + 26, LBL_Y + 3, LBL_W - 30, 20, LABEL);
  pixelFont(tft, 2, LABEL_INK);
  tft.setTextDatum(ML_DATUM);
  tft.drawString(fitChars(toDisplayText(np.title), 19), LBL_X + 28, LBL_Y + 14);
  tft.setTextDatum(TL_DATUM);
  // artist - album scrolls on the LCD
  String t = toDisplayText(np.artist);
  if (np.album.length()) t += " - " + toDisplayText(np.album);
  gMarqueeText = t;
  gMarqueeOff = 0;
  pixelFont(marquee, 2, LCD_FG);
  gMarqueeW = marquee.textWidth(t);
  drawMarquee();
}

bool uiWantsArt() { return false; }                // no cover in this theme
void uiDrawArtJpeg(const uint8_t*, size_t) {}

void uiDrawProgress(uint32_t progressMs, uint32_t durationMs) {
  lcdFill(LCD_TX, LCD_L2_Y, LCD_TW, 16);
  pixelFont(tft, 2, gPlaying ? LCD_FG : LCD_DIM);
  tft.setTextDatum(TL_DATUM);
  tft.drawString(gPlaying ? "> PLAY" : "|| PAUSE", LCD_TX, LCD_L2_Y);
  tft.setTextDatum(TR_DATUM);
  pixelFont(tft, 2, LCD_FG);
  tft.drawString(fmtTime(progressMs) + "/" + fmtTime(durationMs), LCD_TX + LCD_TW, LCD_L2_Y);
  tft.setTextDatum(TL_DATUM);
}

void uiDrawControls(bool playing) {
  gPlaying = playing;
  drawKey(0, false);
  drawKey(1, playing);  // play stays latched down while playing, like a real deck
  drawKey(2, false);
}

void uiAnimate(bool playing, uint32_t progressMs, uint32_t durationMs) {
  if (!gInPlayer) return;
  uint32_t now = millis();

  // marquee: 1 px every 30 ms
  if (gMarqueeW > LCD_TW && now - gLastMarquee >= 30) {
    gLastMarquee = now;
    drawMarquee();
  }

  // reels: ~25 fps
  if (now - gLastAnim < 40) return;
  float dt = (now - gLastAnim) / 1000.0f;
  gLastAnim = now;
  if (dt > 0.2f) dt = 0.2f;

  float rl, rr;
  reelRadii(progressMs, durationMs, rl, rr);
  if (playing) {
    // constant tape speed: the reel with less tape spins faster (omega = v / r)
    const float TAPE_SPEED = 45.0f;  // px per second, purely visual
    gAngleL += TAPE_SPEED / rl * dt;
    gAngleR += TAPE_SPEED / rr * dt;
  }
  if ((int)rl != gDrawnRL || gAngleL != gDrawnAL) {
    drawReel(REEL_LX, rl, gAngleL);
    gDrawnRL = (int)rl;
    gDrawnAL = gAngleL;
  }
  if ((int)rr != gDrawnRR || gAngleR != gDrawnAR) {
    drawReel(REEL_RX, rr, gAngleR);
    gDrawnRR = (int)rr;
    gDrawnAR = gAngleR;
  }
}

void uiToast(const String& msg) {
  lcdFill(LCD_TX, LCD_L2_Y, LCD_TW, 16);
  pixelFont(tft, 2, ACCENT);
  tft.setTextDatum(TL_DATUM);
  String m = msg;
  m.toUpperCase();
  tft.drawString(fitChars("! " + m, 24), LCD_TX, LCD_L2_Y);
}

Button uiHitTest(int x, int y) {
  if (y < KEY_Y - 8) return Button::None;
  if (x < 110) return Button::Prev;
  if (x < 215) return Button::PlayPause;
  return Button::Next;
}

void uiPressFeedback(Button b) {
  int i = b == Button::Prev ? 0 : b == Button::PlayPause ? 1 : b == Button::Next ? 2 : -1;
  if (i >= 0) drawKey(i, true);
}

#endif  // UI_THEME_RETRO
