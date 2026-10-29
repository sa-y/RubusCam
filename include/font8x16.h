#ifndef FONT8X16_H
#define FONT8X16_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Standard 8x16 monochrome bitmap font for printable ASCII characters (ASCII 32 to 126).
 * Each character is 8 pixels wide and 16 pixels high, represented by 16 bytes (1 bit per pixel).
 */
extern const uint8_t font8x16_data[95][16];

#ifdef __cplusplus
}
#endif

#endif /* FONT8X16_H */
