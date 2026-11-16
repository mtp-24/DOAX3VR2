/* DOAX3VR2 agent: freestanding code that runs inside the PS4 game process.
 * The payload (payload/main.c) maps it, applies its relative relocations,
 * resolves its imports by NID in the game's modules, points the game's GOT
 * slots for the hooked functions at it and saves the old values in the
 * hooks' "orig" pointers.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "nids_gen.h"
#include "../common/camera.h"

typedef int8_t s8;
typedef uint8_t u8;
typedef int16_t s16;
typedef uint16_t u16;
typedef int32_t s32;
typedef uint32_t u32;
typedef int64_t s64;
typedef uint64_t u64;

#define AGENT_MAGIC 0x32524f5254534100ull /* "\0ASTROR2": header has vr_state */
#define LOG_SIZE (256 * 1024)

struct agent_import {
  const char *module;
  const char *nid;
  void **slot;
};

struct agent_hook {
  const char *nid;
  void *hook;
  void **orig;
  u64 slot; /* game VA of the eboot GOT slot (filled by the injector, 0 = not imported) */
};

/* Shared with the injector, which polls it with kernel reads. */
struct agent_shared {
  volatile u64 log_wpos; /* bytes ever written; the ring holds the last LOG_SIZE */
  volatile u32 log_lock;
  volatile u32 boot_done;
  volatile u32 cmd;      /* written by the injector */
  volatile u32 cmd_seq;
  volatile u64 cmd_arg[4];
  volatile u32 cmd_ack;
  volatile u32 repatch;  /* set by the agent: GOT needs re-pointing (injector does it) */
  char log[LOG_SIZE];
  struct cam_state cam; /* written by the injector: the headset cameras as the kernel log tells
                         * it (common/camera.h), for the palm tracker's gate (hand.c) */
};

/* Hmd2's reprojection shader as GCN code: built on the console by the injector (from the
 * console's own libSceHmd2) and written here before the agent is mapped. */
#define GCN_MAX_DWORDS 0x1400
struct agent_gcn {
  u32 magic, dwords, rsrc1_vgprs, rsrc1_sgprs;
  u32 code[GCN_MAX_DWORDS];
};

struct agent_header {
  u64 magic;
  struct agent_import *imports;
  u32 nimports;
  u32 reserved0; /* (0; the fields' places are the injector's too: kept) */
  struct agent_hook *hooks;
  u32 nhooks;
  struct agent_shared *shared;
  u64 (*boot)(u64); /* called by the injector as it loads the agent (its argument 0; returns 0) */
  void *vr_state;     /* struct vr_state; its first vr_persist bytes survive a hot swap */
  u32 vr_persist;
  u32 reserved1; /* (0) */
  void *unbound_stub; /* what CMD_STUB points unbound imports at */
  struct agent_gcn *gcn;
  u32 ipd_um; /* written by the injector: the eye distance saved by CMD_SAVE_IPD, 0 = none */
  u32 firmware; /* injector-selected profile, formerly padding; header size is unchanged */
  u64 game_base;  /* written by the injector: where the game's eboot is */
  u32 frame_pace; /* written by the injector: refreshes a frame is shown for (2: the game's 60
                   * fps on the headset's 120 Hz) */
  u32 refresh_hz; /* the headset's refresh rate, 90 or 0 for 120 (the injector leaves 0) */
  u32 reserved2[8]; /* (0; the header's size is the injector's too: kept) */
};
extern struct agent_header astro_hdr;
struct size_align {
  u64 size, align;
};

#define CMD_QUIT 1 /* the agent's threads end; vr2.c turns the VR mode off and then writes
                    * STOP_DONE into cmd_arg[3] (the injector kills the game after that) */
#define STOP_DONE 0x51554954ull
#define CMD_RESOLVE 2 /* injector: resolve every import slot that is still zero */
#define CMD_STUB 5    /* injector: unbound imports of modules cmd_arg[0..3] -> unbound_stub */
#define CMD_OVERRIDE 6 /* injector: module cmd_arg[0]'s import of NID (char *)cmd_arg[1] -> cmd_arg[2] */
#define CMD_STAGE 7 /* injector: make PS5 lib cmd_arg[0] (0-4, 4 libSceVrHand) visible in the game's sandbox as
                     * /doax3vr2/<name>; cmd_arg[1] = 1: done with it */
#define CMD_SAVE_IPD 8 /* injector: keep cmd_arg[0] (micrometres) for the next start */
#define CMD_NOTIFY 9 /* injector: show (char *)cmd_arg[0] as a PS5 notification */
/* 10: retired (CMD_MODEL: libSceVrHand's models through the payload; the agent reads them itself) */
#define CMD_CVARS 12 /* injector: cvars.txt into (char *)cmd_arg[0] (cmd_arg[1] bytes, NUL-ended) */
#define CMD_MENU 14 /* injector: the settings menu's file, menu-<game>.txt (menu.c): cmd_arg[2] = 0 read into
                     * (char *)cmd_arg[0] (cmd_arg[1] bytes, NUL-ended), 1 written from it (cmd_arg[1] bytes;
                     * 0: the file removed) */
#define CMD_SHOT 15 /* injector: cmd_arg[1] bytes at cmd_arg[0] (the agent's copy of the game's flat display
                     * buffer, as the GPU laid it out) to OUT_DIR/shot.raw; cmd_arg[2] its number (a test tool) */
#define CMD_CAMERA 11 /* injector: cmd_arg[0] = 1: let the game open the headset cameras (raise its
                       * credentials), 0: done, its own back; cmd_arg[0] = 0 or -1 failed */
/* a request to the injector, from any thread (core.c) */
int agent_call(u32 cmd, u64 a0, u64 a1, u64 a2, u64 a3, u32 wait_ms, u32 k, u64 *out);

/* Imports: one function pointer per entry of imports.def. */
#define IMP(ret, name, args) extern ret(*name) args;
#include "imports.inc"
#undef IMP

void agent_log(const char *fmt, ...);
int agent_vsnprintf(char *out, size_t n, const char *fmt, __builtin_va_list ap);
int agent_snprintf(char *out, size_t n, const char *fmt, ...); /* returns what it wrote */
void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);

extern struct agent_shared g_shared;
extern char g_cvars_text[4096]; /* cvars.txt, fetched from the injector (CMD_CVARS); its agent.* lines read by hooks.c */
extern u32 g_cvars_len;
extern volatile u32 g_refresh_hz; /* the headset's refresh as Hmd2 runs it (vr2.c): 90 or 120; 0 before */
/* the agent's own options, "agent.<name>=<value>" lines of cvars.txt, read before PS VR2 starts */
extern u32 g_opt_mirrorboth, g_opt_tvhmd; /* agent.mirrorboth=1: the TV shows both eyes as drawn */
#ifndef RELEASE
extern volatile u32 g_opt_capture; /* agent.capture=N: a TV capture into the log each new N (mirror.c), live */
#endif
u64 hooks_frame_count(void);   /* the game's count for the frame being handed over (hooks.c; 0: this game has none) */
int hooks_frame_drawn(u64 count); /* the game's GPU has finished the frame of this count (always, here) */
int hooks_flip_state(s32 vo, s32 *pending, s32 *shown); /* a port's real flips pending, buffer shown */
void hooks_tv_rate_sync(void); /* the main port's flip rate: 0 in VR mode, the game's out of it */
extern u32 g_opt_tvblank;    /* agent.tvblank=1: the TV shows black; the mirror still flips its (black)
                              * buffers at the same pace, which the display path needs (off: a hang) */
extern u32 g_opt_rtime; /* agent.rtime=1: the game's threads' CPU time (rtime.c), a debug tool */
extern u32 g_opt_recentre;   /* agent.recentre=1: the first press of circle recentres (off by default) */
extern u32 g_opt_handload; /* agent.handload=1 (debug, an isolation test): libSceVrHand loaded at PS VR2's
                            * start as for agent.hands, never started (no cameras, no models) */
extern u32 g_opt_handstep; /* agent.handstep=1 (debug, isolation test B): at the clear moment only the
                            * cameras' credentials raised for 5 s and given back; VrHand never called */
extern u32 g_opt_hands; /* the palm tracker: 60, 30, or 0 (the controller stays where the game put
                         * it). (A "mirror off" option brought the console down on 2026-11-01 00:42,
                         * measurement 30: the mirror also keeps the game's main port flipping) */
void cvars_fetch(void);   /* the text from the injector (CMD_CVARS), once */
extern struct agent_gcn g_gcn;

#define LOG(...) agent_log(__VA_ARGS__)
/* a step that might take the console down (hand tracking's, PS VR2's start), logged before it is
 * taken; with agent.hands on the payload syncs each write to the storage, and the 100 ms here let
 * the line get there (its 20 ms poll) before the step runs */
#define LOG_STEP(...)                                                                            \
  do {                                                                                           \
    agent_log(__VA_ARGS__);                                                                      \
    if (g_opt_hands || g_opt_handload || g_opt_handstep)                                                          \
      sceKernelUsleep(100000);                                                                   \
  } while (0)
/* the periodic detail (timings, pose figures, censuses, per-10 s counts beyond the frames' line):
 * left out of a release build (make RELEASE=1), which logs start-up, errors and one frames line
 * every 10 s */
#ifdef RELEASE
#define LOG_DETAIL(...) do { if (0) agent_log(__VA_ARGS__); } while (0) /* compiled away */
#else
#define LOG_DETAIL(...) agent_log(__VA_ARGS__)
#endif
