/* agent/menu.c on the host: the controller's presses through menu_pad, the picture as a PNG.
 *   cc -O2 -I../../agent -o /tmp/menutest menu_test.c -lz && /tmp/menutest <out.png>
 *   cc -O2 -DRELEASE -I../../agent -o /tmp/menutest-r menu_test.c -lz && /tmp/menutest-r <out.png>
 * Checks: L1+R1+touchpad opens and closes it, the game sees nothing while it is open (and not
 * the touchpad held with both shoulders), up/down/left/right/cross change the lines' values.
 * Without RELEASE the debug menu; with it the settings menu: its lines in order, each line's
 * values, and the settings file's round trip (menu_boot, menu_save, Reset all) with the payload
 * played by a stub.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#undef memcpy
#undef memset
#define memcpy agent_memcpy_unused
#define memset agent_memset_unused
#define memcmp agent_memcmp_unused
#define strlen agent_strlen_unused
#include "vr2.h"
#undef memcpy
#undef memset
#undef memcmp
#undef strlen
void agent_log(const char *fmt, ...) { va_list ap; va_start(ap, fmt); printf("  log: "); vprintf(fmt, ap); printf("\n"); va_end(ap); }
int agent_snprintf(char *out, size_t n, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = vsnprintf(out, n, fmt, ap); va_end(ap); return r < 0 ? 0 : r >= (int)n ? (int)n - 1 : r; }
/* the agent's options the menus' lines point at (hooks.c's), at their defaults */
u32 g_opt_tvhmd, g_opt_mirrorboth, g_opt_tvblank, g_opt_hands;
u32 g_opt_fanswings, g_opt_chars, g_opt_malfunction = 1;
u32 g_opt_ownerlv, g_opt_money, g_opt_special = 2, g_opt_starfans, g_opt_sensors, g_opt_portshop;
u32 g_opt_compose = 1, g_opt_composemax = 4096;
volatile u32 g_opt_realheight = 1, g_opt_sharpen = 50; /* (the defaults of height.c and compose.c) */
u32 g_opt_touch = 1, g_opt_touchhead = 1, g_opt_lookat = 1;
static u32 g_follow = 1;
void gaze_set_follow(u32 on, const char *from) { (void)from; g_follow = on != 0; }
int gaze_follows(void) { return (int)g_follow; }
static int g_resets;
void vr_reset_view(const char *why) { printf("  reset view (%s)\n", why); g_resets++; }
/* the payload: the settings file (CMD_MENU) kept in memory */
static char g_payload_file[512];
static int g_payload_has, g_payload_writes;
int agent_call(u32 cmd, u64 a0, u64 a1, u64 a2, u64 a3, u32 wait_ms, u32 k, u64 *out) {
  (void)a3, (void)wait_ms, (void)k, (void)out;
  if (cmd != CMD_MENU) return -1;
  if (a2 == 0) { if (g_payload_has) strncpy((char *)a0, g_payload_file, a1); else *(char *)a0 = 0; return 0; }
  g_payload_writes++;
  g_payload_has = a1 != 0;
  memset(g_payload_file, 0, sizeof g_payload_file);
  if (a1) memcpy(g_payload_file, (const char *)a0, a1 < sizeof g_payload_file ? a1 : sizeof g_payload_file - 1);
  return 0;
}
void *vr2_dalloc(size_t size, size_t align) { (void)align; return calloc(1, size); }
static u64 fake_time(void) { static u64 t; return t += 1000; }
u64 (*sceKernelGetProcessTime)(void) = fake_time;
#include "../../agent/menu.c"
static u8 pad[0x78];
static void press(u32 b) {
  memset(pad, 0, sizeof pad); *(u32 *)pad = b; pad[4] = 10; pad[8] = 99; menu_pad(pad);
  memset(pad, 0, sizeof pad); *(u32 *)pad = 0; menu_pad(pad);
}
static void go(const char *label) { /* down to the line with this label */
  for (u32 i = 0; i < 2 * N_ITEMS && strcmp(k_items[g_m.sel].label, label); i++) press(0x40);
}
static void png(const char *path, const u8 *rgba, int w, int h) {
  FILE *f = fopen(path, "wb"); if (!f) return;
  uLongf n = compressBound((uLong)(h * (w * 3 + 1)));
  u8 *raw = malloc((size_t)h * (w * 3 + 1)), *z = malloc(n);
  for (int y = 0; y < h; y++) { raw[y * (w * 3 + 1)] = 0;
    for (int x = 0; x < w; x++) { const u8 *p = rgba + (y * w + x) * 4; float a = p[3] / 255.0f;
      for (int c = 0; c < 3; c++) raw[y * (w * 3 + 1) + 1 + x * 3 + c] = (u8)(p[c] * a + 90 * (1 - a)); } }
  compress(z, &n, raw, (uLong)(h * (w * 3 + 1)));
  const u8 sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10}; fwrite(sig, 1, 8, f);
  #define BE(v) (u8)((v) >> 24), (u8)((v) >> 16), (u8)((v) >> 8), (u8)(v)
  u8 ihdr[13] = {BE((u32)w), BE((u32)h), 8, 2, 0, 0, 0};
  const char *t[3] = {"IHDR", "IDAT", "IEND"}; const u8 *d[3] = {ihdr, z, 0}; u32 l[3] = {13, (u32)n, 0};
  for (int i = 0; i < 3; i++) { u8 L[4] = {BE(l[i])}; fwrite(L, 1, 4, f); fwrite(t[i], 1, 4, f); if (l[i]) fwrite(d[i], 1, l[i], f);
    uLong c = crc32(0, (const u8 *)t[i], 4); if (l[i]) c = crc32(c, d[i], l[i]); u8 C[4] = {BE((u32)c)}; fwrite(C, 1, 4, f); }
  fclose(f);
}
static int bad;
#define CHECK(cond, ...) do { if (!(cond)) printf("FAIL: " __VA_ARGS__), printf("\n"), bad = 1; } while (0)
/* the common checks: the lines fit the picture, the opening, the game sees nothing while open */
static void common_start(void) {
  /* every line's label and its longest value within the picture's width with a gap: the label from
   * x 14, the value right-aligned to MENU_W - 6, 8 pixels a character (the user, worn: labels over
   * their values) */
  for (u32 i = 0; i < N_ITEMS; i++) {
    const struct item *it = &k_items[i];
    u32 v = 1;
    if (it->names)
      for (int k = it->lo; k <= it->hi; k++)
        v = strlen(it->names[k - it->lo]) > v ? (u32)strlen(it->names[k - it->lo]) : v;
    else {
      char b[16];
      u32 a = (u32)snprintf(b, sizeof b, "%d", it->lo), c = (u32)snprintf(b, sizeof b, "%d", it->hi);
      v = a > c ? a : c;
    }
    CHECK(14 + 8 * strlen(it->label) + 8 <= MENU_LW - 6 - 8 * v, "the line '%s' with its %u-character value is wider than the menu", it->label, v);
  }
  CHECK(!menu_picture(), "a picture while closed");
  /* the touchpad alone, or with one shoulder: not the menu, and the game sees it */
  memset(pad, 0, sizeof pad); *(u32 *)pad = 0x100000 | 0x400; menu_pad(pad);
  CHECK(!g_m.open && *(u32 *)pad == (0x100000 | 0x400), "L1+touchpad opened it or was hidden");
  press(0);
  /* both shoulders then the touchpad: open; the touchpad held with them hidden from the game */
  memset(pad, 0, sizeof pad); *(u32 *)pad = 0x0c00; menu_pad(pad);
  memset(pad, 0, sizeof pad); *(u32 *)pad = 0x0c00 | 0x100000; pad[4] = 10; menu_pad(pad);
  CHECK(g_m.open, "L1+R1+touchpad didn't open it");
  CHECK(*(u32 *)pad == 0 && pad[4] == 0x80, "the game saw the opening press or a stick");
  press(0);
}
static void common_end(const char *png_path, const char *picture_line) {
  /* the game sees nothing while open */
  memset(pad, 0, sizeof pad); *(u32 *)pad = 0x4000 | 0x80000000u; pad[6] = 3; pad[9] = 200; menu_pad(pad);
  CHECK(*(u32 *)pad == 0x80000000u && pad[6] == 0x80 && pad[9] == 0, "the game saw input while open");
  press(0);
  go(picture_line); /* for the picture */
  const u8 *p = menu_picture();
  CHECK(p, "no picture while open");
  if (p && png_path) png(png_path, p, MENU_W, MENU_H);
  press(0x2000); /* circle closes */
  CHECK(!g_m.open && !menu_picture(), "circle didn't close it");
}

/* the value shown for a line, as draw() makes it */
static const char *shown(const char *label) {
  static char val[24];
  u32 i = 0;
  while (strcmp(k_items[i].label, label)) i++;
  const struct item *it = &k_items[i];
  int v, r = value_of(it, &v);
  if (!r) return "-";
  if (r == 2) snprintf(val, sizeof val, "%d", v);
  else if (it->names) return it->names[v - it->lo];
  else snprintf(val, sizeof val, "%d", v);
  return val;
}
#ifdef RELEASE
static void open_menu(void) { /* both shoulders, then the touchpad */
  memset(pad, 0, sizeof pad); *(u32 *)pad = 0x0c00; menu_pad(pad);
  memset(pad, 0, sizeof pad); *(u32 *)pad = 0x0c00 | 0x100000; menu_pad(pad);
  press(0);
}
static void fresh_start(void) { /* the agent as it starts: the menu's state and the options at their defaults */
  memset(&g_m, 0, sizeof g_m);
  g_opt_hands = 0, g_opt_mirrorboth = g_opt_tvblank = 0, g_opt_fanswings = 0, g_opt_malfunction = 1;
  g_opt_realheight = 1, g_opt_touch = g_opt_touchhead = 1, g_opt_lookat = 1, g_follow = 1, g_opt_sharpen = 50;
  g_next_res = RES_DEF(1), g_next_hands = 0, g_tv = 0, g_next_fps = 1, g_next_fade = 0, g_next_scale = 100, g_res_cvar = 0;
}
int main(int argc, char **argv) {
  static const char *const k_order[] = {"Resolution*", "Hand tracking*", "120 fps*", "Fade inside her*", "World scale*",
                                        "Real height", "Touch*", "Look at you", "Eye-tracked sharp area*", "Sharpening",
                                        "Fan shake", "Malfunction, all suits", "TV picture", "Reset view", "Reset all", "Close"};
  CHECK(N_ITEMS == 16, "%u lines, not 16", (u32)N_ITEMS);
  for (u32 i = 0; i < N_ITEMS && i < 16; i++)
    CHECK(!strcmp(k_items[i].label, k_order[i]), "line %u is '%s', not '%s'", i, k_items[i].label, k_order[i]);
  /* the start without a file: the defaults */
  fresh_start();
  menu_boot();
  CHECK(!g_payload_writes && g_next_res == 5 && g_next_hands == 0 && g_tv == 0, "the start without a file");
  common_start();
  CHECK(!strcmp(shown("Resolution*"), "sharp+ (120 fps)") && !strcmp(shown("Hand tracking*"), "off") &&
        !strcmp(shown("World scale*"), "1.0 (true size)") && !strcmp(shown("Real height"), "on") &&
        !strcmp(shown("Fan shake"), "the game's (2)") && !strcmp(shown("Malfunction, all suits"), "on") &&
        !strcmp(shown("TV picture"), "left eye") && !strcmp(shown("Touch*"), "on") && !strcmp(shown("Look at you"), "on") &&
        !strcmp(shown("Eye-tracked sharp area*"), "on") && !strcmp(shown("Sharpening"), "50"),
        "the defaults shown: %s, %s, %s, %s, %s, touch %s, look %s, eyes %s, sharpening %s", shown("Resolution*"),
        shown("Hand tracking*"), shown("Fan shake"), shown("Malfunction, all suits"), shown("TV picture"), shown("Touch*"),
        shown("Look at you"), shown("Eye-tracked sharp area*"), shown("Sharpening"));
  /* the resolution presets: held at both ends */
  go("Resolution*");
  press(0x80);
  CHECK(g_next_res == 4 && !strcmp(shown("Resolution*"), "2400 (60 fps)"), "resolution %u one step down from 5, not 4", g_next_res);
  for (int i = 0; i < 4; i++) press(0x80);
  CHECK(g_next_res == 0 && !strcmp(shown("Resolution*"), "game's own"), "resolution %u five steps down, not 0", g_next_res);
  press(0x80);
  CHECK(g_next_res == 0, "resolution %u, not held at the game's own", g_next_res);
  for (int i = 0; i < 8; i++) press(0x20);
  CHECK(g_next_res == 6 && !strcmp(shown("Resolution*"), "2720 (60 fps)"), "resolution %u, not held at 6", g_next_res);
  press(0x80), press(0x80), press(0x80), press(0x80);
  CHECK(g_next_res == 2, "resolution %u, not 2", g_next_res);
  /* hand tracking: for the next start (this start's option is left) */
  go("Hand tracking*");
  press(0x20);
  CHECK(g_next_hands == 60 && g_opt_hands == 0, "hand tracking: next start %u (not 60), this start %u (not 0)", g_next_hands, g_opt_hands);
  /* the live lines */
  go("Fan shake");
  press(0x20);
  CHECK(g_opt_fanswings == 1, "fan shake %u, not 1 (easy)", g_opt_fanswings);
  go("Malfunction, all suits");
  press(0x80);
  CHECK(g_opt_malfunction == 0, "malfunction %u, not 0", g_opt_malfunction);
  /* the four lines moved from the debug menu (2026-11-14): Touch* and Eye-tracked sharp area* for the
   * next start (this start's options left), Look at you and Sharpening live */
  go("Touch*");
  press(0x80);
  CHECK(g_next_touch == 0 && g_opt_touch == 1 && g_opt_touchhead == 1, "touch: next start %u (not 0), this start %u %u",
        g_next_touch, g_opt_touch, g_opt_touchhead);
  go("Look at you");
  press(0x4000);
  CHECK(g_opt_lookat == 0, "look at you %u after cross, not 0", g_opt_lookat);
  go("Eye-tracked sharp area*");
  press(0x80);
  CHECK(g_next_gaze == 0 && g_follow == 1, "eye-tracked sharp area: next start %u (not 0), this start %u", g_next_gaze, g_follow);
  go("Sharpening");
  press(0x80);
  CHECK(g_opt_sharpen == 0 && !strcmp(shown("Sharpening"), "off"), "sharpening %u one step down from 50, not off", g_opt_sharpen);
  press(0x20), press(0x20);
  CHECK(g_opt_sharpen == 100 && !strcmp(shown("Sharpening"), "100"), "sharpening %u, not 100", g_opt_sharpen);
  press(0x20);
  CHECK(g_opt_sharpen == 100, "sharpening %u, not held at 100", g_opt_sharpen);
  go("TV picture");
  press(0x20);
  CHECK(g_opt_mirrorboth == 1 && g_opt_tvblank == 0, "TV both eyes: mirrorboth %u, tvblank %u", g_opt_mirrorboth, g_opt_tvblank);
  press(0x20);
  CHECK(g_opt_mirrorboth == 0 && g_opt_tvblank == 1, "TV black: mirrorboth %u, tvblank %u", g_opt_mirrorboth, g_opt_tvblank);
  /* the file: a line for each setting off its default, written once */
  menu_save();
  const char *want = "agent.res=2\nagent.hands=60\nagent.touch=0\nagent.lookat=0\nagent.gazefollow=0\n"
                     "agent.sharpen=100\nagent.fanswings=1\nagent.malfunction=0\nagent.mirrorboth=0\nagent.tvblank=1\n";
  CHECK(g_payload_writes == 1 && !strcmp(g_payload_file, want), "the file after the changes (%d writes):\n%s", g_payload_writes, g_payload_file);
  menu_save();
  CHECK(g_payload_writes == 1, "the file written again with nothing changed");
  /* the next start: the file read back, its lines over the defaults */
  fresh_start();
  menu_boot();
  CHECK(g_opt_touch == 0 && g_opt_touchhead == 0 && g_next_touch == 0 && g_opt_lookat == 0 && g_follow == 0 &&
        g_next_gaze == 0 && g_opt_sharpen == 100,
        "the moved lines read back: touch %u %u (next %u), look %u, eyes %u (next %u), sharpening %u", g_opt_touch,
        g_opt_touchhead, g_next_touch, g_opt_lookat, g_follow, g_next_gaze, g_opt_sharpen);
  CHECK(g_next_res == 2 && g_opt_hands == 60 && g_next_hands == 60 && g_opt_fanswings == 1 && g_opt_malfunction == 0 &&
        g_tv == 2 && g_opt_tvblank == 1 && g_opt_mirrorboth == 0,
        "the file read back: resolution %u, hands %u (next %u), fan %u, malfunction %u, TV %u (tvblank %u, mirrorboth %u)", g_next_res,
        g_opt_hands, g_next_hands, g_opt_fanswings, g_opt_malfunction, g_tv, g_opt_tvblank, g_opt_mirrorboth);
  open_menu();
  CHECK(g_m.open && !strcmp(shown("Resolution*"), "wide (60 fps)") && !strcmp(shown("Hand tracking*"), "on") && !strcmp(shown("TV picture"), "black"),
        "the menu after the restart: resolution %s, hands %s, TV %s", shown("Resolution*"), shown("Hand tracking*"), shown("TV picture"));
  /* malfunction back on (its default): its line leaves the file */
  go("Malfunction, all suits");
  press(0x20);
  menu_save();
  want = "agent.res=2\nagent.hands=60\nagent.touch=0\nagent.lookat=0\nagent.gazefollow=0\nagent.sharpen=100\n"
         "agent.fanswings=1\nagent.mirrorboth=0\nagent.tvblank=1\n";
  CHECK(g_opt_malfunction == 1 && g_payload_writes == 2 && !strcmp(g_payload_file, want), "the file after malfunction went back to its default:\n%s", g_payload_file);
  /* Reset view */
  go("Reset view");
  press(0x4000);
  CHECK(g_resets == 1 && g_m.open, "Reset view: %d resets", g_resets);
  /* Reset all: every line at its default, the file removed (this start's hand tracking stays) */
  go("Reset all");
  press(0x4000);
  menu_save();
  CHECK(g_next_touch == 1 && g_opt_lookat == 1 && g_next_gaze == 1 && g_opt_sharpen == 50,
        "Reset all: touch next %u, look %u, eyes next %u, sharpening %u", g_next_touch, g_opt_lookat, g_next_gaze, g_opt_sharpen);
  CHECK(g_next_res == 5 && g_next_hands == 0 && g_opt_hands == 60 && g_opt_fanswings == 0 && g_opt_malfunction == 1 &&
        g_tv == 0 && g_opt_tvblank == 0 && g_opt_mirrorboth == 0 && !g_payload_has,
        "Reset all: resolution %u, next hands %u (now %u), fan %u, malfunction %u, TV %u, the file %s", g_next_res, g_next_hands,
        g_opt_hands, g_opt_fanswings, g_opt_malfunction, g_tv, g_payload_has ? "still there" : "removed");
  /* cvars.txt's own settings are the defaults: agent.hands=30 there shows as 30 (off the list) and
   * steps onto the list; an older file's eye width (agent.resolution) read as a preset and written
   * again as agent.res; a line not the menu's is left */
  fresh_start();
  g_opt_hands = 30;
  g_payload_has = 1, g_payload_writes = 0;
  strcpy(g_payload_file, "agent.resolution=1200\nnot.a.line=1\n");
  menu_boot();
  open_menu();
  CHECK(!strcmp(shown("Hand tracking*"), "30") && g_next_res == 1, "hands %s, the older 1200 read as %u, not 1 (sharp)",
        shown("Hand tracking*"), g_next_res);
  menu_save();
  CHECK(g_payload_writes == 1 && !strcmp(g_payload_file, "agent.res=1\n"), "the older line not written again as agent.res (%d writes):\n%s", g_payload_writes, g_payload_file);
  go("Hand tracking*");
  press(0x20);
  CHECK(g_next_hands == 60, "hand tracking from 30 went right to %u, not 60", g_next_hands);
  menu_save();
  CHECK(!strcmp(g_payload_file, "agent.res=1\nagent.hands=60\n"), "the file over cvars.txt's own:\n%s", g_payload_file);
  /* an older 2048 at 60 fps (the file's agent.fps120=0): preset 3, the default there, so dropped; 0 kept as 0 */
  fresh_start();
  g_payload_has = 1;
  strcpy(g_payload_file, "agent.fps120=0\nagent.resolution=2048\n");
  menu_boot();
  CHECK(g_next_fps == 0 && g_next_res == 3, "60 fps and an older 2048: fps %u, resolution %u (not 3)", g_next_fps, g_next_res);
  menu_save();
  CHECK(!strcmp(g_payload_file, "agent.fps120=0\n"), "the older 2048 at 60 fps:\n%s", g_payload_file);
  fresh_start();
  g_payload_has = 1;
  strcpy(g_payload_file, "agent.resolution=0\n");
  menu_boot();
  menu_save();
  CHECK(g_next_res == 0 && !strcmp(g_payload_file, "agent.res=0\n"), "an older game's own: %u,\n%s", g_next_res, g_payload_file);
  /* the default follows the 120 fps line while not chosen: 120 fps off, Resolution 3 (not saved) */
  fresh_start();
  g_payload_has = 0;
  menu_boot();
  open_menu();
  go("120 fps*");
  press(0x80);
  menu_save();
  CHECK(g_next_fps == 0 && g_next_res == 3 && !strcmp(g_payload_file, "agent.fps120=0\n"), "120 fps off: resolution %u,\n%s",
        g_next_res, g_payload_file);
  press(0x20);
  CHECK(g_next_res == 5, "120 fps back on: resolution %u, not 5", g_next_res);
  common_end(argc > 1 ? argv[1] : 0, "TV picture");
  printf(bad ? "FAIL\n" : "ok\n");
  return bad;
}
#else
int main(int argc, char **argv) {
  menu_boot(); /* (no file: every line's start value, for Reset all) */
  CHECK(!strcmp(k_items[0].label, "Resolution*") && g_next_res == 5, "the debug menu's first line '%s' (%u)", k_items[0].label, g_next_res);
  common_start();
  /* a numbered line with names: right steps it, held at its top; cross wraps */
  go("Fan shake");
  press(0x20);
  CHECK(g_opt_fanswings == 1, "fan swings %u after one step, not 1", g_opt_fanswings);
  for (int i = 0; i < 6; i++) press(0x20);
  CHECK(g_opt_fanswings == 4 && !strcmp(shown("Fan shake"), "4"), "fan swings %u, not held at 4", g_opt_fanswings);
  press(0x4000);
  CHECK(g_opt_fanswings == 0, "fan swings %u after cross at the top, not wrapped to 0", g_opt_fanswings);
  press(0x20);
  /* lines with a step */
  go("Owner Level min");
  press(0x20), press(0x20);
  CHECK(g_opt_ownerlv == 20, "owner level %u after two steps, not 20", g_opt_ownerlv);
  go("Zack Dollars min");
  press(0x20);
  CHECK(g_opt_money == 250000, "Zack Dollars %u after one step, not 250000", g_opt_money);
  go("Special tab");
  CHECK(!strcmp(shown("Special tab"), "everything"), "the Special tab's default shown as %s", shown("Special tab"));
  press(0x80);
  CHECK(g_opt_special == 1 && !strcmp(shown("Special tab"), "weekly"), "the Special tab %u (%s), not 1 (weekly)", g_opt_special, shown("Special tab"));
  go("Compose width max");
  press(0x80);
  CHECK(g_opt_composemax == 3840, "compose width %u one step down from 4096, not 3840", g_opt_composemax);
  for (int i = 0; i < 20; i++) press(0x80);
  CHECK(g_opt_composemax == 1024, "compose width %u, not held at 1024", g_opt_composemax);
  go("TV: headset view");
  press(0x4000);
  CHECK(g_opt_tvhmd == 1, "TV: headset view %u after cross, not 1", g_opt_tvhmd);
  /* the debug menu's cross on a value line steps it: no view reset so far; Reset view's does */
  CHECK(g_resets == 0, "a view reset from a value line");
  go("Reset view");
  press(0x4000);
  CHECK(g_resets == 1, "Reset view: %d resets", g_resets);
  /* Reset all (the line before Close): all back to their values when the menu was opened */
  {
    u32 to_reset = (u32)(N_ITEMS - 2);
    while (g_m.sel != to_reset) press(0x10);
    press(0x4000);
    CHECK(g_opt_fanswings == 0 && g_opt_ownerlv == 0 && g_opt_money == 0 && g_opt_special == 2 && g_opt_composemax == 4096 &&
          g_opt_tvhmd == 0 && g_opt_malfunction == 1 && g_opt_compose == 1,
          "reset gave fan swings %u, owner level %u, Zack Dollars %u, Special tab %u, compose width %u, TV headset %u",
          g_opt_fanswings, g_opt_ownerlv, g_opt_money, g_opt_special, g_opt_composemax, g_opt_tvhmd);
    if (!bad)
      printf("  reset: every line at its start value again\n");
  }
  common_end(argc > 1 ? argv[1] : 0, "Special tab");
  printf(bad ? "FAIL\n" : "ok\n");
  return bad;
}
#endif
