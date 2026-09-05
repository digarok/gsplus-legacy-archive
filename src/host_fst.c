/*
   GSPLUS - Advanced Apple IIGS Emulator Environment
   Based on the KEGS emulator written by Kent Dickey
   See COPYRIGHT.txt for Copyright information
   See LICENSE.txt for license (GPL v2)
 */

#define _BSD_SOURCE

#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <unistd.h>
#include <ctype.h>
#include <string.h>
#include <time.h>
#include <libgen.h>


#if defined(__APPLE__)
#include <sys/xattr.h>
#include <sys/attr.h>
#include <sys/paths.h>
#endif

#ifndef XATTR_FINDERINFO_NAME
#define XATTR_FINDERINFO_NAME "com.apple.FinderInfo"
#endif

#ifndef XATTR_RESOURCEFORK_NAME
#define XATTR_RESOURCEFORK_NAME "com.apple.ResourceFork"
#endif


#include "defc.h"
#include "gsos.h"
#include "fst.h"

#include "host_common.h"



extern Engine_reg engine;


/* direct page offsets */

#define dp_call_number 0x30
#define dp_param_blk_ptr 0x32
#define dp_dev1_num 0x36
#define dp_dev2_num 0x38
#define dp_path1_ptr 0x3a
#define dp_path2_ptr 0x3e
#define dp_path_flag 0x42

#define global_buffer 0x009a00



struct directory {
  int displacement;
  int num_entries;
  char *entries[1];
};

struct fd_entry {
  struct fd_entry *next;
  char *path;
  int cookie;
  int type;
  int access;
  int fd;
  int translate;
  struct directory *dir;
};

static void free_directory(struct directory *dd);
static struct directory *read_directory(const char *path, word16 *error);

#define COOKIE_BASE 0x8000

static word32 cookies[32] = {};

static int alloc_cookie() {
  for (int i = 0; i < 32; ++i) {
    word32 x = cookies[i];

    for (int j = 0; j < 32; ++j, x >>= 1) {
      if (x & 0x01) continue;

      cookies[i] |= (1 << j);
      return COOKIE_BASE + (i * 32 + j);
    }
  }
  return -1;
}

static int free_cookie(int cookie) {
  if (cookie < COOKIE_BASE) return -1;
  cookie -= COOKIE_BASE;
  if (cookie >= 32 *32) return -1;

  int chunk = cookie / 32;
  int offset = 1 << (cookie % 32);

  word32 x = cookies[chunk];

  if ((x & offset) == 0) return -1;
  x &= ~offset;
  cookies[chunk] = x;
  return 0;
}

static struct fd_entry *fd_head = NULL;




static struct fd_entry *find_fd(int cookie) {
  struct fd_entry *head = fd_head;

  while(head) {
    if (head->cookie == cookie) return head;
    head = head->next;
  }
  return NULL;
}


static void free_fd(struct fd_entry *e) {
  if (!e) return;
  if (e->cookie) free_cookie(e->cookie);
  if (e->fd >= 0) close(e->fd);
  if (e->dir) free_directory(e->dir);
  free(e->path);
  free(e);
}

static struct fd_entry *alloc_fd() {
  struct fd_entry *e = calloc(sizeof(struct fd_entry), 1);
  e->fd = -1;
  return e;
}


static word32 remove_fd(int cookie) {
  word32 rv = invalidRefNum;

  struct fd_entry *prev = NULL;
  struct fd_entry *head = fd_head;
  while (head) {
    if (head->cookie == cookie) {
      if (prev) prev->next = head->next;
      else fd_head = head->next;

      free_fd(head);
      rv = 0;
      break;
    }
    prev = head;
    head = head->next;
  }
  return rv;
}


static ssize_t safe_read(struct fd_entry *e, byte *buffer, size_t count) {
  int fd = e->fd;
  int tr = e->translate;

  for (;;) {
    ssize_t ok = read(fd, buffer, count);
    if (ok >= 0) {
      size_t i;
      if (tr == translate_crlf) {
        for (i = 0; i < ok; ++i) {
          if (buffer[i] == '\n') buffer[i] = '\r';
        }
      }
      if (tr == translate_merlin) {
        for (i = 0; i < ok; ++i) {
          unsigned char c = buffer[i];
          if (c == '\t') c = 0xa0;
          if (c == '\n') c = '\r';
          if (c != ' ') c |= 0x80;
          buffer[i] = c;
        }
      }
      return ok;
    }
    if (ok < 0 && errno == EINTR) continue;
    return ok;
  }
}

static ssize_t safe_write(struct fd_entry *e, byte *buffer, size_t count) {
  int fd = e->fd;
  int tr = e->translate;

  if (tr == translate_crlf) {
    size_t i;
    for (i = 0; i < count; ++i) {
      if (buffer[i] == '\r') buffer[i] = '\n';
    }
  }
  if (tr == translate_merlin) {
    size_t i;
    for (i = 0; i < count; ++i) {
      unsigned char c = buffer[i];
      if (c == 0xa0) c = '\t';
      c &= 0x7f;
      if (c == '\r') c = '\n';
      buffer[i] = c;
    }
  }

  for (;;) {
    ssize_t ok = write(fd, buffer, count);
    if (ok >= 0) return ok;
    if (ok < 0 && errno == EINTR) continue;
    return ok;
  }
}


/*
 * if this is an absolute path, verify and skip past :Host:
 *
 */
static const char *check_path(const char *in, word32 *error) {

  word32 tmp;
  if (!error) error = &tmp;

  *error = 0;
  if (!in) return "";

  /* check for .. */
  const char *cp = in;
  do {
    while (*cp == '/') ++cp;
    if (cp[0] == '.' && cp[1] == '.' && (cp[2] == '/' || cp[2] == 0))
    {
      *error = badPathSyntax;
      return NULL;
    }
    cp = strchr(cp, '/');
  } while(cp);


  if (in[0] != '/') return in;

  if (strncasecmp(in, "/Host", 5) == 0 && (in[5] == '/' || in[5] == 0)) {
    in += 5;
    while (*in == '/') ++in;
    return in;
  }
  *error = volNotFound;
  return NULL;
}


static char * get_gsstr(word32 ptr) {
  if (!ptr) return NULL;
  int length = get_memory16_c(ptr, 0);
  ptr += 2;
  char *str = host_gc_malloc(length + 1);
  for (int i = 0; i < length; ++i) {
    char c = get_memory_c(ptr+i, 0);
    if (c == ':') c = '/';
    str[i] = c;
  }
  str[length] = 0;
  return str;
}

static char * get_pstr(word32 ptr) {
  if (!ptr) return NULL;
  int length = get_memory_c(ptr, 0);
  ptr += 1;
  char *str = host_gc_malloc(length + 1);
  for (int i = 0; i < length; ++i) {
    char c = get_memory_c(ptr+i, 0);
    if (c == ':') c = '/';
    str[i] = c;
  }
  str[length] = 0;
  return str;
}

static word32 set_gsstr(word32 ptr, const char *str) {

  if (!ptr) return paramRangeErr;

  int l = str ? strlen(str) : 0;


  word32 cap = get_memory16_c(ptr, 0);
  ptr += 2;

  if (cap < 4) return paramRangeErr;

  set_memory16_c(ptr, l, 0);
  ptr += 2;

  if (cap < l + 4) return buffTooSmall;


  for (int i = 0; i < l; ++i) {
    char c = *str++;
    if (c == '/') c = ':';
    set_memory_c(ptr++, c, 0);
  }
  return 0;
}

static word32 set_gsstr_truncate(word32 ptr, const char *str) {

  if (!ptr) return paramRangeErr;

  int l = str ? strlen(str) : 0;


  word32 cap = get_memory16_c(ptr, 0);
  ptr += 2;

  if (cap < 4) return paramRangeErr;

  set_memory16_c(ptr, l, 0);
  ptr += 2;

  // get dir entry copies data even
  // if buffTooSmall...
  int rv = 0;
  if (cap < l + 4) {
    l = cap - 4;
    rv = buffTooSmall;
  }

  for (int i = 0; i < l; ++i) {
    char c = *str++;
    if (c == '/') c = ':';
    set_memory_c(ptr++, c, 0);
  }
  return rv;
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


static word32 set_option_list(word32 ptr, word16 fstID, const byte *data, int size) {
  if (!ptr) return 0;

  // totalSize
  word32 cap = get_memory16_c(ptr, 0);
  ptr += 2;

  if (cap < 4) return paramRangeErr;

  // reqSize
  set_memory16_c(ptr, size + 2, 0);
  ptr += 2;

  if (cap < size + 6) return buffTooSmall;


  // fileSysID.
  set_memory16_c(ptr, fstID, 0);
  ptr += 2;

  for (int i = 0; i < size; ++i)
    set_memory_c(ptr++, *data++, 0);

  return 0;
}



static char *get_path1(void) {
  word32 direct = engine.direct;
  word16 flags = get_memory16_c(direct + dp_path_flag, 0);
  if (flags & (1 << 14))
    return get_gsstr( get_memory24_c(direct + dp_path1_ptr, 0));
  return NULL;
}

static char *get_path2(void) {
  word32 direct = engine.direct;
  word16 flags = get_memory16_c(direct + dp_path_flag, 0);
  if (flags & (1 << 6))
    return get_gsstr( get_memory24_c(direct + dp_path2_ptr, 0));
  return NULL;
}

#if defined (__linux__)
static void get_resource_pathname(const char *path, char *rpath) {
  char *cpath = strdup(path); 
  snprintf(rpath,1024,"%s/._%s",dirname(cpath),basename(cpath));
  free(cpath);
}
#endif 


/*
 * shutdown is called when switching to p8.
 * startup is ONLY called during initial boot.
 */

static word32 fst_shutdown(void) {

  // close any remaining files.
  struct fd_entry *head = fd_head;
  while (head) {
    struct fd_entry *next = head->next;

    free_fd(head);
    head = next;
  }
  //host_shutdown();
  return 0;
}

static word32 fst_startup(void) {
  // if restart, close any previous files.

  fst_shutdown();
  host_shutdown();

  memset(&cookies, 0, sizeof(cookies));

  return host_startup();

}


static word32 fst_create(int class, const char *path) {

  word32 pb = get_memory24_c(engine.direct + dp_param_blk_ptr, 0);

  struct file_info fi;
  memset(&fi, 0, sizeof(fi));

  word16 pcount = 0;
  if (class) {
    pcount = get_memory16_c(pb, 0);
    if (pcount >= 2) fi.access = get_memory16_c(pb + CreateRecGS_access, 0);
    if (pcount >= 3) fi.file_type = get_memory16_c(pb + CreateRecGS_fileType, 0);
    if (pcount >= 4) fi.aux_type = get_memory32_c(pb + CreateRecGS_auxType, 0);
    if (pcount >= 5) fi.storage_type = get_memory16_c(pb + CreateRecGS_storageType, 0);
    if (pcount >= 6) fi.eof = get_memory32_c(pb + CreateRecGS_eof, 0);
    if (pcount >= 7) fi.resource_eof = get_memory32_c(pb + CreateRecGS_resourceEOF, 0);


    if (pcount >= 4) {
      host_file_type_to_finder_info(fi.finder_info, fi.file_type, fi.aux_type);
      fi.has_fi = 1;
    }


  } else {

    fi.access = get_memory16_c(pb + CreateRec_fAccess, 0);
    fi.file_type = get_memory16_c(pb + CreateRec_fileType, 0);
    fi.aux_type = get_memory32_c(pb + CreateRec_auxType, 0);
    fi.storage_type = get_memory16_c(pb + CreateRec_storageType, 0);
    fi.create_date = host_get_date_time(pb + CreateRec_createDate);

    host_file_type_to_finder_info(fi.finder_info, fi.file_type, fi.aux_type);
    fi.has_fi = 1;
  }
  int ok;

  if (fi.storage_type == 0 && fi.file_type == 0x0f)
    fi.storage_type = 0x0d;

  if (fi.storage_type == 0x0d) {
    ok = mkdir(path, 0777);
    if (ok < 0) {
      return host_map_errno_path(errno, path);
    }

    if (class) {
      if (pcount >= 5) set_memory16_c(pb + CreateRecGS_storageType, fi.storage_type, 0);
    } else {
      set_memory16_c(pb + CreateRec_storageType, fi.storage_type, 0);
    }

    return 0;
  }
  if (fi.storage_type <= 3 || fi.storage_type == 0x05) {
    // normal file.
    // 0x05 is an extended/resource file but we don't do anything special.
    ok = open(path, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (ok < 0) return host_map_errno_path(errno, path);
    // set ftype, auxtype...
    host_set_file_info(path, &fi);
    close(ok);

    fi.storage_type = 1;

    if (class) {
      if (pcount >= 5) set_memory16_c(pb + CreateRecGS_storageType, fi.storage_type, 0);
    } else {
      set_memory16_c(pb + CreateRec_storageType, fi.storage_type, 0);
    }

    return 0;
  }


  if (fi.storage_type == 0x8005) {
    // convert an existing file to an extended file.
    // this checks that the file exists and has a 0-sized resource.
    word32 rv = host_get_file_info(path, &fi);
    if (rv) return rv;
    if (fi.storage_type == extendedFile) return resExistsErr;
    if (fi.storage_type < seedling || fi.storage_type > tree) return resAddErr;
    return 0;
  }

  // other storage types?
  return badStoreType;
}


static word32 fst_destroy(int class, const char *path) {

  struct stat st;

  if (!path) return badStoreType;

  if (stat(path, &st) < 0) {
    return host_map_errno_path(errno, path);
  }

  // can't delete volume root.
  if (host_is_root(&st))
    return badStoreType;

  int ok = S_ISDIR(st.st_mode) ? rmdir(path) : unlink(path);
#if defined(__linux__)
  if ((ok >= 0) && (!S_ISDIR(st.st_mode))) {
    //delete the resource fork file if the unlink was successful
    char rpath[1024] = {0};
    get_resource_pathname(path, rpath);
    fprintf(stderr,"delete res %s\n",rpath);
    unlink(rpath);
  }
#endif



  if (ok < 0) return host_map_errno_path(errno, path);
  return 0;
}

static word32 fst_set_file_info(int class, const char *path) {


  word32 pb = get_memory24_c(engine.direct + dp_param_blk_ptr, 0);

  struct file_info fi;
  memset(&fi, 0, sizeof(fi));


  // load up existing file types / finder info.
  host_get_file_xinfo(path, &fi);

  word32 option_list = 0;
  if (class) {
    word16 pcount = get_memory16_c(pb, 0);

    if (pcount >= 2) fi.access = get_memory16_c(pb + FileInfoRecGS_access, 0);
    if (pcount >= 3) fi.file_type = get_memory16_c(pb + FileInfoRecGS_fileType, 0);
    if (pcount >= 4) fi.aux_type = get_memory32_c(pb + FileInfoRecGS_auxType, 0);
    // reserved.
    //if (pcount >= 5) fi.storage_type = get_memory16_c(pb + FileInfoRecGS_storageType, 0);
    if (pcount >= 6) fi.create_date = host_get_date_time_rec(pb + FileInfoRecGS_createDateTime);
    if (pcount >= 7) fi.modified_date = host_get_date_time_rec(pb + FileInfoRecGS_modDateTime);
    if (pcount >= 8) option_list = get_memory24_c(pb + FileInfoRecGS_optionList, 0);
    // remainder reserved

    if (pcount >= 4) {
      host_file_type_to_finder_info(fi.finder_info, fi.file_type, fi.aux_type);
      fi.has_fi = 1;
    }

  } else {
    fi.access = get_memory16_c(pb + FileRec_fAccess, 0);
    fi.file_type = get_memory16_c(pb + FileRec_fileType, 0);
    fi.aux_type = get_memory32_c(pb + FileRec_auxType, 0);
    // reserved.
    //fi.storage_type = get_memory32_c(pb + FileRec_storageType, 0);
    fi.create_date = host_get_date_time(pb + FileRec_createDate);
    fi.modified_date = host_get_date_time(pb + FileRec_modDate);

    host_file_type_to_finder_info(fi.finder_info, fi.file_type, fi.aux_type);
    fi.has_fi = 1;
  }

  if (option_list) {
    // total size, req size, fst id, data...
    int total_size = get_memory16_c(option_list + 0, 0);
    int req_size = get_memory16_c(option_list + 2, 0);
    int fst_id = get_memory16_c(option_list + 4, 0);

    int size = req_size - 6;
    if ((fst_id == proDOSFSID || fst_id == hfsFSID || fst_id == appleShareFSID) && size >= 32) {
      fi.has_fi = 1;
      for (int i = 0; i <32; ++i)
        fi.finder_info[i] = get_memory_c(option_list + 6 + i, 0);
    }
  }


  return host_set_file_info(path, &fi);
}

static word32 fst_get_file_info(int class, const char *path) {

  word32 pb = get_memory24_c(engine.direct + dp_param_blk_ptr, 0);

  struct file_info fi;
  int rv = 0;

  rv = host_get_file_info(path, &fi);
  if (rv) return rv;

  if (class) {

    word16 pcount = get_memory16_c(pb, 0);

    if (pcount >= 2) set_memory16_c(pb + FileInfoRecGS_access, fi.access, 0);
    if (pcount >= 3) set_memory16_c(pb + FileInfoRecGS_fileType, fi.file_type, 0);
    if (pcount >= 4) set_memory32_c(pb + FileInfoRecGS_auxType, fi.aux_type, 0);
    if (pcount >= 5) set_memory16_c(pb + FileInfoRecGS_storageType, fi.storage_type, 0);

    if (pcount >= 6) host_set_date_time_rec(pb + FileInfoRecGS_createDateTime, fi.create_date);
    if (pcount >= 7) host_set_date_time_rec(pb + FileInfoRecGS_modDateTime, fi.modified_date);

    if (pcount >= 8) {
      word16 fst_id = hfsFSID;
      //if (fi.storage_type == 0x0f) fst_id = mfsFSID;
      word32 option_list = get_memory24_c(pb + FileInfoRecGS_optionList, 0);
      rv = set_option_list(option_list,  fst_id, fi.finder_info, fi.has_fi ? 32 : 0);
    }
    if (pcount >= 9) set_memory32_c(pb + FileInfoRecGS_eof, fi.eof, 0);
    if (pcount >= 10) set_memory32_c(pb + FileInfoRecGS_blocksUsed, fi.blocks, 0);
    if (pcount >= 11) set_memory32_c(pb + FileInfoRecGS_resourceEOF, fi.resource_eof, 0);
    if (pcount >= 12) set_memory32_c(pb + FileInfoRecGS_resourceBlocks, fi.resource_blocks, 0);

  } else {

    set_memory16_c(pb + FileRec_fAccess, fi.access, 0);
    set_memory16_c(pb + FileRec_fileType, fi.file_type, 0);
    set_memory32_c(pb + FileRec_auxType, fi.aux_type, 0);
    set_memory16_c(pb + FileRec_storageType, fi.storage_type, 0);

    host_set_date_time(pb + FileRec_createDate, fi.create_date);
    host_set_date_time(pb + FileRec_modDate, fi.modified_date);

    set_memory32_c(pb + FileRec_blocksUsed, fi.blocks, 0);
  }

  return rv;
}

static word32 fst_judge_name(int class, char *path) {

  if (class == 0) return invalidClass;


  word32 pb = get_memory24_c(engine.direct + dp_param_blk_ptr, 0);
  word16 pcount = get_memory16_c(pb, 0);
  word16 name_type = get_memory16_c(pb + JudgeNameRecGS_nameType, 0);
  word32 name = pcount >= 5 ? get_memory24_c(pb + JudgeNameRecGS_name, 0) : 0;

  // 255 max length.
  if (pcount >= 4) set_memory16_c(pb + JudgeNameRecGS_maxLen, 255, 0);
  if (pcount >= 6) set_memory16_c(pb + JudgeNameRecGS_nameFlags, 0, 0);

  word16 nameFlags = 0;
  word16 rv = 0;

  if (name) {
    word16 cap = get_memory16_c(name, 0);
    if (cap < 4) return buffTooSmall;
    word16 length = get_memory16_c(name + 2, 0);
    if (length == 0) {
      nameFlags |= 1 << 13;
      rv = set_gsstr(name, "A");
    } else {
      // if volume name, only allow "Host" ?
      if (length > 255) nameFlags |= 1 << 14;
      for (int i = 0; i < length; ++i) {
        char c = get_memory_c(name + 4 + i, 0);
        if (c == 0 || c == ':' || c == '/' || c == '\\') {
          nameFlags |= 1 << 15;
          set_memory_c(name + 4 + i, '.', 0);
        }
      }
    }
  }
  if (pcount >= 6) set_memory16_c(pb + JudgeNameRecGS_nameFlags, nameFlags, 0);
  return rv;
}

static word32 fst_volume(int class) {

  word32 pb = get_memory24_c(engine.direct + dp_param_blk_ptr, 0);

  word32 rv = 0;
  if (class) {
    word16 pcount = get_memory16_c(pb, 0);
    if (pcount >= 2) rv = set_gsstr(get_memory24_c(pb + VolumeRecGS_volName, 0), ":Host");
    // finder bug -- if used blocks is 0, doesn't display header.
    if (pcount >= 3) set_memory32_c(pb + VolumeRecGS_totalBlocks, 0x007fffff, 0);
    if (pcount >= 4) set_memory32_c(pb + VolumeRecGS_freeBlocks, 0x007fffff-1, 0);
    if (pcount >= 5) set_memory16_c(pb + VolumeRecGS_fileSysID, mfsFSID, 0);
    if (pcount >= 6) set_memory16_c(pb + VolumeRecGS_blockSize, 512, 0);
    // handled via gs/os
    //if (pcount >= 7) set_memory16_c(pb + VolumeRecGS_characteristics, 0, 0);
    //if (pcount >= 8) set_memory16_c(pb + VolumeRecGS_deviceID, 0, 0);
  } else {

    // prodos 16 uses / sep.
    rv = set_pstr(get_memory24_c(pb + VolumeRec_volName, 0), "/Host");
    set_memory32_c(pb + VolumeRec_totalBlocks, 0x007fffff, 0);
    set_memory32_c(pb + VolumeRec_freeBlocks, 0x007fffff-1, 0);
    set_memory16_c(pb + VolumeRec_fileSysID, mfsFSID, 0);
  }

  return rv;

}

static word32 fst_clear_backup(int class, const char *path) {
  return invalidFSTop;
}


static int open_data_fork(const char *path, word16 *access, word16 *error) {

  int fd = -1;
  for (;;) {

    switch(*access) {
      case readEnableAllowWrite:
      case readWriteEnable:
        fd = open(path, O_RDWR);
        break;
      case readEnable:
        fd = open(path, O_RDONLY);
        break;
      case writeEnable:
        fd = open(path, O_WRONLY);
        break;
    }

    if (*access == readEnableAllowWrite) {
      if (fd < 0) {
        *access = readEnable;
        continue;
      }
      *access = readWriteEnable;
    }
    break;
  }
  if (fd < 0) {
    *error = host_map_errno_path(errno, path);
  }

  return fd;
}
#if defined(__APPLE__) || defined(__linux__)
static int open_resource_fork(const char *path, word16 *access, word16 *error) {
#if defined(__APPLE__)
  // os x / hfs/apfs don't need to specifically create a resource fork.
  // or do they?

  char *rpath = host_gc_append_path(path, _PATH_RSRCFORKSPEC);
#else
  char rpath[1024]; 
  get_resource_pathname(path, rpath);
#endif

  int fd = -1;
  for (;;) {

    switch(*access) {
      case readEnableAllowWrite:
      case readWriteEnable:
        fd = open(rpath, O_RDWR | O_CREAT, 0666);
        break;
      case readEnable:
        fd = open(rpath, O_RDONLY);
        break;
      case writeEnable:
        fd = open(rpath, O_WRONLY | O_CREAT, 0666);
        break;
    }

    if (*access == readEnableAllowWrite) {
      if (fd < 0) {
        *access = readEnable;
        continue;
      }
      *access = readWriteEnable;
    }
    break;
  }
  if (fd < 0) {
    *error = host_map_errno_path(errno, path);
  }

  return fd;



}
#elif defined __sun
static int open_resource_fork(const char *path, word16 *access, word16 *error) {
  int tmp = open(path, O_RDONLY);
  if (tmp < 0) {
    *error = host_map_errno_path(errno, path);
    return -1;
  }

  int fd = -1;
  for(;;) {

    switch(*access) {
      case readEnableAllowWrite:
      case readWriteEnable:
        fd = openat(tmp, XATTR_RESOURCEFORK_NAME, O_RDWR | O_CREAT | O_XATTR, 0666);
        break;
      case readEnable:
        fd = openat(tmp, XATTR_RESOURCEFORK_NAME, O_RDONLY | O_XATTR);
        break;
      case writeEnable:
        fd = openat(tmp, XATTR_RESOURCEFORK_NAME, O_WRONLY | O_CREAT | O_XATTR, 0666);
        break;
    }

    if (*access == readEnableAllowWrite) {
      if (fd < 0) {
        *access = readEnable;
        continue;
      }
      *access = readWriteEnable;
    }
    break;
  }

  if (fd < 0) {
    *error = host_map_errno_path(errno, path);
    close(tmp);
    return -1;
  }
  close(tmp);

  return fd;
}
#else
static int open_resource_fork(const char *path, word16 *access, word16 *error) {
  *error = resForkNotFound;
  return -1;
}
#endif


static word32 fst_open(int class, const char *path) {


  word32 pb = get_memory24_c(engine.direct + dp_param_blk_ptr, 0);

  struct file_info fi;
  word16 rv = 0;

  rv = host_get_file_info(path, &fi);
  if (rv) return rv;


  int fd = -1;
  int type = file_regular;
  struct directory *dd = NULL;

  word16 pcount = 0;
  word16 request_access = readEnableAllowWrite;
  word16 access = 0;
  word16 resource_number = 0;
  if (class) {
    pcount = get_memory16_c(pb, 0);
    if (pcount >= 3) request_access = get_memory16_c(pb + OpenRecGS_requestAccess, 0);
    if (pcount >= 4) resource_number = get_memory16_c(pb + OpenRecGS_resourceNumber, 0);
  }

  if (resource_number) {
    if (resource_number > 1) return paramRangeErr;
    type = file_resource;
  }

  if (access > 3) return paramRangeErr;

  // special access checks for directories.
  if (fi.storage_type == 0x0d || fi.storage_type == 0x0f) {
    if (resource_number) return resForkNotFound;
    switch (request_access) {
      case readEnableAllowWrite:
        request_access = readEnable;
        break;
      case writeEnable:
      case readWriteEnable:
        return invalidAccess;
    }
    type = file_directory;
  }


  if (g_cfg_host_read_only) {
    switch (request_access) {
      case readEnableAllowWrite:
        request_access = readEnable;
        break;
      case readWriteEnable:
      case writeEnable:
        return invalidAccess;
        break;
    }
  }

  access = request_access;
  switch(type) {
    case file_regular:
      fd = open_data_fork(path, &access, &rv);
      break;
    case file_resource:
      fd = open_resource_fork(path, &access, &rv);
      break;
    case file_directory:
      dd = read_directory(path, &rv);
      break;
  }

  if (rv) return rv;


  if (class) {
    if (pcount >= 5) set_memory16_c(pb + OpenRecGS_access, access, 0);
    if (pcount >= 6) set_memory16_c(pb + OpenRecGS_fileType, fi.file_type, 0);
    if (pcount >= 7) set_memory32_c(pb + OpenRecGS_auxType, fi.aux_type, 0);
    if (pcount >= 8) set_memory16_c(pb + OpenRecGS_storageType, fi.storage_type, 0);

    if (pcount >= 9) host_set_date_time_rec(pb + OpenRecGS_createDateTime, fi.create_date);
    if (pcount >= 10) host_set_date_time_rec(pb + OpenRecGS_modDateTime, fi.modified_date);

    if (pcount >= 11) {
      word16 fst_id = hfsFSID;
      //if (fi.storage_type == 0x0f) fst_id = mfsFSID;

      word32 option_list = get_memory24_c(pb + OpenRecGS_optionList, 0);
      word32 tmp = set_option_list(option_list, fst_id, fi.finder_info, fi.has_fi ? 32 : 0);
      if (!rv) rv = tmp;
    }

    if (pcount >= 12) set_memory32_c(pb + OpenRecGS_eof, fi.eof, 0);
    if (pcount >= 13) set_memory32_c(pb + OpenRecGS_blocksUsed, fi.blocks, 0);
    if (pcount >= 14) set_memory32_c(pb + OpenRecGS_resourceEOF, fi.resource_eof, 0);
    if (pcount >= 15) set_memory32_c(pb + OpenRecGS_resourceBlocks, fi.resource_blocks, 0);


  }
  // prodos 16 doesn't return anything in the parameter block.

  struct fd_entry *e = alloc_fd();
  if (!e) {
    if (fd >=0) close(fd);
    if (dd) free_directory(dd);
    return outOfMem;
  }
  e->fd = fd;
  e->dir = dd;

  e->cookie = alloc_cookie();
  if (!e->cookie) {
    free_fd(e);
    return tooManyFilesOpen;
  }

  if (type == file_regular) {

    if (g_cfg_host_crlf) {
      if (fi.file_type == 0x04 || fi.file_type == 0xb0)
        e->translate = translate_crlf;
    }

    if (g_cfg_host_merlin && fi.file_type == 0x04) {
      int n = strlen(path);
      if (n >= 3 && path[n-1] == 'S' && path[n-2] == '.')
        e->translate = translate_merlin;
    }
  }

  e->access = access;
  e->path = strdup(path);
  e->type = type;


  // insert it in the linked list.
  e->next = fd_head;
  fd_head = e;

  engine.xreg = e->cookie;
  engine.yreg = access; // actual access, needed in fcr.

  return rv;
}

static word32 fst_read(int class) {

  int cookie = engine.yreg;
  struct fd_entry *e = find_fd(cookie);

  if (!e) return invalidRefNum;

  switch (e->type) {
    case file_directory:
      return badStoreType;
  }

  word32 pb = get_memory24_c(engine.direct + dp_param_blk_ptr, 0);

  word32 data_buffer = 0;
  word32 request_count = 0;
  word32 transfer_count = 0;

  if (class) {
    data_buffer = get_memory24_c(pb + IORecGS_dataBuffer, 0);
    request_count = get_memory24_c(pb + IORecGS_requestCount, 0);
    // pre-zero transfer count
    set_memory32_c(pb + IORecGS_transferCount, 0, 0);
  } else {
    data_buffer = get_memory24_c(pb + FileIORec_dataBuffer, 0);
    request_count = get_memory24_c(pb + FileIORec_requestCount, 0);
    set_memory32_c(pb + FileIORec_transferCount, 0, 0);
  }

  if (request_count == 0) return 0;


  word16 newline_mask;
  word32 rv = 0;
  ssize_t ok;

  newline_mask = get_memory16_c(global_buffer, 0);
  if (newline_mask) {
    byte newline_table[256];
    for (int i = 0; i < 256; ++i)
      newline_table[i] = get_memory_c(global_buffer + 2 + i, 0);

    for (word32 i = 0; i < request_count; ++i) {
      byte b;
      ok = safe_read(e, &b, 1);
      if (ok < 0) return host_map_errno(errno);
      if (ok == 0) break;
      transfer_count++;
      set_memory_c(data_buffer++, b, 0);
      if (newline_table[b & newline_mask]) break;
    }
    if (transfer_count == 0) rv = eofEncountered;
  }
  else {
    byte *data = host_gc_malloc(request_count);
    if (!data) return outOfMem;


    ok = safe_read(e, data, request_count);
    if (ok < 0) rv = host_map_errno(errno);
    if (ok == 0) rv = eofEncountered;
    if (ok > 0) {
      transfer_count = ok;
      for (size_t i = 0; i < ok; ++i) {
        set_memory_c(data_buffer + i, data[i], 0);
      }
    }
  }

  if (transfer_count) {
    if (class)
      set_memory32_c(pb + IORecGS_transferCount, transfer_count, 0);
    else
      set_memory32_c(pb + FileIORec_transferCount, transfer_count, 0);
  }

  return rv;
}

static word32 fst_write(int class) {

  int cookie = engine.yreg;
  struct fd_entry *e = find_fd(cookie);

  if (!e) return invalidRefNum;

  if (!(e->access & writeEnable))
    return invalidAccess;

  switch (e->type) {
    case file_directory:
      return badStoreType;
  }


  word32 pb = get_memory24_c(engine.direct + dp_param_blk_ptr, 0);

  word32 data_buffer = 0;
  word32 request_count = 0;

  if (class) {
    data_buffer = get_memory24_c(pb + IORecGS_dataBuffer, 0);
    request_count = get_memory24_c(pb + IORecGS_requestCount, 0);
    // pre-zero transfer count
    set_memory32_c(pb + IORecGS_transferCount, 0, 0);
  } else {
    data_buffer = get_memory24_c(pb + FileIORec_dataBuffer, 0);
    request_count = get_memory24_c(pb + FileIORec_requestCount, 0);
    set_memory32_c(pb + FileIORec_transferCount, 0, 0);
  }

  if (request_count == 0) return 0;
  byte *data = host_gc_malloc(request_count);
  if (!data) return outOfMem;

  for (word32 i = 0; i < request_count; ++i) {
    data[i] = get_memory_c(data_buffer + i,0);
  }

  word32 rv = 0;
  ssize_t ok = safe_write(e, data, request_count);
  if (ok < 0) rv = host_map_errno(errno);
  if (ok > 0) {
    if (class)
      set_memory32_c(pb + IORecGS_transferCount, ok, 0);
    else
      set_memory32_c(pb + FileIORec_transferCount, ok, 0);
  }
  return rv;
}

static word32 fst_close(int class) {

  int cookie = engine.yreg;

  return remove_fd(cookie);

}

static word32 fst_flush(int class) {

  int cookie = engine.yreg;
  struct fd_entry *e = find_fd(cookie);

  if (!e) return invalidRefNum;

  switch (e->type) {
    case file_directory:
      return badStoreType;
  }

  int ok = fsync(e->fd);
  if (ok < 0) return host_map_errno(errno);
  return 0;
}

static off_t get_offset(int fd, word16 base, word32 displacement) {

  off_t pos = lseek(fd, 0, SEEK_CUR);
  off_t eof = lseek(fd, 0, SEEK_END);
  lseek(fd, pos, SEEK_SET);

  switch (base) {
    case startPlus:
      return displacement;
      break;
    case eofMinus:
      return eof - displacement;
      break;
    case markPlus:
      return pos + displacement;
      break;
    case markMinus:
      return pos - displacement;
      break;
    default:
      return -1;
  }


}

static word32 fst_set_mark(int class) {

  int cookie = engine.yreg;

  struct fd_entry *e = find_fd(cookie);
  if (!e) return invalidRefNum;

  switch (e->type) {
    case file_directory:
      return badStoreType;
  }

  word32 pb = get_memory24_c(engine.direct + dp_param_blk_ptr, 0);

  word16 base = 0;
  word32 displacement = 0;
  if (class) {
    base = get_memory16_c(pb + SetPositionRecGS_base, 0);
    displacement = get_memory32_c(pb + SetPositionRecGS_displacement, 0);
  } else {
    displacement = get_memory32_c(pb + MarkRec_position, 0);
  }
  if (base > markMinus) return paramRangeErr;

  off_t offset = get_offset(e->fd, base, displacement);
  if (offset < 0) return outOfRange;

  off_t ok = lseek(e->fd, offset, SEEK_SET);
  if (ok < 0) return host_map_errno(errno);
  return 0;
}

static word32 fst_set_eof(int class) {

  int cookie = engine.yreg;

  struct fd_entry *e = find_fd(cookie);
  if (!e) return invalidRefNum;

  switch (e->type) {
    case file_directory:
      return badStoreType;
  }


  word32 pb = get_memory24_c(engine.direct + dp_param_blk_ptr, 0);

  word16 base = 0;
  word32 displacement = 0;
  if (class) {
    base = get_memory16_c(pb + SetPositionRecGS_base, 0);
    displacement = get_memory32_c(pb + SetPositionRecGS_displacement, 0);
  } else {
    displacement = get_memory32_c(pb + EOFRec_eofPosition, 0);
  }

  if (base > markMinus) return paramRangeErr;

  off_t offset = get_offset(e->fd, base, displacement);
  if (offset < 0) return outOfRange;

  int ok = ftruncate(e->fd, offset);
  if (ok < 0) return host_map_errno(errno);
  return 0;

}

static word32 fst_get_mark(int class) {

  int cookie = engine.yreg;

  struct fd_entry *e = find_fd(cookie);
  if (!e) return invalidRefNum;

  word32 pb = get_memory24_c(engine.direct + dp_param_blk_ptr, 0);

  off_t pos = 0;

  switch (e->type) {
    case file_directory:
      return badStoreType;
  }

  pos = lseek(e->fd, 0, SEEK_CUR);
  if (pos < 0) return host_map_errno(errno);

  if (class) {
    set_memory32_c(pb + PositionRecGS_position, pos, 0);
  } else {
    set_memory32_c(pb + MarkRec_position, pos, 0);
  }

  return 0;
}

static word32 fst_get_eof(int class) {

  int cookie = engine.yreg;

  struct fd_entry *e = find_fd(cookie);
  if (!e) return invalidRefNum;

  word32 pb = get_memory24_c(engine.direct + dp_param_blk_ptr, 0);


  switch (e->type) {
    case file_directory:
      return badStoreType;
  }


  off_t eof = 0;
  off_t pos = 0;

  pos = lseek(e->fd, 0, SEEK_CUR);
  eof = lseek(e->fd, 0, SEEK_END);
  if (eof < 0) return host_map_errno(errno);
  lseek(e->fd, pos, SEEK_SET);


  if (class) {
    set_memory32_c(pb + PositionRecGS_position, eof, 0);
  } else {
    set_memory32_c(pb + MarkRec_position, eof, 0);
  }

  return 0;
}

static void free_directory(struct directory *dd) {
  if (!dd) return;
  for (int i = 0; i < dd->num_entries; ++i) {
    free(dd->entries[i]);
  }
  free(dd);
}

static int qsort_callback(const void *a, const void *b) {
  return strcmp(*(const char **)a, *(const char **)b);
}

/*
 * exclude files from get_dir_entries.
 *
 */
static int filter_directory_entry(const char *name) {
  if (!name[0]) return 1;
  if (name[0] == '.') {
    return 1;
    /*
       if (!strcmp(name, ".")) return 1;
       if (!strcmp(name, "..")) return 1;
       if (!strncmp(name, "._", 2)) return 1; // ._ resource fork
       if (!strcmp(name, ".fseventsd")) return 1;
     */
  }
  return 0;
}


static struct directory *read_directory(const char *path, word16 *error) {
  DIR *dirp;
  struct directory *dd;
  int capacity = 100;
  int size = sizeof(struct directory) + capacity * sizeof(char *);

  dirp = opendir(path);
  if (!dirp) {
    *error = host_map_errno_path(errno, path);
    return NULL;
  }

  dd = (struct directory *)malloc(size);
  if (!dd) {
    *error = outOfMem;
    closedir(dirp);
    return NULL;
  }
  memset(dd, 0, size);

  for(;;) {
    struct dirent *d = readdir(dirp);
    if (!d) break;
    if (filter_directory_entry(d->d_name)) continue;

    if (dd->num_entries >= capacity) {
      capacity += capacity;
      size = sizeof(struct directory) + capacity * sizeof(char *);
      struct directory * tmp = realloc(dd, size);
      if (!tmp) {
        *error = host_map_errno(errno);
        free_directory(dd);
        closedir(dirp);
        return NULL;
      }
      dd = tmp;
    }
    dd->entries[dd->num_entries++] = strdup(d->d_name);
  };

  closedir(dirp);

  // sort them....
  qsort(dd->entries, dd->num_entries, sizeof(char *), qsort_callback);

  return dd;
}


static word32 fst_get_dir_entry(int class) {


  int cookie = engine.yreg;

  struct fd_entry *e = find_fd(cookie);
  if (!e) return invalidRefNum;


  if (e->type != file_directory) return badFileFormat;

  word32 pb = get_memory24_c(engine.direct + dp_param_blk_ptr, 0);

  word16 base = 0;
  word16 pcount = 0;
  word32 displacement = 0;
  word32 name = 0;

  if (class) {
    pcount = get_memory16_c(pb, 0);
    base = get_memory16_c(pb + DirEntryRecGS_base, 0);
    displacement = get_memory16_c(pb + DirEntryRecGS_displacement, 0);
    name = get_memory24_c(pb + DirEntryRecGS_name, 0);
  } else {
    base = get_memory16_c(pb + DirEntryRec_base, 0);
    displacement = get_memory16_c(pb + DirEntryRec_displacement, 0);
    name = get_memory24_c(pb + DirEntryRec_nameBuffer, 0);
  }

  if (base == 0 && displacement == 0) {
    // count them up.
    int count = e->dir->num_entries;
    e->dir->displacement = 0;

    if (class) {
      if (pcount >= 6) set_memory16_c(pb + DirEntryRecGS_entryNum, count, 0);
    }
    else {
      set_memory16_c(pb + DirEntryRec_entryNum, count, 0);
    }

    return 0;
  }

  int dir_displacement = e->dir->displacement;
  switch (base) {
    case 0:             // displacement is absolute entry number.
      break;
    case 1:             // displacement is added to the current displacement.
      displacement = dir_displacement + displacement;
      break;
    case 2:             // displacement is substracted from current displacement.
      displacement = dir_displacement - displacement;
      break;
    default:
      return paramRangeErr;
  }
  //if (displacement) --displacement;
  --displacement;
  if (displacement < 0) return endOfDir;
  if (displacement >= e->dir->num_entries) return endOfDir;


  word32 rv = 0;
  const char *dname = e->dir->entries[displacement++];
  e->dir->displacement = displacement;
  char *fullpath = host_gc_append_path(e->path, dname);
  struct  file_info fi;
  rv = host_get_file_info(fullpath, &fi);

  if (dname) fprintf(stderr, " - %s", dname);

  // p16 and gs/os both use truncating c1 output string.
  rv = set_gsstr_truncate(name, dname);

  if (class) {

    if (pcount > 2) set_memory16_c(pb + DirEntryRecGS_flags, fi.storage_type == 0x05 ? 0x8000 : 0, 0);

    if (pcount >= 6) set_memory16_c(pb + DirEntryRecGS_entryNum, displacement, 0);
    if (pcount >= 7) set_memory16_c(pb + DirEntryRecGS_fileType, fi.file_type, 0);
    if (pcount >= 8) set_memory32_c(pb + DirEntryRecGS_eof, fi.eof, 0);
    if (pcount >= 9) set_memory32_c(pb + DirEntryRecGS_blockCount, fi.blocks, 0);

    if (pcount >= 10) host_set_date_time_rec(pb + DirEntryRecGS_createDateTime, fi.create_date);
    if (pcount >= 11) host_set_date_time_rec(pb + DirEntryRecGS_modDateTime, fi.modified_date);

    if (pcount >= 12) set_memory16_c(pb + DirEntryRecGS_access, fi.access, 0);
    if (pcount >= 13) set_memory32_c(pb + DirEntryRecGS_auxType, fi.aux_type, 0);
    if (pcount >= 14) set_memory16_c(pb + DirEntryRecGS_fileSysID, mfsFSID, 0);

    if (pcount >= 15) {
      word16 fst_id = hfsFSID;
      //if (fi.storage_type == 0x0f) fst_id = mfsFSID;
      word32 option_list = get_memory24_c(pb + DirEntryRecGS_optionList, 0);
      word32 tmp = set_option_list(option_list, fst_id, fi.finder_info, fi.has_fi ? 32 : 0);
      if (!rv) rv = tmp;
    }

    if (pcount >= 16) set_memory32_c(pb + DirEntryRecGS_resourceEOF, fi.resource_eof, 0);
    if (pcount >= 17) set_memory32_c(pb + DirEntryRecGS_resourceBlocks, fi.resource_blocks, 0);
  }
  else {
    set_memory16_c(pb + DirEntryRec_flags, fi.storage_type == 0x05 ? 0x8000 : 0, 0);

    set_memory16_c(pb + DirEntryRec_entryNum, displacement, 0);
    set_memory16_c(pb + DirEntryRec_fileType, fi.file_type, 0);
    set_memory32_c(pb + DirEntryRec_endOfFile, fi.eof, 0);
    set_memory32_c(pb + DirEntryRec_blockCount, fi.blocks, 0);

    host_set_date_time_rec(pb + DirEntryRec_createTime, fi.create_date);
    host_set_date_time_rec(pb + DirEntryRec_modTime, fi.modified_date);

    set_memory16_c(pb + DirEntryRec_access, fi.access, 0);
    set_memory32_c(pb + DirEntryRec_auxType, fi.aux_type, 0);
    set_memory16_c(pb + DirEntryRec_fileSysID, mfsFSID, 0);

  }

  return rv;
}

static word32 fst_change_path(int class, const char *path1, const char *path2) {

  /* make sure they're not trying to rename the volume... */
  struct stat st;
  if (stat(path1, &st) < 0) return host_map_errno_path(errno, path1);
  if (host_is_root(&st))
    return invalidAccess;

  // rename will delete any previous file. ChangePath should return an error.
  if (stat(path2, &st) == 0) return dupPathname;
#if defined (__linux__)
  //on linux rename both the file and the resource file.
  char rpath1[1024], rpath2[1024]; 
  get_resource_pathname(path1, rpath1);
  get_resource_pathname(path2, rpath2);
  if(!S_ISDIR(st.st_mode)) { //directories don't have resource forks.
      if (stat(rpath2,&st) == 0) return dupPathname; //abort if rpath2 already exists
      if (stat(rpath1,&st) == 0) //Only rename resource fork file if it exists.
          rename(rpath1, rpath2);
  }
#endif
  if (rename(path1, path2) < 0) return host_map_errno_path(errno, path2);
  return 0;
}

static word32 fst_format(int class) {
  return notBlockDev;
}

static word32 fst_erase(int class) {
  return notBlockDev;
}

static const char *call_name(word16 call) {
  static char* class1[] = {
    // 0x00
    "",
    "CreateGS",
    "DestroyGS",
    "",
    "ChangePathGS",
    "SetFileInfoGS",
    "GetFileInfoGS",
    "JudgeNameGS",
    "VolumeGS",
    "",
    "",
    "ClearBackupGS",
    "",
    "",
    "",
    "",

    // 0x10
    "OpenGS",
    "",
    "ReadGS",
    "WriteGS",
    "CloseGS",
    "FlushGS",
    "SetMarkGS",
    "GetMarkGS",
    "SetEOFGS",
    "GetEOFGS",
    "",
    "",
    "GetDirEntryGS",
    "",
    "",
    "",

    // 0x20
    "",
    "",
    "",
    "",
    "FormatGS",
    "EraseDiskGS",
  };


  static char* class0[] = {
    // 0x00
    "",
    "CREATE",
    "DESTROY",
    "",
    "CHANGE_PATH",
    "SET_FILE_INFO",
    "GET_FILE_INFO",
    "",
    "VOLUME",
    "",
    "",
    "CLEAR_BACKUP_BIT",
    "",
    "",
    "",
    "",

    // 0X10
    "OPEN",
    "",
    "READ",
    "WRITE",
    "CLOSE",
    "FLUSH",
    "SET_MARK",
    "GET_MARK",
    "SET_EOF",
    "GET_EOF",
    "",
    "",
    "GET_DIR_ENTRY",
    "",
    "",
    "",

    // 0X20
    "",
    "",
    "",
    "",
    "FORMAT",
    "ERASE_DISK",
  };



  if (call & 0x8000) {
    static char *sys[] = {
      "",
      "fst_startup",
      "fst_shutdown",
      "fst_remove_vcr",
      "fst_deferred_flush"
    };
    call &= ~0x8000;

    if (call < sizeof(sys) / sizeof(sys[0]))
      return sys[call];

    return "";
  }

  int class = call >> 13;
  call &= 0x1fff;
  switch(class) {
    case 0:
      if (call < sizeof(class0) / sizeof(class0[0]))
        return class0[call];
      break;
    case 1:
      if (call < sizeof(class1) / sizeof(class1[0]))
        return class1[call];
      break;

  }
  return "";
}


void host_fst(void) {

  /*
   * input:
   * c = set
   * a = default error code
   * x = gs/os callnum
   * y = [varies]
   *
   * output:
   * c = set/clear
   * a = error code/0
   * x = varies
   * y = varies
   */


  word32 acc = 0;
  word16 call = engine.xreg;

  fprintf(stderr, "Host FST: %04x %s", call, call_name(call));


  if (call & 0x8000) {
    fputs("\n", stderr);
    // system level.
    switch(call) {
      case 0x8001:
        acc = fst_startup();
        break;
      case 0x8002:
        acc = fst_shutdown();
        break;
      default:
        acc = badSystemCall;
        break;
    }
  } else {

    if (!host_root) {
      acc = networkError;
      engine.acc =  acc;
      SEC();
      fprintf(stderr, "          %02x   %s\n", acc, host_error_name(acc));

      return;
    }

    int class = call >> 13;
    call &= 0x1fff;

    if (class > 1) {
      acc = invalidClass;
      engine.acc = acc;
      SEC();
      fprintf(stderr, "          %02x   %s\n", acc, host_error_name(acc));

      return;
    }

    char *path1 = NULL;
    char *path2 = NULL;
    char *path3 = NULL;
    char *path4 = NULL;
    const char *cp;

    switch(call & 0xff) {
      case 0x01:
      case 0x02:
      case 0x05:
      case 0x06:
      case 0x0b:
      case 0x10:
        path1 = get_path1();
        break;
      case 0x04:
        path1 = get_path1();
        path2 = get_path2();
        break;
    }

    if (path1) fprintf(stderr, " - %s", path1);
    if (path2) fprintf(stderr, " - %s", path2);

    switch(call & 0xff) {
      case 0x01:
        cp = check_path(path1, &acc);
        if (acc) break;
        path3 = host_gc_append_path(host_root, cp);

        acc = fst_create(class, path3);
        break;
      case 0x02:
        cp = check_path(path1, &acc);
        if (acc) break;
        path3 = host_gc_append_path(host_root, cp);

        acc = fst_destroy(class, path3);
        break;
      case 0x04:

        cp = check_path(path1, &acc);
        if (acc) break;
        path3 = host_gc_append_path(host_root, cp);

        cp = check_path(path2, &acc);
        if (acc) break;
        path4 = host_gc_append_path(host_root, cp);

        acc = fst_change_path(class, path3, path4);
        break;
      case 0x05:
        cp = check_path(path1, &acc);
        if (acc) break;
        path3 = host_gc_append_path(host_root, cp);

        acc = fst_set_file_info(class, path3);
        break;
      case 0x06:
        cp = check_path(path1, &acc);
        if (acc) break;
        path3 = host_gc_append_path(host_root, cp);
        acc = fst_get_file_info(class, path3);
        break;
      case 0x07:
        acc = fst_judge_name(class, path1);
        break;
      case 0x08:
        acc = fst_volume(class);
        break;
      case 0x0b:
        cp = check_path(path1, &acc);
        if (acc) break;
        path3 = host_gc_append_path(host_root, cp);
        acc = fst_clear_backup(class, path3);
        break;
      case 0x10:
        cp = check_path(path1, &acc);
        if (acc) break;
        path3 = host_gc_append_path(host_root, cp);
        acc = fst_open(class, path3);
        break;
      case 0x012:
        acc = fst_read(class);
        break;
      case 0x013:
        acc = fst_write(class);
        break;
      case 0x14:
        acc = fst_close(class);
        break;
      case 0x15:
        acc = fst_flush(class);
        break;
      case 0x16:
        acc = fst_set_mark(class);
        break;
      case 0x17:
        acc = fst_get_mark(class);
        break;
      case 0x18:
        acc = fst_set_eof(class);
        break;
      case 0x19:
        acc = fst_get_eof(class);
        break;
      case 0x1c:
        acc = fst_get_dir_entry(class);
        break;
      case 0x24:
        acc = fst_format(class);
        break;
      case 0x25:
        acc = fst_erase(class);
        break;

      default:
        acc = invalidFSTop;
        break;
    }
    fputs("\n", stderr);
  }

  if (acc) fprintf(stderr, "          %02x   %s\n", acc, host_error_name(acc));

  host_gc_free();

  engine.acc = acc;
  if (acc) SEC();
  else CLC();
}
