/* The files a new game version or firmware is supported from, saved and packed into one .tar
 * (common/dump.c) by the payload itself when it meets a Dead or Alive Xtreme 3 it doesn't know or
 * a firmware it has no profile for.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOAX3VR2_DUMP_H
#define DOAX3VR2_DUMP_H
#include <stddef.h>

#ifndef DUMP_ROOT /* (a host test sets its own) */
#define DUMP_ROOT "/data/doax3vr2-dump"
#endif

struct dump_result {
  char name[96];   /* <TITLE ID>-<APP_VER>-fw<x.xx>, or fw<x.xx> for the firmware only */
  char fw[16];     /* x.xx */
  unsigned ok, fail; /* files saved, failed */
  unsigned libs_ok, libs; /* of them, the firmware's libraries */
  int tar_rc;      /* 0: DUMP_ROOT/<name>.tar made */
};

/* The name a dump of the game tid ("" or NULL: no game, the firmware only) is saved under. */
void dump_name(const char *tid, char *name, size_t cap);
/* Saves the firmware's PS VR2 libraries and, with a game (tid), its decrypted executables of the
 * base and the update, its param.sfo/json, its file lists and add-on folder names, with info.txt
 * and dump.log, into DUMP_ROOT/<name>/, then packs it as DUMP_ROOT/<name>.tar. Read only outside
 * DUMP_ROOT; never save data, keys or identifiers. Not re-entrant (one dump at a time). Takes
 * about a minute. Returns r->tar_rc. */
int dump_save(const char *tid, struct dump_result *r);
#endif
