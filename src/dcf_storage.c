#include "dcf_storage.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#define DCF_DIR_MIN 100
#define DCF_DIR_MAX 999
#define DCF_FILE_MIN 1
#define DCF_FILE_MAX 9999

static bool path_exists(const char *path) {
  struct stat st;
  return (stat(path, &st) == 0);
}

static bool is_directory(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0) {
    return false;
  }
  return S_ISDIR(st.st_mode);
}

/*
 * Create directory and any missing parent directories (equivalent to mkdir -p).
 */
static bool ensure_directory_exists(const char *path) {
  if (!path || path[0] == '\0') {
    return false;
  }
  if (is_directory(path)) {
    return true;
  }

  char tmp[512];
  size_t len = strlen(path);
  if (len >= sizeof(tmp)) {
    return false;
  }
  memcpy(tmp, path, len + 1);

  for (size_t i = 1; i < len; ++i) {
    if (tmp[i] == '/') {
      char saved = tmp[i];
      tmp[i] = '\0';
      if (tmp[0] != '\0' && !is_directory(tmp)) {
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
          return false;
        }
      }
      tmp[i] = saved;
    }
  }

  if (!is_directory(tmp)) {
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
      return false;
    }
  }
  return true;
}

static void get_current_date_str(char *out_buf, size_t buf_size) {
  time_t now = time(NULL);
  struct tm tm_buf;
  localtime_r(&now, &tm_buf);
  strftime(out_buf, buf_size, "%Y%m%d", &tm_buf);
}

/*
 * Parse a DCF file name matching "<prefix><4-digit-number>.JPG"
 * (case-insensitive). Returns the 1..9999 file number if matched, or -1
 * otherwise.
 */
static int parse_dcf_filename(const char *name, const char *prefix) {
  size_t prefix_len = strlen(prefix);
  if (strncmp(name, prefix, prefix_len) != 0) {
    return -1;
  }

  const char *digits = name + prefix_len;
  for (int i = 0; i < 4; ++i) {
    if (!isdigit((unsigned char)digits[i])) {
      return -1;
    }
  }

  const char *ext = digits + 4;
  if (ext[0] != '.') {
    return -1;
  }

  /* Match .JPG or .JPEG (case-insensitive) */
  if ((toupper((unsigned char)ext[1]) == 'J' &&
       toupper((unsigned char)ext[2]) == 'P' &&
       toupper((unsigned char)ext[3]) == 'G' && ext[4] == '\0') ||
      (toupper((unsigned char)ext[1]) == 'J' &&
       toupper((unsigned char)ext[2]) == 'P' &&
       toupper((unsigned char)ext[3]) == 'E' &&
       toupper((unsigned char)ext[4]) == 'G' && ext[5] == '\0')) {
    int num = (digits[0] - '0') * 1000 + (digits[1] - '0') * 100 +
              (digits[2] - '0') * 10 + (digits[3] - '0');
    if (num >= DCF_FILE_MIN && num <= DCF_FILE_MAX) {
      return num;
    }
  }

  return -1;
}

/*
 * Parse a DCF standard directory name matching
 * "<3-digit-number><5-char-suffix>". Returns the 100..999 directory number if
 * matched, or -1 otherwise.
 */
static int parse_dcf_dirname(const char *name, const char *suffix) {
  if (strlen(name) != 8) {
    return -1;
  }
  if (!isdigit((unsigned char)name[0]) || !isdigit((unsigned char)name[1]) ||
      !isdigit((unsigned char)name[2])) {
    return -1;
  }
  if (strncmp(name + 3, suffix, 5) != 0) {
    return -1;
  }

  int dir_num = (name[0] - '0') * 100 + (name[1] - '0') * 10 + (name[2] - '0');
  if (dir_num >= DCF_DIR_MIN && dir_num <= DCF_DIR_MAX) {
    return dir_num;
  }
  return -1;
}

/*
 * Scan a directory for the highest existing DCF file number
 * (IMG_0001..IMG_9999).
 */
static int scan_max_file_number(const char *dir_path, const char *file_prefix) {
  int max_num = 0;

  DIR *dir = opendir(dir_path);
  if (!dir) {
    return 0;
  }
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    int num = parse_dcf_filename(entry->d_name, file_prefix);
    if (num > max_num) {
      max_num = num;
    }
  }
  closedir(dir);

  return max_num;
}

/*
 * Scan base_path (e.g. "DCIM") for the highest DCF directory number (100..999).
 */
static int scan_max_dir_number(const char *base_path, const char *dir_suffix) {
  int max_dir = DCF_DIR_MIN;

  DIR *dir = opendir(base_path);
  if (!dir) {
    return DCF_DIR_MIN;
  }
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    int dnum = parse_dcf_dirname(entry->d_name, dir_suffix);
    if (dnum > max_dir) {
      max_dir = dnum;
    }
  }
  closedir(dir);

  return max_dir;
}

bool dcf_storage_init(DcfStorage *dcf, const char *base_path, DcfDirMode mode) {
  if (!dcf) {
    return false;
  }

  memset(dcf, 0, sizeof(*dcf));

  const char *root = (base_path && base_path[0] != '\0') ? base_path : "DCIM";
  snprintf(dcf->base_path, sizeof(dcf->base_path), "%s", root);
  snprintf(dcf->dir_suffix, sizeof(dcf->dir_suffix), "_PICA");
  snprintf(dcf->file_prefix, sizeof(dcf->file_prefix), "IMG_");
  dcf->mode = mode;
  dcf->min_free_bytes = DCF_DEFAULT_MIN_FREE_BYTES;
  dcf->current_dir_num = DCF_DIR_MIN;
  dcf->current_file_num = 0;

  get_current_date_str(dcf->current_date_str, sizeof(dcf->current_date_str));

  if (is_directory(dcf->base_path)) {
    if (dcf->mode == DCF_DIR_MODE_STANDARD) {
      dcf->current_dir_num =
          scan_max_dir_number(dcf->base_path, dcf->dir_suffix);
      char active_dir[512];
      snprintf(active_dir, sizeof(active_dir), "%s/%03d%s", dcf->base_path,
               dcf->current_dir_num, dcf->dir_suffix);
      if (is_directory(active_dir)) {
        dcf->current_file_num =
            scan_max_file_number(active_dir, dcf->file_prefix);
      }
    } else {
      char date_dir[512];
      snprintf(date_dir, sizeof(date_dir), "%s/%s", dcf->base_path,
               dcf->current_date_str);
      if (is_directory(date_dir)) {
        dcf->current_file_num =
            scan_max_file_number(date_dir, dcf->file_prefix);
      }
    }
  }

  return true;
}

void dcf_storage_set_min_free_bytes(DcfStorage *dcf, uint64_t min_free_bytes) {
  if (!dcf) {
    return;
  }
  dcf->min_free_bytes = min_free_bytes;
}

bool dcf_storage_get_free_space(const DcfStorage *dcf,
                                uint64_t *out_free_bytes) {
  if (!dcf || !out_free_bytes) {
    return false;
  }

  /* Check base_path if it already exists, otherwise check current working
   * directory */
  const char *probe_path = is_directory(dcf->base_path) ? dcf->base_path : ".";

  struct statvfs vfs;
  if (statvfs(probe_path, &vfs) != 0) {
    return false;
  }
  *out_free_bytes = (uint64_t)vfs.f_bavail * (uint64_t)vfs.f_frsize;
  return true;
}

bool dcf_storage_next_filepath(DcfStorage *dcf, char *out_path,
                               size_t out_size) {
  if (!dcf || !out_path || out_size == 0) {
    return false;
  }

  /* 1. Pre-check available storage space */
  uint64_t free_bytes = 0;
  if (dcf_storage_get_free_space(dcf, &free_bytes)) {
    if (free_bytes < dcf->min_free_bytes) {
      fprintf(stderr,
              "[エラー] ストレージの空き容量が不足しています (空き: %.1f MiB, "
              "必要: %.1f MiB)。\n",
              (double)free_bytes / (1024.0 * 1024.0),
              (double)dcf->min_free_bytes / (1024.0 * 1024.0));
      return false;
    }
  } else {
    fprintf(stderr,
            "[警告] "
            "ストレージ残容量の取得に失敗しました。保存処理を続行します。\n");
  }

  /* 2. Handle date rollover in DCF_DIR_MODE_DATE */
  if (dcf->mode == DCF_DIR_MODE_DATE) {
    char today[9];
    get_current_date_str(today, sizeof(today));
    if (strcmp(today, dcf->current_date_str) != 0) {
      snprintf(dcf->current_date_str, sizeof(dcf->current_date_str), "%s",
               today);
      char date_dir[512];
      snprintf(date_dir, sizeof(date_dir), "%s/%s", dcf->base_path,
               dcf->current_date_str);
      dcf->current_file_num =
          is_directory(date_dir)
              ? scan_max_file_number(date_dir, dcf->file_prefix)
              : 0;
    }
  }

  /* 3. Find next non-colliding file number and ensure directory exists */
  for (int attempts = 0; attempts < 10000; ++attempts) {
    int next_file = dcf->current_file_num + 1;

    if (next_file > DCF_FILE_MAX) {
      if (dcf->mode == DCF_DIR_MODE_STANDARD) {
        if (dcf->current_dir_num >= DCF_DIR_MAX) {
          fprintf(stderr,
                  "[エラー] DCF ディレクトリ番号の上限 (%d) "
                  "およびファイル番号上限 (%d) に達しました。\n",
                  DCF_DIR_MAX, DCF_FILE_MAX);
          return false;
        }
        dcf->current_dir_num++;
        dcf->current_file_num = 0;
        next_file = 1;
      } else {
        fprintf(stderr,
                "[エラー] 本日のフォルダ (%s) 内のファイル番号上限 (%d) "
                "に達しました。\n",
                dcf->current_date_str, DCF_FILE_MAX);
        return false;
      }
    }

    char dir_path[512];
    if (dcf->mode == DCF_DIR_MODE_STANDARD) {
      snprintf(dir_path, sizeof(dir_path), "%s/%03d%s", dcf->base_path,
               dcf->current_dir_num, dcf->dir_suffix);
    } else {
      snprintf(dir_path, sizeof(dir_path), "%s/%s", dcf->base_path,
               dcf->current_date_str);
    }

    if (!ensure_directory_exists(dir_path)) {
      fprintf(stderr, "[エラー] 保存先ディレクトリの作成に失敗しました: %s\n",
              dir_path);
      return false;
    }

    char candidate[512];
    int written = snprintf(candidate, sizeof(candidate), "%s/%s%04d.JPG",
                           dir_path, dcf->file_prefix, next_file);
    if (written < 0 || (size_t)written >= sizeof(candidate) ||
        (size_t)written >= out_size) {
      fprintf(stderr,
              "[エラー] ファイルパスバッファのサイズが不足しています。\n");
      return false;
    }

    dcf->current_file_num = next_file;

    /* Check for collision in case files were added externally */
    if (!path_exists(candidate)) {
      memcpy(out_path, candidate, (size_t)written + 1);
      return true;
    }
  }

  fprintf(stderr,
          "[エラー] 利用可能なDCFファイル番号が見つかりませんでした。\n");
  return false;
}

void dcf_storage_resync(DcfStorage *dcf) {
  if (!dcf) {
    return;
  }

  dcf->current_dir_num = DCF_DIR_MIN;
  dcf->current_file_num = 0;

  if (!is_directory(dcf->base_path)) {
    return;
  }

  if (dcf->mode == DCF_DIR_MODE_STANDARD) {
    dcf->current_dir_num = scan_max_dir_number(dcf->base_path, dcf->dir_suffix);
    char active_dir[512];
    snprintf(active_dir, sizeof(active_dir), "%s/%03d%s", dcf->base_path,
             dcf->current_dir_num, dcf->dir_suffix);
    if (is_directory(active_dir)) {
      dcf->current_file_num =
          scan_max_file_number(active_dir, dcf->file_prefix);
    }
  } else {
    get_current_date_str(dcf->current_date_str, sizeof(dcf->current_date_str));
    char date_dir[512];
    snprintf(date_dir, sizeof(date_dir), "%s/%s", dcf->base_path,
             dcf->current_date_str);
    if (is_directory(date_dir)) {
      dcf->current_file_num = scan_max_file_number(date_dir, dcf->file_prefix);
    }
  }
}

static bool has_jpeg_extension(const char *name) {
  if (!name) {
    return false;
  }
  const char *dot = strrchr(name, '.');
  if (!dot) {
    return false;
  }
  if (toupper((unsigned char)dot[1]) == 'J' &&
      toupper((unsigned char)dot[2]) == 'P' &&
      toupper((unsigned char)dot[3]) == 'G' && dot[4] == '\0') {
    return true;
  }
  if (toupper((unsigned char)dot[1]) == 'J' &&
      toupper((unsigned char)dot[2]) == 'P' &&
      toupper((unsigned char)dot[3]) == 'E' &&
      toupper((unsigned char)dot[4]) == 'G' && dot[5] == '\0') {
    return true;
  }
  return false;
}

static bool media_list_push(DcfMediaList *list, const DcfMediaEntry *entry) {
  if (list->count >= list->capacity) {
    size_t new_cap = list->capacity == 0 ? 32 : list->capacity * 2;
    DcfMediaEntry *new_arr = (DcfMediaEntry *)realloc(
        list->entries, new_cap * sizeof(DcfMediaEntry));
    if (!new_arr) {
      return false;
    }
    list->entries = new_arr;
    list->capacity = new_cap;
  }
  list->entries[list->count++] = *entry;
  return true;
}

static int compare_media_entries(const void *a, const void *b) {
  const DcfMediaEntry *ea = (const DcfMediaEntry *)a;
  const DcfMediaEntry *eb = (const DcfMediaEntry *)b;
  int cmp = strcmp(ea->display_name, eb->display_name);
  if (cmp != 0) {
    return cmp;
  }
  return strcmp(ea->filepath, eb->filepath);
}

bool dcf_storage_is_protected(const char *filepath) {
  if (!filepath || filepath[0] == '\0') {
    return false;
  }
  struct stat st;
  if (stat(filepath, &st) != 0) {
    return false;
  }
  /* Consider protected if owner write permission bit (S_IWUSR) is cleared */
  return (st.st_mode & S_IWUSR) == 0;
}

bool dcf_storage_set_protected(const char *filepath, bool protect) {
  if (!filepath || filepath[0] == '\0') {
    return false;
  }
  struct stat st;
  if (stat(filepath, &st) != 0) {
    return false;
  }

  mode_t new_mode = st.st_mode;
  if (protect) {
    new_mode &= ~(mode_t)(S_IWUSR | S_IWGRP | S_IWOTH);
    /* Ensure read bits remain set */
    new_mode |= (mode_t)(S_IRUSR | S_IRGRP | S_IROTH);
  } else {
    new_mode |= (mode_t)(S_IWUSR | S_IRUSR | S_IRGRP | S_IROTH);
  }

  if (chmod(filepath, new_mode) != 0) {
    fprintf(stderr, "[エラー] プロテクト属性の変更に失敗しました (%s): %s\n",
            filepath, strerror(errno));
    return false;
  }
  return true;
}

bool dcf_storage_scan_media(const DcfStorage *dcf, DcfMediaList *out_list) {
  if (!out_list) {
    return false;
  }
  memset(out_list, 0, sizeof(*out_list));

  const char *base_path =
      (dcf && dcf->base_path[0] != '\0') ? dcf->base_path : "DCIM";
  if (!is_directory(base_path)) {
    return true; /* Empty list when DCIM does not exist yet */
  }

  DIR *root_dir = opendir(base_path);
  if (!root_dir) {
    return false;
  }

  struct dirent *sub_entry;
  while ((sub_entry = readdir(root_dir)) != NULL) {
    if (sub_entry->d_name[0] == '.') {
      continue;
    }

    char subdir_path[512];
    int sub_written = snprintf(subdir_path, sizeof(subdir_path), "%s/%s",
                               base_path, sub_entry->d_name);
    if (sub_written < 0 || (size_t)sub_written >= sizeof(subdir_path)) {
      continue;
    }

    if (!is_directory(subdir_path)) {
      continue;
    }

    DIR *sub_dir = opendir(subdir_path);
    if (!sub_dir) {
      continue;
    }

    struct dirent *file_entry;
    while ((file_entry = readdir(sub_dir)) != NULL) {
      if (file_entry->d_name[0] == '.') {
        continue;
      }
      if (!has_jpeg_extension(file_entry->d_name)) {
        continue;
      }

      char full_path[512];
      int fp_written = snprintf(full_path, sizeof(full_path), "%s/%s",
                                subdir_path, file_entry->d_name);
      if (fp_written < 0 || (size_t)fp_written >= sizeof(full_path)) {
        continue;
      }

      struct stat st;
      if (stat(full_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        continue;
      }

      DcfMediaEntry item;
      memset(&item, 0, sizeof(item));
      snprintf(item.filepath, sizeof(item.filepath), "%s", full_path);
      int dn_written = snprintf(item.display_name, sizeof(item.display_name),
                                "%s/%s", sub_entry->d_name, file_entry->d_name);
      int fn_written = snprintf(item.filename, sizeof(item.filename), "%s",
                                file_entry->d_name);
      if (dn_written < 0 || (size_t)dn_written >= sizeof(item.display_name) ||
          fn_written < 0 || (size_t)fn_written >= sizeof(item.filename)) {
        continue;
      }
      item.file_size = (uint64_t)st.st_size;
      item.mtime_sec = (int64_t)st.st_mtime;
      item.is_protected = (st.st_mode & S_IWUSR) == 0;

      if (!media_list_push(out_list, &item)) {
        closedir(sub_dir);
        closedir(root_dir);
        dcf_storage_free_media_list(out_list);
        return false;
      }
    }

    closedir(sub_dir);
  }

  closedir(root_dir);

  if (out_list->count > 1) {
    qsort(out_list->entries, out_list->count, sizeof(DcfMediaEntry),
          compare_media_entries);
  }

  return true;
}

void dcf_storage_free_media_list(DcfMediaList *list) {
  if (!list) {
    return;
  }
  free(list->entries);
  list->entries = NULL;
  list->count = 0;
  list->capacity = 0;
}

bool dcf_storage_delete_file(DcfStorage *dcf, const char *filepath) {
  if (!filepath || filepath[0] == '\0') {
    return false;
  }

  if (dcf_storage_is_protected(filepath)) {
    fprintf(stderr, "[警告] 保護された画像は削除できません: %s\n", filepath);
    return false;
  }

  if (unlink(filepath) != 0) {
    fprintf(stderr, "[エラー] ファイルの削除に失敗しました (%s): %s\n",
            filepath, strerror(errno));
    return false;
  }

  if (dcf) {
    dcf_storage_resync(dcf);
  }
  return true;
}

int dcf_storage_delete_all_unprotected(DcfStorage *dcf) {
  DcfMediaList list;
  if (!dcf_storage_scan_media(dcf, &list)) {
    return -1;
  }

  int deleted_count = 0;
  for (size_t i = 0; i < list.count; ++i) {
    if (list.entries[i].is_protected) {
      continue;
    }
    if (unlink(list.entries[i].filepath) == 0) {
      deleted_count++;
    }
  }

  dcf_storage_free_media_list(&list);

  if (dcf && deleted_count > 0) {
    dcf_storage_resync(dcf);
  }
  return deleted_count;
}
