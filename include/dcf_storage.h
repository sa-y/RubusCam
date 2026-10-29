#ifndef DCF_STORAGE_H
#define DCF_STORAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Default minimum required free storage space before capturing (32 MiB).
 */
#define DCF_DEFAULT_MIN_FREE_BYTES (32ULL * 1024ULL * 1024ULL)

/*
 * Directory organization mode under DCIM/ root.
 */
typedef enum {
  DCF_DIR_MODE_STANDARD = 0, /* Standard DCF: DCIM/100_PICA/IMG_0001.JPG */
  DCF_DIR_MODE_DATE = 1      /* Date-based:   DCIM/YYYYMMDD/IMG_0001.JPG */
} DcfDirMode;

/*
 * DCF storage manager state.
 */
typedef struct {
  char base_path[256];      /* Root directory path, e.g. "DCIM" */
  char dir_suffix[6];       /* 5-char directory suffix, e.g. "_PICA" */
  char file_prefix[5];      /* 4-char file prefix, e.g. "IMG_" */
  DcfDirMode mode;          /* Directory naming mode */
  uint64_t min_free_bytes;  /* Minimum required free bytes on storage */
  int current_dir_num;      /* Active DCF directory number (100..999) */
  int current_file_num;     /* Last assigned file number (0..9999) */
  char current_date_str[9]; /* Active "YYYYMMDD" string for date mode */
} DcfStorage;

/*
 * Initialize DCF storage manager and scan existing DCIM directories to resume
 * numbering from the highest existing directory and file number.
 * If base_path is NULL or empty, defaults to "DCIM".
 * Returns true on success, false on invalid parameters.
 */
bool dcf_storage_init(DcfStorage *dcf, const char *base_path, DcfDirMode mode);

/*
 * Override minimum free space threshold in bytes (default: 32 MiB).
 */
void dcf_storage_set_min_free_bytes(DcfStorage *dcf, uint64_t min_free_bytes);

/*
 * Query available free space in bytes on the filesystem hosting base_path.
 * Returns true on success, false if the filesystem could not be queried.
 */
bool dcf_storage_get_free_space(const DcfStorage *dcf,
                                uint64_t *out_free_bytes);

/*
 * Verify sufficient free disk space, create target DCIM subdirectory if needed,
 * and generate the next non-colliding file path (e.g.
 * "DCIM/100_PICA/IMG_0001.JPG"). Returns true on success, false if disk space
 * is low or limits are exhausted.
 */
bool dcf_storage_next_filepath(DcfStorage *dcf, char *out_path,
                               size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* DCF_STORAGE_H */
