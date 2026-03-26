
#define _BSD_SOURCE

#include <ctype.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <libgen.h>

#if defined(__APPLE__)
#include <sys/xattr.h>
#include <sys/attr.h>
#include <sys/paths.h>
#endif

#ifdef __linux__
#include <sys/xattr.h>
#endif

#if defined(__FreeBSD__)
#include <sys/types.h>
#include <sys/extattr.h>
#endif

#if defined(_AIX)
#include <sys/ea.h>
#endif

#ifndef XATTR_FINDERINFO_NAME
#define XATTR_FINDERINFO_NAME "com.apple.FinderInfo"
#endif

#ifndef XATTR_RESOURCEFORK_NAME
#define XATTR_RESOURCEFORK_NAME "com.apple.ResourceFork"
#endif




#include "defc.h"
#include "gsos.h"

#include "host_common.h"


static ino_t root_ino = 0;
static dev_t root_dev = 0;


unsigned host_startup(void) {

  struct stat st;

  if (!g_cfg_host_path) return invalidFSTop;
  if (!*g_cfg_host_path) return invalidFSTop;
  if (host_root) free(host_root);
  host_root = strdup(g_cfg_host_path);

  if (stat(host_root, &st) < 0) {
    fprintf(stderr, "%s does not exist\n", host_root);
    return invalidFSTop;
  }
  if (!S_ISDIR(st.st_mode)) {
    fprintf(stderr, "%s is not a directory\n", host_root);
    return invalidFSTop;
  }

  root_ino = st.st_ino;
  root_dev = st.st_dev;

  return 0;
}

void host_shutdown(void) {
  if (host_root) free(host_root);
  host_root = NULL;
  root_ino = 0;
  root_dev = 0;
}

int host_is_root(struct stat *st) {
  return st->st_ino == root_ino && st->st_dev == root_dev;
}


/*
 * Date/time conversion
 */

/*
 * converts time_t to a gs/os readhextime date/time record.
 */

void host_set_date_time_rec(word32 ptr, time_t time) {

  if (time == 0) {
    for (int i = 0; i < 8; ++i) set_memory_c(ptr++, 0, 0);
    return;
  }

  struct tm *tm = localtime(&time);
  if (tm->tm_sec == 60) tm->tm_sec = 59;       /* leap second */

  set_memory_c(ptr++, tm->tm_sec, 0);
  set_memory_c(ptr++, tm->tm_min, 0);
  set_memory_c(ptr++, tm->tm_hour, 0);
  set_memory_c(ptr++, tm->tm_year, 0);
  set_memory_c(ptr++, tm->tm_mday - 1, 0);
  set_memory_c(ptr++, tm->tm_mon, 0);
  set_memory_c(ptr++, 0, 0);
  set_memory_c(ptr++, tm->tm_wday + 1, 0);
}

/*
 * converts time_t to a prodos16 date/time record.
 */
void host_set_date_time(word32 ptr, time_t time) {

  if (time == 0) {
    for (int i = 0; i < 4; ++i) set_memory_c(ptr++, 0, 0);
    return;
  }

  struct tm *tm = localtime(&time);

  word16 tmp = 0;
  tmp |= (tm->tm_year % 100) << 9;
  tmp |= (tm->tm_mon + 1) << 5;
  tmp |= tm->tm_mday;

  set_memory16_c(ptr, tmp, 0);
  ptr += 2;

  tmp = 0;
  tmp |= tm->tm_hour << 8;
  tmp |= tm->tm_min;
  set_memory16_c(ptr, tmp, 0);
}

word32 host_convert_date_time(time_t time) {

  if (time == 0) return 0;

  struct tm *tm = localtime(&time);

  word16 dd = 0;
  dd |= (tm->tm_year % 100) << 9;
  dd |= (tm->tm_mon + 1) << 5;
  dd |= tm->tm_mday;

  word16 tt = 0;
  tt |= tm->tm_hour << 8;
  tt |= tm->tm_min;


  return (tt << 16) | dd;
}


time_t host_get_date_time(word32 ptr) {

  word16 a = get_memory16_c(ptr + 0, 0);
  word16 b = get_memory16_c(ptr + 2, 0);
  if (!a && !b) return 0;

  struct tm tm;
  memset(&tm, 0, sizeof(tm));

  tm.tm_year = (a >> 9) & 0x7f;
  tm.tm_mon = ((a >> 5) & 0x0f) - 1;
  tm.tm_mday = (a >> 0) & 0x1f;

  tm.tm_hour = (b >> 8) & 0x1f;
  tm.tm_min = (b >> 0) & 0x3f;
  tm.tm_sec = 0;

  tm.tm_isdst = -1;

  // 00 - 39 => 2000-2039
  // 40 - 99 => 1940-1999
  if (tm.tm_year < 40) tm.tm_year += 100;


  return mktime(&tm);
}

time_t host_get_date_time_rec(word32 ptr) {

  byte buffer[8];
  for (int i = 0; i < 8; ++i) buffer[i] = get_memory_c(ptr++, 0);

  if (!memcmp(buffer, "\x00\x00\x00\x00\x00\x00\x00\x00", 8)) return 0;

  struct tm tm;
  memset(&tm, 0, sizeof(tm));

  tm.tm_sec = buffer[0];
  tm.tm_min = buffer[1];
  tm.tm_hour = buffer[2];
  tm.tm_year = buffer[3];
  tm.tm_mday = buffer[4] + 1;
  tm.tm_mon = buffer[5];
  tm.tm_isdst = -1;

  return mktime(&tm);
}



#if defined(__APPLE__)
void host_get_file_xinfo(const char *path, struct file_info *fi) {

  ssize_t tmp;
  tmp = getxattr(path, XATTR_RESOURCEFORK_NAME, NULL, 0, 0, 0);
  if (tmp < 0) tmp = 0;
  fi->resource_eof = tmp;
  fi->resource_blocks = (tmp + 511) / 512;

  tmp = getxattr(path, XATTR_FINDERINFO_NAME, fi->finder_info, 32, 0, 0);
  if (tmp == 16 || tmp == 32) {
    fi->has_fi = 1;

    host_finder_info_to_filetype(fi->finder_info, &fi->file_type, &fi->aux_type);
  }
}
#elif defined(__sun)
void host_get_file_xinfo(const char *path, struct file_info *fi) {

  struct stat st;

  // can't stat an xattr directly?
  int fd;
  fd = attropen(path, XATTR_RESOURCEFORK_NAME, O_RDONLY);
  if (fd >= 0) {
    if (fstat(fd, &st) == 0) {
      fi->resource_eof = st.st_size;
      fi->resource_blocks = st.st_blocks;
    }
    close(fd);
  }

  fd = attropen(path, XATTR_FINDERINFO_NAME, O_RDONLY);
  if (fd >= 0) {
    int tmp = read(fd, fi->finder_info, 32);
    if (tmp == 16 || tmp == 32) {
      fi->has_fi = 1;
      host_finder_info_to_filetype(fi->finder_info, &fi->file_type, &fi->aux_type);
    }
    close(fd);
  }
}
#elif defined(__linux__)
void host_get_file_xinfo(const char *path, struct file_info *fi) {

  ssize_t tmp;
  struct stat st;
  char rpath[1024] = {0};
  char *pos=strrchr(path,'/');
  if (pos) {
    strncpy(rpath, path, (pos-path)+1);
  } else {
    pos = (char *)path - 1;
  }
  strcat(rpath, "._");
  strcat(rpath, pos+1);

  int ok = stat(rpath, &st);
  if (ok < 0) {
      fi->resource_eof = 0;
      fi->resource_blocks = 0;
  } else {
      fi->resource_eof = st.st_size;
      fi->resource_blocks = st.st_blocks;
  }

  tmp = getxattr(path, "user.com.apple.FinderInfo", fi->finder_info, 32);
  if (tmp == 16 || tmp == 32) {
    fi->has_fi = 1;

    host_finder_info_to_filetype(fi->finder_info, &fi->file_type, &fi->aux_type);
  }
}
#else
void host_get_file_xinfo(const char *path, struct file_info *fi) {
}
#endif



word32 host_get_file_info(const char *path, struct file_info *fi) {
  struct stat st;
  memset(fi, 0, sizeof(*fi));

  int ok = stat(path, &st);
  if (ok < 0) return host_map_errno(errno);

  fi->eof = st.st_size;
  fi->blocks = st.st_blocks;

  fi->create_date = st.st_ctime;
  fi->modified_date = st.st_mtime;

#if defined(__APPLE__)
  fi->create_date = st.st_birthtime;
#endif


  if (S_ISDIR(st.st_mode)) {
    fi->storage_type = directoryFile;
    fi->file_type = 0x0f;
    if (host_is_root(&st))
      fi->storage_type = 0x0f;
  } else if (S_ISREG(st.st_mode)) {
    fi->file_type = 0x06;
    if (st.st_size < 0x200) fi->storage_type = seedling;
    else if (st.st_size < 0x20000) fi->storage_type = sapling;
    else fi->storage_type = tree;
  } else {
    fi->storage_type = st.st_mode & S_IFMT;
    fi->file_type = 0;
  }
  // 0x01 = read enable
  // 0x02 = write enable
  // 0x04 = invisible
  // 0x08 = reserved
  // 0x10 = reserved
  // 0x20 = backup needed
  // 0x40 = rename enable
  // 0x80 = destroy enable

  fi->access = 0xc3;       // placeholder...

  if (S_ISREG(st.st_mode)) {
    host_get_file_xinfo(path, fi);

    if (!fi->has_fi) {
    	host_synthesize_file_xinfo(path, fi);
    }
  }

  // get file type/aux type

  if (fi->resource_eof) fi->storage_type = extendedFile;

  return 0;
}




#if defined(__APPLE__)
word32 host_set_file_info(const char *path, struct file_info *fi) {

  int ok;
  struct attrlist list;
  unsigned i = 0;
  struct timespec dates[2];

  if (fi->has_fi && fi->storage_type != 0x0d) {
    ok = setxattr(path, XATTR_FINDERINFO_NAME, fi->finder_info, 32, 0, 0);
    if (ok < 0) return host_map_errno(errno);
  }


  memset(&list, 0, sizeof(list));
  memset(dates, 0, sizeof(dates));

  list.bitmapcount = ATTR_BIT_MAP_COUNT;
  list.commonattr  = 0;

  if (fi->create_date)
  {
    dates[i++].tv_sec = fi->create_date;
    list.commonattr |= ATTR_CMN_CRTIME;
  }

  if (fi->modified_date)
  {
    dates[i++].tv_sec = fi->modified_date;
    list.commonattr |= ATTR_CMN_MODTIME;
  }

  ok = 0;
  if (i) ok = setattrlist(path, &list, dates, i * sizeof(struct timespec), 0);
  return 0;
}
#elif defined(__sun)
word32 host_set_file_info(const char *path, struct file_info *fi) {

  if (fi->has_fi && fi->storage_type != 0x0d) {
    int fd = attropen(path, XATTR_FINDERINFO_NAME, O_WRONLY | O_CREAT, 0666);
    if (fd < 0) return host_map_errno(errno);
    write(fd, fi->finder_info, 32);
    close(fd);
  }

  if (fi->modified_date) {
    struct timeval times[2];

    memset(times, 0, sizeof(times));

    //times[0] = 0; // access
    times[1].tv_sec = fi.modified_date;             // modified
    int ok = utimes(path, times);
    if (ok < 0) return host_map_errno(errno);
  }
  return 0;
}
#elif defined(__linux__)
word32 host_set_file_info(const char *path, struct file_info *fi) {

  if (fi->has_fi && fi->storage_type != 0x0d) {
    int ok = setxattr(path, "user.com.apple.FinderInfo", fi->finder_info, 32, 0);
    if (ok < 0) return host_map_errno(errno);
  }

  if (fi->modified_date) {
    struct timeval times[2];

    memset(times, 0, sizeof(times));

    //times[0] = 0; // access
    times[1].tv_sec = fi->modified_date;             // modified
    int ok = utimes(path, times);
    if (ok < 0) return host_map_errno(errno);
  }
  return 0;
}

#else

word32 host_set_file_info(const char *path, struct file_info *fi) {

  if (fi->modified_date) {

    struct timeval times[2];

    memset(times, 0, sizeof(times));

    times[0] = 0;             // access
    times[1].tv_sec = fi->modified_date;             // modified

    int ok = utimes(path, times);
    if (ok < 0) return host_map_errno(errno);
  }
  return 0;
}

#endif


static int qsort_callback(const void *a, const void *b) {
  return strcasecmp(*(const char **)a, *(const char **)b);
}

unsigned host_scan_directory(const char *path, char ***out, size_t *entries, unsigned p8) {

  DIR *dp;
  char **data = NULL;
  size_t capacity = 0;
  size_t count = 0;

  dp = opendir(path);
  if (!dp) return host_map_errno_path(errno, path);

  for(;;) {
    struct dirent *d = readdir(dp);
    if (!d) break;

    const char *name = d->d_name;

    if (name[0] == 0) continue;
    if (name[0] == '.') continue;
    if (p8) {
      int ok = 1;
      int n = strlen(name);
      if (n > 15) continue;
      /* check for invalid characters? */
      for (int i = 0; i < n; ++i) {
        unsigned char c = name[i];
        if (isalpha(c) || isdigit(c) || c == '.') continue;
        ok = 0;
        break;
      }
      if (!ok) continue;
    }
    if (count == capacity) {
      char **tmp;
      tmp = realloc(data, (capacity + 100) * sizeof(char *));
      if (!tmp) {
        closedir(dp);
        host_free_directory(data, count);
        return outOfMem;
      }
      data = tmp;
      for (int i = count; i < capacity; ++i) data[i] = 0;
      capacity += 100;
    }
    data[count++] = strdup(name);
  }
  closedir(dp);

  qsort(data, count, sizeof(char *), qsort_callback);
  *entries = count;
  *out = data;

  return 0;
}

unsigned host_storage_type(const char *path, word16 *error) {
  struct stat st;
  if (!path) {
    *error = badPathSyntax;
    return 0;
  }
  if (stat(path, &st) < 0) {
    *error = host_map_errno_path(errno, path);
    return 0;
  }
  if (S_ISDIR(st.st_mode)) {
    return host_is_root(&st) ? 0x0f : directoryFile;
  }
  if (S_ISREG(st.st_mode)) return standardFile;
  *error = badStoreType;
  return 0;
}


/*
 * error remapping.
 * NOTE - GS/OS errors are a superset of P8 errors
 */

static word32 enoent(const char *path) {
  /*
          some op on path return ENOENT. check if it's
          fileNotFound or pathNotFound
   */
  char *p = (char *)path;
  for(;;) {
    struct stat st;
    p = dirname(p);
    if (p == NULL) break;
    if (p[0] == '.' && p[1] == 0) break;
    if (p[0] == '/' && p[1] == 0) break;
    if (stat(p, &st) < 0) return pathNotFound;
  }
  return fileNotFound;
}

word32 host_map_errno(int xerrno) {
  switch(xerrno) {
    case 0: return 0;
    case EBADF:
      return invalidAccess;
#ifdef EDQUOT
    case EDQUOT:
#endif
    case EFBIG:
      return volumeFull;
    case ENOENT:
      return fileNotFound;
    case ENOTDIR:
      return pathNotFound;
    case ENOMEM:
      return outOfMem;
    case EEXIST:
      return dupPathname;
    case ENOTEMPTY:
      return invalidAccess;

    default:
      return drvrIOError;
  }
}

word32 host_map_errno_path(int xerrno, const char *path) {
  if (xerrno == ENOENT) return enoent(path);
  return host_map_errno(xerrno);
}

