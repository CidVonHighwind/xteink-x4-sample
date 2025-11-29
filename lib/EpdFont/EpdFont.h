#pragma once
#include "EpdFontData.h"
#include <cstdint>

class EpdFont {
  void getTextBounds(const char *string, int startX, int startY, int *minX,
                     int *minY, int *maxX, int *maxY) const;

public:
  const EpdFontData *data;
  explicit EpdFont(const EpdFontData *data);
  ~EpdFont() = default;
  void getTextDimensions(const char *string, int *w, int *h) const;
  bool hasPrintableChars(const char *string) const;

  const EpdGlyph *getGlyph(uint32_t cp) const;
};
