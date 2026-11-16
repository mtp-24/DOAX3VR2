/* agent.realheight=1 (the settings menu's "Real height", off by default) and agent.heightoffset=<cm>
 * (debug): the seat raised to the player's own height, in the game's own seat, not in the head.
 *
 * Why the seat and not the head (logs/research/scale/HEIGHT.md, REAL-HEIGHT.md): the girls look
 * at, and the view is drawn from, the same field, the camera table's position (Scarlet 0x95aa70,
 * Fortune 0x9529e0, + cam * 0x220 + idx * 0x110 + 0x90; the look-at in vrreact.c, the draw's
 * base in 0x49ed40 / +0xc70), which the camera update copies from the seat slot each frame
 * (0x2454e1: slot + 0x100). A head offset (the tracked pose raised) moves only the view: she
 * would keep looking at the old seat, at the player's chest. So the head pose, its prediction and
 * the tracker are left exactly as they are, and the game's own seat is placed higher instead.
 *
 * The location scene (Photo Paradise's places and poses; Scarlet 0x311597.., Fortune
 * 0x3116f7..) puts the eye and the target at the ground + 0.9 model units (its table at
 * 0x7c5a60 / 0x7be280, one 0.9 for each pose type 0..5; + 0.7 more at location 0x29), times the
 * world scale s, and hands them to the camera placer (0x245890 / 0x246170: slot index, target,
 * eye), then calls the recentre (0x6f1140 / 0x6e9e80), whose sceVrTrackerGetResult call the
 * agent answers (hooks.c). There, in that one call, this file:
 *  - finds the recentre's caller from the stack (the recentre keeps rbp; its frame is aligned to
 *    32, so its return address is at one of two places above the result it asks for), and
 *    when it is the location scene, the scene's own frame (the caller's rbp, saved by the
 *    recentre): the slot index at rbp - 0x64, the target at - 0x60, the eye at - 0x50, as the
 *    scene passed them to the placer a moment before;
 *  - checks every byte of the scene's code from the type read to the placer call (the
 *    rip-relative operands make it the addresses below), the recentre's start and its call, and
 *    that the slot holds that eye and that target;
 *  - H = s * (table[type] (+ 0.7 at 0x29)), read from the game as it is; h = the tracker's head
 *    height above the floor now (PS VR2's y counts from the floor), which is when the recentre
 *    takes the head as its origin; D = clamp(h - H, -0.3, +0.8) (+ agent.heightoffset);
 *  - calls the placer again with the eye and the target both D higher: exactly what the scene
 *    would have done with a table of 0.9 + D / s, the seat's angles and distance the same.
 * The eye then sits at H + D + (head - head at the recentre) above the ground = h + the head's
 * motion since = the head's real height above the floor, for the whole scene.
 * h is the seat's tracker y + the head's game-space y the recentre is answered (the tracker's y
 * itself, live; in a replay, the replayed head counted from the seat).
 * A view reset (OPTIONS held, the menu's Reset view, a re-seat on a tracker jump; with Real height
 * on): height_tick, on the game's main thread, moves the kept scene's seat to the new height at
 * once (the user's worn test, 2026-11-14: sitting down or standing up, then OPTIONS, kept the
 * scene start's height until the camera was changed). See height_tick.
 * Not touched: VR Paradise's four fixed viewpoints (already 1.43-1.67 m, their floor not known)
 * and the scripted scenes' viewpoints. Logged: every scene start while either option is on, with
 * the numbers, every view reset's new raise, and every refusal.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "vr2.h"

volatile u32 g_opt_realheight; /* agent.realheight / the menu's Real height: 0 off (the default since 2026-11-15, the user), 1 on */
volatile s32 g_opt_heightoffset; /* agent.heightoffset=<cm> (debug): added to D; 0 the default */

#define D_MIN (-0.30f)
#define D_MAX (0.80f)

enum { SC_NONE, SC_LOCATION, SC_EVENT, SC_SCRIPT, SC_PARADISE };
static const char *const k_scene[] = {"unknown", "location scene", "view reset (0x1000000a)",
                                      "scripted scene", "VR Paradise viewpoint"};

static const struct hgame {
  const char *name;
  u64 recentre, getresult_ret; /* the recentre, the return of its sceVrTrackerGetResult call */
  u8 getresult_call[5];
  u64 rets[7]; /* its callers' returns: location, event, scripted, VR Paradise x4 */
  u64 scene_code; /* the location scene from its type read to its placer call */
  u8 code[0x76];
  u64 placer, slots, slot_idx, type_ptr, location, table, c29, scale;
  u64 cam_tab, cam_i32; /* the camera table and its second index (vrreact.c's LK_CAM, the s32 index) */
} k_games[] = {
    {"Scarlet 1.08", 0x6f1140, 0x6f11f7, {0xe8, 0x49, 0x38, 0x09, 0x00},
     {0x31165d, 0x4fa358, 0x2ec468, 0x5550c6, 0x555483, 0x55571a, 0x5572bd},
     0x311597,
     {0x48, 0x8b, 0x05, 0x7a, 0x9e, 0x78, 0x00, 0x48, 0x85, 0xc0, 0x74, 0x0e, 0x8b, 0x7d, 0x9c, 0x0f,
      0xb7, 0x40, 0x0e, 0x83, 0xf8, 0x05, 0x77, 0x16, 0xeb, 0x05, 0x8b, 0x7d, 0x9c, 0x31, 0xc0, 0x0f,
      0xb7, 0xc0, 0x48, 0x8d, 0x0d, 0xa0, 0x44, 0x4b, 0x00, 0xc5, 0xfa, 0x58, 0x04, 0x81, 0x0f, 0xb7,
      0x05, 0x34, 0x9e, 0x78, 0x00, 0x83, 0xf8, 0x29, 0x75, 0x08, 0xc5, 0xfa, 0x58, 0x05, 0x2b, 0x7b,
      0x4a, 0x00, 0xc5, 0xfa, 0x11, 0x45, 0xa4, 0xc5, 0xfa, 0x11, 0x45, 0xb4, 0xc4, 0xe2, 0x79, 0x18,
      0x05, 0x94, 0x39, 0x5c, 0x00, 0x48, 0x8d, 0x75, 0xa0, 0x48, 0x8d, 0x55, 0xb0, 0xc5, 0xf8, 0x59,
      0x4d, 0xa0, 0xc5, 0xf8, 0x59, 0x45, 0xb0, 0xc5, 0xf8, 0x29, 0x4d, 0xa0, 0xc5, 0xf8, 0x29, 0x45,
      0xb0, 0xe8, 0x83, 0x42, 0xf3, 0xff},
     0x245890, 0x95b520, 0x972485, 0xa9b418, 0xa9b400, 0x7c5a60, 0x7b9104, 0x8d4f80, 0x95aa70, 0x95aa68},
    {"Fortune 1.19", 0x6e9e80, 0x6e9f37, {0xe8, 0x09, 0x33, 0x09, 0x00},
     {0x3117bd, 0x4f9748, 0x2ecc38, 0x554fd6, 0x555393, 0x55562a, 0x5571cd},
     0x3116f7,
     {0x48, 0x8b, 0x05, 0xba, 0x1c, 0x78, 0x00, 0x48, 0x85, 0xc0, 0x74, 0x0e, 0x8b, 0x7d, 0x9c, 0x0f,
      0xb7, 0x40, 0x0e, 0x83, 0xf8, 0x05, 0x77, 0x16, 0xeb, 0x05, 0x8b, 0x7d, 0x9c, 0x31, 0xc0, 0x0f,
      0xb7, 0xc0, 0x48, 0x8d, 0x0d, 0x60, 0xcb, 0x4a, 0x00, 0xc5, 0xfa, 0x58, 0x04, 0x81, 0x0f, 0xb7,
      0x05, 0x74, 0x1c, 0x78, 0x00, 0x83, 0xf8, 0x29, 0x75, 0x08, 0xc5, 0xfa, 0x58, 0x05, 0x2b, 0x02,
      0x4a, 0x00, 0xc5, 0xfa, 0x11, 0x45, 0xa4, 0xc5, 0xfa, 0x11, 0x45, 0xb4, 0xc4, 0xe2, 0x79, 0x18,
      0x05, 0x38, 0xb8, 0x5b, 0x00, 0x48, 0x8d, 0x75, 0xa0, 0x48, 0x8d, 0x55, 0xb0, 0xc5, 0xf8, 0x59,
      0x4d, 0xa0, 0xc5, 0xf8, 0x59, 0x45, 0xb0, 0xc5, 0xf8, 0x29, 0x4d, 0xa0, 0xc5, 0xf8, 0x29, 0x45,
      0xb0, 0xe8, 0x03, 0x4a, 0xf3, 0xff},
     0x246170, 0x953490, 0x96a3f5, 0xa933b8, 0xa933a0, 0x7be280, 0x7b1964, 0x8ccf84, 0x9529e0, 0x9529d8},
    {"Scarlet 1.00", 0x6ef860, 0x6ef917, {0xe8, 0x89, 0x37, 0x09, 0x00},
     {0x31132d, 0x4f8898, 0x2ec468, 0x5535d6, 0x553993, 0x553c2a, 0x5557cd},
     0x311267,
     {0x48, 0x8b, 0x05, 0x6a, 0xa1, 0x78, 0x00, 0x48, 0x85, 0xc0, 0x74, 0x0e, 0x8b, 0x7d, 0x9c, 0x0f,
      0xb7, 0x40, 0x0e, 0x83, 0xf8, 0x05, 0x77, 0x16, 0xeb, 0x05, 0x8b, 0x7d, 0x9c, 0x31, 0xc0, 0x0f,
      0xb7, 0xc0, 0x48, 0x8d, 0x0d, 0x20, 0x2e, 0x4b, 0x00, 0xc5, 0xfa, 0x58, 0x04, 0x81, 0x0f, 0xb7,
      0x05, 0x24, 0xa1, 0x78, 0x00, 0x83, 0xf8, 0x29, 0x75, 0x08, 0xc5, 0xfa, 0x58, 0x05, 0x0b, 0x65,
      0x4a, 0x00, 0xc5, 0xfa, 0x11, 0x45, 0xa4, 0xc5, 0xfa, 0x11, 0x45, 0xb4, 0xc4, 0xe2, 0x79, 0x18,
      0x05, 0xcc, 0x3c, 0x5c, 0x00, 0x48, 0x8d, 0x75, 0xa0, 0x48, 0x8d, 0x55, 0xb0, 0xc5, 0xf8, 0x59,
      0x4d, 0xa0, 0xc5, 0xf8, 0x59, 0x45, 0xb0, 0xc5, 0xf8, 0x29, 0x4d, 0xa0, 0xc5, 0xf8, 0x29, 0x45,
      0xb0, 0xe8, 0xb3, 0x45, 0xf3, 0xff},
     0x245890, 0x95b520, 0x972485, 0xa9b3d8, 0xa9b3c0, 0x7c40b0, 0x7b77b4, 0x8d4f88, 0x95aa70, 0x95aa68},
};
/* the recentre's start, the same in all three: push rbp; mov rsp, rbp; push r15, r14, rbx;
 * and $-0x20, rsp; sub $0x660, rsp */
static const u8 k_recentre_head[20] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x48,
                                       0x83, 0xe4, 0xe0, 0x48, 0x81, 0xec, 0x60, 0x06, 0x00, 0x00};
/* its GetResult arguments: lea 0x600(%rsp), %rdi; lea 0x10(%rsp), %rsi */
static const u8 k_recentre_args[13] = {0x48, 0x8d, 0xbc, 0x24, 0x00, 0x06, 0x00, 0x00,
                                       0x48, 0x8d, 0x74, 0x24, 0x10};
#define ARGS_OFF 0x63 /* recentre + 0x63: the two lea's (every build) */

#define SLOT_SIZE 0x1510
#define SLOT_ACTIVE 0x12e8

static int g_hg = -2; /* the game, -1: none (or its code not as expected: off for the run) */

static int finite3(const float *v) {
  return __builtin_isfinite(v[0]) && __builtin_isfinite(v[1]) && __builtin_isfinite(v[2]);
}

static int identify(const u8 *base, const u8 *ret) {
  for (int i = 0; i < (int)(sizeof k_games / sizeof *k_games); i++) {
    const struct hgame *g = &k_games[i];
    if (ret != base + g->getresult_ret)
      continue;
    if (memcmp(ret - 5, g->getresult_call, 5) != 0 || memcmp(base + g->recentre, k_recentre_head, 20) != 0 ||
        memcmp(base + g->recentre + ARGS_OFF, k_recentre_args, 13) != 0 ||
        memcmp(base + g->scene_code, g->code, sizeof g->code) != 0) {
      LOG("height: %s's recentre or location scene not as expected: Real height off for this run", g->name);
      return -1;
    }
    LOG("height: %s's recentre and location scene found as expected", g->name);
    return i;
  }
  return -2; /* not the recentre's call: not known yet */
}

/* the recentre's caller: its return address is at rbp + 8, rbp = rsp(aligned) + 0x660 + 8 or 0x18
 * + 0x18; rsp = the result the recentre asks for - 0x10 */
static int caller_of(const u8 *base, const struct hgame *g, const u8 *param, const u8 *r, u64 *caller_rbp) {
  if (param - r != 0x5f0)
    return SC_NONE;
  const u8 *rsp = r - 0x10;
  static const u32 k_at[2] = {0x688, 0x698};
  for (int k = 0; k < 2; k++) {
    u64 ra = *(const u64 *)(rsp + k_at[k]);
    for (int j = 0; j < 7; j++) {
      if (ra != (u64)(base + g->rets[j]))
        continue;
      const u8 *c = base + g->rets[j] - 5; /* a call of the recentre */
      if (c[0] != 0xe8 || (u64)(g->rets[j] + (u64)(s64) * (const s32 *)(c + 1)) != g->recentre)
        return SC_NONE;
      u64 rbp = *(const u64 *)(rsp + k_at[k] - 8); /* the caller's, saved by the recentre */
      u64 here = (u64)(rsp + k_at[k]);
      *caller_rbp = rbp > here && rbp - here < 0x10000 ? rbp : 0;
      return j == 0 ? SC_LOCATION : j == 1 ? SC_EVENT : j == 2 ? SC_SCRIPT : SC_PARADISE;
    }
  }
  return SC_NONE;
}

typedef void (*placer_fn)(s32 slot, const float *target, const float *eye);

/* The location scene's seat as its start left it, for a view reset (height_tick): kept only while
 * Real height is on and the scene was placed by this file; dropped at any other scene's recentre
 * (VR Paradise, scripted, unknown callers) and whenever the slot is not as it was left. */
static struct {
  volatile u32 on;
  s32 slot, cur; /* the scene's slot index, and the game's current camera index (s8) then */
  u32 loc, type;
  float H, d;   /* the seat's height above the ground (m), the raise in force */
  float o;      /* the game's origin y (game space): the head answered to its last recentre */
  volatile u32 o_ok, o_new; /* o known (the answer's status 1); o changed since (0x1000000a) */
  u32 gen;      /* the agent's seat generation the raise was computed for */
  float eye[3], tgt[3]; /* the slot's eye (+0x0) and target (+0x10) as left */
  u32 check;    /* loops to the follow-up read of the live eye and the camera field */
  float live_want, cam_was;
} g_sc;

/* the slot's y fields the placer and the camera update hold the seat in: eye (+0x4) and target
 * (+0x14), their copies (+0xc4, +0xd4), the live camera (+0x104 position, +0x114 target) */
static const u32 k_yoff[6] = {0x04, 0x14, 0xc4, 0xd4, 0x104, 0x114};

static float fabs1(float x) { return x < 0.0f ? -x : x; }

/* the camera field the look-at, the draw and the touch head read (vrreact.c): table + (s8)cur *
 * 0x220 + (s32)idx * 0x110 + 0x90; 0 if the indices are out of range */
static const float *cam_field(const u8 *base, const struct hgame *g) {
  s32 a = *(const s8 *)(base + g->slot_idx), c = *(const s32 *)(base + g->cam_i32);
  if (a < 0 || a > 4 || c < 0 || c > 1)
    return 0;
  return (const float *)(base + g->cam_tab + (u64)a * 0x220 + (u64)c * 0x110 + 0x90);
}

void height_recentre(const void *ret, const void *param, const void *result) {
  float off = (float)g_opt_heightoffset * 0.01f;
  u32 real = g_opt_realheight;
  if (!real && g_opt_heightoffset == 0) {
    g_sc.on = 0; /* (the agent's own state only) */
    return; /* both off (the default): nothing read, nothing written */
  }
  u8 *base = (u8 *)astro_hdr.game_base;
  if (!base || g_hg == -1)
    return;
  if (g_hg == -2) {
    g_hg = identify(base, (const u8 *)ret);
    if (g_hg < 0)
      return;
  }
  const struct hgame *g = &k_games[g_hg];
  if ((const u8 *)ret != base + g->getresult_ret)
    return; /* the frame function's pose read, not the recentre */
  const struct vrt_result *r = (const struct vrt_result *)result;
  /* what the recentre stores as the game's origin (V+0x2c0, RESET.md 1): the answer's position
   * (+0x80) when its status (+0x34) is 1, else its last device position (not known here) */
  int o_ok = r->status == 1 && __builtin_isfinite(r->hmd.device_pose.py);
  float o = o_ok ? r->hmd.device_pose.py : 0.0f;
  u64 fr = 0;
  int sc = caller_of(base, g, (const u8 *)param, (const u8 *)result, &fr);
  if (sc != SC_LOCATION) {
    if (sc == SC_EVENT && g_sc.on) { /* (agent.resetevent=1) the game's origin moved: the raise follows */
      g_sc.o = o, g_sc.o_ok = o_ok;
      __atomic_store_n(&g_sc.o_new, 1, __ATOMIC_RELEASE);
    } else if (g_sc.on) {
      g_sc.on = 0;
      LOG("height: the location scene's seat no longer kept (another scene's recentre)");
    }
    if (sc == SC_PARADISE) {
      s32 i = *(const s8 *)(base + g->slot_idx);
      const float *e = i >= 0 && i <= 4 ? (const float *)(base + g->slots + (u64)i * SLOT_SIZE) : 0;
      LOG("height: recentre from a %s (slot %d, eye y %f): its height left as the game has it (its floor "
          "not known)", k_scene[sc], i, e ? e[1] : 0.0f);
    } else
      LOG("height: recentre from %s%s: the seat left where it is%s", sc == SC_NONE ? "an " : "a ", k_scene[sc],
          sc == SC_EVENT && g_sc.on ? " (the raise follows the game's new origin at the next loop)" : "");
    return;
  }
  g_sc.on = 0; /* a new scene start: the last one's seat is gone */
  if (!fr) {
    LOG("height: location scene: its frame not found on the stack: the seat as the game has it");
    return;
  }
  /* the scene's own frame, as it called the placer */
  s32 i = *(const s32 *)(fr - 0x64);
  const float *tgt = (const float *)(fr - 0x60), *eye = (const float *)(fr - 0x50);
  u8 *slot = i >= 0 && i <= 4 ? base + g->slots + (u64)i * SLOT_SIZE : 0;
  if (!slot || !slot[SLOT_ACTIVE] || memcmp(slot, eye, 12) != 0 || memcmp(slot + 0x10, tgt, 12) != 0 ||
      !finite3(eye) || !finite3(tgt)) {
    LOG("height: location scene: slot %d does not hold the scene's eye and target: the seat as the game has it", i);
    return;
  }
  float s = *(const float *)(base + g->scale);
  const u8 *tp = *(u8 *const *)(base + g->type_ptr);
  u32 type = tp ? *(const u16 *)(tp + 0xe) : 0;
  u32 loc = *(const u16 *)(base + g->location);
  float hm = type <= 5 ? ((const float *)(base + g->table))[type] : 0.0f;
  if (loc == 0x29)
    hm += *(const float *)(base + g->c29);
  float H = hm * s, h = 0.0f, d = 0.0f, sy = 0.0f;
  /* the head's height above the floor: the seat's tracker y + the head's game-space y now (the
   * game space counts from the seat, its y not turned): the tracker's y itself, live; in a replay
   * the replayed head counted from the seat, as the game sees it. The tracker's own y if no seat. */
  u32 gen = vr_seat_y(&sy);
  int have_h = gen && o_ok ? (h = sy + o, 1) : vr_tracker_head_y(&h);
  if (!(s > 0.5f && s < 2.0f) || !__builtin_isfinite(H)) {
    LOG("height: location scene: world scale %f, seat height %f: not sane, the seat as the game has it", s, H);
    return;
  }
  if (real) {
    if (type > 5)
      LOG("height: location scene: pose type %u (no seat height in the game's table): Real height not applied", type);
    else if (!have_h || !__builtin_isfinite(h) || h < 0.2f || h > 2.5f)
      LOG("height: location scene: no head height from the tracker (%f, %s): Real height not applied", h,
          have_h ? "out of 0.2-2.5 m" : "no pose yet");
    else
      d = h - H;
  }
  float raw = d;
  d += off;
  d = d < D_MIN ? D_MIN : d > D_MAX ? D_MAX : d;
  float e2[4] __attribute__((aligned(16))) = {eye[0], eye[1] + d, eye[2], 0.0f};
  float t2[4] __attribute__((aligned(16))) = {tgt[0], tgt[1] + d, tgt[2], 0.0f};
  const float *now = (const float *)slot;
  int placed = 0;
  if (d == 0.0f || !finite3(e2) || !finite3(t2)) {
    LOG("height: location scene (location 0x%x, type %u, slot %d): seat %f m above the ground (%f x s %f), "
        "head %f m: nothing to change", loc, type, i, H, hm, s, h);
    d = 0.0f;
  } else {
    ((placer_fn)(base + g->placer))(i, t2, e2);
    placed = now[1] == e2[1] && now[5] == t2[1];
    LOG("height: location scene (location 0x%x, type %u, slot %d): seat %f m above the ground (%f x s %f), "
        "head %f m above the floor%s: raised %f m (real %f, offset %f, clamped to %.2f..%.2f): eye y %f -> %f%s",
        loc, type, i, H, hm, s, h, real ? "" : " (Real height off)", d, raw, off, D_MIN, D_MAX, eye[1], now[1],
        placed ? "" : " (NOT as written)");
    if (!placed)
      return;
  }
  /* kept for a view reset (height_tick) */
  if (!real || type > 5)
    return;
  g_sc.slot = i, g_sc.cur = *(const s8 *)(base + g->slot_idx), g_sc.loc = loc, g_sc.type = type;
  g_sc.H = H, g_sc.d = d, g_sc.o = o, g_sc.o_ok = o_ok, g_sc.o_new = 0, g_sc.gen = gen, g_sc.check = 0;
  memcpy(g_sc.eye, now, 12);
  memcpy(g_sc.tgt, now + 4, 12);
  __atomic_store_n(&g_sc.on, 1, __ATOMIC_RELEASE);
}

/* A view reset (OPTIONS held, the menu's Reset view, the agent's re-seat on a tracker jump) puts the
 * head back at the seat: the game-space head y drops to 0 while the game's origin stays where the
 * scene started (the ResetVrPosition event is not sent: hooks.c). The eye above the ground is
 * H + D + (head - origin).y, so the raise that puts it at the head's real height is
 *   D = seat y (tracker, at the reset) + origin y (game space) - H
 * (at the scene start it is the h - H above: h = seat y + the head's game y = origin y then). Here,
 * on the game's main thread at the top of its loop, once the agent's seat or the game's origin has
 * changed: the same slot, still active, the same camera index, location and pose type, and its eye
 * and target where the start left them (1 cm), else the kept seat is dropped and nothing written;
 * then every y the placer and the camera update hold the seat in moves by the change in D (the
 * gaze's angles and distance are the same: eye and target move together). The camera field the
 * look-at, the draw and the touch head read is filled from the live camera every frame (0x2454e1). */
void height_tick(u8 *base, int gi) {
  if (g_hg < 0 || gi != g_hg)
    return;
  const struct hgame *g = &k_games[g_hg];
  if (g_sc.check && !--g_sc.check) { /* about half a second later: did the game keep it? */
    const u8 *slot = base + g->slots + (u64)g_sc.slot * SLOT_SIZE;
    const float *cf = cam_field(base, g);
    float live = *(const volatile float *)(slot + 0x104);
    LOG("height: after the view reset: the live eye y %f (written %f%s), the camera field y %f (before %f)", live,
        g_sc.live_want, fabs1(live - g_sc.live_want) < 0.001f ? "" : ": NOT kept", cf ? cf[1] : 0.0f, g_sc.cam_was);
  }
  if (!__atomic_load_n(&g_sc.on, __ATOMIC_ACQUIRE))
    return;
  float sy = 0.0f;
  u32 gen = vr_seat_y(&sy);
  u32 onew = __atomic_exchange_n(&g_sc.o_new, 0, __ATOMIC_ACQ_REL);
  if (!gen || (gen == g_sc.gen && !onew))
    return;
  const char *why = gen != g_sc.gen ? "the seat reset" : "the game's origin moved (0x1000000a)";
  g_sc.gen = gen;
  u8 *slot = base + g->slots + (u64)g_sc.slot * SLOT_SIZE;
  const u8 *tp = *(u8 *const *)(base + g->type_ptr);
  u32 type = tp ? *(const u16 *)(tp + 0xe) : 0, loc = *(const u16 *)(base + g->location);
  s32 cur = *(const s8 *)(base + g->slot_idx);
  const float *f = (const float *)slot;
  int same = slot[SLOT_ACTIVE] && cur == g_sc.cur && loc == g_sc.loc && type == g_sc.type;
  for (int k = 0; same && k < 3; k++)
    same = __builtin_isfinite(f[k]) && __builtin_isfinite(f[4 + k]) && fabs1(f[k] - g_sc.eye[k]) <= 0.01f &&
           fabs1(f[4 + k] - g_sc.tgt[k]) <= 0.01f;
  if (!same) {
    g_sc.on = 0;
    LOG("height: %s: slot %d not as the scene start left it (active %u, camera %d/%d, location 0x%x/0x%x, type "
        "%u/%u, eye y %f/%f): the seat left as it is, no longer kept", why, g_sc.slot, slot[SLOT_ACTIVE], cur,
        g_sc.cur, loc, g_sc.loc, type, g_sc.type, f[1], g_sc.eye[1]);
    return;
  }
  float h = sy + g_sc.o;
  if (!g_sc.o_ok || !__builtin_isfinite(h) || h < 0.2f || h > 2.5f) {
    LOG("height: %s: head %f m (seat %f + the game's origin %f%s): out of 0.2-2.5 m, the seat left as it is", why,
        h, sy, g_sc.o, g_sc.o_ok ? "" : ", not known");
    return;
  }
  float off = (float)g_opt_heightoffset * 0.01f, raw = h - g_sc.H, d = raw + off;
  d = d < D_MIN ? D_MIN : d > D_MAX ? D_MAX : d;
  float delta = d - g_sc.d;
  if (!__builtin_isfinite(delta) || fabs1(delta) < 0.005f) {
    LOG("height: %s: head %f m above the floor (seat %f + the game's origin %f), seat %f m: raised %f m, as it "
        "is", why, h, sy, g_sc.o, g_sc.H, g_sc.d);
    return;
  }
  float v[6];
  for (int k = 0; k < 6; k++) {
    v[k] = *(const volatile float *)(slot + k_yoff[k]) + delta;
    if (!__builtin_isfinite(v[k])) {
      LOG("height: %s: slot %d's y +0x%x not finite with the change %f: the seat left as it is", why, g_sc.slot,
          k_yoff[k], delta);
      return;
    }
  }
  const float *cf = cam_field(base, g);
  float eye0 = f[1], tgt0 = f[5], live0 = f[0x104 / 4];
  g_sc.cam_was = cf ? cf[1] : 0.0f;
  for (int k = 0; k < 6; k++)
    *(volatile float *)(slot + k_yoff[k]) = v[k];
  LOG("height: %s: head %f m above the floor (seat %f + the game's origin %f), seat %f m: raised %f -> %f m (real "
      "%f, offset %f, clamped to %.2f..%.2f): slot %d eye y %f -> %f, target y %f -> %f, live eye y %f -> %f",
      why, h, sy, g_sc.o, g_sc.H, g_sc.d, d, raw, off, D_MIN, D_MAX, g_sc.slot, eye0, f[1], tgt0, f[5], live0,
      f[0x104 / 4]);
  g_sc.d = d;
  g_sc.eye[1] = f[1], g_sc.tgt[1] = f[5];
  g_sc.live_want = v[4];
  g_sc.check = 30;
}
