/* The game's sharp "near" view moved off the eye's axis: the first stages of eye-tracked
 * foveation (logs/research/gaze/REPORT.md). Options:
 *   agent.nearoffset=X,Y  (debug) the near view's centre fixed at tangent (X right, Y up): a screenshot test
 *   agent.gazefollow=1    the centre follows the gaze (fov.c starts eye tracking for it). ON by default,
 *                         both builds: the settings menu's "Eye-tracked sharp area*" (from the next start;
 *                         0: the sharp area stays centred)
 *   agent.nohole=1        (debug) the payload's, at the game's start: the wide view's hole switched off
 *                         (its 0.98 made 0.0); here only checked: without it the centre is not moved
 *                         (a black crescent would show where the near view no longer covers the hole)
 *   agent.movehole=1      with agent.gazefollow / agent.nearoffset, in place of agent.nohole: the game's
 *                         hole kept (its 0.98) and moved with the near view: each frame the wide
 *                         views get a mask of the agent's, built by the game's own rule with the
 *                         hole centred on that frame's near centre (a margin smaller); below.
 *                         ON by default (both builds; cvars.txt's 0 turns it off)
 *   agent.movehole=2      the mechanism's test, nothing visible: the near view kept centred and the
 *                         wide views given, each frame, a fresh agent-built copy of the game's own
 *                         centred masks (rebuilt each frame, to measure the cost and run the ring)
 *
 * How (the report's plan, addresses checked against both eboots):
 * - The near view's projection is built once at VR setup (0x6e9040 Fortune / 0x6f0300 Scarlet) as
 *   a symmetric +-n frustum, n = 0.5 * tan_top (hmd+0x214; agent.nearn's factor for the 0.5 with
 *   that debug option, viewsize.c), and copied into the two frame slots at
 *   hmd + s*0x1c0: near L at +0x410, near R at +0x450, 4x4 rows of 16 bytes. Its third row (+0x20:
 *   near L +0x430, near R +0x470) is (-(r+l)/(r-l), -(t+b)/(t-b), eps, 1), (0, 0) as built; for a
 *   view centred at tangent (cx, cy) it is (-cx/n, -cy/n): the game's own form for its asymmetric
 *   wide views (wide L +0x390, wide R +0x3d0; checked once here against the formula). Nothing
 *   rewrites these per frame; the draw reads them for slot vrr.slot (0x49ed40 / 0x49f9b0).
 * - Written each frame from the main thread at the frame function's pose read (sceVrTrackerGetResult
 *   returning to base+0x4f9ad7 / base+0x4fa6e7; vrr.slot was read just before and flips only after
 *   the draw, 0x4fcaea), for both eyes, and the centre recorded for that slot.
 * - At the hand-over (the VrRenderer thread, h_sceHmdReprojectionStartWideNear) the frame's slot is
 *   its label's place: label s at block + s*8, the block 0x100-aligned (0x548aab..0x548ad5), the
 *   main thread waiting on vrr+0xa0+slot*8 (0x4fca0b), the renderer handing over vrr+0xa0+r15*8.
 *   compose.c shifts the near view's sampling and the blend's centre by the recorded centre (never a
 *   newer gaze), so the composed eye stays seamless.
 * None of this reads or changes the pose (head tracking is not touched): only the near views'
 * projections are written, only while an option is on and the build's code and values are as
 * expected.
 *
 * The moving hole (agent.movehole; logs/research/gaze/REPORT.md section 2, option 3). Read in both
 * eboots (Fortune / Scarlet; the tiling library at the same addresses in both):
 * - The four mask objects (wide L, wide R, near L, near R) are made once at the game's start
 *   (0x548904..: R8, eye size * 0.125) and their pointers kept at vrr+0x108.. (0x1dee000 /
 *   0x1df6120). At each VR start (0x6e9550 / 0x6f0810) the wide ones are filled by 0x6ea580 /
 *   0x6f1840 (object, &hmd.fov, is_left, radius = hmd+0x210 = n - 0.05): a w x h linear byte array
 *   (w, h = the object's u16s at +0x46, +0x48; the T#'s size), one byte a tile of 8 x 8 eye pixels,
 *   with, per tile, its four corners at pixel x*8-1 and x*8+8 (and y alike), +0.5, over w*8 (h*8):
 *     X = (u - c) * (f0+f1)/f2, c = f0/(f0+f1) for the left eye, f1/(f0+f1) for the right
 *     Y = (v - f2/(f2+f3)) * (f2+f3)/f2                       (tangents over f2; Y down)
 *   byte = 0 when every corner is beyond the lens circle (X^2+Y^2 > 1.0, the constant at 0x7b44ec /
 *   0x7bbc80) or every corner is inside the hole (X^2+Y^2 < (radius/f2)^2, times 0.98 at 0x7b44e4
 *   / 0x7bbc78 in Pro mode, sceKernelIsNeoMode); else 0xff (drawn). The array is then tiled into the
 *   texture by the SDK's GpuAddress code: TilingParameters from the T# at object+0x10 (0x14bbe0,
 *   mip 0, slice 0), tileSurface(T#'s base, array, params) (0x14d200; the base from 0x13e680 = the
 *   T#'s 256-byte base without its tile swizzle bits, < 2^40).
 * - The hole pass (0x4cc54b / 0x4cdeeb) reads vrr+0x108+view*8 each view of each frame on the main
 *   thread, after the frame's pose read, and copies the T# and sampler at object+0x10..+0x3f into
 *   its own state (0x686c0): the object itself is not kept by the GPU.
 * - Other readers of the pointers: the VR start's fill (above) and the teardown 0x6ea370 (a
 *   reference count at object+8, decremented; at 0 the object's vtable+0x18 frees it).
 * Here: per eye a ring of 3 agent objects (the game's object's first 0x80 bytes, its T#'s base
 * pointing at 64 KB-aligned agent memory with the game's swizzle bits kept, a reference count that
 * never reaches 0, a mark at +0x78 the game's fill would overwrite), the masks built by the same
 * rule (its floats in the same order) and tiled by the game's own tileSurface with the game's own
 * params. At the start the agent's centred masks are compared byte for byte with the game's (the
 * bytes the tiler writes, found by tiling a marker); only when equal may the hole move. Each frame,
 * at the pose read, the masks for the frame's centre (quantised to whole tiles and held while the
 * near centre stays within a tile's diagonal of it, the hole's radius that much smaller; rebuilt
 * only when that changes, into an entry neither frame in flight uses) are put in vrr+0x108 / +0x110
 * and the entries recorded for the slot. Any mismatch (the game's pointers, objects or code not as
 * found, a failed build): the game's own masks back and the near view centred for that frame; the
 * moving hole off for the session when the game's state changed under it.
 * The builds (logs/research/gaze/MOVEHOLE-COST.md): the game's tiler is read once at the start into a
 * byte map (linear tile -> tiled byte, three tilings of the index), checked against the tiler; an
 * entry is then filled once through the map and afterwards only its changed tiles rewritten (the
 * hole's tiles kept as one span per row), tens of us instead of the tiler's ~0.6 ms an eye.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "vr2.h"

/* ---- options (cvars.txt at the start, hooks.c cvars_fetch) ---- */
static u32 g_gz_fixed, g_gz_follow = 1; /* agent.nearoffset given; agent.gazefollow (on by default) */
static float g_gz_off[2];           /* agent.nearoffset's tangents (x right, y up) */
static u32 g_mh_opt = 1;            /* agent.movehole: 0 off, 1 the hole moved (the default), 2 the test */

#ifndef RELEASE
/* "0.2,-0.1": two decimal numbers; 1 when both were read */
static int parse_pair(const char *v, u32 n, float out[2]) {
  u32 i = 0;
  for (int k = 0; k < 2; k++) {
    while (i < n && (v[i] == ' ' || (k && v[i] == ',')))
      i++;
    float sign = 1.0f, x = 0.0f, scale = 0.0f;
    int digits = 0;
    if (i < n && (v[i] == '-' || v[i] == '+'))
      sign = v[i++] == '-' ? -1.0f : 1.0f;
    for (; i < n && ((v[i] >= '0' && v[i] <= '9') || v[i] == '.'); i++) {
      if (v[i] == '.') {
        if (scale != 0.0f)
          return 0;
        scale = 1.0f;
        continue;
      }
      digits++;
      if (scale != 0.0f) {
        scale *= 0.1f;
        x += (float)(v[i] - '0') * scale;
      } else
        x = x * 10.0f + (float)(v[i] - '0');
    }
    if (!digits)
      return 0;
    out[k] = sign * x;
  }
  return 1;
}
#endif

int gaze_option(const char *key, u32 klen, const char *val, u32 vlen) {
  int off = klen == 10 && memcmp(key, "nearoffset", 10) == 0;
  int fol = (klen == 10 && memcmp(key, "gazefollow", 10) == 0) ||
            (klen == 11 && memcmp(key, "gaze_follow", 11) == 0);
  int hole = klen == 6 && memcmp(key, "nohole", 6) == 0;
  int mh = klen == 8 && memcmp(key, "movehole", 8) == 0;
  if (!off && !fol && !hole && !mh)
    return 0;
#ifdef RELEASE
  if (off || hole) { /* (agent.gazefollow and agent.movehole are the player's: the settings menu) */
    (void)val, (void)vlen;
    LOG("cvars: agent.%.*s is a debug option: not in a release build", (int)klen, key);
    return 1;
  }
#else
  if (hole) {
    LOG("gaze: agent.nohole is the payload's (patched at the game's start); checked here when the "
        "near view is moved");
    return 1;
  }
  if (off) {
    float c[2];
    if (!parse_pair(val, vlen, c) || !(c[0] > -0.7f && c[0] < 0.7f && c[1] > -0.7f && c[1] < 0.7f)) {
      LOG("gaze: agent.nearoffset=%.*s not two tangents within +-0.7 (as 0.2,0): off", (int)vlen, val);
      return 1;
    }
    g_gz_off[0] = c[0], g_gz_off[1] = c[1], g_gz_fixed = 1;
    LOG("gaze: agent.nearoffset = %f, %f (tangent, x right, y up): the near view's centre fixed there",
        c[0], c[1]);
    return 1;
  }
#endif
  u32 x = 0;
  for (u32 i = 0; i < vlen && val[i] >= '0' && val[i] <= '9'; i++)
    x = x * 10 + (u32)(val[i] - '0');
  if (mh) {
#ifdef RELEASE
    g_mh_opt = x == 1; /* (2, the mechanism's test, a debug build's) */
#else
    g_mh_opt = x <= 2 ? x : 0;
#endif
    LOG("gaze: agent.movehole = %u%s", g_mh_opt,
        g_mh_opt == 1   ? " (the game's hole kept and moved with the near view; agent.nohole not needed)"
        : g_mh_opt == 2 ? " (test: the near view centred, the wide views given agent-built copies of the "
                          "game's centred masks each frame)"
        : x             ? " (not 0, 1 or 2: off)"
                        : "");
    return 1;
  }
  gaze_set_follow(x, "cvars.txt");
  return 1;
}

/* agent.gazefollow (on by default; the settings menu's "Eye-tracked sharp area*", taken at the start:
 * eye tracking is started once, by fov.c's rule, from what this is when the VR thread starts, and the
 * hole's mode is chosen once at the first frame, identify) */
void gaze_set_follow(u32 on, const char *from) {
  g_gz_follow = on != 0;
  LOG("gaze: agent.gazefollow = %u (%s)%s", g_gz_follow, from,
      g_gz_follow ? ": the sharp area follows the eyes (eye tracking on)" : ": the sharp area centred");
}
int gaze_follows(void) { return g_gz_follow; }

/* ---- the builds ---- */
static const struct gz_build {
  const char *name;
  u32 ret;      /* the frame function's sceVrTrackerGetResult call returns here */
  u8 code[13];  /* the call (e8 rel32 to the import's stub) and the instruction after it */
  u32 hmd;      /* the headset block: fov at +0x30, n at +0x214, the slots at +s*0x1c0 */
  u32 slot;     /* vrr.slot: the frame slot being drawn */
  u32 hole;     /* the wide masks' 0.98 (Pro mode's inner radius factor), the payload's nohole; 0: none
                 * (Scarlet 1.00: its builder has no factor, the hole always as is) */
  /* the moving hole (agent.movehole) */
  u32 vrr;      /* the renderer block: the mask objects' pointers at +0x108 (wide L), +0x110 (wide R) */
  u32 builder;  /* the wide masks' builder (0x6ea580 / 0x6f1840) */
  u32 calls[3]; /* in it, the calls to TilingParameters' init, the T#'s base and tileSurface */
  u32 kconst;   /* its 0.5 (the corners' half pixel) and, after it, 1.0 (the lens circle) */
  u32 pass;     /* the hole pass's lea of vrr (7 bytes) and its read of vrr+0x108+view*8 (8 bytes) */
} k_gz[] = {
    {"Scarlet 1.08", 0x4fa6e7, {0xe8, 0x59, 0xa3, 0x28, 0x00, 0xc5, 0x79, 0x28, 0x35, 0x31, 0x32, 0x2b, 0x00},
     0x1df6380, 0x1df6078, 0x7bbc78, 0x1df6018, 0x6f1840, {0x6f1b51, 0x6f1b59, 0x6f1b6e}, 0x7bbc7c, 0x4ce267},
    {"Fortune 1.19", 0x4f9ad7, {0xe8, 0x69, 0x37, 0x28, 0x00, 0xc5, 0x79, 0x28, 0x35, 0x11, 0xc7, 0x2a, 0x00},
     0x1dee260, 0x1dedf58, 0x7b44e4, 0x1dedef8, 0x6ea580, {0x6ea891, 0x6ea899, 0x6ea8ae}, 0x7b44e8, 0x4cc8c7},
    {"Scarlet 1.00", 0x4f8c27, {0xe8, 0x79, 0xa4, 0x28, 0x00, 0xc5, 0x79, 0x28, 0x35, 0x81, 0x33, 0x2b, 0x00},
     0x1df6340, 0x1df6038, 0, 0x1df5fd8, 0x6eff60, {0x6f0241, 0x6f0249, 0x6f025e}, 0x7ba328, 0x4cc7a7},
};
/* the SDK's GpuAddress code, at the same addresses in every eboot (each checked through the
 * builder's own calls before use) */
#define FN_TP_INIT 0x14bbe0 /* TilingParameters::initFromTexture(params, T#, mip, slice): 0 = ok */
#define FN_TEX_BASE 0x13e680 /* the T#'s base in 256-byte units, its tile swizzle bits cleared */
#define FN_TILE 0x14d200     /* tileSurface(tiled out, linear in, params): 0 = ok */
static const u32 k_gz_calls[3] = {FN_TP_INIT, FN_TEX_BASE, FN_TILE};
#define VRR_MASKS 0x108 /* vrr+0x108 wide L, +0x110 wide R (+0x118, +0x120 the near views': not ours) */
#define HMD_FOV 0x30
#define HMD_N 0x214
#define SLOT_SIZE 0x1c0
#define WIDE_L 0x390
#define WIDE_R 0x3d0
#define NEAR_L 0x410
#define NEAR_R 0x450
#define ROW2 0x20 /* the off-centre terms' row (the report's "row 3") */
#define EPS_BITS 0xb623d725u /* -2.4414123e-06: the projections' z term (0x7b44d0 / 0x7bbc64) */
#define ONE_BITS 0x3f800000u
#define HOLE_BITS 0x3f7ae148u /* 0.98 */

static u32 fbits(float f) {
  union { float f; u32 u; } x = {f};
  return x.u;
}

/* ---- the record: the centre each slot was drawn with ---- */
struct gz_rec {
  volatile u64 c;   /* two floats: cx, cy (up), packed so a reader never sees half */
  volatile u64 h;   /* agent.movehole: the centre of the hole the slot's wide masks have, packed */
  volatile u32 hv;  /* 1: h holds the agent's masks' centre; 0: the game's own (centred) masks */
  volatile u32 seq; /* the frame that wrote it (0: never) */
};
static struct gz_rec g_rec[2];
static volatile u32 g_gz_seq;      /* frames recorded */
static volatile u32 g_gz_moved;    /* a centre other than 0 was ever written */
static volatile float g_gz_n;      /* n as read (the hand-over's check of the slot's matrix) */
static volatile u64 g_gz_hmd_addr; /* the headset block, once identified */

/* ---- the 10 s figures, gathered on the main thread, printed on the renderer's ---- */
struct gz_stats {
  u32 frames, fixed, follow, fresh, stale, jumps, clamped, refused, bad;
  float xmin, xmax, ymin, ymax;
  /* agent.movehole: frames with the agent's masks, built (both eyes count), reused, frames left
   * with the game's masks (and the near view centred), the build's time (us, per eye) */
  u32 mh_frames, mh_builds, mh_reused, mh_centred, mh_us_max;
  u64 mh_us_sum;
  /* the builds by kind: filled through the map, only the changed tiles, the game's tiler (the map
   * not usable, or a row's hole not one span); frames whose nearest tile moved, the hole held */
  u32 mh_full, mh_diff, mh_slow, mh_held;
};
static struct gz_stats g_st;
static struct {
  volatile u32 due;
  struct gz_stats s;
} g_rep;

static void note_centre(const float c[2]) {
  if (!g_st.frames || c[0] < g_st.xmin) g_st.xmin = c[0];
  if (!g_st.frames || c[0] > g_st.xmax) g_st.xmax = c[0];
  if (!g_st.frames || c[1] < g_st.ymin) g_st.ymin = c[1];
  if (!g_st.frames || c[1] > g_st.ymax) g_st.ymax = c[1];
  g_st.frames++;
}

/* ---- the gaze, filtered ("1 euro", Casiez et al. 2012) ----
 * cut-off 0.1 Hz at rest + 1 Hz a tangent a second of the (0.3 Hz smoothed) speed; a jump past 0.1
 * of tangent (a saccade) taken at once; no fresh gaze (over 300 ms, fov.c): eased back to the centre
 * (time constant 0.3 s). */
#define GF_MINCUT 0.1f
#define GF_BETA 1.0f
#define GF_DCUT 0.3f
#define GF_JUMP 0.1f
#define GF_EASE 0.3f
static float g_f[2], g_d[2];
static u64 g_f_was;

static void follow(float c[2]) {
  u64 now = sceKernelGetProcessTime();
  float dt = g_f_was ? (float)(now - g_f_was) * 1e-6f : 1.0f / 60.0f;
  g_f_was = now;
  dt = dt < 0.001f ? 0.001f : dt > 0.1f ? 0.1f : dt;
  float g[2];
  if (!fov_gaze(g)) {
    float k = dt / GF_EASE;
    k = k > 1.0f ? 1.0f : k;
    g_f[0] -= g_f[0] * k, g_f[1] -= g_f[1] * k;
    g_d[0] = g_d[1] = 0.0f;
    g_st.stale++;
  } else {
    g_st.fresh++;
    float jx = g[0] - g_f[0], jy = g[1] - g_f[1];
    if (jx * jx + jy * jy > GF_JUMP * GF_JUMP) {
      g_f[0] = g[0], g_f[1] = g[1], g_d[0] = g_d[1] = 0.0f;
      g_st.jumps++;
    } else {
      float ad = 1.0f / (1.0f + 1.0f / (6.2831853f * GF_DCUT * dt));
      for (int i = 0; i < 2; i++)
        g_d[i] += ad * ((g[i] - g_f[i]) / dt - g_d[i]);
      float speed = __builtin_sqrtf(g_d[0] * g_d[0] + g_d[1] * g_d[1]);
      float fc = GF_MINCUT + GF_BETA * speed;
      float a = 1.0f / (1.0f + 1.0f / (6.2831853f * fc * dt));
      g_f[0] += a * jx, g_f[1] += a * jy;
    }
  }
  c[0] = g_f[0], c[1] = g_f[1];
}

/* ---- the main thread, at the frame's pose read ---- */
static int g_gz_build = -2;  /* -2 not yet seen, -1 off for good, else k_gz's */
static int g_gz_can_move;    /* the hole off (and Pro mode), or moved: the centre may leave the axis */
enum { GZ_NONE, GZ_NOHOLE, GZ_MOVEHOLE, GZ_SAMEHOLE };
static int g_gz_mode, g_gz_neo;
static float g_last[2][2][2]; /* what was last written: [slot][eye] (x, y) as in the matrix */

static int near_ok(const float *m, float n, const float last[2]) {
  float inv = 1.0f / n, d0 = m[0] - inv, d1 = m[5] - inv;
  if (!(d0 < 1e-3f * inv && d0 > -1e-3f * inv && d1 < 1e-3f * inv && d1 > -1e-3f * inv))
    return 0;
  if (fbits(m[8 + 2]) != EPS_BITS || fbits(m[8 + 3]) != ONE_BITS)
    return 0;
  for (int i = 0; i < 2; i++) /* (as built, -0 or 0, or what was written here last) */
    if ((fbits(m[8 + i]) & 0x7fffffffu) != 0 && fbits(m[8 + i]) != fbits(last[i]))
      return 0;
  return 1;
}

static void self_check(const u8 *hmd, u32 s) {
  const float *f = (const float *)(hmd + HMD_FOV);
  const float *wl = (const float *)(hmd + s * SLOT_SIZE + WIDE_L + ROW2);
  const float *wr = (const float *)(hmd + s * SLOT_SIZE + WIDE_R + ROW2);
  float lx = -(f[1] - f[0]) / (f[0] + f[1]), rx = -(f[0] - f[1]) / (f[0] + f[1]);
  float y = -(f[2] - f[3]) / (f[2] + f[3]);
  float e = 0.0f, t[4] = {wl[0] - lx, wl[1] - y, wr[0] - rx, wr[1] - y};
  for (int i = 0; i < 4; i++)
    e = t[i] > e ? t[i] : -t[i] > e ? -t[i] : e;
  LOG("gaze: self-check, the wide views' off-centre row (fov %f %f %f %f): L game %f %f formula %f %f, "
      "R game %f %f formula %f %f: %s", f[0], f[1], f[2], f[3], wl[0], wl[1], lx, y, wr[0], wr[1], rx, y,
      e < 1e-4f ? "as the formula" : "NOT as the formula (the near view's centre may land elsewhere)");
}

static int identify(const u8 *ret) {
  u8 *base = (u8 *)astro_hdr.game_base;
  if (!base)
    return -1;
  for (int i = 0; i < (int)(sizeof k_gz / sizeof *k_gz); i++) {
    if (ret != base + k_gz[i].ret)
      continue;
    if (memcmp(ret - 5, k_gz[i].code, sizeof k_gz[i].code) != 0) {
      LOG("gaze: %s's pose read not as expected at +0x%x: off", k_gz[i].name, k_gz[i].ret);
      return -1;
    }
    /* (no factor: the hole on, taken as 1.0; agent.nohole has nothing to patch) */
    int nofactor = !k_gz[i].hole;
    u32 hole = nofactor ? ONE_BITS : *(volatile u32 *)(base + k_gz[i].hole);
    int neo = sceKernelIsNeoMode ? sceKernelIsNeoMode() : -1;
    g_gz_neo = neo > 0;
    /* agent.movehole=2: the near view centred, the masks the agent's copies (the hole as the game
     * has it, on or off); the hole off (agent.nohole, Pro mode): the near view moved, nothing else;
     * agent.movehole=1 with the hole on: the near view moved with the agent's masks */
    g_gz_mode = g_mh_opt == 2                  ? GZ_SAMEHOLE
                : hole == 0 && neo > 0         ? GZ_NOHOLE
                : g_mh_opt == 1 && (hole == HOLE_BITS || nofactor) ? GZ_MOVEHOLE
                                               : GZ_NONE;
    g_gz_can_move = g_gz_mode != GZ_NONE;
    LOG("gaze: %s identified (the pose read at +0x%x); the wide view's hole factor at +0x%x is %s, Pro "
        "mode %d: %s", k_gz[i].name, k_gz[i].ret, k_gz[i].hole,
        nofactor ? "none (this build's masks have no 0.98: the hole on, as 1.0)"
        : hole == 0 ? "0 (agent.nohole: the hole off)" : hole == HOLE_BITS ? "0.98 (the hole on)" : "UNKNOWN",
        neo,
        g_gz_mode == GZ_SAMEHOLE ? "agent.movehole=2: the near view kept centred, the wide views given the "
                                   "agent's copies of the game's centred masks"
        : g_gz_mode == GZ_NOHOLE ? (g_mh_opt == 1 ? "the near view may be moved (the hole is off: "
                                                    "agent.movehole not used)"
                                                  : "the near view may be moved")
        : g_gz_mode == GZ_MOVEHOLE ? "the near view may be moved, the hole with it (agent.movehole=1)"
                                   : "the near view is NOT moved (agent.nohole=1 in Pro mode, or "
                                     "agent.movehole=1, needed): nothing written");
    __atomic_store_n(&g_gz_hmd_addr, (u64)(base + k_gz[i].hmd), __ATOMIC_RELEASE);
    return i;
  }
  return -2; /* (another call of GetResult: not the frame's pose read) */
}

/* ---- the moving hole (agent.movehole; the file's head says how the game makes its masks) ----
 * The masks' floats are computed as the game's builder computes them, operation for operation
 * (no fused multiply-add), so the centred ones come out the same, byte for byte. */
#pragma clang fp contract(off)
#define MH_RING 3          /* masks per eye: one per frame in flight (two slots) and one to build */
#define MH_SLOT 0x20000u   /* one mask's memory, 64 KB-aligned (128 KB: its tiled size is checked) */
#define MH_MAXWH 256       /* the masks' size across, tiles (8 x 8 eye pixels), at most */
#define MH_MAXH 512        /* and down (an eye 2048 x 2291 has 256 x 286: 2026-11-14 the first run refused it) */
#define MH_LIN 0x20000u    /* the linear mask and each lens mask: w x h bytes at the most (256 x 512) */
#define MH_OBJ 0x80        /* the game's object, copied (its fill reads up to +0x7f; the pass +0x3f) */
#define MH_MARK 0x7eu      /* the copies' +0x78: the game's map (0x6cbd0) writes 1 and unmap 3 there */
#define MH_REFS 0x10000000u /* the copies' reference count (+8): the teardown never frees them */
#define MH_MARGIN 0.02f    /* the hole's radius under the game's n - 0.05 (tangent), besides the
                            * tile rounding: the near view's sharp disc always covers it with room */
#define MH_HOLD 1.0f       /* the hole held while the near centre is within this many tile diagonals
                            * of it (the rounding alone needs half): its radius that much smaller */

typedef s32 (*mh_tp_init_fn)(void *params, const void *tsharp, u32 mip, u32 slice);
typedef u32 (*mh_tex_base_fn)(const void *tsharp);
typedef s32 (*mh_tile_fn)(void *out, const void *in, const void *params);

struct mh_entry {
  u8 obj[0x100] __attribute__((aligned(16))); /* the game's object, its T#'s base this entry's mem */
  u8 *mem;                                    /* the tiled mask, GPU-visible */
  s32 kx, ky;                                 /* the hole's centre, whole tiles (x right, y down) */
  u32 valid, used;                            /* mem holds kx, ky's mask; the frame last put in */
  u32 filled;                                 /* mem was written through the map: sa, sb are its hole */
  u16 sa[MH_MAXH], sb[MH_MAXH];               /* per row, the hole's tiles [sa, sb) (none: equal) */
};
static struct {
  int state;    /* 0 not tried, 1 ready, -1 off for good */
  int verified; /* the agent's centred masks are the game's, byte for byte (both eyes) */
  u8 *vrr, *gobj[2], *gmem[2], *lin, *lens[2], *blk;
  u8 gcopy[2][MH_OBJ]; /* the game's objects as found */
  float fov[4], rad;   /* the headset's tangents and the game's hole radius (n - 0.05) as found */
  u32 w, h, extent;    /* tiles across, down; the bytes the tiler writes (from 0) */
  u8 tp[2][0x100] __attribute__((aligned(32))); /* the game's TilingParameters for each mask */
  mh_tile_fn tile;
  float x0[2][MH_MAXWH], x1[2][MH_MAXWH], y0[MH_MAXH], y1[MH_MAXH]; /* corner tangents / f2 */
  float r2game, r2move, stepx, stepy, qerr_t;
  u32 *map;     /* the tiler's byte map: linear tile i -> its byte in the tiled mask (w * h) */
  int fast;     /* the map read and checked against the tiler: builds through it */
  int hold;     /* hold_kx, hold_ky: the hole's centre now (held while within hold2) */
  s32 hold_kx, hold_ky;
  float hold2;  /* (MH_HOLD tile diagonals, over f2)^2 */
  u16 na[MH_MAXH], nb[MH_MAXH], ta[MH_MAXH], tb[MH_MAXH]; /* a build's spans; the start's check's */
  struct mh_entry e[2][MH_RING];
  int cur[2];    /* per eye, the entry in the game's pointer now, -1 the game's own */
  int rec[2][2]; /* per slot, per eye: the entry that slot's frame was drawn with, -1 the game's */
  u32 frame;
} g_mh; /* (cur and rec made -1 by mh_init, before anything reads them) */

static int mh_readable(const void *p, u64 n) {
  u8 info[128];
  memset(info, 0, sizeof info);
  if (!sceKernelVirtualQuery || sceKernelVirtualQuery(p, 0, info, 0x48) != 0)
    return 0;
  u64 start = *(u64 *)info, end = *(u64 *)(info + 8);
  s32 prot = *(s32 *)(info + 24);
  return start <= (u64)p && (u64)p + n <= end && (prot & 1);
}

static s32 mh_round(float x) { return (s32)(x + (x >= 0.0f ? 0.5f : -0.5f)); }

/* the corners' tangents over f2, per tile column (each eye) and row: the builder's own arithmetic */
static void mh_axes(const float *f, float k_half) {
  float W8 = (float)(s32)(g_mh.w * 8), H8 = (float)(s32)(g_mh.h * 8);
  float sum = f[1] + f[0], s = sum / f[2];
  for (int eye = 0; eye < 2; eye++) {
    float c = (eye == 0 ? f[0] : f[1]) / sum; /* (wide L: is_left = 1) */
    for (u32 x = 0; x < g_mh.w; x++) {
      float u0 = ((float)(s32)(x * 8 - 1) + k_half) / W8, u1 = ((float)(s32)(x * 8 + 8) + k_half) / W8;
      g_mh.x0[eye][x] = (u0 - c) * s, g_mh.x1[eye][x] = (u1 - c) * s;
    }
  }
  float sy = f[2] + f[3], cy = f[2] / sy, ky = sy / f[2];
  for (u32 y = 0; y < g_mh.h; y++) {
    float v0 = ((float)(s32)(y * 8 - 1) + k_half) / H8, v1 = ((float)(s32)(y * 8 + 8) + k_half) / H8;
    g_mh.y0[y] = (v0 - cy) * ky, g_mh.y1[y] = (v1 - cy) * ky;
  }
  g_mh.stepx = s / (float)g_mh.w, g_mh.stepy = ky / (float)g_mh.h; /* (one tile, over f2) */
}

/* the lens circle's part of the rule: 0xff where a corner is within it (centred, never moved) */
static void mh_lens(int eye, float k_lens) {
  u8 *o = g_mh.lens[eye];
  for (u32 y = 0; y < g_mh.h; y++) {
    float a0 = g_mh.y0[y] * g_mh.y0[y], a1 = g_mh.y1[y] * g_mh.y1[y];
    for (u32 x = 0; x < g_mh.w; x++) {
      float b0 = g_mh.x0[eye][x] * g_mh.x0[eye][x], b1 = g_mh.x1[eye][x] * g_mh.x1[eye][x];
      *o++ = (k_lens >= b0 + a0 || k_lens >= b1 + a0 || k_lens >= b0 + a1 || k_lens >= b1 + a1) ? 0xff : 0;
    }
  }
}

/* one eye's mask with the hole centred at (hx, hy) (over f2, y down), inner radius squared r2,
 * tiled into out; 0 = ok. A tile is 0 (not drawn) when inside the lens circle's rule and every
 * corner is inside the hole; a row or column with a corner's own distance already >= r2 has none
 * (adding a square never lowers a float sum below it), so only the hole's own rows are tested. */
static s32 mh_build(int eye, float hx, float hy, float r2, u8 *out) {
  u32 w = g_mh.w, h = g_mh.h;
  memcpy(g_mh.lin, g_mh.lens[eye], w * h);
  if (r2 > 0.0f)
    for (u32 y = 0; y < h; y++) {
      float dy0 = g_mh.y0[y] - hy, dy1 = g_mh.y1[y] - hy;
      float a0 = dy0 * dy0, a1 = dy1 * dy1;
      if (a0 >= r2 || a1 >= r2)
        continue;
      u8 *row = g_mh.lin + y * w;
      for (u32 x = 0; x < w; x++) {
        if (!row[x])
          continue;
        float dx0 = g_mh.x0[eye][x] - hx, dx1 = g_mh.x1[eye][x] - hx;
        float b0 = dx0 * dx0, b1 = dx1 * dx1;
        if (b0 >= r2 || b1 >= r2)
          continue;
        if (b0 + a0 < r2 && b1 + a0 < r2 && b0 + a1 < r2 && b1 + a1 < r2)
          row[x] = 0;
      }
    }
  return g_mh.tile(out, g_mh.lin, g_mh.tp[eye]);
}

/* the same rule as spans: per row, the tiles [sa, sb) cleared (the hole). A tile is in the hole when
 * its four corner sums are < r2, that is when the largest x term plus the largest y term is (a float
 * sum never falls as an addend grows, and the largest is one of the four); the largest x term along a
 * row first falls then grows, so the hole's tiles are one span. 1 = ok; 0 = a row not one span (the
 * caller then uses mh_build). A tile the lens rule leaves 0 may be in a span: cleared, it stays 0. */
static int mh_spans(int eye, float hx, float hy, float r2, u16 *sa, u16 *sb) {
  u32 w = g_mh.w, h = g_mh.h, c0 = w, c1 = 0;
  float bm[MH_MAXWH];
  for (u32 x = 0; x < w; x++) {
    float dx0 = g_mh.x0[eye][x] - hx, dx1 = g_mh.x1[eye][x] - hx;
    float b0 = dx0 * dx0, b1 = dx1 * dx1;
    bm[x] = b0 > b1 ? b0 : b1;
    if (bm[x] < r2) {
      if (x < c0)
        c0 = x;
      c1 = x + 1;
    }
  }
  for (u32 y = 0; y < h; y++) {
    sa[y] = sb[y] = 0;
    if (!(r2 > 0.0f) || c0 >= c1)
      continue;
    float dy0 = g_mh.y0[y] - hy, dy1 = g_mh.y1[y] - hy;
    float a0 = dy0 * dy0, a1 = dy1 * dy1, am = a0 > a1 ? a0 : a1;
    if (am >= r2)
      continue;
    u32 a = 0, b = 0, in = 0;
    for (u32 x = c0; x < c1; x++) {
      if (bm[x] + am < r2) {
        if (!in) {
          if (b)
            return 0; /* (a second span: never by the argument above) */
          a = x, in = 1;
        }
      } else if (in)
        b = x, in = 0;
    }
    if (in)
      b = c1;
    sa[y] = (u16)a, sb[y] = (u16)b;
  }
  return 1;
}

/* an entry written whole through the map: the lens mask, then the hole's spans cleared */
static void mh_fill(int eye, u8 *dst, const u16 *sa, const u16 *sb) {
  const u32 *map = g_mh.map;
  const u8 *lens = g_mh.lens[eye];
  u32 w = g_mh.w, n = w * g_mh.h;
  for (u32 i = 0; i < n; i++)
    dst[map[i]] = lens[i];
  for (u32 y = 0; y < g_mh.h; y++)
    for (u32 x = sa[y]; x < sb[y]; x++)
      dst[map[y * w + x]] = 0;
}

/* an entry whose hole was [oa, ob) moved to [na, nb): only the tiles that change (the old hole's
 * not in the new one get the lens mask's byte back, the new one's not in the old one cleared) */
static void mh_move(int eye, u8 *dst, const u16 *oa, const u16 *ob, const u16 *na, const u16 *nb) {
  const u32 *map = g_mh.map;
  const u8 *lens = g_mh.lens[eye];
  u32 w = g_mh.w;
  for (u32 y = 0; y < g_mh.h; y++) {
    u32 a0 = oa[y], b0 = ob[y], a1 = na[y], b1 = nb[y];
    if (a0 == a1 && b0 == b1)
      continue;
    const u32 *m = map + y * w;
    const u8 *l = lens + y * w;
    if (a1 >= b1)
      a1 = b1 = 0; /* (an empty span as [0, 0): the ranges below then cover the other whole) */
    if (a0 >= b0)
      a0 = b0 = 0;
    for (u32 x = a0, e = b0 < a1 ? b0 : a1; x < e; x++) /* old, left of the new */
      dst[m[x]] = l[x];
    for (u32 x = a0 > b1 ? a0 : b1; x < b0; x++) /* old, right of the new */
      dst[m[x]] = l[x];
    for (u32 x = a1, e = b1 < a0 ? b1 : a0; x < e; x++) /* new, left of the old */
      dst[m[x]] = 0;
    for (u32 x = a1 > b0 ? a1 : b0; x < b1; x++) /* new, right of the old */
      dst[m[x]] = 0;
  }
}

/* the tiler's byte map, read by tiling the index (three bytes, three tilings) with eye L's params
 * where the marker (in blk's first entry) showed the tiler writes; then scatter builds (filled, and
 * moved through four holes) compared with the tiler's, both eyes, the whole entry. 1 = usable */
static int mh_map_init(u8 *blk) {
  u32 n = g_mh.w * g_mh.h, ext = g_mh.extent;
  u8 *pl[3] = {blk + 3 * MH_SLOT, blk + 4 * MH_SLOT, blk + 5 * MH_SLOT};
  for (int k = 0; k < 3; k++) {
    memset(pl[k], 0, MH_SLOT);
    for (u32 i = 0; i < n; i++)
      g_mh.lin[i] = (u8)(i >> (8 * k));
    s32 rc = g_mh.tile(pl[k], g_mh.lin, g_mh.tp[0]);
    if (rc) {
      LOG("gaze: movehole: tiling the index (byte %d) -> 0x%x: the map not used (the game's tiler)", k, rc);
      return 0;
    }
  }
  memset(g_mh.map, 0xff, (u64)n * 4);
  u32 got = 0;
  for (u32 p = 0; p < ext; p++) {
    if (!blk[p])
      continue;
    u32 i = pl[0][p] | (u32)pl[1][p] << 8 | (u32)pl[2][p] << 16;
    if (i >= n || g_mh.map[i] != 0xffffffffu) {
      LOG("gaze: movehole: the tiled index at %u is %u (of %u%s): the map not used (the game's tiler)", p, i,
          n, i < n ? ", seen twice" : "");
      return 0;
    }
    g_mh.map[i] = p, got++;
  }
  if (got != n) {
    LOG("gaze: movehole: the map has %u of %u tiles: not used (the game's tiler)", got, n);
    return 0;
  }
  static const s32 k_holes[4][2] = {{3, -2}, {0, 0}, {-7, 5}, {25, -18}};
  u8 *mine = blk + 4 * MH_SLOT, *ref = blk + 3 * MH_SLOT;
  for (int eye = 0; eye < 2; eye++) {
    memset(mine, 0, MH_SLOT);
    for (int i = 0; i < 4; i++) {
      float hx = (float)k_holes[i][0] * g_mh.stepx, hy = (float)k_holes[i][1] * g_mh.stepy;
      if (!mh_spans(eye, hx, hy, g_mh.r2move, g_mh.na, g_mh.nb)) {
        LOG("gaze: movehole: the %s hole at tiles %d, %d not one span a row: the map not used", eye ? "R" : "L",
            k_holes[i][0], k_holes[i][1]);
        return 0;
      }
      if (i == 0)
        mh_fill(eye, mine, g_mh.na, g_mh.nb);
      else
        mh_move(eye, mine, g_mh.ta, g_mh.tb, g_mh.na, g_mh.nb);
      memcpy(g_mh.ta, g_mh.na, g_mh.h * 2), memcpy(g_mh.tb, g_mh.nb, g_mh.h * 2);
      memset(ref, 0, MH_SLOT);
      s32 rc = mh_build(eye, hx, hy, g_mh.r2move, ref);
      u32 diff = 0, first = 0;
      for (u32 p = 0; p < MH_SLOT; p++)
        if (mine[p] != ref[p] && !diff++)
          first = p;
      if (rc || diff) {
        LOG("gaze: movehole: the %s mask through the map, the hole at tiles %d, %d (%s): the tiler -> 0x%x, "
            "%u bytes differ (first at %u): the map not used (the game's tiler)", eye ? "R" : "L",
            k_holes[i][0], k_holes[i][1], i ? "moved" : "filled", rc, diff, first);
        return 0;
      }
    }
  }
  return 1;
}

static void mh_put(int eye, int j) {
  u8 *p = j < 0 ? g_mh.gobj[eye] : g_mh.e[eye][j].obj;
  __atomic_store_n((u8 **)(g_mh.vrr + VRR_MASKS + eye * 8), p, __ATOMIC_RELEASE);
  g_mh.cur[eye] = j;
}

/* the game's own masks back, where the pointer is one of the agent's (never over another) */
static void mh_restore(u32 s) {
  if (!g_mh.vrr)
    return;
  for (int eye = 0; eye < 2; eye++) {
    u8 *p = __atomic_load_n((u8 **)(g_mh.vrr + VRR_MASKS + eye * 8), __ATOMIC_ACQUIRE);
    for (int j = 0; j < MH_RING; j++)
      if (p == g_mh.e[eye][j].obj)
        mh_put(eye, -1);
    g_mh.cur[eye] = -1;
    if (s < 2)
      g_mh.rec[s][eye] = -1;
  }
}

static void mh_off(u32 s, const char *why) {
  mh_restore(s);
  g_mh.state = -1;
  LOG("gaze: the moving hole off for good: %s (the game's own masks, the near view centred)", why);
}

static int mh_init(const struct gz_build *b, u8 *base, u8 *hmd) {
  u64 t0 = sceKernelGetProcessTime();
  g_mh.state = -1;
  g_mh.cur[0] = g_mh.cur[1] = -1;
  g_mh.rec[0][0] = g_mh.rec[0][1] = g_mh.rec[1][0] = g_mh.rec[1][1] = -1;
  /* the code as read: the builder's calls of the tiling code, the pass's read of the pointers */
  for (int i = 0; i < 3; i++) {
    const u8 *site = base + b->calls[i];
    if (site[0] != 0xe8 || site + 5 + *(const s32 *)(site + 1) != base + k_gz_calls[i]) {
      LOG("gaze: movehole: the mask builder's call at +0x%x is not to +0x%x: off", b->calls[i], k_gz_calls[i]);
      return 0;
    }
  }
  static const u8 k_pass[11] = {0x49, 0x89, 0xc4, 0x4b, 0x8b, 0x8c, 0xc4, 0x08, 0x01, 0x00, 0x00};
  const u8 *ps = base + b->pass;
  if (ps[0] != 0x48 || ps[1] != 0x8d || ps[2] != 0x05 || ps + 7 + *(const s32 *)(ps + 3) != base + b->vrr ||
      memcmp(ps + 7, k_pass, sizeof k_pass) != 0) {
    LOG("gaze: movehole: the hole pass's read of the masks at +0x%x not as expected: off", b->pass);
    return 0;
  }
  float k_half = *(const float *)(base + b->kconst), k_lens = *(const float *)(base + b->kconst + 4);
  if (fbits(k_half) != 0x3f000000u || fbits(k_lens) != ONE_BITS) {
    LOG("gaze: movehole: the builder's constants at +0x%x are %f, %f (0.5, 1.0 expected): off", b->kconst,
        k_half, k_lens);
    return 0;
  }
  g_mh.vrr = base + b->vrr;
  mh_tp_init_fn tp_init = (mh_tp_init_fn)(base + FN_TP_INIT);
  mh_tex_base_fn tex_base = (mh_tex_base_fn)(base + FN_TEX_BASE);
  g_mh.tile = (mh_tile_fn)(base + FN_TILE);
  u32 swz[2];
  for (int eye = 0; eye < 2; eye++) {
    u8 *o = *(u8 *volatile *)(g_mh.vrr + VRR_MASKS + eye * 8);
    if (!o || !mh_readable(o, MH_OBJ)) {
      LOG("gaze: movehole: the game's wide %s mask object %p not there: off", eye ? "R" : "L", o);
      return 0;
    }
    const u32 *t = (const u32 *)(o + 0x10);
    u32 w = *(const u16 *)(o + 0x46), h = *(const u16 *)(o + 0x48);
    u32 tw = (t[2] & 0x3fff) + 1, th = ((t[2] >> 14) & 0x3fff) + 1, pitch = ((t[4] >> 13) & 0x3fff) + 1;
    u32 dfmt = (t[1] >> 20) & 0x3f, type = t[3] >> 28, levels = (t[3] >> 12) & 0xff;
    LOG("gaze: movehole: the game's wide %s mask, object %p: %ux%u tiles; T# %08x %08x %08x %08x %08x %08x "
        "%08x %08x (%ux%u, pitch %u, dfmt %u, type %u, tiling index %u), sampler %08x %08x %08x %08x",
        eye ? "R" : "L", o, w, h, t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7], tw, th, pitch, dfmt, type,
        (t[3] >> 20) & 0x1f, t[8], t[9], t[10], t[11]);
    if (w != tw || h != th || !w || !h || w > MH_MAXWH || h > MH_MAXH || pitch > MH_MAXWH || w * h > MH_LIN || dfmt != 1 ||
        type != 9 || levels != 0 || *(const u64 *)(o + 0x70) != 0 || (eye && (w != g_mh.w || h != g_mh.h))) {
      LOG("gaze: movehole: that mask is not as expected (8-bit 2D, one level, at most %ux%u, the two the "
          "same size, not mapped): off", MH_MAXWH, MH_MAXH);
      return 0;
    }
    g_mh.w = w, g_mh.h = h, g_mh.gobj[eye] = o;
    memcpy(g_mh.gcopy[eye], o, MH_OBJ);
    s32 rc = tp_init(g_mh.tp[eye], t, 0, 0);
    u32 b256 = tex_base(t);
    swz[eye] = t[0] ^ b256;
    g_mh.gmem[eye] = (u8 *)((u64)b256 << 8);
    if (rc != 0 || (b256 & ~t[0]) != 0 || (t[1] & 0xff) != 0) {
      LOG("gaze: movehole: the tiling parameters -> 0x%x, the base 0x%x (T# 0x%x): off", rc, b256, t[0]);
      return 0;
    }
  }
  /* the agent's memory: the six masks, then the linear mask and the two lens masks, then the map */
  u64 map_size = ((u64)g_mh.w * g_mh.h * 4 + 0xffff) & ~(u64)0xffff;
  u64 size = 2 * MH_RING * MH_SLOT + 3 * MH_LIN + map_size;
  u8 *blk = vr2_dalloc_pref(size, 0x10000, "the moving hole's masks");
  if (!blk || ((u64)blk + size) >> 40 || ((u64)blk & 0xffff)) {
    LOG("gaze: movehole: no memory for the masks below 1 TB (%p): off", blk);
    return 0;
  }
  g_mh.blk = blk, g_mh.lin = blk + 2 * MH_RING * MH_SLOT;
  g_mh.lens[0] = g_mh.lin + MH_LIN, g_mh.lens[1] = g_mh.lin + 2 * MH_LIN;
  g_mh.map = (u32 *)(g_mh.lin + 3 * MH_LIN);
  for (int eye = 0; eye < 2; eye++)
    for (int j = 0; j < MH_RING; j++) {
      g_mh.e[eye][j].mem = blk + (eye * MH_RING + j) * MH_SLOT;
      if (((u32)((u64)g_mh.e[eye][j].mem >> 8)) & swz[eye]) {
        LOG("gaze: movehole: the tile swizzle 0x%x does not fit the masks' alignment: off", swz[eye]);
        return 0;
      }
    }
  /* where the tiler writes: a marker tiled into eye L's three entries (both eyes' params alike) */
  u32 n = g_mh.w * g_mh.h, count = 0, last = 0, same = 1;
  memset(g_mh.lin, 1, n);
  s32 rc0 = g_mh.tile(blk, g_mh.lin, g_mh.tp[0]);
  s32 rc1 = g_mh.tile(blk + MH_RING * MH_SLOT, g_mh.lin, g_mh.tp[1]);
  for (u32 i = 0; i < MH_RING * MH_SLOT; i++) {
    if (blk[i])
      count++, last = i;
    same &= blk[i] == blk[MH_RING * MH_SLOT + i]; /* (the two eyes' layouts alike) */
  }
  g_mh.extent = last + 1;
  if (rc0 || rc1 || count != n || !same || g_mh.extent > MH_SLOT || !mh_readable(g_mh.gmem[0], g_mh.extent) ||
      !mh_readable(g_mh.gmem[1], g_mh.extent)) {
    LOG("gaze: movehole: tiling a marker -> 0x%x, 0x%x, %u bytes written (%u expected), the eyes alike %u, up "
        "to %u (%u at most), the game's masks at %p, %p readable %d %d: off", rc0, rc1, count, n, same,
        g_mh.extent, MH_SLOT,
        g_mh.gmem[0], g_mh.gmem[1], mh_readable(g_mh.gmem[0], g_mh.extent),
        mh_readable(g_mh.gmem[1], g_mh.extent));
    return 0;
  }
  /* the rule, from the values the game built its masks from */
  const float *f = (const float *)(hmd + HMD_FOV);
  memcpy(g_mh.fov, f, sizeof g_mh.fov);
  g_mh.rad = *(const float *)(hmd + 0x210);
  float factor = b->hole ? *(const float *)(base + b->hole) : 1.0f; /* (none: Scarlet 1.00) */
  float q = g_mh.rad / f[2];
  q = q * q;
  if (g_gz_neo)
    q = q * factor;
  g_mh.r2game = q;
  mh_axes(f, k_half);
  mh_lens(0, k_lens);
  mh_lens(1, k_lens);
  /* the moved hole's radius: n - 0.05 less the margin and MH_HOLD tiles' diagonals (the hole's
   * centre is whole tiles, the near view's not: half a diagonal for the rounding, the rest the hold
   * that spares a rebuild for a centre wavering across a tile's edge) */
  float qerr = MH_HOLD * __builtin_sqrtf(g_mh.stepx * g_mh.stepx + g_mh.stepy * g_mh.stepy);
  float rm = g_mh.rad / f[2] - MH_MARGIN / f[2] - qerr;
  g_mh.r2move = rm > 0.0f ? rm * rm * (g_gz_neo ? factor : 1.0f) : 0.0f;
  g_mh.qerr_t = qerr * f[2];
  g_mh.hold2 = qerr * qerr, g_mh.hold = 0;
  /* the agent's centred masks against the game's, on the bytes the tiler writes */
  u32 diff[2] = {0, 0}, first[2] = {0, 0};
  s32 brc[2];
  for (int eye = 0; eye < 2; eye++) {
    u8 *mine = blk + (1 + eye) * MH_SLOT; /* (entries L1, L2: the marker is in L0) */
    brc[eye] = mh_build(eye, 0.0f, 0.0f, g_mh.r2game, mine);
    const volatile u8 *game = g_mh.gmem[eye];
    for (u32 i = 0; i < g_mh.extent; i++)
      if (blk[i] && mine[i] != game[i] && !diff[eye]++)
        first[eye] = i;
  }
  g_mh.verified = !brc[0] && !brc[1] && !diff[0] && !diff[1];
  /* the tiler's byte map (the marker still in entry L0), checked against the tiler */
  u64 tm = sceKernelGetProcessTime();
  g_mh.fast = g_mh.verified && g_mh_opt == 1 && mh_map_init(blk);
  LOG("gaze: movehole: the tiler's byte map %s (%lu us)",
      g_mh.fast ? "read and the same as the tiler (filled, moved, four holes, both eyes): builds through it, "
                  "only the changed tiles rewritten"
                : "not used: each build through the game's tiler",
      sceKernelGetProcessTime() - tm);
  memset(blk, 0, 2 * MH_RING * MH_SLOT);
  for (int eye = 0; eye < 2; eye++)
    for (int j = 0; j < MH_RING; j++) {
      struct mh_entry *e = &g_mh.e[eye][j];
      memset(e->obj, 0, sizeof e->obj);
      memcpy(e->obj, g_mh.gobj[eye], MH_OBJ);
      u32 *t = (u32 *)(e->obj + 0x10);
      t[0] = (u32)((u64)e->mem >> 8) | swz[eye];
      t[1] = (t[1] & ~0xffu) | (u32)(((u64)e->mem >> 40) & 0xff);
      *(u32 *)(e->obj + 8) = MH_REFS;
      *(u32 *)(e->obj + 0x78) = MH_MARK;
      e->valid = 0, e->used = 0, e->filled = 0;
    }
  u64 us = sceKernelGetProcessTime() - t0;
  LOG("gaze: movehole: %ux%u tiles a mask, %u bytes tiled (the game's T# tiling, its own tiler); fov %f %f "
      "%f %f, the game's hole radius %f (r2 %f over f2^2, Pro mode %d, factor %f); the agent's centred masks "
      "against the game's: L %s %u differ (first at %u), R %s %u (first at %u): %s; the moved hole's radius "
      "%f (r2 %f; margin %f, the hold %f tile diagonals = %f, tangent); %lu us",
      g_mh.w, g_mh.h, g_mh.extent, f[0], f[1], f[2], f[3], g_mh.rad, g_mh.r2game, g_gz_neo, factor,
      brc[0] ? "build failed," : "", diff[0], first[0], brc[1] ? "build failed," : "", diff[1], first[1],
      g_mh.verified ? "the same, byte for byte"
      : g_mh_opt == 2 ? "NOT the same: agent.movehole=2 copies the game's bytes instead"
                      : "NOT the same: the hole is not moved",
      __builtin_sqrtf(g_mh.r2move) * f[2], g_mh.r2move, MH_MARGIN, MH_HOLD, g_mh.qerr_t, us);
  if (!g_mh.verified && g_mh_opt != 2)
    return 0;
  g_mh.state = 1;
  return 1;
}

/* the game's state as found: its pointers (the game's or the agent's put there), its objects, the
 * values its masks came from, the agent's objects untouched by its fill; else the reason */
static const char *mh_changed(u8 *hmd) {
  for (int eye = 0; eye < 2; eye++) {
    u8 *p = __atomic_load_n((u8 **)(g_mh.vrr + VRR_MASKS + eye * 8), __ATOMIC_ACQUIRE);
    if (p != (g_mh.cur[eye] < 0 ? g_mh.gobj[eye] : g_mh.e[eye][g_mh.cur[eye]].obj))
      return "the game's mask pointer changed (VR torn down or made again)";
    if (memcmp(g_mh.gobj[eye] + 0x10, g_mh.gcopy[eye] + 0x10, 0x30) != 0 || /* (its T# and sampler) */
        memcmp(g_mh.gobj[eye] + 0x44, g_mh.gcopy[eye] + 0x44, 8) != 0)      /* (its size) */
      return "the game's mask object changed";
    for (int j = 0; j < MH_RING; j++)
      if (*(volatile u32 *)(g_mh.e[eye][j].obj + 0x78) != MH_MARK)
        return "the game's fill ran on the agent's mask object (VR started again)";
  }
  if (memcmp(hmd + HMD_FOV, g_mh.fov, sizeof g_mh.fov) != 0 || *(const float *)(hmd + 0x210) != g_mh.rad)
    return "the field of view or the near view's size changed";
  return 0;
}

/* the frame's masks for slot s: the hole centred at c (tangent, x right, y up; agent.movehole=2:
 * the axis). 1: put in, *hole = their centre (tangent, packed); 0: the game's own masks put back
 * (the caller centres the near view) */
static int mh_frame(const struct gz_build *b, u8 *base, u8 *hmd, u32 s, const float c[2], u64 *hole) {
  if (!g_mh.state)
    mh_init(b, base, hmd);
  if (g_mh.state != 1)
    return 0;
  const char *why = mh_changed(hmd);
  if (why) {
    mh_off(s, why);
    return 0;
  }
  g_mh.frame++;
  float f2 = g_mh.fov[2];
  s32 kx = 0, ky = 0;
  if (g_mh_opt == 1) {
    float ux = c[0] / f2, uy = -c[1] / f2;
    kx = mh_round(ux / g_mh.stepx), ky = mh_round(uy / g_mh.stepy);
    /* the hole held where it is while the centre stays within MH_HOLD diagonals of it (its radius
     * is that much smaller, and the hand-over checks that distance) */
    if (g_mh.hold && (kx != g_mh.hold_kx || ky != g_mh.hold_ky)) {
      float dx = ux - (float)g_mh.hold_kx * g_mh.stepx, dy = uy - (float)g_mh.hold_ky * g_mh.stepy;
      if (dx * dx + dy * dy <= g_mh.hold2) {
        kx = g_mh.hold_kx, ky = g_mh.hold_ky;
        g_st.mh_held++;
      }
    }
  }
  float hx = (float)kx * g_mh.stepx, hy = (float)ky * g_mh.stepy;
  float r2 = g_mh_opt == 2 ? g_mh.r2game : g_mh.r2move;
  int js[2];
  for (int eye = 0; eye < 2; eye++) {
    int j = -1, best = -1;
    if (g_mh_opt == 1) /* (the test rebuilds every frame) */
      for (int i = 0; i < MH_RING; i++)
        if (g_mh.e[eye][i].valid && g_mh.e[eye][i].kx == kx && g_mh.e[eye][i].ky == ky)
          j = i;
    if (j >= 0) {
      g_st.mh_reused++;
    } else {
      for (int i = 0; i < MH_RING; i++) /* the least recently used of those no frame in flight has */
        if (i != g_mh.rec[0][eye] && i != g_mh.rec[1][eye] &&
            (best < 0 || g_mh.e[eye][i].used < g_mh.e[eye][best].used))
          best = i;
      if (best < 0) {
        mh_restore(s);
        return 0;
      }
      struct mh_entry *e = &g_mh.e[eye][best];
      e->valid = 0;
      u64 t0 = sceKernelGetProcessTime();
      s32 rc = 0;
      if (!g_mh.verified) { /* (agent.movehole=2 only: the game's bytes) */
        memcpy(e->mem, g_mh.gmem[eye], g_mh.extent);
        e->filled = 0;
      } else if (g_mh.fast && mh_spans(eye, hx, hy, r2, g_mh.na, g_mh.nb)) {
        if (e->filled) /* (only the tiles that change: the entry's hole as recorded) */
          mh_move(eye, e->mem, e->sa, e->sb, g_mh.na, g_mh.nb), g_st.mh_diff++;
        else
          mh_fill(eye, e->mem, g_mh.na, g_mh.nb), g_st.mh_full++;
        memcpy(e->sa, g_mh.na, g_mh.h * 2), memcpy(e->sb, g_mh.nb, g_mh.h * 2);
        e->filled = 1;
      } else { /* (the map not usable, or a row's hole not one span: the game's tiler, whole) */
        e->filled = 0;
        rc = mh_build(eye, hx, hy, r2, e->mem);
        g_st.mh_slow++;
      }
      u32 us = (u32)(sceKernelGetProcessTime() - t0);
      g_st.mh_builds++, g_st.mh_us_sum += us;
      if (us > g_st.mh_us_max)
        g_st.mh_us_max = us;
      if (rc) {
        static u32 logged;
        if (logged++ < 4)
          LOG("gaze: movehole: tiling the wide %s mask -> 0x%x: the game's masks this frame", eye ? "R" : "L",
              rc);
        mh_restore(s);
        return 0;
      }
      e->kx = kx, e->ky = ky, e->valid = 1;
      j = best;
    }
    g_mh.e[eye][j].used = g_mh.frame;
    js[eye] = j;
  }
  for (int eye = 0; eye < 2; eye++) {
    mh_put(eye, js[eye]);
    g_mh.rec[s][eye] = js[eye];
  }
  g_mh.hold = 1, g_mh.hold_kx = kx, g_mh.hold_ky = ky;
  union { float f[2]; u64 u; } pk = {{hx * f2, -hy * f2}};
  *hole = pk.u;
  g_st.mh_frames++;
  static u32 logged;
  if (logged < 4) {
    logged++;
    LOG("gaze: movehole: frame %u, slot %u: masks L %d R %d put in (vrr+0x108 %p, +0x110 %p), the hole at "
        "tiles %d, %d = tangent %f, %f for the centre %f, %f", g_mh.frame, s, js[0], js[1],
        g_mh.e[0][js[0]].obj, g_mh.e[1][js[1]].obj, kx, ky, pk.f[0], pk.f[1], c[0], c[1]);
  }
  return 1;
}

void gaze_frame(const u8 *ret) {
  if ((!g_gz_fixed && !g_gz_follow && g_mh_opt != 2) || g_gz_build == -1)
    return;
  if (g_gz_build < 0) {
    int b = identify(ret);
    if (b == -2)
      return;
    g_gz_build = b;
    if (b < 0)
      return;
  } else if (ret != (u8 *)astro_hdr.game_base + k_gz[g_gz_build].ret)
    return;
  {
    /* every 10 s the figures handed to the renderer's thread to print (no log line here) */
    static u64 next;
    u64 now = sceKernelGetProcessTime();
    if (!next)
      next = now + 10000000;
    if (now >= next && !__atomic_load_n(&g_rep.due, __ATOMIC_ACQUIRE)) {
      g_rep.s = g_st;
      __atomic_store_n(&g_rep.due, 1, __ATOMIC_RELEASE);
      memset(&g_st, 0, sizeof g_st);
      next = now + 10000000;
    }
  }
  const struct gz_build *b = &k_gz[g_gz_build];
  u8 *base = (u8 *)astro_hdr.game_base, *hmd = base + b->hmd;
  u32 s = *(volatile u32 *)(base + b->slot);
  float n = *(volatile float *)(hmd + HMD_N), top = *(volatile float *)(hmd + HMD_FOV + 8);
  static u32 bad_logged, written_logged, checked;
  float *ml = s < 2 ? (float *)(hmd + s * SLOT_SIZE + NEAR_L) : 0;
  float *mr = s < 2 ? (float *)(hmd + s * SLOT_SIZE + NEAR_R) : 0;
  float dn = n - viewsize_near_factor() * top; /* (the game's 0.5, or agent.nearn's: the payload's) */
  if (s >= 2 || !(n > 0.1f && n < 1.5f) || !(dn < 1e-4f && dn > -1e-4f) || !near_ok(ml, n, g_last[s][0]) ||
      !near_ok(mr, n, g_last[s][1])) {
    g_st.bad++;
    if (bad_logged < 4) {
      bad_logged++;
      LOG("gaze: slot %u, n %f, tan top %f: the near projections not as expected (L row 3 %f %f %f %f, "
          "R %f %f %f %f): nothing written this frame", s, n, top, ml ? ml[8] : 0.0f, ml ? ml[9] : 0.0f,
          ml ? ml[10] : 0.0f, ml ? ml[11] : 0.0f, mr ? mr[8] : 0.0f, mr ? mr[9] : 0.0f, mr ? mr[10] : 0.0f,
          mr ? mr[11] : 0.0f);
    }
    mh_restore(s); /* (the game's own masks, should the agent's be in) */
    return;
  }
  if (!checked) {
    checked = 1;
    self_check(hmd, s);
  }
  g_gz_n = n;
  float c[2] = {0.0f, 0.0f};
  if (g_gz_follow) {
    follow(c);
    g_st.follow++;
  } else if (g_gz_fixed) {
    c[0] = g_gz_off[0], c[1] = g_gz_off[1];
    g_st.fixed++;
  }
  if (g_gz_mode == GZ_SAMEHOLE)
    c[0] = c[1] = 0.0f; /* (agent.movehole=2: the near view as the game has it) */
  if (!g_gz_can_move) { /* (the hole on: nothing written, the game's centred views as they are) */
    g_st.refused++;
    return;
  }
  /* the sharp disc inside the lens circle: |c| <= tan top - n */
  float r = top - n, l2 = c[0] * c[0] + c[1] * c[1];
  if (r <= 0.0f)
    c[0] = c[1] = 0.0f;
  else if (l2 > r * r) {
    float k = r / __builtin_sqrtf(l2);
    c[0] *= k, c[1] *= k;
    g_st.clamped++;
  }
  /* agent.movehole: the wide views' masks for this centre, before the draw reads them; any failure
   * leaves the game's own (centred) masks in, and the near view is then centred with them */
  u64 hole = 0;
  u32 hv = 0;
  if (g_gz_mode == GZ_MOVEHOLE || g_gz_mode == GZ_SAMEHOLE) {
    hv = (u32)mh_frame(b, base, hmd, s, c, &hole);
    if (!hv) {
      if (c[0] != 0.0f || c[1] != 0.0f)
        g_st.mh_centred++;
      c[0] = c[1] = 0.0f;
    }
  }
  float x = -c[0] / n, y = -c[1] / n;
  if (c[0] == 0.0f)
    x = 0.0f; /* (+0, never -0: as compared above) */
  if (c[1] == 0.0f)
    y = 0.0f;
  /* the record first marked as being rewritten, then the matrices, then the record */
  union { float f[2]; u64 u; } pk = {{c[0], c[1]}};
  __atomic_store_n(&g_rec[s].seq, 0, __ATOMIC_RELEASE);
  ((volatile float *)ml)[8] = x, ((volatile float *)ml)[9] = y;
  ((volatile float *)mr)[8] = x, ((volatile float *)mr)[9] = y;
  g_last[s][0][0] = g_last[s][1][0] = x, g_last[s][0][1] = g_last[s][1][1] = y;
  __atomic_store_n(&g_rec[s].c, pk.u, __ATOMIC_RELEASE);
  __atomic_store_n(&g_rec[s].h, hole, __ATOMIC_RELEASE);
  __atomic_store_n(&g_rec[s].hv, hv, __ATOMIC_RELEASE);
  u32 seq = __atomic_add_fetch(&g_gz_seq, 1, __ATOMIC_RELAXED);
  __atomic_store_n(&g_rec[s].seq, seq, __ATOMIC_RELEASE);
  if (c[0] != 0.0f || c[1] != 0.0f)
    __atomic_store_n(&g_gz_moved, 1, __ATOMIC_RELEASE);
  note_centre(c);
  if (written_logged < 4) {
    written_logged++;
    LOG("gaze: frame %u, slot %u: the near views' row 3 written (%f, %f) for the centre (%f, %f), n %f",
        seq, s, x, y, c[0], c[1], n);
  }
}

/* ---- the VrRenderer thread, at the hand-over ---- */
static float g_ho[2];
static u32 g_ho_set;
static struct {
  u32 n, used, centred, bad_label, unwritten, old, torn, differs;
  u32 hole_off, hole_none; /* agent.movehole: the hole farther than half a tile from the near centre;
                            * a moved centre drawn with the game's centred masks (both should be 0) */
} g_ho_st;

void gaze_handover(const void *frame_label) {
  g_ho_set = 0;
  if (!g_gz_fixed && !g_gz_follow && !g_mh_opt)
    return;
  if (__atomic_load_n(&g_rep.due, __ATOMIC_ACQUIRE)) {
    const struct gz_stats *s = &g_rep.s;
    LOG_DETAIL("gaze: last 10 s, %u frames written (fixed %u, following %u: %u fresh gaze, %u stale eased "
               "back, %u jumps), %u kept centred (the hole on), %u clamped to the lens circle, %u not "
               "written (projections not as expected); the centre x %f..%f, y %f..%f; hand-overs %u: "
               "%u with a recorded centre, %u centred, labels unknown %u, slot never written %u, record "
               "older than the last two frames %u, torn %u, record != the slot's matrix %u",
               s->frames, s->fixed, s->follow, s->fresh, s->stale, s->jumps, s->refused, s->clamped, s->bad,
               s->xmin, s->xmax, s->ymin, s->ymax, g_ho_st.n, g_ho_st.used, g_ho_st.centred,
               g_ho_st.bad_label, g_ho_st.unwritten, g_ho_st.old, g_ho_st.torn, g_ho_st.differs);
    if (g_mh_opt) {
      u32 asked = s->mh_builds + s->mh_reused;
      LOG_DETAIL("gaze: movehole last 10 s: %u frames with the agent's masks, %u masks built (%lu us mean, "
                 "%u us most, an eye; %u filled through the map, %u by their changed tiles, %u by the game's "
                 "tiler), %u reused (cache hits %u%%), %u frames the hole held (its nearest tile moved), %u "
                 "moved centres kept centred (the game's masks); hand-overs with the hole off its centre %u, "
                 "moved without the agent's masks %u",
                 s->mh_frames, s->mh_builds, s->mh_builds ? s->mh_us_sum / s->mh_builds : 0, s->mh_us_max,
                 s->mh_full, s->mh_diff, s->mh_slow, s->mh_reused, asked ? s->mh_reused * 100 / asked : 0,
                 s->mh_held, s->mh_centred, g_ho_st.hole_off, g_ho_st.hole_none);
    }
    memset(&g_ho_st, 0, sizeof g_ho_st);
    __atomic_store_n(&g_rep.due, 0, __ATOMIC_RELEASE);
  }
  g_ho_st.n++;
  static u32 logged;
  u64 l = (u64)frame_label;
  u32 s = (u32)(l & 0xff) >> 3;
  if (logged < 3) {
    logged++;
    LOG("gaze: hand-over label %p (slot %u by its place), records: slot 0 seq %u, slot 1 seq %u", frame_label,
        s, g_rec[0].seq, g_rec[1].seq);
  }
  if (!__atomic_load_n(&g_gz_moved, __ATOMIC_ACQUIRE)) { /* nothing ever off the axis */
    g_ho_st.centred++;
    return;
  }
  if (!frame_label || (l & 7) || s > 1) {
    g_ho_st.bad_label++;
    return;
  }
  u32 q0 = __atomic_load_n(&g_rec[s].seq, __ATOMIC_ACQUIRE);
  union { float f[2]; u64 u; } pk, hk;
  pk.u = __atomic_load_n(&g_rec[s].c, __ATOMIC_ACQUIRE);
  hk.u = __atomic_load_n(&g_rec[s].h, __ATOMIC_ACQUIRE);
  u32 hv = __atomic_load_n(&g_rec[s].hv, __ATOMIC_ACQUIRE);
  u32 q1 = __atomic_load_n(&g_rec[s].seq, __ATOMIC_ACQUIRE);
  if (!q0 || q0 != q1) {
    if (!q0 && !q1)
      g_ho_st.unwritten++;
    else
      g_ho_st.torn++;
    return; /* (the main thread rewriting this slot now: the report says it cannot) */
  }
  if (q0 + 2 <= __atomic_load_n(&g_gz_seq, __ATOMIC_RELAXED))
    g_ho_st.old++; /* (counted; the matrix still holds this centre: it is the slot's last write) */
  /* the slot's matrix, as drawn, against the record */
  u64 h = __atomic_load_n(&g_gz_hmd_addr, __ATOMIC_ACQUIRE);
  float n = g_gz_n;
  if (h && n > 0.0f) {
    const volatile float *m = (const volatile float *)(h + s * SLOT_SIZE + NEAR_L + ROW2);
    float dx = -m[0] * n - pk.f[0], dy = -m[1] * n - pk.f[1];
    if (!(dx < 1e-4f && dx > -1e-4f && dy < 1e-4f && dy > -1e-4f))
      g_ho_st.differs++;
  }
  if (g_gz_mode == GZ_MOVEHOLE) { /* the frame's hole against its near centre */
    float ex = hk.f[0] - pk.f[0], ey = hk.f[1] - pk.f[1], lim = g_mh.qerr_t + 1e-4f;
    if (!hv && (pk.f[0] != 0.0f || pk.f[1] != 0.0f))
      g_ho_st.hole_none++;
    else if (hv && ex * ex + ey * ey > lim * lim)
      g_ho_st.hole_off++;
  }
  g_ho[0] = pk.f[0], g_ho[1] = pk.f[1], g_ho_set = 1;
  g_ho_st.used++;
}

/* compose.c, right after: the centre the frame being handed over was drawn with (x right, y up); 0
 * when it is the axis (nothing to shift) */
int gaze_compose_centre(float c[2]) {
  if (!g_ho_set || (g_ho[0] == 0.0f && g_ho[1] == 0.0f))
    return 0;
  c[0] = g_ho[0], c[1] = g_ho[1];
  return 1;
}
