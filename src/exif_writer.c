#include "exif_writer.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* TIFF Data Types */
#define TIFF_TYPE_ASCII 2
#define TIFF_TYPE_SHORT 3
#define TIFF_TYPE_LONG 4
#define TIFF_TYPE_RATIONAL 5
#define TIFF_TYPE_UNDEFINED 7
#define TIFF_TYPE_SRATIONAL 10

static void write_le16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xFFU);
  p[1] = (uint8_t)((v >> 8) & 0xFFU);
}

static void write_le32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xFFU);
  p[1] = (uint8_t)((v >> 8) & 0xFFU);
  p[2] = (uint8_t)((v >> 16) & 0xFFU);
  p[3] = (uint8_t)((v >> 24) & 0xFFU);
}

static uint32_t gcd_u32(uint32_t a, uint32_t b) {
  while (b != 0) {
    uint32_t t = a % b;
    a = b;
    b = t;
  }
  return a == 0 ? 1 : a;
}

static bool contains_substr_ci(const char *haystack, const char *needle) {
  if (!haystack || !needle || needle[0] == '\0') {
    return false;
  }
  size_t nlen = strlen(needle);
  for (const char *p = haystack; *p != '\0'; ++p) {
    size_t i = 0;
    while (i < nlen && p[i] != '\0' &&
           tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
      ++i;
    }
    if (i == nlen) {
      return true;
    }
  }
  return false;
}

void exif_metadata_init(ExifMetadata *meta, const char *camera_model, int width,
                        int height, const CameraExposureStatus *exp_status) {
  if (!meta) {
    return;
  }
  memset(meta, 0, sizeof(*meta));

  snprintf(meta->make, sizeof(meta->make), "Raspberry Pi");
  if (camera_model && camera_model[0] != '\0') {
    snprintf(meta->model, sizeof(meta->model), "%s", camera_model);
  } else {
    snprintf(meta->model, sizeof(meta->model), "RubusCam (libcamera)");
  }
  snprintf(meta->software, sizeof(meta->software), "RubusCam");

  time_t now = time(NULL);
  struct tm tm_buf;
  localtime_r(&now, &tm_buf);
  strftime(meta->datetime_str, sizeof(meta->datetime_str), "%Y:%m:%d %H:%M:%S",
           &tm_buf);

  meta->width = width > 0 ? width : 0;
  meta->height = height > 0 ? height : 0;

  /* Optical defaults based on Raspberry Pi Camera Module generation */
  if (contains_substr_ci(meta->model, "imx708")) {
    /* Camera Module 3 (IMX708): F1.8, 4.74mm */
    meta->f_number = 1.8f;
    meta->focal_length_mm = 4.74f;
  } else if (contains_substr_ci(meta->model, "imx219")) {
    /* Camera Module v2 (IMX219): F2.0, 3.04mm */
    meta->f_number = 2.0f;
    meta->focal_length_mm = 3.04f;
  } else if (contains_substr_ci(meta->model, "ov5647")) {
    /* Camera Module v1 (OV5647): F2.9, 3.60mm */
    meta->f_number = 2.9f;
    meta->focal_length_mm = 3.60f;
  } else {
    /* Default to Camera Module 3 optical specs */
    meta->f_number = 1.8f;
    meta->focal_length_mm = 4.74f;
  }

  meta->metering_mode = 5; /* 5 = Pattern / Matrix */
  meta->light_source = 0;  /* 0 = Unknown / Auto */
  meta->wb_mode = 0;       /* Auto White Balance */
  meta->exp_mode = 0;      /* Auto Exposure */

  if (exp_status) {
    meta->exposure_us = exp_status->actual_exposure_us > 0
                            ? exp_status->actual_exposure_us
                            : exp_status->shutter_us_setting;
    if (exp_status->iso_setting > 0) {
      meta->iso_value = exp_status->iso_setting;
    } else {
      int actual_iso = (int)(exp_status->actual_analogue_gain * 100.0f + 0.5f);
      meta->iso_value = actual_iso >= 100 ? actual_iso : 100;
    }
    meta->ev_bias = exp_status->ev_compensation;
    if (exp_status->iso_setting > 0 || exp_status->shutter_us_setting > 0) {
      meta->exp_mode = 1; /* Manual Exposure */
    }

    switch (exp_status->metering_mode) {
    case CAMERA_METERING_CENTRE_WEIGHTED:
      meta->metering_mode = 2; /* CenterWeightedAverage */
      break;
    case CAMERA_METERING_SPOT:
      meta->metering_mode = 3; /* Spot */
      break;
    case CAMERA_METERING_MATRIX:
    default:
      meta->metering_mode = 5; /* Pattern / Matrix */
      break;
    }

    if (exp_status->awb_locked || exp_status->wb_mode != CAMERA_WB_MODE_AUTO) {
      meta->wb_mode = 1; /* Manual White Balance */
    } else {
      meta->wb_mode = 0; /* Auto White Balance */
    }

    switch (exp_status->wb_mode) {
    case CAMERA_WB_MODE_DAYLIGHT:
      meta->light_source = 1; /* Daylight */
      break;
    case CAMERA_WB_MODE_FLUORESCENT:
      meta->light_source = 2; /* Fluorescent */
      break;
    case CAMERA_WB_MODE_TUNGSTEN:
      meta->light_source = 3; /* Tungsten (incandescent light) */
      break;
    case CAMERA_WB_MODE_CLOUDY:
      meta->light_source = 10; /* Cloudy weather */
      break;
    case CAMERA_WB_MODE_SHADE:
      meta->light_source = 11; /* Shade */
      break;
    case CAMERA_WB_MODE_KELVIN:
      meta->light_source = 255; /* Other light source */
      break;
    case CAMERA_WB_MODE_AUTO:
    default:
      meta->light_source = 0; /* Unknown / Auto */
      break;
    }
  } else {
    meta->exposure_us = 16667;
    meta->iso_value = 100;
    meta->ev_bias = 0.0f;
  }
}

/*
 * Helper functions for writing TIFF IFD entries & out-of-line payloads.
 */
static void put_ifd_short(uint8_t *entry_ptr, uint16_t tag, uint16_t value) {
  write_le16(entry_ptr + 0, tag);
  write_le16(entry_ptr + 2, TIFF_TYPE_SHORT);
  write_le32(entry_ptr + 4, 1);
  write_le16(entry_ptr + 8, value);
  write_le16(entry_ptr + 10, 0);
}

static void put_ifd_long(uint8_t *entry_ptr, uint16_t tag, uint32_t value) {
  write_le16(entry_ptr + 0, tag);
  write_le16(entry_ptr + 2, TIFF_TYPE_LONG);
  write_le32(entry_ptr + 4, 1);
  write_le32(entry_ptr + 8, value);
}

static void put_ifd_undefined4(uint8_t *entry_ptr, uint16_t tag,
                               const char bytes[4]) {
  write_le16(entry_ptr + 0, tag);
  write_le16(entry_ptr + 2, TIFF_TYPE_UNDEFINED);
  write_le32(entry_ptr + 4, 4);
  memcpy(entry_ptr + 8, bytes, 4);
}

static bool put_ifd_ascii(uint8_t *tiff_base, size_t tiff_cap,
                          uint8_t *entry_ptr, uint16_t tag, const char *str,
                          size_t *io_data_off) {
  if (!str) {
    str = "";
  }
  size_t count = strlen(str) + 1; /* Include NUL terminator */

  write_le16(entry_ptr + 0, tag);
  write_le16(entry_ptr + 2, TIFF_TYPE_ASCII);
  write_le32(entry_ptr + 4, (uint32_t)count);

  if (count <= 4) {
    memset(entry_ptr + 8, 0, 4);
    memcpy(entry_ptr + 8, str, count);
    return true;
  }

  size_t off = *io_data_off;
  if (off + count > tiff_cap) {
    return false;
  }
  write_le32(entry_ptr + 8, (uint32_t)off);
  memcpy(tiff_base + off, str, count);
  off += count;
  if (off & 1U) {
    if (off < tiff_cap) {
      tiff_base[off] = 0;
    }
    off++;
  }
  *io_data_off = off;
  return true;
}

static bool put_ifd_rational(uint8_t *tiff_base, size_t tiff_cap,
                             uint8_t *entry_ptr, uint16_t tag, uint32_t num,
                             uint32_t den, size_t *io_data_off) {
  size_t off = *io_data_off;
  if (off + 8 > tiff_cap) {
    return false;
  }
  write_le16(entry_ptr + 0, tag);
  write_le16(entry_ptr + 2, TIFF_TYPE_RATIONAL);
  write_le32(entry_ptr + 4, 1);
  write_le32(entry_ptr + 8, (uint32_t)off);

  write_le32(tiff_base + off, num);
  write_le32(tiff_base + off + 4, den);
  *io_data_off = off + 8;
  return true;
}

static bool put_ifd_srational(uint8_t *tiff_base, size_t tiff_cap,
                              uint8_t *entry_ptr, uint16_t tag, int32_t num,
                              int32_t den, size_t *io_data_off) {
  size_t off = *io_data_off;
  if (off + 8 > tiff_cap) {
    return false;
  }
  write_le16(entry_ptr + 0, tag);
  write_le16(entry_ptr + 2, TIFF_TYPE_SRATIONAL);
  write_le32(entry_ptr + 4, 1);
  write_le32(entry_ptr + 8, (uint32_t)off);

  write_le32(tiff_base + off, (uint32_t)num);
  write_le32(tiff_base + off + 4, (uint32_t)den);
  *io_data_off = off + 8;
  return true;
}

size_t exif_build_app1_segment(const ExifMetadata *meta, uint8_t *out_buf,
                               size_t out_buf_capacity) {
  if (!meta || !out_buf || out_buf_capacity < 512) {
    return 0;
  }

  memset(out_buf, 0, out_buf_capacity);

  /* Reserve 10 bytes for APP1 header: FF E1 <len_hi> <len_lo> "Exif\0\0" */
  uint8_t *tiff_base = out_buf + 10;
  size_t tiff_cap = out_buf_capacity - 10;

  /* 1. TIFF Header (8 bytes, Little Endian 'II') */
  tiff_base[0] = 'I';
  tiff_base[1] = 'I';
  write_le16(tiff_base + 2, 0x002A);
  write_le32(tiff_base + 4, 8); /* Offset to IFD0 */

  /* 2. IFD0 (9 entries, sorted by tag in ascending order) */
  const uint16_t ifd0_count = 9;
  size_t ifd0_off = 8;
  write_le16(tiff_base + ifd0_off, ifd0_count);

  uint8_t *ifd0_entries = tiff_base + ifd0_off + 2;
  size_t ifd0_next_ptr_off = ifd0_off + 2 + (size_t)ifd0_count * 12;
  write_le32(tiff_base + ifd0_next_ptr_off, 0); /* No IFD1 */

  size_t data_off = ifd0_next_ptr_off + 4;

  const char *dt_str = meta->datetime_str[0] != '\0' ? meta->datetime_str
                                                     : "2026:01:01 00:00:00";

  /* IFD0 Entry 0: 0x010F Make */
  if (!put_ifd_ascii(tiff_base, tiff_cap, ifd0_entries + 0 * 12, 0x010F,
                     meta->make, &data_off)) {
    return 0;
  }
  /* IFD0 Entry 1: 0x0110 Model */
  if (!put_ifd_ascii(tiff_base, tiff_cap, ifd0_entries + 1 * 12, 0x0110,
                     meta->model, &data_off)) {
    return 0;
  }
  /* IFD0 Entry 2: 0x0112 Orientation (1 = normal) */
  put_ifd_short(ifd0_entries + 2 * 12, 0x0112, 1);
  /* IFD0 Entry 3: 0x011A XResolution (72/1) */
  if (!put_ifd_rational(tiff_base, tiff_cap, ifd0_entries + 3 * 12, 0x011A, 72,
                        1, &data_off)) {
    return 0;
  }
  /* IFD0 Entry 4: 0x011B YResolution (72/1) */
  if (!put_ifd_rational(tiff_base, tiff_cap, ifd0_entries + 4 * 12, 0x011B, 72,
                        1, &data_off)) {
    return 0;
  }
  /* IFD0 Entry 5: 0x0128 ResolutionUnit (2 = inches) */
  put_ifd_short(ifd0_entries + 5 * 12, 0x0128, 2);
  /* IFD0 Entry 6: 0x0131 Software */
  if (!put_ifd_ascii(tiff_base, tiff_cap, ifd0_entries + 6 * 12, 0x0131,
                     meta->software, &data_off)) {
    return 0;
  }
  /* IFD0 Entry 7: 0x0132 DateTime */
  if (!put_ifd_ascii(tiff_base, tiff_cap, ifd0_entries + 7 * 12, 0x0132, dt_str,
                     &data_off)) {
    return 0;
  }

  /* SubIFD starts right after IFD0's data area */
  size_t subifd_off = data_off;
  /* IFD0 Entry 8: 0x8769 ExifIFDPointer */
  put_ifd_long(ifd0_entries + 8 * 12, 0x8769, (uint32_t)subifd_off);

  /* 3. Exif SubIFD (15 entries, sorted by tag in ascending order) */
  const uint16_t sub_count = 15;
  size_t sub_next_ptr_off = subifd_off + 2 + (size_t)sub_count * 12;
  if (sub_next_ptr_off + 4 > tiff_cap) {
    return 0;
  }

  write_le16(tiff_base + subifd_off, sub_count);
  uint8_t *sub_entries = tiff_base + subifd_off + 2;
  write_le32(tiff_base + sub_next_ptr_off, 0);

  data_off = sub_next_ptr_off + 4;

  /* SubIFD Entry 0: 0x829A ExposureTime (RATIONAL) */
  uint32_t exp_num =
      meta->exposure_us > 0 ? (uint32_t)meta->exposure_us : 16667U;
  uint32_t exp_den = 1000000U;
  uint32_t g = gcd_u32(exp_num, exp_den);
  exp_num /= g;
  exp_den /= g;
  if (!put_ifd_rational(tiff_base, tiff_cap, sub_entries + 0 * 12, 0x829A,
                        exp_num, exp_den, &data_off)) {
    return 0;
  }

  /* SubIFD Entry 1: 0x829D FNumber (RATIONAL) */
  uint32_t f_num =
      meta->f_number > 0.0f ? (uint32_t)(meta->f_number * 100.0f + 0.5f) : 180U;
  if (!put_ifd_rational(tiff_base, tiff_cap, sub_entries + 1 * 12, 0x829D,
                        f_num, 100U, &data_off)) {
    return 0;
  }

  /* SubIFD Entry 2: 0x8827 ISOSpeedRatings (SHORT) */
  uint16_t iso_u16 =
      meta->iso_value > 0
          ? (meta->iso_value > 65535 ? 65535U : (uint16_t)meta->iso_value)
          : 100U;
  put_ifd_short(sub_entries + 2 * 12, 0x8827, iso_u16);

  /* SubIFD Entry 3: 0x9000 ExifVersion (UNDEFINED "0230") */
  put_ifd_undefined4(sub_entries + 3 * 12, 0x9000, "0230");

  /* SubIFD Entry 4: 0x9003 DateTimeOriginal */
  if (!put_ifd_ascii(tiff_base, tiff_cap, sub_entries + 4 * 12, 0x9003, dt_str,
                     &data_off)) {
    return 0;
  }

  /* SubIFD Entry 5: 0x9004 DateTimeDigitized */
  if (!put_ifd_ascii(tiff_base, tiff_cap, sub_entries + 5 * 12, 0x9004, dt_str,
                     &data_off)) {
    return 0;
  }

  /* SubIFD Entry 6: 0x9204 ExposureBiasValue (SRATIONAL, in 1/30 EV units) */
  int32_t ev_num = (int32_t)roundf(meta->ev_bias * 30.0f);
  int32_t ev_den = 30;
  if (!put_ifd_srational(tiff_base, tiff_cap, sub_entries + 6 * 12, 0x9204,
                         ev_num, ev_den, &data_off)) {
    return 0;
  }

  /* SubIFD Entry 7: 0x9207 MeteringMode (SHORT: 2=Center, 3=Spot, 5=Pattern) */
  put_ifd_short(sub_entries + 7 * 12, 0x9207,
                meta->metering_mode > 0 ? meta->metering_mode : 5U);

  /* SubIFD Entry 8: 0x9208 LightSource (SHORT) */
  put_ifd_short(sub_entries + 8 * 12, 0x9208, meta->light_source);

  /* SubIFD Entry 9: 0x920A FocalLength (RATIONAL) */
  uint32_t fl_num = meta->focal_length_mm > 0.0f
                        ? (uint32_t)(meta->focal_length_mm * 100.0f + 0.5f)
                        : 474U;
  if (!put_ifd_rational(tiff_base, tiff_cap, sub_entries + 9 * 12, 0x920A,
                        fl_num, 100U, &data_off)) {
    return 0;
  }

  /* SubIFD Entry 10: 0xA001 ColorSpace (1 = sRGB) */
  put_ifd_short(sub_entries + 10 * 12, 0xA001, 1);

  /* SubIFD Entry 11: 0xA002 PixelXDimension (LONG) */
  put_ifd_long(sub_entries + 11 * 12, 0xA002,
               meta->width > 0 ? (uint32_t)meta->width : 0U);

  /* SubIFD Entry 12: 0xA003 PixelYDimension (LONG) */
  put_ifd_long(sub_entries + 12 * 12, 0xA003,
               meta->height > 0 ? (uint32_t)meta->height : 0U);

  /* SubIFD Entry 13: 0xA402 ExposureMode (0 = Auto, 1 = Manual) */
  put_ifd_short(sub_entries + 13 * 12, 0xA402, meta->exp_mode);

  /* SubIFD Entry 14: 0xA403 WhiteBalance (0 = Auto, 1 = Manual) */
  put_ifd_short(sub_entries + 14 * 12, 0xA403, meta->wb_mode);

  /* 4. Finalize APP1 Marker Header */
  size_t tiff_len = data_off;
  size_t app1_payload_len =
      2 + 6 + tiff_len; /* length field + "Exif\0\0" + TIFF */
  if (app1_payload_len > 65535) {
    return 0;
  }

  out_buf[0] = 0xFF;
  out_buf[1] = 0xE1;
  out_buf[2] = (uint8_t)((app1_payload_len >> 8) & 0xFFU);
  out_buf[3] = (uint8_t)(app1_payload_len & 0xFFU);
  memcpy(out_buf + 4, "Exif\0\0", 6);

  return 2 + app1_payload_len;
}

bool exif_write_jpeg_with_app1(const char *filepath, const uint8_t *jpeg_data,
                               size_t jpeg_size, const ExifMetadata *meta) {
  if (!filepath || !jpeg_data || jpeg_size < 4 || jpeg_data[0] != 0xFF ||
      jpeg_data[1] != 0xD8) {
    return false;
  }

  uint8_t app1_buf[EXIF_MAX_APP1_BYTES];
  size_t app1_len = 0;
  if (meta) {
    app1_len = exif_build_app1_segment(meta, app1_buf, sizeof(app1_buf));
  }

  char tmp_path[540];
  int written = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", filepath);
  if (written < 0 || (size_t)written >= sizeof(tmp_path)) {
    return false;
  }

  FILE *fp = fopen(tmp_path, "wb");
  if (!fp) {
    return false;
  }

  bool ok = true;
  /* 1. Write SOI marker (0xFF 0xD8) */
  if (fwrite(jpeg_data, 1, 2, fp) != 2) {
    ok = false;
  }

  /* 2. Write Exif APP1 segment immediately after SOI */
  if (ok && app1_len > 0) {
    if (fwrite(app1_buf, 1, app1_len, fp) != app1_len) {
      ok = false;
    }
  }

  /* 3. Write remainder of JPEG stream after SOI */
  if (ok && jpeg_size > 2) {
    size_t rem = jpeg_size - 2;
    if (fwrite(jpeg_data + 2, 1, rem, fp) != rem) {
      ok = false;
    }
  }

  if (fflush(fp) != 0) {
    ok = false;
  }
  fclose(fp);

  if (!ok) {
    unlink(tmp_path);
    return false;
  }

  if (rename(tmp_path, filepath) != 0) {
    unlink(tmp_path);
    return false;
  }

  return true;
}

bool exif_inject_into_jpeg_file(const char *filepath,
                                const ExifMetadata *meta) {
  if (!filepath || !meta) {
    return false;
  }

  FILE *in_fp = fopen(filepath, "rb");
  if (!in_fp) {
    return false;
  }

  if (fseek(in_fp, 0, SEEK_END) != 0) {
    fclose(in_fp);
    return false;
  }
  long file_len = ftell(in_fp);
  if (file_len < 4) {
    fclose(in_fp);
    return false;
  }
  if (fseek(in_fp, 0, SEEK_SET) != 0) {
    fclose(in_fp);
    return false;
  }

  uint8_t *jpeg_buf = (uint8_t *)malloc((size_t)file_len);
  if (!jpeg_buf) {
    fclose(in_fp);
    return false;
  }

  size_t nread = fread(jpeg_buf, 1, (size_t)file_len, in_fp);
  fclose(in_fp);

  if (nread != (size_t)file_len) {
    free(jpeg_buf);
    return false;
  }

  bool ok = exif_write_jpeg_with_app1(filepath, jpeg_buf, nread, meta);
  free(jpeg_buf);
  return ok;
}
