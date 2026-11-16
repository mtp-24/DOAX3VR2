/* The menu in the headset: its picture (menu.c) copied into the frame's eye textures before the
 * frame goes to Hmd2, by a small GCN compute shader (tools/gpu/inset.c, its stage 2; the shader's
 * other stage, an alpha pass over the texture, is not used here).
 *
 * One compute buffer on Hmd2's queue an eye a frame (acb.c), held on the queue until its stores
 * are in memory, so Hmd2's work submitted after it reads them. Its copy of the frame's T# has the
 * read-only memory type cleared (stores through it are dropped).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "vr2.h"

#include "inset_gcn.h"

#define NSLOT 2
#define SHADER_SPACE 0x4000
#define INSET_RAMP 40.0f
_Static_assert(sizeof k_inset_gcn <= SHADER_SPACE, "the inset shader outgrew its space");

/* struct inset_k in tools/gpu/inset.c, the same layout (the pads: fields since removed, the offsets
 * kept; the menu's pass sets tex, w, h, stage and the stage 2 fields, the rest 0) */
struct consts {
  u32 tex[8];
  s32 rect[2][4];
  s32 w, h;
  float ramp;
  s32 debug, axis[2][2];
  float ppt[4]; /* (the alpha pass's: pixels a unit of tangent) */
  s32 pad0, stage, pad1, oy; /* stage 2: the menu; oy: the pass's first row */
  s32 pad6[10];
  float gaze[2]; /* (the alpha pass's gaze ring) */
  s32 gazedot, pad2[3]; /* (gazedot always 0: its option is gone; kept for the layout) */
  s32 pad7[8];
  float menu[4]; /* (the alpha pass's: the menu's place in tangent) */
  s32 menuon;
  float menu_dx; /* the menu's shift in each eye (its stereo: the left eye's +, the right's -) */
  s32 pad3[2];
  u32 mtex[8];   /* stage 2: the menu's picture */
  s32 mat[2][2]; /* stage 2: its top-left in each eye */
  s32 mw, mh, pad4[2];
  float ictan[4]; /* (the alpha pass's) */
  s32 pad5[4];
};
_Static_assert(sizeof(struct consts) == 352, "inset constants");
_Static_assert(__builtin_offsetof(struct consts, ictan) == 320, "inset constants");
_Static_assert(__builtin_offsetof(struct consts, mtex) == 256, "inset constants");
_Static_assert(__builtin_offsetof(struct consts, menu) == 224, "inset constants");
_Static_assert(__builtin_offsetof(struct consts, gaze) == 168, "inset constants");
_Static_assert(__builtin_offsetof(struct consts, stage) == 116, "inset constants");
_Static_assert(__builtin_offsetof(struct consts, w) == 64, "inset constants");

struct __attribute__((aligned(256))) slot {
  struct consts k;
  u32 cb[128];
  volatile u32 label; /* seq * 8 + 4 when done */
  u32 pad;
  volatile u64 ts[2]; /* the GPU's clock at the start and the end */
};

static struct {
  u8 *mem; /* the shader */
  u32 off;
} g_i;

static u32 build(struct slot *s, u32 seq, u32 w, u32 h) {
  u32 *cb = s->cb, n = 0;
  u64 shader = (u64)g_i.mem, k = (u64)&s->k;
  n = acb_acquire(cb, n, 1); /* the constants and the commands were written by the CPU */
  cb[n++] = acb_pm4(0x76, 4), cb[n++] = 0x204, cb[n++] = 0, cb[n++] = 0, cb[n++] = 0; /* START */
  cb[n++] = acb_pm4(0x76, 4), cb[n++] = 0x207, cb[n++] = 8, cb[n++] = 8, cb[n++] = 1; /* NUM_THREAD */
  cb[n++] = acb_pm4(0x76, 3), cb[n++] = 0x20c; /* PGM_LO/HI */
  cb[n++] = (u32)(shader >> 8), cb[n++] = (u32)(shader >> 40);
  u32 rsrc1 = ((K_INSET_GCN_VGPRS - 1) / 4) | (((K_INSET_GCN_SGPRS - 1) / 8) << 6) |
              (0xc0u << 12) | (1u << 21);
  u32 rsrc2 = (2u << 1) | (1u << 7) | (1u << 8) | (1u << 11);
  cb[n++] = acb_pm4(0x76, 3), cb[n++] = 0x212, cb[n++] = rsrc1, cb[n++] = rsrc2;
  /* (the timestamps without cache actions: the barrier after the dispatch writes our stores back
   * for Hmd2's work, and the timestamps with them) */
  n = acb_timestamp(cb, n, (u64)&s->ts[0]);
  cb[n++] = acb_pm4(0x76, 3), cb[n++] = 0x240, cb[n++] = (u32)k, cb[n++] = (u32)(k >> 32);
  cb[n++] = acb_pm4(0x15, 4);
  cb[n++] = (w + 7) / 8, cb[n++] = (h + 7) / 8, cb[n++] = 1, cb[n++] = 0x41;
  n = acb_timestamp(cb, n, (u64)&s->ts[1]);
  return acb_barrier(cb, n, (u64)&s->label, seq * 8 + 4);
}

#ifndef RELEASE
/* Once, before the first pass (debug builds only: 1 MB of the game's direct memory, a wait of
 * up to 50 ms): the shader's stores (its alpha stage's magenta fill) tried on textures of our
 * own, read back by the processor: a linear 64x64 texture, and ones tiled as the game's (its T#
 * with our base, 256x256, the pass over 64x64 of it). Logged: whether the stores landed. */
static void self_test(const u32 *gt) {
  enum { NT = 4, SZ = 0x40000 };
  u8 *m = vr2_dalloc(NT * SZ + 0x10000, 0x10000);
  if (!m) {
    LOG("inset: self-test: no memory");
    return;
  }
  struct consts *k = (struct consts *)(m + NT * SZ);
  u32 *cb = (u32 *)(m + NT * SZ + 0x1000);
  volatile u32 *label = (volatile u32 *)(m + NT * SZ + 0x2000);
  static const char *const what[NT] = {"linear", "the game's T#, word 3 bit 26 clear",
                                       "the game's, bit 26 clear, words 6-7 0",
                                       "the game's, linear tiling, bit 26 clear"};
  memset(m, 0x11, NT * SZ);
  *label = 0;
  memset(k, 0, NT * sizeof *k);
  for (int i = 0; i < NT; i++) {
    u64 a = (u64)(m + i * SZ);
    u32 *t = k[i].tex;
    if (i == 0) {
      t[1] = 10u << 20; /* 8_8_8_8 UNORM */
      t[2] = 63u | (63u << 14);
      t[3] = 0xfacu | (8u << 20) | (9u << 28); /* x y z w; linear aligned; 2D */
      t[4] = (64u - 1) << 13;
    } else {
      memcpy(t, gt, 32);
      t[1] &= ~(0xfu << 26);
      t[2] = (t[2] & ~0x0fffffffu) | 255u | (255u << 14);
      t[4] = (t[4] & ~(0x3fffu << 13)) | ((256u - 1) << 13);
      t[3] &= ~(1u << 26); /* the memory type's top bit (read-only, as the game's has it) */
      if (i == 2)
        t[6] = t[7] = 0;
      if (i == 3)
        t[3] = (t[3] & ~(0x1fu << 20)) | (8u << 20);
    }
    t[0] = (u32)(a >> 8);
    t[1] = (t[1] & ~0xffu) | (u32)((a >> 40) & 0xff);
    k[i].w = k[i].h = 64, k[i].ramp = INSET_RAMP, k[i].debug = 1; /* (the alpha stage: all magenta) */
  }
  u32 n = 0;
  u64 shader = (u64)g_i.mem;
  n = acb_acquire(cb, n, 1);
  cb[n++] = acb_pm4(0x76, 4), cb[n++] = 0x204, cb[n++] = 0, cb[n++] = 0, cb[n++] = 0;
  cb[n++] = acb_pm4(0x76, 4), cb[n++] = 0x207, cb[n++] = 8, cb[n++] = 8, cb[n++] = 1;
  cb[n++] = acb_pm4(0x76, 3), cb[n++] = 0x20c;
  cb[n++] = (u32)(shader >> 8), cb[n++] = (u32)(shader >> 40);
  u32 rsrc1 = ((K_INSET_GCN_VGPRS - 1) / 4) | (((K_INSET_GCN_SGPRS - 1) / 8) << 6) |
              (0xc0u << 12) | (1u << 21);
  u32 rsrc2 = (2u << 1) | (1u << 7) | (1u << 8) | (1u << 11);
  cb[n++] = acb_pm4(0x76, 3), cb[n++] = 0x212, cb[n++] = rsrc1, cb[n++] = rsrc2;
  for (int i = 0; i < NT; i++) {
    u64 ka = (u64)&k[i];
    cb[n++] = acb_pm4(0x76, 3), cb[n++] = 0x240, cb[n++] = (u32)ka, cb[n++] = (u32)(ka >> 32);
    cb[n++] = acb_pm4(0x15, 4), cb[n++] = 8, cb[n++] = 8, cb[n++] = 1, cb[n++] = 0x41;
    n = acb_barrier(cb, n, (u64)label, (u32)i + 1);
  }
  __asm__ volatile("mfence" ::: "memory");
  int rc = gen1_submit_acb(cb, n);
  for (int i = 0; rc == 0 && i < 500 && *label != NT; i++)
    sceKernelUsleep(100);
  LOG("inset: self-test: submit 0x%x, label %u (the game's T#: %08x %08x %08x %08x %08x %08x "
      "%08x %08x)", rc, *label, gt[0], gt[1], gt[2], gt[3], gt[4], gt[5], gt[6], gt[7]);
  for (int i = 0; i < NT; i++) {
    const u8 *p = m + i * SZ;
    u32 changed = 0, magenta = 0;
    for (u32 j = 0; j < SZ; j++)
      changed += p[j] != 0x11;
    for (u32 j = 0; j + 4 <= SZ; j += 4)
      magenta += p[j] == 0xff && p[j + 1] == 0 && p[j + 2] == 0xff && p[j + 3] == 0;
    LOG("inset: self-test %d (%s): %u bytes written, %u magenta texels (4096 expected)", i,
        what[i], changed, magenta);
  }
}
#endif

/* the shader, put in GPU-readable memory once Hmd2's queue is known; 1 when ready */
static int ensure_mem(const struct vr_frame *f) {
  if (g_i.off)
    return 0;
  if (g_i.mem)
    return 1;
  if (!gen1_queue_ready()) /* Hmd2's queue known once it has submitted (gen1.c) */
    return 0;
  g_i.mem = vr2_dalloc(SHADER_SPACE + NSLOT * sizeof(struct slot), 0x10000);
  if (!g_i.mem) {
    LOG("inset: no memory for the menu's shader: off");
    g_i.off = 1;
    return 0;
  }
  memcpy(g_i.mem, k_inset_gcn, sizeof k_inset_gcn);
  LOG("inset: the menu's pass ready (%u bytes of shader)", (u32)sizeof k_inset_gcn);
#ifndef RELEASE
  self_test(f->tsharp[0]);
#else
  (void)f;
#endif
  return 1;
}

/* The menu (menu.c) over both eyes, every frame it is open (vr2.c): the shader's stage 2 on
 * Hmd2's queue. at: its top-left in each eye (texels). Two slots an eye, taken in turn: one still
 * busy, the frame goes without the menu. */
static struct slot *g_menu_slot;
static u32 g_menu_seq;
void inset_menu_frame(const struct vr_frame *f, const s32 at[2][2]) {
  const u8 *pic = menu_picture();
  if (!pic)
    return;
  if (!ensure_mem(f))
    return;
  /* A texture an eye (the composed pair): a pass an eye with its own copy (the shader's first; its
   * second off the texture). Both eyes in one texture (a frame not composed): one pass with both
   * copies. */
  u64 b0 = ((u64)f->tsharp[0][0] | ((u64)(f->tsharp[0][1] & 0xff) << 32)) << 8;
  u64 b1 = ((u64)f->tsharp[1][0] | ((u64)(f->tsharp[1][1] & 0xff) << 32)) << 8;
  u32 eyes = b1 && b1 != b0 ? 2 : 1;
  static u32 used[4]; /* the seq each slot last went with (0: never) */
  if (!g_menu_slot) {
    g_menu_slot = vr2_dalloc(4 * sizeof(struct slot), 0x10000);
    if (!g_menu_slot) {
      LOG("inset: no memory for the menu's pass");
      return;
    }
    for (u32 i = 0; i < 4; i++)
      g_menu_slot[i].label = 0, used[i] = 0;
  }
  u32 seq = g_menu_seq + 1;
  for (u32 e = 0; e < eyes; e++) {
    u32 i = (seq & 1) * 2 + e;
    if (used[i] && g_menu_slot[i].label != used[i] * 8 + 4)
      return; /* its last use still on the GPU */
    const u32 *t = f->tsharp[e];
    if (((t[1] >> 20) & 0x3f) != 10 || (t[3] >> 28) != 9)
      return;
  }
  g_menu_seq = seq;
  for (u32 e = 0; e < eyes; e++) {
    u32 i = (seq & 1) * 2 + e;
    struct slot *s = &g_menu_slot[i];
    const u32 *t = f->tsharp[e];
    u32 tw = (t[2] & 0x3fff) + 1, th = ((t[2] >> 14) & 0x3fff) + 1;
    struct consts *k = &s->k;
    memset(k, 0, sizeof *k);
    memcpy(k->tex, t, 32);
    k->tex[1] &= ~(0xfu << 26);
    k->tex[3] &= ~(1u << 26); /* (read-only memory type: stores dropped) */
    k->w = (s32)tw, k->h = (s32)th, k->stage = 2;
    u64 a = (u64)pic;
    u32 mt[8] = {(u32)(a >> 8), (u32)((a >> 40) & 0xff) | (10u << 20), (MENU_W - 1) | ((MENU_H - 1) << 14),
                 0xfacu | (8u << 20) | (9u << 28), (MENU_W - 1) << 13, 0, 0, 0};
    memcpy(k->mtex, mt, 32);
    if (eyes == 2) {
      /* (the eye's copy once: with both of the shader's copies at it, two threads of one dispatch
       * read and wrote each texel, and the menu was blended once or twice as they fell; the second
       * copy off the texture, as vr2.c puts it for a flat page, and not dispatched) */
      k->mat[0][0] = at[e][0], k->mat[0][1] = at[e][1];
      k->mat[1][0] = (s32)tw, k->mat[1][1] = (s32)th;
    } else
      memcpy(k->mat, at, sizeof k->mat);
    k->mw = (s32)MENU_W, k->mh = (s32)MENU_H;
    u32 ndw = build(s, seq, eyes == 2 ? MENU_W : 2 * MENU_W, MENU_H);
    __asm__ volatile("mfence" ::: "memory");
    if (gen1_submit_acb(s->cb, ndw) != 0)
      s->label = seq * 8 + 4;
    used[i] = seq;
  }
}
