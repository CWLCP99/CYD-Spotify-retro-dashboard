#pragma once
#include <Arduino.h>

// The built-in fonts only have ASCII, so map accented letters to plain ones
// (e.g. "Tití Me Preguntó" -> "Titi Me Pregunto", "¿Qué?" -> "?Que?").
inline const char* latin1ToAscii(uint32_t cp) {
  static const char* table[64] = {
      "A", "A", "A", "A", "A", "A", "AE", "C", "E", "E", "E", "E", "I", "I", "I", "I",
      "D", "N", "O", "O", "O", "O", "O", "x", "O", "U", "U", "U", "U", "Y", "Th", "ss",
      "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",
      "d", "n", "o", "o", "o", "o", "o", "/", "o", "u", "u", "u", "u", "y", "th", "y"};
  if (cp >= 0xC0 && cp <= 0xFF) return table[cp - 0xC0];
  switch (cp) {
    case 0xA0: return " ";
    case 0xA1: return "!";  // ¡
    case 0xBF: return "?";  // ¿
    case 0x2018: case 0x2019: case 0x00B4: return "'";
    case 0x201C: case 0x201D: return "\"";
    case 0x2013: case 0x2014: return "-";
    case 0x2026: return "...";
    case 0x00B7: case 0x2022: return "-";
  }
  return "?";
}

inline String toDisplayText(const String& in) {
  String out;
  out.reserve(in.length());
  size_t i = 0;
  while (i < in.length()) {
    uint8_t c = in[i];
    if (c < 0x80) {
      out += (char)c;
      i++;
      continue;
    }
    int len;
    uint32_t cp;
    if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; }
    else { i++; continue; }  // invalid byte
    if (i + len > in.length()) break;
    for (int k = 1; k < len; k++) cp = (cp << 6) | (in[i + k] & 0x3F);
    i += len;
    out += latin1ToAscii(cp);
  }
  return out;
}

inline String fmtTime(uint32_t ms) {
  uint32_t s = ms / 1000;
  char buf[12];
  snprintf(buf, sizeof(buf), "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
  return buf;
}
