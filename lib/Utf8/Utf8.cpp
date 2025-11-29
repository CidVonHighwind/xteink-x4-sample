#include "Utf8.h"

int utf8CodepointLen(const unsigned char c) {
  if (c < 0x80)
    return 1; // 0xxxxxxx
  if ((c >> 5) == 0x6)
    return 2; // 110xxxxx
  if ((c >> 4) == 0xE)
    return 3; // 1110xxxx
  if ((c >> 3) == 0x1E)
    return 4; // 11110xxx
  return 1;   // fallback for invalid
}

uint32_t utf8NextCodepoint(const unsigned char **string) {
  if (**string == 0) {
    return 0;
  }

  const int bytes = utf8CodepointLen(**string);
  const uint8_t *chr = *string;
  *string += bytes;

  if (bytes == 1) {
    return chr[0];
  }

  uint32_t cp = chr[0] & ((1 << (7 - bytes)) - 1); // mask header bits

  for (int i = 1; i < bytes; i++) {
    cp = (cp << 6) | (chr[i] & 0x3F);
  }

  return cp;
}
