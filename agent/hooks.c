/* PS VR (PS4) emulation: the game's libSceHmd / libSceVrTracker / libSceCamera /
 * libSceHmdSetupDialog imports are pointed here. The headset, camera and tracker
 * report "connected"; frames the game hands to sceHmdReprojectionStart are kept
 * in g_vr for the PS VR2 output (vr2.c).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "agent.h"
#include "psvr1.h"
#include "vr2.h"

struct vr_state g_vr = {
    .user_id = -1,
    .ipd = 0.063f,
    /* PS VR2's own field of view (Hmd2 FOV of this headset; out, in, top, bottom), so the game
     * draws what PS VR2 shows: it asks once, at sceHmdOpen, before Hmd2 is up in the process.
     * A real PS VR reports 1.20743, 1.181346, 1.262872, 1.262872. */
    .title_fov = {1.8418f, 0.9472f, 1.3290f, 1.3290f},
    .force_modes = 1,
};

/* Original GOT values (the real library functions). */
#define H(name) static void *orig_##name;
#include "hooks.inc"
#undef H

/* ---------------------------------------------------------------- helpers */
static u64 now_us(void) { return sceKernelGetProcessTime(); }

static int once(volatile u32 *flag) {
  return __atomic_exchange_n(flag, 1, __ATOMIC_ACQ_REL) == 0;
}

#define CALL_ORIG(type, name, ...) (((type)orig_##name)(__VA_ARGS__))
#define LOG_FIRST(counter, n, ...)                                                               \
  do {                                                                                           \
    if (__atomic_fetch_add(&(counter), 1, __ATOMIC_RELAXED) < (n))                               \
      LOG(__VA_ARGS__);                                                                          \
  } while (0)

#define HMD_HANDLE 0x0f000000
static void labels_reset(void);
static void malfunction_keep(void);
static void inv_dump(void);
static void inv_give(void);
static void fan_watch(void);
u32 g_opt_fanswings, g_opt_chars, g_opt_padtest, g_opt_handload, g_opt_handstep, g_opt_nopassport; /* (padtest: vr.c) */ /* (the menu's too; fanswings 0: the game's own, the default since 2026-11-11) */
__attribute__((unused)) static u32 g_opt_shotslice; /* agent.shotslice=N (1-4): drive.py shot copies only slice N-1 of the game's eye array (debug) */
__attribute__((unused)) static u32 g_opt_shoteye; /* agent.shoteye=1: drive.py shot takes the right eye (debug builds) */
static u32 g_opt_inv, g_opt_give;
static u32 g_opt_closet; /* agent.closet=ID: closet_log (debug) */
static u32 g_opt_clockhours; /* agent.clockhours=N: the offline network clock and the sale list N hours ahead (debug, live) */
static void owner_keep(void);
static void port_watch(void);
static void special_sale(void);
static void set_unpack(void);
u32 g_opt_portshop; /* (the debug menu reads and sets it, menu.c) */
static u32 g_opt_portgive, g_opt_giveport, g_opt_wfix;
u32 g_opt_special = 2; /* agent.special=0: the Owner Shop's Special tab left to the online sale data */
u32 g_opt_starfans; /* agent.starfans=1: the three Fan☆ given the Owner Shop flag (their rows have 0: off by
                            * default, the game's own data, as the user asked 2026-11-12) */
u32 g_opt_sensors; /* agent.sensors=1: the Softness Sensors stocked (default: kept out of the shops) */
static void port_shop(u8 *base);
u32 g_opt_ownerlv, g_opt_money;
#ifndef RELEASE
/* Driving the game's flat screens without a player (tools/drive.py; cvars.txt, read live):
 * agent.shot=N, each new N: the flat display buffer the game flipped last, copied and handed to
 * the payload as it is (shot.raw: the GPU's tiling, undone on the PC); agent.press=N:HEX:MS, each
 * new N: the buttons HEX held for MS ms in what the game reads from its controller. */
static volatile u32 g_shot_req, g_press_seq, g_press_buttons;
static volatile u64 g_press_until;
static volatile s32 g_game_flip_index = -1; /* the game's last flip of its main port (a buffer of its own) */
#endif
static u32 g_game_flip_w, g_game_flip_h; /* the main port's buffers' size (the shots; the menu out of VR) */
u32 g_opt_compose = 1;           /* agent.compose=0: the wide views alone (compose.c) */
static u32 g_opt_vrunlock = 0;   /* agent.vrunlock: 1 the five CATEGORY names reported installed (probably no
                                  * effect); 0 off, the default; 2 log the queries only */
static const char *k_all_labels[] = {
  "CATEGORY00000000", "CATEGORY00000001", "CATEGORY00010000",
  "CATEGORY00020000", "CATEGORY00050000",
};
#define N_INJECT_LABELS (sizeof k_all_labels / sizeof k_all_labels[0])
u32 g_opt_composemax = 4096;     /* agent.composemax=N: the composed eye's width at the most (4096: the
                                    * payload's default eye width 2048 composes 3848x4096: 4096 rows the most, compose.c) */
u32 g_opt_malfunction = 1; /* agent.malfunction (default 1, as Fortune has them, asked 2026-11-12; 0 the game's own 0): Scarlet's 15 costumes with the malfunction
                                    * flag off get it on (malfunction_keep) */
static u32 g_opt_rowscan;        /* agent.rowscan=1: rows_scan once (a debug search; it hung) */
static u32 g_opt_port = 1;       /* agent.port: 1 (default) the game's /app0/NN/package_data.bin served
                                    * from /data/doax3vr2/port/NN/ when there is one (rows Scarlet
                                    * lacks); the default matters: the game reads its packages
                                    * before the agent's first cvars.txt. 0: never */
static u32 g_opt_novr2;          /* agent.novr2=1: an observing run, PS VR2 never started */
static u32 g_opt_errdialog;      /* agent.errdialog=1: the system's error dialog shown as the game asks (default: not shown) */
static volatile u32 g_main_closed; /* the game closed its main port: kept open (h_sceVideoOutClose) */
static s32 g_main_port = -1; /* the game's main VideoOut port (bus 0), -1 until opened */
#define VBL_USER_ID 0x7a5b /* see "main port at 120 Hz" */

/* ---------------------------------------------------------------- libSceHmd */
static s32 h_sceHmdInitialize315(const void *param) {
  LOG("hmd: Initialize315(%p)", param);
  return 0;
}

static s32 h_sceHmdOpen(s32 user, s32 type, s32 index, void *param) {
  LOG("hmd: Open(user %d, type %d, index %d, %p) -> 0x%x", user, type, index, param, HMD_HANDLE);
  g_vr.user_id = user;
  return HMD_HANDLE;
}

static s32 h_sceHmdClose(s32 handle) {
  LOG("hmd: Close(0x%x)", handle);
  return 0;
}

static s32 h_sceHmdTerminate(void) {
  LOG("hmd: Terminate");
  return 0;
}

static s32 h_sceHmdGetDeviceInformation(struct hmd_device_information *info) {
  static u32 n;
  if (!info)
    return HMD_ERR_PARAMETER_NULL;
  agent_tick();
  s32 rc = orig_sceHmdGetDeviceInformation
               ? CALL_ORIG(s32(*)(void *), sceHmdGetDeviceInformation, info)
               : -1;
  s32 real_status = rc == 0 ? (s32)info->status : -1;
  s32 user = rc == 0 ? info->user_id : -1;
  if (g_vr.user_id < 0) {
    s32 u = -1;
    if (sceUserServiceGetInitialUser(&u) == 0 && u >= 0)
      g_vr.user_id = u;
  }
  if (user < 0)
    user = g_vr.user_id;
  memset(info, 0, sizeof *info);
  info->status = HMD_STATUS_READY;
  info->user_id = user >= 0 ? user : g_vr.user_id;
  info->panel_width = 1920; /* PS VR's panel (the game's eye size is its own: the payload's
                             * patch_eye_size) */
  info->panel_height = 1080;
  info->latency_90hz = 13000;
  info->latency_120hz = 10000;
  info->hmu_mount = !g_vr.hmd_off;
  u32 k = __atomic_fetch_add(&n, 1, __ATOMIC_RELAXED);
  if (k < 4 || k % 600 == 0)
    LOG_DETAIL("hmd: GetDeviceInformation #%u (real rc 0x%x status %d) -> ready, user %d", k, rc,
        real_status, info->user_id);
  return 0;
}

/* Asked by handle once the headset is open (a game's periodic "still connected" check: the same
 * answer). */
/* 120 fps groundwork (debug options, logs/research/fps/REPORT-2.md). The game's time is an integer
 * tick of one 60 Hz frame: each loop of its main thread runs n = table[mode].ticks + ring.extra
 * ticks, the ring entry (cur + 2) & 3, its extras zeroed (or set by the game's load controller)
 * earlier in the same loop, and n <= 0 is a path the game has. The main loop calls
 * sceHmdGetDeviceInformationByHandle once between that and its update, so this hook is the place
 * to look and to write, known by its return address (no game code patched).
 *   agent.ticklog=1: every 10 s the loops, the ticks they ran, the modes, the game's own extras,
 *     the game speed and the pause byte seen.
 *   agent.halftick=1: in VR every other loop gets extra = -1 (0 ticks): the game at half speed
 *     with a picture drawn for every frame still (the proof of the 0-tick path at 60 fps). */
static u32 g_opt_ticklog, g_opt_halftick;
/* agent.tickhold (on by default since 2026-11-14, a release build too; agent.tickhold=0 turns it
 * off; it acts only at 120 fps, on loops with no tick; the user approved its slowing of the VR seat
 * follow back to its 60 fps rate): at a picture every refresh (frame_pace 1), a loop the scheduler
 * gives no tick runs its update with the game speed float at 0, put back after the update, so the
 * update's per-frame layer sees "no time went by" as it does on a 60 fps loop's in-between refresh
 * that does not exist. Without it that layer runs once a loop, twice as fast at 120 (read from
 * the eboots, Scarlet first, Fortune in brackets; the update is 0x4ef630 [0x4edc80], its ticks
 * at 0x4f07e6-0x4f2bf6, everything after once a loop whatever the tick count):
 *   - the shader time 0xac2190 [0xaba0b0] += speed (0 while the pause byte is set) at 0x4f2c1c
 *     [0x4f126c]: read by the materials' constant setup in the render thread's draw (0x5153c2..
 *     0x51971c, from 0x4c87c0 at 0x4fcdec): water and the other animated materials;
 *   - the timed-callback list *0xbefe38 (each entry's time -= that speed), the manager
 *     (*0xbefe30)->+0x18 vtable[0x38](speed) and 0x168e20's countdowns (+0x230, pause-gated);
 *   - with the raw speed (not stopped by the pause byte): six camera-effect countdowns at 0xbec8b0
 *     (0x4f39f0), timed event queues (+0x58 += speed, 0x4f4d9c), (*0x1c2daf8)->vtable[0x20](speed)
 *     at 0x4f53ec, and the VR seat follow 0xbec7b0 (1% of the way to its target a loop times speed,
 *     0x4f3561/0x4f368c: the game's own camera base, not the pose);
 *   - the frame-count timer list (0x4f80d9 [0x4f6729]: the speed accumulated in 0x958f7c, its whole
 *     frames taken from each node of *0x958eb8; nothing taken with the speed 0).
 * The game itself treats speed <= 1.19e-7 as stopped at each of those (0x4f2c50, 0x4f2e0a,
 * 0x4f2a98) and has a speed 0 of its own (0x5597de compares it with 0.0). Rejected: the pause byte
 * 0xbea2ac [0xbe21cc] is the pause menu's state (set by 0x42a140, cleared by the update itself at
 * 0x4f82cd/0x4f82db), stops only half the users and is read in ~90 places; patching the timer add
 * would hold the water alone. Not held by this (the seconds table 0x80f1cc is in the read-only
 * code segment): the per-frame users of the mode's seconds (0x4f8665 the object list *0x94bde0
 * vtable[0x30](dt): the menus' and message windows' managers, 0x4f87c7 0x5da7f0(dt), 0x4f87ee the
 * boot logo's fade *0xbebad8): agent.uihold (vrreact.c, debug) skips that span on a loop with no
 * tick; without it the menus' fades and scrolls run twice as fast at 120.
 * The speed is put back at the main loop's next sceKernelWaitEventFlag (its loop top, ret 0x54984b
 * [0x548c3b], the first hooked call after the update: none is made inside it), or, when the game
 * skips that wait (0xbd6830 [0xbce740] clear), at the next loop's tick_frame; if the game wrote the
 * speed meanwhile (0x5597cd [0x5596dd], with its applied copy 0x8d4a38 [0x8cca38]) its value stays.
 * Every site is compared with the eboots' bytes first (tick_hold_check); any difference: off.
 * Concurrent readers on other threads see 0 during the hold: the render thread's 0x4fa4ae multiplies
 * it by the same ring entry's tick count, already 0 on such a loop. */
static u32 g_opt_tickhold = 1;
static struct {
  u32 on;            /* the speed is 0 now (the main thread's: tick_frame and the loop-top wait) */
  int g;             /* the game it was done in */
  float saved;       /* the speed before */
  float applied;     /* the game's applied copy then */
  u32 n, wait, late, game, skipped; /* held loops; put back at the wait / at the next loop; the game's own write kept; not held (speed not > 0) */
} g_hold;
static u32 g_opt_touchprobe; /* agent.touchprobe=1: the VR touch test watched (touch_probe), read only */
static u32 g_late_d; /* this hand-over's refreshes after the last one (frame_timing), for gpu_done */
static volatile u32 g_tick_none; /* this loop of the main thread runs no tick (a picture every refresh): the pad read holds */
static u64 g_tick_scene_us; /* the last wide + near hand-over: a VR scene is being drawn (not a flat page) */
u64 rtime_thread_ns(void);
/* a thread's CPU time between its calls (the main thread's a loop, the render thread's a frame:
 * each must fit a refresh, 8.3 ms, for 120 pictures a second), us: the sum, the count, the most */
struct tick_cpu {
  u64 last, sum, max, max_at, last_us;
  u32 n;
};
static u64 g_tick_wall_max, g_tick_wall_max_at, g_tick_wall_last; /* the main loop's wall time a loop */
static struct {
  volatile u32 due;
  u32 frames, zero, halved, modes, catchup, own_extra, paused, in_vr, main_n, render_n;
  u32 held, held_wait, held_late, held_game, held_skipped, hold_state;
  s32 ticks;
  float smin, smax;
  u64 main_avg, main_max, main_max_at, wall_max, wall_max_at, render_avg, render_max;
} g_tick_rep;
/* the tick log's 10 s lines, on the agent's cvars thread (health_report_due) */
static void tick_report(void) {
  if (!__atomic_load_n(&g_tick_rep.due, __ATOMIC_ACQUIRE))
    return;
  LOG("tick: %u loops in the last 10 s ran %d ticks (%u with none, %u given none by the agent); modes 0x%x, "
      "catch-up allowed in %u, the game's own extras in %u, paused in %u, speed %f to %f%s",
      g_tick_rep.frames, g_tick_rep.ticks, g_tick_rep.zero, g_tick_rep.halved, g_tick_rep.modes, g_tick_rep.catchup,
      g_tick_rep.own_extra, g_tick_rep.paused, g_tick_rep.smin, g_tick_rep.smax, g_tick_rep.in_vr ? ", in VR" : "");
  LOG("tick: CPU a loop: the main thread %lu us on average, %lu at the most (%u loops, the most at %lu ms); its "
      "loops' wall time %lu us at the most (at %lu ms); the render thread %lu us a frame, %lu at the most (%u frames)",
      g_tick_rep.main_avg, g_tick_rep.main_max, g_tick_rep.main_n, g_tick_rep.main_max_at / 1000, g_tick_rep.wall_max,
      g_tick_rep.wall_max_at / 1000, g_tick_rep.render_avg, g_tick_rep.render_max, g_tick_rep.render_n);
  if (g_opt_tickhold || g_tick_rep.held)
    LOG_DETAIL("tick: hold (agent.tickhold): %u of the %u loops ran their update with the game speed at 0 (put back "
        "at the loop-top wait in %u, at the next loop in %u, the game's own speed write kept in %u; %u with no "
        "tick not held, the speed not above 0)%s",
        g_tick_rep.held, g_tick_rep.frames, g_tick_rep.held_wait, g_tick_rep.held_late, g_tick_rep.held_game,
        g_tick_rep.held_skipped,
        g_tick_rep.hold_state == 1 ? "" : g_tick_rep.hold_state == 0 ? "; not checked yet" : "; OFF: the code differs");
  __atomic_store_n(&g_tick_rep.due, 0, __ATOMIC_RELEASE);
}
static struct tick_cpu g_tick_main, g_tick_render;
static void tick_cpu_note(struct tick_cpu *c) {
  u64 ns = rtime_thread_ns();
  if (ns && c->last) {
    u64 us = (ns - c->last) / 1000;
    c->sum += us, c->n++, c->last_us = us;
    if (us > c->max)
      c->max = us, c->max_at = now_us();
  }
  c->last = ns;
}
/* the render thread, at each frame's hand-over */
static void tick_render(void) {
  if (g_opt_ticklog)
    tick_cpu_note(&g_tick_render);
}
/* agent.touchprobe=1 (debug, at the start): the VR scene's controller touch test watched, nothing
 * changed (logs/research/touch/REPORT.md, read only). The game places two 7 cm spheres on the
 * tracked controller and tests them against each girl's body spheres every frame, but the list of
 * those controller spheres is never filled, so "touching" never comes. Here, on the game's main
 * thread at the start of its loop (the spheres, S's matrices and the game's flags are all the last
 * update's, one consistent set), the game's own test 0x46cb20 (Fortune 0x46ad70) is called exactly
 * as its call sites call it (Scarlet 0x4711a3/0x4711d7, 0x4722fe/0x472351/0x4723d2):
 *   u8 test(v4f pos, float r, int mode): over the 4 characters (0xaa3fe0[i], found in the model
 *   list 0x94a980 by the value, +0x18 next), the node keyed by the value's low 16 bits in the map
 *   *0xab7da8 (std::map, root at head+8, nil flag +0x19, key u16 at +0x20, its spheres a vector
 *   at +0x38/+0x40 of 32 bytes: centre xyzw, radius +0x10, flag +0x18): a hit when
 *   |centre - pos| (x, y, z only; w ignored) <= r + radius; mode 1 skips spheres whose +0x18 is set.
 *   - head (S+0x120/121/122): pos = S+0x110 as it is (NOT times the world scale), w 0;
 *     0.09 mode 1 -> +0x121; 0.09 mode 0 -> +0x120 (+0x120 = either); 0.11 mode 0 -> +0x122 (only
 *     while +0x120 is off and the fade at *0xab7dd8 is done, so it lags).
 *   - controller: pos = M * (offset * scale, 1) with M the matrix at S+0x40 (rows x, y, z,
 *     translation): the offset is scaled, the translation is not; r = the table's radius; mode 1
 *     -> +0x81, mode 0 -> +0x8d. The game only tests while S+0x82 (tracked, latched).
 * The world scale 0x8d4f80 (Fortune 0x8ccf84) is 1.2 in parts of VR (written at 0x30c426, 0x30d79f,
 * 0x31c61f, 0x552c3b; 1.0 at 0x30d5d2, 0x30e680, 0x310727, 0x31f369, 0x32a9f5, 0x556049): the body
 * spheres are bone * scale, S+0x110 and S+0x70 are already in that space.
 * Logged: each start and end of a would-be touch, the probe's head result against the game's own
 * flags (they must agree: same data), and every 0.5 s the positions and the distance from each to
 * the nearest body sphere's surface, computed from the same structures the test reads (how many
 * characters and spheres it found), with a 10 s summary. */
typedef float v4f __attribute__((vector_size(16)));
static int readable(const void *p, u64 n);
struct touch_k {
  u64 s_ptr, hit, scale, table, map, models, chars;
};
/* the nearest body sphere to p, as the game's test walks them (read only): the distance from p to
 * its surface (the test hits when r >= this), -1e9 when none; *nc characters with spheres, *ns
 * spheres seen, *at its centre */
static float touch_nearest(u8 *base, const struct touch_k *k, v4f p, int mode, u32 *nc, u32 *ns, v4f *at) {
  float best = -1e9f;
  int have = 0;
  *nc = *ns = 0;
  u8 *map = *(u8 **)(base + k->map);
  if (!map)
    return best;
  u8 *head = *(u8 **)(map + 8);
  if (!head)
    return best;
  for (int i = 0; i < 4; i++) {
    u64 id = *(u64 *)(base + k->chars + i * 8);
    u8 *n = *(u8 **)(base + k->models);
    for (int steps = 0; n && *(u64 *)n != id && steps < 256; steps++)
      n = *(u8 **)(n + 0x18);
    if (!n || *(u64 *)n != id)
      continue;
    u16 key = (u16)id;
    u8 *x = *(u8 **)(head + 8), *res = head;
    for (int steps = 0; x && !x[0x19] && steps < 64; steps++) {
      if (*(u16 *)(x + 0x20) < key)
        x = *(u8 **)(x + 0x10);
      else
        res = x, x = *(u8 **)x;
    }
    if (res == head || *(u16 *)(res + 0x20) != key)
      continue;
    const u8 *s = *(u8 **)(res + 0x38), *e = *(u8 **)(res + 0x40);
    if (!s || e <= s || (u64)(e - s) > 0x20 * 4096)
      continue;
    (*nc)++;
    for (; s < e; s += 0x20) {
      if (mode && s[0x18])
        continue;
      const float *c = (const float *)s;
      float dx = c[0] - p[0], dy = c[1] - p[1], dz = c[2] - p[2];
      float g = fsqrt(dx * dx + dy * dy + dz * dz) - c[4];
      (*ns)++;
      if (!have || g < best) {
        best = g, have = 1;
        *at = (v4f){c[0], c[1], c[2], c[4]};
      }
    }
  }
  return best;
}
static void touch_probe(u8 *base, int g) {
  static const struct touch_k k[] = {
      {0xab7e10, 0x46cb20, 0x8d4f80, 0x7d15e0, 0xab7da8, 0x94a980, 0xaa3fe0}, /* Scarlet 1.08 */
      {0xaafd30, 0x46ad70, 0x8ccf84, 0x7c9d90, 0xaafcc8, 0x942900, 0xa9bf80}, /* Fortune 1.19 */
      {0xab7dd0, 0x46b060, 0x8d4f88, 0x7cfc30, 0xab7d68, 0x94a980, 0xaa3fa0}, /* Scarlet 1.00 */
  };
  u8 *S = *(u8 **)(base + k[g].s_ptr);
  static u32 frames, live, tracked, hits, head, game_touch, list_max, was_hit, was_head, agree, was_mine;
  static u64 next;
  u64 now = now_us();
  if (!next)
    next = now + 10000000;
  if (S && readable(S, 0x170)) {
    u32 list = (u32)((*(u64 *)(S + 0xc8) - *(u64 *)(S + 0xc0)) / 16);
    if (list > list_max)
      list_max = list;
    frames++;
    /* the game runs its scene frame (spheres, matrices, flags) only while S+0x154 == 1 and S+0x160 */
    int on = *(u32 *)(S + 0x154) == 1 && S[0x160];
    live += on;
    tracked += S[0x83] != 0;
    game_touch += S[0x81] != 0;
    int hd = S[0x120] || S[0x121] || S[0x122];
    head += hd;
    if (hd != (int)was_head) {
      LOG("touch: the game's head near a girl %s (flags %u %u %u)", hd ? "on" : "off", S[0x120], S[0x121],
          S[0x122]);
      was_head = (u32)hd;
    }
    u8 (*test)(v4f, float, int) = (u8 (*)(v4f, float, int))(base + k[g].hit);
    float sc = *(const float *)(base + k[g].scale);
    const float *h = (const float *)(S + 0x110);
    v4f hp = {h[0], h[1], h[2], 0.0f}; /* as 0x4722b8: S+0x110 as it is, w 0 (0x7ba1e8) */
    int h1 = test(hp, 0.09f, 1) != 0, h0 = h1 || test(hp, 0.09f, 0) != 0, h2 = test(hp, 0.11f, 0) != 0;
    /* +0x121 is written every scene frame; +0x120 = h0 */
    if (on)
      agree += h1 == (S[0x121] != 0) && h0 == (S[0x120] != 0);
    u32 mine = (u32)(h1 | h0 << 1 | h2 << 2);
    if (mine != was_mine) {
      LOG("touch: the probe's head test %u %u %u (0.09 mode 1, 0.09, 0.11), the game's flags %u %u %u at "
          "%f %f %f", h0, h1, h2, S[0x120], S[0x121], S[0x122], h[0], h[1], h[2]);
      was_mine = mine;
    }
    int tr = S[0x82] != 0, hit = 0; /* the game's gate (0x47111b): tracked, latched */
    v4f pos[2];
    if (tr) {
      const float *m = (const float *)(S + 0x40); /* rows: x, y, z, translation */
      const float *tab = (const float *)(base + k[g].table);
      for (int i = 0; i < 2; i++) {
        const float *o = tab + i * 4; /* radius, x, y, z */
        float ox = o[1] * sc, oy = o[2] * sc, oz = o[3] * sc;
        v4f p;
        for (int c = 0; c < 3; c++)
          p[c] = ox * m[c] + oy * m[4 + c] + oz * m[8 + c] + m[12 + c];
        p[3] = 0.0f;
        pos[i] = p;
        if (!hit)
          hit = test(p, o[0], 1) != 0 || test(p, o[0], 0) != 0;
      }
      hits += hit;
      if (hit != (int)was_hit) {
        LOG("touch: the controller %s a girl (sphere at %f %f %f, scale %f; the game's touching flag %u)",
            hit ? "WOULD TOUCH" : "no longer touches", pos[0][0], pos[0][1], pos[0][2], sc, S[0x81]);
        was_hit = (u32)hit;
      }
    } else if (was_hit) {
      LOG("touch: the controller not tracked");
      was_hit = 0;
    }
    /* every 0.5 s: where the controller and the head are (the game's world, the space of the body
     * spheres) and how far each is from the nearest body sphere's surface (m; the test hits when its
     * radius is at least that) */
    static u64 next_pos;
    if (now >= next_pos) {
      next_pos = now + 500000;
      u32 nc, ns, cc = 0, cs = 0;
      v4f at = {0}, cat = {0};
      float gh = touch_nearest(base, &k[g], hp, 0, &nc, &ns, &at), gc = -1e9f;
      if (tr)
        gc = touch_nearest(base, &k[g], pos[0], 0, &cc, &cs, &cat);
      LOG("touch: scale %f, scene frame %s; head %f %f %f: nearest body sphere %f m (centre %f %f %f, r %f; "
          "%u characters, %u spheres)", sc, on ? "on" : "OFF", h[0], h[1], h[2], gh, at[0], at[1], at[2], at[3],
          nc, ns);
      if (tr)
        LOG("touch: controller sphere %f %f %f: nearest body sphere %f m (centre %f %f %f, r %f)", pos[0][0],
            pos[0][1], pos[0][2], gc, cat[0], cat[1], cat[2], cat[3]);
    }
  }
  if (now >= next) {
    if (frames)
      LOG("touch: in the last 10 s, %u loops (%u with the scene frame on): the controller tracked in %u, a "
          "would-be touch in %u, the game's touching flag in %u, the game's head near a girl in %u, the probe's "
          "head test agreeing with the game's flags in %u; the controller sphere list %u long at the most",
          frames, live, tracked, hits, game_touch, head, agree, list_max);
    frames = live = tracked = hits = head = game_touch = list_max = agree = 0;
    next = now + 10000000;
  }
}

/* agent.tickhold: the sites it relies on, as in the three eboots (dumps/CUSA14173, dumps/CUSA04555
 * patch0 eboot.bin, CUSA14831 app0 eboot.bin), and where their RIP-relative operands lead (checked
 * against k_hold below) */
static int writable(const void *p, u64 n);
static const struct {
  u64 speed, applied, wait_ret;
} k_hold[N_GAMES] = {
    {0x8c5070, 0x8d4a38, 0x54984b}, /* Scarlet 1.08 */
    {0x8bd070, 0x8cca38, 0x548c3b}, /* Fortune 1.19 */
    {0x8c5070, 0x8d4a38, 0x547d8b}, /* Scarlet 1.00 */
};
enum { HL_SPEED, HL_APPLIED, HL_FN };
static const struct hold_site {
  const char *what;
  u32 va[N_GAMES];
  u8 len, code[N_GAMES][8];
  u8 disp, end, field; /* the operand's offset, the instruction's end, what it must lead to */
  u32 fn[N_GAMES];     /* HL_FN: the call's target */
} k_hold_sites[] = {
    {"the update's per-frame part: the speed read", {0x4f2bf6, 0x4f1246, 0x4f1136}, 8,
     {{0xc5, 0xfa, 0x10, 0x0d, 0x72, 0x24, 0x3d, 0x00}, {0xc5, 0xfa, 0x10, 0x0d, 0x22, 0xbe, 0x3c, 0x00},
      {0xc5, 0xfa, 0x10, 0x0d, 0x32, 0x3f, 0x3d, 0x00}}, 4, 8, HL_SPEED},
    {"the shader time += speed", {0x4f2c1c, 0x4f126c, 0x4f115c}, 8,
     {{0xc5, 0xe2, 0x58, 0x0d, 0x6c, 0xf5, 0x5c, 0x00}, {0xc5, 0xe2, 0x58, 0x0d, 0x3c, 0x8e, 0x5c, 0x00},
      {0xc5, 0xe2, 0x58, 0x0d, 0xec, 0x0f, 0x5d, 0x00}}, 4, 8, HL_FN,
     {0xac2190, 0xaba0b0, 0xac2150}},
    {"the main loop's call to the update", {0x54b292, 0x54a682, 0x5497d2}, 5,
     {{0xe8, 0x99, 0x43, 0xfa, 0xff}, {0xe8, 0xf9, 0x35, 0xfa, 0xff},
      {0xe8, 0x99, 0x43, 0xfa, 0xff}}, 1, 5, HL_FN, {0x4ef630, 0x4edc80, 0x4edb70}},
    {"its loop-top sceKernelWaitEventFlag", {0x549846, 0x548c36, 0x547d86}, 5,
     {{0xe8, 0x95, 0x9d, 0x23, 0x00}, {0xe8, 0xe5, 0x31, 0x23, 0x00},
      {0xe8, 0xb5, 0x9e, 0x23, 0x00}}, 1, 5, HL_FN, {0x7835e0, 0x77be20, 0x781c40}},
    {"its sceHmdGetDeviceInformationByHandle (tick_frame)", {0x54aff4, 0x54a3e4, 0x549534}, 5,
     {{0xe8, 0x17, 0x98, 0x23, 0x00}, {0xe8, 0x27, 0x2c, 0x23, 0x00},
      {0xe8, 0x37, 0x99, 0x23, 0x00}}, 1, 5, HL_FN, {0x784810, 0x77d010, 0x782e70}},
    {"the game's speed write: the applied copy", {0x5597c5, 0x5596d5, 0x557cd5}, 8,
     {{0xc5, 0xfa, 0x11, 0x05, 0x6b, 0xb2, 0x37, 0x00}, {0xc5, 0xfa, 0x11, 0x05, 0x5b, 0x33, 0x37, 0x00},
      {0xc5, 0xfa, 0x11, 0x05, 0x5b, 0xcd, 0x37, 0x00}}, 4, 8, HL_APPLIED},
    {"the game's speed write: the speed", {0x5597cd, 0x5596dd, 0x557cdd}, 8,
     {{0xc5, 0xfa, 0x11, 0x05, 0x9b, 0xb8, 0x36, 0x00}, {0xc5, 0xfa, 0x11, 0x05, 0x8b, 0x39, 0x36, 0x00},
      {0xc5, 0xfa, 0x11, 0x05, 0x8b, 0xd3, 0x36, 0x00}}, 4, 8, HL_SPEED},
};
static int g_hold_state; /* 0 not checked, 1 on, -1 off (the main thread's) */
/* once a game: the table against itself, the live code against the table, the data writable */
static int tick_hold_check(const u8 *base, int g) {
  for (u32 i = 0; i < sizeof k_hold_sites / sizeof *k_hold_sites; i++) {
    const struct hold_site *s = &k_hold_sites[i];
    s32 d;
    memcpy(&d, s->code[g] + s->disp, 4);
    u64 to = (u64)s->va[g] + s->end + (u64)(s64)d;
    u64 want = s->field == HL_SPEED ? k_hold[g].speed : s->field == HL_APPLIED ? k_hold[g].applied : s->fn[g];
    if (to != want) {
      LOG("tick: hold: the table's site \"%s\" leads to 0x%lx, not 0x%lx: off (a mistake in hooks.c)", s->what, to,
          want);
      return -1;
    }
    if (!readable(base + s->va[g], s->len) || memcmp(base + s->va[g], s->code[g], s->len) != 0) {
      LOG("tick: hold: the game's code at 0x%x (%s) not as expected: off", s->va[g], s->what);
      return -1;
    }
  }
  if (k_hold_sites[3].va[g] + 5 != k_hold[g].wait_ret || !writable(base + k_hold[g].speed, 4) ||
      !readable(base + k_hold[g].applied, 4)) {
    LOG("tick: hold: the speed at 0x%lx not writable memory: off", k_hold[g].speed);
    return -1;
  }
  LOG("tick: hold: %s checked (%u sites), the speed %f, its applied copy %f", game_name(g),
      (u32)(sizeof k_hold_sites / sizeof *k_hold_sites), *(const float *)(base + k_hold[g].speed),
      *(const float *)(base + k_hold[g].applied));
  return 1;
}
/* the speed put back (the main thread, after the update): unless the game wrote its own meanwhile */
static void tick_hold_end(int late) {
  if (!g_hold.on)
    return;
  u8 *base = (u8 *)astro_hdr.game_base;
  volatile float *sp = (volatile float *)(base + k_hold[g_hold.g].speed);
  float applied = *(volatile const float *)(base + k_hold[g_hold.g].applied);
  if (*sp != 0.0f)
    g_hold.game++; /* the game's speed write (0x5597cd) during the update: its value stays */
  else
    *sp = applied != g_hold.applied ? applied : g_hold.saved; /* (a new speed the game applied, 0 included) */
  g_hold.on = 0;
  if (late)
    g_hold.late++;
  else
    g_hold.wait++;
}
/* the loop-top wait of the main loop: the first hooked call after its update */
static s64 h_sceKernelWaitEventFlag(s64 a, s64 b, s64 c, s64 d, s64 e, s64 f) {
  if (g_hold.on && (const u8 *)__builtin_return_address(0) ==
                       (const u8 *)astro_hdr.game_base + k_hold[g_hold.g].wait_ret)
    tick_hold_end(0);
  u32 vt = vrtimers_pending(); /* agent.vrtimers: the no-tick loop's update done, its steps undone */
  if (vt && (const u8 *)__builtin_return_address(0) == (const u8 *)astro_hdr.game_base + k_hold[vt - 1].wait_ret)
    vrtimers_end(0);
  rtime_note(RT_WAIT, 0, 0); /* (agent.rtime=1: the thread's CPU time; nothing when off) */
  return ((s64(*)(s64, s64, s64, s64, s64, s64))orig_sceKernelWaitEventFlag)(a, b, c, d, e, f);
}

/* The VR draw node's water test (logs/research/scale/RESET.md; Scarlet 0x221806.., Fortune
 * 0x2220e6..): water_y x world scale > slot P.y + the head's height term; over: the node's children
 * go to another draw list. Watched (read only) to see whether the scenery lost in the user's worn
 * tests (after the home screen, the play area, sitting down; 2026-11-14) goes with its flips. */
#ifndef RELEASE
static void water_probe(u8 *base, int g) {
  static const struct {
    u64 water, scale, idx, slots, term;
  } k[N_GAMES] = {{0xbe15d4, 0x8d4f80, 0x972485, 0x95b520, 0x1df6524}, {0xbd94e4, 0x8ccf84, 0x96a3f5, 0x953490, 0x1dee404},
                  {0xbe1594, 0x8d4f88, 0x972485, 0x95b520, 0x1df64e4}};
  float w = *(volatile float *)(base + k[g].water), sc = *(volatile float *)(base + k[g].scale);
  float term = *(volatile float *)(base + k[g].term), py = 0;
  s32 idx = *(volatile s8 *)(base + k[g].idx);
  int valid = idx >= 0 && idx <= 4 && *(volatile u8 *)(base + k[g].slots + (u64)idx * 0x1510 + 0x12e8);
  if (valid)
    py = *(volatile float *)(base + k[g].slots + (u64)idx * 0x1510 + 4);
  int under = valid && w * sc > py + term;
  static int last = -1;
  static u32 said, n_under, n;
  static u64 t10;
  n++, n_under += under;
  if (under != last && said++ < 60)
    LOG("water: the test %s (water %.3f x scale %.3f = %.3f against P.y %.3f + term %.3f = %.3f; slot %d%s)",
        under ? "OVER (the other draw list)" : "under (the scene's list)", (double)w, (double)sc, (double)(w * sc),
        (double)py, (double)term, (double)(py + term), idx, valid ? "" : ", not valid");
  last = under;
  u64 t = sceKernelGetProcessTime();
  if (!t10)
    t10 = t;
  if (t - t10 >= 10000000) {
    LOG_DETAIL("water: the last 10 s, %u of %u loops over (term %.3f, P.y %.3f, water x scale %.3f)", n_under, n,
               (double)term, (double)py, (double)(w * sc));
    n = n_under = 0, t10 = t;
  }
}
#endif

static void tick_frame(const u8 *ret) {
  static const struct {
    u64 ret, idx, ring, table, speed, pause;
  } k_tick[] = {
      {0x54aff9, 0xbf2348, 0xbf2350, 0x80f190, 0x8c5070, 0xbea2ac}, /* Scarlet 1.08 */
      {0x54a3e9, 0xbea248, 0xbea250, 0x807140, 0x8bd070, 0xbe21cc}, /* Fortune 1.19 */
      {0x549539, 0xbf2308, 0xbf2310, 0x80d7e0, 0x8c5070, 0xbea26c}, /* Scarlet 1.00 */
  }; /* (g: game_name's order, height.c's k_games) */
  u8 *base = (u8 *)astro_hdr.game_base;
  if (base && g_opt_realheight) /* agent.realheight: after a view reset, the scene's seat at the new height (height.c) */
    for (int i = 0; i < N_GAMES; i++)
      if (ret == base + k_tick[i].ret)
        height_tick(base, i);
  if ((!g_opt_ticklog && !g_opt_halftick && !g_opt_touchprobe && !g_opt_touch && !g_opt_lookat &&
       !g_opt_rateprobe && astro_hdr.frame_pace != 1) ||
      !base) {
    lookat_tick(base, -1); /* (off: its count forgotten, as below) */
    return;
  }
  int g = -1;
  for (int i = 0; i < N_GAMES; i++)
    if (ret == base + k_tick[i].ret)
      g = i;
  if (g < 0)
    return;
  if (g_hold.on) /* agent.tickhold: the last update's hold not undone at the loop-top wait (skipped) */
    tick_hold_end(1);
  if (vrtimers_pending()) /* agent.vrtimers: the last no-tick loop's steps not undone at its wait (skipped) */
    vrtimers_end(1);
  if (g_opt_touch) /* agent.touch=1: the controller spheres made once a scene (touch.c), before the probe */
    touch_tick(base, g);
  /* agent.lookat=1: the dead look-at call when the close-contact count rose (vrreact.c); called when
   * off too, so that it forgets its count: switched on again in the menu, a contact made while it
   * was off is not answered late */
  lookat_tick(base, g);
  if (g_opt_touchprobe && g_vr.in_vr)
    touch_probe(base, g);
#ifndef RELEASE
  if (g_vr.in_vr) /* (debug: a read-only probe) */
    water_probe(base, g);
#endif
  if (g_vr.in_vr) { /* the game's VR "GPU too slow" fallback (logs/research/scale/SCENERY.md): its level
                     * (0, 1: the stage's scenery hidden but 19 nodes, 2) and counter, a change logged */
    static const u64 k_lvl[N_GAMES][2] = {{0x8d4f9c, 0x8d4270}, {0x8ccfa0, 0x8cc260}, {0x8d4fa4, 0x8d4270}};
    static s32 last_lvl = -1;
    static u32 said;
    s32 lvl = *(volatile s32 *)(base + k_lvl[g][0]), cnt = *(volatile s32 *)(base + k_lvl[g][1]);
    if (lvl != last_lvl && said++ < 20)
      LOG("gpufallback: the game's VR quality level %d -> %d (counter %d)%s", last_lvl, lvl, cnt,
          lvl > 0 ? ": the scenery cut (the game judged its GPU too slow)" : "");
    last_lvl = lvl;
  }
  { /* The device allocator's fallback flags (logs/research/mode0/REPORT.md): a render target made as a
     * view of mode 0's memory that did not fit there gets new memory and sets a bit here (Scarlet
     * 0x1c31bec, Fortune 0x1c29acc). The modes' views are made at the game's boot, so with
     * agent.renderw / widew / mode0 the value at the first loop is the one to compare with a run
     * without them (a VR view that did not fit: its depth and colour2 apart: stop); a change later
     * means a target made since did not fit. */
    static const u64 k_flag[N_GAMES] = {0x1c31bec, 0x1c29acc, 0x1c31bac};
    static u32 first, said, last;
    u32 fl = *(volatile u32 *)(base + k_flag[g]);
    if (!said++)
      first = last = fl, LOG("tick: the allocator's fallback flags at the first loop: 0x%x", fl);
    else if (fl != last) {
      LOG("tick: the allocator's fallback flags 0x%x -> 0x%x%s", last, fl,
          fl != first ? ": a render target got memory of its own (agent.renderw / widew too big?)" : "");
      last = fl;
    }
  }
  tick_cpu_note(&g_tick_main);
  {
    u64 w = now_us();
    if (g_tick_wall_last && w - g_tick_wall_last > g_tick_wall_max)
      g_tick_wall_max = w - g_tick_wall_last, g_tick_wall_max_at = w;
    g_tick_wall_last = w;
  }
  u32 cur = *(volatile u32 *)(base + k_tick[g].idx);
  s32 *e = (s32 *)(base + k_tick[g].ring + ((cur + 2) & 3) * 16); /* mode, catch-up allowed, extra ticks, extra vsyncs */
  u32 mode = (u32)e[0];
  s32 table = mode < 5 ? *(s32 *)(base + k_tick[g].table + mode * 12 + 4) : 1, extra = e[2];
  static u32 frames, zero, modes, catchup, own_extra, paused, halved, odd;
  static s32 ticks;
  static float smin, smax;
  static u64 next;
  if (extra)
    own_extra++;
  /* (only while a scene is drawn: on the flat pages the menus took the presses at half speed
   * and the unattended route lost its place) */
  if (g_opt_halftick && g_vr.in_vr && now_us() - __atomic_load_n(&g_tick_scene_us, __ATOMIC_RELAXED) < 200000 &&
      table == 1 && extra == 0 && (odd++ & 1)) {
    e[2] = extra = -1;
    halved++;
  }
  /* a picture every refresh (frame_pace 1): a loop a refresh, a tick every two refreshes. The
   * ticks follow the refresh count, not a fixed 1,0 pattern, so a late loop is made up for (two
   * ticks at the most in one loop) and the game keeps 60 ticks a second. */
  g_tick_none = 0;
  if (astro_hdr.frame_pace == 1 && g_vr.in_vr && table == 1) {
    static u64 ref0, done;
    u64 due = (g_vr.vbl_count - ref0) / 2;
    if (!ref0 || due < done || due - done > 6) /* the start, or after a stall: from here */
      ref0 = g_vr.vbl_count - 1, due = 0, done = 0;
    u32 run = due - done > 2 ? 2 : (u32)(due - done);
    done += run;
    e[2] = extra = (s32)run - table;
    if (run == 0)
      halved++;
    g_tick_none = run == 0;
  }
  if (g_opt_vrtimers && astro_hdr.frame_pace == 1) { /* agent.vrtimers: the per-loop timers (vrreact.c) */
    if (g_tick_none)
      vrtimers_begin(base, g);
    vrreact_window();
  }
  float speed = *(float *)(base + k_tick[g].speed);
  if (!frames || speed < smin)
    smin = speed;
  if (!frames || speed > smax)
    smax = speed;
  /* agent.tickhold: a loop the scheduler gave no tick runs its update with the speed at 0 */
  if (g_opt_tickhold && astro_hdr.frame_pace == 1 && g_tick_none) {
    if (!g_hold_state)
      g_hold_state = tick_hold_check(base, g);
    if (g_hold_state == 1) {
      if (speed > 0.0f && speed < 1000.0f) { /* (a NaN fails both) */
        g_hold.g = g, g_hold.saved = speed;
        g_hold.applied = *(volatile const float *)(base + k_hold[g].applied);
        *(volatile float *)(base + k_hold[g].speed) = 0.0f;
        g_hold.on = 1;
        g_hold.n++;
      } else
        g_hold.skipped++;
    }
  }
  /* agent.uihold: the menu/message layer's span skipped by the payload's jump on this loop when it
   * has no tick (the flag written every loop; vrreact.c) */
  uihold_tick(base, g, g_tick_none);
  /* agent.rateprobe: the per-loop timers' rises over 10 s (read only; vrreact.c) */
  if (g_opt_rateprobe)
    rateprobe_tick(base, g, g_tick_none, table + extra);
  frames++;
  ticks += table + extra > 0 ? table + extra : 0;
  zero += table + extra <= 0;
  modes |= 1u << (mode < 31 ? mode : 31);
  catchup += *(u8 *)(e + 1) != 0;
  paused += *(u8 *)(base + k_tick[g].pause) != 0;
  u64 now = now_us();
  if (!next)
    next = now + 10000000;
  if (now >= next) {
    /* printed on the agent's cvars thread (tick_report): a LOG here, on the game's main thread, may
     * be what held one loop up for ~35 ms in every 10 s window (2026-11-13) */
    if (!__atomic_load_n(&g_tick_rep.due, __ATOMIC_ACQUIRE)) {
      g_tick_rep.frames = frames, g_tick_rep.ticks = ticks, g_tick_rep.zero = zero, g_tick_rep.halved = halved;
      g_tick_rep.modes = modes, g_tick_rep.catchup = catchup, g_tick_rep.own_extra = own_extra;
      g_tick_rep.paused = paused, g_tick_rep.smin = smin, g_tick_rep.smax = smax, g_tick_rep.in_vr = g_vr.in_vr;
      g_tick_rep.main_avg = g_tick_main.n ? g_tick_main.sum / g_tick_main.n : 0, g_tick_rep.main_max = g_tick_main.max;
      g_tick_rep.main_n = g_tick_main.n, g_tick_rep.main_max_at = g_tick_main.max_at;
      g_tick_rep.wall_max = g_tick_wall_max, g_tick_rep.wall_max_at = g_tick_wall_max_at;
      g_tick_rep.render_avg = g_tick_render.n ? g_tick_render.sum / g_tick_render.n : 0;
      g_tick_rep.render_max = g_tick_render.max, g_tick_rep.render_n = g_tick_render.n;
      g_tick_rep.held = g_hold.n, g_tick_rep.held_wait = g_hold.wait, g_tick_rep.held_late = g_hold.late;
      g_tick_rep.held_game = g_hold.game, g_tick_rep.held_skipped = g_hold.skipped;
      g_tick_rep.hold_state = (u32)g_hold_state;
      __atomic_store_n(&g_tick_rep.due, 1, __ATOMIC_RELEASE);
    }
    g_hold.n = g_hold.wait = g_hold.late = g_hold.game = g_hold.skipped = 0;
    g_tick_wall_max = 0;
    g_tick_main.sum = g_tick_main.max = g_tick_render.sum = g_tick_render.max = 0;
    g_tick_main.n = g_tick_render.n = 0;
    frames = zero = modes = catchup = own_extra = paused = halved = 0;
    ticks = 0;
    next = now + 10000000;
  }
}

static s32 h_sceHmdGetDeviceInformationByHandle(s32 handle, struct hmd_device_information *info) {
  static u32 n;
  tick_frame((const u8 *)__builtin_return_address(0));
  s32 rc = h_sceHmdGetDeviceInformation(info);
  LOG_FIRST(n, 2, "hmd: GetDeviceInformationByHandle(0x%x) -> 0x%x (status %u)", handle, rc,
            info ? info->status : 0);
  return rc;
}

/* The order the game reads the structure in: the first tangent is the one at the left eye's left
 * (outer) edge. 0: out, in. */
#define GAME_FOV_IN_FIRST 0
static s32 h_sceHmdGetFieldOfView(s32 handle, struct hmd_fov *fov) {
  if (!fov)
    return HMD_ERR_PARAMETER_NULL;
  *fov = g_vr.title_fov;
  if (GAME_FOV_IN_FIRST)
    fov->tan_out = g_vr.title_fov.tan_in, fov->tan_in = g_vr.title_fov.tan_out;
  LOG("hmd: GetFieldOfView(0x%x) -> %f %f %f %f (%s)", handle, fov->tan_out, fov->tan_in,
      fov->tan_top, fov->tan_bottom, GAME_FOV_IN_FIRST ? "inner first, as this game reads it"
                                                       : "outer first");
  return 0;
}

static s32 h_sceHmdGet2DEyeOffset(s32 handle, struct hmd_eye_offset *l, struct hmd_eye_offset *r) {
  if (!l || !r)
    return HMD_ERR_PARAMETER_NULL;
  memset(l, 0, sizeof *l);
  memset(r, 0, sizeof *r);
  l->x = -g_vr.ipd * 0.5f;
  r->x = g_vr.ipd * 0.5f;
  LOG("hmd: Get2DEyeOffset(0x%x) -> +-%f", handle, r->x);
  return 0;
}

/* ---------------------------------------------------------------- reprojection */
static void log_tsharp(const char *what, const u32 *t);
static s32 h_sceHmdReprojectionInitialize(const struct hmd_reprojection_resource *res, s32 type,
                                          void *option) {
  if (res)
    LOG("hmd: ReprojectionInitialize(type %d) onion %p garlic %p prio %d mask 0x%lx pipe %u "
        "queue %u",
        type, res->onion_buff, res->garlic_buff, res->thread_priority, res->cpu_affinity_mask,
        res->pipe_id, res->queue_id);
  if (res) {
    g_vr.reproj_prio = res->thread_priority;
    g_vr.reproj_affinity = res->cpu_affinity_mask;
  }
  g_vr.reproj_initialized = 1;
  return 0;
}

static s32 h_sceHmdReprojectionSetDisplayBuffers(s32 vo, s32 i0, s32 i1, void *option) {
  LOG("hmd: ReprojectionSetDisplayBuffers(vo 0x%x, %d, %d, %p)", vo, i0, i1, option);
  g_vr.display_vo = vo;
  g_vr.display_index[0] = i0;
  g_vr.display_index[1] = i1;
  labels_reset();
  __atomic_store_n(&g_vr.in_vr, 1, __ATOMIC_RELEASE); /* (the game's VR mode: its port is PS VR's) */
  hooks_tv_rate_sync(); /* (the port at 0: the TV mirror's 60 flips a second) */
  return 0;
}

static s32 h_sceHmdReprojectionSetOutputMinColor(float r, float g, float b) {
  LOG("hmd: ReprojectionSetOutputMinColor(%f %f %f)", r, g, b);
  return 0;
}

static void *vblank_thread(void *arg);
static void raise_vblank(u32 k, int shown, int counted);
static volatile u32 g_stall_req; /* agent.stall: a one-shot hold of the refresh events, ms */
static volatile u32 g_opt_noresume; /* agent.noresume=1 (debug): the system's resume event kept from the game */
static volatile u32 g_vbl_lost; /* the vblank thread went from the headset's refreshes to its clock (per report) */
static volatile u32 g_vbl_readd; /* the game's start event found deleted at a refresh, added again (per report) */
static int vbl_sleep(u64 until, int on_flip);

/* ---- the pace: a frame every two refreshes, as PS VR had it ----
 * Nothing of the game's own timing is changed (its 60 fps, each frame shown for two of the
 * headset's 120 Hz refreshes). The vblank thread is woken as Hmd2 flips (gen1.c), so the game's
 * refresh event follows the flip within microseconds, and every 10 s the log says how Hmd2's
 * flips went. */
volatile u32 g_refresh_hz;
static void *volatile g_wake_eq;
#define WAKE_FLIP 2
void hooks_wake(void) {
  void *eq = g_wake_eq;
  if (eq)
    sceKernelTriggerUserEvent(eq, WAKE_FLIP, 0);
}
volatile u32 g_late_pass, g_release_n;   /* vr2.c's late pass: nothing aims frames later here */
static volatile u32 g_shown, g_repeated; /* Hmd2's flips: a new frame, the same one again */

/* the flight recorder's counters (a snapshot a second, logged around a mark: rec_tick, below) */
static volatile u32 g_rec_shown, g_rec_again, g_rec_clockfb, g_rec_dmem_refused, g_rec_gpu_max;
static volatile u32 g_pool_retries; /* the CPU pool's 224 MB retries refused (logged as a count) */
static u64 g_flex_min = ~0ull; /* the game's free flexible memory, its lowest this session */
extern volatile u32 g_rec_pose_max, g_rec_reseats; /* vr.c */
/* Frames never shown, counted exactly: a frame number missing from Hmd2's flips (vr2.c numbers the
 * frames it passes in order), and frames the hand-off itself skipped (hooks_note_unpassed). It was
 * "handed over minus shown" per 10 s window, which miscounted a frame handed over at a window's end
 * and shown in the next one (one "never shown" there, one more shown than handed over in the next:
 * none was lost), more often once frames wait for their drawing before the headset. */
static volatile u32 g_never, g_rec_never;
void hooks_note_unpassed(u32 n, u32 why) {
  __atomic_fetch_add(&g_never, n, __ATOMIC_RELAXED);
  __atomic_fetch_add(&g_rec_never, n, __ATOMIC_RELAXED);
  /* when and why (the first 60, then every 100th) */
  static volatile u32 said;
  u32 k = __atomic_fetch_add(&said, 1, __ATOMIC_RELAXED);
  if (k < 60 || k % 100 == 0)
    LOG_DETAIL("hmd: %u frame(s) never shown (%s) at %lu us, frame %u handed over", n,
        why == 1 ? "the newest passed instead" : why == 2 ? "gone from the ring" : why == 3 ?
        "overwritten before passed" : "a flip skipped it", (u64)now_us(), g_vr.frame_w);
}
/* The frame's count, and whether the game's GPU has drawn the frame of a count: Dead or Alive
 * Xtreme 3 gives none to read (a frame is handed over at its end of pipe, drawn), so the count is
 * 0 and every frame counts as drawn (the mirror's wait, rtime.c's frames). */
u64 hooks_frame_count(void) { return 0; }
int hooks_frame_drawn(u64 count) {
  (void)count;
  return 1;
}
u64 hooks_drawn_label(void) { return 0; }
/* Hmd2 flipped the headset (gen1.c): the vblank thread's wake, and what the flip shows */
/* How many refreshes each frame stayed on the headset (1, 2, 3, 4 or more), per 10 s: at 60 fps
 * every frame should stay two. Uneven (one, then three) is judder and doubled edges in motion, which
 * the totals above do not show (they come out the same). */
static volatile u32 g_held[5], g_held_run;
void hooks_flip(u32 flip_arg) {
  static u32 last;
  if (flip_arg == last)
    g_repeated++, g_rec_again++, g_held_run++;
  else {
    if (last) {
      u32 r = g_held_run;
      g_held[r >= 4 ? 4 : r]++;
    }
    g_held_run = 1;
    if (last && flip_arg > last + 1 && flip_arg - last < 64) /* passed, replaced before shown */
      hooks_note_unpassed(flip_arg - last - 1, 4);
    g_shown++, g_rec_shown++, last = flip_arg;
  }
  hooks_wake();
}

static s32 h_sceHmdReprojectionSetUserEventEnd(void *eq, s32 id) {
  LOG("hmd: ReprojectionSetUserEventEnd(eq %p, id %d)", eq, id);
  g_vr.end_eq = eq;
  g_vr.end_id = id;
  __atomic_store_n(&g_vr.end_set, 1, __ATOMIC_RELEASE);
  agent_tick();
  return 0;
}

/* Dead or Alive Xtreme 3 arms the reprojection's START event at its boot (id 1, on a queue of its
 * own) and, in VR mode, waits for it at least once a frame: until the label of the eye buffers it
 * is about to draw over is zero, and until its main port's flip count has moved on by two since its
 * last frame (60 fps on the 119.88 Hz port). The real library raises it at each reprojection, one a
 * refresh. Raised here as the end event is (the vblank thread), with the port's flip status
 * counting those refreshes while the game is in VR mode (g_vr.start_pace). */
static s32 h_sceHmdReprojectionSetUserEventStart(void *eq, s32 id) {
  LOG("hmd: ReprojectionSetUserEventStart(eq %p, id %d)", eq, id);
  g_vr.end_eq = eq;
  g_vr.end_id = id;
  g_vr.start_pace = 1;
  __atomic_store_n(&g_vr.end_set, 1, __ATOMIC_RELEASE);
  agent_tick();
  return 0;
}

static s32 h_sceHmdReprojectionFinalize(void) {
  LOG("hmd: ReprojectionFinalize");
  return 0;
}

/* Starts the agent's threads once there is something for them to do (the payload adopts an
 * agent already in a running game rather than putting a new one in). Called from frequent
 * hooks. */
void agent_tick(void) {
  static volatile u32 vblank_started;
  /* An end event given to the reprojection (end_eq) or a vblank event registered (vbl_eq):
   * either is the sign the game paces on PS VR's refreshes */
  if ((g_vr.end_eq || g_vr.vbl_eq) && !vblank_started && once(&vblank_started)) {
    u8 attr[64], thr[16];
    scePthreadAttrInit(attr);
    scePthreadAttrSetstacksize(attr, 256 * 1024);
    int rc = scePthreadCreate(thr, attr, vblank_thread, 0, "DOAX3VR2 vblank");
    scePthreadAttrDestroy(attr);
    LOG("agent: vblank thread -> 0x%x (end eq %p id %d, vblank eq %p)", rc, g_vr.end_eq,
        g_vr.end_id, g_vr.vbl_eq);
    if (rc) /* (the game's pace waits on it without a timeout: tried again at the next call) */
      __atomic_store_n(&vblank_started, 0, __ATOMIC_RELEASE);
  }
  /* PS VR2 once the game has been rendering VR frames for a while (not during its setup). */
  static volatile u32 vr2_once;
  if (g_vr.vr2_started != VR2_BUILD && g_vr.user_id >= 0 && g_vr.frames_in > 120 && !g_opt_novr2 &&
      once(&vr2_once)) {
    g_vr.vr2_started = VR2_BUILD;
    vr2_start();
  }
}

static void log_tsharp(const char *what, const u32 *t) {
  /* GCN T#: base = dw0 | (dw1 & 0xff) << 32 (in 256-byte units), dfmt dw1[25:20], nfmt dw1[29:26],
   * width-1 dw2[13:0], height-1 dw2[27:14], tiling index dw3[24:20], type dw3[31:28],
   * pitch-1 dw4[26:13]. */
  u64 base = ((u64)t[0] | ((u64)(t[1] & 0xff) << 32)) << 8;
  LOG("  %s T# %08x %08x %08x %08x %08x %08x %08x %08x", what, t[0], t[1], t[2], t[3], t[4], t[5],
      t[6], t[7]);
  LOG("  %s base 0x%lx %ux%u dfmt %u nfmt %u tile %u type %u pitch %u", what, base,
      (t[2] & 0x3fff) + 1, ((t[2] >> 14) & 0x3fff) + 1, (t[1] >> 20) & 0x3f, (t[1] >> 26) & 0xf,
      (t[3] >> 20) & 0x1f, t[3] >> 28, ((t[4] >> 13) & 0x3fff) + 1);
}

/* the game's sleeps (sceKernelUsleep): agent.rtime's samples (rtime.c) */
static s32 h_sceKernelUsleep(u32 us) {
  rtime_note(RT_WAIT, 0, 0);
  return CALL_ORIG(s32(*)(u32), sceKernelUsleep, us);
}

/* the game's GPU time a frame: its first command buffer after the last GPU-done event (its
 * graphics EOP, ident 0x40, on its own queue) to the next GPU-done event, the most and the mean
 * of each 10 s */
static volatile u64 g_first_sub, g_gpu_sum;
static volatile u32 g_gpu_max, g_gpu_n;
static void frame_submitted(void) {
  if (!g_first_sub)
    g_first_sub = now_us();
}

static void gpu_done(void);

char g_cvars_text[4096]; /* cvars.txt, then the game's own cvars-<game>.txt (the payload) */
u32 g_cvars_len;
u32 g_opt_resetevent = 1; /* (h_sceSystemServiceReceiveEvent; the console's own behaviour: 0 a debug test) */
u32 g_opt_tvhmd, g_opt_hands = 60 /* (on by default since 2026-11-13, the user: the menu's "on"; 0 watch only) */, g_opt_mirrorboth, g_opt_recentre, g_opt_tvblank,
    g_opt_rtime; /* debug tools: off unless cvars.txt says 1 */
#ifndef RELEASE
static u32 g_cvars_sharpen = 50; /* cvars.txt's agent.sharpen as last read (50, the default, without the line) */
#endif

/* the text, once; its agent.* lines are taken on the spot */
void cvars_fetch(void) {
  static volatile u32 started, done;
  if (!once(&started)) { /* another thread fetches it: its result, up to 5 s */
    for (int i = 0; i < 500 && !__atomic_load_n(&done, __ATOMIC_ACQUIRE); i++)
      sceKernelUsleep(10000);
    return;
  }
  agent_call(CMD_CVARS, (u64)g_cvars_text, sizeof g_cvars_text, 0, 0, 3000, 0, 0);
  g_cvars_text[sizeof g_cvars_text - 1] = 0;
  g_cvars_len = (u32)strlen(g_cvars_text);
  if (!g_cvars_len) {
    menu_boot(); /* (the settings menu's file: its lines over the defaults) */
    __atomic_store_n(&done, 1, __ATOMIC_RELEASE);
    return;
  }
  LOG("cvars: %u bytes of cvars.txt", g_cvars_len);
  for (const char *p = g_cvars_text; *p;) {
    const char *line = p;
    while (*p && *p != '\n') p++;
    u32 n = (u32)(p - line);
    if (*p) p++;
    if (n < 7 || memcmp(line, "agent.", 6) != 0)
      continue;
    const char *eq = line + 6;
    while (eq < line + n && *eq != '=') eq++;
    if (eq >= line + n)
      continue;
    u32 v = 0;
    for (const char *d = eq + 1; d < line + n && *d >= '0' && *d <= '9'; d++) v = v * 10 + (u32)(*d - '0');
    u32 klen = (u32)(eq - line - 6);
    char key[32];
    if (klen >= sizeof key)
      klen = sizeof key - 1;
    memcpy(key, line + 6, klen);
    key[klen] = 0;
#ifdef RELEASE
    /* a release build takes the player's options only: the debug and test ones are ignored */
    static const char *const k_debug[] = {"rtime",    "tvhmd",   "capture",  "novr2",   "handload",
                                         "handstep", "nopassport", "rowscan", "inv",     "give",
                                         "giveport", "padtest", "chars",    "closet",  "shotslice",
                                         "shoteye",  "gaze",    "clockhours", "ticklog", "halftick", "touchprobe",
                                         "nearoffset", "nohole",
                                         "uihold",  "rateprobe", "padlog", "padheldw", "widew", "mode0", "nearn",
                                         "renderw", /* (the settings menu's Resolution sets the sizes) */
                                         "sharpenwide", "resetevent", "noresume", "stall", "composefast",
                                         "heightoffset", "padreach", "handpredict"};
    /* (the player's, the settings menu's since 2026-11-14: agent.touch, touchhead, lookat, gazefollow,
     * movehole, sharpen) */
    int debug = 0;
    for (u32 i = 0; i < sizeof k_debug / sizeof *k_debug; i++)
      debug |= strlen(k_debug[i]) == klen && memcmp(key, k_debug[i], klen) == 0;
    if (debug) {
      LOG("cvars: agent.%s is a debug option: not in a release build", key);
      continue;
    }
#endif
    if (klen == 5 && memcmp(key, "hands", 5) == 0)
      g_opt_hands = v;
    else if (klen == 8 && memcmp(key, "handload", 8) == 0)
      g_opt_handload = v;
    else if (klen == 8 && memcmp(key, "handstep", 8) == 0)
      g_opt_handstep = v;
    else if (klen == 10 && memcmp(key, "nopassport", 10) == 0)
      g_opt_nopassport = v;
    else if (klen == 9 && memcmp(key, "errdialog", 9) == 0)
      g_opt_errdialog = v;
    else if (klen == 7 && memcmp(key, "compose", 7) == 0)
      g_opt_compose = v;
    else if (klen == 8 && memcmp(key, "vrunlock", 8) == 0)
      g_opt_vrunlock = v;
    else if (klen == 4 && memcmp(key, "port", 4) == 0)
      g_opt_port = v;
    else if (klen == 7 && memcmp(key, "rowscan", 7) == 0)
      g_opt_rowscan = v;
    else if (klen == 3 && memcmp(key, "inv", 3) == 0)
      g_opt_inv = v;
    else if (klen == 4 && memcmp(key, "give", 4) == 0)
      g_opt_give = v;
    else if (klen == 9 && memcmp(key, "fanswings", 9) == 0)
      g_opt_fanswings = v;
    else if (klen == 7 && memcmp(key, "padtest", 7) == 0)
      g_opt_padtest = v;
    else if (klen == 5 && memcmp(key, "chars", 5) == 0)
      g_opt_chars = v;
    else if (klen == 8 && memcmp(key, "giveport", 8) == 0)
      g_opt_giveport = v;
    else if (klen == 6 && memcmp(key, "closet", 6) == 0)
      g_opt_closet = v;
    else if (klen == 10 && memcmp(key, "clockhours", 10) == 0)
      g_opt_clockhours = v;
    else if (klen == 7 && memcmp(key, "special", 7) == 0)
      g_opt_special = v;
    else if (klen == 8 && memcmp(key, "starfans", 8) == 0)
      g_opt_starfans = v;
    else if (klen == 10 && memcmp(key, "ownersuits", 10) == 0)
      continue; /* the payload's, at the game's start (the Owner Shop's swimsuit filters) */
    else if ((klen == 10 && memcmp(key, "vrpassport", 10) == 0) || (klen == 6 && memcmp(key, "netlog", 6) == 0) ||
             (klen == 9 && memcmp(key, "bootnotes", 9) == 0))
      continue; /* the payload's (the VR Passport patch; the log to port 9025; the start-up boxes) */
    else if (klen == 7 && memcmp(key, "sensors", 7) == 0)
      g_opt_sensors = v;
    else if (klen == 8 && memcmp(key, "portshop", 8) == 0)
      g_opt_portshop = v;
    else if (klen == 8 && memcmp(key, "portgive", 8) == 0)
      g_opt_portgive = v;
    else if (klen == 4 && memcmp(key, "wfix", 4) == 0)
      g_opt_wfix = v;
    else if (klen == 9 && memcmp(key, "shotslice", 9) == 0)
      g_opt_shotslice = v;
    else if (klen == 7 && memcmp(key, "shoteye", 7) == 0)
      g_opt_shoteye = v;
    else if (klen == 7 && memcmp(key, "ownerlv", 7) == 0)
      g_opt_ownerlv = v > 110 ? 110 : v;
    else if (klen == 5 && memcmp(key, "money", 5) == 0)
      g_opt_money = v > 3000000 ? 3000000 : v;
    else if (klen == 11 && memcmp(key, "malfunction", 11) == 0)
      g_opt_malfunction = v;
    else if (klen == 11 && memcmp(key, "cutfeatures", 11) == 0)
      continue; /* the payload's, at the game's start (patch_cut_features) */
    else if (klen == 10 && memcmp(key, "composemax", 10) == 0)
      g_opt_composemax = v < 1024 ? 1024 : v > 4096 ? 4096 : v;
    else if ((klen == 7 && memcmp(key, "sharpen", 7) == 0) || (klen == 11 && memcmp(key, "sharpenwide", 11) == 0)) {
      extern volatile u32 g_opt_sharpen, g_opt_sharpenwide; /* compose.c's (also followed live: cvars_live_thread) */
      v = v > 100 ? 100 : v;
      *(klen == 7 ? &g_opt_sharpen : &g_opt_sharpenwide) = v;
#ifndef RELEASE
      if (klen == 7)
        g_cvars_sharpen = v; /* (the live reading's last: the menu's choice kept until the line changes) */
#endif
    }
    else if (klen == 11 && memcmp(key, "composefast", 11) == 0) {
      extern volatile u32 g_opt_composefast; /* compose.c's (also followed live: cvars_live_thread) */
      g_opt_composefast = v & 3;
    }
    else if (klen == 5 && memcmp(key, "novr2", 5) == 0)
      g_opt_novr2 = v; /* an observing run: the game's PS VR calls answered and logged, PS VR2 never started */
    else if (klen == 10 && memcmp(key, "mirrorboth", 10) == 0)
      g_opt_mirrorboth = v;
    else if (klen == 5 && memcmp(key, "stall", 5) == 0) { /* (debug: once for each new value) */
      static u32 last_stall;
      if (v != last_stall)
        last_stall = v, g_stall_req = v;
    } else if (klen == 8 && memcmp(key, "noresume", 8) == 0)
      g_opt_noresume = v;
    else if (klen == 10 && memcmp(key, "resetevent", 10) == 0)
      g_opt_resetevent = v;
    else if (klen == 8 && memcmp(key, "recentre", 8) == 0)
      g_opt_recentre = v;
    else if (klen == 5 && memcmp(key, "rtime", 5) == 0)
      g_opt_rtime = v = v != 0;
    else if ((klen == 4 && memcmp(key, "fxaa", 4) == 0) || (klen == 6 && memcmp(key, "fps120", 6) == 0) ||
             (klen == 8 && memcmp(key, "headfade", 8) == 0) || (klen == 7 && memcmp(key, "renderw", 7) == 0) ||
             (klen == 10 && memcmp(key, "worldscale", 10) == 0) || (klen == 3 && memcmp(key, "res", 3) == 0))
      menu_cvar(key, klen, v); /* the payload's, taken at the game's start (patch_fxaa, patch_worldscale,
                                * res_settings, the header's frame_pace): the menu's start-only lines'
                                * defaults (agent.renderw a debug build's: res_settings) */
    else if (klen == 10 && memcmp(key, "realheight", 10) == 0)
      g_opt_realheight = v = v != 0; /* the location scene's seat at the player's height (height.c) */
    else if (klen == 12 && memcmp(key, "heightoffset", 12) == 0) {
      /* cm, signed, -30..80: added to the seat's raise (height.c; debug) */
      int neg = eq + 1 < line + n && eq[1] == '-';
      u32 m = 0;
      for (const char *d = eq + 1 + neg; d < line + n && *d >= '0' && *d <= '9'; d++)
        m = m < 1000 ? m * 10 + (u32)(*d - '0') : m;
      s32 cm = neg ? -(s32)(m > 30 ? 30 : m) : (s32)(m > 80 ? 80 : m);
      g_opt_heightoffset = cm;
      LOG("cvars: agent.heightoffset = %d cm", cm);
      continue;
    }
    else if (klen == 8 && memcmp(key, "padreach", 8) == 0) {
      extern u32 g_opt_padreach;
      g_opt_padreach = v = v != 0;
    } else if (klen == 11 && memcmp(key, "handpredict", 11) == 0) {
      extern u32 g_opt_handpredict;
      g_opt_handpredict = v = v != 0;
    }
    else if (vr_pad_option(key, klen, &v))
      ; /* vr.c's: agent.padmode, padlog, padlead, padimu (the controller's placement) */
    else if (klen == 10 && memcmp(key, "touchprobe", 10) == 0)
      g_opt_touchprobe = v = v != 0;
    else if (klen == 5 && memcmp(key, "touch", 5) == 0)
      g_opt_touch = v = v != 0; /* the controller's touch spheres restored (touch.c) */
    else if (klen == 9 && memcmp(key, "touchhead", 9) == 0)
      g_opt_touchhead = v = v != 0; /* 0: the head's spheres left out (touch.c) */
    else if (klen == 6 && memcmp(key, "lookat", 6) == 0)
      g_opt_lookat = v = v != 0; /* the dead look-at call made when the close-contact count rises (vrreact.c) */
    else if (klen == 8 && memcmp(key, "vrtimers", 8) == 0)
      g_opt_vrtimers = v = v != 0; /* the per-loop timers held on no-tick loops at 120 fps (vrreact.c; on by default) */
    else if (klen == 6 && memcmp(key, "uihold", 6) == 0)
      g_opt_uihold = v = v != 0; /* the menu/message layer skipped on no-tick loops (the payload's jump; also live) */
    else if (klen == 9 && memcmp(key, "rateprobe", 9) == 0)
      g_opt_rateprobe = v = v != 0; /* the per-loop timers' rates logged every 10 s (vrreact.c) */
    else if (klen == 7 && memcmp(key, "ticklog", 7) == 0)
      g_opt_ticklog = v = v != 0;
    else if (klen == 8 && memcmp(key, "halftick", 8) == 0)
      g_opt_halftick = v = v != 0;
    else if (klen == 8 && memcmp(key, "tickhold", 8) == 0)
      g_opt_tickhold = v = v != 0; /* (on by default; also followed live: cvars_live_thread) */
    else if (klen == 5 && memcmp(key, "tvhmd", 5) == 0)
      g_opt_tvhmd = v = v != 0;
    else if (klen == 7 && memcmp(key, "tvblank", 7) == 0)
      g_opt_tvblank = v;
    else if (klen == 7 && memcmp(key, "capture", 7) == 0)
      continue; /* followed live (cvars_live_thread): a TV capture each new value (mirror.c) */
    else if (fov_option(key, klen, eq + 1, (u32)(line + n - eq - 1)))
      continue; /* fov.c's, logged there */
    else if (gaze_option(key, klen, eq + 1, (u32)(line + n - eq - 1)))
      continue; /* gaze.c's (agent.nearoffset, agent.gazefollow, agent.nohole, agent.movehole), logged there */
    else if (viewsize_option(key, klen, eq + 1, (u32)(line + n - eq - 1)))
      continue; /* the payload's (agent.widew, agent.mode0, agent.nearn), logged in viewsize.c */
    else {
      LOG("cvars: agent option '%s' unknown (the options: docs/SETTINGS.md)", key);
      continue;
    }
    LOG("cvars: agent.%s = %u", key, v);
  }
  menu_boot(); /* the settings menu's file: its lines over these (both builds) */
  __atomic_store_n(&done, 1, __ATOMIC_RELEASE);
}

/* ---- the flight recorder ----
 * A snapshot every second of what decides how the headset feels, kept for the last 16 s. The
 * player presses L3+R3 (both sticks in) the moment something feels wrong: the controller buzzes,
 * and the log gets "mark #n" with the 10 s before and the 5 s after, one line a second. Debug
 * builds only (a player would get a buzz they can't explain). */
#ifndef RELEASE
struct rec_snap {
  u32 t_s, handed, shown, again, never, gpu_max_us, pose_max_udeg, reseats, clockfb,
      dmem_refused, worn, hands;
};
static struct rec_snap g_rec_ring[16];
static u32 g_rec_n, g_rec_follow, g_rec_marks;
static volatile u32 g_mark_req;
static volatile s32 g_mark_pad = -1;
static volatile u64 g_mark_buzz_until;
static void rec_log(const char *what, const struct rec_snap *r) {
  LOG("%s %us: %u frames handed, %u shown, %u twice, %u never; GPU %u.%u ms at most; "
      "head step %u.%03u deg at most; %u re-seats, %u clock fallbacks, %u memory refused; "
      "worn %u", what, r->t_s, r->handed, r->shown, r->again,
      r->never, r->gpu_max_us / 1000, r->gpu_max_us / 100 % 10, r->pose_max_udeg / 1000000,
      r->pose_max_udeg / 1000 % 1000, r->reseats, r->clockfb, r->dmem_refused, r->worn);
}
static void rec_tick(void) {
  static u64 next, start;
  static u32 last_handed, last_reseats;
  u64 t = now_us();
  if (g_mark_buzz_until && t >= g_mark_buzz_until) {
    u8 off[2] = {0, 0};
    if (scePadSetVibration && g_mark_pad >= 0)
      scePadSetVibration(g_mark_pad, off);
    g_mark_buzz_until = 0;
  }
  if (g_mark_req) { /* the 10 s before, now; the 5 after as they come */
    g_mark_req = 0;
    g_rec_marks++;
    LOG("mark #%u at %us (L3+R3): the 10 s before, then the 5 after", g_rec_marks,
        start ? (u32)((t - start) / 1000000) : 0);
    u32 have = g_rec_n < 10 ? g_rec_n : 10;
    for (u32 i = have; i > 0; i--)
      rec_log("  before", &g_rec_ring[(g_rec_n - i) % 16]);
    g_rec_follow = 5;
  }
  if (!start)
    start = t, next = t + 1000000;
  if (t < next)
    return;
  next += 1000000;
  struct rec_snap *r = &g_rec_ring[g_rec_n % 16];
  u32 h = g_vr.frames_in, rs = g_rec_reseats;
  r->t_s = (u32)((t - start) / 1000000);
  r->handed = h - last_handed, last_handed = h;
  r->shown = __atomic_exchange_n(&g_rec_shown, 0, __ATOMIC_RELAXED);
  r->again = __atomic_exchange_n(&g_rec_again, 0, __ATOMIC_RELAXED);
  r->never = __atomic_exchange_n(&g_rec_never, 0, __ATOMIC_RELAXED);
  r->gpu_max_us = __atomic_exchange_n(&g_rec_gpu_max, 0, __ATOMIC_RELAXED);
  r->pose_max_udeg = __atomic_exchange_n(&g_rec_pose_max, 0, __ATOMIC_RELAXED);
  r->reseats = rs - last_reseats, last_reseats = rs;
  r->clockfb = __atomic_exchange_n(&g_rec_clockfb, 0, __ATOMIC_RELAXED);
  r->dmem_refused = __atomic_exchange_n(&g_rec_dmem_refused, 0, __ATOMIC_RELAXED);
  r->worn = !g_vr.hmd_off;
  g_rec_n++;
  if (g_rec_follow) {
    g_rec_follow--;
    rec_log("  after", r);
  }
}
/* the controller hook: L3+R3 pressed together (rising) asks for a mark; a short buzz says so */
static void rec_pad(s32 handle, const u8 *data) {
  static u32 was;
  u32 b = *(const u32 *)data, both = (b & 0x6) == 0x6; /* SCE_PAD_BUTTON_L3 0x2, R3 0x4 */
  if (both && !was) {
    g_mark_req = 1;
    g_mark_pad = handle;
    u8 on[2] = {0, 200}; /* large motor, small motor */
    if (scePadSetVibration)
      scePadSetVibration(handle, on);
    g_mark_buzz_until = now_us() + 200000;
  }
  was = both;
}
#endif

/* cvars.txt read again every 2 s while the game runs: a change is applied at once, so settings
 * can be compared worn without a restart. Only what is safe to change mid-game, in a debug build:
 * agent.tvhmd, agent.mirrorboth and the automation's lines (agent.press, agent.shot,
 * agent.shotslice); the other agent options set things up once at the start and are left as they
 * were. */
static char g_cvars_seen[sizeof g_cvars_text];
/* The 10 s report (health_report), made on this thread: the hand-over (the game's render thread)
 * only says it is due. Made in the hand-over it held the game's next frame up once a report:
 * one frame on the headset for one refresh in nearly every 10 s window, at every setting and in
 * stock (2026-11-07, logs/mv80-mv85). */
static volatile u32 g_health_due; /* the frames handed over in the window; 0: none due */
static void health_report(u32 handed);
static void tick_report(void);
static void health_report_due(void) {
  tick_report();
  touch_report();
  vrreact_report();
  viewsize_report();
  u32 d = __atomic_exchange_n(&g_health_due, 0, __ATOMIC_ACQ_REL);
  if (!d)
    return;
  u64 t0 = sceKernelGetProcessTime();
  LOG("hmd: the game drew %u frames in the last 10 s (%u.%u a second)", d, d / 10, d % 10);
  health_report(d);
  static u32 said;
  if (said++ < 3)
    LOG("hmd: the 10 s report took %lu us (on the agent's cvars thread)",
        (u64)(sceKernelGetProcessTime() - t0));
}
#ifndef RELEASE
/* a line's number from p to end (its digits; 0 without one) */
static u32 line_u32(const char *p, const char *end) {
  u32 v = 0;
  for (; p < end && *p >= '0' && *p <= '9' && v < 100000000u; p++)
    v = v * 10 + (u32)(*p - '0');
  return v;
}
#endif
#ifndef RELEASE
/* agent.shot: the game's last flipped flat buffer, copied whole (its rows up to a multiple of 64,
 * a 2D-tiled surface's) into memory of the agent's and handed to the payload. Nothing in VR mode
 * (the game flips nothing then: agent.capture shows the headset's picture). */
static int readable(const void *p, u64 n);
static void shot_tick(void) {
  static u32 done;
  static u8 *copy;
  u32 req = g_shot_req;
  s32 idx = g_game_flip_index;
  if (req == done)
    return;
  done = req;
  if (!g_vr.in_vr && g_main_port >= 0 && sceVideoOutGetFlipStatus) { /* (the buffer on screen: the flat menu's) */
    u64 st[8] = {0};
    if (sceVideoOutGetFlipStatus(g_main_port, st) == 0 && (s32)(u32)st[7] >= 0)
      idx = (s32)(u32)st[7];
  }
  u8 *src = idx >= 0 && idx < 16 ? g_vr.vo_buffers[idx] : 0;
  u32 w = g_game_flip_w, h = g_game_flip_h, pitch = (w + 63) & ~63u, slices = 1;
  if (g_vr.in_vr) {
    /* VR mode: the newest frame's texture as the game handed it over (its left eye's T#): the
     * flat picture's one, or the array the four views are slices of (wide L, wide R, near L, near
     * R), all of them: the PC cuts the views out (tools/detile.c; the header line says what) */
    u32 fw = g_vr.frame_w;
    struct vr_frame f;
    if (!fw || !vr_frame_copy(fw, &f)) {
      LOG("shot %u: VR mode, no frame handed over yet", req);
      return;
    }
    const u32 *t = f.tsharp[g_opt_shoteye && f.tsharp[1][0] ? 1 : 0]; /* (agent.shoteye=1: the right eye) */
    src = (u8 *)(((u64)t[0] | (u64)(t[1] & 0xff) << 32) << 8);
    w = (t[2] & 0x3fff) + 1, h = ((t[2] >> 14) & 0x3fff) + 1, pitch = ((t[4] >> 13) & 0x3fff) + 1;
    slices = (t[3] >> 28) == 13 ? 4 : 1; /* (the game's array, from its slice 0; else one texture: the
                                          * flat picture's, or a composed eye, compose.c) */
    idx = -1;
  }
  if (!src || w < 256 || w > 4096 || h < 256 || h > 4608 || pitch < w || pitch > 4096) {
    LOG("shot %u: no picture (buffer %d, %ux%u pitch %u%s)", req, idx, w, h, pitch, g_vr.in_vr ? ", VR mode" : "");
    return;
  }
  /* agent.shotslice=N: one slice of the array alone (at 2048 the four exceed the payload's limit) */
  if (g_vr.in_vr && g_opt_shotslice >= 1 && g_opt_shotslice <= 4 &&
      readable(src + (size_t)(g_opt_shotslice - 1) * pitch * ((h + 127) & ~127u) * 4, (size_t)pitch * ((h + 127) & ~127u) * 4)) {
    src += (size_t)(g_opt_shotslice - 1) * pitch * ((h + 127) & ~127u) * 4;
    slices = 1;
    LOG("shot %u: slice %u of the eye array alone (agent.shotslice)", req, g_opt_shotslice - 1);
  }
  /* (a 2D-tiled surface's rows go up to a multiple of 128, its macro tiles' height in this mode) */
  size_t size = (size_t)pitch * ((h + 127) & ~127u) * 4 * slices, cap = (size_t)64 << 20; /* (the payload's limit for a shot) */
  if (size > cap) {
    LOG("shot %u: %lu bytes, more than the copy's %lu", req, (u64)size, (u64)cap);
    return;
  }
  if (!copy)
    copy = vr2_dalloc_type(cap, 0x10000, 0);
  if (!copy) {
    LOG("shot %u: no memory for the copy", req);
    return;
  }
  memcpy(copy, src, size);
  int rc = agent_call(CMD_SHOT, (u64)copy, size, req, 0, 8000, 0, 0);
  LOG("shot %u: %s at %p, %ux%u pitch %u, %u slice%s, %lu bytes -> the payload (0x%x)", req,
      g_vr.in_vr ? "the frame's texture" : "the flat buffer", src, w, h, pitch, slices,
      slices > 1 ? "s" : "", (u64)size, rc);
}
#endif

/* ---- the menu out of VR (asked 2026-11-12): the settings / debug menu on the game's flat screens.
 * In VR the menu's shader (inset.c) draws it into the composed eyes; out of VR this thread writes its picture
 * into the game's main-port buffer the TV shows, each time a new one is shown (the flip count;
 * polled every millisecond while the menu is open): a shown buffer is finished and the game draws
 * into the other one until it flips again. Low in the middle of the screen, which the TV's scan
 * reaches about 8 ms into the frame, after the few ms the copy takes. The buffers' tiling is the
 * display 2D tiling of a PS4 Pro (16 pipes), B8G8R8A8 (tools/detile.c, measured on these screens);
 * drawn opaque where the menu's alpha is half or more (reading back the GPU's memory is too slow to
 * blend). */
static int writable(const void *p, u64 n);
static int readable(const void *p, u64 n);
static u64 flat_tile_addr(u32 x, u32 y, u32 pitch) {
#define BIT(v, n) (((v) >> (n)) & 1u)
  u32 pix = BIT(x, 0) | BIT(x, 1) << 1 | BIT(y, 0) << 2 | BIT(x, 2) << 3 | BIT(y, 1) << 4 | BIT(y, 2) << 5;
  u32 pipe = (BIT(x, 3) ^ BIT(y, 3) ^ BIT(x, 4)) | (BIT(x, 4) ^ BIT(y, 4)) << 1 | (BIT(x, 5) ^ BIT(y, 5)) << 2 |
             (BIT(x, 6) ^ BIT(y, 5)) << 3;
  u32 tx = x / 8 / 16, ty = y / 8;
  u32 bank = (BIT(ty, 3) ^ BIT(tx, 0)) | (BIT(ty, 3) ^ BIT(ty, 2) ^ BIT(tx, 1)) << 1 | (BIT(ty, 1) ^ BIT(tx, 2)) << 2 |
             BIT(ty, 0) << 3;
#undef BIT
  const u32 mt_pitch = 8 * 16, mt_height = 8 * 16;
  const u64 mt_bytes = (u64)mt_pitch * mt_height * 4 / 256;
  u64 mt_off = ((u64)(y / mt_height) * (pitch / mt_pitch) + x / mt_pitch) * mt_bytes;
  u64 total = mt_off + pix * 4;
  return (total >> 8) << 16 | (u64)(pipe | bank << 4) << 8 | (total & 0xff);
}
static void *flat_menu_thread(void *arg) {
  (void)arg;
  u64 last = ~0ull;
  u32 said = 0;
  while (g_shared.cmd != CMD_QUIT) {
    /* (the flip status through the agent's own import: the game does not call it in every mode) */
    const u8 *pic = (g_main_port >= 0 && !__atomic_load_n(&g_vr.in_vr, __ATOMIC_ACQUIRE) && sceVideoOutGetFlipStatus)
                        ? menu_picture() : 0;
    if (!pic) {
      last = ~0ull;
      sceKernelUsleep(20000);
      continue;
    }
    u64 st[8] = {0};
    if (sceVideoOutGetFlipStatus(g_main_port, st) != 0 || st[0] == last) {
      sceKernelUsleep(1000);
      continue;
    }
    last = st[0];
    s32 idx = (s32)(u32)st[7];
    u8 *buf = idx >= 0 && idx < 16 ? (u8 *)g_vr.vo_buffers[idx] : 0;
    u32 w = g_game_flip_w, h = g_game_flip_h, pitch = (w + 63) & ~63u;
    if (!buf || w < MENU_W + 16 || h < MENU_H + h / 12 + 16 || (pitch & 127) ||
        !writable(buf, (u64)pitch * ((h + 127) & ~127u) * 4)) {
      if (!said++)
        LOG("menu: out of VR, the TV's buffer not usable (%p, %ux%u): not drawn", (void *)buf, w, h);
      continue;
    }
    if (!said++)
      LOG("menu: out of VR, drawn into the TV's picture (buffer %d, %ux%u)", idx, w, h);
    u32 x0 = (w - MENU_W) / 2, y0 = h - MENU_H - h / 12;
    u64 t0 = sceKernelGetProcessTime();
    for (u32 y = 0; y < MENU_H; y++) {
      const u8 *s = pic + (u64)y * MENU_W * 4;
      for (u32 x = 0; x < MENU_W; x++, s += 4) {
        if (s[3] < 0x80)
          continue;
        u32 bgra = (u32)s[2] | (u32)s[1] << 8 | (u32)s[0] << 16 | 0xffu << 24;
        *(volatile u32 *)(buf + flat_tile_addr(x0 + x, y0 + y, pitch)) = bgra;
      }
    }
    static u32 drawn;
    if (drawn++ < 1)
      LOG("menu: out of VR, a picture drawn in %lu us", sceKernelGetProcessTime() - t0);
  }
  return 0;
}

static void *cvars_live_thread(void *arg) {
  (void)arg;
  static char text[sizeof g_cvars_text];
  /* the file as it is now: what changes after it is applied */
  if (agent_call(CMD_CVARS, (u64)g_cvars_seen, sizeof g_cvars_seen, 0, 0, 3000, 0, 0) != 0)
    memset(g_cvars_seen, 0, sizeof g_cvars_seen);
  g_cvars_seen[sizeof g_cvars_seen - 1] = 0;
  while (g_shared.cmd != CMD_QUIT) {
    for (int i = 0; i < 4 && g_shared.cmd != CMD_QUIT; i++) { /* (the report due within 0.5 s) */
      sceKernelUsleep(500000);
      health_report_due();
#ifndef RELEASE
      shot_tick();
      rec_tick(); /* (the flight recorder's snapshots and its marks: L3+R3) */
#endif
    }
    /* (agent.ticklog: each chore timed, the most of each in 10 s: at a picture every refresh a
     * frame came a refresh late every ~2.03 s, this loop's period, 2026-11-13) */
    static u64 chore_max[8];
    static u32 chore_round;
    u64 c0 = now_us();
#define CHORE(i, call) do { u64 a_ = now_us(); call; u64 d_ = now_us() - a_; if (d_ > chore_max[i]) chore_max[i] = d_; } while (0)
    CHORE(0, malfunction_keep());
    CHORE(1, fan_watch());
    CHORE(2, inv_give());
    CHORE(3, owner_keep());
    /* (the shops' upkeep, 11-15 ms of this thread every 2 s: not in VR, where no shop opens) */
    if (!g_vr.in_vr)
      CHORE(4, port_watch());
    CHORE(5, inv_dump());
    CHORE(6, menu_save()); /* (the settings menu's file: the menu's * lines and the others it saves) */
    memset(text, 0, sizeof text);
    int cvars_rc;
    CHORE(7, cvars_rc = agent_call(CMD_CVARS, (u64)text, sizeof text, 0, 0, 3000, 0, 0));
#undef CHORE
    (void)c0;
    if (g_opt_ticklog && ++chore_round % 5 == 0) {
      LOG("tick: the cvars thread's chores, the most each in 10 s (us): malfunction %lu, fan %lu, give %lu, owner %lu, "
          "port %lu, dump %lu, menu %lu, cvars.txt %lu", chore_max[0], chore_max[1], chore_max[2], chore_max[3],
          chore_max[4], chore_max[5], chore_max[6], chore_max[7]);
      memset(chore_max, 0, sizeof chore_max);
    }
    if (cvars_rc != 0)
      continue;
    text[sizeof text - 1] = 0;
    if (!memcmp(text, g_cvars_seen, sizeof text))
      continue;
    memcpy(g_cvars_seen, text, sizeof text);
    LOG("cvars: cvars.txt changed"
#ifndef RELEASE
        ": agent.tvhmd, agent.mirrorboth, agent.capture, agent.tickhold, agent.uihold, agent.sharpen(wide), agent.composefast and the automation's lines applied now"
#endif
        );
#ifndef RELEASE
    { /* agent.tvhmd and agent.mirrorboth (debug tools) follow the file: their defaults when
       * their line is gone */
      u32 tvhmd = 0, mb = 0, hold = 1 /* (on by default) */, uihold = 0, sharpen = 50 /* (the default) */, sharpenwide = 0, composefast = 3;
      for (const char *p = text; *p;) {
        const char *line = p;
        while (*p && *p != '\n') p++;
        u32 n = (u32)(p - line);
        if (*p) p++;
        if (n >= 13 && memcmp(line, "agent.tvhmd=", 12) == 0)
          tvhmd = line[12] != '0';
        else if (n >= 16 && memcmp(line, "agent.tickhold=", 15) == 0)
          hold = line[15] != '0'; /* (an A/B in one scene: a hold under way is still undone after its update) */
        else if (n >= 14 && memcmp(line, "agent.uihold=", 13) == 0)
          uihold = line[13] != '0'; /* (an A/B in one scene, while the payload's jump is in) */
        else if (n >= 18 && memcmp(line, "agent.mirrorboth=", 17) == 0)
          mb = line[17] >= '0' && line[17] <= '2' ? (u32)(line[17] - '0') : 0;
        else if (n >= 15 && memcmp(line, "agent.sharpen=", 14) == 0)
          sharpen = line_u32(line + 14, line + n); /* (compose.c logs a change, applied at the next frame) */
        else if (n >= 19 && memcmp(line, "agent.sharpenwide=", 18) == 0)
          sharpenwide = line_u32(line + 18, line + n);
        else if (n >= 19 && memcmp(line, "agent.composefast=", 18) == 0)
          composefast = line_u32(line + 18, line + n) & 3; /* (compose.c logs a change) */
        else if (n >= 13 && memcmp(line, "agent.stall=", 12) == 0) { /* (each new value once) */
          static u32 last_stall;
          u32 ms = line_u32(line + 12, line + n);
          if (ms != last_stall)
            last_stall = ms, g_stall_req = ms;
        }
        else if (n >= 14 && memcmp(line, "agent.closet=", 13) == 0)
          g_opt_closet = line_u32(line + 13, line + n); /* (live: a new value searched again) */
        else if (n >= 18 && memcmp(line, "agent.clockhours=", 17) == 0)
          g_opt_clockhours = line_u32(line + 17, line + n); /* (live: the sale list's week moved on) */
        else if (n >= 17 && memcmp(line, "agent.shotslice=", 16) == 0)
          g_opt_shotslice = line_u32(line + 16, line + n); /* (live: before the shot's line) */
        else if (n >= 15 && memcmp(line, "agent.capture=", 14) == 0) { /* (mirror.c: a TV capture each new value) */
          u32 c = line_u32(line + 14, line + n);
          if (c != g_opt_capture)
            LOG("cvars: agent.capture %u -> %u", g_opt_capture, c);
          g_opt_capture = c;
        }
        else if (n >= 12 && memcmp(line, "agent.shot=", 11) == 0)
          g_shot_req = line_u32(line + 11, line + n);
        else if (n >= 13 && memcmp(line, "agent.press=", 12) == 0) { /* N:HEX:MS */
          const char *d = line + 12, *e = line + n;
          u32 seq = 0, hex = 0, ms = 0;
          for (; d < e && *d >= '0' && *d <= '9'; d++)
            seq = seq * 10 + (u32)(*d - '0');
          for (d += d < e; d < e && *d != ':'; d++) {
            u32 c = *d >= '0' && *d <= '9' ? (u32)(*d - '0') : (*d | 0x20) >= 'a' && (*d | 0x20) <= 'f' ? (u32)((*d | 0x20) - 'a' + 10) : 16;
            if (c > 15)
              break;
            hex = hex << 4 | c;
          }
          for (d += d < e; d < e && *d >= '0' && *d <= '9'; d++)
            ms = ms * 10 + (u32)(*d - '0');
          if (seq != g_press_seq) {
            g_press_seq = seq;
            g_press_buttons = hex;
            g_press_until = now_us() + (u64)(ms > 5000 ? 5000 : ms) * 1000;
            LOG("drive: buttons 0x%x for %u ms (press %u)", hex, ms > 5000 ? 5000 : ms, seq);
          }
        }
      }
      if (mb != g_opt_mirrorboth) /* (live; 0 without the line) */
        LOG("cvars: agent.mirrorboth %u -> %u", g_opt_mirrorboth, mb);
      g_opt_mirrorboth = mb;
      if (tvhmd != g_opt_tvhmd)
        LOG("cvars: agent.tvhmd %u -> %u", g_opt_tvhmd, tvhmd);
      g_opt_tvhmd = tvhmd;
      if (hold != g_opt_tickhold)
        LOG("cvars: agent.tickhold %u -> %u", g_opt_tickhold, hold);
      g_opt_tickhold = hold;
      if (uihold != g_opt_uihold)
        LOG("cvars: agent.uihold %u -> %u", g_opt_uihold, uihold);
      g_opt_uihold = uihold;
      extern volatile u32 g_opt_sharpen, g_opt_sharpenwide; /* (live; 0 without the line: compose.c) */
      sharpen = sharpen > 100 ? 100 : sharpen;
      if (sharpen != g_cvars_sharpen) /* (only a change of the line: the settings menu's Sharpening kept) */
        g_opt_sharpen = g_cvars_sharpen = sharpen;
      g_opt_sharpenwide = sharpenwide > 100 ? 100 : sharpenwide;
      extern volatile u32 g_opt_composefast; /* (live; 3 without the line: compose.c) */
      g_opt_composefast = composefast;
    }
#endif
    memcpy(g_cvars_text, text, sizeof text);
    g_cvars_len = (u32)strlen(g_cvars_text);
  }
  return 0;
}

/* From the first frame handed over, once: the settings and the 10 s report's thread, and the
 * menu's thread for the TV's picture out of VR. (The game's GPU time is taken at the hand-over,
 * the frame's end of pipe: gpu_done.) */
void hooks_gpu_timing_start(void) {
  static volatile u32 started;
  if (!astro_hdr.game_base || !once(&started))
    return;
  u8 a2[64], t2[16];
  cvars_fetch();
  scePthreadAttrInit(a2);
  scePthreadAttrSetstacksize(a2, 64 * 1024);
  int rc2 = scePthreadCreate(t2, a2, cvars_live_thread, 0, "DOAX3VR2 cvars");
  u8 t3[16];
  if (scePthreadCreate(t3, a2, flat_menu_thread, 0, "DOAX3VR2 flat menu") != 0)
    LOG("menu: no out-of-VR menu (thread)");
  scePthreadAttrDestroy(a2);
  LOG("gpu: the settings and report thread started (0x%x); the game's GPU timed from its first "
      "submit to the hand-over", rc2);
}
static void gpu_done(void) {
  u64 fs = g_first_sub, t = now_us();
  g_first_sub = 0;
  /* At a picture every refresh (agent.ticklog): each frame handed over two or more refreshes after
   * the one before (a picture shown twice), with what it took: its GPU time (first submit to the
   * hand-over), the main loop's last CPU and its longest wall time lately. The first 200. */
  if (g_opt_ticklog && astro_hdr.frame_pace == 1 && g_late_d >= 2) {
    static u32 n;
    if (n++ < 200)
      LOG("late: frame at %lu ms, %u refreshes after the last; its GPU %lu us; the main loop's last CPU %lu us",
          (u64)(t / 1000), g_late_d, fs && t > fs ? (u64)(t - fs) : 0ul, g_tick_main.last_us);
  }
  if (fs && t > fs && t - fs < 100000) {
    u32 us = (u32)(t - fs);
    g_gpu_sum += us, g_gpu_n++;
    if (us > g_gpu_max)
      g_gpu_max = us;
    if (us > g_rec_gpu_max)
      g_rec_gpu_max = us;
  }
}
static s32 h_sceGnmSubmitCommandBuffers(u32 n, void **d, u32 *ds, void **c, u32 *cs) {
  frame_submitted();
  rtime_note(RT_SUBMIT, n, ds);
  return CALL_ORIG(s32(*)(u32, void **, u32 *, void **, u32 *), sceGnmSubmitCommandBuffers, n, d,
                   ds, c, cs);
}
static s32 h_sceGnmSubmitAndFlipCommandBuffers(u32 n, void **d, u32 *ds, void **c, u32 *cs,
                                               u32 vo, u32 buf, u32 mode, s64 arg) {
  frame_submitted();
  rtime_note(RT_SUBMIT, n, ds);
  return CALL_ORIG(s32(*)(u32, void **, u32 *, void **, u32 *, u32, u32, u32, s64),
                   sceGnmSubmitAndFlipCommandBuffers, n, d, ds, c, cs, vo, buf, mode, arg);
}

/* the health of the pace, every 10 s: what Hmd2's flips showed of what the game handed over. At a
 * frame every two refreshes: ~600 handed over, ~600 shown, ~599 shown again, 0 never shown. */
/* each frame the game hands over: how many refreshes since the one before (1, 2, 3 or more), and
 * how long after the last refresh event it came (the frame's work after its wait) */
static volatile u32 g_int[4], g_after_n, g_after_max;
static volatile u64 g_after_sum;
/* the head pose's prediction: how far ahead of the asking the game predicts (GetResult's
 * prediction_time - now: its own formula is GetTime + base + frame time x 2.5, 0x2591f38) and how
 * far ahead of the hand-over the frame's pose then still is (ReprojectionStart's ts->timestamp -
 * now); the display is a refresh or so after the hand-over */
static volatile s64 g_lead_ask_sum, g_lead_hand_sum, g_lead_ask_min, g_lead_ask_max, g_lead_hand_min,
    g_lead_hand_max;
static volatile u32 g_lead_ask_n, g_lead_hand_n;
static void lead_note(volatile s64 *sum, volatile s64 *mn, volatile s64 *mx, volatile u32 *n, s64 v) {
  if (!*n || v < *mn)
    *mn = v;
  if (!*n || v > *mx)
    *mx = v;
  *sum += v, (*n)++;
}
static void frame_timing(void) {
  static u64 last_count;
  u64 c = g_vr.vbl_count, t = now_us(), v = g_vr.vbl_time;
  g_late_d = 0;
  if (last_count) {
    u64 d = c - last_count;
    g_int[d >= 3 ? 3 : d]++;
    g_late_d = (u32)d;
  }
  last_count = c;
  if (v && t > v && t - v < 100000) {
    u32 a = (u32)(t - v);
    g_after_sum += a, g_after_n++;
    if (a > g_after_max)
      g_after_max = a;
  }
}

static void health_report(u32 handed) {
  LOG_DETAIL("hmd: the 10 s report at %lu us", (u64)now_us());
  LOG_DETAIL("hmd: frames %u in the same refresh, %u a refresh after the last, %u two after, %u three "
      "or more after; handed over %u.%u ms after their refresh on average, %u.%u at the most",
      g_int[0], g_int[1], g_int[2], g_int[3], g_after_n ? (u32)(g_after_sum / g_after_n / 1000) : 0,
      g_after_n ? (u32)(g_after_sum / g_after_n / 100 % 10) : 0, g_after_max / 1000,
      g_after_max / 100 % 10);
  g_int[0] = g_int[1] = g_int[2] = g_int[3] = 0, g_after_n = g_after_max = 0, g_after_sum = 0;
  if (g_lead_ask_n || g_lead_hand_n) {
    LOG_DETAIL("pose: predicted %ld.%ld ms ahead when the game asked (%ld.%ld to %ld.%ld), %ld.%ld ms ahead "
        "at the hand-over (%ld.%ld to %ld.%ld)",
        g_lead_ask_n ? g_lead_ask_sum / g_lead_ask_n / 1000 : 0,
        g_lead_ask_n ? (g_lead_ask_sum / g_lead_ask_n / 100) % 10 : 0, g_lead_ask_min / 1000,
        (g_lead_ask_min / 100) % 10, g_lead_ask_max / 1000, (g_lead_ask_max / 100) % 10,
        g_lead_hand_n ? g_lead_hand_sum / g_lead_hand_n / 1000 : 0,
        g_lead_hand_n ? (g_lead_hand_sum / g_lead_hand_n / 100) % 10 : 0, g_lead_hand_min / 1000,
        (g_lead_hand_min / 100) % 10, g_lead_hand_max / 1000, (g_lead_hand_max / 100) % 10);
    g_lead_ask_sum = g_lead_hand_sum = 0, g_lead_ask_n = g_lead_hand_n = 0;
  }
  vr_jitter_report();
  {
    static u32 said;
    u32 r = g_pool_retries;
    size_t flex = 0;
    if (sceKernelAvailableFlexibleMemorySize && sceKernelAvailableFlexibleMemorySize(&flex) == 0 &&
        flex < g_flex_min)
      g_flex_min = flex;
    if (r > said) {
      LOG_DETAIL("dmem: the game's CPU pool retried its 224 MB %u times in 10 s, refused (it copes)", r - said);
      said = r;
    }
    static u32 n;
    if (n++ % 6 == 0) /* (a minute) */
      LOG("dmem: flexible memory free %lu MB (the least this session %lu MB)", (u64)flex >> 20,
          g_flex_min >> 20);
  }
  {
    u32 late, gap;
    gen1_flip_stats(&late, &gap);
    LOG("hmd: the headset's flips: %u more than a refresh and a half after the one before (the longest "
        "gap %u.%u ms); the refresh clock lost them %u times; the game's refresh event found deleted "
        "(its own delete and add between) and added again %u times", late, gap / 1000, gap / 100 % 10,
        __atomic_exchange_n(&g_vbl_lost, 0, __ATOMIC_RELAXED),
        __atomic_exchange_n(&g_vbl_readd, 0, __ATOMIC_RELAXED));
  }
  if (g_held[1] || g_held[2] || g_held[3] || g_held[4]) {
    LOG_DETAIL("hmd: frames on the headset for 1 refresh %u, 2 %u, 3 %u, 4 or more %u", g_held[1],
        g_held[2], g_held[3], g_held[4]);
    g_held[1] = g_held[2] = g_held[3] = g_held[4] = 0;
  }
  u32 shown = g_shown, again = g_repeated, n = g_gpu_n;
  u64 sum = g_gpu_sum;
  u32 max = g_gpu_max;
  g_shown = g_repeated = 0;
  g_gpu_sum = 0, g_gpu_n = 0, g_gpu_max = 0;
  LOG("hmd: %u frames handed over, %u shown, %u shown again, %u never shown; the game's GPU "
      "%u.%u ms a frame on average, %u.%u at the most (%u frames)", handed, shown, again,
      __atomic_exchange_n(&g_never, 0, __ATOMIC_RELAXED), n ? (u32)(sum / n / 1000) : 0,
      n ? (u32)(sum / n / 100 % 10) : 0, max / 1000, max / 100 % 10, n);
}

/* The PS VR reprojection zeroes the label of every frame older than the newest once it is done
 * with it (libSceHmd sub_fff0); the game waits for that before reusing an eye buffer. One chain
 * for the 3D and the flat hand-overs: a game that goes from one to the other (Dead or Alive
 * Xtreme 3 waits without a timeout) has its last label of the other kind zeroed too. */
static u64 *g_last_label;
static void release_older_label(u64 *label) {
  if (g_last_label && g_last_label != label)
    *(volatile u64 *)g_last_label = 0;
  g_last_label = label;
}
/* the game enters or leaves its VR mode: the last frame's label too (its hand-over thread is
 * stopped, and the next VR session's first wait is for a zero label, without a timeout) */
static void labels_reset(void) {
  if (g_last_label)
    *(volatile u64 *)g_last_label = 0;
  g_last_label = 0;
}

static s32 h_sceHmdReprojectionStart(const struct hmd_reprojection_param *p,
                                     const struct hmd_tracker_state *ts, s64 flip_arg,
                                     s32 option) {
  rtime_note(RT_HANDOVER, 0, 0);
  tick_render();
  if (!p || !ts || !p->texture[0] || !p->texture[1])
    return HMD_ERR_PARAMETER_NULL;
  u32 n = __atomic_fetch_add(&g_vr.frames_in, 1, __ATOMIC_RELAXED);
  { /* how many frames the game draws: 60 a second at full speed (resolution.txt) */
    static u64 since;
    static u32 n0;
    u64 now = sceKernelGetProcessTime();
    if (!since)
      since = now, n0 = n;
    else if (now - since >= 10000000) {
      u32 d = n - n0;
      __atomic_store_n(&g_health_due, d ? d : 1, __ATOMIC_RELEASE); /* (reported off this thread) */
      since = now, n0 = n;
    }
  }
  if (n < 3 || (n % 3600) == 0) {
    LOG("hmd: ReprojectionStart #%u flip_arg %ld option %d flags 0x%x u40 0x%x u48 0x%lx u50 0x%x "
        "label %p",
        n, flip_arg, option, p->flags, p->unknown_40, p->unknown_48, p->unknown_50,
        p->frame_label);
    log_tsharp("L", p->texture[0]);
    log_tsharp("R", p->texture[1]);
    if (p->sampler) {
      const u32 *s = p->sampler;
      LOG("  S# %08x %08x %08x %08x", s[0], s[1], s[2], s[3]);
    }
    LOG("  uv L %f %f %f %f  R %f %f %f %f", p->uv[0].scale_x, p->uv[0].scale_y,
        p->uv[0].offset_x, p->uv[0].offset_y, p->uv[1].scale_x, p->uv[1].scale_y,
        p->uv[1].offset_x, p->uv[1].offset_y);
    LOG("  pose %f %f %f  q %f %f %f %f  ts %lu frame %u", ts->position[0], ts->position[1],
        ts->position[2], ts->orientation[0], ts->orientation[1], ts->orientation[2],
        ts->orientation[3], ts->timestamp, ts->user_frame_number);
  }
  frame_timing();
  if (ts->timestamp) {
    lead_note(&g_lead_hand_sum, &g_lead_hand_min, &g_lead_hand_max, &g_lead_hand_n,
              (s64)ts->timestamp - (s64)now_us());
  }
  gpu_done(); /* handed over at the frame's end of pipe: its GPU work is done */
  hooks_gpu_timing_start();
  vr_submit_frame(p, ts, flip_arg);
  vr2_frame_ready(); /* the hand-off thread (vr2.c) passes it to Hmd2 at once */
  release_older_label(p->frame_label);
  agent_tick();
  return 0;
}

/* Dead or Alive Xtreme 3's hand-over: each eye twice at one size, a wide view (the whole field of
 * view) and a near view (the middle, +-half the upward tangent, sharper there), blended by the PS VR
 * reprojection. 0x98 bytes: textures {left wide, left near, right wide, right near}, the sampler,
 * their four uv, two floats (the blend's inner and outer half tangent), the label, then as
 * sceHmdReprojectionStart's from its unknown_40. The wide views are the eyes here; the near views
 * with the near views composed into them (compose.c). */
struct hmd_widenear_param {
  const void *texture[4];
  const void *sampler;
  struct hmd_eye_uv uv[4];
  float near_inner, near_outer;
  u64 *frame_label;
  u32 unknown_78, padding0;
  u64 unknown_80;
  u32 unknown_88, padding1;
  u8 flags;
};
_Static_assert(offsetof(struct hmd_widenear_param, frame_label) == 0x70, "wide/near param ABI");
_Static_assert(offsetof(struct hmd_widenear_param, flags) == 0x90, "wide/near param ABI");

static s32 h_sceHmdReprojectionStartWideNear(const struct hmd_widenear_param *w,
                                             const struct hmd_tracker_state *ts, s64 flip_arg,
                                             s32 option) {
  if (!w || !ts || !w->texture[0] || !w->texture[2])
    return HMD_ERR_PARAMETER_NULL;
  __atomic_store_n(&g_tick_scene_us, now_us(), __ATOMIC_RELAXED); /* a scene's frame (agent.halftick) */
  static u32 n;
  if (n < 3) {
    n++;
    LOG("hmd: ReprojectionStartWideNear: near views within %f to %f (half tangent)", w->near_inner,
        w->near_outer);
    if (w->texture[1])
      log_tsharp("L near", w->texture[1]);
    if (w->texture[3])
      log_tsharp("R near", w->texture[3]);
    LOG("  uv near L %f %f %f %f  R %f %f %f %f", w->uv[1].scale_x, w->uv[1].scale_y,
        w->uv[1].offset_x, w->uv[1].offset_y, w->uv[3].scale_x, w->uv[3].scale_y, w->uv[3].offset_x,
        w->uv[3].offset_y);
  }
  struct hmd_reprojection_param p;
  memset(&p, 0, sizeof p);
  /* each eye made whole from its two views (compose.c), once PS VR2's GPU queue is up; until
   * then, and for a frame it cannot take, the wide views as they are */
  gaze_handover(w->frame_label); /* (the near centre this frame was drawn with, for compose_frame) */
  if (!compose_frame(w->texture, w->sampler, w->uv, w->near_inner, w->near_outer, &p)) {
    p.texture[0] = w->texture[0], p.texture[1] = w->texture[2];
    p.sampler = w->sampler;
    p.uv[0] = w->uv[0], p.uv[1] = w->uv[2];
  }
  p.frame_label = w->frame_label;
  p.unknown_40 = w->unknown_78, p.unknown_48 = w->unknown_80, p.unknown_50 = w->unknown_88;
  p.flags = w->flags;
  return h_sceHmdReprojectionStart(&p, ts, flip_arg, option);
}

static s32 h_sceHmdReprojectionStop(void) {
  LOG("hmd: ReprojectionStop");
  return 0;
}

/* The game's 2D picture in the headset (PS VR's call for screens drawn flat). The parameter, as
 * libSceHmd checks it: the texture (a GCN T#),
 * its sampler, the uv (scale and offset, 16 bytes), the frame label, a u32 at +0x28, and zeroes
 * from +0x30. Shown as a flat screen fixed before the seat (vr.c, vr2.c), the same picture to both
 * eyes; counted as a frame, so PS VR2 is brought up on these screens too. */
static s32 h_sceHmdReprojectionStart2dVr(const void *p, s64 flip_arg, s32 option) {
  static u32 n;
  const u8 *b = p;
  const u32 *t = b ? *(const u32 *const *)b : 0;
  if (!t)
    return HMD_ERR_PARAMETER_NULL;
  u32 k = __atomic_fetch_add(&g_vr.frames_in, 1, __ATOMIC_RELAXED);
  if (k < 2 || n < 6 || (k % 3600) == 0) {
    LOG("hmd: ReprojectionStart2dVr #%u (%p, flip_arg %ld, option %d) u28 0x%x label %p", k, p,
        flip_arg, option, *(const u32 *)(b + 0x28), *(void *const *)(b + 0x20));
    log_tsharp("2D", t);
    const float *uv = (const float *)(b + 0x10);
    LOG("  uv %f %f %f %f", uv[0], uv[1], uv[2], uv[3]);
    n++;
  }
  vr_submit_flat(t, *(const u32 *const *)(b + 8), (const struct hmd_eye_uv *)(b + 0x10),
                 *(u64 *const *)(b + 0x20), flip_arg);
  vr2_frame_ready();
  release_older_label(*(u64 *const *)(b + 0x20));
  agent_tick();
  return 0;
}

static s32 h_sceHmdReprojectionUnsetDisplayBuffers(void) {
  LOG("hmd: ReprojectionUnsetDisplayBuffers");
  labels_reset();
  g_vr.vr_mode_vo = 0;
  __atomic_store_n(&g_vr.in_vr, 0, __ATOMIC_RELEASE); /* (the game's flat mode: its port's own flips) */
  hooks_tv_rate_sync(); /* (the game's flip rate back on its port, if it set one in VR mode) */
  return 0;
}

/* 120 Hz: the PS VR reprojection's end-of-refresh event, which the game paces on. Once PS VR2
 * shows the game, it is raised on each refresh of the headset (Hmd2 flips it every one); before
 * that, and if a refresh is late, on a clock at the headset's 119.88 Hz (89.91 in its 90 Hz
 * mode). A clock of its own would drift from the headset's: every few seconds a frame would come
 * too many or too few. */
static void *vblank_thread(void *arg) {
  (void)arg;
  /* Made from the game's main thread, it would run where that does (core 0 above all) and wake
   * every fraction of a millisecond there: onto the cores and priority of the agent's other
   * threads and the game's own PS VR thread (Hmd::ReproThread: 4 to 6, 256). */
  {
    void *self = scePthreadSelf();
    int a = scePthreadSetaffinity ? scePthreadSetaffinity(self, 0x70) : -1;
    int pr = scePthreadSetprio ? scePthreadSetprio(self, 256) : -1;
    LOG("vblank: on cores 0x70 (0x%x), priority 256 (0x%x)", a, pr);
    void *eq = 0;
    int c = sceKernelCreateEqueue ? sceKernelCreateEqueue(&eq, "DOAX3VR2 vblank") : -1;
    int e = c == 0 ? sceKernelAddUserEventEdge(eq, WAKE_FLIP) : -1;
    if (c == 0 && e == 0)
      g_wake_eq = eq;
    LOG("vblank: woken by Hmd2's flips (0x%x, 0x%x)", c, e);
  }
  u64 next_ns = now_us() * 1000, last_sync = 0;
  u32 seen = gen1_refreshes(), locked = 0, switches = 0;
  while (g_shared.cmd != 1) {
    /* a refresh in us: the headset's 120 Hz (119.88), or 90 (89.91) in its 90 Hz mode */
    u32 hz = g_refresh_hz ? g_refresh_hz : astro_hdr.refresh_hz == 90 ? 90 : 120;
    u32 period = 1001000 / hz;
    /* a refresh is due a period after the last: sleep most of that, then watch for it */
    u32 doze = period - period / 6;
    /* the clock read once: read again, it could have passed the moment (the sleep then wrapping
     * to 71 minutes, no refresh events, the game frozen waiting for one) */
    u64 now0 = now_us();
    if (locked && last_sync + doze > now0)
      vbl_sleep(last_sync + doze, 1);
    u64 t0 = now_us();
    int synced = 0;
    /* Watched for until 4 ms past the moment it is due (a period after the last one taken), not
     * for 6 ms from here: the doze above ends at once when the flip's wake event is still pending
     * (the flip was found by the poll below, its event not yet taken), and 6 ms from then ended
     * 1.3 ms before the next flip was due. The clock took over for a refresh, the flip came all the
     * same and was counted as another: the game's flip count ran 0.35 % fast, 601-602 frames each
     * 10 s for the headset's 599.4, and 2-5 of them were shown for one refresh (2026-11-11: the
     * headset's flips never more than 8.4 ms apart, the clock lost them 5-16 times a window). */
    u64 watch_end = locked ? last_sync + period + 4000 : t0 + 1000;
    for (u64 t = t0; t < watch_end; t = now_us()) {
      u32 r = gen1_refreshes();
      if (r != seen) {
        seen = r, synced = 1;
        break;
      }
      vbl_sleep(t + (locked ? 200 : 500), 1);
    }
    if (synced != (int)locked) {
      locked = synced;
      if (!locked) {
        g_rec_clockfb++;
        g_vbl_lost++;
        static u32 said;
        u64 tn = now_us();
        if (said++ < 24)
          LOG("vblank: no refresh from the headset %lu us after the last one taken (its last flip "
              "%lu us ago, %u refreshes seen)", tn - last_sync, tn - gen1_last_flip(), seen);
      }
      if (switches++ < 16)
        LOG("vblank: %s (%u Hz)", locked ? "on the headset's refreshes" : "on the clock", hz);
    }
    if (synced) {
      last_sync = now_us();
      next_ns = last_sync * 1000;
    } else {
      next_ns += 1001000000ull / hz; /* ns */
      u64 t = now_us(), next = next_ns / 1000;
      if (next > t)
        vbl_sleep(next, 0);
      else if (t - next > 100000)
        next_ns = t * 1000;
    }
    u32 k = __atomic_fetch_add(&g_vr.vblanks, 1, __ATOMIC_RELAXED);
    if (k % 7200 == 7199)
      LOG_DETAIL("status: %u refreshes, %u frames from the game, %u tracker reads", k + 1,
          g_vr.frames_in, g_vr.tracker_reads);
    if (g_stall_req) { /* agent.stall=MS (debug): the game's pace held MS, as a system screen does */
      u32 ms = __atomic_exchange_n(&g_stall_req, 0, __ATOMIC_ACQ_REL);
      LOG("stall: the game's refresh events held %u ms (agent.stall)", ms);
      sceKernelUsleep(ms > 10000 ? 10000000 : ms * 1000);
      next_ns = now_us() * 1000;
    }
    raise_vblank(k, 1, 1);
  }
  /* the injector's kill waits up to 3 s for the VR thread's STOP_DONE (vr2.c): with PS VR2 never
   * started (the game closed in its flat mode, or an observing run) there is no such thread, and
   * nothing to turn off: answered here */
  if (g_vr.vr2_started != VR2_BUILD)
    __atomic_store_n(&g_shared.cmd_arg[3], STOP_DONE, __ATOMIC_RELEASE);
  return 0;
}

/* the vblank thread's sleep until `until` (us), ended early by Hmd2's flip where `on_flip`
 * (returns 1 then) */
static int vbl_sleep(u64 until, int on_flip) {
  for (;;) {
    u64 t = now_us();
    if (t >= until)
      return 0;
    void *eq = g_wake_eq;
    if (!eq) {
      sceKernelUsleep((u32)(until - t));
      return 0;
    }
    u8 ev[2][32];
    int out = 0;
    u32 timo = (u32)(until - t);
    int rc = sceKernelWaitEqueue(eq, ev, 2, &out, &timo);
    static u32 spurious; /* returns with nothing, in a row */
    if (out <= 0) {
      if (until - t > 200 && now_us() - t < 50 && ++spurious >= 3) {
        /* neither a flip nor the timeout: the wait doesn't work here, sleep as before */
        LOG("vblank: the wait returned 0x%x at once: sleeping instead", rc);
        g_wake_eq = 0;
      }
      continue;
    }
    spurious = 0;
    int flipped = 0;
    for (int i = 0; i < out && i < 2; i++)
      flipped |= *(u64 *)ev[i] == WAKE_FLIP;
    if (flipped && on_flip)
      return 1;
  }
}

/* a refresh of the game's PS VR; `shown`: one of the headset's, where the newest frame is shown;
 * `counted`: a refresh in the flip status */
static void raise_vblank(u32 k, int shown, int counted) {
  if (g_vr.vbl_eq || g_vr.start_pace) {
    /* As the PS VR reprojection at half rate: a frame is shown for two refreshes, then the
     * newest one the game handed over; at 90 fps for one of the headset's. */
    static u32 shown_for;
    u32 w = g_vr.frame_w;
    u32 pace = astro_hdr.frame_pace == 1 ? 1 : 2; /* (the injector gives 2: the game's 60 fps) */
    if (shown && ++shown_for >= pace && w &&
        g_vr.frame[w & 3].flip_arg != g_vr.vbl_flip_arg) {
      g_vr.vbl_flip_arg = g_vr.frame[w & 3].flip_arg;
      shown_for = 0;
    }
    if (counted) {
      u32 lo, hi;
      __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
      g_vr.vbl_tsc = ((u64)hi << 32) | lo;
      g_vr.vbl_time = now_us();
      __atomic_fetch_add(&g_vr.vbl_count, 1, __ATOMIC_RELAXED); /* also from the game's threads */
    }
    if (g_vr.vbl_eq)
      sceKernelTriggerUserEvent(g_vr.vbl_eq, VBL_USER_ID, 0);
  }
  if (g_vr.end_eq) {
    int rc = sceKernelTriggerUserEvent(g_vr.end_eq, g_vr.end_id, 0);
    if (rc != 0) {
      /* The real library adds the user event to the game's queue itself. */
      int a = sceKernelAddUserEvent(g_vr.end_eq, g_vr.end_id);
      int rc2 = sceKernelTriggerUserEvent(g_vr.end_eq, g_vr.end_id, 0);
      g_vbl_readd++; /* (counted for the 10 s report: only the first 4 are logged) */
      static u32 n;
      LOG_FIRST(n, 4, "vblank: trigger -> 0x%x, AddUserEvent -> 0x%x, trigger -> 0x%x", rc, a, rc2);
    } else if (k < 2 && shown) {
      LOG("vblank: trigger ok");
    }
  }
}

/* ---------------------------------------------------------------- main port at 120 Hz */
/* The game paces its renderer on vblank events of its main VideoOut port (on the queue it gave
 * the PS VR reprojection) and on that port's flip status: with PS VR the reprojection flipped
 * it at 120 Hz with each frame's flip_arg. Here it is the 60 Hz TV and nothing flips it, which
 * holds the game to 30 fps. Its vblank event there is replaced by a user event the vblank
 * thread raises every 120 Hz refresh, handed to the game as a VideoOut event, and the port's
 * flip status reports those refreshes. */
#define EVFILT_USER_ (-11)
#define EVFILT_VIDEO_OUT_ (-13)

static s32 h_sceVideoOutAddVblankEvent(void *eq, s32 vo, void *udata) {
  /* A game adds it on the queue it gave the PS VR reprojection's end event, or on a queue of its
   * own. The first vblank event the game registers, on the port it opened for its renderer, is
   * the one: it becomes ours. */
  if (!g_vr.vbl_eq && eq &&
      (g_main_port < 0 || vo == g_main_port || (g_vr.end_eq && eq == g_vr.end_eq))) {
    g_vr.vbl_eq = eq;
    g_vr.main_vo = vo;
    s32 rc = sceKernelAddUserEventEdge(eq, VBL_USER_ID); /* auto-clearing */
    LOG("videoout: AddVblankEvent(eq %p, port 0x%x, udata %p)%s -> 120 Hz user event (0x%x)", eq,
        vo, udata, g_vr.end_eq && eq == g_vr.end_eq ? " on the VR queue" : "", rc);
    agent_tick();
    return 0;
  }
  LOG("videoout: AddVblankEvent(eq %p, port 0x%x, udata %p): another port, passed through", eq, vo,
      udata);
  return CALL_ORIG(s32(*)(void *, s32, void *), sceVideoOutAddVblankEvent, eq, vo, udata);
}

/* the game reads the count of its vblank event: the data field as libSceVideoOut packs it (the
 * count in bits 12-15, the event's data above) */
static s32 h_sceVideoOutGetEventCount(const u8 *ev) {
  s32 rc = CALL_ORIG(s32(*)(const u8 *), sceVideoOutGetEventCount, ev);
  static u32 n;
  LOG_FIRST(n, 2, "videoout: GetEventCount(ident %lu, data 0x%lx) -> %d", ev ? *(const u64 *)ev : 0,
            ev ? *(const u64 *)(ev + 16) : 0, rc);
  return rc;
}

/* The main port's flip rate. In VR mode the game sets its main port to 30 flips a second now
 * and then (SetFlipRate 1, later 0: in the replay too) though it flips nothing there in VR mode:
 * the port is the TV mirror's, and at rate 1 the mirror's flips queued two deep, the TV showed
 * every other one and the mirror waited for its buffers (logs/perf/rp2.log: 99 waits in the one
 * window with the toggle, none in the others; the user saw a dropped frame and tearing on the
 * TV, logs/research/tv/TEARING.md). So in VR mode the port stays at 0 (60 a second); the game's
 * rate is kept and given to the port once out of VR mode (hooks_tv_rate_sync, from the mirror's
 * loop). The game's pace in VR mode is the agent's vblank events, not this port's flips. */
static volatile s32 g_rate_game = -1, g_rate_port = -1; /* (-1: never set) */
void hooks_tv_rate_sync(void) {
  if (g_main_port < 0 || !orig_sceVideoOutSetFlipRate)
    return;
  s32 want = __atomic_load_n(&g_vr.in_vr, __ATOMIC_ACQUIRE) ? 0 : g_rate_game;
  if (want < 0 || __atomic_exchange_n(&g_rate_port, want, __ATOMIC_ACQ_REL) == want)
    return;
  s32 rc = CALL_ORIG(s32(*)(s32, s32), sceVideoOutSetFlipRate, g_main_port, want);
  LOG("videoout: the main port's flip rate %d (%s) -> 0x%x", want,
      __atomic_load_n(&g_vr.in_vr, __ATOMIC_ACQUIRE) ? "VR mode: the TV mirror's 60" : "the game's", rc);
}

static s32 h_sceVideoOutSetFlipRate(s32 h, s32 rate) {
  if (h >= 0 && h == g_main_port) {
    g_rate_game = rate;
    if (rate != 0 && __atomic_load_n(&g_vr.in_vr, __ATOMIC_ACQUIRE)) {
      LOG("videoout: SetFlipRate(0x%x, %d): the port kept at 0 (VR mode: the TV mirror's)", h, rate);
      hooks_tv_rate_sync();
      return 0;
    }
  }
  s32 rc = CALL_ORIG(s32(*)(s32, s32), sceVideoOutSetFlipRate, h, rate);
  if (h >= 0 && h == g_main_port && rc == 0)
    __atomic_store_n(&g_rate_port, rate, __ATOMIC_RELEASE);
  LOG("videoout: SetFlipRate(0x%x, %d) -> 0x%x", h, rate, rc);
  return rc;
}

static s32 h_sceVideoOutUnregisterBuffers(s32 h, s32 set) {
  s32 rc = CALL_ORIG(s32(*)(s32, s32), sceVideoOutUnregisterBuffers, h, set);
  LOG("videoout: UnregisterBuffers(0x%x, set %d) -> 0x%x", h, set, rc);
  return rc;
}

/* the game waits for the TV's vblank only on its way out of VR mode (flips pending) */
static s32 h_sceVideoOutWaitVblank(s32 h) {
  static u32 n;
  LOG_FIRST(n, 4, "videoout: WaitVblank(0x%x)", h);
  return CALL_ORIG(s32(*)(s32), sceVideoOutWaitVblank, h);
}

/* The game's waits on its VR queue: our user event becomes the VideoOut vblank event it expects.
 * The first waits also say what else the game waits on there (its timers, its GPU events): the
 * map of its frame loop, for later. */
static s32 h_sceKernelWaitEqueue(void *eq, u8 *ev, s32 num, s32 *out, u32 *timo) {
  rtime_note(RT_WAIT, 0, 0);
  int vbl = eq && eq == g_vr.vbl_eq;
  s32 rc = CALL_ORIG(s32(*)(void *, u8 *, s32, s32 *, u32 *), sceKernelWaitEqueue, eq, ev, num, out,
                     timo);
  if (rc == 0 && vbl && ev && out)
    for (s32 i = 0; i < *out; i++) {
      u8 *e = ev + (u64)i * 32; /* struct kevent: ident, filter, flags, fflags, data, udata */
      u64 ident = *(u64 *)e;
      s16 filter = *(s16 *)(e + 8);
      if (ident == VBL_USER_ID && filter == EVFILT_USER_) {
        *(u64 *)e = 1; /* SCE_VIDEO_OUT_EVENT_VBLANK */
        *(s16 *)(e + 8) = EVFILT_VIDEO_OUT_;
        /* data as libSceVideoOut packs it: the event's data (the refresh count) above bit 16,
         * the number of events in bits 12-15 */
        *(u64 *)(e + 16) = (g_vr.vbl_count << 16) | (1u << 12);
      } else {
        static u32 n;
        LOG_FIRST(n, 12, "vrqueue: the game's VR queue gave event ident 0x%lx filter %d data 0x%lx",
                  ident, filter, *(u64 *)(e + 16));
      }
    }
  return rc;
}

/* The real flip state of a port, not the pace the game is told (below): the flips submitted and not
 * yet shown, and the buffer on screen. For the TV mirror, which must not write into a buffer the TV
 * still shows or is about to show. -1 when it cannot be read. */
int hooks_flip_state(s32 vo, s32 *pending, s32 *shown) {
  u64 st[8] = {0};
  if (!orig_sceVideoOutGetFlipStatus ||
      CALL_ORIG(s32(*)(s32, u64 *), sceVideoOutGetFlipStatus, vo, st) != 0)
    return -1;
  *pending = (s32)(st[6] >> 32); /* flipPendingNum */
  *shown = (s32)(u32)st[7];      /* currentBuffer */
  return 0;
}

/* Out of VR: the buffer the TV shows now, once it changes (the flip count), to the headset as the
 * flat screen (vr.c vr_flat_flip). Polled by vr2.c's hand-off thread about every millisecond. Sent
 * at the game's sceVideoOutSubmitFlip instead, the buffer was still being drawn (its frame of two
 * flips before showing) and the picture lagged (the user, 2026-11-12); shown, it is finished. */
int hooks_flat_poll(void) {
  static u64 last_count = ~0ull;
  if (g_main_port < 0 || __atomic_load_n(&g_vr.in_vr, __ATOMIC_ACQUIRE) || !orig_sceVideoOutGetFlipStatus)
    return 0;
  u64 st[8] = {0};
  if (CALL_ORIG(s32(*)(s32, u64 *), sceVideoOutGetFlipStatus, g_main_port, st) != 0 || st[0] == last_count)
    return 0;
  static u64 win_at, win_count0;
  static u32 sent, polls_late;
  u64 now = sceKernelGetProcessTime();
  if (last_count != ~0ull && st[0] > last_count + 1)
    polls_late += (u32)(st[0] - last_count - 1); /* (flips the poll never saw: not sent) */
  last_count = st[0];
  int ok = vr_flat_flip((s32)(u32)st[7], (s64)st[3]);
  sent += (u32)ok;
  static u32 samples;
  if (samples++ < 16)
    LOG("vr: out of VR poll: count %lu, buffer shown %d, flips pending %d, arg %ld -> %s", st[0],
        (s32)(u32)st[7], (s32)(st[6] >> 32), (s64)st[3], ok ? "sent" : "not sent");
  if (!win_at)
    win_at = now, win_count0 = st[0];
  else if (now - win_at >= 5000000) {
    LOG("vr: out of VR, in %lu ms: %u pictures to the headset, %lu flips of the TV, %u flips missed",
        (now - win_at) / 1000, sent, st[0] - win_count0, polls_late);
    win_at = now, win_count0 = st[0], sent = 0, polls_late = 0;
  }
  return ok;
}

/* SceVideoOutFlipStatus: count, processTime, tsc, flipArg, submitTsc, reserved, gcQueueNum,
 * flipPendingNum, currentBuffer */
static s32 h_sceVideoOutGetFlipStatus(s32 vo, u64 *st) {
  s32 rc = CALL_ORIG(s32(*)(s32, u64 *), sceVideoOutGetFlipStatus, vo, st);
  /* the pace is ours (the count, its time and tsc, the flip argument); the rest (the flips
   * pending on the real port, the buffer shown) stays real: a game that flips the port and waits
   * for that flip to be done before it changes the port's buffers must see its own flips */
  if (st && ((g_vr.vbl_eq && vo == g_vr.main_vo) ||
             (g_vr.start_pace && g_vr.in_vr && vo == g_vr.display_vo))) {
    if (rc != 0)
      memset(st, 0, 0x40);
    /* a picture every refresh (the header's frame_pace 1, agent.fps120): the game waits for the
     * count to move on by 2 before each frame, so it is told 2 a refresh (tick_frame keeps its
     * ticks at 60 a second) */
    st[0] = astro_hdr.frame_pace == 1 ? g_vr.vbl_count * 2 : g_vr.vbl_count;
    st[1] = g_vr.vbl_time;
    st[2] = g_vr.vbl_tsc;
    st[3] = (u64)g_vr.vbl_flip_arg;
    st[4] = g_vr.vbl_tsc;
    return 0;
  }
  return rc;
}

/* ---------------------------------------------------------------- pad */
/* The game copies only acceleration and angular velocity from each sample; libScePad's own
 * fused orientation (+0x0c, tilt-corrected) becomes the tracked controller's orientation. */
/* While a system screen has the controller (PS VR2's safety notice, its play-area setup, the PS
 * menu), libScePad marks the game's readings intercepted (buttons bit 31): when that was last
 * seen, from the real reading before a test script replaces it. hand.c waits for it to clear. */
#define PAD_INTERCEPTED 0x80000000u
volatile u64 g_pad_intercepted_at;
static void note_intercepted(const u8 *data) {
  static u32 was; /* the game's pad thread and the hand thread both read it: one of them logs */
  u32 now = (*(const u32 *)data & PAD_INTERCEPTED) != 0;
  if (now)
    __atomic_store_n(&g_pad_intercepted_at, now_us(), __ATOMIC_RELEASE);
  if (__atomic_exchange_n(&was, now, __ATOMIC_RELAXED) != now) {
    static u32 n;
    if (__atomic_fetch_add(&n, 1, __ATOMIC_RELAXED) < 16u)
      LOG_DETAIL("pad: %s (%u.%03u s)", now ? "a system screen has the controller" : "the controller is the game's again",
          (u32)(now_us() / 1000000), (u32)(now_us() / 1000 % 1000));
  }
}

/* The game's controller read now, beside the game (scePadReadState takes no samples from it),
 * for when the game stops reading while a system screen is up. */
static volatile s32 g_pad_player = -1;
int hooks_pad_held(void) {
  s32 h = __atomic_load_n(&g_pad_player, __ATOMIC_ACQUIRE);
  u8 d[0x100] __attribute__((aligned(16))); /* ScePadData: 0x78 bytes */
  if (h < 0 || !orig_scePadReadState || CALL_ORIG(s32(*)(s32, u8 *), scePadReadState, h, d) != 0)
    return -1;
  note_intercepted(d);
  return (*(const u32 *)d & PAD_INTERCEPTED) != 0;
}

/* The game's controller: the test mode's recording or script (pads.c), then the view gestures
 * and the orientation for the tracked controller (vr.c). */
static s32 h_scePadRead(s32 handle, u8 *data, s32 num) {
  rtime_note(RT_PAD, 0, 0);
  static s32 player = -1; /* the first controller the game reads is the one it tracks */
  s32 n = CALL_ORIG(s32(*)(s32, u8 *, s32), scePadRead, handle, data, num);
  for (s32 i = 0; data && (handle == player || player < 0) && i < n; i++)
    note_intercepted(data + (u64)i * 0x78);
  if (handle == player || (player < 0 && n <= 0)) /* the player's, or none answering yet */
    n = pads_test_read(handle, data, num, n);     /* test.txt: recorded, or the script's */
  if (player < 0 && n > 0) {
    player = handle;
    s32 none = -1; /* hooks_pad_held's handle, unless scePadReadState gave it one first */
    __atomic_compare_exchange_n(&g_pad_player, &none, handle, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
  }
  for (s32 i = 0; data && handle == player && i < n; i++)
    vr_pad_read(data + (u64)i * 0x78);
  return n;
}

/* The controller read with scePadReadState (one sample): the same path. */
static s32 h_scePadReadState(s32 handle, u8 *data) {
  static s32 player = -1;
  s32 rc = CALL_ORIG(s32(*)(s32, u8 *), scePadReadState, handle, data);
  if (data && rc == 0 && (handle == player || player < 0))
    note_intercepted(data);
  if (handle == player || player < 0) {
    s32 n = pads_test_read(handle, data, 1, rc == 0 ? 1 : 0); /* test.txt: recorded, or the script's */
    if (n > 0)
      rc = 0;
  }
#ifndef RELEASE
  if (data && now_us() < g_press_until) { /* agent.press: held in what the game reads */
    if (rc != 0 || !data[0x4c]) { /* (no controller answering, or one switched off (connected 0, +0x4c):
                                   * a resting one made up) */
      memset(data, 0, 0x78);
      data[4] = data[5] = data[6] = data[7] = 0x80, data[0x4c] = 1;
      rc = 0;
    }
    u32 b = g_press_buttons;
    *(u32 *)data |= b & 0x00ffffffu;
    /* the left stick, as drive.py's ls* names: bits 24-27 left, right, up, down (pushed fully) */
    if (b & 0x01000000u) data[4] = 0x00;
    if (b & 0x02000000u) data[4] = 0xff;
    if (b & 0x04000000u) data[5] = 0x00;
    if (b & 0x08000000u) data[5] = 0xff;
    /* the right stick: bits 29-30 left, right (drive.py's rsleft, rsright) */
    if (b & 0x20000000u) data[6] = 0x00;
    if (b & 0x40000000u) data[6] = 0xff;
    /* drive.py's "shake": the controller shaken up and down (ScePadData's acceleration, +0x1c x,
     * +0x20 y, +0x24 z, in g), 8 g up and down, flipped on every read: what Scarlet's Jet
     * Black Fan thread (0x35a330) counts as swings */
    if (b & 0x10000000u) {
      static u32 flip;
      float y = (flip++ & 1) ? 8.0f : -8.0f; /* (the thread wants an upward swing every other
                                               * sample: its window is 2; so each read flips) */
      memcpy(data + 0x20, &y, 4);
    }
  }
#endif
  if (player < 0 && rc == 0) {
    player = handle;
    __atomic_store_n(&g_pad_player, handle, __ATOMIC_RELEASE);
    /* The tracked controller turns with libScePad's fused orientation (+0x0c), which the pad only
     * computes with its motion sensors on: switched on here, whether or not the game does. */
    s32 m = scePadSetMotionSensorState ? scePadSetMotionSensorState(handle, 1) : -1;
    s32 t = scePadSetTiltCorrectionState ? scePadSetTiltCorrectionState(handle, 1) : -1;
    LOG("pad: controller 0x%x: motion sensors on (0x%x), tilt correction on (0x%x)", handle, m, t);
  }
  if (data && handle == player && rc == 0) {
    static u64 next;
    u64 t = now_us();
    if (t >= next) { /* the orientation the game's controller is turned by, every 2 s */
      next = t + 2000000;
      const float *q = (const float *)(data + 0x0c);
      LOG_DETAIL("pad: orientation %f %f %f %f, turning %f %f %f, connected %u", q[0], q[1], q[2], q[3],
          ((const float *)(data + 0x28))[0], ((const float *)(data + 0x28))[1],
          ((const float *)(data + 0x28))[2], data[0x4c]);
    }
    menu_pad(data); /* the menu: its buttons, and nothing to the game while it is open */
    /* A picture every refresh (frame_pace 1; after the menu, which
     * takes every read as it is): the game reads the pad once a loop, in its update,
     * and takes a press from the change between two reads; a press first read in a loop without a
     * tick was lost to the game's logic (2026-11-13: the route's "down" not taken, the wrong menu
     * entered). In such a loop the buttons, sticks and triggers of the read before are given again,
     * so every change is first seen in a loop that runs a tick. */
    if (astro_hdr.frame_pace == 1) {
      static u8 held[10];
      static u32 have;
      if (g_tick_none && have)
        memcpy(data, held, sizeof held);
      else
        memcpy(held, data, sizeof held), have = 1;
    }
    { /* (a bought set unpacked on the game's own thread, which reads the controller: every 2 s) */
      static u64 next_unpack;
      u64 tu = now_us();
      /* (not in VR: a set is bought in the Owner Shop, out of VR. In VR at a picture every refresh
       * the frames handed over 3-4 refreshes late came exactly every 2 s, 2026-11-13: timed here) */
      if (tu >= next_unpack) {
        next_unpack = tu + 2000000;
        if (!g_vr.in_vr) {
          set_unpack();
        } else if (g_opt_ticklog) {
          static u32 said;
          if (!said++)
            LOG("tick: the set unpack skipped in VR");
        }
        if (g_opt_ticklog) {
          static u64 worst, n;
          u64 d = now_us() - tu;
          if (d > worst)
            worst = d;
          if (++n % 5 == 0)
            LOG("tick: the 2 s pad-thread chores took %lu us at the most (the last 10 s)", worst), worst = 0;
        }
      }
    }
    vr_pad_read(data);
#ifndef RELEASE
    rec_pad(handle, data);
#endif
  }
  return rc;
}

/* ---------------------------------------------------------------- system service */
/* With PS VR2 in VR mode the system reports this PS4 title as running in the background
 * (status byte 5), and the game pauses on that (its VR loop then hands over a black eye
 * texture). The system-UI-overlay flag (byte 4) still pauses it, so the PS menu works. */
static s32 h_sceSystemServiceGetStatus(u8 *st) {
  s32 rc = CALL_ORIG(s32(*)(u8 *), sceSystemServiceGetStatus, st);
  if (rc == 0 && st && g_vr.vr2_started)
    st[5] = 0;
  /* With the headset off the player's head, a console puts its own screen over the game
   * (isSystemUiOverlaid, byte 4), and that is what the game pauses for. */
  if (rc == 0 && st && g_vr.hmd_off)
    st[4] = 1;
  /* the game only asks for events while the status counts some (eventNum, +0) */
  if (rc == 0 && st && g_opt_resetevent && vr_reset_event(0))
    *(s32 *)st += 1;
  else if (rc == 0 && !g_opt_resetevent && vr_reset_event(1)) { /* (owed, not sent) */
    static u32 said;
    if (said++ < 10)
      LOG("system: ResetVrPosition not sent to the game (agent.resetevent=0)");
  }
  return rc;
}

/* A view reset (OPTIONS held, the menu's Reset view) re-seats the head (vr.c seat_reset), and the game
 * is sent ResetVrPosition as a console does (its origin follows the head). Between 2026-11-14 morning
 * and evening it was kept from the game (agent.resetevent=0) for the scenery lost after resets; that
 * was the game's own "GPU too slow" fallback (patch_gpufallback, logs/research/scale/SCENERY.md):
 * with it fixed, the user's recording with its resets keeps the scenery with the event sent
 * (logs/perf/rp7). agent.resetevent=0 (debug) keeps it, and the system's copy, from the game. */
#define SYSTEM_EVENT_RESET_VR_POSITION 0x1000000a
volatile u32 g_sysev_type, g_sysev_kept; /* the last system event (vr.c's origin watch) */
volatile u32 g_resume_n; /* the system's resume events seen (vr2.h) */
volatile u64 g_sysev_t;
static s32 h_sceSystemServiceReceiveEvent(u8 *ev) {
  if (ev && g_opt_resetevent && vr_reset_event(1)) {
    memset(ev, 0, 4 + 8192); /* SceSystemServiceEvent: type, 8 KiB of parameters */
    *(u32 *)ev = SYSTEM_EVENT_RESET_VR_POSITION;
    LOG("system: ResetVrPosition -> game");
    return 0;
  }
  s32 rc = CALL_ORIG(s32(*)(u8 *), sceSystemServiceReceiveEvent, ev);
  /* The system's own ResetVrPosition (the PS5 sends one too, e.g. as the player comes back from the
   * home screen) kept from the game as the agent's own is (agent.resetevent=0): the agent re-seats the
   * head itself, and the game moving its origin at World scale 1.0 took the scenery away (the user,
   * 2026-11-14: "going to ps5 main menu and then going back to the game removes the background").
   * The next event, if any, is given in its place; else the call's own "none". */
  if (rc == 0 && ev)
    g_sysev_type = *(u32 *)ev, g_sysev_t = now_us(),
    g_sysev_kept = !g_opt_resetevent && *(u32 *)ev == SYSTEM_EVENT_RESET_VR_POSITION;
  if (rc == 0 && ev && *(u32 *)ev == 0x10000000u) /* the system resumed the game (ps5compat.c's lines) */
    __atomic_fetch_add(&g_resume_n, 1, __ATOMIC_RELEASE);
  if (rc == 0 && ev && g_opt_noresume && *(u32 *)ev == 0x10000000u) { /* (debug: the resume test) */
    LOG("system: the resume event 0x10000000 kept from the game (agent.noresume)");
    rc = CALL_ORIG(s32(*)(u8 *), sceSystemServiceReceiveEvent, ev);
  }
  if (rc == 0 && ev && !g_opt_resetevent && *(u32 *)ev == SYSTEM_EVENT_RESET_VR_POSITION) {
    static u32 said;
    if (said++ < 20)
      LOG("system: the system's ResetVrPosition not given to the game (agent.resetevent=0; the agent re-seats)");
    rc = CALL_ORIG(s32(*)(u8 *), sceSystemServiceReceiveEvent, ev);
  }
  /* what the system tells the game, PS VR2's own screens among it (the palm tracker's start rule) */
  static u32 n;
  if (rc == 0 && ev && __atomic_fetch_add(&n, 1, __ATOMIC_RELAXED) < 40)
    LOG_DETAIL("system: event 0x%x -> game (%u.%03u s)", *(u32 *)ev, (u32)(now_us() / 1000000),
        (u32)(now_us() / 1000 % 1000));
  return rc;
}

/* ---------------------------------------------------------------- setup dialog */
static s32 h_sceHmdSetupDialogInitialize(void) {
  static u32 n;
  LOG_FIRST(n, 3, "hmdsetup: Initialize");
  return 0;
}
static s32 h_sceHmdSetupDialogOpen(const void *param) {
  /* OrbisHmdSetupDialogParam: CommonDialog base (48 bytes), u64 size, s32 user_id */
  s32 user = param ? *(const s32 *)((const u8 *)param + 56) : -1;
  static u32 n;
  LOG_FIRST(n, 3, "hmdsetup: Open(%p) user %d", param, user);
  if (user > 0) /* (left 0: the initial user, GetDeviceInformation's, stands) */
    g_vr.user_id = user;
  return 0;
}
static s32 h_sceHmdSetupDialogClose(void) {
  static u32 n;
  LOG_FIRST(n, 3, "hmdsetup: Close");
  return 0;
}
/* A real dialog the game opened before the hooks went in (the agent came late) is ShellUI's
 * "connect PS VR" screen. Terminate alone left it on screen until a quick-settings visit or a
 * headset off/on cleared it (issue #4), so it is closed the CommonDialog way: real Close, real
 * UpdateStatus until FINISHED, and only then does the game see FINISHED and go on. */
static s32 h_sceHmdSetupDialogUpdateStatus(void) {
  static u32 closing;
  static u64 since;
  s32 real = CALL_ORIG(s32(*)(void), sceHmdSetupDialogUpdateStatus);
  if (real == CDLG_STATUS_RUNNING) {
    if (!closing) {
      closing = 1;
      since = now_us();
      s32 rc = sceHmdSetupDialogClose ? sceHmdSetupDialogClose() : -1;
      LOG("hmdsetup: the real dialog is up (opened before the agent): Close -> 0x%x", rc);
    }
    if (now_us() - since < 5000000)
      return CDLG_STATUS_RUNNING;
    if (closing == 1) {
      closing = 2;
      LOG("hmdsetup: the real dialog is still up after 5 s");
    }
    return CDLG_STATUS_FINISHED;
  }
  if (closing) {
    LOG("hmdsetup: the real dialog closed after %lu ms (status 0x%x)",
        (unsigned long)((now_us() - since) / 1000), real);
    closing = 0;
  }
  return CDLG_STATUS_FINISHED;
}
static s32 h_sceHmdSetupDialogGetResult(struct hmd_setup_dialog_result *r) {
  static u32 n;
  LOG_FIRST(n, 3, "hmdsetup: GetResult -> OK");
  if (r)
    r->result = 0;
  return 0;
}
static s32 h_sceHmdSetupDialogTerminate(void) {
  static u32 n;
  /* A dialog the real library opened before the hooks went in is closed by the real one. */
  s32 real = CALL_ORIG(s32(*)(void), sceHmdSetupDialogTerminate);
  LOG_FIRST(n, 3, "hmdsetup: Terminate (real -> 0x%x)", real);
  return 0;
}

/* ---------------------------------------------------------------- libSceVrTracker */
static s32 h_sceVrTrackerQueryMemory(const void *param, struct vrt_query_memory_result *r) {
  LOG("vrt: QueryMemory");
  if (!r)
    return VRT_ERR_ARGUMENT_INVALID;
  u32 size = r->size;
  memset(r, 0, sizeof *r);
  r->size = size;
  r->onion_size = 0x400000;
  r->onion_align = 0x10000;
  r->garlic_size = 0x3000000;
  r->garlic_align = 0x10000;
  r->work_size = 0x1000000;
  r->work_align = 0x10000;
  return 0;
}

static s32 h_sceVrTrackerInit(const void *param) {
  LOG("vrt: Init(%p)", param);
  g_vr.tracker_init = 1;
  return 0;
}

static s32 h_sceVrTrackerTerm(void) {
  LOG("vrt: Term");
  g_vr.tracker_init = 0;
  return 0;
}

static s32 h_sceVrTrackerRegisterDevice(s32 type, s32 handle) {
  LOG("vrt: RegisterDevice(type %d, handle 0x%x)", type, handle);
  if (type == VRT_DEVICE_HMD)
    g_vr.hmd_tracker_handle = handle;
  else if (type == VRT_DEVICE_DUALSHOCK4 && !g_vr.pad_tracker_handle)
    g_vr.pad_tracker_handle = handle;
  return 0;
}

static s32 h_sceVrTrackerUnregisterDevice(s32 handle) {
  LOG("vrt: UnregisterDevice(0x%x)", handle);
  if (handle == g_vr.pad_tracker_handle)
    g_vr.pad_tracker_handle = 0;
  return 0;
}

static s32 h_sceVrTrackerGpuSubmit(const void *p) { return 0; }
static s32 h_sceVrTrackerGpuWait(const void *p) { return 0; }
static s32 h_sceVrTrackerCpuProcess(const void *p) { return 0; }
static s32 h_sceVrTrackerGpuWaitAndCpuProcess(void) { return 0; } /* (a game's tracker thread) */
/* the game feeds its controllers' motion sensors to the tracker (param: size 0x20, device type) */
static s32 h_sceVrTrackerUpdateMotionSensorData(const u32 *p) {
  static u32 n;
  LOG_FIRST(n, 3, "vrt: UpdateMotionSensorData(device %u)", p ? p[1] : 0);
  return 0;
}
/* the tracker's clock, which the game's prediction times are counted from */
static s32 h_sceVrTrackerGetTime(u64 *out) {
  static u32 n;
  if (!out)
    return VRT_ERR_ARGUMENT_INVALID;
  *out = now_us();
  LOG_FIRST(n, 2, "vrt: GetTime -> %lu", *out);
  return 0;
}
/* A console's tracker answers "calibrating" for a moment after sceVrTrackerRecalibrate, then
 * "tracking" again. A game may wait on its calibration screen until it has seen that for the
 * controller (one PS VR game was found doing so), so at least one result says so, however late it asks. */
static struct {
  volatile u64 until;
  volatile u32 reported;
} g_recal[4] = {{0, 1}, {0, 1}, {0, 1}, {0, 1}};

static int recalibrating(u32 type, u64 now) {
  if (g_recal[type].reported && now >= g_recal[type].until)
    return 0;
  g_recal[type].reported = 1;
  return 1;
}

static s32 h_sceVrTrackerRecalibrate(const u32 *p) {
  u32 type = p ? p[1] : 0;
  LOG("vrt: Recalibrate(device %u, calibration %u)", type, p ? p[2] : 0);
  if (type < 4) {
    g_recal[type].until = now_us() + 200000;
    g_recal[type].reported = 0;
  }
  if (type == VRT_DEVICE_HMD)
    vr_recenter();
  return 0;
}
/* How the game registers its controllers (Func_B4D26B7D8B18DF06(type, handle, colour); the HMD
 * goes through RegisterDevice): type 1 = DualShock 4, 2 = Move; colour = light bar 0..4. */
static s32 h_sceVrTrackerFuncB4D2(s32 type, s32 handle, s32 colour) {
  LOG("vrt: register controller (type %d, handle 0x%x, colour %d)", type, handle, colour);
  if (type == VRT_DEVICE_DUALSHOCK4 && !g_vr.pad_tracker_handle) {
    g_vr.pad_tracker_handle = handle;
    g_vr.pad_led = colour;
  }
  return 0;
}

static void write_pose(struct vrt_pose *o, const struct pose *p) {
  o->px = p->pos[0];
  o->py = p->pos[1];
  o->pz = p->pos[2];
  o->qx = p->rot[0];
  o->qy = p->rot[1];
  o->qz = p->rot[2];
  o->qw = p->rot[3];
}

static s32 h_sceVrTrackerGetResult(const struct vrt_get_result_param *param, struct vrt_result *r) {
  static u32 n;
  if (!param || !r)
    return VRT_ERR_ARGUMENT_INVALID;
  s32 handle = param->handle;
  int is_hmd = handle == g_vr.hmd_tracker_handle;
  int is_pad = !is_hmd && handle == g_vr.pad_tracker_handle;
  memset(r, 0, sizeof *r);
  r->handle = handle;
  r->user_frame_number = param->user_frame_number;
  r->camera_orientation[3] = 1.0f;
  if (!is_hmd && !is_pad) {
    r->status = 2; /* not tracking */
    return 0;
  }
  agent_tick();
  __atomic_fetch_add(&g_vr.tracker_reads, 1, __ATOMIC_RELAXED);
  struct tracked dev;
  u64 now = now_us();
  /* the time the game asks the pose for (its own prediction ahead of GetTime) */
  u64 pred = param->prediction_time;
  vr_get_pose(is_hmd, pred, &dev);
  if (is_hmd && pred) {
    lead_note(&g_lead_ask_sum, &g_lead_ask_min, &g_lead_ask_max, &g_lead_ask_n, (s64)pred - (s64)now);
  }
  r->connected = 1;
  r->timestamp = pred ? pred : now;
  r->device_timestamp = now;
  r->led_color = is_pad ? (u32)g_vr.pad_led : 0;
  r->status = recalibrating(is_hmd ? VRT_DEVICE_HMD : VRT_DEVICE_DUALSHOCK4, now) ? 3 : 1;
  r->position_quality = 9;
  r->orientation_quality = 9;
  for (int i = 0; i < 3; i++) {
    r->vel[i] = dev.vel[i];
    r->ang_vel[i] = dev.ang_vel[i];
  }
  if (is_pad) {
    write_pose(&r->pad.device_pose, &dev.pose);
  } else {
    write_pose(&r->hmd.device_pose, &dev.pose);
    write_pose(&r->hmd.head_pose, &dev.head);
    write_pose(&r->hmd.left_eye_pose, &dev.eye[0]);
    write_pose(&r->hmd.right_eye_pose, &dev.eye[1]);
    r->hmd.rear_tracking_status = 1;
    r->hmd.sensor_read_system_timestamp = now;
  }
  LOG_FIRST(n, 6, "vrt: GetResult(%s 0x%x, type %u, pred %lu, orient %u, usage %u) pos %f %f %f",
            is_hmd ? "hmd" : "pad", handle, param->result_type, param->prediction_time,
            param->orientation_type, param->usage_type, dev.pose.pos[0], dev.pose.pos[1],
            dev.pose.pos[2]);
  if (is_hmd) /* the near view's centre for this frame (gaze.c, debug options; the pose untouched) */
    gaze_frame((const u8 *)__builtin_return_address(0));
  if (is_hmd) /* agent.realheight / heightoffset: the location scene's seat raised at its recentre
               * (height.c; the pose answered here untouched; both off: nothing done) */
    height_recentre(__builtin_return_address(0), param, r);
  return 0;
}

/* ---------------------------------------------------------------- libSceCamera */
#define CAM_W 1280
#define CAM_H 800
static u8 g_cam_frame[CAM_W * CAM_H * 2] __attribute__((aligned(256)));
static struct {
  volatile u32 started;
  u32 format[2][4];
  struct cam_exposure_gain eg[2];
  u64 count, frame_time, next_time;
} g_cam;

static s32 h_sceCameraOpen(s32 user, s32 type, s32 index, void *param) {
  LOG("cam: Open(user %d, type %d, index %d) -> 1", user, type, index);
  return 1;
}
static s32 h_sceCameraStart(s32 handle, const void *param) {
  LOG("cam: Start(%d)", handle);
  g_cam.started = 1;
  g_cam.next_time = 0;
  return 0;
}
static s32 h_sceCameraStop(s32 handle) {
  LOG("cam: Stop(%d)", handle);
  g_cam.started = 0;
  return 0;
}
static s32 h_sceCameraSetConfig(s32 handle, const struct cam_config *c) {
  if (c) {
    LOG("cam: SetConfig(%d, type %u, fmt0 %u/%u/%u/%u fmt1 %u, %u fps)", handle, c->type,
        c->ext[0].format.level[0], c->ext[0].format.level[1], c->ext[0].format.level[2],
        c->ext[0].format.level[3], c->ext[1].format.level[0], c->ext[0].framerate);
    for (int d = 0; d < 2; d++)
      for (int l = 0; l < 4; l++)
        g_cam.format[d][l] = c->ext[d].format.level[l];
  }
  return 0;
}
static s32 h_sceCameraSetVideoSync(s32 handle, const void *p) { return 0; }
static s32 h_sceCameraSetAutoWhiteBalance(s32 handle, s32 ch, u32 en, void *opt) { return 0; }
static s32 h_sceCameraSetExposureGain(s32 handle, s32 channel, const struct cam_exposure_gain *eg,
                                      void *opt) {
  if (eg)
    for (int d = 0; d < 2; d++)
      if (channel & (1 << d))
        g_cam.eg[d] = *eg;
  return 0;
}
static s32 h_sceCameraGetCalibrationData(const u32 *param, struct cam_calibration_data *out) {
  LOG("cam: GetCalibrationData");
  if (!param || !out)
    return CAM_ERR_PARAM;
  memset(out, 0, sizeof *out);
  out->format_type = param[1];
  out->function_type = param[2];
  return 0;
}
static s32 h_sceCameraGetFrameData(s32 handle, struct cam_frame_data *fd) {
  static u32 n;
  if (!fd || fd->size_this > sizeof(struct cam_frame_data) + 64)
    return CAM_ERR_PARAM;
  if (!g_cam.started)
    return CAM_ERR_NOT_START;
  if ((fd->read_mode & 1) || g_cam.count == 0) {
    u64 t = now_us();
    if (g_cam.next_time > t)
      sceKernelUsleep((u32)(g_cam.next_time - t));
    g_cam.frame_time = now_us();
    g_cam.next_time =
        (g_cam.next_time > g_cam.frame_time ? g_cam.next_time : g_cam.frame_time) + 16667;
    g_cam.count++;
  }
  struct cam_frame_data f;
  memset(&f, 0, sizeof f);
  for (int d = 0; d < 2; d++) {
    for (int l = 0; l < 4; l++) {
      u32 w = CAM_W >> l, h = CAM_H >> l;
      f.position[d][l].xsize = w;
      f.position[d][l].ysize = h;
      f.frame[d][l] = g_cam_frame;
      f.frame_garlic[d][l] = g_cam_frame;
      f.frame_size[d][l] = w * h * 2;
      f.meta.format[d][l] = g_cam.format[d][l];
    }
    f.status[d] = 1;
    f.meta.frame[d] = g_cam.count - 1;
    f.meta.timestamp[d] = g_cam.frame_time;
    f.meta.device_timestamp[d] = (u32)g_cam.frame_time;
    f.meta.exposure_gain[d] = g_cam.eg[d];
  }
  f.meta.acc_y = 1.0f;
  f.meta.vcounter = g_cam.count - 1;
  size_t hdr = 8, size = fd->size_this < sizeof f ? fd->size_this : sizeof f;
  if (size > hdr)
    memcpy((u8 *)fd + hdr, (u8 *)&f + hdr, size - hdr);
  LOG_FIRST(n, 2, "cam: GetFrameData(size %u, mode %u) frame %lu", fd->size_this, fd->read_mode,
            g_cam.count - 1);
  return 0;
}

/* ---------------------------------------------------------------- add-on entitlements
 * Both games query sceAppContentGetAddcontInfoList for their add-on content. Scarlet's VR PARADISE
 * is behind the discontinued "VR Passport"; other content checks specific CATEGORY labels. With
 * agent.vrunlock=1 the agent appends all four known labels as installed, so every content gate
 * passes; agent.vrunlock=2 logs the queries only.
 * SceAppContentAddcontInfo: 17-char label (3 padding), status (u32) at +0x14. The game calls
 * first with list=NULL for the count, then with a buffer. Harmless to Fortune (73 add-ons
 * installed already; any duplicates are just extra entries the game already has). */
struct addcont_info {
  char label[20];
  u32 status;
};
/* agent.nopassport=1 (debug, a test of the fix for players without it): the VR Passport add-on as
 * if not installed, nothing deleted: left out of the add-on list and its mount refused. Fortune's
 * is 10DLC00000000000 ("VR Passport by 14QwerT88"), Scarlet's 00DLC00130000000 (read from their
 * packages' param.sfo, 2026-11-12). */
static int readable(const void *p, u64 n);
/* Scarlet's builds, for its own features below: told by the same 8 bytes (the Item map's malfunction
 * read: mov 0x18(%rsi), %rax; movzwl 2(%rax), %eax) at each build's place, their globals here.
 * 1.00 (CUSA14831, no update) has none of the updates' data (1.08's packages 01-07): the features
 * built on 1.08's rows are off for it (full 0). */
struct scarlet_build {
  const char *name;
  u32 sig;
  u32 owner;              /* owner_global's P: **P +0x30 the owner's and the girls' belongings */
  u32 item_map;           /* the merged item map (item_map_row) */
  u32 obtained, incloset; /* the closet's two calls (port_give) */
  u32 fan;                /* the fan thread's state pointer */
  u32 chars, slots;       /* the characters' list, the slot table (chars_log) */
  u32 labels;             /* the Item labels' map (malfunction_keep) */
  u32 full;               /* 1: the updates' data (1.08) */
};
static const struct scarlet_build k_scarlet[] = {
    {"Scarlet 1.08", 0x5e2d80, 0xaa9e40, 0xc4c488, 0x383210, 0x3834d0, 0xa9fe80, 0x94a980, 0xaa3fe0, 0xbf1960, 1},
    {"Scarlet 1.00", 0x5e1020, 0xaa9e00, 0xc4c448, 0x382b70, 0x382e30, 0xa9fe40, 0x94a980, 0xaa3fa0, 0xbf1920, 0},
};
static const struct scarlet_build *scarlet_of(const u8 *base) {
  static const u8 sig[8] = {0x48, 0x8b, 0x46, 0x18, 0x0f, 0xb7, 0x40, 0x02};
  static const u8 *seen; /* (the answer kept once the code was readable: it does not change) */
  static const struct scarlet_build *found;
  if (!base)
    return 0;
  if (__atomic_load_n(&seen, __ATOMIC_ACQUIRE) == base)
    return found;
  const struct scarlet_build *f = 0;
  u32 all = 1;
  for (u32 i = 0; i < sizeof k_scarlet / sizeof *k_scarlet && !f; i++) {
    int r = readable(base + k_scarlet[i].sig, 8);
    all &= r != 0;
    if (r && memcmp(base + k_scarlet[i].sig, sig, 8) == 0)
      f = &k_scarlet[i];
  }
  if (f || all)
    found = f, __atomic_store_n(&seen, base, __ATOMIC_RELEASE);
  return f;
}
/* (a feature that needs 1.08's data: said once for Scarlet 1.00, 1 when it may run) */
static int scarlet_full(const struct scarlet_build *sb, const char *what, u32 *said) {
  if (sb && !sb->full && !*said && (*said = 1))
    LOG("%s: off for %s (it needs Scarlet 1.08's data, its update packages)", what, sb->name);
  return sb && sb->full;
}
static const char *game_passport(void) {
  u8 *base = (u8 *)astro_hdr.game_base;
  if (!base)
    return 0;
  /* Fortune 1.19: its gate builds this label (0x41ea23, lea of the string at 0x80eee7) and looks
   * it up in the add-ons the boot GetAddcontInfoList listed */
  if (readable(base + 0x80eee7, 16) && memcmp(base + 0x80eee7, "10DLC00000000000", 16) == 0)
    return "10DLC00000000000";
  if (scarlet_of(base))
    return "00DLC00130000000"; /* (Scarlet 1.08 and 1.00: their own gate is patched by the payload) */
  return 0;
}
static int passport_label(const char *l) {
  const char *p = game_passport();
  return p && memcmp(l, p, 16) == 0;
}
/* The Passport reported installed though it is not (Fortune): its mount answered with a folder the
 * game lists that holds no Archive.psarc (the game lists the mount point ("%s/" and a star) for that archive,
 * 0x6eb7a7; a failed mount it never checks, and with an empty mount point it closed itself,
 * 2026-11-12), and the unmount of that folder answered done. 16 bytes the mount point at most. */
static int g_passport_faked;
#define PASSPORT_MOUNT "/app0" /* (the game's own, listed by its file layer; no Archive.psarc at its top:
                               * with /data/doax3vr2 the game closed itself after the answer) */
/* sceAppContentAddcontMount(service label, entitlement label (17 chars), mount point out):
 * logged (which add-ons the game mounts, and when), the Passport's refused with agent.nopassport */
static s32 h_sceAppContentAddcontMount(u32 service_label, const char *label, void *mount) {
  if (g_passport_faked && label && passport_label(label) && mount) {
    s32 real_rc = g_opt_nopassport ? (s32)0x80d90005
                                   : CALL_ORIG(s32(*)(u32, const char *, void *), sceAppContentAddcontMount,
                                               service_label, label, mount);
    if (real_rc == 0)
      return 0; /* (it is there after all) */
    memset(mount, 0, 16);
    memcpy(mount, PASSPORT_MOUNT, sizeof PASSPORT_MOUNT);
    static u32 n;
    LOG_FIRST(n, 4, "appcontent: AddcontMount '%.16s' (not installed, 0x%x): answered with %s", label,
              (u32)real_rc, PASSPORT_MOUNT);
    return 0;
  }
  if (g_opt_nopassport && label && passport_label(label)) {
    static u32 n;
    LOG_FIRST(n, 8, "appcontent: AddcontMount '%.16s' refused (agent.nopassport=1)", label);
    return (s32)0x80d90005; /* (SCE_APP_CONTENT_ERROR_NOT_FOUND, as for one not installed) */
  }
  s32 rc = CALL_ORIG(s32(*)(u32, const char *, void *), sceAppContentAddcontMount, service_label, label, mount);
  static u32 n;
  LOG_FIRST(n, 40, "appcontent: AddcontMount(svc %u, '%.16s') -> 0x%x%s%.16s", service_label, label ? label : "",
            (u32)rc, rc == 0 && mount ? " at " : "", rc == 0 && mount ? (const char *)mount : "");
  return rc;
}
static s32 h_sceAppContentAddcontUnmount(const char *mount) {
  if (mount && memcmp(mount, PASSPORT_MOUNT, sizeof PASSPORT_MOUNT) == 0) {
    static u32 n;
    LOG_FIRST(n, 4, "appcontent: AddcontUnmount %s (the Passport's answer): done", PASSPORT_MOUNT);
    return 0;
  }
  s32 rc = CALL_ORIG(s32(*)(const char *), sceAppContentAddcontUnmount, mount);
  static u32 n;
  LOG_FIRST(n, 10, "appcontent: AddcontUnmount %.16s -> 0x%x", mount ? mount : "", (u32)rc);
  return rc;
}
/* The PSN network clock (sceRtcGetCurrentNetworkTick) fails while the console is not signed in to
 * PSN. Both games date their online sale data with it, and Scarlet's Owner Shop builds its Special
 * tab (0x427170 -> 0x4249f0) and allows a Special purchase (0x4029b3) only when it answers. With
 * agent.special (default on) a failed call answers the console's own clock instead (UTC, in the
 * tick's microseconds since 0001-01-01), so the Special tab works offline (special_sale). */
static s32 h_sceRtcGetCurrentNetworkTick(u64 *tick) {
  s32 rc = CALL_ORIG(s32 (*)(u64 *), sceRtcGetCurrentNetworkTick, tick);
  if (rc != 0 && tick && g_opt_special) {
    struct { s64 sec, nsec; } ts = {0, 0};
    if (clock_gettime && clock_gettime(0, (void *)&ts) == 0 && ts.sec > 0) {
      static u32 said;
      if (said++ < 2)
        LOG("rtc: the network clock failed (0x%x): the console's clock instead (agent.special)", rc);
      ts.sec += (s64)g_opt_clockhours * 3600;
      *tick = (u64)(ts.sec + 62135596800ll) * 1000000ull + (u64)ts.nsec / 1000;
      rc = 0;
    }
  }
  /* (the Owner Shop asks the clock right before it builds its Special tab, 0x424020 -> 0x427170,
   * after the game may have made its sale list anew: the items it never lists added here, on the
   * game's own thread, so the tab has them) */
  if (rc == 0 && g_opt_special)
    special_sale();
  return rc;
}
static s32 h_sceAppContentGetAddcontInfoList(u32 service_label, struct addcont_info *list, u32 list_num,
                                             u32 *hit_num) {
  cvars_fetch(); /* (the game asks before the agent's threads have read agent.vrunlock: once) */
  s32 rc = CALL_ORIG(s32 (*)(u32, struct addcont_info *, u32, u32 *),
                     sceAppContentGetAddcontInfoList, service_label, list, list_num, hit_num);
  u32 real = (rc == 0 && hit_num) ? *hit_num : 0;
  if (g_opt_nopassport && rc == 0 && hit_num) { /* the Passport left out (agent.nopassport=1) */
    static u32 said;
    if (list) {
      u32 k = 0, gone = 0;
      for (u32 i = 0; i < real && i < list_num; i++)
        if (passport_label(list[i].label))
          gone++;
        else
          list[k++] = list[i];
      if (gone && said++ < 4)
        LOG("appcontent: the VR Passport left out of the add-on list (agent.nopassport=1): %u of %u", gone, real);
      real = k;
    } else if (real) {
      real--; /* (the count query: one fewer, the Passport assumed installed; the list call checks) */
    }
    *hit_num = real;
  }
  if (g_opt_vrunlock == 2) {
    static u32 n;
    if (n++ < 40)
      LOG("appcontent: GetAddcontInfoList(svc %u, list %p, num %u) -> 0x%x, %u installed%s", service_label,
          (void *)list, list_num, rc, real, list ? "" : " (count query)");
    if (rc == 0 && list)
      for (u32 i = 0; i < real && i < list_num && i < 8; i++)
        LOG("appcontent:   [%u] label '%.17s' status %u", i, list[i].label, list[i].status);
    return rc;
  }
  if (g_opt_vrunlock != 1 || rc != 0)
    return rc;
  /* Fortune's VR Passport by its own label, when it is not really there (the CATEGORY labels
   * below are not what Fortune's VR gate looks up: it builds "10DLC00000000000", 0x41ea23) */
  const char *pp = game_passport();
  /* OFF (2026-11-12): reported installed, Fortune mounts the Passport and its PackageSetup thread
   * loads the package inside it (no check for one missing: SIGSEGV at 0x641ec7 reading +4 of a null
   * header); with "no extra data" the gate stays shut. The Passport is content, not only a key:
   * this stays off; the gate itself is patched instead (docs/LIMITS.md, Fortune). */
  int pp_add = 0 && pp && memcmp(pp, "10DLC", 5) == 0;
  if (pp_add && list)
    for (u32 i = 0; i < real && i < list_num; i++)
      pp_add &= memcmp(list[i].label, pp, 16) != 0;
  u32 added = N_INJECT_LABELS + (pp_add ? 1u : 0u);
  if (!list) {
    if (hit_num)
      *hit_num = real + added;
    return rc;
  }
  if (pp_add && real < list_num) {
    memset(&list[real], 0, sizeof list[real]);
    memcpy(list[real].label, pp, 16);
    list[real].status = 4; /* (installed: with 0, no extra data, the gate stayed shut; installed,
                            * the game mounts it: answered by h_sceAppContentAddcontMount) */
    g_passport_faked = 1;
    real++;
    static u32 said_pp;
    if (!said_pp++)
      LOG("appcontent: the VR Passport %.16s reported installed (agent.vrunlock=1: not really there)", pp);
  }
  u32 injected = 0, had = 0;
  static u32 told;
  if (!told++)
    for (u32 i = 0; i < real && i < list_num && i < 8; i++)
      LOG("appcontent: installed [%u] '%.17s' status %u", i, list[i].label, list[i].status);
  for (u32 k = 0; k < added && real + injected < list_num; k++) {
    u32 slot = real + injected, have = 0;
    for (u32 i = 0; i < real && i < list_num; i++) /* (a label really installed: the system's own entry) */
      have |= memcmp(list[i].label, k_all_labels[k], 16) == 0;
    if (have) {
      had++;
      continue;
    }
    memset(&list[slot], 0, sizeof list[slot]);
    for (u32 j = 0; j < 17 && k_all_labels[k][j]; j++)
      list[slot].label[j] = k_all_labels[k][j];
    list[slot].status = 2;
    injected++;
  }
  if (hit_num)
    *hit_num = real + injected;
  static u32 said;
  if (!said++)
    LOG("appcontent: %u entitlement labels injected (agent.vrunlock=1); %u real + %u (%u of the labels really installed: not added)",
        added, real, injected, had);
  return rc;
}

/* ---------------------------------------------------------------- Scarlet's malfunction flags
 * The Item labels' map (logs/research/malfunction/REPORT.md): a static at 0xbf1960 points to a
 * hash map (+0x08 bucket count, +0x10 element count, +0x28 bucket array; buckets[bucket count]
 * heads one list through every node: +0x00 next, +0x10 u16 item id, +0x18 the extdata: u16 kind
 * (7 a costume), u16 the malfunction flag). Scarlet 1.08 ships 15 costumes with the flag 0 that
 * Fortune has at 1 (AYA_sh_07, HON_tk_06, OWN_uw_03); agent.malfunction=1 writes 1 there, so
 * their "[Malfunction]" tag and the pictorial settings' On/Off row come back (the break itself is
 * the model's COS_BREAK_DISP_TYPE, the Jet Black Fan's shake). Scarlet only: its own read of the
 * flag (0x5e2d80) checked first. Kept every 2 s: the game rebuilds the maps on loads. */
static int readable(const void *p, u64 n);
static int writable(const void *p, u64 n);
static int malfunction_id(u32 id) {
  return (id >= 0x5a && id <= 0x5c) || (id >= 0xa4 && id <= 0xa6) || (id >= 0x4ae && id <= 0x4b6);
}
/* Where the item master's rows are once loaded: the CPU-readable, writable memory searched once for
 * the start of a row Scarlet has (0x4f6, OWN_et_17_i) and of the one pkg/03-one adds (0x4f7,
 * Salmon Roe A): id, name index, the two icon hashes. Each hit logged with the id of the row
 * before it (stride 0x80) and the region. Found (2026-11-11): the merged table has 0x4f6 then
 * 0x500 (Scarlet's own ids only); the ported 0x4f7 sits after 0x42c in package 03's own loaded
 * copy: the game reads the rows but merges later packages' rows only over ids it has. */
static void rows_scan(void) {
  static const u8 native[12] = {0xf6, 0x04, 0xba, 0x05, 0x9b, 0xef, 0x0e, 0xe0, 0x04, 0x2e, 0x57, 0xca};
  static const u8 ported[12] = {0xf7, 0x04, 0xcd, 0x05, 0x62, 0x7a, 0xb5, 0xae, 0x5a, 0x49, 0xfa, 0x2b};
  if (!sceKernelVirtualQuery)
    return;
  u64 a = 0x100000, scanned_mb = 0;
  u32 regions = 0, hits = 0;
  u64 t0 = now_us();
  for (int guard = 0; guard < 20000 && hits < 24; guard++) {
    u8 info[128];
    memset(info, 0, sizeof info);
    if (sceKernelVirtualQuery((void *)a, 1 /* the next mapping at or after */, info, 0x48) != 0)
      break;
    u64 start = *(u64 *)info, end = *(u64 *)(info + 8);
    s32 prot = *(s32 *)(info + 24);
    if (end <= a)
      break;
    a = end;
    if ((prot & 3) != 3 || end - start > (1ull << 31))
      continue;
    regions++;
    scanned_mb += (end - start) >> 20;
    for (u8 *p = (u8 *)start; p + 12 <= (u8 *)end; p += 2) {
      if (p[0] != 0xf6 && p[0] != 0xf7)
        continue;
      if (p[1] != 0x04)
        continue;
      int which = memcmp(p, native, 12) == 0 ? 1 : memcmp(p, ported, 12) == 0 ? 2 : 0;
      if (!which)
        continue;
      u32 prev = (u8 *)p - 0x80 >= (u8 *)start ? *(u16 *)(p - 0x80) : 0xffff;
      u32 next = p + 0x80 + 2 <= (u8 *)end ? *(u16 *)(p + 0x80) : 0xffff;
      LOG("rows: %s row at %p (region %p-%p prot %d), the row before 0x%x, after 0x%x",
          which == 1 ? "0x4f6 (Scarlet's)" : "0x4f7 (ported, Salmon Roe A)", (void *)p, (void *)start,
          (void *)end, prot, prev, next);
      hits++;
    }
  }
  LOG("rows: %u writable regions, %llu MB searched in %llu ms, %u rows found", regions,
      (unsigned long long)scanned_mb, (unsigned long long)((now_us() - t0) / 1000), hits);
}
/* The owner's and the girls' belongings (Scarlet 1.08, read 2026-11-11 from 0x18d450, which the fan
 * tests call): **(base+0xaa9e40) +0x30 is a map (a tree: node +0x00 left, +0x10 right, +0x19 nil
 * flag, +0x20 u16 key: 0 the owner, else a girl; +0x28 her belongings); the belongings' +0x68 /
 * +0x70 are the begin / end of entries 0x28 bytes each (+0 u16 item id, +6 u16: 1 counted as had).
 * agent.inv=1: each key's entries logged once (how many, how many had, the first ids), read only. */
/* agent.closet=ID (debug, live): each girl's closet map (her belongings +0x90: a tree, node +0x1a
 * u16 id, +0x1c obtained, +0x1d in the closet) searched for ids ID..ID+9 and the result logged
 * once a value: a check that a bought set gave its suits. Both games (owner_global's P). */
static u8 *owner_global(u8 *base, u32 *money_cap);
__attribute__((unused)) static void closet_log(void) {
  static u32 done_for;
  u8 *base = (u8 *)astro_hdr.game_base;
  if (!g_opt_closet || g_opt_closet == done_for || !base)
    return;
  u32 cap;
  u8 *gp = owner_global(base, &cap);
  u8 **pp = gp ? *(u8 ***)gp : 0;
  if (!pp || !readable(pp, 8) || !*pp || !readable(*pp + 0x30, 8))
    return;
  done_for = g_opt_closet;
  u8 *head = *(u8 **)(*pp + 0x30);
  u8 *stack[64];
  u32 sp = 0;
  u8 *n = head && readable(head, 0x30) ? *(u8 **)(head + 8) : 0;
  while ((n && readable(n, 0x30) && !n[0x19]) || sp) {
    while (n && readable(n, 0x30) && !n[0x19] && sp < 64)
      stack[sp++] = n, n = *(u8 **)n;
    if (!sp)
      break;
    n = stack[--sp];
    u32 key = *(u16 *)(n + 0x20);
    u8 *bel = *(u8 **)(n + 0x28);
    char line[200];
    u32 o = 0, found = 0;
    if (bel && readable(bel + 0x90, 8)) {
      u8 *hdr = *(u8 **)(bel + 0x90);
      u8 *st2[64];
      u32 s2 = 0;
      u8 *m = hdr && readable(hdr, 0x20) ? *(u8 **)(hdr + 8) : 0;
      while ((m && readable(m, 0x20) && !m[0x19]) || s2) {
        while (m && readable(m, 0x20) && !m[0x19] && s2 < 64)
          st2[s2++] = m, m = *(u8 **)m;
        if (!s2)
          break;
        m = st2[--s2];
        u32 id = *(u16 *)(m + 0x1a);
        if (id >= g_opt_closet && id < g_opt_closet + 10 && o < sizeof line - 16)
          found++, o += (u32)agent_snprintf(line + o, sizeof line - o, " %x:%u%u", id, m[0x1c], m[0x1d]);
        m = *(u8 **)(m + 0x10);
      }
    }
    if (bel && readable(bel + 0x68, 16)) { /* (her belongings list too: id:state, 2 a wrapped gift) */
      u8 *b = *(u8 **)(bel + 0x68), *e = *(u8 **)(bel + 0x70);
      u64 c = e > b && (u64)(e - b) < 0x100000 ? (u64)(e - b) / 0x28 : 0;
      for (u64 q = 0; q < c && readable(b + q * 0x28, 8); q++) {
        u32 id = *(u16 *)(b + q * 0x28);
        if (id >= g_opt_closet && id < g_opt_closet + 10 && o < sizeof line - 16)
          found++, o += (u32)agent_snprintf(line + o, sizeof line - o, " bel %x:%u", id, *(u16 *)(b + q * 0x28 + 6));
      }
    }
    if (found)
      LOG("closet: key %u has%s (id:obtained,in closet)", key, line);
    n = *(u8 **)(n + 0x10);
  }
  LOG("closet: ids 0x%x-0x%x searched", g_opt_closet, g_opt_closet + 9);
}
static void inv_dump(void) {
#ifndef RELEASE
  closet_log();
#endif
  static u32 done;
  if (!g_opt_inv || done || !astro_hdr.game_base)
    return;
  u8 *base = (u8 *)astro_hdr.game_base;
  u32 cap0;
  u8 *gp0 = owner_global(base, &cap0); /* (both games) */
  u8 **pp = gp0 ? *(u8 ***)gp0 : 0;
  if (!pp || !readable(pp, 8) || !*pp || !readable(*pp + 0x30, 8))
    return;
  u8 *head = *(u8 **)(*pp + 0x30);
  if (!head || !readable(head, 0x30))
    return;
  /* in-order walk of the tree, iteratively, up to 64 nodes */
  u8 *stack[64];
  u32 sp = 0, nodes = 0;
  u8 *n = *(u8 **)(head + 8);
  while ((n && readable(n, 0x30) && !n[0x19]) || sp) {
    while (n && readable(n, 0x30) && !n[0x19] && sp < 64) {
      stack[sp++] = n;
      n = *(u8 **)n;
    }
    if (!sp)
      break;
    n = stack[--sp];
    nodes++;
    u32 key = *(u16 *)(n + 0x20);
    u8 *bel = *(u8 **)(n + 0x28);
    if (bel && readable(bel + 0x68, 16)) {
      u8 *b = *(u8 **)(bel + 0x68), *e = *(u8 **)(bel + 0x70);
      u64 cnt = (e > b && (u64)(e - b) < 0x100000) ? (u64)(e - b) / 0x28 : 0;
      u32 had = 0, fan = 0xffff;
      char ids[256];
      u32 off = 0;
      ids[0] = 0;
      if (cnt && readable(b, cnt * 0x28))
        for (u64 i = 0; i < cnt; i++) {
          u8 *en = b + i * 0x28;
          u32 id = *(u16 *)en, st = *(u16 *)(en + 6);
          if (st == 1)
            had++;
          if (id == 0x3be || id == 0x53a)
            fan = st;
          if (off < sizeof ids - 16 && i < 40)
            off += agent_snprintf(ids + off, sizeof ids - off, " %x:%u", id, st);
        }
      if (cnt >= 4 && (key == 0 || key == 2) && readable(b, cnt * 0x28))
        for (u32 i = 0, shown = 0; i < cnt && shown < (key == 0 ? 80u : 4u); i++) {
          u8 *en = b + i * 0x28;
          if (*(u16 *)en == 0xffff)
            continue;
          shown++;
          char hx[0x28 * 3 + 1];
          for (u32 j = 0; j < 0x28; j++)
            agent_snprintf(hx + j * 3, 4, "%02x ", en[j]);
          LOG("inv: key %u entry %u at %p: %s", key, i, (void *)en, hx);
          u8 *tp = *(u8 **)(en + 0x10);
          if (tp && readable(tp - 0x10, 0x50)) {
            char th[0x50 * 3 + 1];
            for (u32 j = 0; j < 0x50; j++)
              agent_snprintf(th + j * 3, 4, "%02x ", tp[(s32)j - 0x10]);
            LOG("inv:   +0x10 -> %p (from -0x10): %s", (void *)tp, th);
          }
        }
      LOG("inv: key %u: %llu entries, %u with +6 == 1, Jet Black Fan entry %s%u; first:%s", key,
          (unsigned long long)cnt, had, fan == 0xffff ? "none " : "state ", fan == 0xffff ? 0 : fan, ids);
    }
    n = *(u8 **)(n + 0x10);
  }
  LOG("inv: %u keys", nodes);
  done = 1;
}
static u8 *inv_entries(u8 *base, u32 want_key, u64 *cnt) {
  const struct scarlet_build *sb = scarlet_of(base);
  if (!sb)
    return 0;
  u8 **pp = *(u8 ***)(base + sb->owner);
  if (!pp || !readable(pp, 8) || !*pp || !readable(*pp + 0x30, 8))
    return 0;
  u8 *head = *(u8 **)(*pp + 0x30), *n = head && readable(head, 0x30) ? *(u8 **)(head + 8) : 0;
  while (n && readable(n, 0x30) && !n[0x19]) { /* a search tree by key */
    u32 k = *(u16 *)(n + 0x20);
    if (k == want_key) {
      u8 *bel = *(u8 **)(n + 0x28);
      if (!bel || !readable(bel + 0x68, 16))
        return 0;
      u8 *b = *(u8 **)(bel + 0x68), *e = *(u8 **)(bel + 0x70);
      *cnt = (e > b && (u64)(e - b) < 0x100000) ? (u64)(e - b) / 0x28 : 0;
      return *cnt && writable(b, *cnt * 0x28) ? b : 0;
    }
    n = *(u8 **)(n + (want_key < k ? 0x00 : 0x10));
  }
  return 0;
}
/* The closet's own record (read 2026-11-11, 0x383fa0 / 0x3127c0): a girl's belongings +0x90 heads a map
 * (header; root at header+8; node +0x00 left, +0x10 right, +0x19 nil, +0x1a u16 id, +0x1c obtained,
 * +0x1d in the closet). A swimsuit is wearable when both are set; the game saves them (0x396920). */
static int closet_unlock(u8 *base, u32 key, u32 id) {
  const struct scarlet_build *sb = scarlet_of(base);
  if (!sb)
    return -1;
  u8 **pp = *(u8 ***)(base + sb->owner);
  if (!pp || !readable(pp, 8) || !*pp || !readable(*pp + 0x30, 8))
    return -1;
  u8 *head = *(u8 **)(*pp + 0x30), *n = head && readable(head, 0x30) ? *(u8 **)(head + 8) : 0, *bel = 0;
  while (n && readable(n, 0x30) && !n[0x19]) {
    u32 k = *(u16 *)(n + 0x20);
    if (k == key) {
      bel = *(u8 **)(n + 0x28);
      break;
    }
    n = *(u8 **)(n + (key < k ? 0x00 : 0x10));
  }
  if (!bel || !readable(bel + 0x90, 8))
    return -2;
  u8 *hdr = *(u8 **)(bel + 0x90), *m = hdr && readable(hdr, 0x20) ? *(u8 **)(hdr + 8) : 0;
  for (int guard = 0; m && guard < 64 && readable(m, 0x20) && !m[0x19]; guard++) {
    u32 mid = *(u16 *)(m + 0x1a);
    if (mid == id) {
      if (!writable(m + 0x1c, 2))
        return -3;
      u32 was = m[0x1c] | m[0x1d] << 8;
      m[0x1c] = 1;
      m[0x1d] = 1;
      return (int)was;
    }
    m = *(u8 **)(m + (id < mid ? 0x00 : 0x10));
  }
  return -4; /* not in her map: the game's insert (0x758ef0) would be needed, on its thread */
}
/* The merged item map (logs/research/port-fortune, 2026-11-11 research): an unordered_map<u16,row>
 * at *(base+0xc4c488): +0x08 bucket count, +0x10 element count, +0x28 buckets; buckets[count]
 * heads the one list of every node; node +0 next, +0x10 u16 id, +0x14 the row (0x80 bytes,
 * Scarlet). Every package's rows go through its operator[] (0x635580): a new id is inserted. */
/* (a row from a game's item map at base+at: Scarlet 0xc4c488 (1.00 0xc4c448), Fortune 0xc44388) */
static u8 *item_map_row_at(u8 *base, u32 at, u32 id) {
  u8 **mp = (u8 **)(base + at);
  if (!readable(mp, 8) || !*mp || !readable(*mp, 0x30))
    return 0;
  u8 *m = *mp;
  u64 nb = *(u64 *)(m + 0x08), *bk = *(u64 **)(m + 0x28);
  if (!nb || nb > 0x100000 || !bk || !readable(bk + nb, 8))
    return 0;
  u8 *nd = (u8 *)bk[nb];
  for (u32 guard = 0; nd && guard < 20000 && readable(nd, 0x84); guard++, nd = *(u8 **)nd)
    if (*(u16 *)(nd + 0x10) == id)
      return nd + 0x14;
  return 0;
}
static u8 *item_map_row(u8 *base, u32 id, u64 *count) {
  const struct scarlet_build *sb = scarlet_of(base);
  if (!sb)
    return 0;
  u8 **mp = (u8 **)(base + sb->item_map);
  if (!readable(mp, 8) || !*mp || !readable(*mp, 0x30))
    return 0;
  u8 *m = *mp;
  u64 nb = *(u64 *)(m + 0x08), *b = *(u64 **)(m + 0x28);
  if (count)
    *count = *(u64 *)(m + 0x10);
  if (!nb || nb > 0x100000 || !b || !readable(b + nb, 8))
    return 0;
  u8 *n = (u8 *)b[nb];
  for (u32 guard = 0; n && guard < 20000 && readable(n, 0x94); guard++, n = *(u8 **)n)
    if (*(u16 *)(n + 0x10) == id)
      return n + 0x14;
  return 0;
}
/* Fortune's swimsuits Scarlet lacks (rows from /data/doax3vr2/port/03/package_data.bin, pkg/03-all):
 * five sets of nine (Clam, Sandwich, Illusion, Pistachio, Salmon Roe; which is which: the
 * names in MES_ITEM), modelled on Scarlet's disc for Kasumi, Marie Rose, Honoka, Helena. Logged
 * when the map's count changes; agent.portgive=1: each put in Kasumi's closet (its flags, as
 * closet_unlock does for Scarlet's own), if her closet map has a node for it. */
static const u16 k_port_sets[5] = {0x2fa, 0x3ac, 0x3d8, 0x4b7, 0x4f7};
static int closet_unlock(u8 *base, u32 key, u32 id);
/* agent.portshop=N (1 to 8): the Owner Shop's Swimsuits tab (tab 1) stocked with the new suits,
 * batch N of six at a time (Salmon Roe first), so the game's own purchase files them in the
 * closet (the closet map has no node for an id it never saw). The shop: S = *(P+0x98), P =
 * *(base+0xaa9e40); tab t at *(S+0x20) + t*0x28, its entries [*(tab+0x10), *(tab+0x18)) of 0x20:
 * +0 u16 id, +2 state, +8 the row (owner-shop REPORT.md). Written only where each entry's row
 * pointer is the item map's row for its id (the layout confirmed), every 2 s (a day's rebuild). */
static void port_shop(u8 *base) {
  static const u16 order[5] = {0x4f7, 0x2fa, 0x3ac, 0x3d8, 0x4b7};
  const struct scarlet_build *sb = scarlet_of(base);
  if (!g_opt_portshop || g_opt_portshop > 8 || !sb)
    return;
  u8 **pp = *(u8 ***)(base + sb->owner);
  u8 *shop = pp && readable(pp + 0x13, 8) ? (u8 *)pp[0x98 / 8] : 0;
  if (!shop || !readable(shop + 0x20, 8) || !*(u8 **)(shop + 0x20))
    return;
  u8 *tab = *(u8 **)(shop + 0x20) + 1 * 0x28;
  if (!readable(tab, 0x28))
    return;
  u8 *b = *(u8 **)(tab + 0x10), *e = *(u8 **)(tab + 0x18);
  u32 cnt = b && e > b && (u64)(e - b) <= 0x20 * 64 ? (u32)((e - b) / 0x20) : 0;
  if (!cnt || !writable(b, cnt * 0x20))
    return;
  static u32 said;
  for (u32 i = 0; i < cnt; i++) {
    u8 *en = b + i * 0x20;
    u32 id = *(u16 *)en;
    u8 *row = *(u8 **)(en + 8);
    int ours = 0;
    for (u32 k = 0; k < 5; k++)
      ours |= id >= order[k] && id < order[k] + 9u;
    if (ours)
      continue; /* (already written) */
    if (row != item_map_row(base, id, 0)) {
      if (said++ < 4)
        LOG("port: shop entry %u id 0x%x row %p is not the map's %p: left alone", i, id, (void *)row,
            (void *)item_map_row(base, id, 0));
      return;
    }
  }
  u32 first = (g_opt_portshop - 1) * cnt;
  for (u32 i = 0; i < cnt && first + i < 45; i++) {
    u32 k = first + i, nid = order[k / 9] + k % 9;
    u8 *row = item_map_row(base, nid, 0), *en = b + i * 0x20;
    if (!row || *(u16 *)en == nid)
      continue;
    LOG("port: Owner Shop swimsuit %u: 0x%x -> 0x%x (state 0x%x)", i, *(u16 *)en, nid, *(u16 *)(en + 2));
    *(u8 *volatile *)(en + 8) = row;
    *(volatile u16 *)en = (u16)nid;
  }
}
/* The Softness Sensors (Switch: Soft 4D, its rumble; the PS4 game has no code for them, they do
 * nothing): kept out of the shops unless agent.sensors=1 (asked 2026-11-11), by their rows' shop
 * flags (+0x1e normal shop, +0x1f Owner Shop) set to 0 in the item map, every 2 s (the map is
 * rebuilt once after boot); the shop stock already drawn keeps them until the next day's. Their
 * ids 0x55e-0x56a and 0x5b7-0x5c1, category 28 (+0x0c) checked first. With agent.sensors=1 the
 * flags are left as the map has them (the next boot restores the package's). */
static void sensors_keep(u8 *base) {
  static u32 said;
  if (g_opt_sensors)
    return;
  u32 n = 0, rows = 0;
  for (u32 id = 0x55e; id <= 0x5c1; id++) {
    if (id > 0x56a && id < 0x5b7)
      continue;
    u8 *row = item_map_row(base, id, 0);
    if (!row || !writable(row, 0x20) || *(u32 *)(row + 0x0c) != 28)
      continue;
    rows++;
    if (row[0x1e] || row[0x1f]) {
      row[0x1e] = row[0x1f] = 0;
      n++;
    }
  }
  static u32 seen;
  if (rows != seen && said++ < 5)
    LOG("sensors: %u Softness Sensor rows (of 24) found, %u kept out of the shops now (agent.sensors=1 to stock them)",
        rows, n);
  seen = rows;
}
/* The Owner Shop's Special tab offline, both games (asked 2026-11-12: everything obtainable offline
 * by normal play, the game's own Special tab first). The tab (Scarlet 0x4249f0, Fortune 0x423150)
 * lists the items with a current sale record; the records (0x20 bytes: +0 start, +8 end, time_t;
 * +0x10 u16 item id; +0x14 s32 Zack Dollars, -1 tickets only; +0x18 u32 Premium Tickets) are a
 * vector in the online data object (Scarlet *(base+0xc51b08) +0xb8, Fortune *(base+0xc49a08)
 * +0xa0). Offline the game fills it itself, a weekly rotation (Scarlet: 800 records, 310 items,
 * about 20 a week), once the PSN clock answers (h_sceRtcGetCurrentNetworkTick, the console's clock
 * instead). Items no week lists (some group-6 rows, the ported suits and sets, the three Fan☆)
 * are added once after it, on sale always (agent.special=2: every scheduled item too, at its
 * schedule's price, so the tab does not change when a week ends; the tab shows an item once), at their rows' prices (+0x20 Zack Dollars, +0x24
 * tickets): the vector copied into memory from the allocator the game frees this vector with
 * (Scarlet *(base+0x1c1a908): its grow 0x6489c0 and the object's clean-up 0x643767; Fortune
 * +0x1c127e8, 0x643747. Another allocator's block, freed there when the game grew the list,
 * crashed Scarlet: 0x4e7abf, 2026-11-12), with them appended (the old buffer left, as a reader may
 * hold it). Again whenever the game rebuilds the list. */
#define SALE_FOREVER 4102358400ll /* (2099-12-31: what the shop shows as "available until"; a larger end overflows its date) */
/* (scarlet: Scarlet's build in k_scarlet, as its two builds have the same code at 0x19042d; -1 Fortune) */
struct sale_game { u32 code_at; u8 code[7]; u32 objp, vec, map, alloc, tag, files, fill_empty, cap; s32 scarlet; };
static const struct sale_game k_sale[] = {
    {0x19042d, {0x48, 0x8b, 0x90, 0xb8, 0x00, 0x00, 0x00}, 0xc51b08, 0xb8, 0xc4c488, 0x1c1a908, 0x819e1f, 0xc4c5d0, 0, 3000000, 0}, /* Scarlet 1.08 */
    {0x190e8f, {0x49, 0x8b, 0x85, 0xa0, 0x00, 0x00, 0x00}, 0xc49a08, 0xa0, 0xc44388, 0x1c127e8, 0x810d4f, 0xc444d0, 1, 1500000, -1}, /* Fortune 1.19 */
    {0x19042d, {0x48, 0x8b, 0x90, 0xb8, 0x00, 0x00, 0x00}, 0xc51ac8, 0xb8, 0xc4c448, 0x1c1a8c8, 0x81836d, 0xc4c590, 0, 3000000, 1}, /* Scarlet 1.00 */
};
/* Whether the game's file table (Scarlet *(base+0xc4c5d0), Fortune +0xc444d0: an unordered_map by
 * the u32 path hash, node +8 the hash, +0x10 the key; read as the game's own lookup, Scarlet
 * 0x639500, Fortune 0x638be0) has a file. Fortune's item table lists catalog suits whose icons
 * come with their add-on: one without its files in the Special tab crashed the shop (2026-11-12,
 * 0x4f0169: the lookup's 0 read). */
static int file_known(u8 *base, u32 at, u32 key) {
  u8 *m = readable(base + at, 8) ? *(u8 **)(base + at) : 0;
  if (!m || !readable(m, 0x30) || !*(u64 *)(m + 0x10))
    return 0;
  u64 nb = *(u64 *)(m + 0x08), *bk = *(u64 **)(m + 0x28);
  if (!nb || (nb & (nb - 1)) || !bk)
    return 0;
  u64 h = (~(u64)key) + ((u64)key << 21);
  h ^= h >> 24;
  h *= 0x109;
  h ^= h >> 14;
  h *= 0x15;
  h ^= h >> 28;
  h *= 0x80000001ull;
  u64 b = h & (nb - 1);
  if (!readable(bk + b, 8))
    return 0;
  u8 *nd = (u8 *)bk[b];
  if (!nd || !readable(nd, 8))
    return 0;
  for (u32 guard = 0; (nd = *(u8 **)nd) && guard < 4096 && readable(nd, 0x18); guard++) {
    u64 nh = *(u64 *)(nd + 8);
    if (nh == h) {
      if (*(u32 *)(nd + 0x10) == key)
        return 1;
    } else if ((nh & (nb - 1)) != b)
      return 0;
  }
  return 0;
}
static void special_sale_once(void);
/* (only from the game's thread, the clock hook; a try-lock keeps a second caller out) */
static void special_sale(void) {
  static volatile u32 busy;
  if (__atomic_exchange_n(&busy, 1, __ATOMIC_ACQUIRE))
    return;
  special_sale_once();
  __atomic_store_n(&busy, 0, __ATOMIC_RELEASE);
}
static void special_sale_once(void) {
  static u32 said, logged;
  u8 *base = (u8 *)astro_hdr.game_base;
  if (!g_opt_special || !base)
    return;
  const struct sale_game *g = 0;
  const struct scarlet_build *sb = scarlet_of(base);
  for (u32 i = 0; i < sizeof k_sale / sizeof *k_sale; i++)
    if (readable(base + k_sale[i].code_at, 7) && !memcmp(base + k_sale[i].code_at, k_sale[i].code, 7) &&
        (k_sale[i].scarlet < 0 ? !sb : sb == &k_scarlet[k_sale[i].scarlet]))
      g = &k_sale[i];
  static u32 why;
  if (!g) {
    if (!(why & 1) && (why |= 1))
      LOG("special: neither game's sale-list code found (0x19042d / 0x190e8f): off");
    return;
  }
  u8 **objp = (u8 **)(base + g->objp);
  if (!readable(objp, 8) || !*objp || !writable(*objp + g->vec, 0x18)) {
    if (!(why & 2) && (why |= 2))
      LOG("special: no online data object yet (%p)", readable(objp, 8) ? (void *)*objp : 0);
    return;
  }
  u8 **vec = (u8 **)(*objp + g->vec);
  u64 cnt = vec[0] && vec[1] > vec[0] ? (u64)(vec[1] - vec[0]) / 0x20 : 0;
  if (cnt > 20000 || (cnt && !readable(vec[0], cnt * 0x20)))
    return;
  if (!cnt && !g->fill_empty) /* (Scarlet makes its own schedule into an empty list, and makes none into
                                * a list we filled first: seen 2026-11-12. Ours go after its own) */
    return;
  if (!cnt && !(why & 4) && (why |= 4)) /* (Fortune keeps no schedule offline: the list is made of ours alone) */
    LOG("special: the sale list is empty (%p %p %p): the game has no schedule; ours alone", (void *)vec[0],
        (void *)vec[1], (void *)vec[2]);
  for (u64 i = cnt; i-- > 0;) /* (ours are at the end: start 0, end the most) */
    if (*(s64 *)(vec[0] + i * 0x20) == 0 && *(s64 *)(vec[0] + i * 0x20 + 8) == SALE_FOREVER) {
#ifndef RELEASE
      static u32 shown = ~0u;
      if (shown != g_opt_clockhours) { /* (debug: the list's state at the clock's hour, agent.clockhours) */
        struct { s64 sec, nsec; } t = {0, 0};
        if (clock_gettime && clock_gettime(0, (void *)&t) == 0) {
          t.sec += (s64)g_opt_clockhours * 3600;
          u32 cur = 0, ours = 0;
          s64 next = 0;
          for (u64 k = 0; k < cnt; k++) {
            s64 a = *(s64 *)(vec[0] + k * 0x20), e = *(s64 *)(vec[0] + k * 0x20 + 8);
            ours += a == 0 && e == SALE_FOREVER;
            cur += a <= t.sec && t.sec < e;
            s64 c = a > t.sec ? a : e > t.sec ? e : 0;
            if (c && (!next || c < next))
              next = c;
          }
          shown = g_opt_clockhours;
          LOG("special: clock +%uh (%ld): %lu records, %u current (%u ours); next change at %ld", g_opt_clockhours,
              t.sec, cnt, cur, ours, next);
        }
      }
#endif
      return;
    }
  u8 *m = readable(base + g->map, 8) ? *(u8 **)(base + g->map) : 0;
  if (!m || !readable(m, 0x30))
    return;
  u64 nb = *(u64 *)(m + 0x08), *bk = *(u64 **)(m + 0x28);
  if (!nb || nb > 0x100000 || !bk || !readable(bk + nb, 8))
    return;
  /* (nothing to add last time for this very list and item map: not walked again, the clock is
   * called often; a map still empty is not remembered) */
  static u8 *none_for;
  static u64 none_cnt, none_rows;
  static u32 none_valid;
  u64 rows = *(u64 *)(m + 0x10);
  if (none_valid && vec[0] == none_for && cnt == none_cnt && rows == none_rows)
    return;
  static u8 listed[0x800], seen[0x800], added[0x800];
  static s64 rec_e[0x800]; /* (agent.special=2: the record each scheduled item is sold at, its price) */
  static u32 rec_p[0x800], rec_t[0x800];
  memset(listed, 0, sizeof listed);
  memset(seen, 0, sizeof seen);
  memset(added, 0, sizeof added);
  memset(rec_e, 0, sizeof rec_e);
  u32 uniq = 0, now_n = 0;
  struct { s64 sec, nsec; } ts = {0, 0};
  if (!clock_gettime || clock_gettime(0, (void *)&ts) != 0)
    return;
  ts.sec += (s64)g_opt_clockhours * 3600; /* (debug: agent.clockhours) */
  int can_write = writable(vec[0], cnt * 0x20);
  u32 priced = 0;
  for (u64 i = 0; i < cnt; i++) {
    u8 *r = vec[0] + i * 0x20;
    u32 id = *(u16 *)(r + 0x10);
    s64 a = *(s64 *)r, e = *(s64 *)(r + 8);
    int cur = a <= ts.sec && ts.sec < e;
    now_n += cur;
    if (id < 0x800 && !seen[id])
      seen[id] = 1, uniq++;
    /* listed: left to the game's schedule. agent.special=1: an item with a week still to come (or
     * this one). agent.special=2: none: every item is added on sale always (everything at once, as
     * Fortune's list is), the scheduled ones at their schedule's price, so a week ending while the
     * game runs takes none away (seen: Scarlet's tab 375 -> 355 at a week's end, 2026-11-13) */
    if (id < 0x800 && g_opt_special == 1 && e > ts.sec)
      listed[id] = 1;
    if (can_write && *(s32 *)(r + 0x14) == -1 && *(u32 *)(r + 0x18)) { /* tickets only (the sets: the payload lets Scarlet sell them so): Zack Dollars too */
      u64 z = (u64)*(u32 *)(r + 0x18) * 50000;
      *(u32 *)(r + 0x14) = (u32)(z > g->cap ? g->cap : z);
      priced++;
    }
    /* (the price: this week's record, else the one ending last) */
    if (id < 0x800 && rec_e[id] != -1 && (cur || e > rec_e[id]))
      rec_e[id] = cur ? -1 : e, rec_p[id] = *(u32 *)(r + 0x14), rec_t[id] = *(u32 *)(r + 0x18);
  }
  static u16 add[768];
  static u32 addp[768], addt[768];
  u32 n = 0, nofile = 0;
  static u16 nofile_id[64];
  static u32 nofile_h[64][2];
  u8 *nd = (u8 *)bk[nb];
  for (u32 guard = 0; nd && guard < 20000 && readable(nd, 0x84) && n < 768; guard++, nd = *(u8 **)nd) {
    u32 id = *(u16 *)(nd + 0x10);
    u8 *row = nd + 0x14;
    int sched = id < 0x800 && seen[id] && g_opt_special == 2; /* (a scheduled item: agent.special=2) */
    if (id >= 0x800 || listed[id] || added[id] ||
        !(sched || *(u16 *)(row + 0x1c) == 6 || (id >= 0x538 && id <= 0x53a)))
      continue;
    u32 i1 = *(u32 *)(row + 4), i2 = *(u32 *)(row + 8);
    if ((i1 && i1 != 0xffffffffu && !file_known(base, g->files, i1)) ||
        (i2 && i2 != 0xffffffffu && !file_known(base, g->files, i2))) {
      if (nofile < 64)
        nofile_id[nofile] = (u16)id, nofile_h[nofile][0] = i1, nofile_h[nofile][1] = i2;
      nofile++;
      continue;
    }
    added[id] = 1;
    if (sched) {
      u32 zd = rec_p[id], tk = rec_t[id];
      if (zd == 0xffffffffu) /* (a tickets-only record not repriced: the list not writable) */
        zd = (u64)tk * 50000 > g->cap ? g->cap : tk * 50000;
      add[n] = (u16)id, addp[n] = zd, addt[n] = tk, n++;
      continue;
    }
    u32 zd = *(u32 *)(row + 0x20), tk = *(u32 *)(row + 0x24);
    if (tk == 0xffffffffu || tk == 0) /* (no ticket price in the row: the game's ratio, 50,000 a ticket) */
      tk = zd != 0xffffffffu && zd >= 50000 ? zd / 50000 : 1;
    if (zd == 0xffffffffu) /* (tickets only: Premium Tickets need PSN; Zack Dollars at the game's rate, the cap the most) */
      zd = (u64)tk * 50000 > g->cap ? g->cap : tk * 50000;
    add[n] = (u16)id, addp[n] = zd, addt[n] = tk, n++;
  }
  if (logged++ < 4)
    LOG("special: the game's own sale schedule: %lu records (%u items), %u on sale this week; %u items "
        "added for good (%u left out: their icon files are not installed, an add-on's); %u tickets-only records "
        "given Zack Dollars", cnt, uniq, now_n, n, nofile, priced);
  static u32 nf_said;
  if (nofile && !nf_said++)
    for (u32 i = 0; i < nofile && i < 64; i++)
      LOG("special: left out 0x%x (icons %08x %08x)", nofile_id[i], nofile_h[i][0], nofile_h[i][1]);
  if (!n) {
    if (rows)
      none_for = vec[0], none_cnt = cnt, none_rows = rows, none_valid = 1;
    return;
  }
  void **alloc = *(void ***)(base + g->alloc);
  if (!readable(alloc, 8) || !readable(*(void **)alloc, 0xc8))
    return;
  struct { u16 a, b; u32 pad; const char *file; } tag = {0x2c, 0x71, 0, (const char *)(base + g->tag)};
  void *(*fn)(void *, u64, void *) = (void *(*)(void *, u64, void *))(*(void ***)alloc)[0xc0 / 8];
  u64 total = cnt + n;
  u8 *buf = fn(alloc, total * 0x20, &tag);
  if (!buf || !writable(buf, total * 0x20)) {
    LOG("special: no memory from the game's allocator for %lu records", total);
    return;
  }
  if (cnt)
    memcpy(buf, vec[0], cnt * 0x20);
  memset(buf + cnt * 0x20, 0, (u64)n * 0x20);
  for (u32 i = 0; i < n; i++) {
    u8 *r = buf + (cnt + i) * 0x20;
    *(s64 *)r = 0;
    *(s64 *)(r + 8) = SALE_FOREVER;
    *(u16 *)(r + 0x10) = add[i];
    *(u32 *)(r + 0x14) = addp[i];
    *(u32 *)(r + 0x18) = addt[i];
  }
  vec[2] = buf + total * 0x20;
  vec[0] = buf;
  vec[1] = buf + total * 0x20;
  if (said++ < 4) {
    char line[300];
    u32 o = 0;
    for (u32 i = 0; i < n && o < sizeof line - 8; i++)
      o += (u32)agent_snprintf(line + o, sizeof line - o, " %x", add[i]);
    LOG("special: %u items added to the sale list, on sale always:%s", n, line);
  }
}
/* A swimsuit set bought for Zack Dollars (agent.special) lands in the owner's belongings as the set
 * item itself (category 0x1a) but not wrapped (+6 stays 0), so no tab lists it and it cannot be
 * given. The purchase wraps an item (state 2, a gift; Scarlet 0x4050e4 -> 0x18f7e0) only when the
 * paper chooser found a paper for its style and the chosen colour (premium_paper), and a set's style
 * has only Premium Wrapping, which a Zack Dollar purchase does not offer: no paper, no wrap. The game
 * unpacks a set itself when a girl receives it (0x3e1d30 Scarlet, 0x3e1610 Fortune: category 0x1a,
 * its member table 0x3596c0 / 0x359310). So the agent does only the missing step: it wraps the set,
 * with the game's own wrap call (Scarlet 0x18f7e0, Fortune 0x190000: key, slot, the time-of-day
 * value 1-4, the giver, a wrapping paper of category 0xe) in Premium Wrapping, the paper a set
 * bought with Premium Tickets gets. Sets only: a swimsuit is always wrapped by its purchase, in the
 * paper the player picks (seen 2026-11-13). On the game's pad-read thread, every 2 s. */
struct set_game { u32 p, map, wrap, style; }; /* style: the paper style's offset in a row */
/* The paper the game gives an item bought with Premium Tickets: the purchase looks a paper up by the
 * item's paper style (row +0x42) and the chosen colour (+0x44 of the paper rows; 0x190530 Scarlet),
 * falling back to style 5. Colour 11 is Premium Wrapping: only papers 0x393 (style 5) and 0x493
 * (style 6, the sets' style) have it, and style 6 has no other paper, so a set takes no normal colour
 * (the shop says so: "Premium Wrapping will be available for this item if you purchase it with
 * Premium Tickets"; with Zack Dollars the game leaves it unwrapped). Both games' papers are alike;
 * Fortune's rows have the style at +0x32 (its rows lack Scarlet's 16 bytes at +0x28). */
static u32 premium_paper(u8 *base, u32 map, u32 so, u32 style) {
  static const u16 ids[2] = {0x493, 0x393};
  for (u32 i = 0; i < 2; i++) {
    u8 *r = item_map_row_at(base, map, ids[i]);
    if (r && *(u32 *)(r + 0x0c) == 0xe && *(u16 *)(r + so + 2) == 11 &&
        (*(u16 *)(r + so) == style || *(u16 *)(r + so) == 5))
      return ids[i];
  }
  return 0; /* (no Premium Wrapping in this data: the entry left unwrapped) */
}
static const struct set_game k_setg[] = {
    {0xaa9e40, 0xc4c488, 0x18f7e0, 0x42}, /* Scarlet 1.08 */
    {0xaa1de0, 0xc44388, 0x190000, 0x32}, /* Fortune 1.19 */
    {0xaa9e00, 0xc4c448, 0x18f7e0, 0x42}, /* Scarlet 1.00 */
};
static void set_unpack(void) {
  static const u8 wrap_start[10] = {0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53};
  u8 *base = (u8 *)astro_hdr.game_base;
  if (!g_opt_special || !base)
    return;
  u32 cap;
  u8 *gp = owner_global(base, &cap);
  const struct set_game *g = 0;
  for (u32 i = 0; i < sizeof k_setg / sizeof *k_setg; i++)
    if (gp == base + k_setg[i].p)
      g = &k_setg[i];
  if (!g || !readable(base + g->wrap, 10) || memcmp(base + g->wrap, wrap_start, 10))
    return;
  u8 **pp = *(u8 ***)gp;
  if (!pp || !readable(pp, 8) || !*pp || !readable(*pp + 0x30, 8))
    return;
  u8 *head = *(u8 **)(*pp + 0x30), *n = head && readable(head, 0x30) ? *(u8 **)(head + 8) : 0, *bel = 0;
  while (n && readable(n, 0x30) && !n[0x19]) { /* the owner: key 0, the tree's leftmost */
    if (*(u16 *)(n + 0x20) == 0) {
      bel = *(u8 **)(n + 0x28);
      break;
    }
    n = *(u8 **)n;
  }
  if (!bel || !readable(bel + 0x68, 16))
    return;
  u8 *b = *(u8 **)(bel + 0x68), *e = *(u8 **)(bel + 0x70);
  u64 cnt = e > b && (u64)(e - b) < 0x100000 ? (u64)(e - b) / 0x28 : 0;
  if (!cnt || !readable(b, cnt * 0x28))
    return;
  for (u64 i = 0; i < cnt; i++) {
    u8 *en = b + i * 0x28;
    u32 id = *(u16 *)en;
    if (id == 0xffff || *(u16 *)(en + 6) != 0)
      continue;
    u8 *row = item_map_row_at(base, g->map, id);
    if (!row)
      continue;
    u32 cat = *(u32 *)(row + 0x0c);
    if (cat != 0x1a) {
#ifndef RELEASE
      /* (a group-6 swimsuit the owner holds unwrapped: the purchase always wraps one, in a paper
       * the player picks, so only an old build's unpacking left them; logged, left as they are) */
      static u32 g6_said;
      if (cat == 1 && *(u16 *)(row + 0x1c) == 6 && g6_said++ < 8)
        LOG("sets: the owner holds group-6 swimsuit 0x%x unwrapped (slot %lu): left as it is", id, (u64)i);
#endif
      continue;
    }
    { /* (the entry as the purchase left it: which fields it set) */
      static u32 said;
      if (said++ < 4) {
        char hx[0x28 * 3 + 1];
        for (u32 q = 0; q < 0x28; q++)
          agent_snprintf(hx + q * 3, 4, "%02x ", en[q]);
        LOG("sets: unwrapped owner entry %lu: %s", (u64)i, hx);
      }
    }
    u32 paper = *(u16 *)(en + 8);
    u8 *prow = paper != 0xffff ? item_map_row_at(base, g->map, paper) : 0;
    if (!prow || *(u32 *)(prow + 0x0c) != 0xe)
      paper = premium_paper(base, g->map, g->style, *(u16 *)(row + g->style));
    /* (an entry the wrap refuses is tried 3 times, not every 2 s for the session) */
    static struct { u32 slot, id, tries; } failed[8];
    u32 f = 0;
    while (f < 8 && failed[f].tries && !(failed[f].slot == (u32)i && failed[f].id == id))
      f++;
    if (f < 8 && failed[f].tries >= 3)
      continue;
    if (!paper) {
      if (f < 8)
        failed[f].slot = (u32)i, failed[f].id = id, failed[f].tries = 3;
      LOG("sets: the owner's 0x%x left unwrapped: no Premium Wrapping row", id);
      continue;
    }
    ((void (*)(u32, u32, u32, u32, u32))(base + g->wrap))(0, (u32)i, 1, 0, paper);
    if (*(u16 *)(en + 6) == 0 && f < 8)
      failed[f].slot = (u32)i, failed[f].id = id, failed[f].tries++;
    LOG("sets: the owner's 0x%x wrapped in paper 0x%x (a bought set): %s", id, paper,
        *(u16 *)(en + 6) == 2 ? "a gift now" : "NOT wrapped");
  }
}
/* The Owner Shop's Swimsuits tab (tab 1) logged when its stock changes: how many entries, how
 * many of them group 6 (the add-on and ported suits, agent.ownersuits), and their ids. */
static void owner_shop_tab_log(u8 *base, u32 t);
__attribute__((unused)) static void owner_shop_log(u8 *base) {
  for (u32 t = 0; t < 3; t++)
    owner_shop_tab_log(base, t);
}
static void owner_shop_tab_log(u8 *base, u32 t) {
  const struct scarlet_build *sb = scarlet_of(base);
  if (!sb)
    return;
  u8 **pp = *(u8 ***)(base + sb->owner);
  u8 *shop = pp && readable(pp + 0x13, 8) ? (u8 *)pp[0x98 / 8] : 0;
  if (!shop || !readable(shop + 0x20, 8) || !*(u8 **)(shop + 0x20))
    return;
  u8 *tabs = *(u8 **)(shop + 0x20);
  if (!readable(shop + 0x28, 8) || (u64)(*(u8 **)(shop + 0x28) - tabs) < (t + 1) * 0x28u)
    return;
  u8 *tab = tabs + t * 0x28;
  if (!readable(tab, 0x28))
    return;
  u8 *b = *(u8 **)(tab + 0x10), *e = *(u8 **)(tab + 0x18);
  u32 cnt = b && e > b && (u64)(e - b) <= 0x20 * 400 ? (u32)((e - b) / 0x20) : 0;
  if (!cnt || !readable(b, cnt * 0x20))
    return;
  u32 sum = 0, g6 = 0;
  char ids[400 * 6 + 1];
  u32 o = 0;
  for (u32 i = 0; i < cnt; i++) {
    u32 id = *(u16 *)(b + i * 0x20);
    u8 *row = *(u8 **)(b + i * 0x20 + 8);
    sum = sum * 31 + id;
    if (row && readable(row + 0x1c, 2) && *(u16 *)(row + 0x1c) == 6)
      g6++;
    static const char hx[] = "0123456789abcdef";
    if (o + 6 < sizeof ids) {
      ids[o++] = ' ';
      for (int k = 12; k >= 0; k -= 4)
        if ((id >> k) || k == 0)
          ids[o++] = hx[(id >> k) & 15];
    }
  }
  ids[o] = 0;
  static u32 last[3], said;
  static const char *const name[3] = {"Special", "Swimsuits", "Sundries"};
  if (sum != last[t] && said++ < 30)
    LOG("owner shop: %s tab %u entries, %u of them group 6:%s", name[t], cnt, g6, ids);
  last[t] = sum;
}
/* The Switch version's three Fan☆ (Golden 0x538, Silver 0x539, Jet Black 0x53a; Scarlet): rows
 * like the plain fans' (category 17, 18, 24, the same unlock rows: Owner Level 60, 60, 70, the same
 * price) but the Owner Shop flag (+0x1f) 0, so no shop had them. The game's fan code takes them
 * (0x408780: a Golden/Silver Fan or its ☆ sets the wind bits 1-2, the plain one also bit 4;
 * 0x4088d0: the Jet Black Fan or its ☆, the shake). Their flag set to 1, every 2 s, so the Owner
 * Shop's Sundries draw (one fan of category 17|18, one of 24 a day) takes them too, at their
 * levels. Off by default (not the game's own behaviour); agent.starfans=1. */
static void starfans_stock(u8 *base) {
  static const u32 cat[3] = {17, 18, 24};
  static u32 said;
  if (!g_opt_starfans)
    return;
  u32 n = 0, rows = 0;
  for (u32 i = 0; i < 3; i++) {
    u8 *row = item_map_row(base, 0x538 + i, 0);
    if (!row || !writable(row, 0x20) || *(u32 *)(row + 0x0c) != cat[i])
      continue;
    rows++;
    if (!row[0x1f])
      row[0x1f] = 1, n++;
  }
  if (n && said++ < 3)
    LOG("starfans: %u of the 3 Fan☆ rows found, %u stocked in the Owner Shop (agent.starfans=1)", rows, n);
}
/* agent.portgive=1: the 45 suits put in Kasumi's closet (key 2; their models are hers) by the
 * game's own two calls (read 2026-11-11), which take her belongings +0x88 (the closet map) and
 * an id, and insert the id's node when the map lacks it if its row is group 6 (+0x1c), as these
 * are: 0x383210 sets "obtained" (+0x1c), 0x3834d0 "in the closet" (+0x1d). The save's loader
 * (0x3e7969 / 0x3e7987) and the gift path (0x38466b / 0x3846bf) use the same two. Then the
 * owner's copies (agent.giveport's, the "W" entries) emptied (id 0xffff), the suits being
 * Kasumi's instead (asked 2026-11-11). Once, from this thread, while the vacation is loaded. */
static int port_give(u8 *base) {
  const struct scarlet_build *sb = scarlet_of(base);
  if (!sb)
    return 1;
  u8 **pp = *(u8 ***)(base + sb->owner);
  if (!pp || !readable(pp, 8) || !*pp || !readable(*pp + 0x30, 8))
    return 0; /* (the vacation not loaded yet: again in 2 s) */
  u8 *head = *(u8 **)(*pp + 0x30), *n = head && readable(head, 0x30) ? *(u8 **)(head + 8) : 0, *bel = 0;
  while (n && readable(n, 0x30) && !n[0x19]) {
    u32 k = *(u16 *)(n + 0x20);
    if (k == 2) {
      bel = *(u8 **)(n + 0x28);
      break;
    }
    n = *(u8 **)(n + (2 < k ? 0x00 : 0x10));
  }
  if (!bel || !readable(bel + 0x88, 0x18) || !*(u8 **)(bel + 0x88 + 8))
    return 0; /* (her belongings not there yet) */
  typedef void (*closet_fn)(void *, u32);
  closet_fn obtained = (closet_fn)(base + sb->obtained), incloset = (closet_fn)(base + sb->incloset);
  if (*(u32 *)(base + sb->obtained) != 0xe5894855 || *(u32 *)(base + sb->incloset) != 0xe5894855) {
    LOG("port: the closet calls are not where expected: nothing given");
    return 1;
  }
  u32 put = 0, had = 0;
  for (u32 s = 0; s < 5; s++)
    for (u32 v = 0; v < 9; v++) {
      u32 id = k_port_sets[s] + v;
      int was = closet_unlock(base, 2, id); /* (sets both when the node is there) */
      if (was == 0x101) {
        had++;
        continue;
      }
      obtained(bel + 0x88, id);
      incloset(bel + 0x88, id);
      put++;
    }
  u32 ok = 0;
  for (u32 s = 0; s < 5; s++)
    for (u32 v = 0; v < 9; v++)
      ok += closet_unlock(base, 2, k_port_sets[s] + v) == 0x101;
  LOG("port: Kasumi's closet: %u put, %u already there; %u of 45 now obtained and in the closet", put, had,
      ok);
  if (ok != 45)
    return 1; /* (the owner's copies kept) */
  u64 oc = 0;
  u8 *ob = inv_entries(base, 0, &oc);
  u32 gone = 0;
  for (u64 i = 0; ob && i < oc; i++) {
    u8 *en = ob + i * 0x28;
    u32 id = *(u16 *)en, ours = 0;
    for (u32 k = 0; k < 5; k++)
      ours |= id >= k_port_sets[k] && id < k_port_sets[k] + 9u;
    if (!ours)
      continue;
    *(volatile u16 *)en = 0xffff;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    *(u16 *)(en + 6) = 0;
    *(u8 **)(en + 0x10) = 0;
    gone++;
  }
  LOG("port: %u of the suits taken out of the owner's belongings", gone);
  return 1;
}
/* agent.wfix=1: the "W" (equipped: belongings entry +6 == 1; the game's Equip sets the new one
 * and clears the one it replaces, read 2026-11-11) put back to Lily alone on Kasumi (key 2):
 * Scarlet 0x5b6 (written 1 by agent.give, which the game's Equip never cleared) and the 45
 * ported suits 1 -> 0, Lily 0xdd -> 1; the owner's Scarlet 1 -> 0. Once, when loaded. */
static int w_fix(u8 *base) {
  u64 kc = 0, oc = 0;
  u8 *kb = inv_entries(base, 2, &kc), *ob = inv_entries(base, 0, &oc);
  if (!kb || !ob)
    return 0;
  u32 off = 0, on = 0;
  for (u64 i = 0; i < kc; i++) {
    u8 *en = kb + i * 0x28;
    u32 id = *(u16 *)en, ours = id == 0x5b6;
    for (u32 k = 0; k < 5; k++)
      ours |= id >= k_port_sets[k] && id < k_port_sets[k] + 9u;
    if (ours && *(u16 *)(en + 6) == 1)
      *(u16 *)(en + 6) = 0, off++;
    if (id == 0xdd && *(u16 *)(en + 6) == 0)
      *(u16 *)(en + 6) = 1, on++;
  }
  for (u64 i = 0; i < oc; i++) {
    u8 *en = ob + i * 0x28;
    if (*(u16 *)en == 0x5b6 && *(u16 *)(en + 6) == 1)
      *(u16 *)(en + 6) = 0, off++;
  }
  LOG("wfix: %u W marks cleared (Scarlet, the ported suits), Lily's %s", off, on ? "set" : "already set or not found");
  return 1;
}
static void port_watch(void) {
  u8 *base = (u8 *)astro_hdr.game_base;
  const struct scarlet_build *sb = base ? scarlet_of(base) : 0;
  if (!sb)
    return;
  static u32 said_full;
  if (!scarlet_full(sb, "port: the ported rows' shop and closet (agent.portshop, agent.portgive), agent.wfix "
                        "and agent.starfans", &said_full)) {
    sensors_keep(base); /* (by id: rows it does not find are left out) */
#ifndef RELEASE
    owner_shop_log(base);
#endif
    return;
  }
  static u64 last = ~0ull;
  static u32 given;
  u64 n = 0;
  u8 *salmon = item_map_row(base, 0x4f7, &n);
  if (n != last) {
    last = n;
    LOG("port: item map %lu rows (the game's own 1232; with pkg/03-all 1286): Salmon Roe A 0x4f7 %p, "
        "0x2fa %p, Moebius card 0x461 %p", n, (void *)salmon, (void *)item_map_row(base, 0x2fa, 0),
        (void *)item_map_row(base, 0x461, 0));
  }
  static u32 wdone;
  if (g_opt_wfix && !wdone)
    wdone = w_fix(base);
  if (salmon)
    port_shop(base);
  sensors_keep(base);
  starfans_stock(base);
#ifndef RELEASE
  owner_shop_log(base);
#endif
  if (!g_opt_portgive || given || !salmon)
    return;
  given = port_give(base);
}
/* agent.give=1: items put into belongings directly (a slot: +0 id, +2 slot, +6 count 1, +0x10 the
 * item map's row): the Jet Black Fan, the three Fan☆ (Switch items: the PS4 fan code takes them,
 * the user found), Leanan-Sidhe (Ayane, key 5), and with agent.giveport=1 Fortune's 45 suits
 * (rows from the port package) to the owner, from whose belongings the player gives a suit to a
 * girl in the game (as Scarlet went: written into a girl's own belongings it stays locked in her
 * closet). Once, when the belongings are there. */
static void inv_give(void) {
  static u32 done;
  static const struct { u16 key, id; } gifts[] = {
    {0, 0x3be}, /* Jet Black Fan */
    {0, 0x538}, {0, 0x539}, {0, 0x53a}, /* Golden, Silver, Jet Black Fan star */
    {5, 0x5c},  /* Leanan-Sidhe */
    {0, 0x5b6}, /* Scarlet, destructible */
  };
  if ((!g_opt_give && !g_opt_giveport) || done || !astro_hdr.game_base)
    return;
  u8 *base = (u8 *)astro_hdr.game_base;
  static u32 said_full;
  if (!scarlet_full(scarlet_of(base), "give: agent.give, agent.giveport", &said_full))
    return; /* (Scarlet 1.08's code only) */
  u64 kc = 0;
  if (!inv_entries(base, 0, &kc) || !item_map_row(base, 0x3be, 0))
    return; /* (the vacation not loaded yet) */
  done = 1;
  u16 list[64][2];
  u32 n = 0;
  if (g_opt_give)
    for (u32 g = 0; g < sizeof gifts / sizeof gifts[0]; g++)
      list[n][0] = gifts[g].key, list[n][1] = gifts[g].id, n++;
  if (g_opt_giveport)
    for (u32 k = 0; k < 5; k++)
      for (u32 v = 0; v < 9; v++)
        list[n][0] = 0, list[n][1] = (u16)(k_port_sets[k] + v), n++;
  u32 gave = 0, had = 0, failed = 0;
  for (u32 g = 0; g < n; g++) {
    u32 key = list[g][0], id = list[g][1];
    u8 *row = item_map_row(base, id, 0);
    u64 cnt = 0;
    u8 *b = inv_entries(base, key, &cnt);
    if (!row || !b) {
      LOG("give: 0x%x to key %u: %s", id, key, !row ? "not in the item map" : "no belongings");
      failed++;
      continue;
    }
    u32 slot = 0xffffffff, have = 0;
    for (u32 i = 0; i < cnt; i++) {
      u32 eid = *(u16 *)(b + i * 0x28);
      if (eid == id)
        have = 1;
      if (eid == 0xffff && slot == 0xffffffff)
        slot = i;
    }
    if (have) {
      had++;
      continue;
    }
    if (slot == 0xffffffff) {
      LOG("give: 0x%x to key %u: no empty slot", id, key);
      failed++;
      continue;
    }
    u8 *en = b + slot * 0x28;
    *(u16 *)(en + 6) = 1; /* (state 1: in the Swimsuits tab, with the "W" mark; 2 without its other
                           * fields put them in Gifts as blank entries, tried 2026-11-11) */
    *(u8 **)(en + 0x10) = row;
    *(u16 *)(en + 2) = (u16)slot;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    *(u16 *)en = (u16)id;
    LOG("give: 0x%x to key %u in slot %u (row %p)", id, key, slot, (void *)row);
    gave++;
  }
  LOG("give: %u given, %u already had, %u not given", gave, had, failed);
  /* undo of a wrong try (2026-11-11): the owner's suits written with state 2 back to 1 */
  u64 oc = 0;
  u8 *ob = inv_entries(base, 0, &oc);
  u32 fixed = 0;
  for (u64 i = 0; ob && i < oc; i++) {
    u8 *en = ob + i * 0x28;
    u32 id = *(u16 *)en, ours = id == 0x5b6;
    for (u32 k = 0; k < 5; k++)
      ours |= id >= k_port_sets[k] && id < k_port_sets[k] + 9u;
    if (ours && *(u16 *)(en + 6) == 2 && !*(u32 *)(en + 8))
      *(u16 *)(en + 6) = 1, fixed++;
  }
  if (fixed)
    LOG("give: %u of the owner's suits from state 2 back to 1", fixed);
}
/* While drive.py's shake is held: Scarlet's fan thread's state (*(base+0xa9fe80), read by 0x35a330:
 * +0 on, +1 paused, +8 the Jet Black bit, +0x30 and +0x90 armed, +0x34 swings counted, +0x3c
 * fired, +0x70 the filtered motion (x y z), +0x10 the threshold, +0x18 swings needed) logged. */
/* The shake written where the fan thread reads it: the thread's state +0x28 points to the
 * game's controller object, whose +0xb8 is the up-down acceleration (in g, scaled by 113 there).
 * The thread samples about twice per controller read and wants an upward swing two samples after
 * another, so this thread flips the value every millisecond while drive.py's shake is held. */
#ifndef RELEASE /* (drive.py's presses: debug builds only) */
static void *shake_thread(void *arg) {
  (void)arg;
  u32 flip = 0;
  for (;;) {
    u8 *base = (u8 *)astro_hdr.game_base;
    const struct scarlet_build *sb = 0;
    if (!base || !(g_press_buttons & 0x10000000u) || now_us() > g_press_until ||
        !(sb = scarlet_of(base))) { /* (Scarlet's state only) */
      sceKernelUsleep(20000);
      continue;
    }
    u8 *st = *(u8 **)(base + sb->fan);
    u8 **pp = st && readable(st + 0x28, 8) ? *(u8 ***)(st + 0x28) : 0;
    u8 *pad = pp && readable(pp, 8) ? *pp : 0;
    if (pad && writable(pad + 0xb4, 0x78)) {
      float y = (flip++ & 1) ? 8.0f : -8.0f;
      memcpy(pad + 0xb8, &y, 4);
    }
    /* (that alone did not reach it: the thread kept seeing the per-read values. So one swing is
     * kept counted with its window open, +0x34 = 1 and +0x38 = 1: the next upward swing the
     * per-read shake gives is the second, and the fan goes off) */
    if (st && writable(st + 0x34, 8)) {
      *(volatile u32 *)(st + 0x38) = 1; /* (the window held open: strong swings come every 4th sample) */
      if (*(volatile u32 *)(st + 0x34) == 0)
        *(volatile u32 *)(st + 0x34) = 1;
    }
    sceKernelUsleep(1000);
  }
  return 0;
}
#endif
/* agent.fanswings=N (default 0: the game's own 2, left alone): the Jet Black Fan goes off after N
 * strong swings instead of the game's 2. The game wants its 2 within a window of 2 samples (a few hundredths of a second),
 * which a hand does not manage (tried 2026-11-11: the controller shaken hard, nothing); fans were
 * never on in the PS4 game, so this was never tuned. Kept in the state (+0x18) every 2 s. */
static void fan_keep(void) {
  u8 *base = (u8 *)astro_hdr.game_base;
  const struct scarlet_build *sb = base && g_opt_fanswings ? scarlet_of(base) : 0;
  if (!sb)
    return;
  u8 *st = *(u8 **)(base + sb->fan);
  if (!st || !writable(st + 0x18, 4))
    return;
  u32 cur = *(volatile u32 *)(st + 0x18);
  if (cur != g_opt_fanswings && cur >= 1 && cur <= 16) {
    *(volatile u32 *)(st + 0x18) = g_opt_fanswings;
    static u32 said;
    if (said++ < 10)
      LOG("fan: swings needed %u -> %u", cur, g_opt_fanswings);
  }
}
/* agent.ownerlv=N (1..110) and agent.money=N, both games: kept at least that, every 2 s
 * (logs/research/owner-shop/REPORT.md). O = **P: +0x20 Owner Level, +0x1c the EXP into it (zeroed
 * with a raise), *(O+0x08)+0x58 Zack Dollars. P is the global the game's mission reward code loads
 * 70 bytes before its level cap (cmp $0x6e; the same code in both games): Scarlet 1.08 0x3ad379 ->
 * 0xaa9e40 (money cap 3,000,000), Fortune 1.19 0x3ad8b9 -> 0xaa1de0 (money cap 1,500,000), the
 * instruction's own displacement checked against P. A raised level unlocks its items at the next
 * morning's unlock pass (sleep), and the Owner Shop restocks then: in Scarlet fans 60/70, Softness
 * Sensors 20, Skincare EX 110. 0 (default): left alone. */
static u8 *owner_global(u8 *base, u32 *money_cap) {
  static const struct { u32 ins, p, cap; } k[] = {
    {0x3ad379, 0xaa9e40, 3000000}, /* Scarlet 1.08 */
    {0x3ad8b9, 0xaa1de0, 1500000}, /* Fortune 1.19 */
    {0x3accc9, 0xaa9e00, 3000000}, /* Scarlet 1.00 */
  };
  static const u8 lvcap[14] = {0x44, 0x8b, 0x43, 0x20, 0x49, 0x83, 0xf8, 0x6e,
                               0x0f, 0x83, 0xc1, 0x00, 0x00, 0x00};
  for (u32 i = 0; i < sizeof k / sizeof *k; i++) {
    u8 *ins = base + k[i].ins;
    if (!readable(ins, 7 + 70) || ins[0] != 0x48 || ins[1] != 0x8b || ins[2] != 0x05 ||
        k[i].ins + 7 + *(s32 *)(ins + 3) != k[i].p || memcmp(ins + 70, lvcap, sizeof lvcap) != 0)
      continue;
    *money_cap = k[i].cap;
    return base + k[i].p;
  }
  return 0;
}
static void owner_keep(void) {
  u8 *base = (u8 *)astro_hdr.game_base;
  u32 cap = 0;
  u8 *g = (g_opt_ownerlv || g_opt_money) && base ? owner_global(base, &cap) : 0;
  if (!g)
    return;
  u8 **pp = *(u8 ***)g;
  if (!pp || !readable(pp, 8) || !*pp || !writable(*pp + 0x1c, 8))
    return;
  u8 *o = *pp;
  static u32 said;
  u32 lv = *(volatile u32 *)(o + 0x20);
  if (g_opt_ownerlv && lv >= 1 && lv <= 110 && lv < g_opt_ownerlv) {
    *(volatile u32 *)(o + 0x1c) = 0;
    *(volatile u32 *)(o + 0x20) = g_opt_ownerlv;
    if (said++ < 20)
      LOG("owner: level %u -> %u (sleep for the unlocks and the restock)", lv, g_opt_ownerlv);
  }
  u8 *w = readable(o + 0x08, 8) ? *(u8 **)(o + 0x08) : 0;
  if (g_opt_money && w && writable(w + 0x58, 4)) {
    s32 m = *(volatile s32 *)(w + 0x58);
    u32 want = g_opt_money > cap ? cap : g_opt_money;
    if (m >= 0 && (u32)m <= cap && (u32)m < want) {
      *(volatile s32 *)(w + 0x58) = (s32)want;
      if (said++ < 20)
        LOG("owner: Zack Dollars %d -> %u", m, want);
    }
  }
}
/* agent.chars=1: the characters 0x35b510 looks through (*(base+0x94a980), a list by +0x18; +0 the
 * key the slot table at base+0xaa3fe0 gives) logged every 2 s: its ready flags +0x50/+0x51/+0x52,
 * break objects +0x2bc and broken +0x2b8, burst objects +0x2dc and burst +0x2d8. */
static void chars_log(void) {
  u8 *base = (u8 *)astro_hdr.game_base;
  const struct scarlet_build *sb = g_opt_chars && base ? scarlet_of(base) : 0;
  if (!sb)
    return;
  u8 *c = *(u8 **)(base + sb->chars);
  u64 *slots = (u64 *)(base + sb->slots);
  LOG("chars: slots %llx %llx %llx %llx", (unsigned long long)slots[0], (unsigned long long)slots[1],
      (unsigned long long)slots[2], (unsigned long long)slots[3]);
  for (int i = 0; c && i < 16 && readable(c, 0x2e0); i++) {
    LOG("chars: %p key %llx ready %u %u %u break objs %u broken %u burst objs %u burst %u", (void *)c,
        (unsigned long long)*(u64 *)c, c[0x50], c[0x51], c[0x52], c[0x2bc], *(u32 *)(c + 0x2b8), c[0x2dc],
        *(u32 *)(c + 0x2d8));
    u8 *vis = *(u8 **)(c + 0x2a0), *bv = *(u8 **)(c + 0x38);
    if (vis && bv && readable(vis + 150, 8) && readable(bv + 150, 8))
      LOG("chars:   parts 153-156 (Lily: body, body_zbreak, suit, suit_z): shown %u %u %u %u, base %u %u %u %u",
          vis[153], vis[154], vis[155], vis[156], bv[153], bv[154], bv[155], bv[156]);
    c = *(u8 **)(c + 0x18);
  }
}
static void fan_watch(void) {
  static volatile u32 shake_started;
  fan_keep();
  chars_log();
#ifdef RELEASE
  (void)shake_started;
#else
  if (astro_hdr.game_base && (g_press_buttons & 0x10000000u) && once(&shake_started)) {
    u8 attr[64], thr[16];
    scePthreadAttrInit(attr);
    scePthreadAttrSetstacksize(attr, 64 * 1024);
    int rc = scePthreadCreate(thr, attr, shake_thread, 0, "DOAX3VR2 shake");
    scePthreadAttrDestroy(attr);
    LOG("fan: shake thread -> 0x%x", rc);
  }
  if (!astro_hdr.game_base || !(g_press_buttons & 0x10000000u) || now_us() > g_press_until + 600000)
    return;
  u8 *base = (u8 *)astro_hdr.game_base;
  const struct scarlet_build *sb = scarlet_of(base);
  if (!sb)
    return;
  u8 *st = *(u8 **)(base + sb->fan);
  if (!st || !readable(st, 0xa0)) {
    LOG("fan: no state object (%p)", (void *)st);
    return;
  }
  float *m = (float *)(st + 0x70);
  LOG("fan: on %u paused %u jet %u armed %u/%u swings %u fired %u motion %.1f %.1f %.1f thr %.1f need %u gap %u window %u",
      st[0], st[1], st[8], st[0x30], st[0x90], *(u32 *)(st + 0x34), st[0x3c], m[0], m[1], m[2],
      *(float *)(st + 0x10), *(u32 *)(st + 0x18), *(u32 *)(st + 0x20), *(u32 *)(st + 0x1c));
#endif
}
static void malfunction_keep(void) {
  static const struct scarlet_build *sb;
  static int scarlet = -1;
  static u32 said_total;
  if (!g_opt_malfunction || !astro_hdr.game_base)
    return;
  u8 *base = (u8 *)astro_hdr.game_base;
  if (scarlet < 0) {
    /* (1.00: whether its costumes have the break parts, which 1.08 has partly from the port's
     * models, is not known: off) */
    sb = scarlet_of(base);
    scarlet = sb && sb->full;
    LOG("malfunction: %s", scarlet ? "Scarlet 1.08's Item map: the 15 flags kept on"
                           : sb    ? "Scarlet 1.00: off (it needs Scarlet 1.08's data): nothing written"
                                   : "not Scarlet's code: nothing written");
  }
  if (!scarlet)
    return;
  u8 *m = *(u8 *volatile *)(base + sb->labels);
  if (!m || ((u64)m & 7) || !readable(m, 0x30))
    return;
  u64 nb = *(volatile u64 *)(m + 8), count = *(volatile u64 *)(m + 0x10);
  u8 **buckets = *(u8 **volatile *)(m + 0x28);
  if (!count || !nb || nb > 0x100000 || !buckets || !readable(buckets + nb, 8))
    return;
  u32 seen = 0, wrote = 0, nodes = 0, salmon = 0, maxid = 0;
  u8 *n = buckets[nb];
  for (u64 i = 0; n && i < count + 16 && i < 0x10000; i++) {
    if (((u64)n & 7) || !readable(n, 0x20))
      break;
    u32 id = *(volatile u16 *)(n + 0x10);
    nodes++;
    if (id > maxid)
      maxid = id;
    if (id >= 0x4f7 && id <= 0x4ff) /* Salmon Roe (pkg/03-one adds 0x4f7) */
      salmon++;
    if (malfunction_id(id)) {
      u16 *ext = *(u16 **)(n + 0x18);
      if (ext && !((u64)ext & 1) && writable(ext, 4) && ext[0] == 7) {
        seen++;
        if (ext[1] == 0) {
          ext[1] = 1;
          wrote++;
        }
      }
    }
    n = *(u8 **)n;
  }
  static u32 scanned;
  if (g_opt_rowscan && !scanned++)
    rows_scan(); /* (opt-in: on 2026-11-11 the agent's thread never came back from it, after the
                  * hits below were logged; the game had to be closed) */
  static u64 said_count;
  if (count != said_count) { /* each new map: its size and whether the ported rows are in it */
    said_count = count;
    LOG("items: the Item label map holds %u (count %llu), highest id 0x%x, Salmon Roe ids 0x4f7-0x4ff: %u",
        nodes, (unsigned long long)count, maxid, salmon);
  }
  if (wrote && said_total++ < 20)
    LOG("malfunction: %u of the 15 costumes found, %u flags written to 1", seen, wrote);
}

/* ---------------------------------------------------------------- the game's files
 * The game opens and stats each package's /app0/NN/package_data.bin (its item master among the
 * tables) through libkernel directly, at its start, before the agent's first cvars.txt. When
 * /data/doax3vr2/port/NN/package_data.bin exists it is served in its place (agent.port, default 1;
 * logs/research/port-fortune: the item rows Fortune has and Scarlet lacks, added to a package the
 * game already loads). The package files' opens and stats are logged either way. */
#define PORT_DIR "/data/doax3vr2/port/"
/* Also a costume's model (/app0/<pkg>/character/....tmc or .tmcl) when its copy is in the port
 * directory: Fortune's models of the costumes whose break parts Scarlet's lack, made loose files in
 * the served package_data.bin (logs/research/malfunction/breakport.py). */
static int port_model(const char *rest, size_t len) {
  int found = 0;
  for (size_t i = 0; i + 11 <= len && !found; i++)
    found = memcmp(rest + i, "/character/", 11) == 0;
  if (len < 24 || len > 150 || !found)
    return 0;
  return (len > 4 && memcmp(rest + len - 4, ".tmc", 4) == 0) || (len > 5 && memcmp(rest + len - 5, ".tmcl", 5) == 0);
}
/* Each game has its own port directory: Scarlet's /data/doax3vr2/port/ (its package 03 with
 * Fortune's rows, its 00DAT packages with Fortune's models), Fortune's /data/doax3vr2/port-fortune/
 * (its package 15 with the three Fan☆ rows, logs/research/port-fortune/starfans.py). Fortune was
 * served Scarlet's until 2026-11-12 and lost its own package 03 (four catalog designs' icons).
 * Scarlet 1.08 known by its code at 0x5e2d80 (as port_watch), Fortune 1.19 by its sale-list read
 * at 0x190e8f; 0: not known yet, nothing served. Scarlet 1.00: none (the port's packages are 1.08's). */
static const char *port_dir(void) {
  static const char *known;
  if (!known) {
    static const u8 f[7] = {0x49, 0x8b, 0x85, 0xa0, 0x00, 0x00, 0x00};
    u8 *base = (u8 *)astro_hdr.game_base;
    if (!base || !readable(base + 0x5e2d80, 4) || !readable(base + 0x190e8f, 7))
      return 0;
    const struct scarlet_build *sb = scarlet_of(base);
    static u32 said_full;
    known = scarlet_full(sb, "port: agent.port", &said_full) ? PORT_DIR
            : sb                                              ? ""
            : !memcmp(base + 0x190e8f, f, 7)                  ? "/data/doax3vr2/port-fortune/"
                                                              : "";
  }
  return *known ? known : 0;
}
static int port_path(const char *path, char *out, size_t n) {
  const char *dir = port_dir();
  if (!g_opt_port || !path || memcmp(path, "/app0/", 6) != 0 || !dir)
    return 0;
  const char *rest = path + 6;
  size_t len = strlen(rest);
  static const char tail[] = "/package_data.bin";
  if (port_model(rest, len)) {
    agent_snprintf(out, n, "%s%s", dir, rest);
    u8 st[0x80];
    return CALL_ORIG(s32 (*)(const char *, void *), sceKernelStat, out, st) == 0;
  }
  if (len < sizeof tail || len > 64 || memcmp(rest + len - (sizeof tail - 1), tail, sizeof tail - 1) != 0)
    return 0;
  agent_snprintf(out, n, "%s%s", dir, rest);
  return 1;
}
static void port_logpath(const char *what, const char *path, s32 rc, const char *served) {
  static u32 n;
  size_t len = path ? strlen(path) : 0;
  static u32 nc;
  int character = 0;
  for (size_t i = 0; path && i + 11 <= len && !character; i++)
    character = memcmp(path + i, "/character/", 11) == 0;
  if (character && !served)
    LOG_FIRST(nc, 12, "files: %s %s -> 0x%x (a costume's loose file)", what, path, rc);
  if ((len > 17 && memcmp(path, "/app0/", 6) == 0 && memcmp(path + len - 17, "/package_data.bin", 17) == 0) ||
      served)
    LOG_FIRST(n, 160, "files: %s %s -> 0x%x%s%s", what, path, rc, served ? " served from " : "",
              served ? served : "");
}
static s32 h_sceKernelOpen(const char *path, s32 flags, u16 mode) {
  char alt[192];
  if (port_path(path, alt, sizeof alt)) {
    s32 fd = CALL_ORIG(s32 (*)(const char *, s32, u16), sceKernelOpen, alt, flags, mode);
    if (fd >= 0) {
      port_logpath("open", path, fd, alt);
      return fd;
    }
  }
  s32 rc = CALL_ORIG(s32 (*)(const char *, s32, u16), sceKernelOpen, path, flags, mode);
  port_logpath("open", path, rc, 0);
  return rc;
}
/* The game reads its files through Fios (sceFiosFHOpen: attributes, the handle out, the path,
 * the open parameters; it returns an op). Fios may use the path after the call returns, so a
 * served one is kept in a static slot per package. Served only when the file is there. */
static s32 h_sceFiosFHOpen(const void *attr, void *fh, const char *path, const void *params) {
  static char slots[96][192];
  static u32 used;
  char alt[192] = {0};
  if (port_path(path, alt, sizeof alt)) {
    u8 st[0x80];
    if (CALL_ORIG(s32 (*)(const char *, void *), sceKernelStat, alt, st) == 0) {
      const char *keep = 0;
      for (u32 i = 0; i < used && !keep; i++)
        if (memcmp(slots[i], alt, sizeof alt) == 0)
          keep = slots[i];
      if (!keep && used < 96) {
        memcpy(slots[used], alt, sizeof alt);
        keep = slots[used++];
      }
      if (keep) {
        s32 op = CALL_ORIG(s32 (*)(const void *, void *, const char *, const void *), sceFiosFHOpen,
                           attr, fh, keep, params);
        port_logpath("fios open", path, op, keep);
        return op;
      }
    }
  }
  s32 op = CALL_ORIG(s32 (*)(const void *, void *, const char *, const void *), sceFiosFHOpen, attr,
                     fh, path, params);
  port_logpath("fios open", path, op, 0);
  return op;
}
static s32 h_sceKernelStat(const char *path, void *st) {
  char alt[192];
  if (port_path(path, alt, sizeof alt)) {
    s32 rc = CALL_ORIG(s32 (*)(const char *, void *), sceKernelStat, alt, st);
    if (rc == 0) {
      port_logpath("stat", path, rc, alt);
      return rc;
    }
  }
  s32 rc = CALL_ORIG(s32 (*)(const char *, void *), sceKernelStat, path, st);
  port_logpath("stat", path, rc, 0);
  return rc;
}

/* ---------------------------------------------------------------- Dead or Alive Xtreme 3's VR start */
/* The camera must be attached at each VR start (three tries, then the game's VR error) and on
 * every frame in VR mode. */
static s32 h_sceCameraIsAttached(s32 index) {
  static u32 n;
  LOG_FIRST(n, 2, "camera: IsAttached(%d) -> 1", index);
  return 1;
}
static s32 h_sceCameraClose(s32 h) {
  LOG("camera: Close(0x%x)", h);
  return 0;
}
/* Only the byte at +0x10 is read: 0 lets the game's VR intro go on. */
static s32 h_sceVrTrackerGetPlayAreaWarningInfo(u8 *info) {
  static u32 n;
  LOG_FIRST(n, 2, "vrt: GetPlayAreaWarningInfo(%p)", info);
  if (!info)
    return 0;
  info[0x10] = 0;
  return 0;
}
/* The VR service dialog (the system's notice before the first VR start): finished as soon as it
 * is opened; the game stores its result and never reads it. */
static s32 h_sceVrServiceDialogInitialize(void) {
  LOG("vrdialog: Initialize");
  return 0;
}
static s32 h_sceVrServiceDialogOpen(const void *param) {
  LOG("vrdialog: Open(%p): finished at once", param);
  return 0;
}
static s32 h_sceVrServiceDialogUpdateStatus(void) { return 3; /* FINISHED */ }
static s32 h_sceVrServiceDialogGetStatus(void) { return 3; }
static s32 h_sceVrServiceDialogGetResult(void *result) {
  (void)result;
  return 0;
}
static s32 h_sceVrServiceDialogClose(void) { return 0; }
static s32 h_sceVrServiceDialogTerminate(void) {
  LOG("vrdialog: Terminate");
  return 0;
}

/* The system's error dialog. Dead or Alive Xtreme 3 opens it at its title screen when PSN is out of
 * reach (NX-102249-8 on this console) and goes on once it is dismissed; the dialog has the
 * controller then, which an unattended run cannot give it, and a player sees it at every start
 * (the user, 2026-11-10: hidden in normal play too). Not shown unless agent.errdialog=1: the error
 * is logged, and the dialog reported finished. (SceErrorDialogParam: size, errorCode,
 * userId.) */
static volatile u32 g_errdialog_hidden;
static s32 h_sceErrorDialogOpen(const s32 *param) {
  if (g_opt_errdialog == 0) {
    LOG("errdialog: Open(error 0x%x): not shown (agent.errdialog=1 shows it), reported finished",
        param ? (u32)param[1] : 0);
    g_errdialog_hidden = 1;
    return 0;
  }
  s32 rc = CALL_ORIG(s32(*)(const s32 *), sceErrorDialogOpen, param);
  LOG("errdialog: Open(error 0x%x) -> 0x%x", param ? (u32)param[1] : 0, rc);
  return rc;
}
static s32 h_sceErrorDialogUpdateStatus(void) {
  return g_errdialog_hidden ? 3 : CALL_ORIG(s32(*)(void), sceErrorDialogUpdateStatus);
}
static s32 h_sceErrorDialogGetStatus(void) {
  return g_errdialog_hidden ? 3 : CALL_ORIG(s32(*)(void), sceErrorDialogGetStatus);
}
static s32 h_sceErrorDialogClose(void) {
  if (g_errdialog_hidden)
    return 0;
  return CALL_ORIG(s32(*)(void), sceErrorDialogClose);
}
static s32 h_sceErrorDialogTerminate(void) {
  g_errdialog_hidden = 0;
  return CALL_ORIG(s32(*)(void), sceErrorDialogTerminate);
}

/* ---------------------------------------------------------------- sceMsgDialog / sceSaveDataDialog
 * The game opens a MsgDialog for its autosave notice and a SaveDataDialog when creating save data
 * for the first time. Without save data, these block because the system dialog needs the real
 * controller. Suppress them the same way as the error dialog: never shown, reported finished. */
static volatile u32 g_msgdialog_hidden;
static s32 h_sceMsgDialogOpen(const void *param) {
  LOG("msgdialog: Open(%p): not shown, reported finished", param);
  g_msgdialog_hidden = 1;
  return 0;
}
static s32 h_sceMsgDialogUpdateStatus(void) {
  return g_msgdialog_hidden ? 3 : CALL_ORIG(s32(*)(void), sceMsgDialogUpdateStatus);
}
static s32 h_sceMsgDialogGetStatus(void) {
  return g_msgdialog_hidden ? 3 : CALL_ORIG(s32(*)(void), sceMsgDialogGetStatus);
}
static s32 h_sceMsgDialogClose(void) {
  if (g_msgdialog_hidden)
    return 0;
  return CALL_ORIG(s32(*)(void), sceMsgDialogClose);
}
static s32 h_sceMsgDialogTerminate(void) {
  g_msgdialog_hidden = 0;
  return CALL_ORIG(s32(*)(void), sceMsgDialogTerminate);
}

static volatile u32 g_savedatadialog_hidden;
static s32 h_sceSaveDataDialogOpen(const void *param) {
  LOG("savedatadialog: Open(%p): not shown, reported finished", param);
  g_savedatadialog_hidden = 1;
  return 0;
}
static s32 h_sceSaveDataDialogUpdateStatus(void) {
  return g_savedatadialog_hidden ? 3 : CALL_ORIG(s32(*)(void), sceSaveDataDialogUpdateStatus);
}
static s32 h_sceSaveDataDialogGetStatus(void) {
  return g_savedatadialog_hidden ? 3 : CALL_ORIG(s32(*)(void), sceSaveDataDialogGetStatus);
}
static s32 h_sceSaveDataDialogClose(void) {
  if (g_savedatadialog_hidden)
    return 0;
  return CALL_ORIG(s32(*)(void), sceSaveDataDialogClose);
}
static s32 h_sceSaveDataDialogTerminate(void) {
  g_savedatadialog_hidden = 0;
  return CALL_ORIG(s32(*)(void), sceSaveDataDialogTerminate);
}

/* ---------------------------------------------------------------- sysmodule / videoout */
static s32 h_sceSysmoduleLoadModule(u16 id) {
  s32 rc = CALL_ORIG(s32(*)(u16), sceSysmoduleLoadModule, id);
  LOG("sysmodule: LoadModule(0x%x) -> 0x%x", id, rc);
  /* Loading a module re-binds the game's imports from it, over our hooks: have the
   * injector re-point them before the game can call the real functions. */
  __atomic_store_n(&g_shared.repatch, 1, __ATOMIC_RELEASE);
  for (int i = 0; i < 400 && __atomic_load_n(&g_shared.repatch, __ATOMIC_ACQUIRE); i++)
    sceKernelUsleep(5000);
  LOG("sysmodule: re-patch %s", g_shared.repatch ? "timed out" : "done");
  return rc;
}

static s32 h_sceSysmoduleIsLoaded(u16 id) {
  s32 rc = CALL_ORIG(s32(*)(u16), sceSysmoduleIsLoaded, id);
  static u32 n;
  LOG_FIRST(n, 20, "sysmodule: IsLoaded(0x%x) -> 0x%x", id, rc);
  return rc;
}

#define BUS_MAIN 0

static s32 h_sceVideoOutOpen(s32 user, s32 bus, s32 index, const void *param) {
  if (bus == BUS_MAIN && g_main_closed && g_main_port > 0) {
    g_main_closed = 0;
    LOG("videoout: Open(user %d, bus %d, index %d): the main port 0x%x, kept open, again", user, bus,
        index, g_main_port);
    return g_main_port;
  }
  s32 rc = CALL_ORIG(s32(*)(s32, s32, s32, const void *), sceVideoOutOpen, user, bus, index, param);
  LOG("videoout: Open(user %d, bus %d, index %d, %p) -> 0x%x", user, bus, index, param, rc);
  if (bus == BUS_MAIN && rc > 0 && g_main_port < 0)
    g_main_port = rc;
  return rc;
}

/* Dead or Alive Xtreme 3 closes its main port and opens it again each time it enters or leaves
 * its VR mode (buffers unregistered before, registered again after). The port is kept open and the
 * same handle given back: the TV mirror's buffers are registered on it, PS VR2's output may be
 * running, and a main port left without flips in VR mode has brought the console down. The game's
 * own unregister, flip events and register go through as they are. */
static s32 h_sceVideoOutClose(s32 h) {
  if (h == g_main_port && h > 0) {
    g_main_closed = 1;
    LOG("videoout: Close(0x%x): the main port, kept open", h);
    return 0;
  }
  s32 rc = CALL_ORIG(s32(*)(s32), sceVideoOutClose, h);
  LOG("videoout: Close(0x%x) -> 0x%x", h, rc);
  if (h == g_main_port)
    g_main_port = -1;
  return rc;
}

s32 hooks_register_buffers(s32 port, s32 start, void *const *addrs, s32 n, const void *attr) {
  return CALL_ORIG(s32(*)(s32, s32, void *const *, s32, const void *), sceVideoOutRegisterBuffers,
                   port, start, addrs, n, attr);
}

/* ---------------------------------------------------------------- audio */
/* The game's audio ports, and where the system sends them. With PS VR the wearer heard the main
 * port and the TV the AUX port (type 127, a mix for whoever watches), and the BGM port (type 1)
 * went to both. PS VR2 keeps the TV quiet, so the AUX port plays silence and the BGM port opens
 * as a main port, which the system sends to the headset's headphones only. A port's type is in
 * bits 16-23 of its handle. */
#define AUDIO_MAIN 0
#define AUDIO_BGM 1
#define AUDIO_AUX 127
static struct {
  s32 handle, type;
} g_audio[8];
static volatile u32 g_naudio, g_aux_silenced;
/* the longest buffer a port takes: 2048 samples of 8 float channels */
static const u8 g_silence[2048 * 8 * 4];

static int is_aux(s32 h) { return h >= 0 && ((h >> 16) & 0xff) == AUDIO_AUX; }

static s32 h_sceAudioOutOpen(s32 user, s32 type, s32 index, u32 len, u32 freq, u32 param) {
  s32 h = -1;
  if (type == AUDIO_BGM)
    h = CALL_ORIG(s32(*)(s32, s32, s32, u32, u32, u32), sceAudioOutOpen, user, AUDIO_MAIN, index,
                  len, freq, param);
  int moved = h >= 0;
  if (h < 0)
    h = CALL_ORIG(s32(*)(s32, s32, s32, u32, u32, u32), sceAudioOutOpen, user, type, index, len,
                  freq, param);
  LOG("audio: Open(user 0x%x, type %d, len %u, %u Hz, format 0x%x) -> 0x%x%s", user, type, len,
      freq, param, h,
      moved ? " (as a main port: headphones only)"
            : type == AUDIO_AUX && h >= 0 ? " (TV mix: played as silence)" : "");
  u32 n = g_naudio;
  if (h >= 0 && n < 8) {
    g_audio[n].handle = h, g_audio[n].type = type;
    g_naudio = n + 1;
  }
  return h;
}

static s32 h_sceAudioOutOutput(s32 h, const void *buf) {
  if (buf && is_aux(h)) {
    buf = g_silence;
    __atomic_fetch_add(&g_aux_silenced, 1, __ATOMIC_RELAXED);
  }
  return CALL_ORIG(s32(*)(s32, const void *), sceAudioOutOutput, h, buf);
}

/* SceAudioOutOutputParam */
struct audio_out_param {
  s32 handle;
  const void *ptr;
};

static s32 h_sceAudioOutOutputs(const struct audio_out_param *p, u32 num) {
  struct audio_out_param q[25];
  if (p && num <= 25) {
    for (u32 i = 0; i < num; i++) {
      q[i] = p[i];
      if (q[i].ptr && is_aux(q[i].handle)) {
        q[i].ptr = g_silence;
        __atomic_fetch_add(&g_aux_silenced, 1, __ATOMIC_RELAXED);
      }
    }
    p = q;
  }
  return CALL_ORIG(s32(*)(const struct audio_out_param *, u32), sceAudioOutOutputs, p, num);
}

void hooks_log_audio(void) {
  for (u32 i = 0; i < g_naudio; i++) {
    u8 st[32];
    memset(st, 0, sizeof st);
    s32 rc = sceAudioOutGetPortState ? sceAudioOutGetPortState(g_audio[i].handle, st) : -1;
    /* SceAudioOutPortState: output (u16, connected devices), channels, volume, reroute count */
    LOG("audio: port type %d (0x%x): state 0x%x output 0x%x channels %u volume %d reroutes %u",
        g_audio[i].type, g_audio[i].handle, rc, *(u16 *)st, st[2], *(s16 *)(st + 4),
        *(u16 *)(st + 6));
  }
  LOG("audio: %u TV mix buffers played as silence", g_aux_silenced);
}

static s32 h_sceVideoOutSubmitFlip(s32 vo, s32 index, u32 mode, s64 arg) {
  s32 rc = CALL_ORIG(s32(*)(s32, s32, u32, s64), sceVideoOutSubmitFlip, vo, index, mode, arg);
#ifndef RELEASE
  if (vo == g_main_port && index >= 0)
    g_game_flip_index = index;
#endif
  hooks_gpu_timing_start(); /* (this game is flat first: the settings and report thread from its first flip) */

  static u32 n;
  LOG_FIRST(n, 12, "videoout: game flip(port 0x%x, buffer %d, mode %u, arg %ld) -> 0x%x", vo, index,
            mode, arg, rc);
  return rc;
}

static s32 h_sceVideoOutRegisterBuffers(s32 h, s32 start, void *const *addrs, s32 n,
                                        const u32 *attr) {
  s32 rc = CALL_ORIG(s32(*)(s32, s32, void *const *, s32, const u32 *), sceVideoOutRegisterBuffers,
                     h, start, addrs, n, attr);
  LOG("videoout: RegisterBuffers(0x%x, start %d, n %d, %p) -> 0x%x", h, start, n,
      n > 0 ? addrs[0] : 0, rc);
  if (attr)
    LOG("  attr %08x %08x %08x %08x %08x %08x %08x %08x", attr[0], attr[1], attr[2], attr[3],
        attr[4], attr[5], attr[6], attr[7]);
  for (s32 i = 0; i < n && i < 8; i++)
    if (start + i >= 0 && start + i < 16)
      g_vr.vo_buffers[start + i] = addrs[i];
  if (rc >= 0 && h == g_main_port && start == 0) {
    g_vr.vo_count = (u32)n; /* (the mirror's own come after them) */
    if (attr)
      g_game_flip_w = attr[3], g_game_flip_h = attr[4];
    mirror_note_port(h, attr);
  }
  return rc;
}

static s32 h_sceVideoOutSetBufferAttribute(u32 *attr, u32 fmt, u32 tiling, u32 aspect, u32 w,
                                           u32 h, u32 pitch) {
  LOG("videoout: SetBufferAttribute(fmt 0x%x, tiling %u, aspect %u, %ux%u pitch %u)", fmt, tiling,
      aspect, w, h, pitch);
  return CALL_ORIG(s32(*)(u32 *, u32, u32, u32, u32, u32, u32), sceVideoOutSetBufferAttribute, attr,
                   fmt, tiling, aspect, w, h, pitch);
}

static s32 h_sceVideoOutGetDeviceCapabilityInfo_(s32 h, u64 *info) {
  s32 rc = CALL_ORIG(s32(*)(s32, u64 *), sceVideoOutGetDeviceCapabilityInfo_, h, info);
  static u32 n;
  if (rc != 0) /* not filled: nothing added, the failure passed on */
    return rc;
  if (info) {
    LOG_FIRST(n, 4, "videoout: GetDeviceCapabilityInfo(0x%x) -> 0x%x cap 0x%lx, adding VR view", h,
              rc, info[0]);
    info[0] |= 0x20;
  }
  return 0;
}

/* The game asks for the PS VR output mode (119.88 Hz, refresh code 0x0d) on its main port.
 * Passed through, the system sees a PS VR mode with no PS VR attached and suspends the game
 * ("can't continue ... PlayStation VR isn't connected"), so it is only pretended. */
static s32 h_sceVideoOutConfigureOutputMode_(s32 h, u32 a1, const void *mode, const void *opt,
                                             u32 size_mode, u32 size_opt) { /* six arguments */
  const u8 *m = mode;
  if (m && (m[8] == 0x0d || m[8] == 0x23)) { /* 119.88 Hz, or PS VR's 90 Hz */
    LOG("videoout: ConfigureOutputMode(0x%x, %u) PS VR mode, refresh code 0x%x (%s): pretended", h,
        a1, m[8], m[8] == 0x0d ? "119.88 Hz" : "90 Hz");
    g_vr.vr_mode_vo = h;
    g_vr.vr_mode_refresh = m[8];
    return 0;
  }
  s32 rc = CALL_ORIG(s32(*)(s32, u32, const void *, const void *, u32, u32),
                     sceVideoOutConfigureOutputMode_, h, a1, mode, opt, size_mode, size_opt);
  if (m)
    LOG("videoout: ConfigureOutputMode(0x%x, %u) mode %02x %02x %02x %02x %02x %02x %02x %02x "
        "%02x %02x %02x %02x -> 0x%x",
        h, a1, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], rc);
  return rc;
}

static s32 h_sceVideoOutModeSetAny_(void *mode, u32 size) {
  s32 rc = CALL_ORIG(s32(*)(void *, u32), sceVideoOutModeSetAny_, mode, size);
  LOG("videoout: ModeSetAny(%p, %u) -> 0x%x", mode, size, rc);
  return rc;
}

/* A game may size its main port's buffers from the TV's resolution (SceVideoOutResolutionStatus:
 * full width and height, pane width and height, refresh rate): on a 4K output four times 1080p's
 * memory. Over 1920x1080 it is told 1920x1080, as on a 1080p TV: the main port and the memory are
 * then the same whatever the TV is set to, and the system scales the 1080p main port up. */
static s32 h_sceVideoOutGetResolutionStatus(s32 h, u32 *st) {
  s32 rc = CALL_ORIG(s32(*)(s32, u32 *), sceVideoOutGetResolutionStatus, h, st);
  static u32 n;
  u32 real_w = st ? st[0] : 0, real_h = st ? st[1] : 0;
  if (st && rc == 0 && (st[0] > 1920 || st[1] > 1080)) {
    st[0] = st[2] = 1920;
    st[1] = st[3] = 1080;
  }
  if (st && rc == 0 && h == g_vr.vr_mode_vo && g_vr.vr_mode_vo)
    *(u64 *)(st + 4) = g_vr.vr_mode_refresh; /* refreshRate: the PS VR mode the game asked for */
  if (st)
    LOG_FIRST(n, 6, "videoout: GetResolutionStatus(0x%x) -> 0x%x %ux%u (told %ux%u) refresh 0x%x", h,
              rc, real_w, real_h, st[0], st[1], st[4]);
  return rc;
}

/* ---------------------------------------------------------------- the video decoder's GPU queues
 * The tutorial screens play with AvPlayer's Software2 decoder (Videodec2), which maps a GPU compute
 * queue (sceGnmMapComputeQueue, through libSceVdecCore) when a video starts. With the PS5
 * libraries in the game, every new gen1 compute queue is refused (0x80d170ff on all pipes; queues
 * mapped before keep working), so the decoder never started and the screens stayed blue (issue
 * #4). Two of its queues are made before the PS5 libraries load, on pipe 2 (Hmd2's queue is pipe 1
 * queue 0), and lent to AvPlayer in place of the ones it would make. */
#define VQ_N 2
static struct {
  u64 handle;
  volatile u32 used;
} g_vq[VQ_N];
static int vq_alloc(const void *cfg, const void *mem, u64 *out) {
  for (u32 i = 0; i < VQ_N && out; i++)
    if (g_vq[i].handle && !__atomic_exchange_n(&g_vq[i].used, 1, __ATOMIC_ACQ_REL)) {
      *out = g_vq[i].handle;
      return 0;
    }
  int rc = sceVideodec2AllocateComputeQueue(cfg, mem, out);
  LOG("video: no free decoder queue; AvPlayer's own -> 0x%x", rc);
  return rc;
}

static int vq_release(u64 q) {
  for (u32 i = 0; i < VQ_N; i++)
    if (g_vq[i].handle && g_vq[i].handle == q) {
      __atomic_store_n(&g_vq[i].used, 0, __ATOMIC_RELEASE);
      return 0;
    }
  return sceVideodec2ReleaseComputeQueue(q);
}

void hooks_video_queues(void) {
  agent_call(CMD_RESOLVE, 0, 0, 0, 0, 3000, 0, 0); /* the game loaded Videodec2 after the agent */
  if (!sceVideodec2QueryComputeMemoryInfo || !sceVideodec2AllocateComputeQueue ||
      !sceVideodec2ReleaseComputeQueue) {
    LOG("video: libSceVideodec2 isn't loaded: no decoder queues");
    return;
  }
  /* SceVideodec2ComputeMemoryInfo: size of the struct, memory size, memory */
  u64 mi[3] = {0x18, 0, 0};
  int rc = sceVideodec2QueryComputeMemoryInfo(mi);
  u32 made = 0;
  for (u32 i = 0; rc == 0 && i < VQ_N; i++) {
    void *mem = vr2_dalloc_pref((mi[1] + 0x1fffff) & ~(u64)0x1fffff, 0x200000, "a video decoder queue");
    /* SceVideodec2ComputeConfigInfo: size, u16 pipe, u16 queue, checkMemoryType 0 */
    u64 cfg[2] = {0x10, 2 | ((u64)i << 16)}, mem_info[3] = {0x18, mi[1], (u64)mem};
    rc = mem ? sceVideodec2AllocateComputeQueue(cfg, mem_info, &g_vq[i].handle) : -1;
    made += rc == 0;
  }
  LOG("video: %u decoder queues ready on pipe 2 (%lu KB each, 0x%x)", made,
      (unsigned long)(mi[1] >> 10), rc);
}

static void video_queues_lend(void) {
  static volatile u32 done;
  if (!g_vq[0].handle || !once(&done))
    return;
  int m = vr2_find_module("libSceAvPlayer.sprx");
  int a = vr2_override(m, "eD+X2SmxUt4", (void *)vq_alloc);     /* AllocateComputeQueue */
  int r = vr2_override(m, "UvtA3FAiF4Y", (void *)vq_release);   /* ReleaseComputeQueue */
  LOG("video: AvPlayer (%d) gets the decoder queues (%d %d)", m, a, r);
}

/* ---------------------------------------------------------------- libSceAvPlayer (logged)
 * The game plays its tutorial videos with AvPlayer. Passed through; what AvPlayer reports
 * (events, warnings, whether frames come) goes to the log. SceAvPlayerInitData: event
 * replacement (object, callback) at +0x50, 0x78 bytes in all. */
struct avp_watch {
  void *handle, *game_obj;
  void (*game_cb)(void *, s32, s32, void *);
  u32 frames, empty;
  u8 init[0x78];
};
static struct avp_watch g_avp[4];
static u32 g_avp_next;

static struct avp_watch *avp_find(void *h) {
  for (u32 i = 0; i < 4; i++)
    if (h && g_avp[i].handle == h)
      return &g_avp[i];
  return 0;
}

static void avp_event(void *p, s32 event, s32 src, void *data) {
  struct avp_watch *w = p;
  static u32 n;
  if (event == 0x20 && data) /* WarningId */
    LOG_FIRST(n, 60, "avplayer: %p warning 0x%x (stream %d)", w->handle, *(s32 *)data, src);
  else
    LOG_FIRST(n, 60, "avplayer: %p event 0x%x (stream %d)", w->handle, event, src);
  if (w->game_cb)
    w->game_cb(w->game_obj, event, src, data);
}

static void *h_sceAvPlayerInit(const u8 *init) {
  video_queues_lend();
  struct avp_watch *w = &g_avp[__atomic_fetch_add(&g_avp_next, 1, __ATOMIC_RELAXED) % 4];
  const u8 *use = init;
  w->handle = 0, w->frames = w->empty = 0;
  if (init) {
    memcpy(w->init, init, sizeof w->init);
    w->game_obj = *(void *const *)(init + 0x50);
    w->game_cb = *(void *const *)(init + 0x58);
    *(void **)(w->init + 0x50) = w;
    *(void **)(w->init + 0x58) = (void *)avp_event;
    use = w->init;
  }
  void *h = CALL_ORIG(void *(*)(const u8 *), sceAvPlayerInit, use);
  w->handle = h;
  if (init)
    LOG("avplayer: Init(%d frame buffers, auto start %u, own file reader %s) -> %p",
        *(const s32 *)(init + 0x68), init[0x6c], *(void *const *)(init + 0x30) ? "yes" : "no", h);
  return h;
}

static s32 h_sceAvPlayerPostInit(void *h, const void *data) {
  s32 rc = CALL_ORIG(s32(*)(void *, const void *), sceAvPlayerPostInit, h, data);
  LOG("avplayer: %p PostInit -> 0x%x", h, rc);
  return rc;
}

static s32 h_sceAvPlayerAddSource(void *h, const char *path) {
  s32 rc = CALL_ORIG(s32(*)(void *, const char *), sceAvPlayerAddSource, h, path);
  LOG("avplayer: %p AddSource(%s) -> 0x%x", h, path ? path : "(null)", rc);
  return rc;
}

static s32 h_sceAvPlayerStart(void *h) {
  s32 rc = CALL_ORIG(s32(*)(void *), sceAvPlayerStart, h);
  LOG("avplayer: %p Start -> 0x%x", h, rc);
  return rc;
}

static s32 h_sceAvPlayerStop(void *h) {
  s32 rc = CALL_ORIG(s32(*)(void *), sceAvPlayerStop, h);
  struct avp_watch *w = avp_find(h);
  LOG("avplayer: %p Stop -> 0x%x (%u video frames, %u empty polls)", h, rc, w ? w->frames : 0,
      w ? w->empty : 0);
  return rc;
}

static s32 h_sceAvPlayerClose(void *h) {
  struct avp_watch *w = avp_find(h);
  LOG("avplayer: %p Close (%u video frames, %u empty polls)", h, w ? w->frames : 0,
      w ? w->empty : 0);
  s32 rc = CALL_ORIG(s32(*)(void *), sceAvPlayerClose, h);
  if (w)
    w->handle = 0;
  return rc;
}

/* SceAvPlayerFrameInfoEx: data pointer, timestamp at +0x10, video details (width, height) +0x18 */
static u8 h_sceAvPlayerGetVideoDataEx(void *h, u8 *info) {
  u8 ok = CALL_ORIG(u8(*)(void *, u8 *), sceAvPlayerGetVideoDataEx, h, info);
  struct avp_watch *w = avp_find(h);
  if (!w)
    return ok;
  if (ok && w->frames++ == 0 && info)
    LOG("avplayer: %p first video frame %ux%u at %p (after %u empty polls)", h,
        *(u32 *)(info + 0x18), *(u32 *)(info + 0x1c), *(void **)info, w->empty);
  else if (!ok && ++w->empty % 600 == 0 && !w->frames)
    LOG("avplayer: %p no video frame yet (%u polls)", h, w->empty);
  return ok;
}

/* Generic pass-through with logging (integer/pointer arguments only). */
#define PASS(name, force_ok)                                                                     \
  static s64 h_##name(s64 a, s64 b, s64 c, s64 d, s64 e, s64 f) {                               \
    s64 rc = ((s64(*)(s64, s64, s64, s64, s64, s64))orig_##name)(a, b, c, d, e, f);              \
    static u32 n;                                                                                \
    LOG_FIRST(n, 8, #name "(0x%lx, 0x%lx, 0x%lx, 0x%lx) -> 0x%x", a, b, c, d, (u32)rc);         \
    return ((s32)rc < 0 && (force_ok)) ? 0 : rc;                                                 \
  }
PASS(sceAudioInOpen, 0)
/* the game's compute queues (pipe, queue, ...): hand tracking's submits on pipe 5 queue 0 were
 * refused (0x8a6d0103) in play test 3 */
PASS(sceGnmMapComputeQueue, 0)
PASS(sceGnmMapComputeQueueWithPriority, 0)
/* the PS VR reprojection's work buffers, which the game allocates and hands to
 * ReprojectionInitialize (answered without them): the real library's sizes, logged */
PASS(sceHmdReprojectionQueryOnionBuffSize, 0)
PASS(sceHmdReprojectionQueryOnionBuffAlign, 0)
PASS(sceHmdReprojectionQueryGarlicBuffSize, 0)
PASS(sceHmdReprojectionQueryGarlicBuffAlign, 0)
PASS(sceAudioInHqOpen, 0)
PASS(sceVideoOutAddFlipEvent, 0)
PASS(sceSocialScreenInitialize, 1)
PASS(sceSocialScreenSetMode, 1)
PASS(sceSocialScreenInitializeSeparateModeParameter, 1)
PASS(sceSocialScreenConfigureSeparateMode, 1)
PASS(sceSocialScreenOpenSeparateMode, 0)
PASS(sceSocialScreenCloseSeparateMode, 1)

/* ---------------------------------------------------------------- direct memory (observed)
 * Run 6: with the libraries loaded, every direct-memory allocation for PS VR2 failed (32 MB,
 * 1 MB, 96 MB): the game takes its budget. Logged here, unchanged: each allocation, the
 * available size it asks for, each release, and the totals. */
/* what the game is told less is free when it sizes its pools at its start (see
 * h_sceKernelAvailableDirectMemorySize); its CPU pool retries exactly this much all session */
#define DMEM_KEEP (224ull << 20)
static u64 caller_site(u64 *fp);
static volatile u64 g_dmem_taken;
static volatile u32 g_dmem_n;
static s32 h_sceKernelAllocateDirectMemory(s64 start, s64 end, u64 len, u64 align, s32 type,
                                           s64 *phys) {
  /* Once PS VR2 runs, the CPU pool's retry of its 224 MB (type 0) is refused outright: it failed
   * every time by 2 MB only (222 MB free with the palm tracker off), and granted it would take the
   * room the GPU pool's scene-change peaks need (see h_sceKernelAvailableDirectMemorySize); the
   * game copes with it refused, as all along */
  s32 rc = g_reserve.va && type == 0 && len == DMEM_KEEP
               ? (s32)0x80020023
               : CALL_ORIG(s32(*)(s64, s64, u64, u64, s32, s64 *), sceKernelAllocateDirectMemory,
                           start, end, len, align, type, phys);
  /* Step 3 (after Ghost-VR's h_sceKernelAllocateDirectMemory): a block refused for want of memory
   * is asked for again in the container's extra segment below the reserve (256 MB nobody else
   * allocates): to the game, a block like any other. Only once the reserve is in (PS VR2 started) */
  /* Not the CPU pool's retry of exactly what the game was told less (224 MB, type 0): granted (it
   * took 224 of the 256 MB there, 2026-11-04 01:17) it changed nothing the game showed, and at 2000
   * the GPU pool's 74 MB blocks then found no room and the game quit; refused, as all along, the
   * game copes. */
  if (rc == (s32)0x80020023 && g_reserve.spare_hi > g_reserve.spare_lo &&
      !(type == 0 && len == DMEM_KEEP)) {
    s32 rc2 = CALL_ORIG(s32(*)(s64, s64, u64, u64, s32, s64 *), sceKernelAllocateDirectMemory,
                        g_reserve.spare_lo, g_reserve.spare_hi, len, align, type, phys);
    static u32 said;
    if (rc2 == 0 || said++ < 8)
      LOG("dmem: the game's %lu MB (type %d) refused in its range, from the extra segment -> 0x%x at "
          "0x%lx", len >> 20, type, rc2, rc2 == 0 && phys ? *phys : 0);
    if (rc2 == 0)
      rc = 0;
  }
  if (g_reserve.va && type == 3 && len >= (8ull << 20)) {
    /* which of the game's code makes the big GPU blocks once PS VR2 runs (the scene-change peaks):
     * its call site and the one above it in the eboot, and the frame */
    static u32 said;
    if (said++ < 64)
      LOG("dmem: the game's %lu MB GPU block from eboot+0x%lx (caller eboot+0x%lx), frame count %lu "
          "-> 0x%x", len >> 20, (u64)__builtin_return_address(0) - astro_hdr.game_base,
          caller_site((u64 *)__builtin_frame_address(0)), hooks_frame_count(), rc);
  }
  u32 k = __atomic_fetch_add(&g_dmem_n, 1, __ATOMIC_RELAXED);
  int retry = rc != 0 && type == 0 && len == DMEM_KEEP; /* the game's known CPU-pool retry */
  if (rc == 0)
    g_dmem_taken += len;
  else if (!retry)
    g_rec_dmem_refused++;
  if (retry && __atomic_fetch_add(&g_pool_retries, 1, __ATOMIC_RELAXED) >= 3)
    return rc; /* logged three times, then counted (health_report): ~5 a second before */
  if (k < 60 || len >= (16u << 20))
    LOG("dmem: game Allocate(0x%lx-0x%lx, %lu MB 0x%lx, align 0x%lx, type %d) -> 0x%x at 0x%lx; "
        "the game holds %lu MB", start, end, len >> 20, len, align, type, rc,
        rc == 0 && phys ? *phys : 0, g_dmem_taken >> 20);
  return rc;
}
/* Run 7: the game takes 8, 256, 3624 (its GPU heap) and 200 MB, then sizes its last pool from
 * this call: what is free minus 1 MB (997 MB), leaving PS VR2 1 MB. It is told DMEM_KEEP less is
 * free (its fixed allocations still fit with gigabytes to spare), so its last pool comes out
 * DMEM_KEEP smaller and PS VR2 gets that much (it took 155 MB in run 8: the tracker 32, the
 * reprojection's display buffers 96, the TV mirror 25, small buffers 2). With 256 MB kept the
 * game ran short of its pool and retried a 256 MB allocation 4 times a second (run 8). */
static s32 h_sceKernelAvailableDirectMemorySize(s64 start, s64 end, u64 align, s64 *phys,
                                                u64 *avail) {
  s32 rc = CALL_ORIG(s32(*)(s64, s64, u64, s64 *, u64 *), sceKernelAvailableDirectMemorySize,
                     start, end, align, phys, avail);
  u64 real = avail ? *avail : 0;
  if (rc == 0 && avail && real > 2 * DMEM_KEEP)
    *avail = real - DMEM_KEEP;
  else if (rc == 0 && avail && g_reserve.va && real > (1ull << 20))
    /* Once PS VR2 runs (its buffers in the reserve), what is free in the game's range is the
     * room its GPU pool's peaks need at a scene change (74 MB blocks, released after). Told the
     * truth, the game's CPU pool took it all for good right after the first peak ("free minus
     * 1 MB": 159 MB, manual2, 2026-11-04 01:30), every later peak went to the spare, and after 23
     * minutes at 2048 a block was refused and the game aborted. Told 1 MB it takes nothing (the
     * same log: asked with 1 and 12 MB free, nothing allocated); the GPU pool allocates without
     * asking and still gets the room. */
    *avail = 1ull << 20;
  static u32 n;
  LOG_FIRST(n, 20, "dmem: game AvailableDirectMemorySize(0x%lx-0x%lx, align 0x%lx) -> 0x%x: %lu MB "
            "at 0x%lx (told %lu MB: %lu kept for PS VR2)", start, end, align, rc, real >> 20,
            phys ? *phys : 0, avail ? *avail >> 20 : 0, (real - (avail ? *avail : 0)) >> 20);
  return rc;
}
static s32 h_sceKernelReleaseDirectMemory(s64 start, u64 len) {
  s32 rc = CALL_ORIG(s32(*)(s64, u64), sceKernelReleaseDirectMemory, start, len);
  if (rc == 0)
    g_dmem_taken -= len;
  static u32 n;
  LOG_FIRST(n, 30, "dmem: game Release(0x%lx, %lu KB) -> 0x%x; the game holds %lu MB", start,
            len >> 10, rc, g_dmem_taken >> 20);
  return rc;
}
/* Probe only (2026-11-04, after Ghost-VR's vr2.c reserve_hidden): a PS4 game's direct-memory
 * container on PS5 is said to hold 512 MB more than the game is told, at offsets of their own
 * (0x4000000000 + its size; "SISR DMEM" in the launch log), which nothing allocates. Asked with the
 * call the agent already makes for the game's own range (the largest free piece in a range):
 * nothing is allocated, nothing changes. */
void hooks_probe_hidden_dmem(const char *when) {
  if (!sceKernelAvailableDirectMemorySize)
    return;
  u64 total = (u64)sceKernelGetDirectMemorySize();
  s64 base = 0x4000000000ll + (s64)total, top = base + 0x20000000ll;
  s64 at = 0, at_all = 0;
  u64 avail = 0, avail_all = 0;
  s32 rc = sceKernelAvailableDirectMemorySize(base, top, 0x200000, &at, &avail);
  s32 rc_all = sceKernelAvailableDirectMemorySize(0, top, 0x200000, &at_all, &avail_all);
  LOG("dmem probe %s: %lu MB direct memory; the extra segment 0x%lx-0x%lx: 0x%x, %lu MB free in one "
      "piece at 0x%lx; up to its top: 0x%x, %lu MB free in one piece at 0x%lx", when, total >> 20,
      base, top, rc, avail >> 20, at, rc_all, avail_all >> 20, at_all);
}

void hooks_log_dmem(const char *when) {
  u64 avail = 0;
  s64 at = 0;
  s32 rc = sceKernelAvailableDirectMemorySize
               ? sceKernelAvailableDirectMemorySize(0, (s64)sceKernelGetDirectMemorySize(), 0x10000,
                                                    &at, &avail)
               : -1;
  LOG("dmem %s: %lu MB in all, the game holds %lu MB in %u allocations, %lu MB free in one piece "
      "(0x%x)", when, (u64)sceKernelGetDirectMemorySize() >> 20, g_dmem_taken >> 20, g_dmem_n,
      avail >> 20, rc);
}

/* ---------------------------------------------------------------- the game's waits
 * Its condition waits, timed sleeps and event-flag waits, hooked for agent.rtime's samples
 * (rtime.c) and passed through. */
/* the caller of the game function a hook was called from (its frame's return address; the dmem
 * log's big GPU blocks): the hook's frame pointer is passed in; the game's saved one is followed only if it is 8-byte
 * aligned, just above on the stack and, asked of the system first (sceKernelVirtualQuery, a
 * query), in readable memory: a function without a frame pointer leaves anything there */
static int readable(const void *p, u64 n) {
  u8 info[128];
  memset(info, 0, sizeof info);
  /* SceKernelVirtualQueryInfo: start, end, offset, protection, type, flags, name[32]: 0x48 */
  if (!sceKernelVirtualQuery || sceKernelVirtualQuery(p, 0, info, 0x48) != 0)
    return 0;
  u64 start = *(u64 *)info, end = *(u64 *)(info + 8);
  s32 prot = *(s32 *)(info + 24);
  return start <= (u64)p && (u64)p + n <= end && (prot & 1);
}
static int writable(const void *p, u64 n) {
  u8 info[128];
  memset(info, 0, sizeof info);
  if (!sceKernelVirtualQuery || sceKernelVirtualQuery(p, 0, info, 0x48) != 0)
    return 0;
  u64 start = *(u64 *)info, end = *(u64 *)(info + 8);
  s32 prot = *(s32 *)(info + 24);
  return start <= (u64)p && (u64)p + n <= end && (prot & 3) == 3;
}
static u64 caller_site(u64 *fp) {
  if (!fp || !readable(fp, 16))
    return 0;
  u64 *up = (u64 *)fp[0];
  if (!up || ((u64)up & 7) || up <= fp || (u64)up - (u64)fp > 0x100000 || !readable(up, 16))
    return 0;
  return up[1] - astro_hdr.game_base;
}
#define WAITHOOK(name, label)                                                                    \
  static s64 h_##name(s64 a, s64 b, s64 c, s64 d, s64 e, s64 f) {                               \
    rtime_note(RT_WAIT, 0, 0); /* (agent.rtime=1: the thread's CPU time; nothing when off) */  \
    return ((s64(*)(s64, s64, s64, s64, s64, s64))orig_##name)(a, b, c, d, e, f);                \
  }
WAITHOOK(nanosleep, "nanosleep")
WAITHOOK(pthread_cond_wait, "pthread_cond_wait")
WAITHOOK(pthread_cond_timedwait, "pthread_cond_timedwait")
/* (sceKernelWaitEventFlag: with tick_frame, it also puts the game speed back after agent.tickhold) */

/* ---------------------------------------------------------------- modules the game loads */
static s32 h_sceKernelLoadStartModule(const char *path, u64 argc, const void *argv, u32 flags,
                                      void *opt, s32 *res) {
  u64 t0 = now_us();
  s32 h = CALL_ORIG(s32(*)(const char *, u64, const void *, u32, void *, s32 *),
                    sceKernelLoadStartModule, path, argc, argv, flags, opt, res);
  LOG("kernel: the game loads %s -> 0x%x in %lu ms", path ? path : "?", h, (now_us() - t0) / 1000);
  if (h >= 0) { /* its imports may re-bind ours: the injector re-points them, as for sysmodule */
    __atomic_store_n(&g_shared.repatch, 1, __ATOMIC_RELEASE);
    for (int i = 0; i < 400 && __atomic_load_n(&g_shared.repatch, __ATOMIC_ACQUIRE); i++)
      sceKernelUsleep(5000);
  }
  return h;
}

/* ---------------------------------------------------------------- why the game ends
 * (after Ghost-VR's exitlog.c) The game's calls to abort, exit and _Exit (its main program imports
 * these three) are logged with their callers (module and offset, from the frame-pointer chain)
 * and the last of what it printed through puts, before they go on; then a pause, so the payload
 * (which reads the log every 20 ms) has the lines before the process is gone. printf and fprintf
 * are not hooked: their arguments vary, and the game imports no vprintf to pass them on with. */
#define OUT_TAIL 4096
static char g_out_tail[OUT_TAIL];
static volatile u64 g_out_w, g_out_logged;

static void log_text(const char *tag, const char *p, u64 n) {
  char line[200];
  while (n) {
    u64 k = 0;
    while (k < n && k < sizeof line - 1 && p[k] != '\n')
      line[k] = p[k], k++;
    line[k] = 0;
    if (k)
      LOG("%s%s", tag, line);
    p += k, n -= k;
    if (n && *p == '\n')
      p++, n--;
  }
}

static s32 h_puts(const char *s) {
  if (s) {
    u64 n = strlen(s), w = __atomic_fetch_add(&g_out_w, n + 1, __ATOMIC_RELAXED);
    for (u64 i = 0; i < n; i++)
      g_out_tail[(w + i) % OUT_TAIL] = s[i];
    g_out_tail[(w + n) % OUT_TAIL] = '\n';
    if (__atomic_fetch_add(&g_out_logged, n, __ATOMIC_RELAXED) < 16384)
      log_text("out: ", s, n);
  }
  return CALL_ORIG(s32(*)(const char *), puts, s);
}

static void last_words(const char *what, s64 code, u64 from) {
  const char *mod;
  u64 base = vr2_module_of(from, &mod);
  LOG("exit: %s(%ld) called from %s+0x%lx", what, (long)code, mod, (unsigned long)(from - base));
  /* the callers: each frame holds the caller's frame pointer, then the return address (frames
   * looked at only up the stack, 8-byte aligned, within 256 KB of this one, and in readable memory:
   * a function without a frame pointer leaves anything there) */
  u64 *fp = (u64 *)__builtin_frame_address(0), *top = fp;
  for (int i = 0; i < 12 && fp && !((u64)fp & 7) && (u64)fp - (u64)top < 0x40000 && readable(fp, 16);
       i++) {
    u64 ret = fp[1];
    if (ret < 0x10000)
      break;
    base = vr2_module_of(ret, &mod);
    LOG("exit:   #%d %s+0x%lx", i, mod, (unsigned long)(ret - base));
    if ((u64 *)fp[0] <= fp)
      break;
    fp = (u64 *)fp[0];
  }
  u64 w = g_out_w, n = w < OUT_TAIL ? w : OUT_TAIL;
  static char tail[OUT_TAIL];
  for (u64 i = 0; i < n; i++)
    tail[i] = g_out_tail[(w - n + i) % OUT_TAIL];
  if (n)
    LOG("exit: the game's last %lu bytes through puts:", (unsigned long)n);
  log_text("exit| ", tail, n);
  sceKernelUsleep(150000);
}

static void h_abort(void) {
  last_words("abort", 0, (u64)__builtin_return_address(0));
  CALL_ORIG(void (*)(void), abort);
}
static void h_exit(s32 code) {
  last_words("exit", code, (u64)__builtin_return_address(0));
  CALL_ORIG(void (*)(s32), exit, code);
}
static void h__Exit(s32 code) {
  last_words("_Exit", code, (u64)__builtin_return_address(0));
  CALL_ORIG(void (*)(s32), _Exit, code);
}

/* ---------------------------------------------------------------- table */
#define H(name) {HNID_##name, (void *)h_##name, &orig_##name, 0},
struct agent_hook g_hooks[] = {
#include "hooks.inc"
    {0, 0, 0, 0},
};
#undef H

/* Called by the injector on a hijacked game thread right after the hooks go live. */
u64 agent_boot(u64 arg) {
  (void)arg; /* (the payload passes 0) */
  /* the headset's own eye distance, as measured in an earlier session (vr2.c), for the game's
   * eye offsets: the game asks for them once, before PS VR2 is up */
  extern struct agent_header astro_hdr;
  if (astro_hdr.ipd_um >= 50000 && astro_hdr.ipd_um <= 80000)
    g_vr.ipd = (float)astro_hdr.ipd_um * 1e-6f;
  LOG("agent: boot (user %d, eq %p, vo 0x%x, eye distance %f mm)", g_vr.user_id, g_vr.end_eq,
      g_vr.display_vo, g_vr.ipd * 1000.0f);
  /* the handshake for the headset cameras' state (common/camera.h): the injector writes the
   * block only once it reads these back, so an older agent (none there) is never written over */
  g_shared.cam.version = CAM_STATE_VERSION;
  g_shared.cam.size = sizeof g_shared.cam;
  hooks_probe_hidden_dmem("at the game's start");
  agent_tick();
  return 0;
}
