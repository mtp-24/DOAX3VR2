/* Running Hmd2's GPU work in a PS4-mode ("gen1") GPU context.
 *
 * The PS4 game's GPU context runs GCN code and GCN descriptors. Hmd2 builds an RDNA compute
 * buffer per headset refresh (Agc queue 0x28, submitted through AgcDriver's export F5ZTyyVUHTs)
 * and flips the headset from the GPU. Every buffer is rewritten just before it is submitted:
 *  - its profiler markers (SQ_THREAD_TRACE_USERDATA writes) are privileged in gen1 and become
 *    NOPs ("GPU Bad packet error: Privilege reg" otherwise);
 *  - RELEASE_MEM / ACQUIRE_MEM take their gfx7 compute forms (the RDNA ones never complete);
 *  - the shader becomes the GCN translation the payload built from this console's Hmd2
 *    (struct agent_gcn), with a gfx7 COMPUTE_PGM_RSRC1;
 *  - the RDNA texture descriptors in its constant data become GCN ones (the headset buffers are
 *    linear for this, see ps5compat.c); the game's own eye textures are GCN already and are put
 *    back where Hmd2 rewrote their size in RDNA layout.
 * Hmd2's flip on the GPU (sceAgcDriverSetFlip: an EOP flip the kernel performs on an interrupt
 * that a gen1 queue never raises) becomes a CPU flip once the frame's GPU work is done, and its
 * wait for that work polls the label instead of an event that never comes.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "vr2.h"

volatile u32 g_display_pitch = 4032;
static u32 *g_shader; /* the GCN code in GPU-visible memory */

int gen1_upload_shader(void) {
  if (g_shader)
    return 0;
  if (!g_gcn.dwords || g_gcn.dwords > GCN_MAX_DWORDS)
    return -1;
  u32 *m = vr2_dalloc_pref(g_gcn.dwords * 4 + 0x100, 0x10000, "the reprojection's GCN shader");
  if (!m)
    return -1;
  memcpy(m, g_gcn.code, g_gcn.dwords * 4);
  g_shader = m;
  return 0;
}

/* ---------------------------------------------------------------- descriptors */
#include "tsharp.h" /* gfx10 -> GCN descriptors (a header for the host tests) */

#define MAX_GAME_TSHARPS (2 * MAX_PAIRS) /* up to MAX_PAIRS pairs */
static u32 g_game_tsharp[MAX_GAME_TSHARPS][8];
static volatile u32 g_ngame_tsharp;

void gen1_note_game_tsharp(const u32 *t) {
  u32 n = g_ngame_tsharp;
  /* (an array texture's slices share a base: kept apart by the slice, word 5's base array field) */
  for (u32 i = 0; i < n; i++)
    if (g_game_tsharp[i][0] == t[0] && (g_game_tsharp[i][1] & 0xff) == (t[1] & 0xff) &&
        (g_game_tsharp[i][5] & 0x1fff) == (t[5] & 0x1fff))
      return;
  if (n < MAX_GAME_TSHARPS) {
    memcpy(g_game_tsharp[n], t, 32);
    __atomic_store_n(&g_ngame_tsharp, n + 1, __ATOMIC_RELEASE);
  }
}

static int restore_game_tsharp(u32 *t) {
  u32 n = g_ngame_tsharp;
  /* an array's slices (Dead or Alive Xtreme 3's eyes) share base and type: the one whose slice
   * Hmd2's copy still carries (word 5's base array, where both layouts have it); none such (Hmd2
   * did not carry it), the first of that base, said once */
  int exact = 0;
  for (u32 i = 0; i < n && !exact; i++)
    exact = g_game_tsharp[i][0] == t[0] && (g_game_tsharp[i][1] & 0xff) == (t[1] & 0xff) &&
            (t[3] >> 28) == (g_game_tsharp[i][3] >> 28) && (g_game_tsharp[i][3] >> 28) == 13 &&
            (g_game_tsharp[i][5] & 0x1fff) == (t[5] & 0x1fff);
  if (!exact && (t[3] >> 28) == 13) {
    static u32 said;
    if (said++ < 4)
      LOG("gen1: an array T# from Hmd2 with slice %u matches none the game gave (words 4-5 %08x %08x): "
          "the first of its base", t[5] & 0x1fff, t[4], t[5]);
  }
  for (u32 i = 0; i < n; i++)
    if (g_game_tsharp[i][0] == t[0] && (g_game_tsharp[i][1] & 0xff) == (t[1] & 0xff) &&
        (t[3] >> 28) == (g_game_tsharp[i][3] >> 28) &&
        (!exact || (g_game_tsharp[i][5] & 0x1fff) == (t[5] & 0x1fff))) {
      /* Already this texture's GCN T# (converted by an earlier dispatch that shares these
       * constants): left alone. Read again as Hmd2's gfx10 T#, its width comes out wrong (its
       * bits sit elsewhere in the GCN word; the height's are where gfx10 has them). */
      {
        u32 want1 = g_game_tsharp[i][1];
        if (((want1 >> 26) & 0xf) == 9)
          want1 &= ~(0xfu << 26);
        if (t[1] == want1 && t[3] == g_game_tsharp[i][3] && t[4] == g_game_tsharp[i][4] &&
            t[5] == g_game_tsharp[i][5] && t[6] == g_game_tsharp[i][6] && t[7] == g_game_tsharp[i][7]) {
          static u32 again;
          if (again++ < 3)
            LOG("gen1: the game's T# seen again already converted (%ux%u): left alone",
                (t[2] & 0x3fff) + 1, ((t[2] >> 14) & 0x3fff) + 1);
          return 1;
        }
      }
      memcpy(t, g_game_tsharp[i], 32);
      /* The game's eye textures are sRGB-encoded, and Hmd2's reprojection writes what it samples
       * straight into the headset buffer: sampled as sRGB (decoded to linear) they come out far
       * too dark. Read the raw encoded values, as the TV mirror does. */
      if (((t[1] >> 26) & 0xf) == 9) /* NUM_FORMAT SRGB -> UNORM */
        t[1] &= ~(0xfu << 26);
      return 1;
    }
  return 0;
}

/* agent.tvhmd=1 (mirror.c): the headset's own picture on the TV. Hmd2's output, the 4000-wide
 * headset buffer (linear, see ps5compat.c), as converted here: the two newest of different bases
 * (Hmd2 draws each refresh into the buffer after the one on the headset), the older one finished. */
static u32 g_hmd_out[2][8];
static volatile u32 g_hmd_out_n;
static void note_hmd_out(const u32 *t) {
  if ((t[2] & 0x3fff) + 1 != 4000)
    return;
  u32 n = g_hmd_out_n;
  if (n && g_hmd_out[(n - 1) & 1][0] == t[0] && g_hmd_out[(n - 1) & 1][1] == t[1])
    return; /* the same buffer again */
  memcpy(g_hmd_out[n & 1], t, 32);
  __atomic_store_n(&g_hmd_out_n, n + 1, __ATOMIC_RELEASE);
}
/* the older of the two (written over next, so read and checked again) */
int gen1_hmd_out(u32 *t) {
  for (int tries = 0; tries < 3; tries++) {
    u32 n = __atomic_load_n(&g_hmd_out_n, __ATOMIC_ACQUIRE);
    if (n < 2)
      return 0;
    memcpy(t, g_hmd_out[n & 1], 32);
    if (__atomic_load_n(&g_hmd_out_n, __ATOMIC_ACQUIRE) == n)
      return 1;
  }
  return 0;
}

/* Hmd2's constant data for one dispatch: A (0x100 bytes, shared) and B (0x478, per eye) */
static void convert_descriptors(u32 *a, u32 *b) {
  u32 *bufs[2] = {a, b}, sizes[2] = {0x100 / 4, 0x478 / 4};
  for (int k = 0; k < 2; k++) {
    if (!bufs[k])
      continue;
    for (u32 i = 0; i + 8 <= sizes[k]; i++)
      if (restore_game_tsharp(bufs[k] + i) || (is_gfx10_tsharp(bufs[k] + i) &&
                                               (tsharp_to_gcn(bufs[k] + i), note_hmd_out(bufs[k] + i), 1)))
        i += 7;
  }
}

/* ---------------------------------------------------------------- the compute buffer */
static volatile u64 *volatile g_wait_label;
static volatile u64 g_wait_value;

static void convert_buffer(u32 *cb, u32 ndw) {
  u32 i = 0;
  while (i < ndw) {
    u32 h = cb[i], type = h >> 30;
    if (type == 2) {
      i++;
      continue;
    }
    u32 len = ((h >> 16) & 0x3fff) + 2;
    if (type != 3 || i + len > ndw)
      return;
    u32 op = (h >> 8) & 0xff, *d = cb + i + 1;
    if ((op == 0x79 && (d[0] & 0xffff) >= 0x340 && (d[0] & 0xffff) <= 0x347) ||
        (op == 0x37 && ((d[0] >> 8) & 0xf) == 0 && d[1] >= 0xc340 && d[1] <= 0xc347)) {
      cb[i] = 0xc0001000u | ((len - 2) << 16); /* profiler marker -> NOP */
    } else if (op == 0x76 && len >= 4 && d[0] == 0x20c && g_shader) {
      u64 pgm = (u64)g_shader; /* COMPUTE_PGM_LO/HI */
      d[1] = (u32)(pgm >> 8), d[2] = (u32)(pgm >> 40);
    } else if (op == 0x76 && len >= 4 && d[0] == 0x212 && g_shader) {
      /* COMPUTE_PGM_RSRC1, gfx7: register counts, float mode 0xc0, DX10 clamp */
      d[1] = g_gcn.rsrc1_vgprs | (g_gcn.rsrc1_sgprs << 6) | (0xc0u << 12) | (1u << 21);
    } else if (op == 0x76 && len >= 6 && d[0] == 0x240 && d[1]) {
      /* COMPUTE_USER_DATA_0..3: the two constant buffers */
      u32 *ca = (u32 *)(((u64)d[2] << 32) | d[1]), *cbb = (u32 *)(((u64)d[4] << 32) | d[3]);
      convert_descriptors(ca, cbb);
    } else if (op == 0x15 && len >= 5) {
      d[3] &= ~(1u << 13); /* DISPATCH_INITIATOR: no RDNA-only bits */
    } else if (op == 0x49 && len == 8) {
      /* RELEASE_MEM, gfx7 compute form (Linux gfx_v7): TCL1|TC|TC_WB, EOP flush, no interrupt */
      u32 data_sel = d[1] >> 29, lo = d[2], hi = d[3], v0 = d[4], v1 = d[5];
      cb[i] = 0xc0054900u;
      d[0] = (1u << 16) | (1u << 17) | (1u << 15) | (5u << 8) | 0x14;
      d[1] = data_sel << 29;
      d[2] = lo, d[3] = hi, d[4] = v0, d[5] = v1;
      d[6] = 0x80000000u;
      if (data_sel == 1 || data_sel == 2) {
        g_wait_label = (volatile u64 *)(((u64)hi << 32) | lo);
        g_wait_value = data_sel == 2 ? ((u64)v1 << 32 | v0) : v0;
      }
    } else if (op == 0x58 && len == 8) {
      /* ACQUIRE_MEM, gfx7 compute form: TCL1, TC, K$, I$ over everything */
      cb[i] = 0xc0055800u;
      d[0] = (1u << 22) | (1u << 23) | (1u << 27) | (1u << 29);
      d[1] = 0xffffffffu, d[2] = 0xff, d[3] = 0, d[4] = 0, d[5] = 0x0a;
      d[6] = 0x80000000u;
    }
    i += len;
  }
}

struct cb_desc {
  u64 addr;
  u32 dwords;
  u8 flags;
};

static void convert(const u64 *addrs, const u32 *dwords, u32 count) {
  for (u32 k = 0; k < count; k++)
    if (addrs[k] && dwords[k] && dwords[k] < 0x100000)
      convert_buffer((u32 *)addrs[k], dwords[k]);
}

/* Hmd2's queue, which the TV mirror (mirror.c) submits its own small dispatch on */
static volatile u32 g_q_ready, g_q, g_q_flags;
static volatile u64 g_q_arg;

/* Hmd2 builds each refresh just after the flip before, from the newest frame passed by then:
 * the build's submit, with the flipArg it set for it (vr2.c's frame count). The hand-off
 * (vr2.c) knows from it, within 0.25 ms of a flip, whether a frame it passed was in time. */
static volatile u32 g_built_n;
static volatile u64 g_built_arg;
static volatile u64 g_flip_arg; /* Hmd2's flip (set_flip, below) */
u32 gen1_last_built(u32 *frame) {
  u32 n = g_built_n;
  *frame = (u32)g_built_arg;
  return n;
}

static int submit_f5(u32 q, const struct cb_desc *c, u64 arg) {
  convert(&c->addr, &c->dwords, 1);
  g_built_arg = g_flip_arg;
  __atomic_fetch_add(&g_built_n, 1, __ATOMIC_RELEASE);
  if (!g_q_ready) {
    g_q = q, g_q_arg = arg, g_q_flags = c->flags;
    __atomic_store_n(&g_q_ready, 1, __ATOMIC_RELEASE);
    LOG("gen1: Hmd2 queue %u (arg 0x%lx, flags 0x%x)", q, arg, c->flags);
  }
  return sceAgcDriverSubmitAcbF5 ? sceAgcDriverSubmitAcbF5(q, c, arg) : -1;
}

int gen1_queue_ready(void) { return __atomic_load_n(&g_q_ready, __ATOMIC_ACQUIRE); }

/* A GCN compute buffer of our own, not rewritten (AgcDriver locks the queue per submit). */
int gen1_submit_acb(const u32 *cb, u32 ndw) {
  if (!gen1_queue_ready() || !sceAgcDriverSubmitAcbF5)
    return -1;
  struct cb_desc d = {(u64)cb, ndw, (u8)g_q_flags};
  return sceAgcDriverSubmitAcbF5(g_q, &d, g_q_arg);
}
static int submit_dcb(const struct cb_desc *c) {
  convert(&c->addr, &c->dwords, 1);
  return sceAgcDriverSubmitDcb ? sceAgcDriverSubmitDcb(c) : -1;
}
static int submit_acb(u32 q, const struct cb_desc *c) {
  convert(&c->addr, &c->dwords, 1);
  return sceAgcDriverSubmitAcb ? sceAgcDriverSubmitAcb(q, c) : -1;
}
static int submit_multi_dcbs(void *const *a, const u32 *n, u32 count) {
  convert((const u64 *)a, n, count);
  return sceAgcDriverSubmitMultiDcbs ? sceAgcDriverSubmitMultiDcbs(a, n, count) : -1;
}
static int submit_multi_acbs(u32 q, void *const *a, const u32 *n, u32 count) {
  convert((const u64 *)a, n, count);
  return sceAgcDriverSubmitMultiAcbs ? sceAgcDriverSubmitMultiAcbs(q, a, n, count) : -1;
}

/* ---------------------------------------------------------------- flips and waits */
static volatile int g_flip_pending;
static volatile int g_flip_handle, g_flip_index, g_flip_mode; /* g_flip_arg: above */

/* Agc's sceAgcAcbSetFlip -> AgcDriver's SetFlip: the packet space becomes one NOP, the flip is
 * kept for the CPU (cpu_flip) */
static int set_flip(u32 **cursor, u32 size_dw, u32 engine, int handle, int index, int mode,
                    u64 arg) {
  (void)engine;
  u32 *p = *cursor;
  if (size_dw >= 2) {
    p[0] = 0xc0001000u | ((size_dw - 2) << 16);
    memset(p + 1, 0, (size_dw - 1) * 4);
  }
  *cursor = p + size_dw;
  g_flip_handle = handle, g_flip_index = index, g_flip_mode = mode, g_flip_arg = arg;
  __atomic_store_n(&g_flip_pending, 1, __ATOMIC_RELEASE);
  return 0;
}

/* headset refreshes Hmd2 has flipped: the game's 120 Hz events follow them (hooks.c) */
static volatile u32 g_refreshes;
static volatile u64 g_flip_us; /* when the last was flipped */
u32 gen1_refreshes(void) { return __atomic_load_n(&g_refreshes, __ATOMIC_ACQUIRE); }
u64 gen1_last_flip(void) { return g_flip_us; }
/* the headset's flips since the last call: how many came more than a refresh and a half after the
 * one before (a refresh without a flip), and the longest gap (us) */
static volatile u32 g_flip_late, g_flip_gap_max;
void gen1_flip_stats(u32 *late, u32 *gap_max) {
  *late = __atomic_exchange_n(&g_flip_late, 0, __ATOMIC_RELAXED);
  *gap_max = __atomic_exchange_n(&g_flip_gap_max, 0, __ATOMIC_RELAXED);
}

static void cpu_flip(void) {
  if (!__atomic_exchange_n(&g_flip_pending, 0, __ATOMIC_ACQ_REL))
    return;
  int rc = sceVideoOutSubmitFlip(g_flip_handle, g_flip_index, g_flip_mode, (s64)g_flip_arg);
  static u32 errors;
  if (rc && errors++ < 8)
    LOG("gen1: headset flip -> 0x%x", rc);
  if (!rc) {
    u64 t = sceKernelGetProcessTime(), d = g_flip_us ? t - g_flip_us : 0;
    if (d > 12500 && d < 1000000) /* (more than a refresh and a half after the last: one missed) */
      __atomic_fetch_add(&g_flip_late, 1, __ATOMIC_RELAXED);
    if (d < 1000000 && d > g_flip_gap_max)
      g_flip_gap_max = (u32)d;
    g_flip_us = t;
    __atomic_fetch_add(&g_refreshes, 1, __ATOMIC_RELEASE);
    hooks_flip((u32)g_flip_arg);
  }
}

/* Hmd2 waits up to 10 ms on its GPU queue's event after each submit (the waits at Hmd2 offsets
 * 0x400-0x1200), then spins on the label: here the label is polled, then the frame flipped. */
static u64 g_hmd2_base;

static int wait_equeue(void *eq, void *ev, int num, int *out, u32 *timo) {
  u64 ra = (u64)__builtin_return_address(0);
  if (g_hmd2_base && ra - g_hmd2_base >= 0x400 && ra - g_hmd2_base < 0x1200) {
    volatile u64 *label = g_wait_label;
    u64 want = g_wait_value;
    if (out)
      *out = 0;
    for (int k = 0; k < 400 && label; k++) {
      if (*label == want) {
        cpu_flip();
        return 0;
      }
      sceKernelUsleep(50);
    }
    static u32 timeouts;
    if (label && timeouts++ < 8)
      LOG("gen1: GPU wait timed out (label %lx, want %lx)", *label, want);
    return (int)0x8002003c;
  }
  return sceKernelWaitEqueue(eq, ev, num, out, timo);
}

void gen1_install(const int h[NLIBS]) {
  g_hmd2_base = vr2_module_base(h[LIB_HMD2]);
  for (int i = LIB_AGC; i <= LIB_HMD2; i++) {
    vr2_override(h[i], NID_sceAgcDriverSubmitAcbF5, (void *)submit_f5);
    vr2_override(h[i], NID_sceAgcDriverSubmitDcb, (void *)submit_dcb);
    vr2_override(h[i], NID_sceAgcDriverSubmitAcb, (void *)submit_acb);
    vr2_override(h[i], NID_sceAgcDriverSubmitMultiDcbs, (void *)submit_multi_dcbs);
    vr2_override(h[i], NID_sceAgcDriverSubmitMultiAcbs, (void *)submit_multi_acbs);
  }
  vr2_override(h[LIB_AGC], NID_sceAgcDriverSetFlip, (void *)set_flip);
  vr2_override(h[LIB_HMD2], NID_sceKernelWaitEqueue, (void *)wait_equeue);
}
