/* The files a new game version or firmware is supported from (dump.h), saved by the payload.
 *
 *   DUMP_ROOT/<name>/                    <name>: <TITLE ID>-<APP_VER>-fw<x.xx>, or fw<x.xx> with no game
 *     info.txt                           firmware, title, APP_VER, VERSION, content ID, what was saved
 *     dump.log                           every step, and anything that failed
 *     firmware/                          the system libraries the payload and agent use, decrypted:
 *                                        libSceAgcDriver, libSceAgc, libSceHmd2, libSceVrTracker2,
 *                                        libSceVideoOut, libSceVrHand, SceSysAvControl.elf
 *     game/appmeta/                      the installed game's param.sfo / param.json (its version)
 *     game/app0/, game/patch0/           eboot.bin and every .prx/.sprx/.elf decrypted, sce_sys/param.*
 *                                        and changeinfo
 *     game/files-app0.txt, files-patch0.txt   every file of the game with its size
 *     game/addcont.txt                   the game's installed add-ons (folder names only)
 *   DUMP_ROOT/<name>.tar                 all of the above in one file, to send
 *
 * Read only on the system's and the game's files; it writes only under DUMP_ROOT. No account, save
 * data, keys (sce_sys/keystone, nptitle.dat) or console identifiers are read.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <ps5/kernel.h>

#include "dump.h"
#include "../payload/selfpager.h"

#define OUT_ROOT DUMP_ROOT
#define SMALL_MAX (1 << 20) /* a metadata file copied as it is: at most 1 MiB */
static FILE *g_log;
static unsigned g_ok, g_fail;

static void dlog(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  if (g_log) {
    vfprintf(g_log, fmt, ap);
    fputc('\n', g_log);
    fflush(g_log);
  }
  va_end(ap);
}
static void mkdirs(const char *path) {
  char p[1024];
  snprintf(p, sizeof p, "%s", path);
  for (char *s = p + 1; *s; s++)
    if (*s == '/') {
      *s = 0;
      mkdir(p, 0777);
      *s = '/';
    }
  mkdir(p, 0777);
}
static int write_all(int fd, const void *data, size_t size) {
  const char *p = data;
  while (size) {
    ssize_t w = write(fd, p, size);
    if (w <= 0)
      return -1;
    p += w, size -= (size_t)w;
  }
  return 0;
}
static int write_file(const char *path, const void *data, size_t size) {
  char dir[1024];
  snprintf(dir, sizeof dir, "%s", path);
  char *slash = strrchr(dir, '/');
  if (slash) {
    *slash = 0;
    mkdirs(dir);
  }
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fd < 0)
    return -1;
  int rc = write_all(fd, data, size);
  close(fd);
  return rc;
}
/* a whole small file into memory (free it); NULL if missing, empty or over max */
static char *read_small(const char *path, size_t max, size_t *size) {
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return NULL;
  struct stat st;
  char *buf = NULL;
  if (fstat(fd, &st) == 0 && st.st_size > 0 && (size_t)st.st_size <= max && (buf = malloc((size_t)st.st_size))) {
    if (read(fd, buf, (size_t)st.st_size) != st.st_size)
      free(buf), buf = NULL;
    else
      *size = (size_t)st.st_size;
  }
  close(fd);
  return buf;
}
static int copy_small(const char *src, const char *dst) {
  size_t n = 0;
  char *buf = read_small(src, SMALL_MAX, &n);
  if (!buf)
    return dlog("FAIL read %s", src), g_fail++, -1;
  int rc = write_file(dst, buf, n);
  free(buf);
  if (rc == 0)
    dlog("ok   %s -> %s (copied)", src, dst), g_ok++;
  else
    dlog("FAIL write %s", dst), g_fail++;
  return rc;
}
static int dump_self(const char *src, const char *dst) {
  int fd = open(src, O_RDONLY);
  if (fd < 0)
    return dlog("FAIL open %s (errno %d)", src, errno), g_fail++, -1;
  char *elf = NULL;
  uint64_t size = 0;
  int rc = decrypt_self(fd, &elf, &size);
  close(fd);
  if (rc != 0 || !elf)
    return dlog("FAIL decrypt %s (%d)", src, rc), g_fail++, -1;
  rc = write_file(dst, elf, size);
  if (rc == 0)
    dlog("ok   %s -> %s (%lu bytes, decrypted)", src, dst, (unsigned long)size), g_ok++;
  else
    dlog("FAIL write %s", dst), g_fail++;
  munmap(elf, size);
  return rc;
}

/* ---- param.sfo: the few keys a profile needs ---- */
static void sfo_get(const char *sfo, size_t n, const char *key, char *out, size_t cap) {
  out[0] = 0;
  if (n < 20 || memcmp(sfo, "\0PSF", 4) != 0)
    return;
  uint32_t keys = *(const uint32_t *)(sfo + 8), data = *(const uint32_t *)(sfo + 12),
           count = *(const uint32_t *)(sfo + 16);
  for (uint32_t i = 0; i < count && 20 + (size_t)(i + 1) * 16 <= n; i++) {
    const char *e = sfo + 20 + (size_t)i * 16;
    uint16_t koff = *(const uint16_t *)e, fmt = *(const uint16_t *)(e + 2);
    uint32_t len = *(const uint32_t *)(e + 4), doff = *(const uint32_t *)(e + 12);
    size_t k = (size_t)keys + koff, at = (size_t)data + doff;
    if (k >= n || strncmp(sfo + k, key, n - k) != 0)
      continue;
    if (fmt == 0x0404 && at + 4 <= n)
      snprintf(out, cap, "0x%x", *(const uint32_t *)(sfo + at));
    else if (at + len <= n)
      snprintf(out, cap, "%.*s", (int)(len < cap ? len : cap - 1), sfo + at);
    return;
  }
}

/* ---- the game's files ---- */
static int is_self_name(const char *n) {
  size_t l = strlen(n);
  return !strcmp(n, "eboot.bin") || (l > 4 && (!strcmp(n + l - 4, ".prx") || !strcmp(n + l - 4, ".elf"))) ||
         (l > 5 && (!strcmp(n + l - 5, ".sprx") || !strcmp(n + l - 5, ".self")));
}
/* sce_sys metadata worth having (never keystone, nptitle.dat or the trophies) */
static int is_meta(const char *rel) {
  return !strcmp(rel, "sce_sys/param.sfo") || !strcmp(rel, "sce_sys/param.json") ||
         !strncmp(rel, "sce_sys/changeinfo/", 19);
}
/* every file listed with its size; the SELFs decrypted, the metadata copied */
static void walk(const char *root, const char *rel, FILE *list, const char *out, int depth) {
  char path[1024];
  snprintf(path, sizeof path, "%s%s%s", root, *rel ? "/" : "", rel);
  DIR *d = depth < 16 ? opendir(path) : NULL;
  if (!d)
    return;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
      continue;
    char r2[1024], full[1100], dst[1100];
    snprintf(r2, sizeof r2, "%s%s%s", rel, *rel ? "/" : "", e->d_name);
    snprintf(full, sizeof full, "%s/%s", root, r2);
    struct stat st;
    if (lstat(full, &st) != 0)
      continue;
    if (S_ISDIR(st.st_mode)) {
      walk(root, r2, list, out, depth + 1);
      continue;
    }
    fprintf(list, "%12lld %s\n", (long long)st.st_size, r2);
    snprintf(dst, sizeof dst, "%s/%s", out, r2);
    if (is_self_name(e->d_name))
      dump_self(full, dst);
    else if (is_meta(r2))
      copy_small(full, dst);
  }
  closedir(d);
}
static void list_names(const char *dirpath, const char *listpath) {
  DIR *d = opendir(dirpath);
  if (!d) {
    dlog("%s: not there (no add-ons installed?)", dirpath);
    return;
  }
  FILE *f = fopen(listpath, "w");
  struct dirent *e;
  unsigned n = 0;
  while (f && (e = readdir(d)))
    if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
      fprintf(f, "%s\n", e->d_name), n++;
  if (f)
    fclose(f);
  closedir(d);
  dlog("ok   %s: %u add-on folders listed in %s", dirpath, n, listpath), g_ok++;
}

/* ---- one .tar of the folder (ustar; names up to 255 characters) ---- */
static int tar_header(int fd, const char *name, long long size, int dir) {
  char h[512];
  memset(h, 0, sizeof h);
  size_t l = strlen(name);
  if (l <= 100)
    memcpy(h, name, l);
  else {
    const char *cut = NULL; /* the prefix (155) / name (100) split at a slash */
    for (const char *s = name; *s; s++)
      if (*s == '/' && s - name <= 155 && strlen(s + 1) <= 100 && s[1])
        cut = s;
    if (!cut)
      return -1;
    memcpy(h + 345, name, (size_t)(cut - name));
    memcpy(h, cut + 1, strlen(cut + 1));
  }
  snprintf(h + 100, 8, "%07o", dir ? 0755 : 0644);
  snprintf(h + 108, 8, "%07o", 0);
  snprintf(h + 116, 8, "%07o", 0);
  snprintf(h + 124, 12, "%011llo", size);
  snprintf(h + 136, 12, "%011o", 0);
  h[156] = dir ? '5' : '0';
  memcpy(h + 257, "ustar", 6);
  memcpy(h + 263, "00", 2);
  memset(h + 148, ' ', 8);
  unsigned sum = 0;
  for (int i = 0; i < 512; i++)
    sum += (unsigned char)h[i];
  snprintf(h + 148, 8, "%06o", sum);
  return write_all(fd, h, 512);
}
static int tar_add(int tfd, const char *root, const char *rel, int depth) {
  char path[1100];
  snprintf(path, sizeof path, "%s/%s", root, rel);
  DIR *d = depth < 16 ? opendir(path) : NULL;
  if (!d)
    return -1;
  static char buf[1 << 16];
  struct dirent *e;
  int rc = 0;
  while (rc == 0 && (e = readdir(d))) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
      continue;
    char r2[1024], full[1100];
    snprintf(r2, sizeof r2, "%s/%s", rel, e->d_name);
    snprintf(full, sizeof full, "%s/%s", root, r2);
    struct stat st;
    if (lstat(full, &st) != 0)
      continue;
    if (S_ISDIR(st.st_mode)) {
      char dn[1030];
      snprintf(dn, sizeof dn, "%s/", r2);
      rc = tar_header(tfd, dn, 0, 1);
      if (rc == 0)
        rc = tar_add(tfd, root, r2, depth + 1);
      continue;
    }
    if (!S_ISREG(st.st_mode))
      continue;
    int fd = open(full, O_RDONLY);
    if (fd < 0 || tar_header(tfd, r2, st.st_size, 0) != 0) {
      if (fd >= 0)
        close(fd);
      dlog("FAIL tar %s", r2);
      rc = -1;
      break;
    }
    long long left = st.st_size;
    while (left > 0) {
      ssize_t r = read(fd, buf, left < (long long)sizeof buf ? (size_t)left : sizeof buf);
      if (r <= 0 || write_all(tfd, buf, (size_t)r) != 0) {
        rc = -1;
        break;
      }
      left -= r;
    }
    close(fd);
    if (rc == 0 && (st.st_size % 512)) {
      memset(buf, 0, 512);
      rc = write_all(tfd, buf, 512 - (size_t)(st.st_size % 512));
    }
  }
  closedir(d);
  return rc;
}
static int make_tar(const char *name) {
  char tp[300], part[310];
  snprintf(tp, sizeof tp, OUT_ROOT "/%s.tar", name);
  snprintf(part, sizeof part, "%s.part", tp);
  int fd = open(part, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fd < 0)
    return -1;
  char top[300];
  snprintf(top, sizeof top, "%s/", name);
  int rc = tar_header(fd, top, 0, 1);
  if (rc == 0)
    rc = tar_add(fd, OUT_ROOT, name, 0);
  static const char zero[1024];
  if (rc == 0)
    rc = write_all(fd, zero, sizeof zero);
  close(fd);
  if (rc == 0)
    rc = rename(part, tp);
  if (rc != 0)
    unlink(part);
  return rc;
}


static char *game_sfo(const char *tid, size_t *n) {
  static const char *const where[2] = {"/system_data/priv/appmeta", "/user/appmeta"};
  char *sfo = NULL;
  for (int i = 0; i < 2 && !sfo; i++) {
    char p[160];
    snprintf(p, sizeof p, "%s/%s/param.sfo", where[i], tid);
    sfo = read_small(p, SMALL_MAX, n);
  }
  return sfo;
}
static void fw_name(char *out, size_t cap) {
  unsigned fw = kernel_get_fw_version() >> 16;
  snprintf(out, cap, "%x.%02x", fw >> 8, fw & 0xff);
}

void dump_name(const char *tid, char *name, size_t cap) {
  char fw[16], appver[16] = "";
  fw_name(fw, sizeof fw);
  if (!tid || !tid[0]) {
    snprintf(name, cap, "fw%s", fw);
    return;
  }
  size_t n = 0;
  char *sfo = game_sfo(tid, &n);
  if (sfo)
    sfo_get(sfo, n, "APP_VER", appver, sizeof appver);
  free(sfo);
  snprintf(name, cap, "%.9s-%s-fw%s", tid, appver[0] ? appver : "unknown", fw);
}

int dump_save(const char *tid, struct dump_result *r) {
  char appver[16] = "", ver[16] = "", content[64] = "", title[128] = "", cat[8] = "";
  if (!tid)
    tid = "";
  memset(r, 0, sizeof *r);
  g_ok = g_fail = 0;
  fw_name(r->fw, sizeof r->fw);
  char *sfo = NULL;
  size_t sfo_n = 0;
  if (tid[0] && (sfo = game_sfo(tid, &sfo_n))) {
    sfo_get(sfo, sfo_n, "APP_VER", appver, sizeof appver);
    sfo_get(sfo, sfo_n, "VERSION", ver, sizeof ver);
    sfo_get(sfo, sfo_n, "CONTENT_ID", content, sizeof content);
    sfo_get(sfo, sfo_n, "TITLE", title, sizeof title);
    sfo_get(sfo, sfo_n, "CATEGORY", cat, sizeof cat);
  }
  free(sfo);
  dump_name(tid, r->name, sizeof r->name);

  char out[256], p[400];
  snprintf(out, sizeof out, OUT_ROOT "/%s", r->name);
  mkdirs(out);
  snprintf(p, sizeof p, "%s/dump.log", out);
  g_log = fopen(p, "w");
  dlog("doax3vr2 dump: firmware %s; game %s", r->fw, tid[0] ? tid : "none");

  /* 1. the firmware's libraries */
  static const char *const libs[] = {
      "/system/common/lib/libSceAgcDriver.sprx", "/system/common/lib/libSceAgc.sprx",
      "/system/common/lib/libSceHmd2.sprx",      "/system/common/lib/libSceVrTracker2.sprx",
      "/system/common/lib/libSceVideoOut.sprx",  "/system/common/lib/libSceVrHand.sprx",
      "/SceSysAvControl.elf",
  };
  r->libs = (unsigned)(sizeof libs / sizeof libs[0]);
  for (unsigned i = 0; i < r->libs; i++) {
    snprintf(p, sizeof p, "%s/firmware/%s", out, strrchr(libs[i], '/') + 1);
    r->libs_ok += dump_self(libs[i], p) == 0;
  }

  /* 2. the game */
  if (tid[0]) {
    static const char *const meta[2] = {"/system_data/priv/appmeta", "/user/appmeta"};
    for (int i = 0; i < 2; i++) {
      char src[160], dst[400];
      static const char *const files[] = {"param.sfo", "param.json"};
      for (int f = 0; f < 2; f++) {
        snprintf(src, sizeof src, "%s/%s/%s", meta[i], tid, files[f]);
        snprintf(dst, sizeof dst, "%s/game/appmeta/%s/%s", out, i ? "user" : "system_data", files[f]);
        struct stat st;
        if (stat(src, &st) == 0)
          copy_small(src, dst);
      }
    }
    static const char *const parts[2] = {"app0", "patch0"};
    for (int i = 0; i < 2; i++) {
      char root[256], o[400], lp[400];
      snprintf(root, sizeof root, "/mnt/sandbox/pfsmnt/%s-%s", tid, parts[i]);
      struct stat st;
      if (stat(root, &st) != 0) {
        dlog("%s: %s not mounted (%s)", parts[i], root, i ? "no update installed" : "the game not mounted");
        continue;
      }
      snprintf(o, sizeof o, "%s/game/%s", out, parts[i]);
      snprintf(lp, sizeof lp, "%s/game/files-%s.txt", out, parts[i]);
      mkdirs(o);
      FILE *list = fopen(lp, "w");
      if (!list) {
        dlog("FAIL create %s", lp), g_fail++;
        continue;
      }
      walk(root, "", list, o, 0);
      fclose(list);
    }
    char ac[160];
    snprintf(ac, sizeof ac, "/user/addcont/%s", tid);
    snprintf(p, sizeof p, "%s/game/addcont.txt", out);
    list_names(ac, p);
  }

  /* 3. info.txt */
  snprintf(p, sizeof p, "%s/info.txt", out);
  FILE *info = fopen(p, "w");
  if (info) {
    fprintf(info, "firmware %s\n", r->fw);
    if (tid[0])
      fprintf(info, "game %s, APP_VER %s, VERSION %s, CONTENT_ID %s, CATEGORY %s, TITLE %s\n", tid,
              appver[0] ? appver : "?", ver[0] ? ver : "?", content[0] ? content : "?", cat[0] ? cat : "?",
              title[0] ? title : "?");
    else
      fprintf(info, "no game: the firmware part only\n");
    fprintf(info, "firmware libraries saved: %u of %u\nfiles saved: %u, failed: %u\n", r->libs_ok, r->libs, g_ok,
            g_fail);
    fclose(info);
  }
  dlog("done: %u files saved, %u failed; making %s.tar", g_ok, g_fail, r->name);

  /* 4. one file to send */
  r->tar_rc = make_tar(r->name);
  dlog(r->tar_rc == 0 ? "ok   " OUT_ROOT "/%s.tar" : "FAIL the .tar of %s (the folder is complete)", r->name);
  if (g_log)
    fclose(g_log), g_log = NULL;
  r->ok = g_ok, r->fail = g_fail;
  return r->tar_rc;
}
