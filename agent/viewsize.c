/* The VR views' render sizes, the payload's (all patched at the game's start; the settings menu's
 * Resolution preset, agent.res, sets them, and a debug build's cvars.txt the raw ones over it):
 * agent.widew=M (the wide views on render mode 1 at M, a per-view switch at the view loop's head;
 * logs/research/gpu120/REPORT.md candidate C), agent.mode0=W (render mode 0, the memory every VR
 * view is made in, W x 9W/16: agent.renderw past 1280; logs/research/mode0/REPORT.md option 1) and
 * agent.nearn=F (the near view's half angle F/100 of the top tangent, the game's 50) and
 * agent.vdensity=P, agent.vdensitywide=Q (the views' heights P% of the game's, mode 1's Q%: an even
 * pixel density both ways; logs/research/gpu120/DENSITY-FXAA.md) and
 * agent.renderw (mode 3: the near views, and the wide ones without widew); agent.nearcull=1 (the
 * near views culled with their own field of view; logs/research/gpu120/NEAR.md): its two counters.
 * Here the options are taken and, every 10 s on the agent's cvars thread, the switch's two counters
 * are read and logged, once with the sizes the game built its modes 0, 1 and 3 at (the grid's
 * colour descriptors: mode m's at grid + 0x50 + 0xc0 * m, width at +4, height at +8; built at
 * 0x4ae600 Scarlet / 0x4ad990 Fortune from the table; both builds: the sizes a preset gave). gaze.c
 * takes the near view's factor (its check of n) as the payload wrote it in the game.
 * Only read: nothing in the game is written from here.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "vr2.h"

static u32 g_vs_nearn; /* agent.nearn as given (25..49), 0 the game's 0.5 */

int viewsize_option(const char *key, u32 klen, const char *val, u32 vlen) {
  if (klen == 5 && memcmp(key, "nearw", 5) == 0) {
    LOG("cvars: agent.nearw is gone: agent.renderw sizes the near views (and agent.widew the wide ones)");
    return 1;
  }
  int wide = klen == 5 && memcmp(key, "widew", 5) == 0, mode0 = klen == 5 && memcmp(key, "mode0", 5) == 0,
      nearn = klen == 5 && memcmp(key, "nearn", 5) == 0;
  int vd = (klen == 8 && memcmp(key, "vdensity", 8) == 0) || (klen == 12 && memcmp(key, "vdensitywide", 12) == 0);
  int cull = klen == 8 && memcmp(key, "nearcull", 8) == 0;
  if (!wide && !mode0 && !nearn && !vd && !cull)
    return 0;
#ifdef RELEASE
  (void)val, (void)vlen;
  LOG("cvars: agent.%.*s is a debug option: not in a release build (the settings menu's Resolution sets the "
      "sizes)", (int)klen, key);
#else
  u32 v = 0;
  for (u32 i = 0; i < vlen && val[i] >= '0' && val[i] <= '9'; i++)
    v = v * 10 + (u32)(val[i] - '0');
  if (nearn) /* (the payload's range: anything else leaves the game's 0.5) */
    g_vs_nearn = v >= 25 && v <= 49 ? v : 0;
  LOG("viewsize: agent.%.*s=%.*s is the payload's (patched at the game's start; its log says what)%s",
      (int)klen, key, (int)vlen, val,
      wide ? "; the switch's counts here every 10 s" : cull ? "; its counts here every 10 s" : "");
#endif
  return 1;
}

static const struct vs_build {
  const char *name;
  u32 site, cave, count, grid, nearn; /* nearn: the near view's 0.5 (the payload's patch_nearn) */
  u8 site_orig[7]; /* movl %r8d, <view index>(%rip) */
} k_vsb[] = {
    {"Scarlet 1.08", 0x4cdd46, 0x89ef90, 0x1df6c40, 0x1c2b450, 0x7bbc68, {0x44, 0x89, 0x05, 0x5b, 0x8e, 0x92, 0x01}},
    {"Fortune 1.19", 0x4cc3a6, 0x895580, 0x1deeb20, 0x1c23330, 0x7b44d4, {0x44, 0x89, 0x05, 0xdb, 0x26, 0x92, 0x01}},
    {"Scarlet 1.00", 0x4cc286, 0x89d4e0, 0x1df6c00, 0x1c2b410, 0x7ba318, {0x44, 0x89, 0x05, 0xdb, 0xa8, 0x92, 0x01}},
};
#define N_VSB (int)(sizeof k_vsb / sizeof k_vsb[0])

/* the site holds the switch's call (2), the game's own instruction (1), or neither (0: not this build) */
static int vs_site(const u8 *base, int i) {
  const u8 *s = base + k_vsb[i].site;
  s32 rel;
  memcpy(&rel, s + 1, 4);
  if (s[0] == 0xe8 && s[5] == 0x66 && s[6] == 0x90 && k_vsb[i].site + 5 + rel == k_vsb[i].cave)
    return 2;
  return memcmp(s, k_vsb[i].site_orig, 7) == 0;
}

/* [p, p + n) in one readable map entry (the game's text may not be readable: checked, as touch.c) */
static int vs_readable(const void *p, u64 n) {
  u8 info[128];
  memset(info, 0, sizeof info);
  if (!sceKernelVirtualQuery || sceKernelVirtualQuery(p, 0, info, 0x48) != 0)
    return 0;
  u64 start = *(u64 *)info, end = *(u64 *)(info + 8);
  s32 prot = *(s32 *)(info + 24);
  return start <= (u64)p && (u64)p + n <= end && (prot & 1);
}

/* the factor the game's n is made of the top tangent with (gaze.c's check): read from the game,
 * where the payload wrote it at the start (the Resolution preset's or cvars.txt's agent.nearn), once
 * readable; until then agent.nearn as given, else the game's 0.5 */
float viewsize_near_factor(void) {
  static float f;
  if (f > 0.0f)
    return f;
  const u8 *base = (const u8 *)astro_hdr.game_base;
  for (int i = 0; base && i < N_VSB; i++)
    if (vs_readable(base + k_vsb[i].site, 7) && vs_site(base, i) && vs_readable(base + k_vsb[i].nearn, 4)) {
      float v = *(const volatile float *)(base + k_vsb[i].nearn);
      if (v >= 0.25f && v <= 0.5f) {
        f = v;
        LOG("viewsize: %s: the near view's half angle %u/100 of the top tangent (the game's 50)", k_vsb[i].name,
            (u32)(v * 100.0f + 0.5f));
        return f;
      }
    }
  return g_vs_nearn ? (float)g_vs_nearn / 100.0f : 0.5f;
}

#ifndef RELEASE
/* agent.nearcull (the payload's patch_nearcull): the frustum's first call made a call to code at the
 * switch's place + 0x100; its two counters after the switch's (narrowed, kept by its checks), about
 * three a VR frame (the right eye's views build the frustum twice) */
static const u32 k_ncsite[N_VSB] = {0x4a0c1c, 0x49ffac, 0x49f15c};
static int g_nc_on = -1; /* -1 not looked yet */
static void viewsize_nearcull(const u8 *base, int i) {
  int on = g_nc_on;
  static u32 last[2];
  if (on < 0) {
    const u8 *s = base + k_ncsite[i];
    s32 rel;
    on = 0;
    if (vs_readable(s, 5) && vs_readable(base + k_vsb[i].count + 8, 8)) {
      memcpy(&rel, s + 1, 4);
      on = s[0] == 0xe8 && k_ncsite[i] + 5 + rel == k_vsb[i].cave + 0x100;
    }
    g_nc_on = on;
    if (on)
      LOG("viewsize: %s: the near views culled with their own field of view (agent.nearcull)", k_vsb[i].name);
  }
  if (!on)
    return;
  const volatile u32 *c = (const volatile u32 *)(base + k_vsb[i].count + 8);
  u32 now_c[2] = {c[0], c[1]};
  LOG("viewsize: nearcull: in the last 10 s %u near-view frusta narrowed (about 3 a VR frame), %u kept wide by "
      "its checks", now_c[0] - last[0], now_c[1] - last[1]);
  last[0] = now_c[0], last[1] = now_c[1];
}
#endif

void viewsize_report(void) {
  static int g = -2, sized; /* g: -2 not looked yet, -1 not installed */
  static u64 next;
  u8 *base = (u8 *)astro_hdr.game_base;
#ifndef RELEASE
  if (!base || (g == -1 && sized && g_nc_on == 0))
#else
  if (!base || (g == -1 && sized))
#endif
    return;
  u64 now = sceKernelGetProcessTime();
  if (now < next)
    return;
  next = now + 10000000;
  if (!sized) { /* the sizes the game built its modes at, once they are built (its boot); every build */
    for (int i = 0; i < N_VSB && !sized; i++) {
      const u8 *grid = base + k_vsb[i].grid;
      if (!vs_readable(base + k_vsb[i].site, 7) || !vs_readable(grid + 0x50, 0x300) || !vs_site(base, i))
        continue;
      const u32 *m0 = (const u32 *)(grid + 0x50), *m1 = (const u32 *)(grid + 0x50 + 0xc0),
                *m3 = (const u32 *)(grid + 0x50 + 0xc0 * 3);
      if (!m0[1] || !m3[1])
        break; /* not built yet: the next 10 s */
      sized = 1;
      LOG("viewsize: %s: the game built mode 0 at %ux%u, mode 1 at %ux%u (the wide views with agent.widew) "
          "and mode 3 at %ux%u (the near views, and the wide ones without widew)%s",
          k_vsb[i].name, m0[1], m0[2], m1[1], m1[2], m3[1], m3[2],
          m0[1] == 1920 && m3[1] == 940 && m3[2] == 1052 && m1[1] == 1440 && m1[2] == 810 ? " (the game's own)" : "");
    }
  }
#ifndef RELEASE
  for (int i = 0; i < N_VSB; i++) /* (agent.nearcull, with or without the switch) */
    if (vs_readable(base + k_vsb[i].site, 7) && vs_site(base, i))
      viewsize_nearcull(base, i);
  static u32 last[2];
  if (g == -2) {
    g = -1;
    for (int i = 0; i < N_VSB; i++)
      if (vs_readable(base + k_vsb[i].site, 7) && vs_readable(base + k_vsb[i].count, 8) && vs_site(base, i) == 2)
        g = i;
    if (g < 0) {
      if (!sized)
        g = -2; /* (looked again until the sizes are logged) */
      return; /* agent.widew not given, or not applied (the payload's log says why) */
    }
    LOG("viewsize: %s: the per-view switch is in (agent.widew)", k_vsb[g].name);
  }
  if (g < 0)
    return;
  const volatile u32 *c = (const volatile u32 *)(base + k_vsb[g].count);
  u32 now_c[2] = {c[0], c[1]};
  LOG("viewsize: in the last 10 s %u views switched (4 a VR frame), %u refused by its checks", now_c[0] - last[0],
      now_c[1] - last[1]);
  last[0] = now_c[0], last[1] = now_c[1];
#else
  g = -1; /* (the per-view switch's counts: a debug build's) */
#endif
}
