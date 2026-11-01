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

/*
 * Resynchronize current directory and file number counters by rescanning DCIM.
 * Useful after deleting the most recently captured image(s).
 */
void dcf_storage_resync(DcfStorage *dcf);

/*
 * Metadata entry for a single captured media file in DCIM storage.
 */
typedef struct {
  char
      filepath[512]; /* Full relative path, e.g. "DCIM/100_PICA/IMG_0001.JPG" */
  char display_name[512]; /* Short display name, e.g. "100_PICA/IMG_0001.JPG" */
  char filename[256];     /* Base file name, e.g. "IMG_0001.JPG" */
  uint64_t file_size;     /* File size in bytes */
  int64_t mtime_sec;      /* Last modification time (epoch seconds) */
  bool is_protected;      /* True if file is marked read-only (protected) */
} DcfMediaEntry;

/*
 * Dynamic array of scanned DCF media entries, sorted in chronological/DCF
 * order.
 */
typedef struct {
  DcfMediaEntry *entries;
  size_t count;
  size_t capacity;
} DcfMediaList;

/*
 * Scan all subdirectories under dcf->base_path (both standard NNN_PICA and
 * YYYYMMDD directories) and populate out_list with all .JPG/.JPEG files sorted
 * by directory name and file name.
 * Caller must free resources with dcf_storage_free_media_list().
 */
bool dcf_storage_scan_media(const DcfStorage *dcf, DcfMediaList *out_list);

/*
 * Free memory allocated inside a DcfMediaList.
 */
void dcf_storage_free_media_list(DcfMediaList *list);

/*
 * Check if a media file is protected (read-only).
 */
bool dcf_storage_is_protected(const char *filepath);

/*
 * Set or clear protection (read-only flag via chmod) on a media file.
 * Returns true on success, false on error.
 */
bool dcf_storage_set_protected(const char *filepath, bool protect);

/*
 * Delete a single media file if it is not protected.
 * Returns true if deleted, false if protected or unlink failed.
 */
bool dcf_storage_delete_file(DcfStorage *dcf, const char *filepath);

/*
 * Delete all unprotected media files found in the given media list (or scanned
 * from DCIM if list is NULL).
 * Returns the number of files deleted, or -1 on error.
 */
int dcf_storage_delete_all_unprotected(DcfStorage *dcf);

#ifdef __cplusplus
}
#endif

#endif /* DCF_STORAGE_H */
