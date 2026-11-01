#ifndef EXIF_WRITER_H
#define EXIF_WRITER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "camera_stream.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Maximum size in bytes of the generated JPEG APP1 (Exif 2.3) segment.
 */
#define EXIF_MAX_APP1_BYTES 1024

/*
 * Exif 2.3 metadata fields to embed into a captured JPEG file.
 */
typedef struct ExifMetadata {
  char make[64];          /* Camera manufacturer, e.g. "Raspberry Pi" */
  char model[64];         /* Camera sensor/model name, e.g. "RP_imx708" */
  char software[64];      /* Application name, e.g. "RubusCam" */
  char datetime_str[20];  /* Capture timestamp "YYYY:MM:DD HH:MM:SS" */
  int width;              /* Image width in pixels */
  int height;             /* Image height in pixels */
  int32_t exposure_us;    /* Exposure time in microseconds */
  int iso_value;          /* ISO sensitivity (e.g. 100 .. 3200) */
  float ev_bias;          /* Exposure compensation in EV (-3.0 .. +3.0) */
  float f_number;         /* Lens aperture F-number (e.g. 1.8) */
  float focal_length_mm;  /* Lens focal length in mm (e.g. 4.74) */
  uint16_t metering_mode; /* Exif MeteringMode (0x9207): 2=Center, 3=Spot,
                             5=Pattern */
  uint16_t
      light_source; /* Exif LightSource (0x9208): 0=Unknown, 1=Daylight, etc. */
  uint16_t wb_mode; /* White balance mode: 0 = Auto, 1 = Manual */
  uint16_t exp_mode; /* Exposure mode: 0 = Auto, 1 = Manual */
} ExifMetadata;

/*
 * Populate ExifMetadata with current timestamp, camera sensor optical defaults,
 * and live exposure parameters from CameraExposureStatus.
 */
void exif_metadata_init(ExifMetadata *meta, const char *camera_model, int width,
                        int height, const CameraExposureStatus *exp_status);

/*
 * Build a complete JPEG APP1 (0xFFE1) Exif 2.3 segment in out_buf.
 * Returns the number of bytes written to out_buf, or 0 on error.
 */
size_t exif_build_app1_segment(const ExifMetadata *meta, uint8_t *out_buf,
                               size_t out_buf_capacity);

/*
 * Write a JPEG buffer (already encoded in memory starting with 0xFFD8) to
 * filepath while injecting the Exif APP1 segment immediately after the SOI
 * marker. Writes via a temporary file and renames atomically.
 * Returns true on success, false on failure.
 */
bool exif_write_jpeg_with_app1(const char *filepath, const uint8_t *jpeg_data,
                               size_t jpeg_size, const ExifMetadata *meta);

/*
 * Inject an Exif APP1 segment into an existing JPEG file on disk atomically.
 * Returns true on success, false on failure.
 */
bool exif_inject_into_jpeg_file(const char *filepath, const ExifMetadata *meta);

#ifdef __cplusplus
}
#endif

#endif /* EXIF_WRITER_H */
