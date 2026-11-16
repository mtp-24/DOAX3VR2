/* The in-headset menu: what can be switched while the game runs, from the controller, drawn in
 * the headset. A release build has the player's settings menu (k_items below, its choices saved
 * to the game's menu-<game>.txt); a debug build the debug menu: the settings menu's lines first,
 * the same and saved the same way, then the debug lines (live options of the port, not saved).
 *  - L1 + R1 held, then a click of the touchpad: open or close it. The touchpad click goes to the
 *    agent while both shoulder buttons are held.
 *  - Open, the game gets no buttons, sticks or triggers. Up and down choose a line, left and
 *    right change its value (cross too, upwards, wrapping), circle closes it.
 *  - Each line is one of the agent's options. A change takes at once; the lines marked * take
 *    from the next start (through the settings menu's file). Not here: the palm tracker's live
 *    start (never started mid-game).
 *  - Reset all puts every line back to its default (the value without the menu's file: the
 *    port's own, or cvars.txt's), and removes the file.
 * The settings file (both builds): /data/doax3vr2/menu-<game>.txt, "name=value" lines
 * in cvars.txt's names (agent.hands=0, agent.fanswings=1, ...; the Resolution preset as agent.res,
 * the payload's res_settings; an older file's eye width, agent.resolution=N, is read as a preset
 * and written again as agent.res), fetched from the payload as the
 * agent's options are read (CMD_MENU, menu_boot: its lines applied there, after cvars.txt's own,
 * so the menu's choices win) and written through the payload from the cvars thread within 2 s of
 * a change (menu_save): a line for each setting the player changed from the default, and no
 * file at all when there is none.
 * The picture (MENU_W x MENU_H, RGBA) is drawn here on the CPU into one of two buffers the GPU
 * reads, and copied into both eyes, head-locked a little below straight ahead at about 1.2 m
 * (each eye's copy moved in by that distance's half disparity), by the menu shader's stage 2
 * (inset.c inset_menu_frame, from vr2.c every frame it is open). Out of VR it is drawn into the
 * TV's picture (hooks.c flat_menu_thread).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "vr2.h"
#include "font8x12.h"

#define PAD_L1 0x0400u
#define PAD_R1 0x0800u
#define PAD_TOUCH 0x100000u
#define PAD_UP 0x0010u
#define PAD_RIGHT 0x0020u
#define PAD_DOWN 0x0040u
#define PAD_LEFT 0x0080u
#define PAD_CROSS 0x4000u
#define PAD_CIRCLE 0x2000u
#define PAD_INTERCEPTED 0x80000000u

enum { M_OPT, M_CLOSE, M_RESET, M_VIEW }; /* M_OPT: one of the agent's options; M_VIEW: reset the view */
struct item {
  const char *label;
  int kind;
  volatile u32 *opt;  /* M_OPT */
  int lo, hi, step;
  const char *const *names; /* the values' names from lo, or 0 for the number */
  const u32 *vals;          /* a list: its choices, lo..hi their indices (the line's value its index;
                             * one not on the list, from a file, shown as its number) */
  const char *key;          /* the settings file's line, 0: none (not saved) */
};
static const char *const k_offon[] = {"off", "on"};
/* the player's settings (what cvars.txt can set that a player would, docs/SETTINGS.md): the
 * settings menu, and the debug menu's first lines (the same lines, saved the same way) */
/* Resolution* (agent.res, the payload's res_settings, docs/SETTINGS.md): presets of the game's real
 * render size (agent.renderw), the sharp area (agent.nearn), the wide views (agent.widew) and the eye
 * width, by index: 0 the game's own; 1 sharp (render 1280, sharp area 40/100, wide views 940, eye
 * 1400: measured clean at 120 fps, logs/perf/stage30.out), the default at 120 fps; 2 sharp, wide
 * area (render 1280 everywhere, eye 1400); 3 and 4 the 60 fps ones (render 1280, the eye 2048 or
 * 2400 supersampled; the payload makes them 1 at 120 fps), 3 the default at 60 fps; 5 sharp+ (render
 * 1440 on mode 0 at 2688x1512, heights 96%/91%, wide views 700, eye 1440: measured as steady as 1 at
 * 120 fps, logs/perf/chain37.out combo1440b; appended so a saved 0-4 keeps its preset). (Before
 * 2026-11-14 the line was "Eye width*", agent.resolution: only the last pass's size, no new detail;
 * res_legacy maps an older file's.) */
static const char *const k_res[] = {"game's own",    "sharp (120 fps)", "wide (60 fps)", "2048 (60 fps)",
                                    "2400 (60 fps)", "sharp+ (120 fps)", "2720 (60 fps)"};
#define RES_LAST 6u /* (the payload's RES_N - 1) */
#define RES_DEF(fps) ((fps) ? 5u : 3u) /* the default by the frame rate (the payload's): sharp+ at 120 (the user, 2026-11-14) */
static u32 res_legacy(int w) { return w <= 0 ? 0u : w <= 1400 ? 1u : w <= 2048 ? 3u : w <= 2400 ? 4u : 6u; } /* (the payload's res_legacy) */
static const u32 k_hands_vals[] = {0, 60};
/* the Jet Black Fan's swings (agent.fanswings; 0: the game's own 2, the default). The debug menu's
 * line has the other counts too (it was the debug menu's "Fan swings needed") */
#ifdef RELEASE
static const char *const k_fan[] = {"the game's (2)", "easy (1 swing)"};
#define FAN_HI 1
#else
static const char *const k_fan[] = {"the game's (2)", "easy (1 swing)", "2", "3", "4"};
#define FAN_HI 4
#endif
static const char *const k_padmode[] = {"in both hands", "in one hand"};
static const char *const k_tv[] = {"left eye", "both eyes", "black"};
#ifndef RELEASE
static const char *const k_special[] = {"PSN only", "weekly", "everything"};
#endif
/* the world scale (the payload's patch_worldscale at the game's start): the girls' size against
 * the player's, in hundredths; 120 the game's own (logs/research/scale/REPORT.md) */
static const char *const k_scales[] = {"1.0 (true size)", "1.1", "1.2 (the game's)", "1.3"};
static const u32 k_scale_vals[] = {100, 110, 120, 130};
/* Sharpening (agent.sharpen, compose.c: live): the composed eye's sharpening, in hundredths */
static const char *const k_sharp[] = {"off", "50", "100"};
static const u32 k_sharp_vals[] = {0, 50, 100};
/* the next start's choices (the file's lines; their defaults cvars.txt's, menu_cvar; Touch* and
 * Eye-tracked sharp area* the options as cvars.txt left them, menu_boot) and the TV's
 * (agent.mirrorboth 1, agent.tvblank 2) */
static volatile u32 g_next_res = RES_DEF(1), g_next_hands, g_next_fps = 1, g_next_fade = 0, g_next_scale = 120, g_tv; /* (world scale 1.2, the game's own, by default: the user, 2026-11-15) */
static volatile u32 g_next_touch = 1, g_next_gaze = 1;
static const struct item k_items[] = {
    {"Resolution*", M_OPT, &g_next_res, 0, RES_LAST, 1, k_res, 0, "agent.res"},
    {"Hand tracking*", M_OPT, &g_next_hands, 0, 1, 1, k_offon, k_hands_vals, "agent.hands"},
    /* a picture every refresh of the headset, the game's 60 ticks a second kept (the payload reads
     * the line at the game's start: the header's frame_pace; hooks.c, tick_frame). On by default. */
    {"120 fps*", M_OPT, &g_next_fps, 0, 1, 1, k_offon, 0, "agent.fps120"}, /* (on by default) */
    /* the black fade as the head goes into a girl (the payload's patch at the game's start). Off by default */
    {"Fade inside her*", M_OPT, &g_next_fade, 0, 1, 1, k_offon, 0, "agent.headfade"},
    {"World scale*", M_OPT, &g_next_scale, 0, 3, 1, k_scales, k_scale_vals, "agent.worldscale"},
    /* the photo scenes' seat at the player's own height (height.c): live, taken at the next scene
     * start (a new pose or place); the head's tracking untouched. Off by default (the user, 2026-11-15) */
    {"Real height", M_OPT, &g_opt_realheight, 0, 1, 1, k_offon, 0, "agent.realheight"},
    /* the controller's and the head's touch spheres (touch.c: agent.touch with agent.touchhead, one
     * line for both): made at a scene's start, so taken at the next start. On by default */
    {"Touch*", M_OPT, &g_next_touch, 0, 1, 1, k_offon, 0, "agent.touch"},
    /* the controller's place from the palm tracker (vr.c agent.padmode): live. Both hands by default
     * (the user, 2026-11-15, after a tester's "the controller isn't moving in game" with the one-hand rule) */
    {"Controller", M_OPT, &g_opt_padmode, 0, 1, 1, k_padmode, 0, "agent.padmode"},
    /* the girl looks at the player on a close contact (vrreact.c agent.lookat): live. On by default */
    {"Look at you", M_OPT, &g_opt_lookat, 0, 1, 1, k_offon, 0, "agent.lookat"},
    /* the sharp area follows the eyes, the wide views' hole with it (gaze.c agent.gazefollow with
     * agent.movehole; fov.c's eye tracking, started once at the start by the palm tracker's rule; the
     * hole's mode chosen at the first frame): taken at the next start. Off: the sharp area centred. On
     * by default */
    {"Eye-tracked sharp area*", M_OPT, &g_next_gaze, 0, 1, 1, k_offon, 0, "agent.gazefollow"},
    /* the composed eye sharpened (compose.c agent.sharpen): live. 50 by default (the user, 2026-11-14) */
    {"Sharpening", M_OPT, &g_opt_sharpen, 0, 2, 1, k_sharp, k_sharp_vals, "agent.sharpen"},
    {"Fan shake", M_OPT, &g_opt_fanswings, 0, FAN_HI, 1, k_fan, 0, "agent.fanswings"},
    {"Malfunction, all suits", M_OPT, &g_opt_malfunction, 0, 1, 1, k_offon, 0, "agent.malfunction"},
    {"TV picture", M_OPT, &g_tv, 0, 2, 1, k_tv, 0, "tv"}, /* (two lines: agent.mirrorboth, agent.tvblank) */
#ifndef RELEASE
    /* the debug menu's own lines: live, not saved (their start values cvars.txt's). Its former
     * "Fan swings needed", "TV: both eyes" and "TV: black" are the lines above (Fan shake with
     * all its counts; TV picture) */
    {"Owner Level min", M_OPT, &g_opt_ownerlv, 0, 110, 10, 0, 0, 0},
    {"Zack Dollars min", M_OPT, &g_opt_money, 0, 3000000, 250000, 0, 0, 0},
    {"Special tab", M_OPT, &g_opt_special, 0, 2, 1, k_special, 0, 0},
    {"Fan stars, Owner Shop", M_OPT, &g_opt_starfans, 0, 1, 1, k_offon, 0, 0},
    {"Softness Sensors stocked", M_OPT, &g_opt_sensors, 0, 1, 1, k_offon, 0, 0},
    {"Ported suits batch", M_OPT, &g_opt_portshop, 0, 8, 1, 0, 0, 0},
    {"Characters' log", M_OPT, &g_opt_chars, 0, 1, 1, k_offon, 0, 0},
    {"Compose", M_OPT, &g_opt_compose, 0, 1, 1, k_offon, 0, 0},
    {"Compose width max", M_OPT, &g_opt_composemax, 1024, 4096, 256, 0, 0, 0},
    {"TV: headset view", M_OPT, &g_opt_tvhmd, 0, 1, 1, k_offon, 0, 0},
#endif
    {"Reset view", M_VIEW, 0, 0, 0, 0, 0, 0, 0},
    {"Reset all", M_RESET, 0, 0, 0, 0, 0, 0, 0},
    {"Close", M_CLOSE, 0, 0, 0, 0, 0, 0, 0},
};
#define N_ITEMS (sizeof k_items / sizeof *k_items)

/* cvars_fetch (hooks.c): the payload's options that the menu's start-only lines hold, as
 * cvars.txt has them: the lines' defaults (the menu's file, read after, wins) */
static u32 g_res_cvar; /* cvars.txt's agent.res given: the default whatever the frame rate */
void menu_cvar(const char *key, u32 klen, u32 v) {
  if (klen == 3 && memcmp(key, "res", 3) == 0)
    g_next_res = v <= RES_LAST ? v : RES_DEF(g_next_fps), g_res_cvar = v <= RES_LAST;
  else if (klen == 6 && memcmp(key, "fps120", 6) == 0)
    g_next_fps = v != 0;
  else if (klen == 8 && memcmp(key, "headfade", 8) == 0)
    g_next_fade = v != 0;
  else if (klen == 10 && memcmp(key, "worldscale", 10) == 0)
    g_next_scale = v;
}

static struct {
  volatile u32 open, sel;
  u32 prev, shoulder_touch; /* the last buttons; the touchpad held with both shoulders (hidden) */
  u32 swallow; /* buttons held as the menu closed: hidden from the game until each is let go */
  volatile u32 dirty;       /* the picture to draw again */
  u8 *px[2];                /* the two pictures (GPU-readable) */
  volatile u32 front;       /* the one the GPU copies */
  volatile u32 made;        /* a picture drawn (the front is one) */
  u64 drawn_at;
  u32 said;
  int start[64]; /* each line's value for Reset all: its default, as cvars.txt left it (menu_boot) */
  u8 has_start[64];
  u8 has[64];  /* the saved lines (a key's): the player chose them (from the file, or changed here) */
  int set[64]; /* their values */
  volatile u32 file_dirty; /* the file to write again (menu_save, the cvars thread) */
} g_m;

/* Resolution*'s default follows the 120 fps line (the payload's res_settings: 1 at 120 fps, 3 at 60)
 * unless cvars.txt has agent.res; its value too while the player hasn't chosen one (no line) */
static void res_follow_fps(void) {
  u32 i = 0;
  while (i < N_ITEMS && k_items[i].opt != &g_next_res) i++;
  if (i == N_ITEMS || g_res_cvar)
    return;
  g_m.start[i] = (int)RES_DEF(g_next_fps);
  if (!g_m.has[i])
    g_next_res = RES_DEF(g_next_fps);
}

/* a line's value as it is: the option's (the list's value, not its index) */
static int raw_of(const struct item *it, int *v) {
  if (it->kind == M_OPT) {
    if (it->opt == &g_tv) /* (from the two options: a debug build's cvars.txt changes them live) */
      g_tv = g_opt_tvblank ? 2 : g_opt_mirrorboth ? 1 : 0;
    *v = (int)*it->opt;
    return 1;
  }
  return 0; /* (Close, Reset all, Reset view) */
}
static void raw_set(const struct item *it, int n) {
  if (it->kind == M_OPT)
    *it->opt = (u32)n;
  if (it->opt == &g_tv) /* the TV picture: live (the mirror thread reads both each frame) */
    g_opt_mirrorboth = g_tv == 1, g_opt_tvblank = g_tv == 2;
}
/* the line's value as the menu steps it: 1 with a list's index, else the value itself; 2 with
 * the value itself when a list's isn't one of the list's (set from a file) */
static int value_of(const struct item *it, int *v) {
  if (!raw_of(it, v))
    return 0;
  if (it->vals) {
    u32 o = (u32)*v;
    for (int k = it->lo; k <= it->hi; k++)
      if (it->vals[k] == o) {
        *v = k;
        return 1;
      }
    return 2;
  }
  return 1;
}
/* n: a list's index, else the value */
static void set_value(const struct item *it, int n) {
  int raw = it->vals ? (int)it->vals[n] : n;
  u32 i = (u32)(it - k_items);
  if (it->key)
    g_m.set[i] = raw;
  raw_set(it, raw);
  if (it->key) { /* (set, so the file can have it; the debug menu's own lines are not saved) */
    g_m.has[i] = !(g_m.has_start[i] && raw == g_m.start[i]); /* (back at its default: the menu holds it
                                                               * no more, its line leaves the file) */
    __atomic_store_n(&g_m.file_dirty, 1, __ATOMIC_RELEASE); /* (after has and set: the cvars thread reads them) */
  }
}

static void change(const struct item *it, int dir, int wrap) {
  int v, r = value_of(it, &v);
  if (!r)
    return;
  int was = 0; /* (the list's value before, for the log) */
  if (it->vals)
    raw_of(it, &was);
  if (r == 2) { /* (a value off the list: the list's next one past it) */
    int k = -1;
    for (int q = it->lo; q <= it->hi; q++)
      if (dir > 0 ? (int)it->vals[q] > was && k < 0 : (int)it->vals[q] < was)
        k = q;
    if (k < 0) { /* (none: wrapped to the list's other end, or left as it is) */
      if (!wrap)
        return;
      k = dir > 0 ? it->lo : it->hi;
    }
    set_value(it, k);
    LOG("menu: %s %d -> %u", it->label, was, it->vals[k]);
    return;
  }
  int n = v + dir * it->step, hi = it->hi;
  if (n > hi)
    n = wrap ? it->lo : hi;
  if (n < it->lo)
    n = wrap ? hi : it->lo;
  if (n == v)
    return;
  set_value(it, n);
  if (it->opt == &g_next_fps)
    res_follow_fps();
  if (it->vals)
    LOG("menu: %s %d -> %u", it->label, was, it->vals[n]);
  else
    LOG("menu: %s %d -> %d", it->label, v, n);
}

static void reset_all(void) {
  for (u32 i = 0; i < N_ITEMS && i < 64; i++) {
    const struct item *it = &k_items[i];
    int v;
    g_m.has[i] = 0; /* (the file goes: menu_save) */
    if (!g_m.has_start[i] || !raw_of(it, &v) || v == g_m.start[i])
      continue;
    raw_set(it, g_m.start[i]);
    LOG("menu: %s %d -> %d (reset)", it->label, v, g_m.start[i]);
  }
  res_follow_fps();
  __atomic_store_n(&g_m.file_dirty, 1, __ATOMIC_RELEASE);
}

/* the player's controller sample (hooks.c, the game's pad thread) */
void menu_pad(u8 *d) {
  u32 b = *(u32 *)d, pressed = b & ~g_m.prev, hidden_before = g_m.open || g_m.shoulder_touch;
  g_m.prev = b;
  int shoulders = (b & (PAD_L1 | PAD_R1)) == (PAD_L1 | PAD_R1);
  if (shoulders && (pressed & PAD_TOUCH)) {
    g_m.open = !g_m.open, g_m.dirty = 1;
    LOG("menu: %s", g_m.open ? "open" : "closed");
  }
  g_m.shoulder_touch = shoulders && (b & PAD_TOUCH);
  if (g_m.open) {
    const struct item *it = &k_items[g_m.sel];
    if (pressed & PAD_UP)
      g_m.sel = (g_m.sel + N_ITEMS - 1) % N_ITEMS, g_m.dirty = 1;
    if (pressed & PAD_DOWN)
      g_m.sel = (g_m.sel + 1) % N_ITEMS, g_m.dirty = 1;
    if (pressed & PAD_LEFT)
      change(it, -1, 0), g_m.dirty = 1;
    if (pressed & PAD_RIGHT)
      change(it, 1, 0), g_m.dirty = 1;
    if (pressed & PAD_CROSS) {
      if (it->kind == M_CLOSE)
        g_m.open = 0, LOG("menu: closed");
      else if (it->kind == M_RESET)
        reset_all();
      else if (it->kind == M_VIEW)
        vr_reset_view("the menu's Reset view");
      else
        change(it, 1, 1);
      g_m.dirty = 1;
    }
    if (pressed & PAD_CIRCLE)
      g_m.open = 0, g_m.dirty = 1, LOG("menu: closed");
  }
  /* The game sees nothing of it, nor of a press that closed it: the buttons held as it closes
   * (circle, cross on Close, the shoulders and the touchpad) stay hidden until each is let go.
   * Before (2026-11-13, the user): the circle that closed the menu reached the game as a press of
   * its own (the close was decided before the sample was hidden, and the game had seen the button
   * up all the while the menu was open). */
  if (g_m.open || g_m.shoulder_touch || hidden_before) {
    g_m.swallow = b & ~PAD_INTERCEPTED;
    *(u32 *)d = b & PAD_INTERCEPTED;
    d[4] = d[5] = d[6] = d[7] = 0x80; /* sticks centred */
    d[8] = d[9] = 0;                  /* triggers */
  } else if (g_m.swallow) {
    g_m.swallow &= b;
    *(u32 *)d = b & ~g_m.swallow;
  }
}

/* ---- the settings file (menu-<game>.txt, through the payload: CMD_MENU; both builds) ---- */
static char g_file[512]; /* as fetched (the payload keeps it to this) */

/* At the end of cvars_fetch (hooks.c), the agent's options as cvars.txt left them: those are
 * the defaults; then the file's lines over them (applied here; agent.res the payload read itself
 * before the boot, its log says what it did). */
/* the line of the option o */
static u32 item_of(volatile u32 *o) {
  u32 i = 0;
  while (i < N_ITEMS && k_items[i].opt != o) i++;
  return i;
}

void menu_boot(void) {
  g_next_hands = g_opt_hands;
  g_next_touch = g_opt_touch;
  g_next_gaze = (u32)gaze_follows();
  g_tv = g_opt_tvblank ? 2 : g_opt_mirrorboth ? 1 : 0;
  for (u32 i = 0; i < N_ITEMS; i++)
    if (k_items[i].kind == M_OPT) {
      g_m.has_start[i] = 1;
      g_m.start[i] = (int)*k_items[i].opt;
    }
  if (agent_call(CMD_MENU, (u64)g_file, sizeof g_file, 0, 0, 3000, 0, 0) != 0)
    g_file[0] = 0; /* (a payload without the command: no file) */
  g_file[sizeof g_file - 1] = 0;
  u32 n = 0, mb = 0, tb = 0, tv_line = 0, res_line = 0, legacy = 0;
  int legacy_w = 0;
  for (const char *p = g_file; *p;) {
    const char *line = p, *eq;
    while (*p && *p != '\n') p++;
    u32 len = (u32)(p - line);
    if (*p) p++;
    for (eq = line; eq < line + len && *eq != '='; eq++)
      ;
    if (eq >= line + len)
      continue;
    int neg = eq[1] == '-', v = 0;
    for (const char *d = eq + 1 + neg; d < line + len && *d >= '0' && *d <= '9' && v < 100000000; d++)
      v = v * 10 + (*d - '0'); /* (the digits stop short of overflowing: a hand-edited file) */
    if (neg)
      v = -v;
    u32 klen = (u32)(eq - line), i = 0;
    if (klen == 16 && memcmp(line, "agent.mirrorboth", 16) == 0) {
      mb = (u32)v, tv_line = 1;
      continue;
    }
    if (klen == 13 && memcmp(line, "agent.tvblank", 13) == 0) {
      tb = (u32)v, tv_line = 1;
      continue;
    }
    if (klen == 16 && memcmp(line, "agent.resolution", 16) == 0) { /* an older menu's eye width */
      legacy = 1, legacy_w = v;
      continue;
    }
    if (klen == 9 && memcmp(line, "agent.res", 9) == 0)
      res_line = 1;
    for (; i < N_ITEMS; i++)
      if (k_items[i].key && strlen(k_items[i].key) == klen && memcmp(k_items[i].key, line, klen) == 0)
        break;
    if (i == N_ITEMS || k_items[i].opt == &g_tv) {
      LOG("menu: the file's line '%.*s' is not one of the menu's", (int)len, line);
      continue;
    }
    g_m.has[i] = 1, g_m.set[i] = v, n++;
    if (k_items[i].opt == &g_next_hands)
      g_opt_hands = (u32)v, LOG("menu: agent.hands = %d (the file)", v);
    if (k_items[i].kind == M_OPT)
      *k_items[i].opt = (u32)g_m.set[i];
  }
  if (legacy) { /* written again as agent.res (the payload read it as this preset: res_legacy) */
    u32 i = 0;
    while (k_items[i].opt != &g_next_res) i++;
    if (!res_line)
      g_m.has[i] = 1, g_m.set[i] = (int)res_legacy(legacy_w), g_next_res = (u32)g_m.set[i], n++;
    LOG("menu: the file's agent.resolution=%d (the eye width, before the Resolution presets)%s %s", legacy_w,
        res_line ? ": left for its agent.res," : ": read as", k_res[g_next_res <= RES_LAST ? g_next_res : 0]);
    __atomic_store_n(&g_m.file_dirty, 1, __ATOMIC_RELEASE); /* (the file again, without the old line) */
  }
  { /* the start-only lines the agent takes here: the file's choice for this start */
    u32 t = item_of(&g_next_touch), z = item_of(&g_next_gaze);
    if (t < N_ITEMS && g_m.has[t]) {
      g_opt_touch = g_opt_touchhead = g_next_touch = g_next_touch != 0;
      LOG("menu: agent.touch = %u, agent.touchhead = %u (the file)", g_opt_touch, g_opt_touchhead);
    }
    if (z < N_ITEMS && g_m.has[z])
      gaze_set_follow(g_next_gaze = g_next_gaze != 0, "the menu's file"); /* (before fov.c's start) */
    LOG("menu: this start: Touch %s (head %s), Look at you %s, Eye-tracked sharp area %s, Sharpening %u",
        g_opt_touch ? "on" : "off", g_opt_touchhead ? "on" : "off", g_opt_lookat ? "on" : "off",
        gaze_follows() ? "on" : "off", g_opt_sharpen);
  }
  res_follow_fps(); /* (the default as the payload took it: by this start's frame rate) */
  {
    u32 i = 0;
    while (k_items[i].opt != &g_next_res) i++;
    if (g_m.has[i] && g_m.set[i] == g_m.start[i])
      g_m.has[i] = 0; /* (an old line at the default: dropped from the file) */
  }
  if (tv_line) {
    u32 i = 0;
    while (k_items[i].opt != &g_tv) i++;
    g_tv = tb ? 2 : mb ? 1 : 0;
    g_m.has[i] = 1, g_m.set[i] = (int)g_tv, n++;
    g_opt_mirrorboth = g_tv == 1, g_opt_tvblank = g_tv == 2;
    LOG("menu: the TV %s (the file)", k_tv[g_tv]);
  }
  if (g_file[0])
    LOG("menu: %u settings from the game's menu file", n);
}

/* The cvars thread, every 2 s: the file written when a line changed since (within 2 s of the
 * player's presses, not every press): a line for each setting that isn't its default, none
 * at all (the file removed) when there is none. A payload without the command: said once. */
void menu_save(void) {
  static char text[512];
  static u32 said;
  if (!__atomic_exchange_n(&g_m.file_dirty, 0, __ATOMIC_ACQ_REL))
    return;
  u32 o = 0;
  for (u32 i = 0; i < N_ITEMS; i++) {
    const struct item *it = &k_items[i];
    if (!it->key || !g_m.has[i] || (g_m.has_start[i] && g_m.set[i] == g_m.start[i]))
      continue;
    if (it->opt == &g_tv)
      o += (u32)agent_snprintf(text + o, sizeof text - o, "agent.mirrorboth=%u\nagent.tvblank=%u\n",
                               (u32)(g_m.set[i] == 1), (u32)(g_m.set[i] == 2));
    else
      o += (u32)agent_snprintf(text + o, sizeof text - o, "%s=%d\n", it->key, g_m.set[i]);
  }
  if (o >= sizeof text)
    o = sizeof text - 1;
  int rc = agent_call(CMD_MENU, (u64)text, o, 1, 0, 2000, 0, 0);
  if (rc == 0)
    LOG("menu: %s", o ? "the settings saved for the next start" : "no settings: the file removed");
  else if (!said++)
    LOG("menu: the payload did not save the settings (an older payload?)");
}

/* ---- the picture ---- */
static void fill(u8 *p, u32 x0, u32 y0, u32 w, u32 h, u32 rgba) {
  for (u32 y = y0; y < y0 + h && y < MENU_LH; y++)
    for (u32 x = x0; x < x0 + w && x < MENU_LW; x++)
      memcpy(p + (y * MENU_LW + x) * 4, &rgba, 4);
}
static void text(u8 *p, u32 x, u32 y, const char *s, u32 rgba) {
  for (; *s && x + 8 <= MENU_LW; s++, x += 8) {
    u32 c = (u8)*s;
    if (c < 32 || c > 126)
      c = '?';
    const u8 *g = k_font8x12[c - 32];
    for (u32 r = 0; r < 12; r++)
      for (u32 i = 0; i < 8; i++)
        if (g[r] & (0x80u >> i))
          memcpy(p + ((y + r) * MENU_LW + x + i) * 4, &rgba, 4);
  }
}
static void fmt_int(char *o, int v) {
  char t[12];
  int n = 0, neg = v < 0;
  u32 u = neg ? (u32)-v : (u32)v;
  do
    t[n++] = (char)('0' + u % 10), u /= 10;
  while (u);
  if (neg)
    *o++ = '-';
  while (n)
    *o++ = t[--n];
  *o = 0;
}
#define RGBA(r, g, b, a) ((u32)(r) | (u32)(g) << 8 | (u32)(b) << 16 | (u32)(a) << 24)
static void draw(u8 *p) {
  fill(p, 0, 0, MENU_LW, MENU_LH, RGBA(16, 18, 24, 215));
  fill(p, 0, 0, MENU_LW, 2, RGBA(255, 210, 60, 255));
  fill(p, 0, MENU_LH - 2, MENU_LW, 2, RGBA(255, 210, 60, 255));
#ifdef RELEASE
  text(p, 6, 3, "SETTINGS  L1+R1+pad", RGBA(255, 210, 60, 255));
#else
  text(p, 6, 3, "DEBUG MENU  L1+R1+pad", RGBA(255, 210, 60, 255));
#endif
  text(p, 14, MENU_LH - 16, "* from the next start", RGBA(180, 180, 180, 255));
  /* as many lines as fit above that note (11), scrolled to keep the chosen one in view */
  u32 rows = (MENU_LH - 20 - 14) / 11, first = 0;
  if (g_m.sel >= rows / 2)
    first = g_m.sel - rows / 2;
  if (first + rows > N_ITEMS)
    first = N_ITEMS > rows ? N_ITEMS - rows : 0;
  if (first)
    text(p, MENU_LW - 14, 3, "^", RGBA(255, 210, 60, 255));
  if (first + rows < N_ITEMS)
    text(p, MENU_LW - 14, MENU_LH - 14, "v", RGBA(255, 210, 60, 255));
  for (u32 i = first; i < N_ITEMS && i < first + rows; i++) {
    const struct item *it = &k_items[i];
    u32 y = 18 + (i - first) * 11, sel = i == g_m.sel;
    u32 col = sel ? RGBA(255, 230, 90, 255) : RGBA(235, 235, 235, 255);
    if (sel)
      fill(p, 2, y - 1, MENU_LW - 4, 12, RGBA(70, 60, 20, 230));
    text(p, 4, y, sel ? ">" : " ", col);
    text(p, 14, y, it->label, col);
    int v, r;
    char val[24];
    if (it->kind == M_CLOSE || it->kind == M_RESET || it->kind == M_VIEW)
      val[0] = 0;
    else if (!(r = value_of(it, &v)))
      memcpy(val, "-", 2);
    else if (r == 2)
      fmt_int(val, v); /* (a value off the list) */
    else if (it->names && v >= it->lo && v <= it->hi)
      memcpy(val, it->names[v - it->lo], strlen(it->names[v - it->lo]) + 1);
    else
      fmt_int(val, v);
    u32 len = (u32)strlen(val);
    text(p, MENU_LW - 6 - len * 8, y, val, col);
  }
}

/* the picture to copy while the menu is open (vr2.c each frame), else 0: drawn again when
 * something changed, and every half second (an option changed elsewhere) */
static const u8 *menu_picture_locked(void);
const u8 *menu_picture(void) {
  if (!g_m.open)
    return 0;
  /* one thread at a time: out of VR with PS VR2 started both the VR thread (the flat screen in the
   * headset, vr2.c) and the TV's (hooks.c flat_menu_thread) ask; together they made the two
   * pictures twice (the first lost: 3 MB) and drew into one at once. The other gets the picture as
   * it is (none before the first is made). */
  static volatile u32 busy;
  if (__atomic_exchange_n(&busy, 1, __ATOMIC_ACQUIRE))
    return g_m.made ? g_m.px[g_m.front] : 0;
  const u8 *pic = menu_picture_locked();
  __atomic_store_n(&busy, 0, __ATOMIC_RELEASE);
  return pic;
}
static const u8 *menu_picture_locked(void) {
  if (!g_m.px[0]) {
    u8 *m = vr2_dalloc(2 * MENU_W * MENU_H * 4, 0x10000);
    if (!m) {
      if (!g_m.said++)
        LOG("menu: no memory for its picture");
      g_m.open = 0;
      return 0;
    }
    g_m.px[0] = m, g_m.px[1] = m + MENU_W * MENU_H * 4;
    g_m.dirty = 1;
  }
  u64 now = sceKernelGetProcessTime();
  if (g_m.dirty || now - g_m.drawn_at > 500000) {
    g_m.dirty = 0, g_m.drawn_at = now;
    u32 back = !g_m.front;
    static u8 small[MENU_LW * MENU_LH * 4];
    draw(small);
    u32 *dst = (u32 *)g_m.px[back];
    const u32 *src = (const u32 *)small;
    for (u32 y = 0; y < MENU_H; y++) /* (enlarged, each pixel MENU_SCALE x MENU_SCALE) */
      for (u32 x = 0; x < MENU_W; x++)
        dst[y * MENU_W + x] = src[(y / MENU_SCALE) * MENU_LW + x / MENU_SCALE];
    __atomic_thread_fence(__ATOMIC_SEQ_CST); /* the picture written before it is the front */
    g_m.front = back;
    g_m.made = 1; /* (before it the other thread gets none: the memory is not a picture yet) */
  }
  return g_m.px[g_m.front];
}
