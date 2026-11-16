/* PS VR2 inside the PS4 game: shared by vr2.c (output and tracking), gen1.c (running the PS5
 * GPU work in a PS4-mode GPU context) and ps5compat.c (making the PS5 libraries work in a PS4
 * process).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "vr.h"

/* the four libraries, in load order */
enum { LIB_AGCDRIVER, LIB_AGC, LIB_HMD2, LIB_VRTRACKER2, NLIBS };

/* helpers (vr2.c) */
int vr2_override(int handle, const char *nid, void *fn);
int vr2_find_module(const char *name);
u64 vr2_module_base(int handle);
u64 vr2_module_of(u64 addr, const char **name); /* the module an address is in: its base, its name */
void *vr2_dalloc(size_t size, size_t align);
void *vr2_dalloc_type(size_t size, size_t align, int type);
void *vr2_dalloc_range(size_t size, size_t align, s64 lo, s64 hi);
/* the reserve in the container's extra segment (vr2.c; step 1: allocated and checked, unused) */
struct vr2_reserve {
  u8 *va;
  s64 phys;
  u64 size, used;
  s64 spare_lo, spare_hi; /* step 3: the segment below the reserve, for the game's refused blocks */
};
extern struct vr2_reserve g_reserve;
void vr2_reserve_init(void);
void *vr2_ralloc(size_t size, size_t align); /* a piece of the reserve, cleared; 0: none */
void *vr2_dalloc_pref(size_t size, size_t align, const char *what); /* the reserve first */
/* the VR port's buffers (vr2.c): the game's eye texture pairs, up to MAX_PAIRS */
#define MAX_PAIRS 12 /* (the VR port has 16 buffers; compose.c adds its four output pairs) */

/* gen1.c */
void gen1_install(const int handles[NLIBS]);
int gen1_upload_shader(void);
void gen1_note_game_tsharp(const u32 *t);
int gen1_queue_ready(void);
void gen1_flip_stats(u32 *late, u32 *gap_max);
int gen1_hmd_out(u32 *tsharp); /* the headset's last finished picture (agent.tvhmd) */
u32 gen1_refreshes(void);
u64 gen1_last_flip(void); /* when Hmd2 last flipped the headset (us) */
u32 gen1_last_built(u32 *frame); /* Hmd2's builds so far, and the frame the newest took */
/* hooks.c: the hand-off (vr2.c) passed a frame too late for its refresh (counted; nothing
 * consumes it) */
extern volatile u32 g_late_pass, g_release_n;
int gen1_submit_acb(const u32 *cb, u32 ndw);
extern volatile u32 g_display_pitch; /* set by ps5compat.c for the linear headset buffers */

/* ps5compat.c */
void ps5compat_install(const int handles[NLIBS]);
void ps5compat_mark_thread(void);
void ps5compat_allow_vr_layout(void);
void ps5compat_install_hand(int handle);

/* hooks.c */
void hooks_wake(void);         /* the vblank thread woken: Hmd2 flipped */
void hooks_flip(u32 flip_arg); /* Hmd2 flipped, showing this frame of vr2.c's */
int hooks_flat_poll(void);     /* hooks.c: out of VR, the TV's new picture to the headset (1 = one sent) */
void vr2_frame_ready(void);    /* vr2.c: the game handed a frame over */
void hooks_note_unpassed(u32 n, u32 why); /* hooks.c: frames that will never be shown (the exact count;
                                          * why: 1 the newest passed, 2 gone from the ring, 3 overwritten,
                                          * 4 a flip skipped a passed frame) */

/* hand.c: PS VR2 hand tracking (libSceVrHand) */
void hand_load(void);  /* load_stack: loaded, stubbed and overridden, not started */
void hand_start(void); /* hand_palm() is in vr.h */
int hand_clear_why(u8 *st, int *src); /* 0: worn, no system screen up (st: 512 bytes) */
extern volatile u32 g_hand_settled; /* the palm tracker's start is over (or won't happen) */
/* hand.c: the one camera start at a time, the palm tracker's (CAMERA_START_PALM) or eye tracking's
 * (CAMERA_START_GAZE): take it (taken, or busy), give it back (the owner only; started: a camera
 * start really happened, which keeps the other kind off for 5 s: camera_start_recent) */
enum { CAMERA_START_PALM = 1, CAMERA_START_GAZE = 2 };
int camera_start_take(u32 who);
void camera_start_give(u32 who, int started);
int camera_start_recent(u32 who); /* held by the other, or the other's start given back less than 5 s ago */
extern volatile u32 g_resume_n; /* hooks.c: the system's resume events (0x10000000) seen */
extern volatile u64 g_hand_lib_camera_at; /* ps5compat.c: the last [VrHand][CameraManager]/[ImageLock] line */
/* a PM4 type-3 draw opcode (gfx7/8; 0x2e is SI's DRAW_INDEX_IMMD) */
static inline int pm4_is_draw(u32 op) {
  switch (op) {
  case 0x24: case 0x25: case 0x27: case 0x2c: case 0x2d: case 0x2e: case 0x30: case 0x35: case 0x38:
    return 1;
  }
  return 0;
}
/* hooks.c: when the game's controller was last seen held by a system screen (0: never), and a
 * reading of it now, beside the game's (1 held, 0 not, -1 unknown) */
extern volatile u64 g_pad_intercepted_at;
int hooks_pad_held(void);

/* fov.c: PS VR2's eye tracking (agent.gaze) */
int fov_option(const char *key, u32 klen, const char *val, u32 vlen); /* 1: agent.gaze's line */
/* rtime.c (agent.rtime=1): a game thread's CPU time sampled at a hooked call of this kind */
enum { RT_SUBMIT = 1, RT_HANDOVER = 2, RT_PAD = 4, RT_WAIT = 8 };
void rtime_note(u32 kind, u32 n, const u32 *bytes);
int fov_wants_gaze(void);
void fov_gaze_start(int hmd, int after_hands); /* after_hands: wait for g_hand_settled */
int fov_gaze(float t[2]); /* the newest gaze as tangents (x right, y up), 1 while fresh */
/* gaze.c: the near view moved off the eye's axis (agent.gazefollow, agent.movehole: on by default;
 * agent.nearoffset: debug) */
int gaze_option(const char *key, u32 klen, const char *val, u32 vlen); /* 1: one of its lines */
void gaze_set_follow(u32 on, const char *from); /* agent.gazefollow (cvars.txt, the menu's file) */
int gaze_follows(void);
void gaze_frame(const u8 *ret); /* the headset's sceVrTrackerGetResult, its return address */
void gaze_handover(const void *frame_label); /* the wide/near hand-over, before compose_frame */
int gaze_compose_centre(float c[2]); /* compose.c: that frame's near centre (x right, y up), 1 if off 0 */
u64 hooks_drawn_label(void); /* the game's newest drawn label (hooks.c; 0: this game has none) */
/* the game index tick_frame (hooks.c) hands on, g: 0 Scarlet 1.08, 1 Fortune 1.19, 2 Scarlet 1.00
 * (CUSA14831), the order of height.c's k_games and hooks.c's k_tick */
#define N_GAMES 3
static inline const char *game_name(int g) {
  static const char *const n[N_GAMES] = {"Scarlet 1.08", "Fortune 1.19", "Scarlet 1.00"};
  return g >= 0 && g < N_GAMES ? n[g] : "?";
}
/* touch.c: the VR controller's and the head's two collision spheres each made as the game would
 * (agent.touch, on by default, both builds: the settings menu's "Touch*" with agent.touchhead;
 * agent.touchhead=0 leaves the head's out) */
extern u32 g_opt_touch, g_opt_touchhead;
void touch_tick(u8 *base, int g); /* the game's main thread, each loop (tick_frame); g as above */
void touch_report(void);          /* the agent's cvars thread: its 10 s line when due */
/* vrreact.c: agent.lookat (the dead "look at the player" call made when the close-contact count
 * rises; on by default, both builds: the settings menu's "Look at you", live), agent.vrtimers (the per-loop timers held on no-tick loops at 120 fps; on by
 * default, both builds), agent.uihold (the menu/message layer skipped on no-tick loops, with the
 * payload's jump; debug) and agent.rateprobe (the per-loop timers' rates logged; debug) */
extern u32 g_opt_lookat, g_opt_vrtimers, g_opt_uihold, g_opt_rateprobe;
extern volatile u32 g_opt_sharpen; /* compose.c: agent.sharpen (the settings menu's Sharpening, live) */
void uihold_tick(u8 *base, int g, int none);                  /* tick_frame, every loop */
void rateprobe_tick(u8 *base, int g, int none, s32 ticks);    /* tick_frame, every loop */
void lookat_tick(u8 *base, int g);     /* the game's main thread, each loop (tick_frame) */
void vrtimers_begin(u8 *base, int g);  /* tick_frame, a loop with no tick (frame_pace 1) */
void vrtimers_end(int late);           /* after that loop's update (its loop-top wait, or late) */
u32 vrtimers_pending(void);            /* 0, or the game + 1 while a loop's hold is to be undone */
void vrreact_window(void);             /* tick_frame: the 10 s counts handed over */
void vrreact_report(void);             /* the agent's cvars thread: the lines due */
/* viewsize.c: the VR views' render sizes (agent.widew, agent.mode0, agent.nearn; the payload's, debug) */
int viewsize_option(const char *key, u32 klen, const char *val, u32 vlen); /* 1: one of its lines */
void viewsize_report(void); /* the agent's cvars thread: the sizes built and the per-view switch's 10 s line */
float viewsize_near_factor(void); /* n / tan_top: agent.nearn / 100, else the game's 0.5 (gaze.c) */
/* menu.c, the menu (the settings menu in a release build, the debug menu in a debug one) */
#define MENU_LW 256u /* its picture as drawn: 32 characters of 8 */
#define MENU_LH 160u /* (11 lines and the "*" note; longer menus scroll) */
#define MENU_SCALE 3u /* enlarged so in the GPU's copy: DOAX's composed eye is 3360 wide (about 1400
                       * pixels a unit of tangent), where 1:1 was too small to read */
#define MENU_W (MENU_LW * MENU_SCALE)
#define MENU_H (MENU_LH * MENU_SCALE)
void menu_pad(u8 *sample);  /* the player's controller sample: L1+R1+touchpad opens and closes it */
const u8 *menu_picture(void); /* its picture (RGBA, MENU_W x MENU_H, GPU-readable) while open, else 0 */
/* the settings menu's file (both builds: the debug menu has the settings menu's lines): hooks.c calls them */
void menu_cvar(const char *key, u32 klen, u32 v); /* cvars_fetch: a payload option a start-only line holds */
void menu_boot(void);        /* at the end of cvars_fetch: the file fetched, its agent.* lines applied */
void menu_save(void);        /* the cvars thread, every 2 s: the file written when a choice changed */
/* inset.c: the menu's picture copied into the frame's eyes (the shader's stage 2) */
void inset_menu_frame(const struct vr_frame *f, const s32 at[2][2]);

/* acb.c: command-buffer pieces for compute work on Hmd2's queue (gen1_submit_acb) */
u32 acb_pm4(u32 op, u32 body);
u32 acb_release(u32 *cb, u32 n, u64 addr, u32 data_sel, u32 v);
u32 acb_timestamp(u32 *cb, u32 n, u64 addr); /* the GPU's clock, no cache actions */
u32 acb_acquire(u32 *cb, u32 n, int l2);
u32 acb_barrier(u32 *cb, u32 n, u64 label, u32 v);
/* compose.c: Dead or Alive Xtreme 3's wide and near views into one texture an eye */
struct hmd_eye_uv;
struct hmd_reprojection_param;
int compose_frame(const void *const tex[4], const void *sampler, const struct hmd_eye_uv uv[4],
                  float inner, float outer, struct hmd_reprojection_param *p);
u32 compose_outputs(const u32 (**tsharps)[2][8]); /* its output pairs' T#s, once made (0 before) */
void compose_registered(int ok); /* vr2.c: they are VR port buffers (1), or were refused (0: off) */
int compose_stopped(void); /* vr2.c: registered, and the pass off for good since */
extern u32 g_opt_fanswings, g_opt_chars, g_opt_malfunction; /* the Jet Black Fan's swings, the characters' log, Scarlet's 15 malfunction flags (hooks.c) */
extern u32 g_opt_ownerlv, g_opt_money, g_opt_special, g_opt_starfans, g_opt_sensors, g_opt_portshop; /* hooks.c: the debug menu's */
extern u32 g_opt_compose, g_opt_composemax; /* agent.composemax=N: the composed eye up to N wide (4096; rows 4096 at most) */ /* agent.compose=0: the wide views alone, as the game hands them over */
/* height.c: agent.realheight (the menu's Real height, off by default), agent.heightoffset=<cm> (debug):
 * the location scene's seat placed at the player's own height (the head pose untouched) */
extern volatile u32 g_opt_realheight;
extern volatile u32 g_opt_padmode; /* vr.c: agent.padmode (the settings menu's Controller, live) */
extern volatile s32 g_opt_heightoffset;
void height_recentre(const void *ret, const void *param, const void *result); /* hooks.c, GetResult */
void height_tick(u8 *base, int g); /* hooks.c tick_frame (the main loop; g as game_name's): a view reset's new raise */

/* mirror.c: the headset picture on the TV */
void mirror_start(void);
void mirror_note_port(s32 port, const void *attr);

/* hooks.c: the game's own VideoOut buffer registration (PS4 attribute) */
void hooks_log_audio(void);
void hooks_log_dmem(const char *when); /* hooks.c: the game's direct memory, and what is free */
void hooks_probe_hidden_dmem(const char *when); /* hooks.c: the container's extra segment (probe) */
void hooks_video_queues(void);
s32 hooks_register_buffers(s32 port, s32 start, void *const *addrs, s32 n, const void *attr);
