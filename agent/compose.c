/* Dead or Alive Xtreme 3's eyes made whole for PS VR2 (tools/gpu/compose.c, the shader).
 * The game hands each eye over twice (sceHmdReprojectionStartWideNear): a wide view over the
 * whole field of view with a black disc in its middle, and a near view over the middle, twice as
 * dense, which PS VR's reprojection put into the disc. Their alpha holds nothing usable, so they
 * cannot go to Hmd2 as two layers; each eye is written here into one texture of the agent's own:
 * the wide view's field of view at the near view's density, the near view inside the blend's inner
 * tangent, the wide view outside its outer one, a linear blend between (the two floats of the
 * game's parameter). Hmd2 then gets two plain 2D textures an eye pair, as from any game.
 *
 * One compute buffer on Hmd2's queue a frame (acb.c): a dispatch an eye, then a barrier, so Hmd2's
 * work, submitted after it for that frame, reads the stores. It goes at the hand-over: the game's
 * GPU has finished the frame (its command stream ends with an end-of-pipe event and waits for the
 * hand-over to return) and draws its next into its other set of views. NSET sets of outputs, taken
 * in turn. They are made at the game's first such frame and registered as VR port buffers by vr2.c
 * in one go (compose_outputs, compose_registered); frames are composed from then on.
 *
 * The outputs are tiled as the game's own views (its T# with our base, size and pitch, one slice,
 * the read-only memory type cleared: stores through that are dropped, inset.c), in the game's
 * direct memory range. Nothing here before Hmd2's queue is up (gen1_queue_ready): until then, and
 * whenever a frame cannot be composed, the caller hands the wide views over as they are.
 *
 * agent.sharpen=N, agent.sharpenwide=N (debug, live: cvars.txt read again every 2 s; 0 off, the
 * default; 1..100): the near view's, the wide view's samples sharpened as they are composed (a
 * contrast-adaptive sharpen on the cross of a view's texels, AMD FidelityFX CAS's: the shader's
 * comment), the neighbours' weight at the most N / 500 (100: CAS's sharpest, 1 / 5). Against the
 * softness of the game's FXAA and the two resamplings after it (here, then the headset's lens).
 *
 * agent.composefast=N (debug, live; 3 by default, and always 3 in a release build): the pass made
 * cheaper without changing the picture (logs/research/gpu120/COMPOSE-COST.md). 1: the texels beyond
 * the lens circle black and not sampled, their black stored once an output (the game's wide view
 * is black there: its mask leaves those tiles undrawn; the circle is the game's, 1.0 x the wide
 * view's top tangent, plus a margin for the mask's tiles and the resamplings, LENS_MARGIN); 2: each
 * view sampled only where the blend uses it. 0: the pass as it was (an A/B of the GPU time).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "vr2.h"
#include "psvr1.h"

#include "compose_gcn.h"

#define NSET 4 /* (the frame ring is four deep: a frame still to be handed over keeps its outputs) */
#define SHADER_SPACE 0x1000
/* the outputs' size at the most: agent.composemax wide (4096 by default; 1600-wide views compose
 * 3360 wide, 1800 3784, both shown clean on PS VR2), and as many rows as that in proportion, but
 * never more than 4096 rows: 2048-wide views composed 4096x4368 showed the wide view's black disc
 * in the eye's middle (the near view drawn, checked in its slice; every frame composed), and the
 * same views composed 3840x4096 are whole (2026-11-12) */
#define OUT_W_MAX g_opt_composemax
#define OUT_H_MAX (g_opt_composemax * 9 / 8 < 4096u ? g_opt_composemax * 9 / 8 : 4096u)
_Static_assert(sizeof k_compose_gcn <= SHADER_SPACE, "the compose shader outgrew its space");
/* the lens circle's margin beyond the game's (1.0 x the top tangent), in texels of the wide view's
 * slice, each way: the game's mask draws a tile of 8 x 8 when any of its corners (a pixel outside
 * the tile) is within the circle, so drawn texels reach about 9 x sqrt 2 = 13 past it; the wide
 * view drawn smaller than its slice (agent.widew) and FXAA's upsampling into the slice add a few
 * more, and the compose's bilinear sample one (logs/research/gpu120/COMPOSE-COST.md) */
#define LENS_MARGIN 24.0f
/* the shader's mode bits (tools/gpu/compose.c) */
#define COMPOSE_LENS 1
#define COMPOSE_FILL 2
#define COMPOSE_NEED 4

/* struct compose_k in tools/gpu/compose.c, the same layout */
struct consts {
  u32 wide[8], near[8], out[8], samp[4];
  float wu0, wdu, wv0, wdv;
  float nu0, ndu, nv0, ndv;
  float tx0, tdx, ty0, tdy;
  float inner, inv_band;
  s32 w, h;
  float wsu, wsv, nsu, nsv; /* a texel of each view in u and v (the sharpen's neighbours) */
  float wpeak, npeak;       /* the sharpen's weight at the most, -N / 500; 0 off */
  float lx0, ly0;           /* the eye's own tangents at texel (0, 0) (not moved with the near view) */
  float lens2;              /* the lens circle's radius with its margin, squared */
  s32 mode;                 /* COMPOSE_* */
  u32 pad[2]; /* (the shader's struct: 32-byte aligned, as its T#s) */
};
_Static_assert(sizeof(struct consts) == 224, "compose constants");
_Static_assert(__builtin_offsetof(struct consts, samp) == 96, "compose constants");
_Static_assert(__builtin_offsetof(struct consts, wu0) == 112, "compose constants");
_Static_assert(__builtin_offsetof(struct consts, w) == 168, "compose constants");
_Static_assert(__builtin_offsetof(struct consts, wsu) == 176, "compose constants");
_Static_assert(__builtin_offsetof(struct consts, lx0) == 200, "compose constants");
_Static_assert(__builtin_offsetof(struct consts, mode) == 212, "compose constants");

/* agent.sharpen (0 by default; both builds: the settings menu's "Sharpening", live; cvars.txt at the
 * start, and live in a debug build) and agent.sharpenwide (debug: hooks.c, at the start and live) */
volatile u32 g_opt_sharpen = 50, g_opt_sharpenwide; /* (sharpening 50 by default: the user, 2026-11-14) */
/* agent.composefast (hooks.c: at the start and live): 1 the lens circle, 2 fetch on need; always
 * 3 in a release build */
volatile u32 g_opt_composefast = 3;

struct __attribute__((aligned(256))) slot {
  struct consts k[2];
  u32 cb[128];
  volatile u32 label; /* seq * 8 + 4 when done */
  u32 pad;
  volatile u64 ts[2]; /* the GPU's clock at the start and the end */
};

static struct {
  u8 *mem; /* the shader, then the slots */
  u32 w, h, pitch;
  u32 tsharp[NSET][2][8];
  u32 src[4]; /* the views' size and format the outputs were made for (T# words 1-4, masked) */
  u8 *out0;   /* the first output (the stores' check) */
  size_t out_bytes;
  volatile u32 made, registered; /* the outputs exist; vr2.c has registered them */
  u32 seq, off, failed, busy, n, n_gpu;
  u32 wide_only; /* the last frame was the wide view alone (the debug menu's "Compose" off live) */
  u32 sharp[2]; /* the sharpen (near, wide) the last frame was composed with (the report) */
  u32 fast;     /* agent.composefast the last frame was composed with */
  /* each output's lens constants when its black was stored (lx0, ly0, tdx, tdy, lens2); all 0
   * until then: an output whose lens differs gets COMPOSE_FILL */
  u32 filled[NSET][2][5];
  u32 lens_logged;
  u64 report_at, s_gpu, gpu_max, cal_cpu, cal_gpu, done_cpu, done_gpu;
} g_c;

static struct slot *slot(u32 i) { return (struct slot *)(g_c.mem + SHADER_SPACE) + i; }

static u32 build(struct slot *s, u32 seq) {
  u32 *cb = s->cb, n = 0;
  u64 shader = (u64)g_c.mem;
  n = acb_acquire(cb, n, 1); /* the constants and the commands were written by the CPU */
  cb[n++] = acb_pm4(0x76, 4), cb[n++] = 0x204, cb[n++] = 0, cb[n++] = 0, cb[n++] = 0; /* START */
  cb[n++] = acb_pm4(0x76, 4), cb[n++] = 0x207, cb[n++] = 8, cb[n++] = 8, cb[n++] = 1; /* NUM_THREAD */
  cb[n++] = acb_pm4(0x76, 3), cb[n++] = 0x20c; /* PGM_LO/HI */
  cb[n++] = (u32)(shader >> 8), cb[n++] = (u32)(shader >> 40);
  u32 rsrc1 = ((K_COMPOSE_GCN_VGPRS - 1) / 4) | (((K_COMPOSE_GCN_SGPRS - 1) / 8) << 6) |
              (0xc0u << 12) | (1u << 21);
  u32 rsrc2 = (2u << 1) | (1u << 7) | (1u << 8) | (1u << 11);
  cb[n++] = acb_pm4(0x76, 3), cb[n++] = 0x212, cb[n++] = rsrc1, cb[n++] = rsrc2;
  n = acb_timestamp(cb, n, (u64)&s->ts[0]);
  for (int e = 0; e < 2; e++) {
    u64 k = (u64)&s->k[e];
    cb[n++] = acb_pm4(0x76, 3), cb[n++] = 0x240, cb[n++] = (u32)k, cb[n++] = (u32)(k >> 32);
    cb[n++] = acb_pm4(0x15, 4);
    cb[n++] = (g_c.w + 7) / 8, cb[n++] = (g_c.h + 7) / 8, cb[n++] = 1, cb[n++] = 0x41;
  }
  n = acb_timestamp(cb, n, (u64)&s->ts[1]);
  return acb_barrier(cb, n, (u64)&s->label, seq * 8 + 4);
}

/* The outputs, once Hmd2's queue is up, for views like `wide` with these mappings; 1 when ready.
 * Their size: the wide view's field of view at the near view's texels a unit of tangent (a
 * view's width times its u a tangent), each way; within the reprojection's limits, kept in
 * proportion. */
static int ensure(const u32 *wide, const struct hmd_eye_uv *wuv, const struct hmd_eye_uv *nuv) {
  if (g_c.off)
    return 0;
  u32 vw = (wide[2] & 0x3fff) + 1, vh = ((wide[2] >> 14) & 0x3fff) + 1;
  u32 src[4] = {wide[1] & ~0xffu, wide[2], wide[3], wide[4] & ~0x1fffu};
  if (g_c.mem) {
    if (memcmp(src, g_c.src, sizeof src) == 0)
      return 1;
    LOG("compose: the game's views changed (%ux%u): off", vw, vh);
    g_c.off = 1;
    return 0;
  }
  u32 dfmt = (wide[1] >> 20) & 0x3f, type = wide[3] >> 28;
  if (dfmt != 10 || (type != 13 && type != 9) || wuv->scale_x <= 0.0f || wuv->scale_y <= 0.0f ||
      nuv->scale_x <= 0.0f || nuv->scale_y <= 0.0f) {
    LOG("compose: views of format %u type %u, or their mappings, not as taken: off", dfmt, type);
    g_c.off = 1;
    return 0;
  }
  float fw = (float)vw * nuv->scale_x / wuv->scale_x, fh = (float)vh * nuv->scale_y / wuv->scale_y;
  if (fw > (float)OUT_W_MAX)
    fh *= (float)OUT_W_MAX / fw, fw = (float)OUT_W_MAX;
  if (fh > (float)OUT_H_MAX)
    fw *= (float)OUT_H_MAX / fh, fh = (float)OUT_H_MAX;
  u32 w = ((u32)fw + 7) & ~7u, h = ((u32)fh + 7) & ~7u;
  if (w < vw || h < vh || w > OUT_W_MAX || h > OUT_H_MAX) {
    LOG("compose: an output of %ux%u from views of %ux%u: off", w, h, vw, vh);
    g_c.off = 1;
    return 0;
  }
  /* (as the game's own: the pitch and the rows up to a multiple of the macro tile's 128) */
  u32 pitch = (w + 127) & ~127u;
  size_t bytes = (size_t)pitch * ((h + 127) & ~127u) * 4;
  /* The shader and the slots in one piece; each output a piece of its own (cleared: black, alpha
   * 0), in the container's extra segment as far as it has room (its spare part, otherwise only
   * the game's blocks refused in its own range go there), then in the game's range. In one piece
   * of 496 MB in the game's range, eye width 2720 found no room: the game's views took a fifth
   * 512 MB pool block (2026-11-13, logs/perf/ss-2720.log). */
  size_t head = (SHADER_SPACE + NSET * sizeof(struct slot) + 0xffff) & ~(size_t)0xffff;
  u8 *mem = vr2_dalloc(head, 0x10000), *outs[NSET][2];
  u32 in_spare = 0, found = 0;
  for (int i = 0; i < NSET && mem; i++)
    for (int e = 0; e < 2 && mem; e++) {
      outs[i][e] = vr2_dalloc_range(bytes, 0x10000, g_reserve.spare_lo, g_reserve.spare_hi);
      if (outs[i][e])
        in_spare++, found++;
      else if ((outs[i][e] = vr2_dalloc(bytes, 0x10000)))
        found++;
      else
        mem = 0;
    }
  if (!mem) {
    LOG("compose: no memory for the pass and its outputs (%u of %d outputs of %lu MB found, %u in the "
        "extra segment): off (the pieces found stay unused)", found, 2 * NSET, (u64)bytes >> 20, in_spare);
    g_c.off = 1;
    return 0;
  }
  LOG("compose: %d outputs of %lu MB, %u in the extra segment", 2 * NSET, (u64)bytes >> 20, in_spare);
  g_c.out0 = outs[0][0], g_c.out_bytes = bytes;
  for (int i = 0; i < NSET; i++)
    for (int e = 0; e < 2; e++) {
      u8 *tex = outs[i][e];
      u32 *t = g_c.tsharp[i][e];
      u64 a = (u64)tex;
      memcpy(t, wide, 32);
      t[0] = (u32)(a >> 8);
      t[1] = (t[1] & ~0xffu & ~(0xfu << 26)) | (u32)((a >> 40) & 0xff); /* UNORM: the values as they are */
      t[2] = (t[2] & 0xf0000000u) | (w - 1) | ((h - 1) << 14);
      t[3] = (t[3] & 0x0fffffffu & ~(1u << 26)) | (9u << 28); /* 2D; not the read-only memory type */
      t[4] = (pitch - 1) << 13;
      t[5] &= ~0x3ffffffu; /* no slice */
    }
  memcpy(mem, k_compose_gcn, sizeof k_compose_gcn);
  g_c.w = w, g_c.h = h, g_c.pitch = pitch;
  memcpy(g_c.src, src, sizeof src);
  for (u32 i = 0; i < NSET; i++)
    ((struct slot *)(mem + SHADER_SPACE) + i)->label = 0;
  g_c.mem = mem;
  __atomic_store_n(&g_c.made, 1, __ATOMIC_RELEASE);
  LOG("compose: each eye %ux%u (pitch %u, %lu KB) from its wide and near view (%ux%u each), %d sets",
      w, h, pitch, (u64)bytes >> 10, vw, vh, NSET);
  return 1;
}

static void report(u64 now) {
  if (!g_c.report_at) {
    g_c.report_at = now + 10000000;
    return;
  }
  if (now < g_c.report_at)
    return;
  u64 gpu_hz = 0;
  if (g_c.cal_cpu && g_c.done_cpu > g_c.cal_cpu + 1000000 && g_c.done_gpu > g_c.cal_gpu)
    gpu_hz = (g_c.done_gpu - g_c.cal_gpu) * 1000000 / (g_c.done_cpu - g_c.cal_cpu);
  u64 avg = g_c.n_gpu ? g_c.s_gpu / g_c.n_gpu : 0;
  LOG("compose: %u frames composed in the last 10 s, %u handed over as wide views (the pass busy); "
      "the GPU took %lu us a frame (longest %lu); sharpen %u, wide %u, fast %u", g_c.n, g_c.busy,
      gpu_hz ? avg * 1000000 / gpu_hz : 0, gpu_hz ? g_c.gpu_max * 1000000 / gpu_hz : 0,
      g_c.sharp[0], g_c.sharp[1], g_c.fast);
  g_c.n = g_c.busy = g_c.n_gpu = 0, g_c.s_gpu = g_c.gpu_max = 0;
  g_c.report_at = now + 10000000;
}

/* vr2.c: the output pairs' T#s to register as VR port buffers, once made */
u32 compose_outputs(const u32 (**tsharps)[2][8]) {
  if (!__atomic_load_n(&g_c.made, __ATOMIC_ACQUIRE) || g_c.off)
    return 0;
  *tsharps = (const u32(*)[2][8])g_c.tsharp;
  return NSET;
}
void compose_registered(int ok) {
  if (ok)
    __atomic_store_n(&g_c.registered, 1, __ATOMIC_RELEASE);
  else
    g_c.off = 1;
}
/* vr2.c: the outputs are registered and the pass is off for good (the GPU stopped finishing it,
 * its submits failed, the views changed): the headset has no frame it can show but the last one */
int compose_stopped(void) {
  return __atomic_load_n(&g_c.registered, __ATOMIC_ACQUIRE) && g_c.off;
}

/* A frame of the game's (textures: wide left, near left, wide right, near right; their mappings
 * likewise; the blend's inner and outer tangent): composed into `p`'s two textures and mappings
 * (the wide views'), 1; 0 when it is not (the caller hands the wide views over). */
int compose_frame(const void *const tex[4], const void *sampler, const struct hmd_eye_uv uv[4],
                  float inner, float outer, struct hmd_reprojection_param *p) {
  if (!tex[0] || !tex[1] || !tex[2] || !tex[3] || !sampler || !(outer > inner))
    return 0;
  /* agent.compose=0 (the debug menu's "Compose" off): before the outputs are registered, the wide
   * views as they are (registered as the game's own pairs). After, the VR port has no room for
   * them (it takes two sets: the game's pairs and these outputs; a third is refused, 0x8029000a),
   * and the headset would keep its last frame until "Compose" is on again: so the wide view alone
   * goes through the pass (the blend's band moved inside the centre, 0 to 1 wide everywhere: with
   * agent.composefast's 2 the near view is not sampled), the same picture as agent.compose=0 from
   * the start (the black disc in the middle), into the registered outputs */
  u32 wide_only = 0;
  if (g_opt_compose == 0) {
    if (!__atomic_load_n(&g_c.registered, __ATOMIC_ACQUIRE))
      return 0;
    wide_only = 1;
    inner = -2.0f, outer = -1.0f;
  }
  if (wide_only != g_c.wide_only && !g_c.off) {
    LOG("compose: %s", wide_only ? "off in the menu with the outputs registered: the wide view alone "
                                   "through the pass (the headset keeps getting frames)"
                                 : "on again: the wide and near views composed");
    g_c.wide_only = wide_only;
  }
  if (!ensure(tex[0], &uv[0], &uv[1]))
    return 0;
  /* (made; composed only once Hmd2's queue is up and vr2.c has registered the outputs) */
  if (!gen1_queue_ready() || !__atomic_load_n(&g_c.registered, __ATOMIC_ACQUIRE))
    return 0;
  u64 now = sceKernelGetProcessTime();
  report(now);
  u32 next = g_c.seq + 1;
  struct slot *s = slot(next % NSET);
  if (next > NSET) { /* the slot's (and its outputs') last frame done on the GPU */
    u32 before = next - NSET;
    if (s->label != before * 8 + 4) {
      g_c.busy++;
      if (g_c.busy > 120 && g_c.n == 0) {
        LOG("compose: the GPU hasn't finished a pass for 120 frames: off");
        g_c.off = 1;
      }
      return 0;
    }
    if (s->ts[1] > s->ts[0]) {
      u64 d = s->ts[1] - s->ts[0];
      g_c.s_gpu += d, g_c.n_gpu++, g_c.gpu_max = d > g_c.gpu_max ? d : g_c.gpu_max;
      g_c.done_cpu = now, g_c.done_gpu = s->ts[1];
    }
  }
  u32 seq = ++g_c.seq, set = seq % NSET;
  float fw = (float)g_c.w, fh = (float)g_c.h;
  /* the near view drawn off the axis (gaze.c, debug): centred at tangent (cx right, cy up), the
   * centre recorded with this frame */
  float centre[2];
  int moved = gaze_compose_centre(centre);
  /* (each frame's: a change in cvars.txt shows at once; the views' texels from their T#s) */
  u32 sn = g_opt_sharpen, sw = g_opt_sharpenwide;
  sn = sn > 100 ? 100 : sn, sw = sw > 100 ? 100 : sw;
  if (sn != g_c.sharp[0] || sw != g_c.sharp[1])
    LOG("compose: sharpen %u -> %u, wide %u -> %u", g_c.sharp[0], sn, g_c.sharp[1], sw);
  g_c.sharp[0] = sn, g_c.sharp[1] = sw;
  u32 fast = g_opt_composefast & 3;
  if (seq == 1 || fast != g_c.fast)
    LOG("compose: fast %u (1: black beyond the lens circle, nothing sampled there; 2: each view "
        "sampled only where the blend uses it; 0: the pass as it was)", fast);
  g_c.fast = fast;
  for (int e = 0; e < 2; e++) {
    struct consts *k = &s->k[e];
    const struct hmd_eye_uv *wu = &uv[2 * e], *nu = &uv[2 * e + 1];
    memset(k, 0, sizeof *k);
    memcpy(k->wide, tex[2 * e], 32);
    memcpy(k->near, tex[2 * e + 1], 32);
    memcpy(k->out, g_c.tsharp[set][e], 32);
    memcpy(k->samp, sampler, 16);
    /* a texel's centre in the wide view (the output spans it): u = (x + 0.5) / w; its direction's
     * tangent = (u - offset) / scale; the near view's u = its offset + tangent x its scale */
    k->wu0 = 0.5f / fw, k->wdu = 1.0f / fw, k->wv0 = 0.5f / fh, k->wdv = 1.0f / fh;
    k->tx0 = (k->wu0 - wu->offset_x) / wu->scale_x, k->tdx = k->wdu / wu->scale_x;
    k->ty0 = (k->wv0 - wu->offset_y) / wu->scale_y, k->tdy = k->wdv / wu->scale_y;
    /* the lens circle: the game's mask draws within 1.0 x the wide view's top tangent (its v 0 at
     * tangent -offset / scale), centred on the eye's axis (never moved), plus LENS_MARGIN texels
     * of the wide view's slice each way */
    const u32 *tw = tex[2 * e], *tn = tex[2 * e + 1];
    float twu = (float)((tw[2] & 0x3fff) + 1), twv = (float)(((tw[2] >> 14) & 0x3fff) + 1);
    float top = wu->offset_y / wu->scale_y;
    float mx = LENS_MARGIN / (wu->scale_x * twu), my = LENS_MARGIN / (wu->scale_y * twv);
    float lens = top + __builtin_sqrtf(mx * mx + my * my);
    k->lx0 = k->tx0, k->ly0 = k->ty0, k->lens2 = lens * lens;
    if ((fast & 1) && top > 0.25f && top < 4.0f) {
      k->mode |= COMPOSE_LENS;
      u32 key[5];
      memcpy(&key[0], &k->lx0, 4), memcpy(&key[1], &k->ly0, 4), memcpy(&key[2], &k->tdx, 4);
      memcpy(&key[3], &k->tdy, 4), memcpy(&key[4], &k->lens2, 4);
      if (memcmp(key, g_c.filled[set][e], sizeof key) != 0) {
        k->mode |= COMPOSE_FILL; /* (this output's first frame with this lens: its black stored) */
        memcpy(g_c.filled[set][e], key, sizeof key);
      }
      if (!g_c.lens_logged) {
        g_c.lens_logged = 1;
        /* (the share of the eye's texels, on a grid, with the near view centred) */
        u32 out = 0, nearu = 0, wideu = 0, all = 0;
        for (u32 gy = 0; gy < 128; gy++)
          for (u32 gx = 0; gx < 128; gx++, all++) {
            float x = ((float)gx + 0.5f) * fw / 128.0f, y = ((float)gy + 0.5f) * fh / 128.0f;
            float lx = k->lx0 + k->tdx * x, ly = k->ly0 + k->tdy * y, r2 = lx * lx + ly * ly;
            if (r2 > k->lens2) {
              out++;
              continue;
            }
            nearu += r2 < outer * outer, wideu += r2 > inner * inner;
          }
        LOG("compose: the lens circle at tangent %f (the game's %f and a margin of %f): %u%% of the eye "
            "beyond it (black, not sampled); within it the near view sampled on %u%% of the eye, the "
            "wide view on %u%% (the near view centred)", lens, top, lens - top, out * 100 / all,
            nearu * 100 / all, wideu * 100 / all);
      }
    } else if ((fast & 1) && !g_c.lens_logged) {
      g_c.lens_logged = 1;
      LOG("compose: the wide view's top tangent %f not as taken: the lens circle not used", top);
    }
    if (fast & 2)
      k->mode |= COMPOSE_NEED;
    if (moved) {
      /* off the axis: the near view's u = its offset + (tangent - c) x its scale (y down here),
       * and the blend centred on c (its tangents feed only the blend) */
      float cx = centre[0], cy = -centre[1];
      k->nu0 = nu->offset_x + (k->tx0 - cx) * nu->scale_x, k->ndu = k->tdx * nu->scale_x;
      k->nv0 = nu->offset_y + (k->ty0 - cy) * nu->scale_y, k->ndv = k->tdy * nu->scale_y;
      k->tx0 -= cx, k->ty0 -= cy;
    } else {
      k->nu0 = nu->offset_x + k->tx0 * nu->scale_x, k->ndu = k->tdx * nu->scale_x;
      k->nv0 = nu->offset_y + k->ty0 * nu->scale_y, k->ndv = k->tdy * nu->scale_y;
    }
    k->inner = inner, k->inv_band = 1.0f / (outer - inner);
    k->w = (s32)g_c.w, k->h = (s32)g_c.h;
    k->wsu = 1.0f / twu, k->wsv = 1.0f / twv;
    k->nsu = 1.0f / (float)((tn[2] & 0x3fff) + 1), k->nsv = 1.0f / (float)(((tn[2] >> 14) & 0x3fff) + 1);
    k->npeak = -(float)sn / 500.0f, k->wpeak = -(float)sw / 500.0f;
  }
  u32 ndw = build(s, seq);
  __asm__ volatile("mfence" ::: "memory");
  int rc = gen1_submit_acb(s->cb, ndw);
  if (rc != 0) {
    s->label = seq * 8 + 4, s->ts[0] = s->ts[1] = 0;
    memset(g_c.filled[set], 0, sizeof g_c.filled[set]); /* (its black not stored) */
    if (g_c.failed++ < 8)
      LOG("compose: frame %u: submit 0x%x", seq, rc);
    if (g_c.failed > 30) {
      LOG("compose: giving up");
      g_c.off = 1;
    }
    return 0;
  }
  g_c.n++;
  if (seq == 1) { /* the first: done on the GPU? (and the timestamps calibrate from here) */
    for (int i = 0; i < 1000 && s->label != seq * 8 + 4; i++)
      sceKernelUsleep(100);
    int done = s->label == seq * 8 + 4;
    LOG("compose: the first frame %s (label 0x%x, GPU clock %lu -> %lu)",
        done ? "done" : "NOT done in 100 ms: off", s->label, s->ts[0], s->ts[1]);
    g_c.cal_cpu = sceKernelGetProcessTime(), g_c.cal_gpu = s->ts[1];
    if (!done) {
      g_c.off = 1;
      return 0;
    }
    /* and its stores landed (stores through a T# the GPU takes as read-only are dropped without
     * a fault, inset.c): the output was all zero, and every texel written has alpha 0xff */
    const u8 *o = (const u8 *)((((u64)g_c.tsharp[set][0][0] | ((u64)(g_c.tsharp[set][0][1] & 0xff) << 32)) << 8));
    u32 written = 0;
    for (size_t j = 3; j < g_c.out_bytes && j < (4u << 20); j += 4096)
      written += o[j] != 0;
    LOG("compose: the first frame's stores: %u of the %u texels sampled written", written,
        (u32)((g_c.out_bytes < (4u << 20) ? g_c.out_bytes : (4u << 20)) / 4096));
    if (!written) {
      LOG("compose: nothing written to the output: off");
      g_c.off = 1;
      return 0;
    }
  }
  p->texture[0] = g_c.tsharp[set][0], p->texture[1] = g_c.tsharp[set][1];
  p->sampler = sampler;
  p->uv[0] = uv[0], p->uv[1] = uv[2];
  return 1;
}
