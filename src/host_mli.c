
#define _BSD_SOURCE

#ifdef _WIN32
#include <Windows.h>
#else

#include <dirent.h>
#include <fcntl.h>
#include <libgen.h>
#include <time.h>
#include <unistd.h>

#endif

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <string.h>


#include "defc.h"
#include "gsos.h"
#include "fst.h"


#include "host_common.h"


#define LEVEL 0xBFD8                            // current file level
#define DEVNUM 0xBF30                           // last slot / drive
#define DEVCNT 0xBF31                           // count - 1
#define DEVLST 0xBF32                           // active device list
#define PFIXPTR 0xbf9a                          // active prefix?

/* bf97 is appletalk prefix? */


#define ENTRY_LENGTH 0x27
#define ENTRIES_PER_BLOCK 0x0d


static unsigned saved_call;
static unsigned saved_dcb;
static unsigned saved_p;
static unsigned saved_patch_address;
static unsigned saved_mli_address;
static unsigned saved_unit;
static char *saved_prefix;



enum {
  CREATE = 0xc0,
  DESTROY = 0xc1,
  RENAME = 0xc2,
  SET_FILE_INFO = 0xc3,
  GET_FILE_INFO = 0xc4,
  ONLINE = 0xc5,
  SET_PREFIX = 0xc6,
  GET_PREFIX = 0xc7,
  OPEN = 0xc8,
  NEWLINE = 0xc9,
  READ = 0xca,
  WRITE = 0xcb,
  CLOSE = 0xcc,
  FLUSH = 0xcd,
  SET_MARK = 0xce,
  GET_MARK = 0xcf,
  SET_EOF = 0xd0,
  GET_EOF = 0xd1,
  SET_BUF = 0xd2,
  GET_BUF = 0xd3,
  ALLOC_INTERRUPT = 0x40,
  DEALLOC_INTERRUPT = 0x41,
  ATINIT = 0x99,
  READ_BLOCK = 0x80,
  WRITE_BLOCK = 0x81,
  GET_TIME = 0x82,
  QUIT = 0x65
};

#define badBufferAddress 0x56

struct file_entry {
  unsigned type;
  unsigned translate;

#ifdef _WIN32
  HANDLE h;
#else
  int fd;
#endif

  unsigned buffer;
  unsigned level;
  unsigned newline_mask;
  unsigned newline_char;

  /* directory stuff */
  unsigned mark;
  unsigned eof;
  unsigned char *directory_buffer;
};

#define MAX_FILES 8
#define MIN_FILE 0x40
#define MAX_FILE 0x48
struct file_entry files[MAX_FILES];




#if _WIN32
static word16 file_open(const char *path, struct file_entry *file) {

  HANDLE h;

  if (g_cfg_host_read_only) {
    h = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  } else {
    h = CreateFile(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
      h = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    }
  }

  if (h == INVALID_HANDLE_VALUE) return host_map_win32_error(GetLastError());

  file->h = h;
  return 0;
}

static word16 file_close(struct file_entry *file) {

  if (!file->type) return invalidRefNum;

  if (file->h != INVALID_HANDLE_VALUE) CloseHandle(file->h);

  if (file->directory_buffer) free(file->directory_buffer);
  memset(file, 0, sizeof(*file));
  file->h = INVALID_HANDLE_VALUE;

  return 0;
}

static word16 file_flush(struct file_entry *file) {
  if (!file->type) return invalidRefNum;

  if (file->h != INVALID_HANDLE_VALUE)
    FlushFileBuffers(file->h);

  return 0;
}

static word16 file_eof(struct file_entry *file) {
  LARGE_INTEGER li;
  if (!GetFileSizeEx(file->h, &li))
    return host_map_win32_error(GetLastError());
  if (li.QuadPart > 0xffffff) {
    file->eof = 0xffffff;
    return outOfRange;
  }
  file->eof = li.QuadPart;
  return 0;
}


#else

static word16 file_open(const char *path, struct file_entry *file) {

  int fd;

  if (g_cfg_host_read_only) {
    fd = open(path, O_RDONLY | O_NONBLOCK);
  } else {
    fd = open(path, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
      fd = open(path, O_RDONLY | O_NONBLOCK);
    }
  }
  if (fd < 0) return host_map_errno_path(errno, path);

  file->fd = fd;
  return 0;
}

static word16 file_close(struct file_entry *file) {

  if (!file->type) return invalidRefNum;

  if (file->fd >= 0) close(file->fd);

  if (file->directory_buffer) free(file->directory_buffer);
  memset(file, 0, sizeof(*file));
  file->fd = -1;
  return 0;
}

static word16 file_flush(struct file_entry *file) {
  if (!file->type) return invalidRefNum;

  if (file->fd >= 0)
    fsync(file->fd);

  return 0;
}

static word16 file_eof(struct file_entry *file) {
  off_t eof;
  eof = lseek(file->fd, 0, SEEK_END);
  if (eof < 0) return host_map_errno(errno);
  if (eof > 0xffffff) {
    file->eof = 0xffffff;
    return outOfRange;
  }
  file->eof = eof;
  return 0;
}

#endif




static char *get_pstr(word32 ptr) {
  if (!ptr) return NULL;
  int length = get_memory_c(ptr, 0);
  ptr += 1;
  char *str = host_gc_malloc(length + 1);
  for (int i = 0; i < length; ++i) {
    char c = get_memory_c(ptr+i, 0) & 0x7f;
    str[i] = c;
  }
  str[length] = 0;
  return str;
}

static word32 set_pstr(word32 ptr, const char *str) {

  if (!ptr) return paramRangeErr;

  int l = str ? strlen(str) : 0;

  if (l > 255) return buffTooSmall;
  // / is the pascal separator.
  set_memory_c(ptr++, l, 0);
  for (int i = 0; i < l; ++i) {
    set_memory_c(ptr++, *str++, 0);
  }
  return 0;
}



static char *is_host_path(unsigned pathname) {
  /* add + 5 below to skip past the /HOST/ part */
  /* (prefix may be missing trailing / ) */
  char *p = get_pstr(pathname);
  if (!p) return NULL;
  if (*p == '/') {
    if (!strncasecmp(p, "/HOST", 5)) {
      if (p[5] == 0) return "/";
      if (p[5] == '/') return p + 5;
    }
    return NULL;
  }

  if (saved_prefix) {
    return host_gc_append_path(saved_prefix + 5, p);
  }
  return NULL;
}


static unsigned lowercase_bits(const char *name) {
  unsigned rv = 0x8000;
  unsigned bit = 0x4000;
  for(unsigned i = 0; i < 15; ++i, bit >>= 1) {
    char c = name[i];
    if (c == 0) break;
    if (islower(c)) rv |= bit;
  }
  return rv;
}

/* name is relative to the host directory. */

void *create_directory_file(const char *name, const char *path, word16 *error_ptr, unsigned *block_ptr) {


  byte *data = NULL;
  int capacity = 0;
  int count = 0;
  unsigned offset = 0;
  int i;
  struct file_info fi;
  unsigned blocks = 1;
  unsigned terr;

  word32 w32;

  char **entries = NULL;
  size_t entry_count = 0;
  terr = host_scan_directory(path, &entries, &entry_count, 1);
  if (terr) {
    *error_ptr = terr;
    return NULL;
  }

  /* also need space for volume/directory header */
  capacity = 1 + (1 + entry_count) / ENTRIES_PER_BLOCK;
  capacity *= 512;

  data = malloc(capacity);
  if (!data) {
    *error_ptr = outOfMem;
    goto exit;
  }
  memset(data, 0, capacity);


  terr = host_get_file_info(path, &fi);
  if (terr) {
    *error_ptr = terr;
    goto exit;
  }

  /* trailing /s should already be stripped */
  const char *base_name = strchr(name, '/');
  base_name = base_name ? base_name + 1 : name;
  if (!base_name || !*base_name) base_name = "HOST";

  int len = strlen(base_name);
  if (len > 15) len = 15;
  /* previous / next pointers */
  data[0] = 0;
  data[1] = 0;
  data[2] = 0;
  data[3] = 0;
  offset = 4;

  int root = fi.storage_type == 0x0f;
  if (root) {
    data[offset++] = 0xf0 | len;
  } else {
    data[offset++] = 0xe0 | len;
  }
  for (i = 0; i < len; ++i) {
    data[offset++] = toupper(base_name[i]);
  }
  for(; i < 15; ++i) { data[offset++] = 0;  }
  if (root) {
    data[offset++] = 0;             /* reserved */
    data[offset++] = 0;

    w32 = host_convert_date_time(fi.modified_date);

    data[offset++] = (w32  >> 0) & 0xff;             /* last modified... */
    data[offset++] = (w32  >> 8) & 0xff;
    data[offset++] = (w32  >> 16) & 0xff;
    data[offset++] = (w32  >> 24) & 0xff;

    w32 = lowercase_bits(base_name);
    data[offset++] = (w32 >> 0) & 0xff;             /* file name case bits */
    data[offset++] = (w32 >> 8) & 0xff;


  } else {
    data[offset++] = 0x75;             /* password enabled */
    for (i = 0; i < 7; ++i) data[offset++] = 0;             /* password */
  }

  w32 = host_convert_date_time(fi.create_date);

  data[offset++] = (w32  >> 0) & 0xff;       /* creation... */
  data[offset++] = (w32  >> 8) & 0xff;
  data[offset++] = (w32  >> 16) & 0xff;
  data[offset++] = (w32  >> 24) & 0xff;

  data[offset++] = 0;       /* version */
  data[offset++] = 0;       /* min version */
  data[offset++] = fi.access;       /* access */
  data[offset++] = ENTRY_LENGTH;       /* entry length */
  data[offset++] = ENTRIES_PER_BLOCK;       /* entries per block */

  data[offset++] = 0;       /* file count (placeholder) */
  data[offset++] = 0;

  data[offset++] = 0;       /* bitmap ptr, parent ptr, etc */
  data[offset++] = 0;
  data[offset++] = 0;
  data[offset++] = 0;



  int path_len = strlen(path);

  count = 0;
  blocks = 1;
  for (int j = 0; j < entry_count; ++j) {

    char *name = entries[j];
    int len = strlen(name);
    char *tmp = malloc(path_len + 2 + len);
    if (!tmp) continue;

    memcpy(tmp, path, path_len);
    tmp[path_len] = '/';
    strcpy(tmp + path_len + 1, name);

    unsigned terr = host_get_file_info(tmp, &fi);
    free(tmp);
    if (terr) continue;

    /* wrap to next block ? */
    if ((offset + ENTRY_LENGTH) >> 9 == blocks) {
      blocks++;
      offset = (offset + 511) & ~511;
      data[offset++] = 0;                   /* next/previous block pointers */
      data[offset++] = 0;
      data[offset++] = 0;
      data[offset++] = 0;
    }

    assert(offset + ENTRY_LENGTH < capacity);

    /* create a prodos entry... */


    if (fi.storage_type == extendedFile)
      fi.storage_type = 1;
    int st = fi.storage_type << 4;

    data[offset++] = st | len;
    for(i = 0; i < len; ++i) {
      data[offset++] = toupper(name[i]);
    }
    for(; i < 15; ++i) data[offset++] = 0;
    data[offset++] = fi.file_type;
    data[offset++] = 0;             /* key ptr */
    data[offset++] = 0;             /* key ptr */
    data[offset++] = fi.blocks & 0xff;
    data[offset++] = (fi.blocks >> 8) & 0xff;
    data[offset++] = fi.eof & 0xff;
    data[offset++] = (fi.eof >> 8) & 0xff;
    data[offset++] = (fi.eof >> 16) & 0xff;

    w32 = host_convert_date_time(fi.create_date);

    data[offset++] = (w32  >> 0) & 0xff;             /* creation... */
    data[offset++] = (w32  >> 8) & 0xff;
    data[offset++] = (w32  >> 16) & 0xff;
    data[offset++] = (w32  >> 24) & 0xff;

    w32 = lowercase_bits(name);
    data[offset++] = (w32 >> 0) & 0xff;             /* file name case bits */
    data[offset++] = (w32 >> 8) & 0xff;

    data[offset++] = fi.access;
    data[offset++] = fi.aux_type & 0xff;
    data[offset++] = (fi.aux_type >> 8) & 0xff;

    w32 = host_convert_date_time(fi.modified_date);
    data[offset++] = (w32  >> 0) & 0xff;             /* last mod... */
    data[offset++] = (w32  >> 8) & 0xff;
    data[offset++] = (w32  >> 16) & 0xff;
    data[offset++] = (w32  >> 24) & 0xff;

    data[offset++] = 0;             /* header ptr */
    data[offset++] = 0;
    count++;
  }

  data[4 + 0x21] = count & 0xff;
  data[4 + 0x22] = (count >> 8)& 0xff;
  *block_ptr = blocks;

exit:
  if (entries) host_free_directory(entries, entry_count);
  return data;
}



static int mli_atinit(unsigned dcb) {
  unsigned pcount = get_memory_c(dcb, 0);

  if (pcount < 3|| pcount > 4) return invalidPcount;

  unsigned version = get_memory_c(dcb + 1, 0);
  saved_mli_address = get_memory16_c(dcb + 2, 0);
  saved_patch_address = get_memory16_c(dcb + 4, 0);
  if (!saved_patch_address) return paramRangeErr;
  if (!saved_mli_address) return paramRangeErr;

  saved_unit = 0x80;
  if (pcount >= 4) saved_unit = get_memory_c(dcb + 6, 0) & 0xf0;

  if (!saved_unit) return paramRangeErr;

  saved_prefix = 0;

  memset(files, 0, sizeof(files));       /* ??? */

  return host_startup();
}


enum {
  FILE_INFO_pcount = 0,
  FILE_INFO_pathname = 1,
  FILE_INFO_access = 3,
  FILE_INFO_file_type = 4,
  FILE_INFO_aux_type = 5,
  FILE_INFO_storage_type = 7,
  FILE_INFO_blocks = 8,
  FILE_INFO_mod_date = 10,
  FILE_INFO_mod_time = 12,
  FILE_INFO_create_date = 14,
  FILE_INFO_create_time = 16
};

static int mli_set_file_info(unsigned dcb, const char *path) {


  struct file_info fi;
  memset(&fi, 0, sizeof(fi));

  fi.access = get_memory_c(dcb + FILE_INFO_access, 0);
  fi.file_type = get_memory_c(dcb + FILE_INFO_file_type, 0);
  fi.aux_type = get_memory16_c(dcb + FILE_INFO_aux_type, 0);
  fi.modified_date = host_get_date_time(dcb + FILE_INFO_mod_date);

  return host_set_file_info(path, &fi);
}

static int mli_get_file_info(unsigned dcb, const char *path) {

  struct file_info fi;
  int rv = host_get_file_info(path, &fi);
  if (rv) return rv;

  /* ignore resource fork */
  if (fi.storage_type == extendedFile)
    fi.storage_type = seedling;

  set_memory_c(dcb + FILE_INFO_access, fi.access, 0);
  set_memory_c(dcb + FILE_INFO_file_type, fi.file_type, 0);
  set_memory16_c(dcb + FILE_INFO_aux_type, fi.aux_type, 0);
  set_memory_c(dcb + FILE_INFO_storage_type, fi.storage_type, 0);
  set_memory16_c(dcb + FILE_INFO_blocks, fi.blocks, 0);
  host_set_date_time(dcb + FILE_INFO_mod_date, fi.modified_date);
  host_set_date_time(dcb + FILE_INFO_create_date, fi.create_date);

  return 0;
}

enum {
  CREATE_pcount = 0,
  CREATE_pathname = 1,
  CREATE_access = 3,
  CREATE_file_type = 4,
  CREATE_aux_type = 5,
  CREATE_storage_type = 7,
  CREATE_create_date = 8,
  CREATE_create_time = 10
};

static int mli_create(unsigned dcb, const char *path) {

  struct file_info fi;
  memset(&fi, 0, sizeof(fi));

  fi.access = get_memory_c(dcb + CREATE_access, 0);
  fi.file_type = get_memory_c(dcb + CREATE_file_type, 0);
  fi.aux_type = get_memory16_c(dcb + CREATE_aux_type, 0);
  fi.storage_type = get_memory_c(dcb + CREATE_storage_type, 0);
  fi.create_date = host_get_date_time(dcb + CREATE_create_date);

  if (g_cfg_host_read_only) return drvrWrtProt;

  /*
   * actual prodos needs the correct storage type and file type
   * for a usable directory.  this just does the right thing.
   * TODO - does ProDOS update the storage type in the dcb?
   */
  switch(fi.storage_type) {
    case 0x0d:
      fi.file_type = 0x0f;
      break;
    case 0x01:
    case 0x02:
    case 0x03:
      fi.storage_type = 0x01;
      break;
    case 0x00:
      if (fi.file_type == 0x0f) fi.storage_type = 0x0d;
      else fi.storage_type = 0x01;
      break;
    default:
      return badStoreType;
  }
  // todo -- remap access.

  if (fi.storage_type == 0x0d) {
#if _WIN32
    if (!CreateDirectory(path, NULL))
      return host_map_win32_error(GetLastError());
#else
    if (mkdir(path, 0777) < 0)
      return host_map_errno_path(errno, path);
#endif
  } else {
#if _WIN32
    HANDLE h = CreateFile(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return host_map_win32_error(GetLastError());

    afp_init(&fi.afp, fi.file_type, fi.aux_type);
    fi.has_fi = 1;

    // set ftype, auxtype...
    host_set_file_info(path, &fi);
    CloseHandle(h);
#else
    int fd = open(path, O_CREAT | O_EXCL | O_RDONLY, 0666);
    if (fd < 0)
      return host_map_errno_path(errno, path);

    host_file_type_to_finder_info(fi.finder_info, fi.file_type, fi.aux_type);
    fi.has_fi = 1;
    host_set_file_info(path, &fi);
    close(fd);
#endif
  }

  return 0;
}

static int mli_destroy(unsigned dcb, const char *path) {

  word16 terr = 0;
  unsigned type = host_storage_type(path, &terr);
  if (type == 0) return terr;

  switch(type) {
    case 0: return terr;
    case 0x0f: return badPathSyntax; /* root directory */
    case 0x0d:
#if _WIN32
      if (!RemoveDirectory(path))
        return host_map_win32_error(GetLastError());
#else
      if (rmdir(path) < 0)
        return host_map_errno_path(errno, path); 
#endif
      break;
    case 1:
    case 2:
    case 3:
    case 5:
#if _WIN32
      if (!DeleteFile(path))
        return host_map_win32_error(GetLastError());
#else
      if (unlink(path) < 0)
        return host_map_errno_path(errno, path); 

#endif
    default:
      return badStoreType;
  }
  return 0;
}

static int mli_rename(unsigned dcb, const char *path1, const char *path2) {
  /* can't rename the root directory */
  word16 terr = 0;
  unsigned type;

  if (!path1 || !path2) return badPathSyntax;


  type = host_storage_type(path1, &terr);
  if (!type) return terr;
  if (type == 0x0f) return badPathSyntax;

  type = host_storage_type(path2, &terr);
  if (type) return dupPathname;

#if _WIN32
  if (!MoveFile(path1, path2))
    return host_map_win32_error(GetLastError());
#else
  if (rename(path1, path2) < 0)
    return host_map_errno(errno);
#endif
  return 0;
}


static int mli_write(unsigned dcb, struct file_entry *file) {

  /* todo -- translate */

  set_memory16_c(dcb + 6, 0, 0);

  if (!file->type) return invalidRefNum;
  if (file->type != file_regular) return invalidAccess;

  unsigned data_buffer = get_memory16_c(dcb + 2, 0);
  unsigned request_count = get_memory16_c(dcb + 4, 0);

  if (!data_buffer) return badBufferAddress;

  byte *data = host_gc_malloc(request_count);
  if (!data) return drvrIOError;

  for (unsigned i = 0; i < request_count; ++i) {
    data[i] = get_memory_c(data_buffer + i, 0);
  }

  switch (file->translate) {
    case translate_crlf:
      host_cr_to_lf(data, request_count);
      break;
    case translate_merlin:
      host_merlin_to_text(data, request_count);
      break;
  }

#if _WIN32
  DWORD rv;
  LARGE_INTEGER li;
  li.QuadPart = file->mark;
  if (!SetFilePointerEx(file->h, li, NULL, FILE_BEGIN))
    return host_map_win32_error(GetLastError());
  if (!WriteFile(file->h, data, request_count, &rv, NULL))
    return host_map_win32_error(GetLastError());
#else
  int rv = pwrite(file->fd, data, request_count, file->mark);
  if (rv < 0) return host_map_errno(errno);
#endif
  set_memory16_c(dcb + 6, rv, 0);
  file->mark += rv;

  return 0;
}


static int mli_read(unsigned dcb, struct file_entry *file) {

  /* todo -- translate */

  set_memory16_c(dcb + 6, 0, 0);

  if (!file->type) return invalidRefNum;

  unsigned data_buffer = get_memory16_c(dcb + 2, 0);
  unsigned request_count = get_memory16_c(dcb + 4, 0);

  if (!data_buffer) return badBufferAddress;

  unsigned count = 0;
  unsigned mask = file->newline_mask;
  unsigned nl = file->newline_char;

  if (file->type == file_directory) {
    unsigned mark = file->mark;
    if (mark >= file->eof) return eofEncountered;
    /* support newline mode ... */

    if (mark + request_count > file->eof)
      request_count = file->eof - mark;


    for (count = 0; count < request_count; ++count) {
      byte b = file->directory_buffer[mark++];
      set_memory_c(data_buffer + count, b, 0);
      if (mask && (b & mask) == nl) break;
    }
    file->mark = mark;
    set_memory16_c(dcb + 6, count, 0);
    return 0;
  }

  if (file->type != file_regular) return invalidAccess;

  byte *data = host_gc_malloc(request_count);
  if (!data) return drvrIOError;

#if _WIN32
  LARGE_INTEGER li;
  DWORD rv;
  li.QuadPart = file->mark;
  if (!SetFilePointerEx(file->h, li, NULL, FILE_BEGIN))
    return host_map_win32_error(GetLastError());

  if (!ReadFile(file->h, data, request_count, &rv, NULL))
    return host_map_win32_error(GetLastError());
#else
  int rv = pread(file->fd, data, request_count, file->mark);
  if (rv < 0) return host_map_errno(errno);
#endif

  if (rv == 0) return eofEncountered;
  count = rv;

  switch (file->translate) {
    case translate_crlf:
      host_lf_to_cr(data, count);
      break;
    case translate_merlin:
      host_text_to_merlin(data, count);
      break;
  }


  if (mask) {
    for (int count = 0; count < rv; ) {
      byte b = data[count++];
      if ((b & mask) == nl) break;
    }
  }
  for (unsigned i = 0; i < count; ++i)
    set_memory_c(data_buffer + i, data[i], 0);

  file->mark += count;
  set_memory16_c(dcb + 6, count, 0);
  return 0;
}



static int mli_get_buf(unsigned dcb, struct file_entry *file) {

  if (!file->type) return invalidRefNum;
  set_memory16_c(dcb + 2, file->buffer, 0);
  return 0;
}

static int mli_set_buf(unsigned dcb, struct file_entry *file) {

  unsigned buffer = get_memory16_c(dcb + 2, 0);
  if (!file->type) return invalidRefNum;
  //if (!buffer) return badBufferAddress;
  if (buffer & 0xff) return badBufferAddress;

  file->buffer = buffer;
  return 0;
}

static int mli_get_eof(unsigned dcb, struct file_entry *file) {

  word16 terr = 0;

  switch (file->type) {
    default:
      return invalidRefNum;

    case file_directory:
      break;

    case file_regular:
      terr = file_eof(file);
      if (terr) return terr;
      break;
    }

  set_memory24_c(dcb + 2, file->eof, 0);
  return 0;
}


static int mli_set_eof(unsigned dcb, struct file_entry *file) {

  off_t eof = get_memory24_c(dcb + 2, 0);
#if _WIN32
  LARGE_INTEGER tmp;
#endif

  switch (file->type) {
    default:
      return invalidRefNum;

    case file_directory:
      return invalidAccess;
      break;

    case file_regular:
#if _WIN32
      tmp.QuadPart = eof;
      if (!SetFilePointerEx(file->h, tmp, NULL, FILE_BEGIN))
        return host_map_win32_error(GetLastError());
      if (!SetEndOfFile(file->h))
        return host_map_win32_error(GetLastError());
#else
      if (ftruncate(file->fd, eof) < 0)
        return host_map_errno(errno);
#endif
      break;
  }
  return 0;
}

static int mli_get_mark(unsigned dcb, struct file_entry *file) {

  off_t position = 0;

  switch (file->type) {
    default:
      return invalidRefNum;

    case file_directory:
    case file_regular:
      position = file->mark;
      break;
  }
  if (position > 0xffffff) return outOfRange;
  set_memory24_c(dcb + 2, position, 0);
  return 0;

}

static int mli_set_mark(unsigned dcb, struct file_entry *file) {

  word16 terr = 0;

  word32 position = get_memory24_c(dcb + 2, 0);

  switch (file->type) {
    default:
      return invalidRefNum;
    case file_directory:
      break;
    case file_regular:
      terr = file_eof(file);
      if (terr) return terr;
      break;
  }

  if (position > file->eof) return outOfRange;
  file->mark = position;

  return 0;
}

static int mli_newline(unsigned dcb, struct file_entry *file) {
  if (!file->type) return invalidRefNum;

  file->newline_mask = get_memory_c(dcb + 2, 0);
  file->newline_char = get_memory_c(dcb + 3, 0);
  return 0;
}



static int mli_close(unsigned dcb, struct file_entry *file) {

  if (!file) {
    unsigned level = get_memory_c(LEVEL, 0);
    unsigned i;
    for (i = 0; i < MAX_FILES; ++i) {
      file = &files[i];
      if (!file->type) continue;
      if (file->level < level) continue;
      file_close(file);
    }
    return -1;             /* pass to prodos mli */
  }

  return file_close(file);
}


static int mli_flush(unsigned dcb, struct file_entry *file) {

  if (!file) {
    unsigned level = get_memory_c(LEVEL, 0);
    unsigned i;
    for (i = 0; i < MAX_FILES; ++i) {
      file = &files[i];
      if (!file->type) continue;
      if (file->level < level) continue;

      file_flush(file);
    }
    return -1;             /* pass to prodos mli */
  }
  return file_flush(file);
}


/*
 * quit
 * close all open files.
 */
static int mli_quit(unsigned dcb) {
  unsigned i;
  for (i = 0; i < MAX_FILES; ++i) {
    struct file_entry *file = &files[i];
    if (!file->type) continue;

#if _WIN32
  if (file->h != INVALID_HANDLE_VALUE) CloseHandle(file->h);
#else
  if (file->fd >= 0) close(file->fd);
#endif

    if (file->directory_buffer) free(file->directory_buffer);
    memset(file, 0, sizeof(*file));

#if _WIN32
  file->h = INVALID_HANDLE_VALUE;
#else
  file->fd = -1;
#endif


  }
  /* need a better way to know... */
  /* host_shutdown(); */
  return -1;
}




static int mli_open(unsigned dcb, const char *name, const char *path) {

  int refnum = 0;

  struct file_entry *file = NULL;
  for (unsigned i = 0; i < MAX_FILES; ++i) {
    file = &files[i];
    if (!file->type) {
      refnum = MIN_FILE + i;
      break;
    }
  }
  if (!refnum) return tooManyFilesOpen;

  unsigned buffer = get_memory_c(dcb + 3, 0);
  //if (buffer == 0) return badBufferAddress;
  if (buffer & 0xff) return badBufferAddress;

  struct file_info fi;
  word16 terr = host_get_file_info(path, &fi);
  if (terr) return terr;


  if (fi.storage_type == 0x0f || fi.storage_type == 0x0d) {
      unsigned blocks;
      void *tmp;
      tmp = create_directory_file(name, path, &terr, &blocks);
      if (!tmp) return terr;
      file->type = file_directory;
      file->directory_buffer = tmp;
      file->eof = blocks * 512;
  } else {
    terr = file_open(path, file);
    if (terr) return terr;

    file->type = file_regular;

    if (g_cfg_host_crlf) {
      if (fi.file_type == 0x04 || fi.file_type == 0xb0)
        file->translate = translate_crlf;
    }

    if (g_cfg_host_merlin && fi.file_type == 0x04) {
      int n = strlen(path);
      if (n >= 3 && path[n-1] == 'S' && path[n-2] == '.')
        file->translate = translate_merlin;
    }
  }

  file->level = get_memory_c(LEVEL, 0);
  file->buffer = buffer;
  file->mark = 0;
  set_memory_c(dcb + 5, refnum, 0);
  return 0;
}


static int mli_get_prefix(unsigned dcb) {
  if (!saved_prefix) return -1;
  unsigned buffer = get_memory16_c(dcb + 1, 0);
  if (!buffer) return badBufferAddress;

  set_pstr(buffer, saved_prefix);
  return 0;
}

/*
 * name is relative the the /HOST device.
 * path is the path on the native fs.
 */
static int mli_set_prefix(unsigned dcb, char *name, char *path) {
  /*
   * IF path is null, tail patch if we own the prefix.
   * otherwise, handle it or forward to mli.
   */
  if (!path) {
    return saved_prefix ? -2 : -1;
  }

  unsigned type;
  word16 terr;

  type = host_storage_type(path, &terr);
  switch(type) {
    case 0x0f:
    case 0x0d:
      break;
    case 0: return terr;
    default: return badStoreType;
  }

  /* /HOST/ was previously stripped... add it back. */
  name = host_gc_append_path("/HOST", name);

  int l = strlen(name);
  /* trim trailing / */
  while (l > 1 && name[l-1] == '/') --l;
  name[l] = 0;

  if (l > 63) return badPathSyntax;


  if (saved_prefix)
    free(saved_prefix);
  saved_prefix = strdup(name);
  set_memory_c(PFIXPTR, 1, 0);
  return 0;
}

static int mli_online(unsigned dcb) {
  /* not yet ... */
  unsigned unit = get_memory_c(dcb + 1, 0);
  unsigned buffer = get_memory16_c(dcb + 2, 0);
  if (unit == saved_unit) {
    if (!buffer) return badBufferAddress;
    /* slot 2, drive 1 ... why not*/
    set_memory_c(buffer++, saved_unit | 0x04, 0);
    set_memory_c(buffer++, 'H', 0);
    set_memory_c(buffer++, 'O', 0);
    set_memory_c(buffer++, 'S', 0);
    set_memory_c(buffer++, 'T', 0);
    return 0;
  }
  if (unit == 0) return -2;
  return -1;
}

static int mli_online_tail(unsigned dcb) {
  unsigned buffer = get_memory16_c(dcb + 2, 0);

  host_hexdump(buffer, 256);

  /* if there was an error with the device
     there will be an error code instead of a name (length = 0)
   */

  for (unsigned i = 0; i < 16; ++i, buffer += 16) {
    unsigned x = get_memory_c(buffer, 0);
    if (x == 0 || ((x & 0xf0) == saved_unit)) {
      set_memory_c(buffer++, saved_unit | 0x04, 0);
      set_memory_c(buffer++, 'H', 0);
      set_memory_c(buffer++, 'O', 0);
      set_memory_c(buffer++, 'S', 0);
      set_memory_c(buffer++, 'T', 0);
      break;
    }
  }

  return 0;
}

static int mli_rw_block(unsigned dcb) {
  unsigned unit = get_memory_c(dcb + 1, 0);
  unsigned buffer = get_memory16_c(dcb + 2, 0);

  if (unit == saved_unit) {
    if (!buffer) return badBufferAddress;
    return notBlockDev;             /* network error? */
  }
  return -1;
}


static unsigned call_pcount(unsigned call) {
  switch (call) {
    case CREATE: return 7;
    case DESTROY: return 1;
    case RENAME: return 2;
    case SET_FILE_INFO: return 7;
    case GET_FILE_INFO: return 10;
    case ONLINE: return 2;
    case SET_PREFIX: return 1;
    case GET_PREFIX: return 1;
    case OPEN: return 3;
    case NEWLINE: return 3;
    case READ: return 4;
    case WRITE: return 4;
    case CLOSE: return 1;
    case FLUSH: return 1;
    case SET_MARK: return 2;
    case GET_MARK: return 2;
    case SET_EOF: return 2;
    case GET_EOF: return 2;
    case SET_BUF: return 2;
    case GET_BUF: return 2;

    default: return 0;
  }
}


static const char *call_name(unsigned call) {

  switch (call) {
    case CREATE: return "CREATE";
    case DESTROY: return "DESTROY";
    case RENAME: return "RENAME";
    case SET_FILE_INFO: return "SET_FILE_INFO";
    case GET_FILE_INFO: return "GET_FILE_INFO";
    case ONLINE: return "ONLINE";
    case SET_PREFIX: return "SET_PREFIX";
    case GET_PREFIX: return "GET_PREFIX";
    case OPEN: return "OPEN";
    case NEWLINE: return "NEWLINE";
    case READ: return "READ";
    case WRITE: return "WRITE";
    case CLOSE: return "CLOSE";
    case FLUSH: return "FLUSH";
    case SET_MARK: return "SET_MARK";
    case GET_MARK: return "GET_MARK";
    case SET_EOF: return "SET_EOF";
    case GET_EOF: return "GET_EOF";
    case SET_BUF: return "SET_BUF";
    case GET_BUF: return "GET_BUF";
    case ATINIT: return "ATINIT";

    default: return "";
  }
}


#ifdef __CYGWIN__

#include <sys/cygwin.h>

static char *expand_path(const char *path, word32 *error) {

  char buffer[PATH_MAX];
  if (!path) return path;

  ssize_t ok = cygwin_conv_path(CCP_POSIX_TO_WIN_A, path, buffer, sizeof(buffer));
  if (ok < 0) {
    if (error) *error = fstError;
    return NULL;
  }
  return host_gc_strdup(buffer);
}


#else
#define expand_path(x, y) (x)
#endif


/*
 * mli head patch. called before ProDOS mli.
 * this call will either
 * 1. handle and return
 * 2. redirect to the real mli
 * 3. call real mli, then call host_mli_tail after
 */
void host_mli_head() {

  word32 rts = get_memory16_c(engine.stack+1, 0);
  saved_call = get_memory_c(rts + 1, 0);
  saved_dcb = get_memory16_c(rts + 2, 0);
  saved_p = engine.psr;
  word16 terr = 0;

  /* do pcount / path stuff here */
  char *path1 = NULL;
  char *path2 = NULL;
  char *path3 = NULL;
  char *path4 = NULL;

  struct file_entry *file = NULL;

  unsigned pcount = get_memory_c(saved_dcb, 0);
  unsigned xpcount = call_pcount(saved_call);
  int acc = 0;
  int refnum = 0;

  if (xpcount && xpcount != pcount) {
    acc = invalidPcount;
    goto cleanup;
  }

  switch(saved_call) {
    case CREATE:
    case DESTROY:
    case SET_FILE_INFO:
    case GET_FILE_INFO:
    case OPEN:
      path1 = is_host_path(get_memory16_c(saved_dcb + 1, 0));
      if (!path1) goto prodos_mli;
      path3 = host_gc_append_path(host_root, path1);
      path3 = expand_path(path3, &terr);
      break;

    case RENAME:
      path1 = is_host_path(get_memory16_c(saved_dcb + 1,0));
      path2 = is_host_path(get_memory16_c(saved_dcb + 3,0));
      if (!path1 && !path2) goto prodos_mli;
      if (path1) path3 = host_gc_append_path(host_root, path1);
      if (path2) path4 = host_gc_append_path(host_root, path2);
      path3 = expand_path(path3, &terr);
      path4 = expand_path(path4, &terr);
      break;

    case SET_PREFIX:
      path1 = is_host_path(get_memory16_c(saved_dcb + 1,0));
      if (!path1 && !saved_prefix) goto prodos_mli;
      if (path1) path3 = host_gc_append_path(host_root, path1);
      path3 = expand_path(path3, &terr);
      break;

    case GET_PREFIX:
      if (!saved_prefix) goto prodos_mli;
      break;

    /* refnum based */
    case NEWLINE:
    case READ:
    case WRITE:
    case SET_MARK:
    case GET_MARK:
    case SET_EOF:
    case GET_EOF:
    case SET_BUF:
    case GET_BUF:
      refnum = get_memory_c(saved_dcb + 1, 0);

      if (refnum >= MIN_FILE && refnum < MAX_FILE) {
        file = &files[refnum - MIN_FILE];
      } else {
        goto prodos_mli;
      }
      break;

    case CLOSE:
    case FLUSH:
      /* special case for refnum == 0 */
      refnum = get_memory_c(saved_dcb + 1, 0);

      if (refnum >= MIN_FILE && refnum < MAX_FILE) {
        file = &files[refnum - MIN_FILE];
      } else if (refnum) {
        goto prodos_mli;
      }

      break;

  }

  fprintf(stderr, "MLI: %02x %s", saved_call, call_name(saved_call));
  if (path1) fprintf(stderr, " - %s", path1);
  if (path2) fprintf(stderr, " - %s", path2);

  switch (saved_call) {
    default:
      engine.kpc = saved_mli_address;
      return;

    case ATINIT:
      acc = mli_atinit(saved_dcb);
      break;

    case CREATE:
      acc = mli_create(saved_dcb, path3);
      break;

    case DESTROY:
      acc = mli_destroy(saved_dcb, path3);
      break;

    case SET_FILE_INFO:
      acc = mli_set_file_info(saved_dcb, path3);
      break;

    case GET_FILE_INFO:
      acc = mli_get_file_info(saved_dcb, path3);
      break;

    case OPEN:
      acc = mli_open(saved_dcb, path1, path3);
      break;

    case RENAME:
      acc = mli_rename(saved_dcb, path3, path4);
      break;

    case SET_PREFIX:
      acc = mli_set_prefix(saved_dcb, path1, path3);
      break;

    case GET_PREFIX:
      acc = mli_get_prefix(saved_dcb);
      break;

    case ONLINE:
      acc = mli_online(saved_dcb);
      break;

    case NEWLINE:
      acc = mli_newline(saved_dcb, file);
      break;

    case READ:
      acc = mli_read(saved_dcb, file);
      break;

    case WRITE:
      acc = mli_write(saved_dcb, file);
      break;

    case SET_MARK:
      acc = mli_set_mark(saved_dcb, file);
      break;

    case GET_MARK:
      acc = mli_get_mark(saved_dcb, file);
      break;

    case SET_EOF:
      acc = mli_set_eof(saved_dcb, file);
      break;

    case GET_EOF:
      acc = mli_get_eof(saved_dcb, file);
      break;

    case SET_BUF:
      acc = mli_set_buf(saved_dcb, file);
      break;

    case GET_BUF:
      acc = mli_get_buf(saved_dcb, file);
      break;

    case CLOSE:
      acc = mli_close(saved_dcb, file);
      break;

    case FLUSH:
      acc = mli_flush(saved_dcb, file);
      break;

    case WRITE_BLOCK:
    case READ_BLOCK:
      acc = mli_rw_block(saved_dcb);
      break;


    case QUIT:
      acc = mli_quit(saved_dcb);
      break;

  }
  fputs("\n", stderr);
  fflush(stderr);
  host_gc_free();


  if (acc == -2) {
    /* tail call needed */
    /*
            jsr xxxx
            dc.b xx
            dc.w xxxx
            wdm ..
            rts
     */
    SEI();
    set_memory_c(saved_patch_address + 0, saved_call, 0);
    set_memory16_c(saved_patch_address + 1, saved_dcb, 0);
    set_memory16_c(engine.stack+1, rts + 3, 0);
    return;
  }
  if (acc < 0) {
prodos_mli:
    host_gc_free();
    /* pass to normal dispatcher */
    engine.kpc = saved_mli_address;
    return;
  }
cleanup:
  /* fixup */
  acc &= 0xff;

  if (acc) fprintf(stderr, "          %02x   %s\n", acc, host_error_name(acc));
  if (acc == 0 && saved_call != ATINIT) {
    set_memory_c(DEVNUM, saved_unit, 0);
  }
  engine.acc &= 0xff00;
  engine.acc |= acc;
  if (acc) {
    SEC();
    CLZ();
  } else {
    CLC();
    SEZ();
  }
  engine.kpc = rts + 4;
  engine.stack += 2;
  return;

}

void host_mli_tail() {

  if (!(engine.psr & C)) {

    switch(saved_call) {
      case SET_PREFIX:
        free(saved_prefix);
        saved_prefix = NULL;
        break;
      case ONLINE:
        mli_online_tail(saved_dcb);
        break;
    }
  }

  // clean up I bit (set in head)
  engine.psr &= ~I;
  engine.psr |= (saved_p & I);
  host_gc_free();
}
