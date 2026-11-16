/* Four options, all on the game's main thread at the start of its loop (hooks.c tick_frame, before
 * the update): agent.lookat (on by default, both builds: the settings menu's "Look at you", live), agent.vrtimers (on by default; both builds; it acts only
 * at 120 fps), agent.uihold (debug, off) and agent.rateprobe (debug, off); the last two are
 * described at their code below (logs/research/fps/TIMERS-120.md, TIMERS-120-FIX.md).
 *
 * agent.lookat=1: the VR Paradise "look at the player" reaction its code was written to make
 * (approved by the owner; logs/research/touch/DATA.md section 3). The scene manager (coroutine
 * slot 6: Fortune 0x552b10, Scarlet 0x552c10; state *0xbe24b8 / *0xbea598: +0x10 girl index, +0x14 level,
 * +0x18 count, +0x20 timer) counts close contacts each tick (Fortune 0x553f10, Scarlet 0x554000):
 * while the level is 1 or 2 and its random 150-250 tick timer has run out, the head within 9 cm of
 * a body sphere (S+0x121) or the controller touching (S+0x81) adds 1 to the count, the level
 * becomes 1, 2 (count 5..9) or 3 (10 on). The segment start (Fortune 0x556b90, Scarlet 0x556c80,
 * given state+0x10) ends with a gaze call that can never run: the same function sets level 1 and
 * count 0 just before it. Its code (Fortune 0x556d23..0x556e1f, Scarlet 0x556e13..0x556f0f):
 *   gaze = 0x8c8920 + i * 0x2b0 (Scarlet 0x8d0930), i the girl index, once her model is found in
 *     the model list (girls[i] at 0xa98080 + i * 0x7e0 (Scarlet 0xaa00e0), its +0x2c a slot j < 4,
 *     the list *0x942900 (*0x94a980) walked by +0x18 for the value 0xa9bf80[j] (0xaa3fe0[j]));
 *   gaze+0x2a0 = 1000;
 *   cam = 0x9529e0 + (s8)*0x96a3f5 * 0x220 + (s32)*0x9529d8 * 0x110 + 0x90
 *     (Scarlet 0x95aa70, 0x972485, 0x95aa68);
 *   if ((level & ~1) == 2) { if (gaze+0x278 <= 0) look(gaze, cam, 2, -1, 0, 0xffff); }
 *   else if (count != 0 && gaze+0x278 <= 0) look(gaze, cam, 2, 300, 1, 9);
 * with look = 0x34c120 (Scarlet 0x34c4d0): (object, target, priority, duration in ticks (-1: until
 * changed), a flag, mode; 0xffff: its own choice). Here, when the count has gone up since the last
 * loop (a contact counted: at most once every 2.5-4.2 s), that code is run as written, with the
 * level and count the escalation has just set. Nothing else is called or written. Logged (on the
 * cvars thread): each call with its arguments, each skip and why.
 *
 * agent.vrtimers=1: the game's frame-counted timers that a picture every refresh (frame_pace 1,
 * 120 fps) runs twice as fast. Read in both eboots: the VR scene function (Fortune
 * 0x46c7d0, Scarlet 0x46e580, coroutine slot 0x1b) and the VR Paradise manager (slot 6) are
 * coroutines the update resumes inside its tick loop (Scarlet 0x4f08c0..0x4f09bb, all 28 slots
 * each tick; Fortune 0x4eef10..0x4ef00b), skipped whole on a loop with no tick (0x4f0815 /
 * 0x4eee65 jle): their timers (the touch rumble 180/24, the post-touch 36, the stroke window 60,
 * the escalation's 150-250, the head-near fade's 8) already count ticks, 60 a second. Not so four
 * counters in the update's part after the tick loop, which runs once a loop and is gated neither by
 * the ticks nor by the game speed (so agent.tickhold does not hold them either); each steps by 1
 * a loop and drives something drawn:
 *   1. the light set crossfade: of the two sets at 0xbd6890 + k * 0x120 (Fortune 0xbce7a0), k = 1
 *      when the second's +0x100 pointer (0xbd6ab0 / 0xbce9c0) is set, the countdown +0x11c over
 *      its total +0x118 blends the scene's lighting parameters (0x51f070) toward the new set
 *      (Scarlet 0x4f7b93..0x4f7e67, Fortune 0x4f61e3..0x4f64b7);
 *   2. an overlay fade in, hold, fade out: state 0xbd6bf0 (1, 2, 3), its count 0xbd6bf4, the
 *      lengths at 0xbd6be0, the amount 0xbd6bf8 (read at 0x51fcf8 by the same lighting code)
 *      (Fortune 0xbceb00/04, 0xbceaf0, 0xbceb08; code 0x4f7e6a..0x4f7f65 / 0x4f64ba..0x4f65b5);
 *   3. a light fade: state 0xbe5780 (1 in, 3 out), count 0xbe5788 over 0xbe5784, the amount
 *      0xbe578c read by the scene's light setup (0x4bca13) (Fortune 0xbdd690/98/94/9c; code
 *      0x4f800d..0x4f80d9 / 0x4f665d..0x4f6729; run while the speed is not negative);
 *   4. a ramp: count 0xabe424 up to 0xabe420 while not paused, the value 0xabe430 = count * rate /
 *      a constant + start read by the render setup (0x4c4815) (Fortune 0xab6344/40, 0xab6350;
 *      code 0x4f9c18..0x4f9c5f / 0x4f8268..0x4f82af);
 *   5. the same object's envelope time 0xabe440 (Fortune 0xab6360) += 1.0 right after the ramp's
 *      step (0x4f9c5f / 0x4f82af), under the ramp's own condition (its two jumps skip both, to
 *      0x4f9c77 / 0x4f82c7: the ramp site's bytes); read by the render setup 0x4c4800 / 0x4c3b90
 *      (an amount t / in, then (total - t) / out). Put back when it rose by exactly 1.0.
 * (agent.vrtimers is on by default since 2026-11-14 and taken by a release build: at 60 fps no
 * loop is without a tick, so it does nothing there.)
 * Their lengths are given by their callers (no one constant to double), so instead: on a loop the
 * scheduler gives no tick, before the update, each that the update will step this loop is stepped
 * back by 1 (its own condition, read the same way), so the update's step leaves it as it was and
 * shows the same value again: each value lasts two refreshes, as at 60. Nothing else is written.
 * Logged every 10 s (cvars thread): how often each was held.
 * Not touched here: the per-loop countdowns of the update's block run only while the game speed is
 * above 0 (Scarlet 0x4f5901..0x4f79c1, Fortune 0x4f3f51..0x4f6011: five at 0xbd7234 + k * 0x270,
 * the u16 +0xaa/+0xca/+0x21a of the objects in *0xbd7c00, two per-entry countdowns at 0x4f77a2 /
 * 0x4f7869): agent.tickhold's speed 0 skips that block whole on a no-tick loop. The block at
 * 0x4f8a9c..0x4f8dee runs only when ticks * speed > 0: right already.
 *
 * Every site is compared with the bytes of both eboots (dumps/CUSA14173, dumps/CUSA04555 patch0)
 * before anything is called or written, the RIP-relative operands with this file's addresses; any
 * difference and that option stays off.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "vr2.h"

u32 g_opt_lookat = 1; /* hooks.c cvars; on by default (the settings menu's "Look at you", live) */
u32 g_opt_vrtimers = 1; /* (on by default: it acts only on loops with no tick, at 120 fps) */
u32 g_opt_uihold, g_opt_rateprobe; /* (debug, off) */

static u64 now_us(void) { return sceKernelGetProcessTime(); }
static int mem_ok(const void *p, u64 n, int write) {
  u8 info[128];
  memset(info, 0, sizeof info);
  if (!sceKernelVirtualQuery || sceKernelVirtualQuery(p, 0, info, 0x48) != 0)
    return 0;
  u64 start = *(u64 *)info, end = *(u64 *)(info + 8);
  s32 prot = *(s32 *)(info + 24);
  return start <= (u64)p && (u64)p + n <= end && (prot & (write ? 3 : 1)) == (write ? 3 : 1);
}

/* ---- the sites (Scarlet 1.08, Fortune 1.19, Scarlet 1.00) ---- */
#define RS_MAX 0x110 /* the longest site (the dead gaze branch, 0x101 bytes) fits */
struct rs_site {
  const char *what;
  u32 va[3];
  u16 len;
  const char *hex[3]; /* the bytes, as hex */
};
struct rs_link {
  u8 site;
  u16 disp, end; /* the operand's offset in the site, its instruction's end */
  u32 to[3];     /* where it must lead */
};

static const struct rs_site k_look_sites[] = {
    {"the segment start's girls base", {0x556cb0, 0x556bc0, 0x5551c0}, 7, {"4c8d3529945400", "4c8d35b9145400", "4c8d35d9ae5400"}},
    {"the girl's model looked up", {0x556dbc, 0x556ccc, 0x5552cc}, 0x57,
     {"8b034883f8040f835a0100004869c8e0070000428b54312c4883fa040f834b010000488b0d9b3b3f004885c90f84d0000000488d35ebd15400488b14d60f1f8000000000483911740e488b49184885c975f2e9ab000000",
      "8b034883f8040f835a0100004869c8e0070000428b54312c4883fa040f834b010000488b0d0bbc3e004885c90f84d0000000488d357b525400488b14d60f1f8000000000483911740e488b49184885c975f2e9ab000000",
      "8b034883f8040f835a0100004869c8e0070000428b54312c4883fa040f834b010000488b0d8b563f004885c90f84d0000000488d359bec5400488b14d60f1f8000000000483911740e488b49184885c975f2e9ab000000"}},
    {"the dead gaze branch", {0x556e13, 0x556d23, 0x555323}, 0x101,
     {"8b4b044869c0b0020000488d3d0c9b3700c78438a0020000e803000083e1fe83f90275648b034883f8040f83f40000004869c0b002000083bc3878020000007f6a480fbe1529b6410048630d053c4000488d35063c40004801c74531c041b9ffff00004869d2200200004869c9100100004801d6ba02000000488db43190000000b9ffffffffeb70837b0800741d8b034883f8040f838a0000004869c0b002000083bc3878020000007e055b415e5dc3480fbe15bab5410048630d963b4000488d35973b40004801c741b80100000041b9090000004869d2200200004869c9100100004801d6ba02000000488db43190000000b92c0100005b415e5de9bc55dfff",
      "8b4b044869c0b0020000488d3dec1b3700c78438a0020000e803000083e1fe83f90275648b034883f8040f83f40000004869c0b002000083bc3878020000007f6a480fbe158936410048630d65bc3f00488d3566bc3f004801c74531c041b9ffff00004869d2200200004869c9100100004801d6ba02000000488db43190000000b9ffffffffeb70837b0800741d8b034883f8040f838a0000004869c0b002000083bc3878020000007e055b415e5dc3480fbe151a36410048630df6bb3f00488d35f7bb3f004801c741b80100000041b9090000004869d2200200004869c9100100004801d6ba02000000488db43190000000b92c0100005b415e5de9fc52dfff",
      "8b4b044869c0b0020000488d3dfcb53700c78438a0020000e803000083e1fe83f90275648b034883f8040f83f40000004869c0b002000083bc3878020000007f6a480fbe1519d1410048630df5564000488d35f65640004801c74531c041b9ffff00004869d2200200004869c9100100004801d6ba02000000488db43190000000b9ffffffffeb70837b0800741d8b034883f8040f838a0000004869c0b002000083bc3878020000007e055b415e5dc3480fbe15aad0410048630d86564000488d35875640004801c741b80100000041b9090000004869d2200200004869c9100100004801d6ba02000000488db43190000000b92c0100005b415e5de95c6adfff"}},
    {"the escalation", {0x554000, 0x553f10, 0x552510}, 0xc2,
     {"418b5424148d72ff83fe027315418b44242085c07452ffc84189442420e95e02000083fa030f85550200004280bc31c80700000075244280bc31cd070000007519488d04c0488d0d047b5400817cc1142c0100000f8c2602000041c744241400000000e91802000041c744242400000000488b05983d56004885c00f84ff01000080b82101000000750d80b881000000000f84e9010000418b4c2418be03000000ffc183f90441894c24180f97c00fb6c0ffc083f9090f47c639c274054189442414",
      "418b5424148d72ff83fe027315418b44242085c07452ffc84189442420e95e02000083fa030f85550200004280bc31c80700000075244280bc31cd070000007519488d04c0488d0d94fb5300817cc1142c0100000f8c2602000041c744241400000000e91802000041c744242400000000488b05a8bd55004885c00f84ff01000080b82101000000750d80b881000000000f84e9010000418b4c2418be03000000ffc183f90441894c24180f97c00fb6c0ffc083f9090f47c639c274054189442414",
      "418b5424148d72ff83fe027315418b44242085c07452ffc84189442420e95e02000083fa030f85550200004280bc31c80700000075244280bc31cd070000007519488d04c0488d0db4955400817cc1142c0100000f8c2602000041c744241400000000e91802000041c744242400000000488b05485856004885c00f84ff01000080b82101000000750d80b881000000000f84e9010000418b4c2418be03000000ffc183f90441894c24180f97c00fb6c0ffc083f9090f47c639c274054189442414"}},
    {"the manager's state pointer", {0x553ed0, 0x553de0, 0x5523e0}, 7, {"4c8b25c1666900", "4c8b25d1e66800", "4c8b2571816900"}},
    {"the look-at's start (its priority test)", {0x34c4d0, 0x34c120, 0x34be80}, 0x37,
     {"554889e54157415641554154534881ec080100004c8b35fd3357004989ff4189cd4989f4498b06488945d04139170f8f040d0000418917",
      "554889e54157415641554154534881ec080100004c8b35c5b356004989ff4189cd4989f4498b06488945d04139170f8f040d0000418917",
      "554889e54157415641554154534881ec080100004c8b350d3a57004989ff4189cd4989f4498b06488945d04139170f8f040d0000418917"}},
};
enum { LK_GIRLS, LK_MODELS, LK_CHARS, LK_GAZE, LK_CAM_I8, LK_CAM_I32, LK_CAM, LK_LOOK, LK_STATE, LK_N };
static const struct rs_link k_look_links[LK_N] = {
    [LK_GIRLS] = {0, 3, 7, {0xaa00e0, 0xa98080, 0xaa00a0}},     [LK_MODELS] = {1, 37, 41, {0x94a980, 0x942900, 0x94a980}},
    [LK_CHARS] = {1, 53, 57, {0xaa3fe0, 0xa9bf80, 0xaa3fa0}},   [LK_GAZE] = {2, 13, 17, {0x8d0930, 0x8c8920, 0x8d0930}},
    [LK_CAM_I8] = {2, 69, 73, {0x972485, 0x96a3f5, 0x972485}},  [LK_CAM_I32] = {2, 76, 80, {0x95aa68, 0x9529d8, 0x95aa68}},
    [LK_CAM] = {2, 83, 87, {0x95aa70, 0x9529e0, 0x95aa70}},     [LK_LOOK] = {2, 253, 257, {0x34c4d0, 0x34c120, 0x34be80}},
    [LK_STATE] = {4, 3, 7, {0xbea598, 0xbe24b8, 0xbea558}},
};
/* (the second branch's operands, and the escalation's S pointer, lead to the same: checked too) */
static const struct rs_link k_look_links2[] = {
    {2, 180, 184, {0x972485, 0x96a3f5, 0x972485}}, {2, 187, 191, {0x95aa68, 0x9529d8, 0x95aa68}},
    {2, 194, 198, {0x95aa70, 0x9529e0, 0x95aa70}}, {3, 116, 120, {0xab7e10, 0xaafd30, 0xab7dd0}},
};

static const struct rs_site k_vt_sites[] = {
    {"the light set countdown's read", {0x4f7b93, 0x4f61e3, 0x4f60d3}, 0x39,
     {"48833d15ef6d00004c8d3deeec6d000f95c00fb6c0488d1cc048c1e305428b843b1c0100004d8d241f4e8db43b1c01000085c00f8e66010000",
      "48833dd5876d00004c8d3dae856d000f95c00fb6c0488d1cc048c1e305428b843b1c0100004d8d241f4e8db43b1c01000085c00f8e66010000",
      "48833d95096e00004c8d3d6e076e000f95c00fb6c0488d1cc048c1e305428b843b1c0100004d8d241f4e8db43b1c01000085c00f8e66010000"}},
    {"the light set countdown's step", {0x4f7e38, 0x4f6488, 0x4f6378}, 5, {"418b06ffc8", "418b06ffc8", "418b06ffc8"}},
    {"the overlay fade", {0x4f7e6a, 0x4f64ba, 0x4f63aa}, 0xfb,
     {"488b056fed6d00488b3578ed6d00bf01000000c5fa10157f272c000fb7c889c383f9010f46cfc1eb1048c1e820c5f857c0c5fa2ac14889f148c1e920eb1b0f1f84000000000089cf0fb7ca893535ed6d00f7df890d31ed6d0083fe01742883fe02754c29f9be0300000089c2890d18ed6d00c70512ed6d000000803feb2b0f1f84000000000029f9be0200000089dac5fa2ac9890df1ec6d00c5f25ec8c5ea5cc9c5fa110de5ec6d0085c97e99eb4c83fe03754729f90fb7c0ba01000000c5f857c0c5fa2ac183f801890dbbec6d000f47d085c9c5fa2acac5fa5ec1c5fa1105aaec6d007f1548c70595ec6d0000000000c70593ec6d0000000000",
      "488b052f866d00488b3538866d00bf01000000c5fa101593c92b000fb7c889c383f9010f46cfc1eb1048c1e820c5f857c0c5fa2ac14889f148c1e920eb1b0f1f84000000000089cf0fb7ca8935f5856d00f7df890df1856d0083fe01742883fe02754c29f9be0300000089c2890dd8856d00c705d2856d000000803feb2b0f1f84000000000029f9be0200000089dac5fa2ac9890db1856d00c5f25ec8c5ea5cc9c5fa110da5856d0085c97e99eb4c83fe03754729f90fb7c0ba01000000c5f857c0c5fa2ac183f801890d7b856d000f47d085c9c5fa2acac5fa5ec1c5fa11056a856d007f1548c70555856d0000000000c70553856d0000000000",
      "488b05ef076e00488b35f8076e00bf01000000c5fa1015ef282c000fb7c889c383f9010f46cfc1eb1048c1e820c5f857c0c5fa2ac14889f148c1e920eb1b0f1f84000000000089cf0fb7ca8935b5076e00f7df890db1076e0083fe01742883fe02754c29f9be0300000089c2890d98076e00c70592076e000000803feb2b0f1f84000000000029f9be0200000089dac5fa2ac9890d71076e00c5f25ec8c5ea5cc9c5fa110d65076e0085c97e99eb4c83fe03754729f90fb7c0ba01000000c5f857c0c5fa2ac183f801890d3b076e000f47d085c9c5fa2acac5fa5ec1c5fa11052a076e007f1548c70515076e0000000000c70513076e0000000000"}},
    {"the light fade", {0x4f800d, 0x4f665d, 0x4f654d}, 0xcc,
     {"c5fa10842490010000c5f82ec10f82b90000008b055ad76e0083f803752e8b0d57d76e008d41ff83f90289054bd76e007c65c5f828d0c5f857c0c5fa2ac0c5fa2a0d31d76e00c5fa5ec1eb3d83f801757b8b0d24d76e008d41ff83f902890518d76e007c48c5f828d0c5f857c0c5fa2ac0c5fa2a0dfed66e00c5fa5ec1c5fa100d72252c00c5f25cc0c5fa1105eed66e00c5f828c2eb35c705d2d66e0000000000c705d4d66e0000000000eb14c705bcd66e0002000000c705bed66e000000803f48c705abd66e0001000000",
      "c5fa10842490010000c5f82ec10f82b90000008b051a706e0083f803752e8b0d17706e008d41ff83f90289050b706e007c65c5f828d0c5f857c0c5fa2ac0c5fa2a0df16f6e00c5fa5ec1eb3d83f801757b8b0de46f6e008d41ff83f9028905d86f6e007c48c5f828d0c5f857c0c5fa2ac0c5fa2a0dbe6f6e00c5fa5ec1c5fa100d86c72b00c5f25cc0c5fa1105ae6f6e00c5f828c2eb35c705926f6e0000000000c705946f6e0000000000eb14c7057c6f6e0002000000c7057e6f6e000000803f48c7056b6f6e0001000000",
      "c5fa10842490010000c5f82ec10f82b90000008b05daf16e0083f803752e8b0dd7f16e008d41ff83f9028905cbf16e007c65c5f828d0c5f857c0c5fa2ac0c5fa2a0db1f16e00c5fa5ec1eb3d83f801757b8b0da4f16e008d41ff83f902890598f16e007c48c5f828d0c5f857c0c5fa2ac0c5fa2a0d7ef16e00c5fa5ec1c5fa100de2262c00c5f25cc0c5fa11056ef16e00c5f828c2eb35c70552f16e0000000000c70554f16e0000000000eb14c7053cf16e0002000000c7053ef16e000000803f48c7052bf16e0001000000"}},
    {"the ramp", {0x4f9c18, 0x4f8268, 0x4f8158}, 0x47,
     {"8b0506485c003b05fc475c0073518a0d80066f0084c9754789c1ffc0c4e1fa2ac1c5fa5905af475c00c5fa5e051f0a2c00c5fa58059b475c00c5fa1105d7475c008905c5475c00",
      "8b05d6e05b003b05cce05b0073518a0d509f6e0084c9754789c1ffc0c4e1fa2ac1c5fa59057fe05b00c5fa5e0533ac2b00c5fa58056be05b00c5fa1105a7e05b00890595e05b00",
      "8b0586625c003b057c625c0073518a0d00216f0084c9754789c1ffc0c4e1fa2ac1c5fa59052f625c00c5fa5e058f0b2c00c5fa58051b625c00c5fa110557625c00890545625c00"}},
    /* the order of the main loop (the hold is undone at its loop-top wait, after the update) and
     * the update's tick loop (the coroutines resumed per tick, the loop skipped with no tick) */
    {"the main loop's sceHmdGetDeviceInformationByHandle (tick_frame)", {0x54aff4, 0x54a3e4, 0x549534}, 5, {"e817982300", "e8272c2300", "e837992300"}},
    {"the main loop's call to the update", {0x54b292, 0x54a682, 0x5497d2}, 5, {"e89943faff", "e8f935faff", "e89943faff"}},
    {"the main loop's loop-top sceKernelWaitEventFlag", {0x549846, 0x548c36, 0x547d86}, 5, {"e8959d2300", "e8e5312300", "e8b59e2300"}},
    {"the update's tick loop skipped with no tick", {0x4f0813, 0x4eee63, 0x4eed53}, 8, {"85c90f8edb230000", "85c90f8edb230000", "85c90f8edb230000"}},
    {"the 28 coroutine slots resumed in each tick", {0x4f09b0, 0x4ef000, 0x4eeef0}, 17,
     {"ffc08905682f740183f81c0f822fffffff", "ffc08905f8c7730183f81c0f822fffffff", "ffc08905e849740183f81c0f822fffffff"}},
    /* 10: right after the ramp (whose jae / jne skip to this site's end) */
    {"the envelope time += 1.0", {0x4f9c5f, 0x4f82af, 0x4f819f}, 24,
     {"c5fa1005d9475c00c5fa580595092c00c5fa1105c9475c00", "c5fa1005a9e05b00c5fa5805a9ab2b00c5fa110599e05b00", "c5fa100559625c00c5fa5805050b2c00c5fa110549625c00"}},
};
enum { VT_FLAG, VT_SET, VT_ODUR, VT_OSTATE, VT_FSTATE, VT_FCOUNT, VT_FTOTAL, VT_RCOUNT, VT_RLIMIT, VT_PAUSE, VT_ENV,
       VT_ONE, VT_N };
static const struct rs_link k_vt_links[VT_N] = {
    [VT_FLAG] = {0, 3, 8, {0xbd6ab0, 0xbce9c0, 0xbd6a70}},    [VT_SET] = {0, 11, 15, {0xbd6890, 0xbce7a0, 0xbd6850}},
    [VT_ODUR] = {2, 3, 7, {0xbd6be0, 0xbceaf0, 0xbd6ba0}},    [VT_OSTATE] = {2, 10, 14, {0xbd6bf0, 0xbceb00, 0xbd6bb0}},
    [VT_FSTATE] = {3, 21, 25, {0xbe5780, 0xbdd690, 0xbe5740}}, [VT_FCOUNT] = {3, 32, 36, {0xbe5788, 0xbdd698, 0xbe5748}},
    [VT_FTOTAL] = {3, 66, 70, {0xbe5784, 0xbdd694, 0xbe5744}}, [VT_RCOUNT] = {4, 2, 6, {0xabe424, 0xab6344, 0xabe3e4}},
    [VT_RLIMIT] = {4, 8, 12, {0xabe420, 0xab6340, 0xabe3e0}},  [VT_PAUSE] = {4, 16, 20, {0xbea2ac, 0xbe21cc, 0xbea26c}},
    [VT_ENV] = {10, 4, 8, {0xabe440, 0xab6360, 0xabe400}},     [VT_ONE] = {10, 12, 16, {0x7ba604, 0x7b2e68, 0x7b8cb4}}, /* (1.0f) */
};
static const struct rs_link k_vt_links2[] = {
    {2, 77, 81, {0xbd6bf0, 0xbceb00, 0xbd6bb0}},  /* the overlay's state written */
    {2, 85, 89, {0xbd6bf4, 0xbceb04, 0xbd6bb4}},  /* its count written */
    {3, 44, 48, {0xbe5788, 0xbdd698, 0xbe5748}},  /* the light fade's count written */
    {4, 67, 71, {0xabe424, 0xab6344, 0xabe3e4}},  /* the ramp's count written */
    {5, 1, 5, {0x784810, 0x77d010, 0x782e70}},    /* sceHmdGetDeviceInformationByHandle */
    {6, 1, 5, {0x4ef630, 0x4edc80, 0x4edb70}},    /* the update */
    {7, 1, 5, {0x7835e0, 0x77be20, 0x781c40}},    /* sceKernelWaitEventFlag */
    {8, 4, 8, {0x4f2bf6, 0x4f1246, 0x4f1136}},    /* the tick loop's end */
    {10, 20, 24, {0xabe440, 0xab6360, 0xabe400}}, /* the envelope time written */
};
/* the game speed (as agent.tickhold's, hooks.c): the light fade steps only while it is not negative */
static const u32 k_speed[N_GAMES] = {0x8c5070, 0x8bd070, 0x8c5070};

static int hexval(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }
/* the site's bytes into out (RS_MAX long): 0 when the entry is longer than that or not hex */
static int site_bytes(const struct rs_site *s, int g, u8 *out) {
  const char *h = s->hex[g];
  if (!h || s->len > RS_MAX)
    return 0;
  for (u32 i = 0; i < s->len; i++) {
    int a = hexval(h[2 * i]), b = a < 0 ? -1 : hexval(h[2 * i + 1]);
    if (a < 0 || b < 0)
      return 0;
    out[i] = (u8)(a << 4 | b);
  }
  return h[2 * s->len] == 0;
}
/* 1: the table leads where it says and the live code is as in the eboot (logged either way) */
static int rs_check(const char *opt, const u8 *base, int g, const struct rs_site *sites, u32 ns,
                    const struct rs_link *l1, u32 n1, const struct rs_link *l2, u32 n2) {
  static u8 buf[RS_MAX];
  for (u32 k = 0; k < n1 + n2; k++) {
    const struct rs_link *l = k < n1 ? &l1[k] : &l2[k - n1];
    const struct rs_site *s = &sites[l->site];
    if (!site_bytes(s, g, buf) || l->end > s->len) {
      LOG("%s: the table's site \"%s\" is not well formed: off (a mistake in vrreact.c)", opt, s->what);
      return 0;
    }
    s32 d;
    memcpy(&d, buf + l->disp, 4);
    u64 to = (u64)s->va[g] + l->end + (u64)(s64)d;
    if (to != l->to[g]) {
      LOG("%s: the table's site \"%s\" leads to 0x%lx, not 0x%x: off (a mistake in vrreact.c)", opt, s->what, to,
          l->to[g]);
      return 0;
    }
  }
  for (u32 i = 0; i < ns; i++) {
    const struct rs_site *s = &sites[i];
    const u8 *p = base + s->va[g];
    if (!site_bytes(s, g, buf)) {
      LOG("%s: the table's site \"%s\" (%u bytes) is not well formed or longer than %u: off (a mistake in "
          "vrreact.c)", opt, s->what, s->len, RS_MAX);
      return 0;
    }
    if (!mem_ok(p, s->len, 0)) {
      LOG("%s: the game's code at 0x%x (%s, %u bytes) not readable memory: off", opt, s->va[g], s->what, s->len);
      return 0;
    }
    for (u32 j = 0; j < s->len; j++)
      if (p[j] != buf[j]) {
        LOG("%s: the game's code at 0x%x (%s) not as expected: at +0x%x (0x%lx) 0x%02x, the eboot's 0x%02x: off",
            opt, s->va[g], s->what, j, (u64)s->va[g] + j, p[j], buf[j]);
        return 0;
      }
  }
  LOG("%s: %s: the game's code as expected (%u sites)", opt, game_name(g), ns);
  return 1;
}
#define NSITES(a) ((u32)(sizeof(a) / sizeof *(a)))

/* ---- agent.lookat ---- */
/* the calls and skips, printed on the cvars thread (a LOG on the main thread may hold a loop up) */
struct look_ev {
  u32 girl, level, count, branch; /* branch: 1 level 2-3, 2 the 5 s one, 0 none */
  s32 cool, prio0, prio1, dur1, old2a0;
  const char *skip; /* 0: called */
  u8 *gaze, *target;
};
static struct look_ev g_look_ev[8];
static volatile u32 g_look_w, g_look_r;
static u32 g_look_lost;
static void look_push(const struct look_ev *e) {
  u32 w = g_look_w;
  if (w - __atomic_load_n(&g_look_r, __ATOMIC_ACQUIRE) >= 8) {
    g_look_lost++;
    return;
  }
  g_look_ev[w % 8] = *e;
  __atomic_store_n(&g_look_w, w + 1, __ATOMIC_RELEASE);
}

void lookat_tick(u8 *base, int g) {
  static int state; /* 0 not checked, 1 on, -1 off */
  static u8 *last_st;
  static u32 last_count, have;
  if (!g_opt_lookat || g < 0 || g >= N_GAMES) {
    have = 0; /* (switched off in the menu: on again, the count is looked at afresh) */
    return;
  }
  if (!state)
    state = rs_check("lookat", base, g, k_look_sites, NSITES(k_look_sites), k_look_links, LK_N, k_look_links2,
                     NSITES(k_look_links2))
                ? 1
                : -1;
  if (state < 0)
    return;
#define LA(f) (base + k_look_links[f].to[g])
  u8 *st = *(u8 *volatile *)LA(LK_STATE);
  if (!st || !g_vr.in_vr) {
    have = 0;
    return;
  }
  if (st != last_st) {
    if (!mem_ok(st, 0x28, 0)) {
      LOG("lookat: the manager's state at %p not readable: off", st);
      state = -1;
      return;
    }
    last_st = st, have = 0;
  }
  u32 count = *(volatile u32 *)(st + 0x18);
  if (!have || count <= last_count) { /* the first look, nothing new, or a segment's start (count 0) */
    have = 1, last_count = count;
    return;
  }
  last_count = count;
  /* the count went up in the last update: the dead branch, as 0x556b90 has it after its lookup */
  struct look_ev e = {0};
  e.girl = *(volatile u32 *)(st + 0x10), e.level = *(volatile u32 *)(st + 0x14), e.count = count;
  if (e.girl >= 4) {
    e.skip = "girl index not below 4 (the game would stop there)";
    look_push(&e);
    return;
  }
  u32 j = *(volatile u32 *)(LA(LK_GIRLS) + e.girl * 0x7e0 + 0x2c);
  u8 *n = *(u8 *volatile *)LA(LK_MODELS);
  if (j >= 4) {
    e.skip = "her slot not below 4 (the game would stop there)";
    look_push(&e);
    return;
  }
  u64 want = *(volatile u64 *)(LA(LK_CHARS) + j * 8);
  for (u32 steps = 0; n && *(volatile u64 *)n != want && steps < 4096; steps++)
    n = *(u8 *volatile *)(n + 0x18);
  if (!n || *(volatile u64 *)n != want) {
    e.skip = "her model not in the model list (the game returns)";
    look_push(&e);
    return;
  }
  u8 *gaze = LA(LK_GAZE) + e.girl * 0x2b0;
  e.gaze = gaze;
  if (*(volatile u32 *)(gaze + 4) != e.girl) { /* (0x34c120 looks her model up by its +4, with no end test) */
    e.skip = "the gaze object's girl (+4) is not hers: not called";
    look_push(&e);
    return;
  }
  e.old2a0 = *(volatile s32 *)(gaze + 0x2a0);
  *(volatile s32 *)(gaze + 0x2a0) = 1000; /* movl $0x3e8, 0x2a0(gaze) */
  s64 a = *(volatile s8 *)LA(LK_CAM_I8), c = *(volatile s32 *)LA(LK_CAM_I32);
  u8 *target = LA(LK_CAM) + a * 0x220 + c * 0x110 + 0x90;
  e.target = target;
  e.cool = *(volatile s32 *)(gaze + 0x278), e.prio0 = *(volatile s32 *)gaze;
  s32 dur, flag, mode;
  if ((e.level & ~1u) == 2)
    e.branch = 1, dur = -1, flag = 0, mode = 0xffff;
  else if (count != 0)
    e.branch = 2, dur = 300, flag = 1, mode = 9;
  else {
    e.skip = "count 0";
    look_push(&e);
    return;
  }
  if (e.cool > 0) {
    e.skip = "gaze +0x278 above 0 (the game returns)";
    look_push(&e);
    return;
  }
  if (!mem_ok(target, 0x110, 0)) {
    e.skip = "the camera target not readable: not called";
    look_push(&e);
    return;
  }
  void (*look)(u8 *, u8 *, s32, s32, s32, s32) = (void (*)(u8 *, u8 *, s32, s32, s32, s32))LA(LK_LOOK);
  look(gaze, target, 2, dur, flag, mode);
  e.prio1 = *(volatile s32 *)gaze, e.dur1 = *(volatile s32 *)(gaze + 0x270);
  look_push(&e);
#undef LA
}

/* ---- agent.vrtimers ---- */
static struct {
  u32 on;          /* a no-tick loop's snapshot taken, its update not yet undone */
  int g;
  u8 *base;
  s32 *set_count;  /* 1: the light set's countdown (0: not running) */
  s32 set_was;
  u32 o_state;     /* 2: the overlay (state 1-3) */
  s32 o_was;
  u32 f_state;     /* 3: the light fade (state 1 or 3) */
  s32 f_was;
  u32 r_on, r_was; /* 4: the ramp (below its end, not paused); 5: the envelope time, the same condition */
  float e_was;
} g_vt;
static int g_vt_state; /* 0 not checked, 1 on, -1 off (the main thread's) */
#define VT_HELD 5
static struct {
  volatile u32 due;
  u32 loops, held[VT_HELD], changed[VT_HELD], late;
} g_vt_rep;
static u32 g_vt_loops, g_vt_held[VT_HELD], g_vt_changed[VT_HELD], g_vt_late;
static u64 g_vt_next;

/* 0: nothing to undo; else the game (g, game_name's) + 1 */
u32 vrtimers_pending(void) { return g_vt.on ? (u32)g_vt.g + 1 : 0; }

/* after the no-tick loop's update (its loop-top wait, or the next loop's tick_frame when the game
 * skipped that wait): each counter the update stepped by exactly 1 is set back */
void vrtimers_end(int late) {
  if (!g_vt.on)
    return;
  g_vt.on = 0;
  u8 *base = g_vt.base;
  int g = g_vt.g;
#define VA(f) (base + k_vt_links[f].to[g])
  if (g_vt.set_count) {
    if (*(volatile s32 *)g_vt.set_count == g_vt.set_was - 1)
      *(volatile s32 *)g_vt.set_count = g_vt.set_was, g_vt_held[0]++;
    else
      g_vt_changed[0]++;
  }
  if (g_vt.o_state) {
    if (*(volatile u32 *)VA(VT_OSTATE) == g_vt.o_state && *(volatile s32 *)(VA(VT_OSTATE) + 4) == g_vt.o_was - 1)
      *(volatile s32 *)(VA(VT_OSTATE) + 4) = g_vt.o_was, g_vt_held[1]++;
    else
      g_vt_changed[1]++;
  }
  if (g_vt.f_state) {
    if (*(volatile u32 *)VA(VT_FSTATE) == g_vt.f_state && *(volatile s32 *)VA(VT_FCOUNT) == g_vt.f_was - 1)
      *(volatile s32 *)VA(VT_FCOUNT) = g_vt.f_was, g_vt_held[2]++;
    else
      g_vt_changed[2]++;
  }
  if (g_vt.r_on) {
    if (*(volatile u32 *)VA(VT_RCOUNT) == g_vt.r_was + 1)
      *(volatile u32 *)VA(VT_RCOUNT) = g_vt.r_was, g_vt_held[3]++;
    else
      g_vt_changed[3]++;
    /* 5: stepped under the ramp's condition, so with it (its += 1.0 the game's own float add) */
    volatile float *et = (volatile float *)VA(VT_ENV);
    if (*et == g_vt.e_was + 1.0f)
      *et = g_vt.e_was, g_vt_held[4]++;
    else
      g_vt_changed[4]++;
  }
  g_vt_late += late != 0;
}

/* the start of a loop the scheduler gives no tick (tick_frame, frame_pace 1): what the update will
 * step this loop, as its own conditions read */
void vrtimers_begin(u8 *base, int g) {
  if (!g_opt_vrtimers || g < 0 || g >= N_GAMES)
    return;
  if (!g_vt_state) {
    g_vt_state = rs_check("vrtimers", base, g, k_vt_sites, NSITES(k_vt_sites), k_vt_links, VT_N, k_vt_links2,
                          NSITES(k_vt_links2))
                     ? 1
                     : -1;
    if (g_vt_state > 0 && *(const volatile float *)(base + k_vt_links[VT_ONE].to[g]) != 1.0f) {
      LOG("vrtimers: the envelope's step at 0x%x is not 1.0: off", k_vt_links[VT_ONE].to[g]);
      g_vt_state = -1;
    }
  }
  if (g_vt_state < 0)
    return;
  g_vt.base = base, g_vt.g = g;
  u32 k = *(volatile u64 *)VA(VT_FLAG) != 0;
  s32 *sc = (s32 *)(VA(VT_SET) + k * 0x120 + 0x11c);
  g_vt.set_was = *(volatile s32 *)sc;
  g_vt.set_count = g_vt.set_was > 0 ? sc : 0;
  u32 os = *(volatile u32 *)VA(VT_OSTATE);
  g_vt.o_state = os >= 1 && os <= 3 ? os : 0;
  g_vt.o_was = *(volatile s32 *)(VA(VT_OSTATE) + 4);
  u32 fs = *(volatile u32 *)VA(VT_FSTATE);
  float speed = *(volatile float *)(base + k_speed[g]);
  g_vt.f_state = (fs == 1 || fs == 3) && !(speed < 0.0f) ? fs : 0;
  g_vt.f_was = *(volatile s32 *)VA(VT_FCOUNT);
  g_vt.r_was = *(volatile u32 *)VA(VT_RCOUNT);
  g_vt.r_on = g_vt.r_was < *(volatile u32 *)VA(VT_RLIMIT) && !*(volatile u8 *)VA(VT_PAUSE);
  g_vt.e_was = *(volatile float *)VA(VT_ENV);
  g_vt.on = g_vt.set_count || g_vt.o_state || g_vt.f_state || g_vt.r_on;
  g_vt_loops++;
#undef VA
}

/* every 10 s, from tick_frame (the main thread): the counts handed to the cvars thread */
static void vrtimers_window(void) {
  u64 now = now_us();
  if (!g_vt_next)
    g_vt_next = now + 10000000;
  if (now < g_vt_next)
    return;
  g_vt_next = now + 10000000;
  if (g_vt_loops && !__atomic_load_n(&g_vt_rep.due, __ATOMIC_ACQUIRE)) {
    g_vt_rep.loops = g_vt_loops, g_vt_rep.late = g_vt_late;
    for (int i = 0; i < VT_HELD; i++)
      g_vt_rep.held[i] = g_vt_held[i], g_vt_rep.changed[i] = g_vt_changed[i];
    __atomic_store_n(&g_vt_rep.due, 1, __ATOMIC_RELEASE);
  }
  g_vt_loops = g_vt_late = 0;
  for (int i = 0; i < VT_HELD; i++)
    g_vt_held[i] = g_vt_changed[i] = 0;
}
void vrreact_window(void) {
  if (g_opt_vrtimers && g_vt_state > 0)
    vrtimers_window();
}

/* ---- agent.uihold (debug, off; logs/research/fps/TIMERS-120.md item 3, TIMERS-120-FIX.md) ----
 * The update's "seconds table" span runs once a loop, ungated by the ticks, the game speed or the
 * pause, with dt = the mode's 1/60 s (Scarlet 0x4f864a..0x4f8a4b, Fortune 0x4f6c9a..0x4f709b; every
 * path through it ends at its mutex unlock and falls through to its end, nothing jumps into it but
 * to its first instruction): at 120 fps twice a tick. In it:
 *   - the UI managers' map *0x94bde0 / *0x943d60 walked, each node's object (+0x28)
 *     vtable[0x30](dt): 37 update functions (menus, message windows, scrolling text 0x5df370 /
 *     0x5df810, timed screen states, frame-counted fades 0x586fd0 / 0x586ee0 ...): twice as fast;
 *   - the layout manager 0x5da7f0 / 0x5dac90 (dt: stored, not read);
 *   - the boot logo's fade state *0xbebad8 / *0xbe39f8 (+0x24 += dt; started by the boot logo only).
 * The payload (patch_uihold, at the game's start, with agent.uihold=1 in cvars.txt; debug builds)
 * puts a jmp at the span's first instruction (movl <ring index>, %eax, 6 bytes) to 38 bytes written
 * past the text's end (agent.widew's place + 0x400, after agent.nearcull's): if the flag past the
 * data segment's end is set, clear it, count, and jump to the span's end (the same movl again);
 * else the movl and back. Here,
 * on every loop: the flag = no tick this loop (and agent.uihold, followed live in a debug build),
 * so the span runs once a tick, as at 60. The cave clears the flag itself: a loop that never
 * reaches the span leaves nothing for the next one.
 * What could show (to test first, worn or by shots): a manager that builds per-picture draw data
 * in its update would draw its UI on every other picture only (flicker), or a cursor / auto-repeat
 * counted per call would take input at 60 (as at 60 fps). The light setup after the span copies
 * 12 bytes of a stack slot the span writes (the low 4 the walk's map key) into the padding of a
 * light constant (0x8d42f4 / its copy 0x8d4884) on its blend path: an uninitialised read of the
 * game's own, which then keeps an earlier value on a held loop (the vec4's first float is the
 * value; likely nothing visible). */
static const struct ui_k {
  u32 site, cave, flag, after; /* flag: u32 flag, then the cave's u32 count */
} k_ui[N_GAMES] = {
    {0x4f864a, 0x89ef90 + 0x400, 0x1df6c40 + 0x20, 0x4f8a4b}, /* Scarlet 1.08 */
    {0x4f6c9a, 0x895580 + 0x400, 0x1deeb20 + 0x20, 0x4f709b}, /* Fortune 1.19 */
    {0x4f6b8a, 0x89d4e0 + 0x400, 0x1df6c00 + 0x20, 0x4f6f8b}, /* Scarlet 1.00 */
};
static int g_ui_state; /* 0 not looked, 1 the payload's jump in, -1 not (the main thread's) */
static struct {
  volatile u32 due;
  u32 loops, none, set, skipped;
} g_ui_rep;
static u32 g_ui_loops, g_ui_none, g_ui_set, g_ui_count0;
static u64 g_ui_next;

void uihold_tick(u8 *base, int g, int none) {
  if (g < 0 || g >= N_GAMES || (!g_opt_uihold && g_ui_state <= 0))
    return;
  const struct ui_k *k = &k_ui[g];
  if (!g_ui_state) {
    const u8 *s = base + k->site;
    int ok = mem_ok(s, 6, 0) && mem_ok(base + k->cave, 2, 0) && mem_ok(base + k->flag, 8, 1);
    s32 rel = 0;
    if (ok) {
      memcpy(&rel, s + 1, 4);
      ok = s[0] == 0xe9 && s[5] == 0x90 && (u64)k->site + 5 + (u64)(s64)rel == k->cave && base[k->cave] == 0x80 &&
           base[k->cave + 1] == 0x3d;
    }
    g_ui_state = ok ? 1 : -1;
    if (!ok) {
      LOG("uihold: %s: the payload's jump at 0x%x not there (agent.uihold=1 must be in cvars.txt at the game's "
          "start; the payload's log says why it refused): off", game_name(g), k->site);
      return;
    }
    g_ui_count0 = *(volatile u32 *)(base + k->flag + 4);
    LOG("uihold: %s: the payload's jump at 0x%x to 0x%x in: the menu/message layer (the span to 0x%x) skipped "
        "on loops with no tick", game_name(g), k->site, k->cave, k->after);
  }
  if (g_ui_state < 0)
    return;
  u32 set = g_opt_uihold && none;
  *(volatile u32 *)(base + k->flag) = set;
  g_ui_loops++, g_ui_none += none != 0, g_ui_set += set;
  u64 now = now_us();
  if (!g_ui_next)
    g_ui_next = now + 10000000;
  if (now < g_ui_next)
    return;
  g_ui_next = now + 10000000;
  u32 c = *(volatile u32 *)(base + k->flag + 4);
  if (!__atomic_load_n(&g_ui_rep.due, __ATOMIC_ACQUIRE)) {
    g_ui_rep.loops = g_ui_loops, g_ui_rep.none = g_ui_none, g_ui_rep.set = g_ui_set;
    g_ui_rep.skipped = c - g_ui_count0;
    __atomic_store_n(&g_ui_rep.due, 1, __ATOMIC_RELEASE);
  }
  g_ui_count0 = c, g_ui_loops = g_ui_none = g_ui_set = 0;
}

/* ---- agent.rateprobe (debug, off; TIMERS-120.md "A rate probe") ----
 * Read only, on the main thread at each loop's start (the values the last update and picture
 * left): how far each per-loop timer of the report moved, summed over 10 s and given per second,
 * and on how many loops after a loop with no tick it moved (those should be none with the fixes,
 * the frame clock aside). At 60 fps and at 120 with the fixes the rises should match:
 *   shader time (water) 60/s x speed; envelope time 60/s while its ramp runs; the game clock (the
 *   frame clock less the stopped time, 0x1c1b828 - 0x9430ac, the frozen 0x9430a4 while stopped)
 *   60/s; the boot fade *0xbebad8 +0x24 (s; its state +0x28 should stay 0 in VR); the frame-count
 *   list's whole frames 0x958f80 60/s; the seat follow 0xbec7b0 (metres moved); the UI managers
 *   found in the map *0x94bde0 by vtable: 0x8bb018's +0xdc (timed screen states, s: 1.0/s),
 *   0x8bb588's +0x1c (fade frames: 60/s while fading), and text objects 0x8af868 +0xc (the wait,
 *   s) and +0x10 (the scroll, px: 120/s) if any is in the map (they are likely made by managers
 *   instead: "0 found" then). Fortune's addresses in k_rp. Only rises are summed (a reset or a
 *   wrap is not). */
static const struct rp_k {
  u32 shader, env, clock, latch, frozen, off, fade, frames, pause, seat, map, vt[3];
} k_rp[N_GAMES] = {
    {0xac2190, 0xabe440, 0x1c1b828, 0x9430a0, 0x9430a4, 0x9430ac, 0xbebad8, 0x958f80, 0xbea2ac, 0xbec7b0, 0x94bde0,
     {0x8bb018, 0x8bb588, 0x8af868}}, /* Scarlet 1.08 */
    {0xaba0b0, 0xab6360, 0x1c13708, 0x93b0d0, 0x93b0d4, 0x93b0dc, 0xbe39f8, 0x950f00, 0xbe21cc, 0xbe46b0, 0x943d60,
     {0x8b2d30, 0x8b32a0, 0x8a7918}}, /* Fortune 1.19 */
    {0xac2150, 0xabe400, 0x1c1b7e8, 0x9430a0, 0x9430a4, 0x9430ac, 0xbeba98, 0x958f80, 0xbea26c, 0xbec770, 0x94bde0,
     {0x8bafe0, 0x8bb550, 0x8af868}}, /* Scarlet 1.00 */
};
enum { RP_SHADER, RP_ENV, RP_CLOCK, RP_FADE, RP_FRAMES, RP_SEAT, RP_UISTATE, RP_UIFADE, RP_TWAIT, RP_TSCROLL, RP_N };
static const char *const k_rp_name[RP_N] = {
    "shader time", "envelope time", "game clock", "boot fade time (s)", "frame-count list frames",
    "seat follow (m)", "UI timed states +0xdc (s)", "UI fades +0x1c (frames)", "text wait +0xc (s)",
    "text scroll +0x10 (px)"};
struct rp_acc {
  double sum;
  u32 moved, moved_none;
};
static struct rp_acc g_rp[RP_N];
static struct {
  volatile u32 due;
  u64 us;
  u32 loops, ticks, none, found[3], fade_state_max, uihold_skips;
  struct rp_acc a[RP_N];
} g_rp_rep;
static u32 g_rp_loops, g_rp_ticks, g_rp_none, g_rp_found[3], g_rp_fstate;
static u64 g_rp_t0;
#define RP_OBJ 16
struct rp_obj {
  const u8 *o;
  float a, b;
};
static struct rp_obj g_rp_objs[3][RP_OBJ];
static u32 g_rp_nobj[3];

static void rp_note(int i, double d, int prev_none) {
  if (d == 0.0)
    return;
  if (d > 0.0)
    g_rp[i].sum += d;
  g_rp[i].moved++;
  g_rp[i].moved_none += prev_none != 0;
}

/* the map's objects of the three vtables (the game's own walk, read only), their fields' rises */
static void rp_ui(u8 *base, const struct rp_k *k, int prev_none, int first) {
  static u8 *head_ok;
  u8 *head = *(u8 *volatile *)(base + k->map);
  struct rp_obj now[3][RP_OBJ];
  u32 n[3] = {0, 0, 0};
  if (head && head != head_ok) {
    if (((u64)head & 7) || !mem_ok(head, 0x20, 0))
      head = 0;
    else
      head_ok = head;
  }
  u8 *x = head ? *(u8 **)head : 0; /* the leftmost node (head+0) */
  for (u32 steps = 0; x && x != head && !x[0x19] && steps < 256; steps++) {
    const u8 *o = *(u8 *const *)(x + 0x28);
    if (o && !((u64)o & 7)) {
      u64 vt = *(const volatile u64 *)o;
      for (int c = 0; c < 3; c++) {
        if (vt != (u64)(base + k->vt[c]) || n[c] >= RP_OBJ)
          continue;
        struct rp_obj *r = &now[c][n[c]++];
        r->o = o;
        if (c == 0)
          r->a = *(const volatile float *)(o + 0xdc), r->b = 0;
        else if (c == 1)
          r->a = (float)*(const volatile u16 *)(o + 0x1c), r->b = 0;
        else
          r->a = *(const volatile float *)(o + 0xc), r->b = *(const volatile float *)(o + 0x10);
      }
    }
    u8 *r = *(u8 **)(x + 0x10); /* the in-order successor (Dinkumware's tree: left +0, parent +8, right +0x10) */
    if (!r)
      break;
    if (!r[0x19]) {
      x = r;
      for (u32 d = 0; *(u8 **)x && !(*(u8 **)x)[0x19] && d < 64; d++)
        x = *(u8 **)x;
    } else {
      u8 *p = *(u8 **)(x + 8);
      for (u32 d = 0; p && !p[0x19] && x == *(u8 **)(p + 0x10) && d < 64; d++)
        x = p, p = *(u8 **)(x + 8);
      x = p;
    }
  }
  for (int c = 0; c < 3; c++) {
    if (n[c] > g_rp_found[c])
      g_rp_found[c] = n[c];
    for (u32 i = 0; i < n[c] && !first; i++)
      for (u32 j = 0; j < g_rp_nobj[c]; j++)
        if (g_rp_objs[c][j].o == now[c][i].o) {
          static const int k_a[3] = {RP_UISTATE, RP_UIFADE, RP_TWAIT};
          rp_note(k_a[c], (double)now[c][i].a - g_rp_objs[c][j].a, prev_none);
          if (c == 2)
            rp_note(RP_TSCROLL, (double)now[c][i].b - g_rp_objs[c][j].b, prev_none);
          break;
        }
    memcpy(g_rp_objs[c], now[c], n[c] * sizeof now[c][0]);
    g_rp_nobj[c] = n[c];
  }
}

void rateprobe_tick(u8 *base, int g, int none, s32 ticks) {
  static int prev_none, have;
  static float shader, env, clock, ftime, seat[3];
  static u8 *fade_ok;
  if (!g_opt_rateprobe || g < 0 || g >= N_GAMES)
    return;
  const struct rp_k *k = &k_rp[g];
  u64 now = now_us();
  if (!have) {
    g_rp_t0 = now;
    LOG("rateprobe: %s: the per-loop timers read at each loop's start, a line every 10 s",
        game_name(g));
  }
  float sh = *(const volatile float *)(base + k->shader), en = *(const volatile float *)(base + k->env);
  float ck = *(const volatile u8 *)(base + k->latch)
                 ? *(const volatile float *)(base + k->clock) - *(const volatile float *)(base + k->off)
                 : *(const volatile float *)(base + k->frozen);
  const volatile float *sp = (const volatile float *)(base + k->seat);
  float s3[3] = {sp[0], sp[1], sp[2]};
  u8 *fo = *(u8 *volatile *)(base + k->fade);
  float ft = 0.0f;
  if (fo && fo != fade_ok)
    fade_ok = ((u64)fo & 3) || !mem_ok(fo + 0x24, 8, 0) ? 0 : fo;
  if (fo && fo == fade_ok) {
    ft = *(const volatile float *)(fo + 0x24);
    u32 st = *(const volatile u32 *)(fo + 0x28);
    if (st > g_rp_fstate)
      g_rp_fstate = st;
  }
  if (have) {
    rp_note(RP_SHADER, (double)sh - shader, prev_none);
    rp_note(RP_ENV, (double)en - env, prev_none);
    rp_note(RP_CLOCK, (double)ck - clock, prev_none);
    if (fo)
      rp_note(RP_FADE, (double)ft - ftime, prev_none);
    if (!*(const volatile u8 *)(base + k->pause))
      rp_note(RP_FRAMES, (double)*(const volatile u32 *)(base + k->frames), prev_none);
    float dx = s3[0] - seat[0], dy = s3[1] - seat[1], dz = s3[2] - seat[2];
    rp_note(RP_SEAT, (double)fsqrt(dx * dx + dy * dy + dz * dz), prev_none);
  }
  rp_ui(base, k, prev_none, !have);
  shader = sh, env = en, clock = ck, ftime = ft, seat[0] = s3[0], seat[1] = s3[1], seat[2] = s3[2];
  have = 1, prev_none = none;
  g_rp_loops++, g_rp_ticks += ticks > 0 ? (u32)ticks : 0, g_rp_none += none != 0;
  if (now - g_rp_t0 < 10000000)
    return;
  if (!__atomic_load_n(&g_rp_rep.due, __ATOMIC_ACQUIRE)) {
    g_rp_rep.us = now - g_rp_t0, g_rp_rep.loops = g_rp_loops, g_rp_rep.ticks = g_rp_ticks, g_rp_rep.none = g_rp_none;
    g_rp_rep.fade_state_max = g_rp_fstate;
    g_rp_rep.uihold_skips = g_ui_state > 0 ? *(volatile u32 *)(base + k_ui[g].flag + 4) : 0;
    for (int c = 0; c < 3; c++)
      g_rp_rep.found[c] = g_rp_found[c];
    memcpy(g_rp_rep.a, g_rp, sizeof g_rp);
    __atomic_store_n(&g_rp_rep.due, 1, __ATOMIC_RELEASE);
  }
  memset(g_rp, 0, sizeof g_rp);
  g_rp_t0 = now, g_rp_loops = g_rp_ticks = g_rp_none = g_rp_fstate = 0;
  g_rp_found[0] = g_rp_found[1] = g_rp_found[2] = 0;
}

static void rateprobe_report(void) {
  if (!__atomic_load_n(&g_rp_rep.due, __ATOMIC_ACQUIRE))
    return;
  double s = (double)g_rp_rep.us / 1e6;
  LOG("rateprobe: %u loops in %f s, %u ticks (%f a second), %u loops with no tick; UI objects found: %u timed "
      "states, %u fades, %u texts; the boot fade's state at most %u; the uihold cave's count %u",
      g_rp_rep.loops, s, g_rp_rep.ticks, (double)g_rp_rep.ticks / s, g_rp_rep.none, g_rp_rep.found[0],
      g_rp_rep.found[1], g_rp_rep.found[2], g_rp_rep.fade_state_max, g_rp_rep.uihold_skips);
  for (int i = 0; i < RP_N; i++)
    LOG("rateprobe:   %s: +%f a second, moved on %u loops, %u of them after a loop with no tick", k_rp_name[i],
        g_rp_rep.a[i].sum / s, g_rp_rep.a[i].moved, g_rp_rep.a[i].moved_none);
  __atomic_store_n(&g_rp_rep.due, 0, __ATOMIC_RELEASE);
}

/* the agent's cvars thread: the lines due */
void vrreact_report(void) {
  rateprobe_report();
  if (__atomic_load_n(&g_ui_rep.due, __ATOMIC_ACQUIRE)) {
    LOG("uihold: in the last 10 s, %u loops, %u with no tick: the menu/message layer skipped on %u (the flag set "
        "on %u)%s", g_ui_rep.loops, g_ui_rep.none, g_ui_rep.skipped, g_ui_rep.set,
        g_opt_uihold ? "" : " (agent.uihold now 0)");
    __atomic_store_n(&g_ui_rep.due, 0, __ATOMIC_RELEASE);
  }
  while (g_look_r != __atomic_load_n(&g_look_w, __ATOMIC_ACQUIRE)) {
    const struct look_ev *e = &g_look_ev[g_look_r % 8];
#ifdef RELEASE
    static u32 said; /* (a release build: the first few, then nothing: on by default since 2026-11-14) */
    if (said++ >= 3)
      ;
    else
#endif
    if (e->skip)
      LOG("lookat: the count rose to %u (level %u, girl %u): not called: %s (gaze %p, +0x278 %d, +0x2a0 was %d)",
          e->count, e->level, e->girl, e->skip, e->gaze, e->cool, e->old2a0);
    else
      LOG("lookat: the count rose to %u (level %u, girl %u): the game's look-at called: gaze %p, target %p, "
          "priority 2, duration %d, flag %d, mode 0x%x (%s); her priority %d before, %d after, its duration now %d; "
          "+0x2a0 was %d, now 1000",
          e->count, e->level, e->girl, e->gaze, e->target, e->branch == 1 ? -1 : 300, e->branch == 1 ? 0 : 1,
          e->branch == 1 ? 0xffff : 9, e->branch == 1 ? "level 2-3: until changed" : "5 s", e->prio0, e->prio1,
          e->dur1, e->old2a0);
    __atomic_store_n(&g_look_r, g_look_r + 1, __ATOMIC_RELEASE);
  }
  if (g_look_lost) {
    LOG("lookat: %u events not logged (the queue was full)", g_look_lost);
    g_look_lost = 0;
  }
  if (__atomic_load_n(&g_vt_rep.due, __ATOMIC_ACQUIRE)) {
    LOG_DETAIL("vrtimers: in the last 10 s, %u loops with no tick: held (stepped back after the update) the light "
               "set crossfade in %u, the overlay fade in %u, the light fade in %u, the ramp in %u, the envelope time "
               "in %u (not held, the game changed it: %u %u %u %u %u; undone at the next loop: %u)",
               g_vt_rep.loops, g_vt_rep.held[0], g_vt_rep.held[1], g_vt_rep.held[2], g_vt_rep.held[3],
               g_vt_rep.held[4], g_vt_rep.changed[0], g_vt_rep.changed[1], g_vt_rep.changed[2], g_vt_rep.changed[3],
               g_vt_rep.changed[4], g_vt_rep.late);
    __atomic_store_n(&g_vt_rep.due, 0, __ATOMIC_RELEASE);
  }
}
