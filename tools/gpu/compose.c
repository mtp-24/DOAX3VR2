/* Dead or Alive Xtreme 3's eye, made whole (agent/compose.c). The game draws each eye twice at
 * one size: a wide view over the whole field of view with a black disc in its middle, and a near
 * view over the middle (about +-34 degrees), twice as dense; PS VR's reprojection
 * (sceHmdReprojectionStartWideNear) put the near view into the disc, blended over a band of
 * tangent at its edge. Here one texture an eye is written from the two, for PS VR2's reprojection
 * to take as any game's eye: the wide view's field of view at (about) the near view's density.
 *
 * One thread a texel of the output. Each texel's direction (tangents from the eye's axis, x right,
 * y down) is linear in its place; so are its coordinates in the wide and in the near view. Inside
 * `inner` (the tangent's length) the near view alone, outside inner + 1 / inv_band the wide view
 * alone, a linear blend between. Both views are sampled with the game's sampler (bilinear); the
 * output is opaque. Built for the PS4's GPU (gfx701) by tools/gcn_cc.py (GCN_ENTRY=compose_main)
 * into agent/compose_gcn.h.
 *
 * Sharpened (agent.sharpen, agent.sharpenwide: debug) when a view's peak is below 0: its sample
 * and the four one of its texels away (left, right, up, down; bilinear as the sample) make a
 * contrast-adaptive sharpen as AMD FidelityFX CAS's, on the cross: per channel, the room to the
 * neighbourhood's limits min(min, 1 - max) / max, its square root times the peak the neighbours'
 * weight w, the colour (c + w (l + r + u + d)) / (1 + 4 w). Little contrast sharpens fully, a
 * neighbourhood near black or white (a strong edge, the wide view's black disc) hardly at all, so
 * it doesn't ring. Each view is sharpened on its own before the blend, so the band is a blend of
 * the two as before (no seam), and only where the view counts (the near view inside the outer
 * tangent, the wide view outside the inner one). Off, the code is the plain compose's (a branch
 * on the constant).
 *
 * Cheaper (`mode`, agent/compose.c; logs/research/gpu120/COMPOSE-COST.md), each part a bit of the
 * constant so the old pass stays one cvar away (agent.composefast=0):
 * - COMPOSE_LENS: a texel beyond the lens circle (its tangent from the eye's axis, never the moved
 *   near centre's, longer than `lens`: the game's circle, 1.0 x the top tangent, plus the mask's
 *   tile and the resamplings' reach) is opaque black: the game's wide view is black there (its
 *   mask leaves those tiles undrawn). Nothing is sampled, and nothing is stored either unless
 *   COMPOSE_FILL is set: the CPU sets it for an output's first frame (and whenever the lens
 *   constants change), so the black is stored once and kept (only this pass writes the outputs).
 * - COMPOSE_NEED: each view sampled only where the blend uses it: the near view where a < 1, the
 *   wide view where a > 0 (a = 0 gives the near sample exactly, as before; a = 1 the wide sample,
 *   where the blend gave it to within a float's last bit).
 * SPDX-License-Identifier: GPL-3.0-or-later */
typedef int int4 __attribute__((ext_vector_type(4)));
typedef int int8 __attribute__((ext_vector_type(8)));
typedef float float4 __attribute__((ext_vector_type(4)));

/* The constants for one dispatch (one eye), written by agent/compose.c (struct consts there). */
struct compose_k {
  int8 wide;  /* T#: the eye's wide view (a slice of the game's array) */
  int8 near;  /* T#: its near view */
  int8 out;   /* T#: the output, written (UNORM, the read-only memory type cleared) */
  int4 samp;  /* S#: the game's sampler */
  float wu0, wdu, wv0, wdv; /* the wide view's u, v at texel (0, 0)'s centre, and a texel's step */
  float nu0, ndu, nv0, ndv; /* the near view's */
  float tx0, tdx, ty0, tdy; /* the direction's tangents likewise */
  float inner, inv_band;    /* the blend: (|tangent| - inner) * inv_band, 0 near to 1 wide */
  int w, h;                 /* the output's size */
  float wsu, wsv, nsu, nsv; /* a texel of the wide view, of the near view, in u and v */
  float wpeak, npeak;       /* the sharpen's neighbour weight at the most (-0.2 .. 0), 0 not sharpened */
  float lx0, ly0;           /* the eye's own tangents at texel (0, 0) (tdx, tdy the step): the lens test */
  float lens2;              /* the lens circle's radius (tangent) squared, with its margin */
  int mode;                 /* COMPOSE_*: 0 the pass as it was */
};
#define COMPOSE_LENS 1 /* beyond the lens circle black, nothing sampled */
#define COMPOSE_FILL 2 /* (with COMPOSE_LENS) that black stored, else the texel left as it is */
#define COMPOSE_NEED 4 /* each view sampled only where the blend uses it */
_Static_assert(sizeof(struct compose_k) == 224, "compose constants (agent/compose.c)");
_Static_assert(__builtin_offsetof(struct compose_k, samp) == 96, "compose constants");
_Static_assert(__builtin_offsetof(struct compose_k, wu0) == 112, "compose constants");
_Static_assert(__builtin_offsetof(struct compose_k, w) == 168, "compose constants");
_Static_assert(__builtin_offsetof(struct compose_k, wsu) == 176, "compose constants");
_Static_assert(__builtin_offsetof(struct compose_k, lx0) == 200, "compose constants");
_Static_assert(__builtin_offsetof(struct compose_k, mode) == 212, "compose constants");

#define K __attribute__((address_space(4))) const struct compose_k
/* (sampled as a 2D array at slice 0: the views after the first are one-slice views of the game's
 * array texture, where the GPU clamps the slice to the view's own; a plain 2D texture ignores it.
 * A 2D sample would leave the slice to a register nobody wrote.) */
float4 img_sample_lz(int dmask, float u, float v, float slice, int8 t, int4 s, _Bool unorm, int te, int cp)
    __asm("llvm.amdgcn.image.sample.lz.2darray.v4f32.f32");
void img_store_4(float4 v, int dmask, int x, int y, int8 t, int te, int cp)
    __asm("llvm.amdgcn.image.store.2d.v4f32.i32");
float sqrt_f32(float) __asm("llvm.sqrt.f32");

static inline float4 min4(float4 a, float4 b) { return __builtin_elementwise_min(a, b); }
static inline float4 max4(float4 a, float4 b) { return __builtin_elementwise_max(a, b); }

/* The sample c at (u, v) of view t sharpened with its neighbours a texel (su, sv) away; peak < 0. */
static inline float4 sharpen(float4 c, float u, float v, float su, float sv, float peak, int8 t, int4 s) {
  float4 l = img_sample_lz(7, u - su, v, 0.0f, t, s, 0, 0, 0);
  float4 r = img_sample_lz(7, u + su, v, 0.0f, t, s, 0, 0, 0);
  float4 up = img_sample_lz(7, u, v - sv, 0.0f, t, s, 0, 0, 0);
  float4 dn = img_sample_lz(7, u, v + sv, 0.0f, t, s, 0, 0, 0);
  float4 mn = min4(min4(min4(l, r), min4(up, dn)), c);
  float4 mx = max4(max4(max4(l, r), max4(up, dn)), c);
  /* (the views are UNORM: 0..1; mx kept off 0 for a black neighbourhood, whose room is 0) */
  float4 room = min4(mn, 1.0f - mx) / max4(mx, (float4)(1.0f / 1024.0f));
  room = min4(max4(room, (float4)0.0f), (float4)1.0f);
  float4 w = (float4){sqrt_f32(room.x), sqrt_f32(room.y), sqrt_f32(room.z), 0.0f} * peak;
  return (c + (l + r + up + dn) * w) / (1.0f + 4.0f * w);
}

void compose_main(K *k, int gx, int gy, int tx, int ty) {
  int x = gx * 8 + tx, y = gy * 8 + ty;
  if (x >= k->w || y >= k->h)
    return;
  float fx = (float)x, fy = (float)y;
  int mode = k->mode;
  if (mode & COMPOSE_LENS) {
    float lx = k->lx0 + k->tdx * fx, ly = k->ly0 + k->tdy * fy;
    if (lx * lx + ly * ly > k->lens2) {
      if (mode & COMPOSE_FILL)
        img_store_4((float4){0.0f, 0.0f, 0.0f, 1.0f}, 15, x, y, k->out, 0, 0);
      return;
    }
  }
  float ax = k->tx0 + k->tdx * fx, ay = k->ty0 + k->tdy * fy;
  float a = (sqrt_f32(ax * ax + ay * ay) - k->inner) * k->inv_band;
  a = a < 0.0f ? 0.0f : a > 1.0f ? 1.0f : a;
  float wu = k->wu0 + k->wdu * fx, wv = k->wv0 + k->wdv * fy;
  float nu = k->nu0 + k->ndu * fx, nv = k->nv0 + k->ndv * fy;
  float4 c;
  if (mode & COMPOSE_NEED) {
    float4 cw = (float4)0.0f, cn = (float4)0.0f;
    if (a < 1.0f) {
      cn = img_sample_lz(7, nu, nv, 0.0f, k->near, k->samp, 0, 0, 0);
      if (k->npeak < 0.0f)
        cn = sharpen(cn, nu, nv, k->nsu, k->nsv, k->npeak, k->near, k->samp);
    }
    if (a > 0.0f) {
      cw = img_sample_lz(7, wu, wv, 0.0f, k->wide, k->samp, 0, 0, 0);
      if (k->wpeak < 0.0f)
        cw = sharpen(cw, wu, wv, k->wsu, k->wsv, k->wpeak, k->wide, k->samp);
    }
    c = a >= 1.0f ? cw : a > 0.0f ? cn + (cw - cn) * a : cn;
  } else {
    float4 cw = img_sample_lz(7, wu, wv, 0.0f, k->wide, k->samp, 0, 0, 0);
    float4 cn = img_sample_lz(7, nu, nv, 0.0f, k->near, k->samp, 0, 0, 0);
    if (k->npeak < 0.0f && a < 1.0f)
      cn = sharpen(cn, nu, nv, k->nsu, k->nsv, k->npeak, k->near, k->samp);
    if (k->wpeak < 0.0f && a > 0.0f)
      cw = sharpen(cw, wu, wv, k->wsu, k->wsv, k->wpeak, k->wide, k->samp);
    c = cn + (cw - cn) * a;
  }
  c.w = 1.0f;
  img_store_4(c, 15, x, y, k->out, 0, 0);
}
