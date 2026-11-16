/* A CPU reference of the compose pass (tools/gpu/compose.c): the shader's own source compiled for
 * the host, twice (the pass before the savings, from git, and as it is now), each run over the
 * whole of a composed eye from synthetic views made as the game makes them, and their outputs
 * compared byte for byte. Built and run by tools/gpu/compose_ref.sh; the results in
 * logs/research/gpu120/COMPOSE-COST.md.
 *
 * The views (as the game draws them for PS VR2's field of view, docs/HOW-IT-WORKS.md):
 * - the wide view drawn at RW x RH (agent.widew) over tangents out 1.8418, in 0.9472, up and down
 *   1.329, its tiles masked as the game's mask does (agent/gaze.c: a tile of 8 x 8 eye pixels, R8
 *   at the eye size / 8, drawn when any of its corners, a pixel outside the tile, is within the lens
 *   circle 1.0 x f2, and not all of them inside the hole, 0.98 x ((n - 0.05) / f2)^2, the hole
 *   centred on the near centre as agent.movehole does); undrawn texels black; then upsampled
 *   (bilinear, as the game's FXAA pass into its slice) to the eye size EW x EH;
 * - the near view drawn at EW x EH over +-n about the near centre, its corners masked to the disc
 *   (tiles likewise);
 * - a scene of fine noise, stripes and edges in tangent space, so the bilinear samples, the blend
 *   and the sharpen all see detail.
 * The constants as agent/compose.c's compose_frame writes them (the same arithmetic, copied).
 * Sampling as the GPU's bilinear (texel centres, clamped to the edge), stores as UNORM8 (rounded).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int int4 __attribute__((ext_vector_type(4)));
typedef int int8 __attribute__((ext_vector_type(8), aligned(32)));
typedef float float4 __attribute__((ext_vector_type(4)));

/* the shader's constants (tools/gpu/compose.c, struct compose_k; the old pass reads its prefix) */
struct consts {
  int8 wide, near, out;
  int4 samp;
  float wu0, wdu, wv0, wdv, nu0, ndu, nv0, ndv, tx0, tdx, ty0, tdy;
  float inner, inv_band;
  int w, h;
  float wsu, wsv, nsu, nsv, wpeak, npeak;
  float lx0, ly0, lens2;
  int mode;
  int pad[2];
};
_Static_assert(sizeof(struct consts) == 224, "compose constants");
_Static_assert(__builtin_offsetof(struct consts, lx0) == 200, "compose constants");

/* the images: 0 wide, 1 near (RGBA8), 2 the output */
struct img {
  int w, h;
  uint8_t *p;
};
static struct img g_img[3];
static uint8_t *g_written; /* the output's texels stored this run */
static uint64_t g_fetch[2]; /* bilinear samples of the wide, the near view */

float4 img_sample_lz(int dmask, float u, float v, float slice, int8 t, int4 s, _Bool unorm, int te, int cp) {
  (void)dmask, (void)slice, (void)s, (void)unorm, (void)te, (void)cp;
  const struct img *im = &g_img[t.s0];
  g_fetch[t.s0]++;
  float x = u * (float)im->w - 0.5f, y = v * (float)im->h - 0.5f;
  float fx = floorf(x), fy = floorf(y);
  float ax = x - fx, ay = y - fy;
  int x0 = (int)fx, y0 = (int)fy;
  float4 c = 0;
  for (int j = 0; j < 2; j++)
    for (int i = 0; i < 2; i++) {
      int xi = x0 + i, yi = y0 + j;
      xi = xi < 0 ? 0 : xi >= im->w ? im->w - 1 : xi;
      yi = yi < 0 ? 0 : yi >= im->h ? im->h - 1 : yi;
      const uint8_t *q = im->p + ((size_t)yi * im->w + xi) * 4;
      float wt = (i ? ax : 1.0f - ax) * (j ? ay : 1.0f - ay);
      c += (float4){q[0], q[1], q[2], 0.0f} * (wt / 255.0f);
    }
  return c;
}

void img_store_4(float4 v, int dmask, int x, int y, int8 t, int te, int cp) {
  (void)dmask, (void)t, (void)te, (void)cp;
  struct img *im = &g_img[2];
  uint8_t *q = im->p + ((size_t)y * im->w + x) * 4;
  for (int i = 0; i < 4; i++) {
    float f = v[i] < 0.0f ? 0.0f : v[i] > 1.0f ? 1.0f : v[i];
    q[i] = (uint8_t)lrintf(f * 255.0f);
  }
  g_written[(size_t)y * im->w + x] = 1;
}

float sqrt_f32(float f) { return sqrtf(f); }

void compose_old(const struct consts *k, int gx, int gy, int tx, int ty);
void compose_new(const struct consts *k, int gx, int gy, int tx, int ty);

/* ---- the scene and the views ---- */
static const float F0 = 1.8418f, F1 = 0.9472f, F2 = 1.3290f, F3 = 1.3290f;

static uint32_t hash(uint32_t a) {
  a ^= a >> 16, a *= 0x7feb352d, a ^= a >> 15, a *= 0x846ca68b, a ^= a >> 16;
  return a;
}
/* the scene's colour at tangent (x right, y down) */
static void scene(float x, float y, uint8_t o[4]) {
  int qx = (int)floorf(x * 900.0f), qy = (int)floorf(y * 900.0f);
  uint32_t h = hash((uint32_t)qx * 73856093u ^ (uint32_t)qy * 19349663u);
  float stripes = 0.5f + 0.5f * sinf(x * 160.0f + y * 40.0f);
  float edge = (x * x + (y - 0.2f) * (y - 0.2f) < 0.09f) ? 1.0f : 0.25f;
  float r = 0.15f + 0.6f * stripes * edge + 0.25f * (float)(h & 0xff) / 255.0f;
  float g = 0.1f + 0.5f * edge + 0.3f * (float)((h >> 8) & 0xff) / 255.0f;
  float b = 0.05f + 0.9f * (float)((h >> 16) & 0xff) / 255.0f * (0.3f + 0.7f * stripes);
  o[0] = (uint8_t)lrintf(fminf(r, 1.0f) * 255.0f), o[1] = (uint8_t)lrintf(fminf(g, 1.0f) * 255.0f);
  o[2] = (uint8_t)lrintf(fminf(b, 1.0f) * 255.0f), o[3] = 255;
}

struct uv { float sx, sy, ox, oy; };

struct cfg {
  const char *name;
  int ew, eh;   /* the eye (slice) size */
  int rw, rh;   /* the wide view's drawn size */
  float nearn;  /* n = nearn/100 x f2 */
  float cx, cy; /* the near centre (tangent, x right, y up) */
  int eye;      /* 0 left, 1 right */
  unsigned sharpen, sharpenwide;
};

/* the game's wide mask, one byte a tile of the eye size / 8 (agent/gaze.c's rule) */
static uint8_t *wide_mask(const struct cfg *c, int mw, int mh, float inner, int *drawn) {
  uint8_t *m = calloc((size_t)mw * mh, 1);
  float cu = c->eye == 0 ? F0 / (F0 + F1) : F1 / (F0 + F1), kx = (F0 + F1) / F2;
  float cv = F2 / (F2 + F3), ky = (F2 + F3) / F2;
  float hx = c->cx / F2, hy = -c->cy / F2, hr2 = (inner / F2) * (inner / F2) * 0.98f;
  float W8 = (float)(mw * 8), H8 = (float)(mh * 8);
  *drawn = 0;
  for (int y = 0; y < mh; y++)
    for (int x = 0; x < mw; x++) {
      float us[2] = {((float)(x * 8 - 1) + 0.5f) / W8, ((float)(x * 8 + 8) + 0.5f) / W8};
      float vs[2] = {((float)(y * 8 - 1) + 0.5f) / H8, ((float)(y * 8 + 8) + 0.5f) / H8};
      int lens = 0, hole = 1;
      for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++) {
          float X = (us[i] - cu) * kx, Y = (vs[j] - cv) * ky;
          lens |= X * X + Y * Y <= 1.0f;
          hole &= (X - hx) * (X - hx) + (Y - hy) * (Y - hy) < hr2;
        }
      m[y * mw + x] = lens && !hole;
      *drawn += m[y * mw + x];
    }
  return m;
}

static uint8_t *make_wide(const struct cfg *c, const struct uv *wu, float inner) {
  int mw = c->ew / 8, mh = c->eh / 8, drawn;
  uint8_t *m = wide_mask(c, mw, mh, inner, &drawn);
  uint8_t *r = malloc((size_t)c->rw * c->rh * 4);
  for (int j = 0; j < c->rh; j++)
    for (int i = 0; i < c->rw; i++) {
      float u = ((float)i + 0.5f) / (float)c->rw, v = ((float)j + 0.5f) / (float)c->rh;
      int tx = (int)(u * (float)mw), ty = (int)(v * (float)mh);
      uint8_t *o = r + ((size_t)j * c->rw + i) * 4;
      if (!m[ty * mw + tx]) {
        o[0] = o[1] = o[2] = 0, o[3] = 255;
        continue;
      }
      scene((u - wu->ox) / wu->sx, (v - wu->oy) / wu->sy, o);
    }
  /* the FXAA pass's upsampling into the slice (bilinear) */
  uint8_t *s = malloc((size_t)c->ew * c->eh * 4);
  struct img tmp = {c->rw, c->rh, r};
  g_img[0] = tmp;
  for (int j = 0; j < c->eh; j++)
    for (int i = 0; i < c->ew; i++) {
      float4 v = img_sample_lz(7, ((float)i + 0.5f) / (float)c->ew, ((float)j + 0.5f) / (float)c->eh, 0, (int8){0}, (int4){0}, 0, 0, 0);
      uint8_t *o = s + ((size_t)j * c->ew + i) * 4;
      for (int k = 0; k < 3; k++)
        o[k] = (uint8_t)lrintf(fminf(fmaxf(v[k], 0.0f), 1.0f) * 255.0f);
      o[3] = 255;
    }
  free(r), free(m);
  return s;
}

static uint8_t *make_near(const struct cfg *c, const struct uv *nu) {
  uint8_t *s = malloc((size_t)c->ew * c->eh * 4);
  for (int j = 0; j < c->eh; j++)
    for (int i = 0; i < c->ew; i++) {
      float u = ((float)i + 0.5f) / (float)c->ew, v = ((float)j + 0.5f) / (float)c->eh;
      /* (its disc, by tiles of 8: a tile drawn when any corner is within radius 0.5 in uv) */
      float tu0 = (float)((i / 8) * 8 - 1) / (float)c->ew - 0.5f, tu1 = (float)((i / 8) * 8 + 8) / (float)c->ew - 0.5f;
      float tv0 = (float)((j / 8) * 8 - 1) / (float)c->eh - 0.5f, tv1 = (float)((j / 8) * 8 + 8) / (float)c->eh - 0.5f;
      float mu = fminf(fabsf(tu0), fabsf(tu1)), mv = fminf(fabsf(tv0), fabsf(tv1));
      if (tu0 < 0 && tu1 > 0) mu = 0;
      if (tv0 < 0 && tv1 > 0) mv = 0;
      uint8_t *o = s + ((size_t)j * c->ew + i) * 4;
      if (mu * mu + mv * mv > 0.25f) {
        o[0] = o[1] = o[2] = 0, o[3] = 255;
        continue;
      }
      scene((u - nu->ox) / nu->sx + c->cx, (v - nu->oy) / nu->sy - c->cy, o);
    }
  return s;
}

/* agent/compose.c's constants for one eye (the same arithmetic) */
#define LENS_MARGIN 24.0f
static void consts(struct consts *k, const struct cfg *c, const struct uv *wu, const struct uv *nu,
                   int w, int h, float inner, float outer, unsigned fast, int fill) {
  memset(k, 0, sizeof *k);
  k->wide.s0 = 0, k->near.s0 = 1, k->out.s0 = 2;
  float fw = (float)w, fh = (float)h;
  k->wu0 = 0.5f / fw, k->wdu = 1.0f / fw, k->wv0 = 0.5f / fh, k->wdv = 1.0f / fh;
  k->tx0 = (k->wu0 - wu->ox) / wu->sx, k->tdx = k->wdu / wu->sx;
  k->ty0 = (k->wv0 - wu->oy) / wu->sy, k->tdy = k->wdv / wu->sy;
  float twu = (float)c->ew, twv = (float)c->eh;
  float top = wu->oy / wu->sy;
  float mx = LENS_MARGIN / (wu->sx * twu), my = LENS_MARGIN / (wu->sy * twv);
  float lens = top + sqrtf(mx * mx + my * my);
  k->lx0 = k->tx0, k->ly0 = k->ty0, k->lens2 = lens * lens;
  if (fast & 1)
    k->mode |= 1 | (fill ? 2 : 0);
  if (fast & 2)
    k->mode |= 4;
  if (c->cx != 0.0f || c->cy != 0.0f) {
    float cx = c->cx, cy = -c->cy;
    k->nu0 = nu->ox + (k->tx0 - cx) * nu->sx, k->ndu = k->tdx * nu->sx;
    k->nv0 = nu->oy + (k->ty0 - cy) * nu->sy, k->ndv = k->tdy * nu->sy;
    k->tx0 -= cx, k->ty0 -= cy;
  } else {
    k->nu0 = nu->ox + k->tx0 * nu->sx, k->ndu = k->tdx * nu->sx;
    k->nv0 = nu->oy + k->ty0 * nu->sy, k->ndv = k->tdy * nu->sy;
  }
  k->inner = inner, k->inv_band = 1.0f / (outer - inner);
  k->w = w, k->h = h;
  k->wsu = 1.0f / twu, k->wsv = 1.0f / twv, k->nsu = 1.0f / twu, k->nsv = 1.0f / twv;
  k->npeak = -(float)c->sharpen / 500.0f, k->wpeak = -(float)c->sharpenwide / 500.0f;
}

static void run(void (*fn)(const struct consts *, int, int, int, int), const struct consts *k) {
  memset(g_written, 0, (size_t)k->w * k->h);
  g_fetch[0] = g_fetch[1] = 0;
  for (int gy = 0; gy < (k->h + 7) / 8; gy++)
    for (int gx = 0; gx < (k->w + 7) / 8; gx++)
      for (int ty = 0; ty < 8; ty++)
        for (int tx = 0; tx < 8; tx++)
          fn(k, gx, gy, tx, ty);
}

static int one(const struct cfg *c) {
  float n = c->nearn / 100.0f * F2, inner = n - 0.05f, outer = n;
  struct uv wu = {1.0f / (F0 + F1), 1.0f / (F2 + F3), (c->eye == 0 ? F0 : F1) / (F0 + F1), F2 / (F2 + F3)};
  struct uv nu = {1.0f / (2.0f * n), 1.0f / (2.0f * n), 0.5f, 0.5f};
  /* the output's size as ensure() makes it (composemax 4096) */
  float fw = (float)c->ew * nu.sx / wu.sx, fh = (float)c->eh * nu.sy / wu.sy;
  if (fw > 4096.0f) fh *= 4096.0f / fw, fw = 4096.0f;
  if (fh > 4096.0f) fw *= 4096.0f / fh, fh = 4096.0f;
  int w = ((int)fw + 7) & ~7, h = ((int)fh + 7) & ~7;
  uint8_t *wide = make_wide(c, &wu, inner), *near = make_near(c, &nu);
  g_img[0] = (struct img){c->ew, c->eh, wide};
  g_img[1] = (struct img){c->ew, c->eh, near};
  size_t px = (size_t)w * h;
  uint8_t *o_old = calloc(px, 4), *o_new = calloc(px, 4), *o_new2 = calloc(px, 4);
  g_written = malloc(px);
  struct consts k;
  /* the pass before the savings */
  consts(&k, c, &wu, &nu, w, h, inner, outer, 0, 0);
  g_img[2] = (struct img){w, h, o_old};
  run(compose_old, &k);
  uint64_t f_old[2] = {g_fetch[0], g_fetch[1]};
  /* now, an output's first frame (its black stored) */
  consts(&k, c, &wu, &nu, w, h, inner, outer, 3, 1);
  g_img[2] = (struct img){w, h, o_new};
  run(compose_new, &k);
  /* and a later frame into an output that already holds its black (from a 0xcd fill, to see that
   * nothing beyond the lens is stored; the black there then from the first frame's) */
  memset(o_new2, 0xcd, px * 4);
  consts(&k, c, &wu, &nu, w, h, inner, outer, 3, 0);
  g_img[2] = (struct img){w, h, o_new2};
  run(compose_new, &k);
  uint64_t f_new[2] = {g_fetch[0], g_fetch[1]};
  size_t stored2 = 0;
  for (size_t i = 0; i < px; i++)
    stored2 += g_written[i];
  /* also: the new source with agent.composefast=0 (the old path kept in it) */
  uint8_t *o_off = calloc(px, 4);
  consts(&k, c, &wu, &nu, w, h, inner, outer, 0, 0);
  g_img[2] = (struct img){w, h, o_off};
  run(compose_new, &k);
  int off_same = memcmp(o_off, o_old, px * 4) == 0;

  /* compare: inside the lens circle (as the shader decides) every byte; beyond it the old pass's
   * output must be black, the new first frame's opaque black, the later frame's untouched */
  consts(&k, c, &wu, &nu, w, h, inner, outer, 3, 0);
  size_t in = 0, diff = 0, diff_wide = 0, maxd = 0, out = 0, old_nonblack = 0, new_bad = 0, later_bad = 0;
  float rmax = 0.0f;
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      size_t i = (size_t)y * w + x;
      const uint8_t *a = o_old + i * 4, *b = o_new + i * 4, *b2 = o_new2 + i * 4;
      float lx = k.lx0 + k.tdx * (float)x, ly = k.ly0 + k.tdy * (float)y;
      float r = sqrtf(lx * lx + ly * ly);
      if (a[0] | a[1] | a[2])
        rmax = r > rmax ? r : rmax;
      if (lx * lx + ly * ly > k.lens2) {
        out++;
        old_nonblack += (a[0] | a[1] | a[2]) != 0;
        new_bad += !(b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 255);
        later_bad += !(b2[0] == 0xcd && b2[1] == 0xcd && b2[2] == 0xcd && b2[3] == 0xcd);
        continue;
      }
      in++;
      int d = 0;
      for (int j = 0; j < 4; j++) {
        int e = abs((int)a[j] - (int)b[j]);
        d = e > d ? e : d;
        e = abs((int)a[j] - (int)b2[j]);
        d = e > d ? e : d;
      }
      diff += d != 0;
      if (d) { /* (where the blend gave the wide view alone: a = 1) */
        float ax = k.tx0 + k.tdx * (float)x, ay = k.ty0 + k.tdy * (float)y;
        float al = (sqrtf(ax * ax + ay * ay) - k.inner) * k.inv_band;
        diff_wide += al >= 1.0f;
      }
      maxd = (size_t)d > maxd ? (size_t)d : maxd;
    }
  printf("%-34s eye %dx%d out %dx%d: inside the lens %zu texels, %zu differ (max %zu; %zu where a = 1); beyond %zu (%.1f%%): "
         "old non-black %zu, new not opaque black %zu, later frame stored %zu; old's last non-black at "
         "tangent %.4f (lens %.4f = %.4f x f2); fetches wide %.2f -> %.2f, near %.2f -> %.2f a texel "
         "(%.1f%% of the old); later frame stores %.1f%%; composefast=0 same as old: %s\n",
         c->name, c->ew, c->eh, w, h, in, diff, maxd, diff_wide, out, 100.0 * out / px, old_nonblack, new_bad,
         later_bad, rmax, sqrtf(k.lens2), sqrtf(k.lens2) / F2, (double)f_old[0] / px, (double)f_new[0] / px,
         (double)f_old[1] / px, (double)f_new[1] / px,
         100.0 * (f_new[0] + f_new[1]) / (double)(f_old[0] + f_old[1]), 100.0 * stored2 / px,
         off_same ? "yes" : "NO");
  /* (a = 1: the wide sample itself now, where the old blend's cn + (cw - cn) could differ from it in
   * a float's last bit, and so, rarely, by one in the stored byte) */
  int ok = diff == diff_wide && maxd <= 1 && old_nonblack == 0 && new_bad == 0 && later_bad == 0 && off_same;
  free(wide), free(near), free(o_old), free(o_new), free(o_new2), free(o_off), free(g_written);
  return ok;
}

int main(void) {
  static const struct cfg cases[] = {
      /* the user's replay: renderw 1280, widew 940, nearn 40 */
      {"1280/w940/n40 centred L", 1280, 1432, 940, 1052, 40, 0, 0, 0, 0, 0},
      {"1280/w940/n40 centred R", 1280, 1432, 940, 1052, 40, 0, 0, 1, 0, 0},
      {"1280/w940/n40 c(+0.30,-0.20) L", 1280, 1432, 940, 1052, 40, 0.30f, -0.20f, 0, 0, 0},
      {"1280/w940/n40 c(-0.55,+0.50) R", 1280, 1432, 940, 1052, 40, -0.55f, 0.50f, 1, 0, 0},
      {"1280/w940/n40 c(0.2,0.1) sh50/50", 1280, 1432, 940, 1052, 40, 0.2f, 0.1f, 0, 50, 50},
      {"1280/w1280/n50 centred sh50", 1280, 1432, 1280, 1432, 50, 0, 0, 0, 50, 0},
      /* the sweep's: eye 1600, wide 700 (the margin's worst case) */
      {"1600/w700/n40 c(0.79,0) L", 1600, 1790, 700, 783, 40, 0.79f, 0.0f, 0, 0, 0},
      {"1600/w700/n40 c(-0.4,-0.6) R sh100", 1600, 1790, 700, 783, 40, -0.4f, -0.6f, 1, 100, 100},
      {"940/w940/n50 stock", 940, 1052, 940, 1052, 50, 0, 0, 0, 0, 0},
  };
  int ok = 1;
  for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++)
    ok &= one(&cases[i]);
  printf("%s\n", ok ? "PASS" : "FAIL");
  return !ok;
}
