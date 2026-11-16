/* PS VR side of the head pose: VrTracker2 poses (vr2.c) are moved into the PS VR tracker space
 * the game expects (metres, +X right, +Y up, -Z towards the camera; the player sits 1.5 m in
 * front of it, facing it), the seat taken from where the head is. The controller, which nothing
 * locates, is held in front of the player and turned by its own motion sensors; where PS VR2's
 * cameras see both palms around it (hand.c), it sits between them, as the PS Camera placed it.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "vr.h"
#include "../common/test.h"

static const float k_origin[3]; /* the seat's origin in game space, below */
static u64 now_us(void);

/* The ring's slot for `seq`, taken out of use while it is filled in (seq 0): a reader copying it
 * meanwhile (vr_frame_copy) sees that. */
static struct vr_frame *frame_slot(u32 seq) {
  struct vr_frame *f = &g_vr.frame[seq & 3];
  __atomic_store_n(&f->seq, 0, __ATOMIC_RELAXED);
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  return f;
}

/* Frame `seq` copied out of the ring whole, 0 when it is not there (older, or being written over:
 * a reader three frames behind copies the slot the next frame is going into) */
int vr_frame_copy(u32 seq, struct vr_frame *out) {
  const struct vr_frame *f = &g_vr.frame[seq & 3];
  if (__atomic_load_n(&f->seq, __ATOMIC_ACQUIRE) != seq)
    return 0;
  *out = *f;
  __atomic_thread_fence(__ATOMIC_ACQUIRE);
  return out->seq == seq && __atomic_load_n(&f->seq, __ATOMIC_RELAXED) == seq;
}

/* Dead or Alive Xtreme 3 hands its views over as slices of one 2D array texture (GCN T# type 13,
 * the slice in word 5's base and last array fields; wide left 0, wide right 1, near left 2, near
 * right 3: run 2, 2026-11-10). A slice is not a 2D texture of its own at an offset: slices after
 * the first have their banks rotated (tools/detile.c, measured on a VR-mode shot, 2026-11-11: a
 * plain 2D T# at slice 1's address read its rows in the wrong order). The T#s stay as the game
 * gave them (a one-slice view: the GPU clamps the layer to it whatever the shader gives), and
 * gen1.c keeps them apart by base address and slice. */
static int is_array_slice(const u32 *t) { return (t[3] >> 28) == 13; }

void vr_submit_frame(const struct hmd_reprojection_param *p, const struct hmd_tracker_state *ts,
                     s64 flip_arg) {
  u32 seq = g_vr.frame_w + 1;
  struct vr_frame *f = frame_slot(seq);
  memcpy(f->tsharp[0], p->texture[0], 32);
  memcpy(f->tsharp[1], p->texture[1], 32);
  if (is_array_slice(f->tsharp[0]) && !g_vr.eye_array) {
    g_vr.eye_array = 1;
    LOG("vr: the eyes are slices %u and %u of one array texture", f->tsharp[0][5] & 0x1fff,
        f->tsharp[1][5] & 0x1fff);
  }
  if (p->sampler)
    memcpy(f->sampler, p->sampler, 16);
  f->uv[0] = p->uv[0];
  f->uv[1] = p->uv[1];
  for (int i = 0; i < 3; i++)
    f->render_pose.pos[i] = ts->position[i];
  for (int i = 0; i < 4; i++)
    f->render_pose.rot[i] = ts->orientation[i];
  f->render_time = ts->timestamp;
  f->flip_arg = flip_arg;
  f->label = (volatile u64 *)p->frame_label;
  f->gpu_count = hooks_frame_count(); /* before the frame is published (the mirror waits on it) */
  f->flat = 0;
  __atomic_store_n(&f->seq, seq, __ATOMIC_RELEASE);
  __atomic_store_n(&g_vr.frame_w, seq, __ATOMIC_RELEASE);
}

static u32 g_flat_sampler[4];
static int g_flat_sampler_ok;
/* A flat picture (sceHmdReprojectionStart2dVr): the one texture for both eyes, drawn from where
 * the seat is, facing its forward (game space: the origin, no turn), so that it stands still in
 * the room like a screen. vr2.c gives it a screen's tangents. */
void vr_submit_flat(const u32 *tsharp, const u32 *sampler, const struct hmd_eye_uv *uv, u64 *label,
                    s64 flip_arg) {
  u32 seq = g_vr.frame_w + 1;
  struct vr_frame *f = frame_slot(seq);
  memcpy(f->tsharp[0], tsharp, 32);
  memcpy(f->tsharp[1], tsharp, 32);
  if (sampler != g_flat_sampler) { /* remembered (by base and size), for vr2.c's first registration */
    u32 n = g_vr.flat_n, known = 0;
    for (u32 i = 0; i < n; i++)
      known |= g_vr.flat_ts[i][0] == tsharp[0] && g_vr.flat_ts[i][1] == tsharp[1] &&
               g_vr.flat_ts[i][2] == tsharp[2];
    if (!known && n < 4) {
      memcpy(g_vr.flat_ts[n], tsharp, 32);
      __atomic_store_n(&g_vr.flat_n, n + 1, __ATOMIC_RELEASE);
    }
  }
  if (sampler) {
    memcpy(f->sampler, sampler, 16);
    if (sampler != g_flat_sampler) /* (the game's own: kept for vr_flat_flip) */
      memcpy(g_flat_sampler, sampler, 16), g_flat_sampler_ok = 1;
  }
  f->uv[0] = f->uv[1] = *uv;
  for (int i = 0; i < 3; i++)
    f->render_pose.pos[i] = k_origin[i];
  f->render_pose.rot[0] = f->render_pose.rot[1] = f->render_pose.rot[2] = 0.0f;
  f->render_pose.rot[3] = 1.0f;
  f->render_time = now_us();
  f->flip_arg = flip_arg;
  f->label = (volatile u64 *)label;
  f->gpu_count = 0; /* a flat picture: not waited for */
  f->flat = 1;
  __atomic_store_n(&f->seq, seq, __ATOMIC_RELEASE);
  __atomic_store_n(&g_vr.frame_w, seq, __ATOMIC_RELEASE);
}

/* Out of VR (back to the title menu), the game flips its own 2D picture to the TV and the
 * headset had nothing (black, the user 2026-11-12). Each of those flips is shown on the headset as
 * the VR menus' flat screen: the display buffer described like the game's own 2D-in-VR texture
 * (its first flat T#: 1920x1080 8_8_8_8, BGRA order, pitch 1920, as SetBufferAttribute has them)
 * with the buffer's base and the display buffers' tile mode, Display_2dThin (index 10; the VR 2D
 * texture is Thin_2dThin, 14: tools/detile's "display" and "thin"). Only once a flat T# was seen
 * (PS VR2 started from the VR menus) and only for the game's own registered buffers. */
int vr_flat_desc(u32 index, u32 out[8]) {
  if (!g_vr.flat_n || index >= g_vr.vo_count || index >= 16 || !g_vr.vo_buffers[index])
    return 0;
  const u32 *t = g_vr.flat_ts[0];
  u32 w = (t[2] & 0x3fff) + 1, h = ((t[2] >> 14) & 0x3fff) + 1;
  if (w != 1920 || h != 1080)
    return 0;
  u64 base = (u64)g_vr.vo_buffers[index];
  if (base & 0xff)
    return 0;
  memcpy(out, t, 32);
  out[0] = (u32)(base >> 8);
  out[1] = (out[1] & ~0xffu) | (u32)((base >> 40) & 0xff);
  out[3] = (out[3] & ~(0x1fu << 20)) | (10u << 20);
  return 1;
}
int vr_flat_flip(s32 index, s64 flip_arg) {
  u32 t[8];
  if (__atomic_load_n(&g_vr.in_vr, __ATOMIC_ACQUIRE) || !g_flat_sampler_ok || index < 0 ||
      !vr_flat_desc((u32)index, t))
    return 0;
  static const struct hmd_eye_uv whole = {1.0f, 1.0f, 0.0f, 0.0f};
  vr_submit_flat(t, g_flat_sampler, &whole, 0, flip_arg);
  static u32 said;
  if (!said++)
    LOG("vr: out of VR: the game's own flips shown on the headset as a flat screen (buffer %d at %p)",
        index, g_vr.vo_buffers[index]);
  return 1;
}

float fsqrt(float x) {
  float r;
  __asm__("sqrtss %1, %0" : "=x"(r) : "x"(x));
  return r;
}

static void qmul(const float a[4], const float b[4], float o[4]) {
  float r[4] = {
      a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
      a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
      a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
      a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2],
  };
  memcpy(o, r, sizeof r);
}

static void qconj(const float q[4], float o[4]) {
  o[0] = -q[0], o[1] = -q[1], o[2] = -q[2], o[3] = q[3];
}

static void qnorm(float q[4]) {
  float m = fsqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
  if (m > 1e-6f)
    for (int i = 0; i < 4; i++)
      q[i] /= m;
}

/* v' = q v q* */
static void qrot(const float q[4], const float v[3], float o[3]) {
  float x = q[0], y = q[1], z = q[2], w = q[3];
  float tx = 2 * (y * v[2] - z * v[1]), ty = 2 * (z * v[0] - x * v[2]), tz = 2 * (x * v[1] - y * v[0]);
  o[0] = v[0] + w * tx + (y * tz - z * ty);
  o[1] = v[1] + w * ty + (z * tx - x * tz);
  o[2] = v[2] + w * tz + (x * ty - y * tx);
}

/* yaw-only rotation taking -Z to the forward (fx, fz), and its inverse */
static void yaw_to(float fx, float fz, float yaw[4], float inv[4]) {
  float n = fsqrt(fx * fx + fz * fz), s = 0.0f, c = 1.0f;
  if (n > 1e-4f)
    s = -fx / n, c = -fz / n; /* R_y(a) (0,0,-1) = (-sin a, 0, -cos a) = forward */
  /* quaternion for angle a about +Y: normalize(0, sin a, 0, 1 + cos a) */
  float y = s, w = 1 + c, m = fsqrt(y * y + w * w);
  if (m < 1e-6f)
    y = 1, w = 0, m = 1;
  yaw[0] = 0, yaw[1] = y / m, yaw[2] = 0, yaw[3] = w / m;
  inv[0] = 0, inv[1] = -y / m, inv[2] = 0, inv[3] = w / m;
}

/* ... to q's forward */
static void yaw_of(const float q[4], float yaw[4], float inv[4]) {
  yaw_to(-2 * (q[0] * q[2] + q[1] * q[3]), -(1 - 2 * (q[0] * q[0] + q[1] * q[1])), yaw, inv);
}

static u64 now_us(void) { return sceKernelGetProcessTime(); }

/* ---------------------------------------------------------------- the seat */
/* Where the head was and which way it faced (yaw only) at the last reset, in VrTracker2 space,
 * and the one before it: frames drawn just before a change were counted from that one. */
static struct {
  volatile u32 ok, had;
  float pos[3];
  float yaw[4];     /* rotation about +Y taking -Z to the seat's forward */
  float yaw_inv[4]; /* its inverse: tracker -> game */
  float prev_pos[3], prev_yaw[4];
  u64 changed;
  volatile u32 gen; /* bumps on every change */
  volatile u32 want; /* a recentre asked for: the next live head pose takes the seat (vr_recenter) */
} g_seat;
static const float k_origin[3] = {0.0f, 0.0f, 1.5f}; /* declared above for vr_submit_flat */

/* The head: in VrTracker2 space as last read, and in game space as last given to the game. */
static struct {
  float pos[3], rot[4];
  float game_rot[4];
  u32 valid, game_valid;
} g_head;

/* What a console sends the game when the player resets the view (ResetVrPosition): the game then
 * counts positions from where the head is (its origin). Every seat reset owes the game one, or
 * the game's origin stays where the head was in the old seat: its calibration then
 * draws the controller's outline away from the controller. It goes out once the game has read
 * the head in the new seat. */
static volatile u32 g_reset_owed, g_reset_after, g_hmd_reads;

int vr_reset_event(int take) {
  if (!g_reset_owed || (s32)(g_hmd_reads - g_reset_after) < 0)
    return 0;
  return take ? __atomic_exchange_n(&g_reset_owed, 0, __ATOMIC_ACQ_REL) : 1;
}

/* ---- the controller ---- */
static struct {
  volatile u32 ok, realign;
  float q[4];      /* libScePad orientation, latest sample */
  float w[3];      /* its angular velocity, in the controller's frame */
  float align[4];  /* libScePad's frame -> game space (heading) */
  float anchor[3]; /* smoothed head position (game space) */
  float shown[3];  /* where it is reported, gliding to where it is held */
  u64 anchor_t, shown_t;
  u32 anchor_ok, shown_ok;
} g_pad = {.realign = 1};
/* The resting place of a controller nothing sees: 17 cm below and 50 cm in front of the head.
 * It is also where the game's calibration draws the controller's outline (calibration.xml
 * Position), counted from its origin, which a recentre puts at the head. */
extern u32 g_opt_padtest;
static const float k_pad_offset[3] = {0.0f, -0.17f, -0.50f};

extern volatile u32 g_rec_reseats;
static void seat_reset(const float p[3], const float q[4]) {
  g_rec_reseats++;
  float yaw[4], inv[4];
  yaw_of(q, yaw, inv);
  if (g_seat.had) {
    memcpy(g_seat.prev_pos, g_seat.pos, 12);
    memcpy(g_seat.prev_yaw, g_seat.yaw, 16);
    /* The controller keeps pointing where it points: its heading counts from the new straight
     * ahead, which is the way the head faced in the old seat. */
    if (g_head.game_valid && !g_pad.realign) {
      float d[4], dinv[4];
      yaw_of(g_head.game_rot, d, dinv);
      qmul(dinv, g_pad.align, g_pad.align);
      qnorm(g_pad.align);
    }
  } else {
    memcpy(g_seat.prev_pos, p, 12);
    memcpy(g_seat.prev_yaw, yaw, 16);
  }
  memcpy(g_seat.yaw, yaw, 16);
  memcpy(g_seat.yaw_inv, inv, 16);
  memcpy(g_seat.pos, p, 12);
  g_seat.changed = now_us();
  g_seat.had = 1;
  __atomic_fetch_add(&g_seat.gen, 1, __ATOMIC_RELEASE);
  __atomic_store_n(&g_seat.ok, 1, __ATOMIC_RELEASE);
  /* the controller is held in front of the new seat at once */
  g_pad.anchor_ok = g_pad.shown_ok = 0;
  g_reset_after = g_hmd_reads + 2;
  __atomic_store_n(&g_reset_owed, 1, __ATOMIC_RELEASE);
  LOG("vr: seat reset at %f %f %f, facing %f %f", p[0], p[1], p[2], -2 * (q[0] * q[2] + q[1] * q[3]),
      -(1 - 2 * (q[0] * q[0] + q[1] * q[1])));
}

static void to_game(const float seat_pos[3], const float seat_inv[4], const float p[3],
                    const float q[4], struct pose *out) {
  float d[3] = {p[0] - seat_pos[0], p[1] - seat_pos[1], p[2] - seat_pos[2]}, r[3];
  qrot(seat_inv, d, r);
  for (int i = 0; i < 3; i++)
    out->pos[i] = r[i] + k_origin[i];
  qmul(seat_inv, q, out->rot);
}

static void from_game(const float seat_pos[3], const float seat_yaw[4], const struct pose *g,
                      float p[3], float q[4]) {
  float d[3] = {g->pos[0] - k_origin[0], g->pos[1] - k_origin[1], g->pos[2] - k_origin[2]}, r[3];
  qrot(seat_yaw, d, r);
  for (int i = 0; i < 3; i++)
    p[i] = r[i] + seat_pos[i];
  qmul(seat_yaw, g->rot, q);
}

/* A render pose back to VrTracker2 space, for Hmd2. A pose the game drew a frame from just
 * before the seat changed was counted from the old seat; the head turns little in a frame, so of
 * the two seats the one that puts the pose nearer to the head now is the one it came from
 * (the seat it came from). */
void vr_game_to_tracker(const struct pose *g, float p[3], float q[4]) {
  if (!g_seat.ok) {
    memcpy(p, g->pos, 12);
    memcpy(q, g->rot, 16);
    return;
  }
  from_game(g_seat.pos, g_seat.yaw, g, p, q);
  if (!g_head.valid || now_us() - g_seat.changed >= 250000)
    return;
  float pp[3], pq[4];
  from_game(g_seat.prev_pos, g_seat.prev_yaw, g, pp, pq);
  const float *h = g_head.rot;
  float a = q[0] * h[0] + q[1] * h[1] + q[2] * h[2] + q[3] * h[3];
  float b = pq[0] * h[0] + pq[1] * h[1] + pq[2] * h[2] + pq[3] * h[3];
  if ((b < 0 ? -b : b) > (a < 0 ? -a : a)) {
    memcpy(p, pp, 12);
    memcpy(q, pq, 16);
  }
}

/* ---- the controller in the player's hands (PS VR2 hand tracking, hand.c) ----
 * The rule: both palms seen 5 to 32 cm apart hold it at their midpoint, 1.5 cm
 * up. When the palm line (left to right) is near level (its level part over 0.6 of it), it is the
 * controller's sideways axis: the heading is pulled towards facing straight out of it at 1.5/s,
 * and the middle sits 3.5 cm ahead along it (the palms close round the grips, behind it). One
 * hand alone is not used here (agent.padmode=0, the default, "in both hands"; agent.padmode=1, below
 * pad_pose2, adds one hand, prediction and the controller's own motion sensors).
 * goal: where it is held, in game space. */
static void two_hand_goal(const float l[3], const float r[3], float apart, const float q[4], float dt,
                          float goal[3]);
static void pad_account(int mode, int side, float dt);
static int pad_in_hands(const float q[4], float dt, float goal[3]) {
  static int held = -1;
  float l[3], r[3];
  int ok = g_seat.ok && hand_palm(0, l) && hand_palm(1, r);
  float apart = 0.0f;
  if (ok) {
    float d[3] = {r[0] - l[0], r[1] - l[1], r[2] - l[2]};
    apart = fsqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    ok = apart > 0.05f && apart < 0.32f;
  }
  if (ok != held) {
    static u32 logged;
    if (held >= 0 && logged++ < 20)
      LOG("vr: controller %s", ok ? "in your hands" : "back at its place (hands not seen)");
    held = ok;
  }
  if (!ok)
    return 0;
  two_hand_goal(l, r, apart, q, dt, goal);
  return 1;
}

/* both palms (tracker space) -> the controller's middle in game space, the heading pulled towards
 * the palm line (the rule above) */
static void two_hand_goal(const float l[3], const float r[3], float apart, const float q[4], float dt,
                          float goal[3]) {
  static const float id[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  struct pose gl, gr;
  to_game(g_seat.pos, g_seat.yaw_inv, l, id, &gl);
  to_game(g_seat.pos, g_seat.yaw_inv, r, id, &gr);
  float dx = gr.pos[0] - gl.pos[0], dz = gr.pos[2] - gl.pos[2];
  for (int i = 0; i < 3; i++)
    goal[i] = (gl.pos[i] + gr.pos[i]) * 0.5f;
  goal[1] += 0.015f;
  if (fsqrt(dx * dx + dz * dz) <= 0.6f * apart)
    return; /* hands one above the other: no heading from them */
  /* the heading: forward is the palm line turned 90 degrees, (dx, dz) -> (dz, -dx) */
  float want[4], want_inv[4], have[4], have_inv[4], rot[4];
  yaw_to(dz, -dx, want, want_inv);
  qmul(g_pad.align, q, rot);
  yaw_of(rot, have, have_inv);
  float delta[4], k = dt * 1.5f;
  qmul(want, have_inv, delta);
  if (delta[3] < 0)
    for (int i = 0; i < 4; i++)
      delta[i] = -delta[i];
  if (k > 1.0f)
    k = 1.0f;
  float step[4] = {delta[0] * k, delta[1] * k, delta[2] * k, 1.0f - k + delta[3] * k};
  qnorm(step);
  qmul(step, g_pad.align, g_pad.align);
  qnorm(g_pad.align);
  /* 3.5 cm ahead along the palm line's own heading (forward is -z) */
  static const float ahead[3] = {0.0f, 0.0f, -0.035f};
  float o[3];
  qrot(want, ahead, o);
  for (int i = 0; i < 3; i++)
    goal[i] += o[i];
}

/* Hands lost (out of the cameras' view, or one of them unseen a moment), the controller stays where
 * they last had it, for PAD_KEEP or as long as it is held still (barely turning), before it glides
 * to its place; seen again, it glides back into them over PAD_REGAIN rather than snapping. At the
 * edge of the cameras' view, where the palms come and go, it stays in the hands. */
#define PAD_KEEP 2000000  /* us */
#define PAD_STILL_W 0.35f /* rad/s */
#define PAD_REGAIN 150000 /* us */
static struct {
  float at[3]; /* where the hands last had it */
  u64 lost, back; /* when they were lost (0: they have it) and seen again */
  u32 had, gen;
} g_kept;

/* ---- the controller: orientation from libScePad, place in front of the player ---- */
/* The game's VR scene places the controller 1.5 times as far from its VR origin as it is (the
 * origin: the head where the game last recentred; a fixed gain, Scarlet 0x4701bc / Fortune 0x46e40c,
 * the constant 1.5 at 0x7ba1d4 / 0x7b2a34, Scarlet 1.00's at 0x7b8884, used only there), while the
 * head is placed 1:1: held 50 cm out it showed 75 cm out (the user, 2026-11-14: "close to my body, farther away in the game";
 * logs/research/scale/REPORT.md). agent.padreach=1 (the default; 0 the game's own) hands the game the
 * controller at O + (pos - O) / 1.5, so after its gain it lands where the hands are. O is the game's
 * own origin, read live (V+0x2c0: Scarlet 0x1df6640, Fortune 0x1dee520). The controller only. */
u32 g_opt_padreach = 1;
extern volatile u32 g_sysev_type, g_sysev_kept;
extern volatile u64 g_sysev_t;
__attribute__((unused)) static float fabs_(float x) { return x < 0 ? -x : x; }
static void pad_reach_undo(float pos[3]) {
  static const struct {
    u64 gain, origin;
    const char *name;
  } k[] = {{0x7ba1d4, 0x1df6640, "Scarlet 1.08"}, {0x7b2a34, 0x1dee520, "Fortune 1.19"},
           {0x7b8884, 0x1df6600, "Scarlet 1.00"}};
  static int g = -2;
  u8 *base = (u8 *)astro_hdr.game_base;
  if (!g_opt_padreach || !base || g == -1)
    return;
  if (g == -2) {
    g = -1;
    for (int i = 0; i < (int)(sizeof k / sizeof k[0]); i++)
      if (*(const float *)(base + k[i].gain) == 1.5f)
        g = i;
    LOG("vr: the game's controller reach gain %s%s%s", g < 0 ? "not found: the controller as it is" : "1.5 found (",
        g < 0 ? "" : k[g].name, g < 0 ? "" : "): undone (agent.padreach)");
    if (g < 0)
      return;
  }
  const float *o = (const float *)(base + k[g].origin);
  for (int i = 0; i < 3; i++) {
    if (!__builtin_isfinite(o[i]) || o[i] > 100.0f || o[i] < -100.0f)
      return; /* (not set yet) */
  }
  /* the game's origin watched (2026-11-14: the scenery lost after the PS5's home screen and after
   * leaving the play area): each move over 1 cm logged with the last system event the game was given
   * or kept from (hooks.c g_sysev_*) */
#ifndef RELEASE /* (debug: the release build doesn't log from this thread) */
  static float last[3];
  static int have;
  static u32 said;
  float d = have ? fabs_(o[0] - last[0]) + fabs_(o[1] - last[1]) + fabs_(o[2] - last[2]) : 1.0f;
  if (d > 0.01f) {
    if (said++ < 40)
      LOG("vr: the game's origin %s %.3f %.3f %.3f (was %.3f %.3f %.3f); the last system event 0x%x %s %u ms ago",
          have ? "moved to" : "at", (double)o[0], (double)o[1], (double)o[2], (double)last[0], (double)last[1],
          (double)last[2], g_sysev_type, g_sysev_kept ? "(kept from the game)" : "(given)",
          g_sysev_t ? (u32)((now_us() - g_sysev_t) / 1000) : 0);
    memcpy(last, o, 12), have = 1;
  }
#endif
  for (int i = 0; i < 3; i++)
    pos[i] = o[i] + (pos[i] - o[i]) / 1.5f;
}

/* ---- agent.padmode=1: the controller from two hands or one, predicted, with its own motion
 * sensors between the palm tracker's results (2026-11-14, the user: "make it track when i only
 * hold the controller with one hand ... faster/more detailed and with less delay") ----
 *  - Two palms seen 5-32 cm apart (38 cm to stay): the rule above (the middle, the heading).
 *  - One palm (the other unseen, or the two farther apart than a grip, or under 5 cm, one hand
 *    seen twice): the controller's middle at a grip offset from that palm along the controller's
 *    own axes (its orientation from its motion sensors, which is fast and sure). The offset starts
 *    at 8 cm towards the other side, 1.5 cm up and 3.5 cm ahead (a DualSense held by one grip:
 *    the same middle the two-hand rule gives for palms 16 cm apart), and is learnt while both
 *    hands hold it (each palm's place in the controller's frame, over about a second, kept within
 *    3-14 cm sideways, -6..8 cm up, -10..6 cm ahead), so that letting go with one hand doesn't
 *    move the controller.
 *  - Which hand holds it (2026-11-14 worn test: "sometimes it tracks my empty hand"): each hand
 *    gets a score, 0 holding ... 1 not, from (a) turning: the hand's frame against the
 *    controller's, both at the result's camera time (before, the controller's orientation of now
 *    against a frame 46-60 ms old: a holding hand looked 20-35 degrees "off" while turning, the
 *    worn log's "turning against the controller 25-36 deg" in both hands), compared with the
 *    controller's own turn over ~100 ms steps, weighted by that turn, over ~0.3 s (pf_turn); a
 *    still controller says nothing; (b) the hand's frame against the one learnt while it held it
 *    (both hands; or one hand that surely turned with it); (c) how far the controller would go.
 *    The one that held it stays until the other scores 0.25 better for 150 ms, or it surely
 *    turns on its own. One hand seen that isn't the one that held it is taken only if it turns
 *    with the controller, or fits the learnt grip within 12 cm of where it is, or after 0.8 s with
 *    nothing against it; else the controller is kept where it was (kept: 2 s, or while it is still).
 *  - Each palm result is filtered at the time its camera frame was taken (the result's own time
 *    stamp, else a latency measured by matching the palm's motion with the controller's, else
 *    none; pf_take: why, the numbers and a simulation there), then moved forward to the frame's
 *    display time along the filtered velocity as far as it moves: none under 3 cm/s, all from
 *    15 cm/s, to the display time (beyond now: the head's own prediction, at most agent.padlead
 *    ms). The controller's accelerometer is added only once its motion has matched the palms'
 *    (the latency match over 0.6). The prediction is kept within 10 cm.
 *  - A One Euro filter on top (53 ms still, opening with the filtered speed, to 5 ms), and at
 *    every change of rule the jump fades over ~80 ms instead of the filter lagging it.
 *  - Lost: kept where the hands had it 2 s (or while it is still), then to its place in 0.3 s.
 *  - (2026-11-14 07:23 worn test, "the single hand tracking still follows the empty hand ... if a
 *    controller is present the hand tracking follows the hand with the controller. then if there are
 *    no controllers we have to figure out which hand to follow"): held or not, from the controller's
 *    own motion (held_note, pad_held). Held: a hand is taken only once it shows it holds it (turns
 *    with it, or holds it as learnt near where it is), or is one of the two that held it; never one
 *    that can't be (moving over 30 cm/s while the controller barely moves, turning on its own, or
 *    scoring 0.9 or worse); the 0.8 s "nothing against it" is gone. Not held (put down 4 s, or none
 *    connected): one hand followed, the palm itself, the same one while seen; the other once it has
 *    been unseen 0.5 s or reaches 20 cm farther ahead for 0.5 s; unseen: kept 2 s, then to its place.
 * agent.padlog=1 logs each change of rule with each hand's score (its turn ratio over how many
 * degrees, its angle to the learnt grip, how far), and the 10 s summary (a detail line) gives the
 * still palm's noise, the controller's shimmer when still, the prediction made, and the holder's
 * changes by turning and the times the other hand was refused.
 * Everything bounded and checked: no number that isn't one reaches the game. */
volatile u32 g_opt_padmode; /* 0 (the default): between both hands; 1: the held / one-hand rule above
                               * (the settings menu's Controller) */
u32 g_opt_padlog;      /* 1 (a debug option): every change of rule logged with why */
u32 g_opt_padlead = 30; /* ms: the most the controller is predicted beyond now (0-60) */
u32 g_opt_padimu = 1;   /* 0: the controller's accelerometer not used for its place */
u32 g_opt_padheld = 20; /* the controller's motion that says it is in a hand, thousandths (held_note); 0: always held */
u32 g_opt_padheldw;     /* (a debug option, for a worn A/B) not 0: its turning alone against this, ten-thousandths of a rad/s */

int vr_pad_option(const char *key, u32 klen, u32 *v) {
  if (klen == 7 && !memcmp(key, "padmode", 7))
    g_opt_padmode = *v = *v != 0;
  else if (klen == 6 && !memcmp(key, "padlog", 6))
    g_opt_padlog = *v = *v != 0;
  else if (klen == 7 && !memcmp(key, "padlead", 7))
    g_opt_padlead = *v = *v > 60 ? 60 : *v;
  else if (klen == 6 && !memcmp(key, "padimu", 6))
    g_opt_padimu = *v = *v != 0;
  else if (klen == 7 && !memcmp(key, "padheld", 7))
    g_opt_padheld = *v = *v > 1000 ? 1000 : *v;
  else if (klen == 8 && !memcmp(key, "padheldw", 8))
    g_opt_padheldw = *v = *v > 10000 ? 10000 : *v;
  else
    return 0;
  return 1;
}

static float vlen(const float v[3]) { return fsqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
static int vfinite(const float *v, int n) {
  for (int i = 0; i < n; i++)
    if (!__builtin_isfinite(v[i]))
      return 0;
  return 1;
}
static void vclamp(float v[3], float most) {
  float l = vlen(v);
  if (!(l <= most)) /* (also a NaN) */
    for (int i = 0; i < 3; i++)
      v[i] = l > most && __builtin_isfinite(l) ? v[i] * most / l : 0.0f;
}
/* the angle between two orientations, degrees (small angles exact enough) */
static float qangle_deg(const float a[4], const float b[4]) {
  float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
  if (d < 0)
    d = -d;
  if (d > 1.0f)
    d = 1.0f;
  float s = 1.0f - d * d;
  return 2.0f * fsqrt(s > 0 ? s : 0) * 57.29578f;
}

/* -- the controller's accelerometer: a ring of its samples' linear acceleration in game space -- */
#define IMU_RING 256
static struct {
  u64 t;
  float a[3];   /* m/s^2, gravity out, game space; 0 while the sensors aren't proven */
  float vhp[3]; /* the velocity from it, high-passed at 0.16 Hz (for the latency match) */
  float q[4];   /* its orientation then (libScePad's), for which hand turns with it */
} g_imu[IMU_RING];
static volatile u32 g_imu_w;
static struct {
  float g[2][3]; /* gravity in libScePad's world, the sample turned by q (0) or by its inverse (1) */
  float res[2];  /* how far the turned samples stray from it while the controller turns, in g */
  u32 turning, have;
  s32 conv;      /* which of the two, -1 not proven */
  s64 off;       /* the pad's time stamp to process time */
  u32 off_ok;
  u64 last_t;
  float vhp[3];
} g_imuc = {.conv = -1};

/* Is the controller in a hand? (2026-11-14, the user: "we don't need a controller for this game ...
 * if a controller is present the hand tracking follows the hand with the controller. then if there
 * are no controllers we have to figure out which hand to follow"). A held controller is never quite
 * still: a hand's tremor turns it and shakes it. Its motion, |turn| (rad/s) + 2 |acceleration less
 * its 0.5 s mean| (g), smoothed over 0.25 s; held while that was above agent.padheld/1000 in the last
 * 4 s; not held after 4 s below it, or with no samples for 1 s (put down, or none connected). The 10 s
 * summary gives the motion's least and most so that the line can be set from a worn log.
 * agent.padheld=0: always held (the rule before).
 * (The worn logs, logs/research/pad/REVIEW.md: lying, the turning is 0.001-0.002 but the acceleration's
 * own noise makes the motion 0.013-0.015 and it was not seen put down in 10 s; held still in both
 * hands the motion is only 0.016-0.025. agent.padheldw=N, a debug option, tests the turning alone
 * against N/10000 rad/s instead, for an A/B in one worn run; the 10 s summary gives both.) */
static struct {
  float a_mean[3], lp, lo, hi;
  u64 last_t, move_t;
  u32 a_ok, held, changes;
  float held_s, free_s;
  /* for setting the line from a worn log (the 10 s summary): the turning part alone, and the most
   * of each 2 s (a line works if it is above a lying controller's most and below a held one's
   * least 2 s peak) */
  float lp_w, lo_w, hi_w, pk, pk_w, pk_lo, pk_lo_w;
  u64 pk_t;
} g_held = {.lo = 1e9f, .lo_w = 1e9f, .pk_lo = 1e9f, .pk_lo_w = 1e9f};
#define HELD_STILL_US 4000000
static void held_note(const float a[3], const float w[3], u64 t, float dt) {
  if (!vfinite(a, 3) || !vfinite(w, 3))
    return;
  if (!g_held.a_ok)
    memcpy(g_held.a_mean, a, 12), g_held.a_ok = 1;
  float da[3];
  for (int i = 0; i < 3; i++) {
    da[i] = a[i] - g_held.a_mean[i];
    g_held.a_mean[i] += da[i] * (dt / (0.5f + dt));
  }
  float mw = vlen(w), m = mw + 2.0f * vlen(da);
  if (m > 5.0f)
    m = 5.0f;
  if (mw > 5.0f)
    mw = 5.0f;
  g_held.lp += (m - g_held.lp) * (dt / (0.25f + dt));
  g_held.lp_w += (mw - g_held.lp_w) * (dt / (0.25f + dt));
  if (g_held.lp < g_held.lo)
    g_held.lo = g_held.lp;
  if (g_held.lp > g_held.hi)
    g_held.hi = g_held.lp;
  if (g_held.lp_w < g_held.lo_w)
    g_held.lo_w = g_held.lp_w;
  if (g_held.lp_w > g_held.hi_w)
    g_held.hi_w = g_held.lp_w;
  if (g_held.lp > g_held.pk)
    g_held.pk = g_held.lp;
  if (g_held.lp_w > g_held.pk_w)
    g_held.pk_w = g_held.lp_w;
  if (!g_held.pk_t || t < g_held.pk_t)
    g_held.pk_t = t;
  if (t - g_held.pk_t >= 2000000) {
    if (g_held.pk < g_held.pk_lo)
      g_held.pk_lo = g_held.pk;
    if (g_held.pk_w < g_held.pk_lo_w)
      g_held.pk_lo_w = g_held.pk_w;
    g_held.pk = g_held.pk_w = 0, g_held.pk_t = t;
  }
  if (g_opt_padheldw ? g_held.lp_w * 10000.0f > (float)g_opt_padheldw : g_held.lp * 1000.0f > (float)g_opt_padheld)
    g_held.move_t = t;
  g_held.last_t = t;
}
static int pad_held(u64 t) {
  /* (the two times are written on the thread that reads the controller and can be later than t,
   * taken at the top of pad_pose2: their ages signed, or one frame would say "no samples for 1 s") */
  u64 last = __atomic_load_n(&g_held.last_t, __ATOMIC_RELAXED), move = __atomic_load_n(&g_held.move_t, __ATOMIC_RELAXED);
  int live = last && (s64)(t - last) < 1000000;
  int held = !g_opt_padheld || (live && move && (s64)(t - move) < HELD_STILL_US);
  if (held != (int)g_held.held) {
    static u32 said;
    if (said++ < 40)
      LOG("pad: the controller %s (its motion %.4f, the turning alone %.4f, the line %.4f%s; %s)",
          held ? "picked up: the hand holding it followed" : "put down: one hand followed", (double)g_held.lp,
          (double)g_held.lp_w, g_opt_padheldw ? (double)g_opt_padheldw / 10000.0 : (double)g_opt_padheld / 1000.0,
          g_opt_padheldw ? " for the turning alone (agent.padheldw)" : "",
          !live ? "no samples for 1 s" : held ? "moving" : "still 4 s");
    g_held.held = held, g_held.changes++;
  }
  return held;
}

/* one ScePadData sample: acceleration (g) at +0x1c, angular velocity +0x28, its time +0x50 */
static void imu_sample(const u8 *d) {
  if (!g_opt_padmode)
    return;
  const float *a = (const float *)(d + 0x1c), *w = (const float *)(d + 0x28);
  u64 now = now_us(), ts = *(const u64 *)(d + 0x50), t = now;
  if (ts) { /* (the samples of one read come together: their own times kept apart) */
    s64 off = (s64)now - (s64)ts;
    if (!g_imuc.off_ok || off < g_imuc.off || off - g_imuc.off > 200000)
      g_imuc.off = off, g_imuc.off_ok = 1;
    else
      g_imuc.off += (off - g_imuc.off) / 256; /* (creeps up after a fast one) */
    t = (u64)((s64)ts + g_imuc.off);
    if (t > now || now - t > 100000)
      t = now;
  }
  if (g_imuc.last_t && t <= g_imuc.last_t)
    return; /* the same sample again */
  if (g_imuc.last_t && t - g_imuc.last_t > 1000000) {
    /* no sample taken for over 1 s (the menu's Controller in both hands meanwhile, or no controller):
     * its gravity and velocity taken afresh, not from then; which way q turns it stays proven */
    g_imuc.have = 0, g_imuc.last_t = 0;
    memset(g_imuc.vhp, 0, sizeof g_imuc.vhp);
  }
  float dt = g_imuc.last_t ? (float)(t - g_imuc.last_t) * 1e-6f : 0.0f;
  g_imuc.last_t = t;
  if (dt > 0.05f)
    dt = 0.05f;
  if (dt > 0)
    held_note(a, w, t, dt);
  float qc[4], aw[2][3];
  qconj(g_pad.q, qc);
  qrot(g_pad.q, a, aw[0]);
  qrot(qc, a, aw[1]);
  int turning = w[0] * w[0] + w[1] * w[1] + w[2] * w[2] > 1.0f; /* over 1 rad/s */
  float k = dt / (1.0f + dt);
  for (int c = 0; c < 2; c++) {
    if (!g_imuc.have) {
      memcpy(g_imuc.g[c], aw[c], 12);
      continue;
    }
    float r[3] = {aw[c][0] - g_imuc.g[c][0], aw[c][1] - g_imuc.g[c][1], aw[c][2] - g_imuc.g[c][2]};
    if (turning)
      g_imuc.res[c] += (vlen(r) - g_imuc.res[c]) * 0.02f;
    for (int i = 0; i < 3; i++)
      g_imuc.g[c][i] += r[i] * k;
  }
  g_imuc.have = 1;
  g_imuc.turning += turning;
  /* proven: after 100 turning samples, one way strays under 0.2 g and under half the other's,
   * and its gravity is 1 g (0.8-1.25); unproven again past 0.35 g or out of that */
  int best = g_imuc.res[0] <= g_imuc.res[1] ? 0 : 1;
  float gn = vlen(g_imuc.g[best]);
  s32 conv = g_imuc.conv;
  if (conv < 0 && g_imuc.turning >= 100 && g_imuc.res[best] < 0.2f &&
      g_imuc.res[best] * 2.0f < g_imuc.res[1 - best] && gn > 0.8f && gn < 1.25f)
    conv = best;
  else if (conv >= 0) {
    float gc = vlen(g_imuc.g[conv]);
    if (g_imuc.res[conv] > 0.35f || !(gc > 0.8f && gc < 1.25f))
      conv = -1;
  }
  if (conv != g_imuc.conv) {
    static u32 said;
    if (said++ < 10)
      LOG("vr: the controller's accelerometer %s (strays %.3f g turning%s, %.3f the other way; gravity %.3f g)",
          conv >= 0 ? "proven: used for its motion between the palm tracker's results"
                    : "no longer agrees with its orientation: not used",
          (double)g_imuc.res[conv >= 0 ? conv : best], conv == 1 ? ", turned by q's inverse" : "",
          (double)g_imuc.res[conv >= 0 ? 1 - conv : 1 - best], (double)gn);
    g_imuc.conv = conv;
  }
  u32 i = g_imu_w;
  float lin[3] = {0, 0, 0};
  if (conv >= 0 && vfinite(aw[conv], 3)) {
    float l[3];
    for (int j = 0; j < 3; j++)
      l[j] = (aw[conv][j] - g_imuc.g[conv][j]) * 9.80665f;
    qrot(g_pad.align, l, lin); /* (libScePad's heading to the game's) */
    vclamp(lin, 50.0f);
  }
  for (int j = 0; j < 3; j++)
    g_imuc.vhp[j] += (lin[j] - g_imuc.vhp[j]) * dt; /* (v' = a - v / 1 s) */
  vclamp(g_imuc.vhp, 5.0f);
  g_imu[i % IMU_RING].t = t;
  memcpy(g_imu[i % IMU_RING].a, lin, 12);
  memcpy(g_imu[i % IMU_RING].vhp, g_imuc.vhp, 12);
  memcpy(g_imu[i % IMU_RING].q, g_pad.q, 16);
  __atomic_store_n(&g_imu_w, i + 1, __ATOMIC_RELEASE);
}

/* the newest ring entry at or before t, -1 if the ring doesn't reach back that far */
static s64 imu_at(u64 t, u32 w) {
  if (!w)
    return -1;
  u32 lim = w > IMU_RING - 16 ? w - (IMU_RING - 16) : 0;
  for (u32 i = w - 1;; i--) {
    if (g_imu[i % IMU_RING].t <= t)
      return i;
    if (i == lim)
      return -1;
  }
}

/* The controller's motion by its own accelerometer from t0 to now: the velocity it gained, dv,
 * and how far it went beyond what its velocity at t0 explains, d. 0 when it can't say. */
static int imu_motion(u64 t0, u64 now, float dv[3], float d[3]) {
  memset(dv, 0, 12);
  memset(d, 0, 12);
  if (!g_opt_padimu || g_imuc.conv < 0 || now <= t0 || now - t0 > 150000)
    return 0;
  u32 w = __atomic_load_n(&g_imu_w, __ATOMIC_ACQUIRE);
  s64 first = imu_at(t0, w);
  if (first < 0)
    return 0;
  u64 tc = t0;
  for (u32 i = (u32)first; i < w; i++) {
    const float *a = g_imu[i % IMU_RING].a;
    u64 tn = i + 1 < w ? g_imu[(i + 1) % IMU_RING].t : now;
    if (tn > now)
      tn = now;
    if (tn <= tc)
      continue;
    float dt = (float)(tn - tc) * 1e-6f;
    int gap = dt > 0.05f; /* (no sample for 50 ms: no acceleration taken across it) */
    for (int k = 0; k < 3; k++) {
      float ak = gap ? 0.0f : a[k];
      d[k] += dv[k] * dt + 0.5f * ak * dt * dt;
      dv[k] += ak * dt;
    }
    tc = tn;
  }
  if (!vfinite(dv, 3) || !vfinite(d, 3)) {
    memset(dv, 0, 12);
    memset(d, 0, 12);
    return 0;
  }
  vclamp(dv, 3.0f);
  vclamp(d, 0.08f);
  return 1;
}

/* -- the palm tracker's latency, by the palm's motion against the controller's -- */
#define LAG_N 16 /* 0 to 120 ms in 8 ms steps */
static struct {
  double s[LAG_N], a[LAG_N], b;
  u32 n;
  float lp[3];
  u64 lp_t;
  s32 lat_us; /* -1 not measured */
  float match;
} g_lag = {.lat_us = -1};

static void lag_note(const struct hand_obs *o, const float vg[3]) {
  if (g_imuc.conv < 0)
    return;
  float dts = g_lag.lp_t && o->t_pub > g_lag.lp_t ? (float)(o->t_pub - g_lag.lp_t) * 1e-6f : 0.0f;
  g_lag.lp_t = o->t_pub;
  if (dts <= 0.0f || dts > 0.2f) {
    memcpy(g_lag.lp, vg, 12);
    return;
  }
  float hp[3];
  for (int k = 0; k < 3; k++) {
    g_lag.lp[k] += (vg[k] - g_lag.lp[k]) * dts / (1.0f + dts);
    hp[k] = vg[k] - g_lag.lp[k];
  }
  float b = hp[0] * hp[0] + hp[1] * hp[1] + hp[2] * hp[2];
  if (b < 0.1f * 0.1f || !(b < 100.0f))
    return;
  u32 w = __atomic_load_n(&g_imu_w, __ATOMIC_ACQUIRE);
  float vi[LAG_N][3];
  for (int L = 0; L < LAG_N; L++) {
    s64 i = imu_at(o->t_pub - (u64)L * 8000, w);
    if (i < 0)
      return;
    memcpy(vi[L], g_imu[i % IMU_RING].vhp, 12);
  }
  for (int L = 0; L < LAG_N; L++) {
    g_lag.s[L] += hp[0] * vi[L][0] + hp[1] * vi[L][1] + hp[2] * vi[L][2];
    g_lag.a[L] += vi[L][0] * vi[L][0] + vi[L][1] * vi[L][1] + vi[L][2] * vi[L][2];
  }
  g_lag.b += b, g_lag.n++;
}

/* every 10 s: the lag that matches best, taken when the match is good (velocity from the
 * samples trails the palm by about 12 ms more than the library's own) */
static void lag_window(int vel_lib) {
  if (g_lag.n >= 60) {
    int best = 0;
    double bc = -2;
    for (int L = 0; L < LAG_N; L++) {
      double c = g_lag.s[L] / (__builtin_sqrt(g_lag.a[L] * g_lag.b) + 1e-12);
      if (c > bc)
        bc = c, best = L;
    }
    g_lag.match = (float)bc;
    if (bc > 0.6 && best > 0 && best < LAG_N - 1) {
      s32 lat = best * 8000 - (vel_lib ? 0 : 12000);
      g_lag.lat_us = lat < 0 ? 0 : lat;
    }
  }
  memset(g_lag.s, 0, sizeof g_lag.s);
  memset(g_lag.a, 0, sizeof g_lag.a);
  g_lag.b = 0, g_lag.n = 0;
}

/* -- the palms, filtered (2026-11-14, the owner's worn test: "the controller is a bit jittery") --
 * The cause: each palm's velocity was the difference of two 60 Hz results (half new, half old),
 * and a still palm's few millimetres of noise make that 10-15 cm/s of noise. That opened the
 * speed gate (4-12 cm/s) on a still hand, and the place was carried along that noisy velocity for
 * the whole 84 ms (the camera's 46 ms, the result's age, 30 ms beyond now): the worn log's
 * "predicted 84 ms on average" in windows where both hands only held it. Simulated (60 Hz palms
 * with 3 mm of noise an axis, 46 ms late): 15 mm RMS off and 24 mm of frame-to-frame shimmer still,
 * against 1.8 mm and 0.3 mm now (5 mm of noise: 26 / 44 against 3.0 / 0.6).
 * Now each palm goes through two constant-velocity (alpha-beta) filters at its results' own camera
 * times: a slow one (theta 0.85: alpha 0.28, beta 0.023; its place has 0.44 of a result's noise,
 * its velocity under 1 cm/s of noise for 3 mm; it trails a change of speed by ~100 ms) and a fast
 * one (theta 0.40: alpha 0.84, beta 0.36). The fast one is taken as the palm moves, by the slow
 * one's speed, or half the fast one's beyond 10 cm/s (noise alone doesn't reach that): none under
 * 8 cm/s, all from 38 cm/s (smoothstep). The place is then predicted from its camera time to the
 * display time only as fast as it moves: no extrapolation under 3 cm/s, all of it from 15 cm/s
 * (smoothstep), so a still controller is never carried along noise. A One Euro filter last, on that
 * clean speed (3 Hz still, +50 Hz a m/s, 30 Hz at the most: 53 ms still, 12 ms at 20 cm/s, 5 ms
 * from 54 cm/s) smooths the 60 Hz steps. In the same simulation a 1 Hz circle of 10 cm (63 cm/s)
 * is placed as well as before (29 mm RMS both) and a 30 cm flick in 0.25 s is 10% further off
 * (54 against 48 mm RMS): the price of a still controller. */
#define PF_HIST 16
struct pf_rel {
  u64 t;
  float rel[4]; /* the hand's frame in the controller's (at the result's camera time) */
  float qc[4];  /* the controller's own orientation then (libScePad's) */
};
static struct {
  u32 ok;
  u64 t;                         /* the camera time of the newest result taken */
  float sx[3], sv[3], fx[3], fv[3]; /* the slow and the fast filter (tracker space) */
  float x[3], v[3], speed;       /* the two blended */
  /* which hand holds it: the hand's frame against the controller's over the last ~300 ms */
  struct pf_rel hist[PF_HIST];
  u32 hn;
  float ev_w, ev_m; /* the controller's turn (deg, weighted), and how much of it the hand missed */
  u64 ev_t;
  float rel[4];     /* the newest */
  u64 rel_t;
  float grip[4], grip_s, grip_d; /* the frame learnt while it held it, how long, the angle to it now */
  u32 grip_ok, grip_d_ok;
} g_pf[2];

static float smooth01(float x) {
  x = x < 0 ? 0 : x > 1 ? 1 : x;
  return x * x * (3 - 2 * x);
}

static void pf_step(float x[3], float v[3], const float z[3], float dt, float a, float b) {
  for (int i = 0; i < 3; i++) {
    float xp = x[i] + v[i] * dt, r = z[i] - xp;
    x[i] = xp + a * r;
    v[i] += b / dt * r;
  }
  vclamp(v, 5.0f);
}

/* the still palm's noise and the controller's shimmer, for the 10 s summary */
static struct {
  double palm2, shown2;
  u32 palm_n;
  float shown_t, still_t;
  float mean[3];
  u32 mean_ok;
} g_noise;

/* a new palm result (tracker space) at its camera time tc */
static void pf_take(int h, const float z[3], u64 tc) {
  if (!vfinite(z, 3))
    return;
  float dt = g_pf[h].ok && tc > g_pf[h].t ? (float)(tc - g_pf[h].t) * 1e-6f : 0.0f;
  if (g_pf[h].ok && tc <= g_pf[h].t + 2000)
    return; /* (the same camera frame, or one out of order) */
  float rs[3];
  for (int i = 0; i < 3; i++)
    rs[i] = z[i] - (g_pf[h].sx[i] + g_pf[h].sv[i] * dt);
  if (!g_pf[h].ok || dt > 0.1f || !(vlen(rs) < 0.25f)) { /* (a gap, or a jump no hand makes: start again) */
    memcpy(g_pf[h].sx, z, 12), memcpy(g_pf[h].fx, z, 12), memcpy(g_pf[h].x, z, 12);
    memset(g_pf[h].sv, 0, 12), memset(g_pf[h].fv, 0, 12), memset(g_pf[h].v, 0, 12);
    g_pf[h].speed = 0;
    g_pf[h].t = tc, g_pf[h].ok = 1;
    return;
  }
  /* theta 0.85 and 0.40: alpha 1 - theta^2, beta (1 - theta)^2 */
  pf_step(g_pf[h].sx, g_pf[h].sv, z, dt, 0.2775f, 0.0225f);
  pf_step(g_pf[h].fx, g_pf[h].fv, z, dt, 0.84f, 0.36f);
  float ss = vlen(g_pf[h].sv), sf = vlen(g_pf[h].fv), u = (sf - 0.10f) * 0.5f;
  float b = smooth01(((ss > u ? ss : u) - 0.08f) / 0.30f);
  for (int i = 0; i < 3; i++) {
    g_pf[h].x[i] = g_pf[h].sx[i] + (g_pf[h].fx[i] - g_pf[h].sx[i]) * b;
    g_pf[h].v[i] = g_pf[h].sv[i] + (g_pf[h].fv[i] - g_pf[h].sv[i]) * b;
  }
  g_pf[h].speed = vlen(g_pf[h].v);
  g_pf[h].t = tc;
  if (!vfinite(g_pf[h].x, 3) || !vfinite(g_pf[h].v, 3) || !vfinite(g_pf[h].sx, 3) || !vfinite(g_pf[h].fx, 3))
    g_pf[h].ok = 0; /* (never: then from the next result) */
  else if (ss < 0.03f) { /* still: the result's distance from where the slow filter expected it */
    float r2 = rs[0] * rs[0] + rs[1] * rs[1] + rs[2] * rs[2];
    if (r2 < 0.05f * 0.05f)
      g_noise.palm2 += r2, g_noise.palm_n++;
  }
}

/* the palm at time t + lead (tracker space): from its camera time along its velocity, as far as
 * it moves (none under 3 cm/s, all from 15); h: the prediction made, s */
static void pf_predict(int h, u64 t, float lead, float p[3], float *hz) {
  float age = t > g_pf[h].t ? (float)(t - g_pf[h].t) * 1e-6f : 0.0f;
  if (age > 0.15f)
    age = 0.15f;
  float g = smooth01((g_pf[h].speed - 0.03f) / 0.12f), d[3];
  for (int i = 0; i < 3; i++)
    d[i] = g_pf[h].v[i] * g * (age + lead);
  vclamp(d, 0.10f);
  for (int i = 0; i < 3; i++)
    p[i] = g_pf[h].x[i] + d[i];
  *hz = g * (age + lead);
}

/* -- the controller's orientation at a past time (the ring of its samples) -- */
static void pad_q_at(u64 t, float q[4]) {
  u32 w = __atomic_load_n(&g_imu_w, __ATOMIC_ACQUIRE);
  s64 i = imu_at(t, w);
  if (i >= 0 && vfinite(g_imu[i % IMU_RING].q, 4))
    memcpy(q, g_imu[i % IMU_RING].q, 16);
  else if (g_pad.ok)
    memcpy(q, g_pad.q, 16);
  else
    q[0] = q[1] = q[2] = 0.0f, q[3] = 1.0f; /* (no controller has answered yet) */
}

/* Which hand holds it, by turning: a new result's hand frame against the controller's at the same
 * camera time (rel). The holding hand's rel stays put while the controller turns; an empty hand's
 * rel changes by as much as the controller turned (or by its own motion). Over ~100 ms steps:
 * the controller's own turn (its gyro's orientation, exact) and how far rel moved; summed weighted
 * by the turn, decaying over 0.3 s. The hand frame's own noise (from its joints; not known before
 * a worn test) is measured: how far rel moves over a step while the controller turns under 2
 * degrees and the palm is still (the quieter hand's, 2-15 degrees: the floor). A step counts only
 * once the controller turned 2.5 floors (6 degrees at the least), and what rel moved less 1.5
 * floors is the miss. The ratio (miss / turn): about 0 holding, 0.5-1 or more not holding;
 * counted only after 15 degrees of turn (a still controller says nothing). */
#define PF_TURN_SURE 15.0f /* deg of evidence (weighted, decaying) before the ratio counts */
static float g_rel_noise[2] = {8.0f, 8.0f};
static float pf_floor(void) {
  float f = g_rel_noise[0] < g_rel_noise[1] ? g_rel_noise[0] : g_rel_noise[1];
  return !(f >= 2.0f) ? 2.0f : f > 15.0f ? 15.0f : f;
}
static void pf_turn(int h, const struct hand_obs *o, u64 tc) {
  float qc[4], R[4], Rc[4], hq[4], rel[4];
  pad_q_at(tc, qc);
  qmul(g_pad.align, qc, R);
  qconj(R, Rc);
  qmul(g_seat.yaw_inv, o->frame, hq);
  qmul(Rc, hq, rel);
  qnorm(rel);
  if (!vfinite(rel, 4))
    return;
  float dts = g_pf[h].ev_t && tc > g_pf[h].ev_t ? (float)(tc - g_pf[h].ev_t) * 1e-6f : 1.0f;
  if (dts >= 1.0f)
    g_pf[h].ev_w = g_pf[h].ev_m = 0, g_pf[h].hn = 0, g_pf[h].grip_d_ok = 0;
  else {
    float k = 0.3f / (0.3f + dts);
    g_pf[h].ev_w *= k, g_pf[h].ev_m *= k;
  }
  g_pf[h].ev_t = tc;
  /* the newest entry 80-200 ms older */
  for (u32 n = 0; n < g_pf[h].hn && n < PF_HIST; n++) {
    const struct pf_rel *e = &g_pf[h].hist[(g_pf[h].hn - 1 - n) % PF_HIST];
    if (e->t >= tc || tc - e->t < 80000)
      continue;
    if (tc - e->t > 200000)
      break;
    float tc_deg = qangle_deg(qc, e->qc), e_deg = qangle_deg(rel, e->rel), fl = pf_floor();
    if (!__builtin_isfinite(e_deg) || !__builtin_isfinite(tc_deg))
      break;
    if (tc_deg < 2.0f && g_pf[h].speed < 0.05f) {
      g_rel_noise[h] += (e_deg - g_rel_noise[h]) * 0.05f;
      if (!__builtin_isfinite(g_rel_noise[h]))
        g_rel_noise[h] = 8.0f;
    } else if (tc_deg >= (fl * 2.5f > 6.0f ? fl * 2.5f : 6.0f)) {
      float w = tc_deg > 60.0f ? 60.0f : tc_deg, m = (e_deg - 1.5f * fl) / tc_deg;
      g_pf[h].ev_w += w, g_pf[h].ev_m += w * (m < 0 ? 0 : m > 2.0f ? 2.0f : m);
    }
    break;
  }
  struct pf_rel *e = &g_pf[h].hist[g_pf[h].hn++ % PF_HIST];
  e->t = tc;
  memcpy(e->rel, rel, 16);
  memcpy(e->qc, qc, 16);
  memcpy(g_pf[h].rel, rel, 16);
  g_pf[h].rel_t = tc;
  if (g_pf[h].grip_ok) {
    float d = qangle_deg(rel, g_pf[h].grip);
    if (!g_pf[h].grip_d_ok)
      g_pf[h].grip_d = d, g_pf[h].grip_d_ok = 1;
    else
      g_pf[h].grip_d += (d - g_pf[h].grip_d) * (dts / (0.15f + dts));
  }
}

/* the holding hand's frame against the controller's, learnt (k: the share of the new one) */
static void pf_grip_learn(int h, float k, float dts) {
  float *a = g_pf[h].grip, *r = g_pf[h].rel;
  if (!g_pf[h].grip_s) {
    memcpy(a, r, 16);
  } else {
    float dot = a[0] * r[0] + a[1] * r[1] + a[2] * r[2] + a[3] * r[3], sg = dot < 0 ? -1.0f : 1.0f;
    for (int i = 0; i < 4; i++)
      a[i] += (sg * r[i] - a[i]) * k;
    qnorm(a);
  }
  if (!vfinite(a, 4)) {
    g_pf[h].grip_s = 0, g_pf[h].grip_ok = 0;
    return;
  }
  g_pf[h].grip_s += dts;
  if (g_pf[h].grip_s >= 0.5f)
    g_pf[h].grip_ok = 1;
}

/* -- the rule's state -- */
enum { PM_PLACE, PM_KEPT, PM_ONE, PM_TWO };
static const char *const k_pm_name[4] = {"to its place", "kept where the hands had it", "in one hand",
                                         "in both hands"};
static volatile s64 g_hmd_lead_us; /* the head's prediction ahead of the asking (vr_get_pose) */
static struct {
  int mode, side;            /* side: the hand holding it alone, -1 */
  int holder;                /* the last holder: 0/1, 2 both, -1 none */
  u64 chal_t, alone_t;       /* the other hand better since; the other hand alone since */
  int free_side;             /* the controller not held: the hand followed, -1 none yet */
  u64 free_lost_t, reach_t;  /* that hand unseen since; the other hand reaching farther since */
  u64 empty_t[2];            /* each hand moving fast while the controller is still, since */
  u32 empty_n, free_switch;  /* (the 10 s summary) */
  int was_held, was_refused;
  float off[2][3];           /* the middle from each palm, in the controller's own frame */
  u32 seen_n[2];
  float blend[3];            /* the jump at the last change, fading */
  float ex[3];               /* the One Euro filter */
  u32 euro_ok;
  /* the 10 s summary */
  u64 window_at;
  float in_mode[4], in_side[2];
  u32 changes, logs, refused, switched;
  double hz_sum, hz_now_sum, age_sum;
  float hz_max, moving_t;
  u32 hz_n, imu_n;
} g_pm = {.mode = -1, .side = -1, .holder = -1, .free_side = -1, .off = {{0.08f, 0.015f, -0.035f}, {-0.08f, 0.015f, -0.035f}}};

static void pad_account(int mode, int side, float dt) {
  if (mode >= 0 && mode < 4 && dt > 0 && dt < 0.2f) {
    g_pm.in_mode[mode] += dt;
    if (side == 0 || side == 1)
      g_pm.in_side[side] += dt;
  }
  u64 t = now_us();
  if (!g_pm.window_at)
    g_pm.window_at = t;
  if (t - g_pm.window_at < 10000000)
    return;
  float rate[2];
  s32 lat;
  hand_obs_figures(rate, &lat);
  struct hand_obs o;
  int vel_lib = hand_obs_get(0, &o) && o.vel_lib;
  lag_window(vel_lib);
  /* (four lines: a log line holds 510 characters, and as one line everything from the accelerometer
   * on was cut: the grips, the hand frame's noise, the controller's motion and its line) */
  LOG_DETAIL("pad: 10 s (agent.padmode=%u): both hands %.1f s, one hand %.1f s (left %.1f, right %.1f), kept "
             "%.1f s, at its place %.1f s, %u changes (%u by turning, %u times the other hand refused: kept); "
             "hand results %.1f / %.1f a second, latency %d ms by their time, %d ms by the controller's motion "
             "(match %.2f)",
             g_opt_padmode, (double)g_pm.in_mode[PM_TWO], (double)g_pm.in_mode[PM_ONE], (double)g_pm.in_side[0],
             (double)g_pm.in_side[1], (double)g_pm.in_mode[PM_KEPT], (double)g_pm.in_mode[PM_PLACE], g_pm.changes,
             g_pm.switched, g_pm.refused, (double)rate[0], (double)rate[1], lat >= 0 ? lat / 1000 : -1,
             g_lag.lat_us >= 0 ? g_lag.lat_us / 1000 : -1, (double)g_lag.match);
  LOG_DETAIL("pad: 10 s, the palms: %.1f ms old on average, predicted %.1f ms on average (%.1f at the most, "
             "%.1f of it past now; moving %.1f s); still %.1f s: palm noise %.1f mm, the controller's shimmer "
             "%.1f mm; accelerometer %s",
             g_pm.hz_n ? g_pm.age_sum / g_pm.hz_n : 0.0, g_pm.hz_n ? g_pm.hz_sum / g_pm.hz_n : 0.0,
             (double)g_pm.hz_max, g_pm.hz_n ? g_pm.hz_now_sum / g_pm.hz_n : 0.0, (double)g_pm.moving_t,
             (double)g_noise.still_t, g_noise.palm_n ? __builtin_sqrt(g_noise.palm2 / g_noise.palm_n) * 1000 : 0.0,
             g_noise.shown_t > 0 ? __builtin_sqrt(g_noise.shown2 / g_noise.shown_t) * 1000 : 0.0,
             g_imuc.conv < 0 ? "not proven (not used)"
             : !g_opt_padimu ? "proven, off (agent.padimu=0)"
             : g_lag.match < 0.6f ? "proven, not used (its motion doesn't match the palms')" : "used");
  LOG_DETAIL("pad: 10 s, the grip: from the left palm %.1f %.1f %.1f cm, from the right %.1f %.1f %.1f cm; grip "
             "frames learnt %s / %s; the hand frame's noise %.1f / %.1f deg (floor %.1f)",
             (double)(g_pm.off[0][0] * 100), (double)(g_pm.off[0][1] * 100), (double)(g_pm.off[0][2] * 100),
             (double)(g_pm.off[1][0] * 100), (double)(g_pm.off[1][1] * 100), (double)(g_pm.off[1][2] * 100),
             g_pf[0].grip_ok ? "yes" : "no", g_pf[1].grip_ok ? "yes" : "no", (double)g_rel_noise[0],
             (double)g_rel_noise[1], (double)pf_floor());
  LOG_DETAIL("pad: 10 s, the controller: held %.1f s, not %.1f s (its motion %.4f..%.4f, the turning alone "
             "%.4f..%.4f; the least of the 2 s peaks %.4f, of the turning alone %.4f; the line %.3f; %u changes), "
             "an empty hand refused %u times, the hand followed without it changed %u times",
             (double)g_held.held_s, (double)g_held.free_s, (double)(g_held.lo > 1e8f ? 0 : g_held.lo),
             (double)g_held.hi, (double)(g_held.lo_w > 1e8f ? 0 : g_held.lo_w), (double)g_held.hi_w,
             (double)(g_held.pk_lo > 1e8f ? 0 : g_held.pk_lo), (double)(g_held.pk_lo_w > 1e8f ? 0 : g_held.pk_lo_w),
             (double)g_opt_padheld / 1000.0, g_held.changes, g_pm.empty_n, g_pm.free_switch);
  g_held.held_s = g_held.free_s = 0, g_held.lo = 1e9f, g_held.hi = 0, g_held.changes = 0;
  g_held.lo_w = g_held.pk_lo = g_held.pk_lo_w = 1e9f, g_held.hi_w = 0;
  g_pm.empty_n = g_pm.free_switch = 0;
  memset(g_pm.in_mode, 0, sizeof g_pm.in_mode);
  memset(g_pm.in_side, 0, sizeof g_pm.in_side);
  g_pm.changes = g_pm.logs = g_pm.refused = g_pm.switched = 0;
  g_pm.hz_sum = g_pm.hz_now_sum = g_pm.age_sum = 0, g_pm.hz_max = 0, g_pm.hz_n = 0, g_pm.moving_t = 0;
  g_noise.palm2 = g_noise.shown2 = 0, g_noise.palm_n = 0, g_noise.shown_t = g_noise.still_t = 0;
  g_pm.window_at = t;
}

/* -- each hand's score as the holder: 0 holds it ... 1 and more doesn't -- */
struct hold_score {
  float turn, turn_w; /* the turn ratio and its evidence (deg), turn_w under PF_TURN_SURE: not known */
  float grip;         /* the angle to the learnt grip frame (deg), -1 not known */
  float near;         /* how far the controller would go (m), -1 not known */
  float s;            /* the score, -1 nothing known */
};
static void hold_score(int h, int fresh, const float P[3], const float R[4], u64 t, struct hold_score *sc) {
  sc->turn = -1, sc->turn_w = g_pf[h].ev_w, sc->grip = -1, sc->near = -1, sc->s = -1;
  if (!fresh)
    return;
  float sum = 0, w = 0;
  if (g_pf[h].ev_w >= PF_TURN_SURE) {
    sc->turn = g_pf[h].ev_m / g_pf[h].ev_w;
    float wt = g_pf[h].ev_w / 30.0f;
    wt = wt < 0.5f ? 0.5f : wt > 1.5f ? 1.5f : wt;
    sum += wt * sc->turn, w += wt;
  }
  if (g_pf[h].grip_ok && g_pf[h].grip_d_ok && t - g_pf[h].rel_t < 150000) {
    sc->grip = g_pf[h].grip_d;
    float s = (sc->grip - 15.0f) / 45.0f;
    sum += 0.6f * (s < 0 ? 0 : s > 1.5f ? 1.5f : s), w += 0.6f;
  }
  if (g_pad.shown_ok && g_pm.mode != PM_PLACE && g_pm.mode >= 0) {
    float off[3], d[3];
    qrot(R, g_pm.off[h], off);
    for (int i = 0; i < 3; i++)
      d[i] = P[i] + off[i] - g_pad.shown[i];
    sc->near = vlen(d);
    float s = (sc->near - 0.03f) / 0.15f;
    sum += 0.4f * (s < 0 ? 0 : s > 1.5f ? 1.5f : s), w += 0.4f;
  }
  if (w > 0 && __builtin_isfinite(sum))
    sc->s = sum / w;
}
/* turning on its own, surely (the ratio over 0.55 on 30 degrees of the controller's turn); turning
 * with it (under 0.25 on 15) */
static int hold_free(const struct hold_score *sc) { return sc->turn > 0.55f && sc->turn_w >= 30.0f; }
static int hold_turns(const struct hold_score *sc) { return sc->turn >= 0 && sc->turn < 0.25f; }

/* agent.padlog: where a hand was last seen, from the head (x right, y up, z ahead, cm), and its
 * score as the holder */
static int hand_where(int h, const struct hand_obs *o, int have, u64 t, const struct hold_score *sc, char *buf,
                      u32 size) {
  if (!have)
    return agent_snprintf(buf, size, "%s never seen", h ? "right" : "left");
  float v[3] = {o->pos[0] - g_head.pos[0], o->pos[1] - g_head.pos[1], o->pos[2] - g_head.pos[2]}, hc[4], r[3];
  qconj(g_head.rot, hc);
  qrot(hc, v, r);
  return agent_snprintf(buf, size,
                        "%s state %d, last seen %u ms ago %.0f cm right %.0f cm up %.0f cm ahead of the head%s, "
                        "%.0f cm/s; as the holder %.2f (turn %.2f over %.0f deg, grip %.0f deg, %.0f cm from it)",
                        h ? "right" : "left", o->state, (u32)((t - o->t_pub) / 1000), (double)(r[0] * 100),
                        (double)(r[1] * 100), (double)(-r[2] * 100), o->state != 3 ? " (the tracker lost it)" : "",
                        (double)(g_pf[h].speed * 100), (double)sc->s, (double)sc->turn, (double)sc->turn_w,
                        (double)sc->grip, (double)(sc->near * 100));
}

/* how far ahead of the head a hand is (m, along where the head faces) */
static float hand_ahead(const struct hand_obs *o) {
  float v[3] = {o->pos[0] - g_head.pos[0], o->pos[1] - g_head.pos[1], o->pos[2] - g_head.pos[2]}, hc[4], r[3];
  qconj(g_head.rot, hc);
  qrot(hc, v, r);
  return -r[2];
}

static void pad_pose2(struct tracked *out) {
  static const float k_id[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  float q[4];
  int pad_ok = g_pad.ok; /* 0: no controller has answered yet (then never held: one hand followed) */
  memcpy(q, pad_ok ? g_pad.q : k_id, 16);
  if (g_pad.realign && pad_ok) {
    float yaw[4], inv[4];
    yaw_of(q, yaw, inv);
    memcpy(g_pad.align, inv, 16);
    g_pad.realign = 0;
    LOG("vr: controller heading aligned");
  } else if (g_pad.realign) {
    memcpy(g_pad.align, k_id, 16); /* (level, facing ahead, until one answers and is aligned) */
  }
  u64 t = now_us();
  float dt = g_pad.shown_ok ? (float)(t - g_pad.shown_t) * 1e-6f : 0.0f;
  if (dt > 0.1f)
    dt = 0.1f;
  s64 lead_us = g_hmd_lead_us, cap = (s64)g_opt_padlead * 1000;
  float lead = (float)(lead_us < 0 ? 0 : lead_us > cap ? cap : lead_us) * 1e-6f;
  /* the palms: seen (state 3) with a result in the last 120 ms; each new one filtered at its
   * camera time, and its frame against the controller's then */
  static struct hand_obs o[2]; /* (kept: a read that meets the hand thread's write leaves the last) */
  int have[2], fresh[2], newer[2];
  float P[2][3], Pt[2][3], V[2][3], hz[2] = {0, 0};
  u64 tc[2] = {t, t};
  for (int h = 0; h < 2; h++) {
    have[h] = hand_obs_get(h, &o[h]) || o[h].n != 0;
    fresh[h] = g_seat.ok && have[h] && o[h].state == 3 && (s64)(t - o[h].t_pub) < 120000 &&
               (s64)(t - o[h].t_pub) > -20000; /* (taken up on the hand thread a moment ago) */
    newer[h] = fresh[h] && o[h].n != g_pm.seen_n[h];
    if (have[h])
      g_pm.seen_n[h] = o[h].n;
    if (fresh[h]) {
      u64 lat = g_lag.lat_us >= 0 ? (u64)g_lag.lat_us : 0;
      tc[h] = o[h].t_ts ? o[h].t_ts : o[h].t_pub > lat ? o[h].t_pub - lat : o[h].t_pub;
      if (tc[h] > t)
        tc[h] = t;
      if (newer[h]) {
        pf_take(h, o[h].pos, tc[h]);
        if (o[h].frame_ok)
          pf_turn(h, &o[h], tc[h]);
      }
      fresh[h] = g_pf[h].ok;
    } else if (g_pf[h].ok && t > g_pf[h].t && t - g_pf[h].t > 300000) {
      g_pf[h].ok = 0; /* (unseen: started again when seen) */
    }
    if (fresh[h]) {
      static const float id[4] = {0, 0, 0, 1};
      float pt[3];
      struct pose gp;
      pf_predict(h, t, lead, pt, &hz[h]);
      memcpy(Pt[h], pt, 12);
      to_game(g_seat.pos, g_seat.yaw_inv, pt, id, &gp);
      memcpy(P[h], gp.pos, 12);
      qrot(g_seat.yaw_inv, g_pf[h].v, V[h]);
    }
  }
  float R[4], Rc[4];
  qmul(g_pad.align, q, R);
  qconj(R, Rc);
  struct hold_score sc[2];
  for (int h = 0; h < 2; h++)
    hold_score(h, fresh[h], P[h], R, t, &sc[h]);
  /* the rule */
  int mode = PM_PLACE, side = -1, refused = 0, by_turn = 0;
  float apart = -1.0f;
  const char *why = "";
  int held = pad_held(t), free_hand = !held;
  if (dt > 0 && dt < 0.2f)
    *(held ? &g_held.held_s : &g_held.free_s) += dt;
  /* a hand that can't be holding it: moving over 30 cm/s for 150 ms while the controller barely
   * moves (its motion under 0.05), turning on its own, or scoring 0.9 or worse (its grip and place
   * far from the controller's) */
  /* (turning on its own counts against a hand only if the other one, seen and measured over the same
   * turn, does better by 0.25, the margin a challenger needs below: the worn logs' hands both on the
   * controller score 0.45-0.8 each, and with 0.56 against 0.55 one of them "couldn't be holding it":
   * both hands <-> one hand 49 times in 100 s, and with both over 0.55 "neither", kept) */
  int empty[2], fast[2], alone[2] = {hold_free(&sc[0]), hold_free(&sc[1])};
  for (int h = 0; h < 2; h++) {
    if (fresh[h] && g_pf[h].speed > 0.30f && g_held.lp < 0.05f) {
      if (!g_pm.empty_t[h])
        g_pm.empty_t[h] = t;
    } else {
      g_pm.empty_t[h] = 0;
    }
    int free_turn = alone[h] && !(fresh[1 - h] && sc[1 - h].turn >= 0 && sc[h].turn < sc[1 - h].turn + 0.25f);
    /* (its grip AND its place far: a grip over 82 degrees off alone scores 0.6 * 1.5 / 1.0, which
     * as floats is just over 0.9, and a hand 0-3 cm from its grip's place was refused for it, 3 times
     * in rec-0920 and 12 in worn-0842; a learnt grip frame can be stale) */
    int far = sc[h].s >= 0.9f && !(sc[h].near >= 0 && sc[h].near < 0.06f);
    fast[h] = fresh[h] && g_pm.empty_t[h] && t - g_pm.empty_t[h] >= 150000;
    empty[h] = fast[h] || (fresh[h] && (free_turn || far));
  }
  if (held && !g_pm.was_held)
    g_pm.holder = -1; /* (picked up: the hand holding it proven again) */
  if (held)
    g_pm.free_side = -1, g_pm.free_lost_t = g_pm.reach_t = 0;
  g_pm.was_held = held;
  int holder = g_pm.mode == PM_TWO ? 2 : g_pm.holder;
  if (!held) {
    /* no controller in a hand: one hand followed, the same one while it is seen; the other only
     * once it has been unseen 0.5 s, or the other reaches 20 cm farther ahead for 0.5 s. The first:
     * the one seen, the farther ahead of two */
    int cur = g_pm.free_side;
    if (cur >= 0 && fresh[cur]) {
      side = cur, g_pm.free_lost_t = 0;
      why = "no controller in a hand: the hand followed";
      if (fresh[1 - cur] && hand_ahead(&o[1 - cur]) > hand_ahead(&o[cur]) + 0.20f) {
        if (!g_pm.reach_t)
          g_pm.reach_t = t;
        if (t - g_pm.reach_t >= 500000)
          side = 1 - cur, g_pm.reach_t = 0, why = "no controller in a hand: the other hand reaches farther";
      } else {
        g_pm.reach_t = 0;
      }
    } else if (cur >= 0) {
      if (!g_pm.free_lost_t)
        g_pm.free_lost_t = t;
      g_pm.reach_t = 0;
      if (fresh[1 - cur] && t - g_pm.free_lost_t >= 500000)
        side = 1 - cur, g_pm.free_lost_t = 0, why = "no controller in a hand: the hand followed not seen 0.5 s";
      else
        why = "no controller in a hand: the hand followed not seen";
    } else if (fresh[0] || fresh[1]) {
      side = fresh[0] && fresh[1] ? (hand_ahead(&o[0]) >= hand_ahead(&o[1]) ? 0 : 1) : fresh[0] ? 0 : 1;
      why = "no controller in a hand: the first hand seen";
    } else {
      why = "no controller in a hand, no hand seen";
    }
    if (side >= 0) {
      if (side != cur && cur >= 0)
        g_pm.free_switch++;
      g_pm.free_side = side, mode = PM_ONE;
    }
    g_pm.chal_t = 0, g_pm.alone_t = 0;
  } else if (fresh[0] && fresh[1]) {
    float d[3] = {g_pf[1].x[0] - g_pf[0].x[0], g_pf[1].x[1] - g_pf[0].x[1], g_pf[1].x[2] - g_pf[0].x[2]};
    apart = vlen(d);
    int grip_apart = apart > 0.05f && apart < (g_pm.mode == PM_TWO ? 0.38f : 0.32f);
    /* a grip apart, but one turns with the controller and the other surely on its own: one hand */
    int one_by_turn = -1;
    if (grip_apart && hold_turns(&sc[0]) && hold_free(&sc[1]))
      one_by_turn = 0;
    else if (grip_apart && hold_turns(&sc[1]) && hold_free(&sc[0]))
      one_by_turn = 1;
    else if (grip_apart && empty[1] && !empty[0])
      one_by_turn = 0;
    else if (grip_apart && empty[0] && !empty[1])
      one_by_turn = 1;
    /* (both moving fast while the controller is still: two empty hands a grip apart, not both on it) */
    if (grip_apart && one_by_turn < 0 && !(fast[0] && fast[1])) {
      mode = PM_TWO;
    } else if (one_by_turn >= 0) {
      why = "a grip apart, but the other hand can't be holding it";
      mode = PM_ONE, side = one_by_turn, by_turn = 1;
    } else {
      why = apart <= 0.05f ? "the palms under 5 cm apart (one hand seen twice?)" : "the palms too far apart for both on it";
      int f0 = empty[0], f1 = empty[1];
      if (f0 && f1) {
        why = "neither hand can be holding it";
        refused = 1;
      } else if (holder == 0 || holder == 1) {
        /* the one that held it, until the other is better by 0.25 for 150 ms, or it surely isn't */
        int h0 = holder, h1 = 1 - holder;
        int better = sc[h1].s >= 0 && (sc[h0].s < 0 ? hold_turns(&sc[h1]) : sc[h1].s + 0.25f < sc[h0].s);
        if (!better)
          g_pm.chal_t = 0;
        else if (!g_pm.chal_t)
          g_pm.chal_t = t;
        if ((empty[h0] && !empty[h1]) || (better && !empty[h1] && t - g_pm.chal_t >= 150000)) {
          side = h1, by_turn = 1, g_pm.chal_t = 0;
          why = empty[h0] ? "the hand that held it can't be holding it" : "the other hand fits better";
        } else {
          side = h0;
        }
      } else if (f0 || f1) {
        side = f0 ? 1 : 0;
        why = "the other hand can't be holding it";
      } else {
        /* none held it yet: only a hand that shows it holds it (turns with it, or holds it as
         * learnt near where it is); else kept, as it was */
        int p0 = hold_turns(&sc[0]) || (sc[0].grip >= 0 && sc[0].grip < 35.0f && sc[0].near >= 0 && sc[0].near < 0.12f);
        int p1 = hold_turns(&sc[1]) || (sc[1].grip >= 0 && sc[1].grip < 35.0f && sc[1].near >= 0 && sc[1].near < 0.12f);
        if (p0 && p1)
          side = sc[0].s >= 0 && sc[1].s >= 0 ? (sc[0].s <= sc[1].s ? 0 : 1) : (hold_turns(&sc[0]) ? 0 : 1);
        else if (p0 || p1)
          side = p0 ? 0 : 1;
        else
          why = "neither hand shows it holds it (yet)", refused = 1;
      }
      if (side >= 0)
        mode = PM_ONE;
    }
    g_pm.alone_t = 0;
  } else if (fresh[0] || fresh[1]) {
    int h = fresh[0] ? 0 : 1;
    g_pm.chal_t = 0;
    int fits0 = sc[h].grip >= 0 && sc[h].grip < 35.0f && sc[h].near >= 0 && sc[h].near < 0.12f;
    if (holder == h || holder == 2 || (holder < 0 && (hold_turns(&sc[h]) || fits0))) {
      /* the hand that held it (or one of the two; or, none yet, one that shows it holds it): kept
       * unless it can't be holding it */
      g_pm.alone_t = 0;
      if (empty[h]) {
        why = "the one hand seen can't be holding it";
        refused = 1;
      } else {
        mode = PM_ONE, side = h;
        why = h ? "the left hand not seen" : "the right hand not seen";
      }
    } else {
      /* the other hand held it: this one only if it turns with the controller, or fits the learnt
       * grip near where it is, or after 0.8 s alone with nothing against it */
      if (!g_pm.alone_t)
        g_pm.alone_t = t;
      int fits = sc[h].turn < 0 && sc[h].grip >= 0 && sc[h].grip < 35.0f && sc[h].near >= 0 && sc[h].near < 0.12f;
      /* (2026-11-14 07:23 worn test: "still follows the empty hand": the 0.8 s "nothing against it"
       * took it; now only a hand that shows it holds the controller) */
      if (!empty[h] && (hold_turns(&sc[h]) || fits)) {
        mode = PM_ONE, side = h, by_turn = hold_turns(&sc[h]);
        why = hold_turns(&sc[h]) ? "the hand that held it not seen; this one turns with it"
                                 : "the hand that held it not seen; this one holds it as learnt";
      } else {
        why = "the hand that held it not seen; this one doesn't show it holds it (yet)";
        refused = 1;
      }
    }
  } else {
    why = "no hand seen";
    g_pm.chal_t = 0, g_pm.alone_t = 0;
  }
  if (refused && !g_pm.was_refused) /* (times, not frames) */
    g_pm.empty_n++;
  g_pm.was_refused = refused;
  /* where it is held (game space, predicted to the display time), and its velocity */
  float goal[3] = {0, 0, 0}, vel[3] = {0, 0, 0}, speed = 0;
  u64 tcap = t;
  int hands = mode == PM_TWO || mode == PM_ONE;
  if (hands) {
    float dts = dt > 0 ? dt : 0;
    if (mode == PM_TWO) {
      two_hand_goal(Pt[0], Pt[1], apart, q, dt, goal);
      qmul(g_pad.align, q, R); /* (the heading may have moved) */
      qconj(R, Rc);
      for (int i = 0; i < 3; i++)
        vel[i] = (V[0][i] + V[1][i]) * 0.5f;
      speed = g_pf[0].speed > g_pf[1].speed ? g_pf[0].speed : g_pf[1].speed;
      tcap = tc[0] / 2 + tc[1] / 2;
      g_pm.age_sum += (double)(t - tcap) * 1e-3;
      if (hz[0] > hz[1])
        hz[1] = hz[0];
      /* each palm's grip offset, and its frame against the controller's, learnt (both fresh, a
       * grip's width apart) */
      if (apart > 0.08f && apart < 0.30f && dt > 0)
        for (int h = 0; h < 2; h++) {
          float d[3] = {goal[0] - P[h][0], goal[1] - P[h][1], goal[2] - P[h][2]}, l[3];
          qrot(Rc, d, l);
          if (newer[h] && o[h].frame_ok && t - g_pf[h].rel_t < 150000)
            pf_grip_learn(h, dts / (1.0f + dts), dts);
          if (!vfinite(l, 3))
            continue;
          float k = dt / (1.0f + dt);
          for (int i = 0; i < 3; i++)
            g_pm.off[h][i] += (l[i] - g_pm.off[h][i]) * k;
          float *f = g_pm.off[h];
          float sx = h ? -1.0f : 1.0f, x = f[0] * sx;
          f[0] = sx * (x < 0.03f ? 0.03f : x > 0.14f ? 0.14f : x);
          f[1] = f[1] < -0.06f ? -0.06f : f[1] > 0.08f ? 0.08f : f[1];
          f[2] = f[2] < -0.10f ? -0.10f : f[2] > 0.06f ? 0.06f : f[2];
        }
    } else {
      float off[3] = {0, 0, 0};
      if (!free_hand)
        qrot(R, g_pm.off[side], off); /* (no controller: the palm itself) */
      for (int i = 0; i < 3; i++)
        goal[i] = P[side][i] + off[i];
      memcpy(vel, V[side], 12);
      speed = g_pf[side].speed;
      tcap = tc[side];
      g_pm.age_sum += (double)(t - tcap) * 1e-3;
      hz[1] = hz[side];
      /* one hand that surely turns with it: its frame learnt slowly (a one-hand grip) */
      if (!free_hand && newer[side] && o[side].frame_ok && sc[side].turn >= 0 && sc[side].turn < 0.15f &&
          sc[side].turn_w >= 30.0f && t - g_pf[side].rel_t < 150000)
        pf_grip_learn(side, dts / (3.0f + dts), dts);
    }
    /* the latency match: the holding hand's new velocity against the controller's */
    if (mode == PM_TWO && (newer[0] || newer[1]))
      lag_note(&o[newer[0] ? 0 : 1], vel);
    else if (mode == PM_ONE && newer[side] && !free_hand)
      lag_note(&o[side], vel);
    /* the controller's accelerometer, from the camera time to now (only once its motion has
     * matched the palms': the worn test's never did, match 0.00-0.40, and while turning it strays
     * up to 0.2 g, 7 mm over these 80 ms) */
    float dv[3], dd[3];
    int imu = !free_hand && g_lag.match >= 0.6f && imu_motion(tcap, t, dv, dd);
    if (imu) {
      float g = smooth01((speed - 0.03f) / 0.12f), d[3];
      for (int i = 0; i < 3; i++)
        d[i] = (dd[i] + dv[i] * lead) * g;
      vclamp(d, 0.05f);
      for (int i = 0; i < 3; i++)
        goal[i] += d[i];
    }
    float hms = hz[1] * 1000.0f;
    g_pm.hz_sum += hms, g_pm.hz_now_sum += lead * 1000.0f * smooth01((speed - 0.03f) / 0.12f), g_pm.hz_n++;
    if (hms > g_pm.hz_max)
      g_pm.hz_max = hms;
    if (speed > 0.15f && dt > 0)
      g_pm.moving_t += dt;
    g_pm.imu_n += imu;
    if (!vfinite(goal, 3)) { /* (never seen: then the hands aren't taken) */
      hands = 0, mode = PM_PLACE, why = "a place that isn't a number";
    }
  }
  /* lost (or the hand seen not the one holding it): kept, then to its place (the rule before) */
  if (g_kept.gen != g_seat.gen)
    g_kept.had = 0, g_kept.gen = g_seat.gen;
  if (hands) {
    if (g_kept.lost)
      g_kept.back = t;
    memcpy(g_kept.at, goal, 12);
    g_kept.had = 1, g_kept.lost = 0;
  } else if (g_kept.had) {
    float w2 = g_pad.w[0] * g_pad.w[0] + g_pad.w[1] * g_pad.w[1] + g_pad.w[2] * g_pad.w[2];
    if (!g_kept.lost || (held && w2 < PAD_STILL_W * PAD_STILL_W)) /* (no controller held: 2 s) */
      g_kept.lost = t;
    if ((t > g_kept.lost ? t - g_kept.lost : 0) < PAD_KEEP) {
      memcpy(goal, g_kept.at, 12);
      mode = PM_KEPT;
    } else {
      g_kept.had = 0;
    }
  }
  if (mode == PM_PLACE)
    for (int i = 0; i < 3; i++)
      goal[i] = g_pad.anchor[i] + k_pad_offset[i];
  if (g_opt_padtest && g_opt_padtest <= 200)
    goal[0] += (float)g_opt_padtest * 0.01f, goal[1] += (float)g_opt_padtest * 0.01f;
  /* a change of rule: said, and its jump faded */
  int changed = mode != g_pm.mode || (mode == PM_ONE && side != g_pm.side);
  if (refused && g_pm.mode != PM_KEPT && mode == PM_KEPT)
    g_pm.refused++;
  if (changed) {
    static u32 said;
    if (g_pm.mode >= 0)
      g_pm.changes++;
    if (by_turn && mode == PM_ONE)
      g_pm.switched++;
    if (g_opt_padlog && g_pm.logs++ < 60) {
      char a[300], b[300];
      hand_where(0, &o[0], have[0], t, &sc[0], a, sizeof a);
      hand_where(1, &o[1], have[1], t, &sc[1], b, sizeof b);
      LOG("pad: %s%s (%s; palms %.0f cm apart; %s; %s)", k_pm_name[mode],
          mode == PM_ONE ? (side ? " (right)" : " (left)") : "", *why ? why : "both palms a grip apart",
          (double)(apart * 100), a, b);
    } else if (!g_opt_padlog && g_pm.mode >= 0 && said++ < 30) {
      LOG("vr: controller %s%s", k_pm_name[mode], mode == PM_ONE ? (side ? " (right)" : " (left)") : "");
    }
  }
  if (!g_pad.shown_ok || !vfinite(g_pad.shown, 3)) {
    memcpy(g_pad.shown, goal, 12);
    g_pad.shown_ok = 1;
    g_pm.euro_ok = 0;
    memset(g_pm.blend, 0, 12);
  } else if (mode == PM_PLACE) {
    for (int i = 0; i < 3; i++) /* to its place in 0.3 s (a glide, not a jump) */
      g_pad.shown[i] += (goal[i] - g_pad.shown[i]) * (dt / (0.30f + dt));
    g_pm.euro_ok = 0;
    memset(g_pm.blend, 0, 12);
  } else {
    if (changed || !g_pm.euro_ok) {
      for (int i = 0; i < 3; i++)
        g_pm.blend[i] = g_pad.shown[i] - goal[i];
      vclamp(g_pm.blend, 0.5f);
      memcpy(g_pm.ex, goal, 12);
      g_pm.euro_ok = 1;
    } else if (dt > 0) {
      /* One Euro on the filters' speed (not on the place's own differences): 3 Hz still (53 ms),
       * +50 Hz a m/s, 30 Hz at the most (5 ms) */
      float f = 3.0f + 50.0f * (hands ? speed : 0.0f);
      float tau = 1.0f / (6.2831853f * (f > 30.0f ? 30.0f : f));
      for (int i = 0; i < 3; i++)
        g_pm.ex[i] += (goal[i] - g_pm.ex[i]) * (dt / (tau + dt));
    }
    float keep = dt > 0 ? 0.08f / (0.08f + dt) : 1.0f; /* the jump fades, ~80 ms */
    for (int i = 0; i < 3; i++) {
      g_pm.blend[i] *= keep;
      g_pad.shown[i] = g_pm.ex[i] + g_pm.blend[i];
    }
  }
  if (!vfinite(g_pad.shown, 3)) { /* (never: then at its place at once) */
    for (int i = 0; i < 3; i++)
      g_pad.shown[i] = g_pad.anchor[i] + k_pad_offset[i];
    g_pm.euro_ok = 0;
  }
  /* the shimmer: in the hands and still (under 2 cm/s), how far it strays from its 0.5 s mean */
  if (hands && speed < 0.02f && dt > 0 && !changed) {
    if (!g_noise.mean_ok)
      memcpy(g_noise.mean, g_pad.shown, 12), g_noise.mean_ok = 1;
    float d[3];
    for (int i = 0; i < 3; i++) {
      g_noise.mean[i] += (g_pad.shown[i] - g_noise.mean[i]) * (dt / (0.5f + dt));
      d[i] = g_pad.shown[i] - g_noise.mean[i];
    }
    float d2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
    if (d2 < 0.05f * 0.05f)
      g_noise.shown2 += d2 * dt, g_noise.shown_t += dt;
    g_noise.still_t += dt;
  } else {
    g_noise.mean_ok = 0;
  }
  g_pm.mode = mode, g_pm.side = side;
  if (mode == PM_ONE)
    g_pm.holder = side;
  else if (mode == PM_TWO)
    g_pm.holder = 2;
  else if (mode == PM_PLACE)
    g_pm.holder = -1;
  g_pad.shown_t = t;
  qmul(g_pad.align, q, out->pose.rot);
  qrot(out->pose.rot, g_pad.w, out->ang_vel);
  memcpy(out->pose.pos, g_pad.shown, 12);
  pad_reach_undo(out->pose.pos);
  pad_account(mode, side, dt);
}

static void pad_pose(struct tracked *out) {
  if (g_opt_padmode || !g_pad.ok) { /* (or the option turned off this instant with no controller yet) */
    pad_pose2(out);
    return;
  }
  float q[4];
  memcpy(q, g_pad.q, 16);
  if (g_pad.realign) {
    /* straight ahead is where it points now */
    float yaw[4], inv[4];
    yaw_of(q, yaw, inv);
    memcpy(g_pad.align, inv, 16);
    g_pad.realign = 0;
    LOG("vr: controller heading aligned");
  }
  u64 t = now_us();
  float dt = g_pad.shown_ok ? (float)(t - g_pad.shown_t) * 1e-6f : 0.0f, goal[3];
  int hands = pad_in_hands(q, dt, goal), kept = 0;
  if (g_kept.gen != g_seat.gen) /* a place counted from the seat before */
    g_kept.had = 0, g_kept.gen = g_seat.gen;
  if (hands) {
    if (g_kept.lost)
      g_kept.back = t;
    memcpy(g_kept.at, goal, 12);
    g_kept.had = 1, g_kept.lost = 0;
  } else if (g_kept.had) {
    float w2 = g_pad.w[0] * g_pad.w[0] + g_pad.w[1] * g_pad.w[1] + g_pad.w[2] * g_pad.w[2];
    if (!g_kept.lost || w2 < PAD_STILL_W * PAD_STILL_W)
      g_kept.lost = t; /* held still: kept there */
    if ((t > g_kept.lost ? t - g_kept.lost : 0) < PAD_KEEP) {
      memcpy(goal, g_kept.at, 12);
      kept = 1;
    } else {
      g_kept.had = 0;
      static u32 logged;
      if (logged++ < 20)
        LOG("vr: controller unseen %u s: to its place", PAD_KEEP / 1000000);
    }
  }
  qmul(g_pad.align, q, out->pose.rot);
  qrot(out->pose.rot, g_pad.w, out->ang_vel);
  /* Unseen, it hangs off the head's position, not its turn (hands do not swing round when the
   * player looks about), at the standard place; and glides there rather than jumping
   * (a glide, not a jump). */
  if (!hands && !kept)
    for (int i = 0; i < 3; i++)
      goal[i] = g_pad.anchor[i] + k_pad_offset[i];
  /* agent.padtest=N (a debug tool, live): N cm to the right and N cm up, to see whether the game
   * uses the controller's position at all (hand tracking only ever moves that) */
  if (g_opt_padtest && g_opt_padtest <= 200)
    goal[0] += (float)g_opt_padtest * 0.01f, goal[1] += (float)g_opt_padtest * 0.01f;
  if (!g_pad.shown_ok) {
    memcpy(g_pad.shown, goal, 12);
    g_pad.shown_ok = 1;
  } else {
    /* just back in the hands 80 ms, kept 35 ms (it doesn't move), to its place 0.3 s; in the
     * hands as a One Euro filter (Casiez, Roussel, Vogel 2012): 35 ms while it is still or slow,
     * less as it moves faster (to 8 ms), so a move trails it less and a still one doesn't jitter;
     * its speed taken over about 80 ms (2 Hz), the palms' jitter barely counting */
    static float e_dx[3];
    int euro = hands && (t > g_kept.back ? t - g_kept.back : 0) >= PAD_REGAIN;
    float tc = !hands && !kept ? 0.30f : hands ? 0.08f : 0.035f;
    for (int i = 0; i < 3; i++) {
      if (euro && dt > 0) {
        float raw = (goal[i] - g_pad.shown[i]) / dt;
        e_dx[i] += (raw - e_dx[i]) * (dt / (dt + 1.0f / (6.2831853f * 2.0f)));
        float f = 4.55f + 25.0f * (e_dx[i] < 0 ? -e_dx[i] : e_dx[i]); /* Hz: 35 ms still */
        tc = 1.0f / (6.2831853f * (f > 20.0f ? 20.0f : f));
      } else {
        e_dx[i] = 0;
      }
      g_pad.shown[i] += (goal[i] - g_pad.shown[i]) * (dt / (tc + dt));
    }
  }
  g_pad.shown_t = t;
  memcpy(out->pose.pos, g_pad.shown, 12);
  pad_reach_undo(out->pose.pos);
  pad_account(hands ? 3 : kept ? 1 : 0, -1, dt); /* (the 10 s summary only) */
}

static void pad_follow_head(const struct pose *head) {
  u64 now = now_us();
  if (!g_pad.anchor_ok) {
    memcpy(g_pad.anchor, head->pos, 12);
    g_pad.anchor_ok = 1;
  } else {
    float dt = (float)(now - g_pad.anchor_t) * 1e-6f;
    float a = dt / (0.25f + dt);
    for (int i = 0; i < 3; i++)
      g_pad.anchor[i] += (head->pos[i] - g_pad.anchor[i]) * a;
  }
  g_pad.anchor_t = now;
}

/* ---------------------------------------------------------------- the controller's buttons */
/* What the player does with the controller about the view itself (the view gestures),
 * which a console's system would see to:
 *  - with agent.recentre=1, the first press of circle takes the seat where the player is (they
 *    have settled by then); off by default;
 *  - holding OPTIONS for a second resets the view, and the controller's heading with it.
 * Every button reaches the game. */
#define PAD_OPTIONS 0x0008
#define PAD_CIRCLE 0x2000

static struct {
  u32 prev, circle_seen, options_fired;
  u64 options_since;
} g_btn;

void vr_pad_read(u8 *d) {
  u32 b = *(u32 *)d;
  u64 now = now_us();
  int finite = 1; /* orientation, acceleration and turn (0x0c..0x34): all numbers, or none taken */
  for (int i = 0; i < 10; i++)
    finite &= __builtin_isfinite(((const float *)(d + 0x0c))[i]);
  if (d[0x4c] && finite) {
    memcpy(g_pad.q, d + 0x0c, 16);
    memcpy(g_pad.w, d + 0x28, 12);
    g_pad.ok = 1;
    imu_sample(d); /* (agent.padmode=1: its acceleration, for its motion between hand results) */
  }
  u32 pressed = b & ~g_btn.prev;
  if (g_opt_recentre && (pressed & PAD_CIRCLE) && !g_btn.circle_seen) {
    g_btn.circle_seen = 1;
    vr_request_recenter("first press of circle (agent.recentre)");
  }
  if (!(b & PAD_OPTIONS))
    g_btn.options_since = 0;
  else if (!g_btn.options_since)
    g_btn.options_since = now, g_btn.options_fired = 0;
  else if (!g_btn.options_fired && now - g_btn.options_since >= 1000000) {
    g_btn.options_fired = 1;
    vr_reset_view("OPTIONS held");
  }
  g_btn.prev = b;
}

/* the view reset (OPTIONS held for a second; the settings menu's Reset view): the seat from the
 * head at the next pose, the controller's heading counted from the new straight ahead */
void vr_reset_view(const char *why) {
  g_btn.circle_seen = 1;
  vr_request_recenter(why);
  g_pad.realign = 1;
}

static void set_pose(struct pose *p, float x, float y, float z) {
  p->pos[0] = x;
  p->pos[1] = y;
  p->pos[2] = z;
  p->rot[0] = p->rot[1] = p->rot[2] = 0.0f;
  p->rot[3] = 1.0f;
}

/* Eye = device pose moved half the IPD along its own x axis. */
static void eye_pose(const struct pose *dev, float dx, struct pose *eye) {
  float v[3] = {dx, 0, 0}, r[3];
  qrot(dev->rot, v, r);
  for (int i = 0; i < 3; i++)
    eye->pos[i] = dev->pos[i] + r[i];
  memcpy(eye->rot, dev->rot, 16);
}

/* How fast the head moves and turns, from one pose to the next: the
 * game takes a head that pushes into something for a push (a headbutt) only when it comes at
 * some speed. Game space; starts again whenever the seat changes. */
static struct {
  float pos[3], rot[4], v[3], w[3];
  u64 t;
  u32 gen, ok;
} g_vel;

static void head_velocity(const struct pose *p, u64 t, float v[3], float w[3]) {
  u32 gen = g_seat.gen;
  if (!g_vel.ok || g_vel.gen != gen) {
    memset(&g_vel, 0, sizeof g_vel);
    g_vel.ok = 1, g_vel.gen = gen;
  } else {
    float dt = (float)(s64)(t - g_vel.t) * 1e-6f;
    if (dt < 0.002f && dt > -0.002f)
      goto out; /* the same moment again */
    if (dt > 0.1f || dt < 0) {
      memset(g_vel.v, 0, 12);
      memset(g_vel.w, 0, 12);
    } else {
      /* the turn over dt, in game space: q_now q_prev* = (sin(a/2) n, cos(a/2)); small angles */
      float c[4], dq[4];
      qconj(g_vel.rot, c);
      qmul(p->rot, c, dq);
      float sgn = dq[3] < 0 ? -1.0f : 1.0f;
      for (int i = 0; i < 3; i++) {
        g_vel.v[i] += ((p->pos[i] - g_vel.pos[i]) / dt - g_vel.v[i]) * 0.5f;
        g_vel.w[i] += (2.0f * sgn * dq[i] / dt - g_vel.w[i]) * 0.5f;
      }
    }
  }
  memcpy(g_vel.pos, p->pos, 12);
  memcpy(g_vel.rot, p->rot, 16);
  g_vel.t = t;
out:
  memcpy(v, g_vel.v, 12);
  memcpy(w, g_vel.w, 12);
}

/* The head pose's motion between consecutive asks, in degrees: of the pose given to the game
 * (predicted as far as asked, or capped: agent.predict) and of the tracker's unpredicted pose.
 * With the headset still, both are noise; the predicted one shows what the horizon amplifies. */
static u64 g_jit_pred_sum, g_jit_raw_sum; /* micro-degrees */
static u32 g_jit_n, g_jit_pred_max, g_jit_raw_max;
static u32 quat_angle_udeg(const float *a, const float *b) {
  float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
  if (d < 0)
    d = -d;
  if (d > 1.0f)
    d = 1.0f;
  float s = 1.0f - d * d; /* sin^2(theta/2) */
  float th = 2.0f * fsqrt(s > 0 ? s : 0) * 57.29578f; /* small angles: theta = 2 sin(theta/2) */
  return (u32)(th * 1000000.0f);
}
volatile u32 g_rec_pose_max, g_rec_reseats; /* the flight recorder (hooks.c): the largest per-ask
                                             * step of the pose given to the game, re-seats */
static void jitter_note(const float *pred_q, const float *raw_q) {
  static float lp[4], lr[4];
  static int have;
  if (have) {
    u32 a = quat_angle_udeg(pred_q, lp), b = quat_angle_udeg(raw_q, lr);
    g_jit_pred_sum += a, g_jit_raw_sum += b, g_jit_n++;
    if (a > g_jit_pred_max)
      g_jit_pred_max = a;
    if (a > g_rec_pose_max)
      g_rec_pose_max = a;
    if (b > g_jit_raw_max)
      g_jit_raw_max = b;
  }
  memcpy(lp, pred_q, 16);
  memcpy(lr, raw_q, 16);
  have = 1;
}
void vr_jitter_report(void) {
  if (!g_jit_n)
    return;
  u32 p = (u32)(g_jit_pred_sum / g_jit_n), r = (u32)(g_jit_raw_sum / g_jit_n);
  LOG_DETAIL("pose: moved %u.%03u deg an ask on average as predicted (%u.%03u at the most), %u.%03u raw "
      "(%u.%03u at the most), %u asks",
      p / 1000000, p / 1000 % 1000, g_jit_pred_max / 1000000, g_jit_pred_max / 1000 % 1000,
      r / 1000000, r / 1000 % 1000, g_jit_raw_max / 1000000, g_jit_raw_max / 1000 % 1000, g_jit_n);
  g_jit_pred_sum = g_jit_raw_sum = 0, g_jit_n = 0, g_jit_pred_max = g_jit_raw_max = 0;
}

/* A head pose that is not a number (never seen from VrTracker2) never reaches the game: the
 * tracker's pose is checked as it comes (a NaN there would also pass the "tracker jumped" test,
 * both comparisons false, and be kept as g_head and the seat), and the game's after it is placed;
 * either one not finite, the last finite one in its place. A finite pose is passed as it is: no
 * change to the prediction, filtering, timing or values. Logged at most once in 5 s, with a count. */
static struct {
  float p[3], q[4];        /* the tracker's last finite pose */
  struct pose game;        /* the last finite pose given to the game */
  int have, game_have;
  u32 n;                   /* poses replaced, all session */
  u64 log_at;
} g_head_ok;
static void head_nonfinite(const char *what, const float *p, const float *q) {
  g_head_ok.n++;
  u64 now = now_us();
  if (now < g_head_ok.log_at)
    return;
  g_head_ok.log_at = now + 5000000;
  LOG("vr: the %s head pose not finite (%f %f %f q %f %f %f %f): the last finite one in its place "
      "(%u so far)", what, p[0], p[1], p[2], q[0], q[1], q[2], q[3], g_head_ok.n);
}

void vr_get_pose(int hmd, u64 prediction_time, struct tracked *out) {
  memset(out, 0, sizeof *out);
  if (!hmd) {
    /* (agent.padmode=1: also before any controller answers, or with none: a hand is followed) */
    if ((g_pad.ok || g_opt_padmode) && g_seat.ok && g_pad.anchor_ok)
      pad_pose(out);
    else
      set_pose(&out->pose, k_origin[0] + k_pad_offset[0], k_origin[1] + k_pad_offset[1],
               k_origin[2] + k_pad_offset[2]);
    return;
  }
  float p[3], q[4];
  int tracked = vr2_head_pose(prediction_time, p, q);
  if (tracked && !(vfinite(p, 3) && vfinite(q, 4))) {
    head_nonfinite("tracker's", p, q);
    if (g_head_ok.have)
      memcpy(p, g_head_ok.p, 12), memcpy(q, g_head_ok.q, 16);
    else
      tracked = 0; /* none yet: as with no pose (the seated default below) */
  } else if (tracked) {
    memcpy(g_head_ok.p, p, 12), memcpy(g_head_ok.q, q, 16);
    g_head_ok.have = 1;
  }
  if (tracked) {
    /* The tracker starts in a provisional frame (IMU only) and moves to its map frame when it
     * relocalises: a jump no head makes within 100 ms re-seats the player. */
    static u64 lt;
    u64 now = now_us();
    if (g_seat.ok && lt && now - lt < 100000) {
      const float *lp = g_head.pos, *lq = g_head.rot;
      float dx = p[0] - lp[0], dy = p[1] - lp[1], dz = p[2] - lp[2];
      float dot = q[0] * lq[0] + q[1] * lq[1] + q[2] * lq[2] + q[3] * lq[3];
      if (dot < 0)
        dot = -dot;
      if (dx * dx + dy * dy + dz * dz > 0.25f * 0.25f || dot < 0.966f) {
        LOG("vr: tracker jumped (%f m, cos %f): re-seat", fsqrt(dx * dx + dy * dy + dz * dz), dot);
        vr_recenter();
      }
    }
    memcpy(g_head.pos, p, 12);
    memcpy(g_head.rot, q, 16);
    g_head.valid = 1;
    lt = now;
    u32 want = __atomic_exchange_n(&g_seat.want, 0, __ATOMIC_ACQ_REL);
    if (!g_seat.ok || want)
      seat_reset(p, q);
    to_game(g_seat.pos, g_seat.yaw_inv, p, q, &out->pose);
    if (g_test & (TEST_RECORD | TEST_REPLAY))
      test_head(&out->pose);
    if (!(vfinite(out->pose.pos, 3) && vfinite(out->pose.rot, 4))) {
      head_nonfinite("game's", out->pose.pos, out->pose.rot);
      if (g_head_ok.game_have)
        out->pose = g_head_ok.game;
      else
        set_pose(&out->pose, 0.0f, 0.0f, 1.5f); /* (none yet: the seated default, as below) */
    } else {
      g_head_ok.game = out->pose;
      g_head_ok.game_have = 1;
    }
    if (prediction_time) { /* (how far ahead the frame is shown: the controller predicted as far; read only) */
      s64 lead = (s64)prediction_time - (s64)now;
      if (lead >= 0 && lead <= 100000)
        g_hmd_lead_us = lead;
    }
    head_velocity(&out->pose, prediction_time ? prediction_time : now, out->vel, out->ang_vel);
    memcpy(g_head.game_rot, out->pose.rot, 16);
    g_head.game_valid = 1;
    if (prediction_time) { /* the raw pose beside the predicted one, for the jitter figures */
      float rp[3], rq[4];
      if (vr2_head_pose(0, rp, rq))
        jitter_note(q, rq);
    }
    static u32 n;
    if (n++ % 600 == 0)
      LOG_DETAIL("vr: head tracker %f %f %f q %f %f %f %f -> game %f %f %f q %f %f %f %f, %f m/s",
          p[0], p[1], p[2], q[0], q[1], q[2], q[3], out->pose.pos[0], out->pose.pos[1],
          out->pose.pos[2], out->pose.rot[0], out->pose.rot[1], out->pose.rot[2],
          out->pose.rot[3],
          fsqrt(out->vel[0] * out->vel[0] + out->vel[1] * out->vel[1] + out->vel[2] * out->vel[2]));
  } else {
    /* Seated 1.5 m in front of the camera, facing it (-z). */
    set_pose(&out->pose, 0.0f, 0.0f, 1.5f);
    if (g_test & TEST_REPLAY)
      test_head(&out->pose);
  }
  out->head = out->pose;
  __atomic_fetch_add(&g_hmd_reads, 1, __ATOMIC_RELAXED);
  pad_follow_head(&out->pose);
  eye_pose(&out->pose, -g_vr.ipd * 0.5f, &out->eye[0]);
  eye_pose(&out->pose, g_vr.ipd * 0.5f, &out->eye[1]);
}

/* Test mode (test.txt, pads.c): the head's pose as the game gets it, recorded to the log as
 * `head:` lines (ms of process time, position, rotation; one at most every 50 ms, when it has
 * moved), or taken from head.txt (the same lines, read into g_head_script by the injector),
 * since the game's menus are chosen by looking at them. */
struct head_step {
  u32 t_ms;
  float pos[3], rot[4];
};
_Static_assert(sizeof(struct head_step) == 32, "payload/main.c fills it as 32 bytes a step");
struct head_step g_head_script[TEST_HEAD_STEPS];
u32 g_head_script_n;

void test_head(struct pose *p) {
  u64 now = now_us();
  u32 t = (u32)(now / 1000);
  if (g_test & TEST_RECORD) {
    static struct head_step last;
    static u64 next;
    if (now < next)
      return;
    float dx = p->pos[0] - last.pos[0], dy = p->pos[1] - last.pos[1], dz = p->pos[2] - last.pos[2];
    float dot = p->rot[0] * last.rot[0] + p->rot[1] * last.rot[1] + p->rot[2] * last.rot[2] +
                p->rot[3] * last.rot[3];
    if (dx * dx + dy * dy + dz * dz < 0.01f * 0.01f && (dot > 0.99995f || dot < -0.99995f))
      return;
    last.t_ms = t;
    memcpy(last.pos, p->pos, 12);
    memcpy(last.rot, p->rot, 16);
    next = now + 50000;
    LOG("head: %u %f %f %f %f %f %f %f", t, p->pos[0], p->pos[1], p->pos[2], p->rot[0], p->rot[1],
        p->rot[2], p->rot[3]);
    return;
  }
  static u32 i;
  while (i < g_head_script_n && g_head_script[i].t_ms <= t)
    i++;
  if (!i)
    return;
  const struct head_step *a = &g_head_script[i - 1];
  memcpy(p->pos, a->pos, 12);
  memcpy(p->rot, a->rot, 16);
  if (i < g_head_script_n) {
    /* between two samples (50 ms apart at most): the pose moved on, else the view turns in
     * steps (seen on the TV mirror, which shows the frames as drawn) */
    const struct head_step *b = &g_head_script[i];
    float f = b->t_ms > a->t_ms ? (float)(t - a->t_ms) / (float)(b->t_ms - a->t_ms) : 0.0f;
    if (f > 1.0f)
      f = 1.0f;
    float dot = a->rot[0] * b->rot[0] + a->rot[1] * b->rot[1] + a->rot[2] * b->rot[2] +
                a->rot[3] * b->rot[3];
    float sgn = dot < 0.0f ? -1.0f : 1.0f, n2 = 0.0f;
    for (int k = 0; k < 3; k++)
      p->pos[k] += (b->pos[k] - a->pos[k]) * f;
    for (int k = 0; k < 4; k++) {
      p->rot[k] += (sgn * b->rot[k] - a->rot[k]) * f;
      n2 += p->rot[k] * p->rot[k];
    }
    if (n2 > 0.0f) {
      float inv = 1.0f / fsqrt(n2);
      for (int k = 0; k < 4; k++)
        p->rot[k] *= inv;
    }
  }
}

/* The seat follows the head again at the next live pose; the game's origin follows it there
 * (ResetVrPosition, above). The old seat stays in force until then: cleared at once (as it was),
 * a frame handed over in that moment went to PS VR2 with its game-space pose taken for a tracker
 * one, and the view jolted for a frame at every recentre. */
void vr_recenter(void) { __atomic_store_n(&g_seat.want, 1, __ATOMIC_RELEASE); }

/* (height.c, agent.realheight: read only) */
int vr_tracker_head_y(float *y) {
  if (!g_head.valid)
    return 0;
  *y = g_head.pos[1];
  return 1;
}

/* (height.c: read only) the seat's height (VrTracker2 y of the head at the last seat reset) and its
 * generation (bumps on every reset); 0: no seat yet */
u32 vr_seat_y(float *y) {
  u32 g;
  do {
    g = __atomic_load_n(&g_seat.gen, __ATOMIC_ACQUIRE);
    *y = g_seat.pos[1];
  } while (g != __atomic_load_n(&g_seat.gen, __ATOMIC_ACQUIRE));
  return __atomic_load_n(&g_seat.ok, __ATOMIC_ACQUIRE) ? g : 0;
}

void vr_request_recenter(const char *why) {
  LOG("vr: recentre (%s)", why);
  vr_recenter();
}
