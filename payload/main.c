/* DOAX3VR2: puts the agent into Dead or Alive Xtreme 3 (Fortune and Scarlet, the PS4 PS VR
 * games) and stays resident as its controller.
 *
 * 1. Builds, on the first run, the PS VR2 libraries and the GCN reprojection shader from this
 *    console's own firmware (prepare.c).
 * 2. Waits for the game, then maps the agent (agent/build/agent.elf, embedded) into it with
 *    ptrace, applies its R_X86_64_RELATIVE relocations and resolves its imports by NID in the
 *    game's own modules.
 * 3. Reads the eboot's import relocations from the kernel's dynlib object (dynsec +0x28 symtab,
 *    +0x38 strtab, +0x48 jmprel) and points the GOT slot of every hooked NID at the agent,
 *    saving the old value in the hook's orig.
 * 4. Detaches, then relays the agent's log to klog and /data/doax3vr2/agent.log, serves the
 *    agent's requests, and re-points GOT slots that a module load re-bound.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <elf.h>
#include <stdarg.h>
#include <stddef.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/uio.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <dirent.h>
#include <time.h>
#include <machine/reg.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <ps5/kernel.h>
#include <ps5/klog.h>

#include "pt.h"
#include "../common/test.h"
#include "../common/camera.h"
#include "../common/dump.h"
#include "msgbuf.h"
#include "camparse.h"

#define OUT_DIR "/data/doax3vr2"
/* The games: Dead or Alive Xtreme 3 Fortune (CUSA04555, 1.19) and Scarlet (CUSA14173, 1.08, and
 * CUSA14831, 1.00), by the title id and the version of their param.sfo (APP_VER), each confirmed
 * by its code (build_by_code). The agent hooks their imports by NID, which all share; the payload's own
 * patches (the eye size, the cut features, the VR Passport, the shop's) are at each build's own
 * addresses. Another version or title is refused, with a notification. */
struct game_build {
  const char *title_id, *version, *name;
  const char *key; /* its own settings: cvars-<key>.txt, resolution-<key>.txt (game_file) */
  /* the build's code (build_code_is): three stretches unique to it, each a call to an import
   * (rel32: the site's and the stub's addresses both) or the one instruction the two games' VR
   * setup differs by */
  struct {
    uint32_t at;
    uint8_t len, bytes[13];
  } sig[3];
  /* the eye views' size: two float pairs in the game's constants, {PS4 Pro, PS4} widths and
   * heights (940.8 and 672, 1052.8 and 752), read once at its boot (patch_eye_size) */
  uint32_t eye_w_at, eye_h_at;
};
static const struct game_build k_builds[] = {
    /* HP0106-CUSA04555_00-DOAX300000000000, APP_VER 01.19 (VERSION 01.00): PS VR came with its
     * updates. The calls to sceHmdReprojectionStart and sceHmdInitialize315, and mov [rip+..], 0.25f */
    {"CUSA04555", "01.19", "Fortune", "fortune",
     {{0x77b794, 12, {0xe8, 0x67, 0x19, 0x00, 0x00, 0x85, 0xc0, 0x74, 0x1a, 0x48, 0x8b, 0x84}},
      {0x548578, 13, {0xe8, 0x23, 0x4b, 0x23, 0x00, 0x85, 0xc0, 0x0f, 0x85, 0x45, 0x06, 0x00, 0x00}},
      {0x547e17, 10, {0xc7, 0x05, 0x5b, 0x1d, 0x70, 0x00, 0x00, 0x00, 0x80, 0x3e}}},
     0x7b4988, 0x7b4990},
    /* JP0106-CUSA14173_00-ISLANDGAMEAPP000, APP_VER 01.08 (VERSION 01.00): Fortune's VR code,
     * instruction for instruction, at its own addresses; its mov is 0.9f */
    {"CUSA14173", "01.08", "Scarlet", "scarlet",
     {{0x782f74, 12, {0xe8, 0x87, 0x19, 0x00, 0x00, 0x85, 0xc0, 0x74, 0x1a, 0x48, 0x8b, 0x84}},
      {0x549188, 13, {0xe8, 0x13, 0xb7, 0x23, 0x00, 0x85, 0xc0, 0x0f, 0x85, 0x45, 0x06, 0x00, 0x00}},
      {0x548a27, 10, {0xc7, 0x05, 0x6b, 0x92, 0x70, 0x00, 0x66, 0x66, 0x66, 0x3f}}},
     0x7bc0d0, 0x7bc0d8},
    /* CUSA14831, the Asian release, APP_VER 01.00 with no update (untested on a console): the same
     * code as 1.08 at its own addresses, without the updates' data (some features are off for it).
     * Scarlet's settings files */
    {"CUSA14831", "01.00", "Scarlet", "scarlet",
     {{0x7815d4, 12, {0xe8, 0x87, 0x19, 0x00, 0x00, 0x85, 0xc0, 0x74, 0x1a, 0x48, 0x8b, 0x84}},
      {0x5476c8, 13, {0xe8, 0x33, 0xb8, 0x23, 0x00, 0x85, 0xc0, 0x0f, 0x85, 0x45, 0x06, 0x00, 0x00}},
      {0x546f67, 10, {0xc7, 0x05, 0xeb, 0xac, 0x70, 0x00, 0x66, 0x66, 0x66, 0x3f}}},
     0x7ba778, 0x7ba780},
};
#define N_BUILDS (int)(sizeof k_builds / sizeof k_builds[0])
#define BUILDS_TEXT "Fortune (CUSA04555) 1.19, Scarlet (CUSA14173) 1.08 and Scarlet (CUSA14831) 1.00"
static int g_build = -1; /* the game's build: an index into k_builds, -1 before it is found */
static int title_known(const char *tid) {
  for (int i = 0; i < N_BUILDS; i++)
    if (strncmp(tid, k_builds[i].title_id, 9) == 0)
      return 1;
  return 0;
}
static int build_index(const char *tid, const char *ver) {
  for (int i = 0; i < N_BUILDS; i++)
    if (strncmp(tid, k_builds[i].title_id, 9) == 0 && strcmp(ver, k_builds[i].version) == 0)
      return i;
  return -1;
}
#define GAME_NAME (g_build < 0 ? "?" : k_builds[g_build].name)
/* A game's own settings, read after the shared ones (theirs win): OUT_DIR/<what>-<key>.txt, e.g.
 * cvars-scarlet.txt, resolution-fortune.txt; NULL before the game is known */
static const char *game_file(char *buf, size_t n, const char *what) {
  if (g_build < 0)
    return NULL;
  snprintf(buf, n, OUT_DIR "/%s-%s.txt", what, k_builds[g_build].key);
  return buf;
}
#define TITLE_NAME "Dead or Alive Xtreme 3"
#define TITLE_WORD "dead or alive xtreme 3" /* in the title of any version or region of it */
#include "prepare.h"
static struct gcn_file g_gcn;
static unsigned g_gcn_code[GCN_MAX_DWORDS];
static const struct astro_firmware *g_fw;
#define PAGE 0x4000
#define LOG_SIZE (256 * 1024)
#define AGENT_MAGIC 0x32524f5254534100ull

extern const uint8_t agent_elf[];
extern const uint8_t agent_elf_end[];
/* The agent image that is injected: OUT_DIR/agent.elf when there is one (an upload by
 * tools/testrun.sh or by hand: a new agent without a payload swap, the step that has brought
 * the console down three times; not in a release build), else the copy built in. Read again
 * before every injection. */
static const uint8_t *g_agent = agent_elf;
static size_t g_agent_size;
void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static long agent_header_size(const uint8_t *elf, size_t n);
/* a debugging switch file in OUT_DIR (trap_only, watch): there, and not a release build */
static int debug_file(const char *name, struct stat *st) {
#ifdef RELEASE
  (void)name, (void)st;
  return 0;
#else
  char path[96];
  snprintf(path, sizeof path, OUT_DIR "/%s", name);
  return stat(path, st) == 0;
#endif
}

static void choose_agent(void) {
  static uint8_t *file;
  static size_t file_size;
  g_agent = agent_elf;
  g_agent_size = (size_t)(agent_elf_end - agent_elf);
#ifdef RELEASE
  return; /* a release build: always its own agent (an agent.elf there is a test upload) */
#endif
  FILE *f = fopen(OUT_DIR "/agent.elf", "rb");
  if (!f)
    return;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n < 4096 || n > (4 << 20)) {
    fclose(f);
    say("agent.elf: %ld bytes: not an agent, the built-in one used", n);
    return;
  }
  if (!file || file_size < (size_t)n) {
    free(file);
    file = malloc((size_t)n);
    file_size = file ? (size_t)n : 0;
  }
  size_t got = file ? fread(file, 1, (size_t)n, f) : 0;
  fclose(f);
  if (got != (size_t)n || memcmp(file, "\x7f" "ELF", 4) != 0) {
    say("agent.elf: unreadable or not an ELF: the built-in agent used");
    return;
  }
  /* one built with another header (an upload from an older build): the payload would write
   * past its header into the agent */
  long hs = agent_header_size(file, (size_t)n);
  if (hs != agent_header_size(agent_elf, (size_t)(agent_elf_end - agent_elf))) {
    say("agent.elf: its header is %ld bytes, this payload's %ld: the built-in agent used", hs,
        agent_header_size(agent_elf, (size_t)(agent_elf_end - agent_elf)));
    return;
  }
  g_agent = file;
  g_agent_size = (size_t)n;
  say("agent: " OUT_DIR "/agent.elf, %ld bytes (the built-in one is %ld)", n,
      (long)(agent_elf_end - agent_elf));
}
__asm__(".section .rodata\n.global agent_elf\n.global agent_elf_end\n.balign 16\n"
        "agent_elf:\n.incbin \"../agent/build/agent.elf\"\nagent_elf_end:\n.previous\n");

/* Layouts as in agent/agent.h, with game pointers as u64. */
struct a_import {
  uint64_t module, nid, slot;
};
struct a_hook {
  uint64_t nid, hook, orig, slot;
};
struct a_header {
  uint64_t magic;
  uint64_t imports;
  uint32_t nimports, reserved0; /* (reserved: 0) */
  uint64_t hooks;
  uint32_t nhooks, pad1;
  uint64_t shared;
  uint64_t boot;
  uint64_t vr_state; /* AGENT_MAGIC (2) and later */
  uint32_t vr_persist, reserved1; /* (reserved: 0) */
  uint64_t unbound_stub;
  uint64_t gcn; /* struct agent_gcn: the GCN reprojection shader goes here */
  uint32_t ipd_um, firmware;
  uint64_t game_base;
  uint32_t frame_pace, refresh_hz;
  uint32_t reserved2[8]; /* (reserved: 0; the header's size and the fields' places are kept) */
};
struct a_shared_head {
  uint64_t log_wpos;
  uint32_t log_lock, boot_done, cmd, cmd_seq;
  uint64_t cmd_arg[4];
  uint32_t cmd_ack, repatch;
};
_Static_assert(sizeof(struct a_header) == 144, "agent header ABI");
_Static_assert(offsetof(struct a_header, firmware) == 92, "agent firmware field ABI");
_Static_assert(offsetof(struct a_header, reserved0) == 20, "agent header ABI (reserved0)");
_Static_assert(offsetof(struct a_header, reserved1) == 68, "agent header ABI (reserved1)");
_Static_assert(offsetof(struct a_header, frame_pace) == 104, "agent frame pace field ABI");
_Static_assert(offsetof(struct a_header, reserved2) == 112, "agent header ABI (reserved2)");

/* The size of an agent image's header (its astro_hdr symbol), -1 when it can't be told. */
static long agent_header_size(const uint8_t *elf, size_t n) {
  const Elf64_Ehdr *eh = (const Elf64_Ehdr *)elf;
  if (n < sizeof *eh || eh->e_shentsize != sizeof(Elf64_Shdr) || eh->e_shoff > n ||
      (uint64_t)eh->e_shnum * sizeof(Elf64_Shdr) > n - eh->e_shoff)
    return -1;
  const Elf64_Shdr *sh = (const Elf64_Shdr *)(elf + eh->e_shoff);
  for (int i = 0; i < eh->e_shnum; i++) {
    if (sh[i].sh_type != SHT_SYMTAB || sh[i].sh_link >= eh->e_shnum)
      continue;
    const Elf64_Shdr *st = &sh[sh[i].sh_link];
    if (sh[i].sh_offset > n || sh[i].sh_size > n - sh[i].sh_offset || st->sh_offset > n ||
        st->sh_size > n - st->sh_offset)
      return -1;
    const Elf64_Sym *sy = (const Elf64_Sym *)(elf + sh[i].sh_offset);
    const char *str = (const char *)(elf + st->sh_offset);
    for (size_t j = 0; j < sh[i].sh_size / sizeof *sy; j++)
      if (sy[j].st_name < st->sh_size && st->sh_size - sy[j].st_name >= sizeof "astro_hdr" &&
          memcmp(str + sy[j].st_name, "astro_hdr", sizeof "astro_hdr") == 0)
        return (long)sy[j].st_size;
  }
  return -1;
}

static uint64_t g_base;    /* agent base in the game */
static uint8_t *g_mirror;  /* local copy of the agent image */
static size_t g_size;
static FILE *g_logf;
static uint64_t g_hdroff;
/* The log to the network (port 9025, debug builds): every line the payload writes (its own, the
 * agent's, the kernel log's) also sent to a client connected there, as it is written. With
 * agent.hands=1 or agent.netlog=1 (read at each game start) the console's storage gets none of
 * it while the game runs: a crash then can neither lose the last lines (they are on the client
 * already) nor be caught writing (the first worn hand-tracking run crashed the console,
 * 2026-11-12, and the user asked not to risk the filesystem). */
#define NETLOG_PORT 9025
static volatile int g_netfd = -1, g_netonly;
static pthread_mutex_t g_netlock = PTHREAD_MUTEX_INITIALIZER;
static void netlog(const void *b, size_t n) {
  if (g_netfd < 0)
    return;
  pthread_mutex_lock(&g_netlock);
  int fd = g_netfd;
  for (size_t off = 0; fd >= 0 && off < n;) {
    ssize_t w = send(fd, (const char *)b + off, n - off, MSG_NOSIGNAL);
    if (w <= 0) {
      close(fd);
      g_netfd = fd = -1;
      break;
    }
    off += (size_t)w;
  }
  pthread_mutex_unlock(&g_netlock);
}
static void log_synced(FILE *f) {
  fflush(f);
}
static int log_to_storage(void) {
  return !g_netonly;
}

#define G2M(a) (g_mirror + ((uint64_t)(a) - g_base))

void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void say(const char *fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  printf("doax3vr2: %s\n", buf);
  char line[540];
  int ln = snprintf(line, sizeof line, "doax3vr2: %s\n", buf);
  netlog(line, ln > 0 && ln < (int)sizeof line ? (size_t)ln : strlen(line));
  if (g_logf && log_to_storage()) {
    fputs(line, g_logf);
    log_synced(g_logf);
  }
}

/* The kernel's log, two ways:
 *  - /dev/klog, read by a thread into OUT_DIR/klog.txt (not in a release build): the kernel says
 *    there why a process died (a signal, a failed SELF, a loader error), which the agent's own log
 *    cannot once the game is gone. Each read takes what was written since the last, from whoever
 *    reads: with klogsrv on the console too the two split the lines (the switch-repro logs), so
 *    nothing here depends on it, and a release build never opens it (a user's own klogsrv keeps
 *    every line).
 *  - The kernel message buffer (sysctl kern.msgbuf, consumed by nobody), polled by msgbuf_thread
 *    every 200 ms in every build and parsed, line by line, into the headset cameras' state
 *    (common/camera.h) for the palm tracker's gate: who has the image stream, the system's camera
 *    job, the safety notice, a play-area setup, the tracker relocalising, the headset put on; the
 *    times of the last of each (CLOCK_MONOTONIC). The new text of a snapshot is what follows the
 *    previous one's anchor (payload/msgbuf.h); an anchor not found (the first poll, the buffer
 *    overwritten faster than the polls) parses the whole snapshot and marks lines possibly missed.
 *    publish_cam_state() writes the state into the agent (serve loop). Whether kern.msgbuf can be
 *    read here and carries the userland lines (<118>[XFD]...) is said in the log at the start
 *    (the first console check of the gate). */
#define KLOG_MAX (8 << 20) /* bytes in klog.txt before it becomes klog.prev.txt */
static struct cam_state g_cam = {.version = CAM_STATE_VERSION, .size = sizeof(struct cam_state),
                                 .img_pri = CAM_PRI_NONE, .mount = 0xff};
/* (instance: set at the start, below) */
static pthread_mutex_t g_cam_lock = PTHREAD_MUTEX_INITIALIZER;

static uint64_t mono_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}

/* the complete lines of (buf, n) (a line of 512 bytes or more is cut there) */
/* lost: the lines come after a loss (the levels were set unknown; they are new text still, read
 * once, but a new game's stream line among them is not taken as the start's) */
static void cam_parse(const char *buf, size_t n, uint64_t now, int lost) {
  char line[512];
  size_t at = 0;
  uint64_t k = 0; /* each line a microsecond later than the one before: their order kept (and
                   * never past the next poll, 200 ms on: 100 ms at most) */
  pthread_mutex_lock(&g_cam_lock);
  for (size_t i = 0; i < n; i++) {
    char ch = buf[i];
    if (ch == '\n') {
      line[at] = 0;
      if (at && !strstr(line, MSGBUF_MARK))
        cam_parse_line(&g_cam, line, now + (k < 100000 ? k++ : k), now, !lost);
      at = 0;
    } else if (at < sizeof line - 1) {
      line[at++] = ch;
    }
  }
  pthread_mutex_unlock(&g_cam_lock);
}

#define MSGBUF_POLL_US 200000
#define MSGBUF_MARK_US 1000000 /* a marker line a second the whole time a game is served */
static volatile pid_t g_live_pid;  /* the game being served (below) */
static volatile unsigned g_msgbuf_gen; /* +1 each game served: the reading starts over */
static volatile int g_adopt_history;   /* the game adopted from an earlier payload: its first
                                        * snapshot gives the LEVELS from the buffer's history,
                                        * once (the system's state had no loss), and they are
                                        * kept through the polls up to the first marker of ours
                                        * (no anchor yet: "lost" for the reading, not for them) */
static void *msgbuf_thread(void *arg) {
  (void)arg;
  struct msgbuf_track tr;
  memset(&tr, 0, sizeof tr);
  size_t cap = 0;
  char *buf = NULL;
  unsigned said_fail = 0, said_miss = 0, said_seen = 0, mark_seq = 0, gen = 0, adopted_levels = 0;
  uint64_t mark_at = 0;
  unsigned nonce = (unsigned)(mono_us() * 2654435761u) ^ (unsigned)getpid();
  snprintf(tr.nonce, sizeof tr.nonce, "%08x", nonce);
  for (;;) {
    size_t n = 0;
    uint64_t now = mono_us();
    /* the whole time a game is served (the level states stay current for the gate, and the
     * palm tracker's start, whenever it comes), idle otherwise (no polls, no scans); with each
     * game the tracking starts over (g_msgbuf_gen), a new nonce: an earlier game's markers are
     * not this one's */
    if (!g_live_pid) {
      usleep(MSGBUF_POLL_US * 2);
      continue;
    }
    if (gen != g_msgbuf_gen) {
      gen = g_msgbuf_gen;
      nonce = (unsigned)(mono_us() * 2654435761u) ^ (unsigned)getpid() ^ (gen * 0x9e3779b9u);
      memset(&tr, 0, sizeof tr);
      snprintf(tr.nonce, sizeof tr.nonce, "%08x", nonce);
      mark_seq = 0, mark_at = 0;
      said_miss = said_seen = 0, adopted_levels = 0;
    }
    /* our marker in the kernel log: klog_printf, the SDK's way into it (the same as klogsrv's
     * "<118>[klogsrv.elf] ..." lines) */
    if (now - mark_at >= MSGBUF_MARK_US) {
      mark_seq++;
      klog_printf(MSGBUF_MARK "%u %08x\n", mark_seq, nonce);
      mark_at = now;
      pthread_mutex_lock(&g_cam_lock);
      g_cam.mark_written_seq = mark_seq;
      g_cam.mark_written_at = now;
      pthread_mutex_unlock(&g_cam_lock);
    }
    int rc = sysctlbyname("kern.msgbuf", NULL, &n, NULL, 0);
    if (rc == 0 && n > cap) {
      char *b = realloc(buf, n + 65536);
      if (b)
        buf = b, cap = n + 65536;
    }
    if (rc == 0 && buf) {
      n = cap;
      rc = sysctlbyname("kern.msgbuf", buf, &n, NULL, 0);
    }
    if (rc != 0 || !buf) {
      pthread_mutex_lock(&g_cam_lock);
      g_cam.fails++;
      g_cam.missed_at = now; /* a gap in the reading: lines possibly missed */
      pthread_mutex_unlock(&g_cam_lock);
      if (said_fail++ < 3)
        say("msgbuf: kern.msgbuf not read (rc %d, errno %d, %zu bytes): the palm tracker's gate stays shut", rc, errno, n);
      usleep(MSGBUF_POLL_US * 5);
      continue;
    }
    size_t from, to;
    int lost = msgbuf_delta(&tr, buf, n, &from, &to);
    size_t from0 = from; /* (what the reading accounts for, whoever parses it) */
    if (lost) { /* the levels not known from here: only the lines after re-establish them (an
                 * adopted game's history levels kept until the reading is anchored: those polls
                 * read nothing, and the first marker's text follows what the history covered) */
      pthread_mutex_lock(&g_cam_lock);
      if (!adopted_levels)
        cam_levels_unknown(&g_cam, now);
      if (tr.mlen) /* anchored now (the first marker, or again after a loss): the history is over */
        adopted_levels = 0;
      if (g_adopt_history) { /* an adopted game, once: the levels from the whole buffer */
        struct cam_state parsed = g_cam;
        char line[512];
        size_t at = 0;
        for (size_t i = 0; i < to; i++) {
          if (buf[i] == '\n') {
            line[at] = 0;
            if (at && !strstr(line, MSGBUF_MARK))
              cam_parse_line(&parsed, line, now, now, 0);
            at = 0;
          } else if (at < sizeof line - 1) {
            line[at++] = buf[i];
          }
        }
        cam_levels_from_history(&g_cam, &parsed, now);
        g_adopt_history = 0;
        adopted_levels = !tr.mlen; /* (kept until the first marker of ours anchors the reading) */
        from = to; /* (read once: nothing of it again below) */
        say("msgbuf: an adopted game: the system's state from the buffer's history (camera job %u, "
            "stream %s mode %u)", g_cam.xfd, g_cam.img_known ? "known" : "unknown", g_cam.img_mode);
      }
      pthread_mutex_unlock(&g_cam_lock);
    }
    cam_parse(buf + from, to - from, now, lost);
    unsigned newest = 0;
    msgbuf_advance(&tr, buf, from0, to, &newest); /* (the adoption's history read counts) */
    pthread_mutex_lock(&g_cam_lock);
    g_cam.feed_at = now;
    g_cam.msgbuf_size = (uint32_t)n;
    g_cam.polls++;
    if (lost) {
      g_cam.misses++;
      g_cam.missed_at = now;
    }
    if (newest > g_cam.mark_seen_seq && gen == g_msgbuf_gen) /* (not an earlier game's marker) */
      g_cam.mark_seen_seq = newest, g_cam.mark_seen_at = now;
    unsigned polls = g_cam.polls, lines = g_cam.lines, userland = g_cam.userland;
    pthread_mutex_unlock(&g_cam_lock);
    if (polls == 1) {
      /* the first console check of the gate: the buffer's size, its lines, the last three */
      say("msgbuf: kern.msgbuf %zu bytes, %u lines parsed (the text after this instance's first "
          "marker), userland lines %s", n, lines,
          userland ? "yes" : lines ? "NO (the gate stays shut while none is seen)"
                                   : "none yet (the gate waits for the system's lines)");
      size_t e = to;
      for (int k = 0; k < 3 && e; k++) {
        size_t b = e - 1;
        while (b && buf[b - 1] != '\n')
          b--;
        say("msgbuf: last line %d: %.*s", k, (int)(e - 1 - b > 120 ? 120 : e - 1 - b), buf + b);
        e = b;
      }
    } else if (lost && said_miss++ < 5) {
      say("msgbuf: lines possibly missed (%s; %zu bytes): the gate shut 15 s, the system's state "
          "unknown until its lines", tr.mlen ? "anchored again on the newest marker" : "no marker in the buffer", n);
    }
    if (newest && said_seen < 3 && newest > said_seen) {
      /* the marker seen: the end-to-end proof (the first three, with the lag) */
      say("msgbuf: marker %u seen, %u ms after writing it", newest,
          (unsigned)((now - mark_at) / 1000));
      said_seen = newest;
    }
    usleep(MSGBUF_POLL_US);
  }
  return NULL;
}

static void msgbuf_start(void) {
  g_cam.instance = mono_us() * 6364136223846793005ull + (uint64_t)getpid();
  pthread_t t;
  if (pthread_create(&t, NULL, msgbuf_thread, NULL) == 0)
    pthread_detach(t);
}

#ifndef RELEASE
static void *klog_thread(void *arg) {
  (void)arg;
  int fd = open("/dev/klog", O_RDONLY);
  if (fd < 0) {
    say("klog: /dev/klog not readable (errno %d)", errno);
    return NULL;
  }
  /* A file per payload start, the one before kept as klog.prev.txt (a crash's kernel log is read
   * after the reboot, when the payload has started again), and the same at KLOG_MAX: it was
   * appended to for good before (1.8 MB in a day, written to the console's storage all along). */
  rename(OUT_DIR "/klog.txt", OUT_DIR "/klog.prev.txt");
  FILE *f = fopen(OUT_DIR "/klog.txt", "w");
  if (!f) {
    close(fd);
    return NULL;
  }
  static char buf[65536];
  for (;;) {
    ssize_t n = read(fd, buf, sizeof buf);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      say("klog: read failed (errno %d)", errno);
      break;
    }
    if (n == 0) {
      usleep(200000);
      continue;
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    netlog(buf, (size_t)n);
    if (!log_to_storage()) /* (nor the time line: stdio would write it out when its buffer fills) */
      continue;
    fprintf(f, "--- %ld.%03ld\n", (long)ts.tv_sec, (long)(ts.tv_nsec / 1000000));
    fwrite(buf, 1, (size_t)n, f);
    log_synced(f);
    if (ftell(f) > KLOG_MAX) {
      fclose(f);
      rename(OUT_DIR "/klog.txt", OUT_DIR "/klog.prev.txt");
      if (!(f = fopen(OUT_DIR "/klog.txt", "w")))
        break;
    }
  }
  if (f)
    fclose(f);
  close(fd);
  return NULL;
}

static void klog_start(void) {
  pthread_t t;
  if (pthread_create(&t, NULL, klog_thread, NULL) == 0)
    pthread_detach(t);
}
#endif

/* The camera state into the agent's shared block (after its log ring), with the time of the
 * write: the agent takes it as alive while `now` keeps moving. Every 100 ms of the serve loop,
 * once the agent has put the block's version and size there (its boot): an older agent, with
 * nothing after its log ring, is never written over. The agent reads it as a seqlock (below). */
static void publish_cam_state(pid_t pid, uint64_t shared, int *ok) {
  struct cam_state c;
  if (*ok <= 0) {
    uint32_t vs[2] = {0, 0};
    if (*ok < -600)
      return;
    if (kernel_proc_copyout(pid, shared + 0x40 + LOG_SIZE, vs, sizeof vs) != 0)
      return; /* (not read now: tried again, not counted) */
    if (vs[0] != CAM_STATE_VERSION || vs[1] != sizeof c) {
      if (--*ok == -600) /* a minute */
        say("cameras: the agent has no camera-state block (version %u, size %u): the gate stays shut", vs[0], vs[1]);
      return;
    }
    *ok = 1;
    say("cameras: the agent takes the kernel log's camera state (version %u)", vs[0]);
  }
  pthread_mutex_lock(&g_cam_lock);
  g_cam.seq += 2; /* even: the block settled */
  g_cam.now = mono_us();
  c = g_cam;
  pthread_mutex_unlock(&g_cam_lock);
  /* a seqlock in three copyins: seq odd, the body (seq odd in it), seq even; the agent takes a
   * copy only with the same even seq before and after it. Each copyin is a kernel call that
   * completes before the next begins, which orders the three writes as the agent needs (no
   * fence of our own could reach the game's memory) */
  uint64_t odd = c.seq - 1, even = c.seq;
  uint64_t at = shared + 0x40 + LOG_SIZE;
  c.seq = odd;
  kernel_proc_copyin(pid, &odd, at + offsetof(struct cam_state, seq), sizeof odd);
  kernel_proc_copyin(pid, &c, at, sizeof c);
  kernel_proc_copyin(pid, &even, at + offsetof(struct cam_state, seq), sizeof even);
}

/* A PS5 notification, so a problem shows on screen and not only in agent.log. */
typedef struct {
  char unused[45];
  char message[3075];
} notify_request_t;
int sceKernelSendNotificationRequest(int device, notify_request_t *req, size_t size, int blocking);

static void notify(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void notify(const char *fmt, ...) {
  static notify_request_t req;
  memset(&req, 0, sizeof req);
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(req.message, sizeof req.message, fmt, ap);
  va_end(ap);
  say("notify: %s", req.message);
  sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
}

/* The agent loads the PS5 libraries from inside the game's sandbox (/doax3vr2/... there):
 * the game can't see /data, and giving it the real root let its level loader fail /app0 reads.
 * OUT_DIR/libs is mounted there read-only (nullfs) while they load. 1.0.0 and 1.0.1 copied them
 * into the sandbox and deleted them after: that writes into the /mnt tmpfs every sandbox shares,
 * and loading from it could panic the kernel ("recursing on non recursive lockmgr tmpfs"). */
static const char *const g_libs[] = {"libSceAgcDriver.sprx", "libSceAgc.sprx", "libSceHmd2.sprx",
                                     "libSceVrTracker2.sprx", "libSceVrHand.sprx"};
/* The game's sandbox is /mnt/sandbox/<title id>_NNN, usually _000; the newest one is the
 * running game's. */
static char g_sandbox_libs[96];

static void find_sandbox(void) {
  const char *tid = k_builds[g_build < 0 ? 0 : g_build].title_id;
  snprintf(g_sandbox_libs, sizeof g_sandbox_libs, "/mnt/sandbox/%s_000/doax3vr2", tid);
  DIR *d = opendir("/mnt/sandbox");
  struct dirent *e;
  time_t best = 0;
  if (!d)
    return;
  while ((e = readdir(d)) != NULL) {
    char path[96];
    struct stat st;
    if (strncmp(e->d_name, tid, 9) != 0 || e->d_name[9] != '_' || strlen(e->d_name) != 13)
      continue;
    snprintf(path, sizeof path, "/mnt/sandbox/%s", e->d_name);
    if (stat(path, &st) == 0 && st.st_mtime >= best) {
      best = st.st_mtime;
      snprintf(g_sandbox_libs, sizeof g_sandbox_libs, "%s/doax3vr2", path);
    }
  }
  closedir(d);
}
#define SANDBOX_LIBS g_sandbox_libs

static int g_libs_mounted;
static char g_mounted_at[96]; /* where they are mounted (SANDBOX_LIBS follows the game served) */

static void unmount_libs(int force);
static int mount_libs(void) {
  if (g_libs_mounted && strcmp(g_mounted_at, SANDBOX_LIBS) == 0)
    return 0;
  if (g_libs_mounted) { /* kept from a game before (an unmount that failed): that game is gone */
    unmount_libs(1);
    if (g_libs_mounted)
      return -1;
  }
  if (mkdir(SANDBOX_LIBS, 0755) != 0 && errno != EEXIST) {
    say("libs: mkdir %s: errno %d", SANDBOX_LIBS, errno);
    return -1;
  }
  const char *target = OUT_DIR "/libs";
  struct iovec iov[] = {
      {(void *)"fstype", 7},  {(void *)"nullfs", 7},
      {(void *)"fspath", 7},  {(void *)SANDBOX_LIBS, strlen(SANDBOX_LIBS) + 1},
      {(void *)"target", 7},  {(void *)target, strlen(target) + 1},
  };
  if (nmount(iov, 6, MNT_RDONLY) != 0) {
    say("libs: nullfs mount of %s on %s failed: errno %d", target, SANDBOX_LIBS, errno);
    return -1;
  }
  g_libs_mounted = 1;
  snprintf(g_mounted_at, sizeof g_mounted_at, "%s", SANDBOX_LIBS);
  say("libs: %s mounted read-only on %s", target, SANDBOX_LIBS);
  return 0;
}

/* force: the game is gone (nothing maps the libraries any more) */
static void unmount_libs(int force) {
  if (!g_libs_mounted)
    return;
  if (unmount(g_mounted_at, force ? MNT_FORCE : 0) == 0) {
    g_libs_mounted = 0;
    rmdir(g_mounted_at);
    say("libs: unmounted from %s", g_mounted_at);
  } else {
    say("libs: unmount %s%s: errno %d (kept)", g_mounted_at, force ? " (forced)" : "", errno);
  }
}

/* the agent's stage request: the library is there already through the mount (delete: nothing to
 * do) */
static void stage_lib(uint64_t i, uint64_t del) {
  if (i >= sizeof g_libs / sizeof g_libs[0] || del)
    return;
  if (mount_libs() != 0)
    return;
  char path[160];
  struct stat st;
  snprintf(path, sizeof path, "%s/%s", SANDBOX_LIBS, g_libs[i]);
  if (stat(path, &st) == 0)
    say("stage: %s (%ld bytes, through the mount)", path, (long)st.st_size);
  else
    say("stage: %s not visible: errno %d", path, errno);
}

/* ---- the eboot's import table, from the kernel's dynlib object ---- */
struct import_slot {
  char nid[16];
  uint64_t slot;
};
static struct import_slot *g_slots;
static int g_nslots;

/* eboot is handle 0. Some launch environments randomize its image base (observed
 * at 0x43be8000 on 11.60); use the loader's mapbase for code, GOT and entry alike. */
static uint64_t eboot_mapbase(pid_t pid) {
  intptr_t proc = kernel_get_proc(pid);
  uint64_t obj = 0;
  if (!proc || kernel_copyout(proc + 0x3e8, &obj, 8) != 0)
    return 0;
  for (int i = 0; obj && i < 300; ++i) {
    uint64_t o[8];
    if (kernel_copyout(obj, o, sizeof o) != 0)
      return 0;
    if (o[0x28 / 8] == 0) {
      uint64_t base = o[0x30 / 8];
      return base >= 0x10000 && (base >> 47) == 0 ? base : 0;
    }
    obj = o[0];
  }
  return 0;
}

static int read_eboot_imports(pid_t pid) {
  intptr_t proc = kernel_get_proc(pid);
  uint64_t obj = 0;
  if (!proc || kernel_copyout(proc + 0x3e8, &obj, 8) != 0)
    return -1;
  for (int i = 0; obj && i < 300; i++) {
    uint8_t o[0x180];
    if (kernel_copyout(obj, o, sizeof o) != 0)
      return -1;
    if (*(uint64_t *)&o[0x28] == 0) { /* handle 0: eboot */
      uint64_t dynsec = *(uint64_t *)&o[0x148];
      uint64_t s[0x60 / 8];
      if (kernel_copyout(dynsec, s, sizeof s) != 0)
        return -1;
      uint64_t symtab = s[5], symsz = s[6], strtab = s[7], strsz = s[8], jmprel = s[9],
               jmpsz = s[10];
      uint8_t *sym = malloc(symsz), *str = malloc(strsz + 1), *jr = malloc(jmpsz);
      if (!sym || !str || !jr || kernel_copyout(symtab, sym, symsz) ||
          kernel_copyout(strtab, str, strsz) || kernel_copyout(jmprel, jr, jmpsz)) {
        free(sym);
        free(str);
        free(jr);
        return -1;
      }
      str[strsz] = 0;
      g_nslots = (int)(jmpsz / 24);
      free(g_slots);
      g_slots = calloc(g_nslots ? g_nslots : 1, sizeof *g_slots);
      if (!g_slots)
        g_nslots = 0;
      for (int k = 0; k < g_nslots; k++) {
        Elf64_Rela *r = (Elf64_Rela *)(jr + k * 24);
        uint64_t si = ELF64_R_SYM(r->r_info);
        if ((si + 1) * 24 > symsz) /* (left empty: no hook finds it) */
          continue;
        Elf64_Sym *sy = (Elf64_Sym *)(sym + si * 24);
        if (sy->st_name >= strsz)
          continue;
        const char *name = (const char *)str + sy->st_name;
        size_t n = strcspn(name, "#");
        if (n > 15)
          n = 15;
        memcpy(g_slots[k].nid, name, n);
        g_slots[k].slot = *(uint64_t *)&o[0x30] + r->r_offset;
      }
      free(sym);
      free(str);
      free(jr);
      return 0;
    }
    obj = *(uint64_t *)&o[0];
  }
  return -1;
}

static uint64_t slot_of(const char *nid) {
  for (int k = 0; k < g_nslots; k++)
    if (strcmp(g_slots[k].nid, nid) == 0)
      return g_slots[k].slot;
  return 0;
}

/* ---- an agent already in the game (this payload was sent again) ----
 * /data/doax3vr2/agent_state: "<pid> <base> <header offset> <size>". The agent keeps running
 * and this payload takes over serving it, from a copy of its image. */
static int adopt_agent(pid_t pid) {
  FILE *f = fopen(OUT_DIR "/agent_state", "r");
  if (!f)
    return -1;
  int ppid = -1;
  unsigned long base = 0, hdroff = 0, size = 0;
  int n = fscanf(f, "%d %lx %lx %lx", &ppid, &base, &hdroff, &size);
  fclose(f);
  if (n < 4 || ppid != pid || !size || size > 64 << 20 || hdroff >= size)
    return -1;
  uint64_t magic = 0;
  if (kernel_proc_copyout(pid, base + hdroff, &magic, 8) != 0 || magic != AGENT_MAGIC)
    return -1;
  g_mirror = malloc(size);
  if (!g_mirror || kernel_proc_copyout(pid, base, g_mirror, size) != 0) {
    free(g_mirror);
    g_mirror = NULL;
    return -1;
  }
  g_base = base, g_size = size, g_hdroff = hdroff;
  say("the agent is already in the game (at 0x%lx): serving it", base);
  return 0;
}

/* Import slots the trap fallback (inject_by_trap, below) points at unmapped pages. */
#define TRAP_BASE 0x2a0000ull
static const char *const k_trap_nids[] = {
    "Ud7j3+RDIBg", /* sceHmdSetupDialogUpdateStatus */
    "6lVRHMV5LY0", /* sceHmdSetupDialogGetResult */
    "q1cHNfGycLI", /* scePadRead */
    "4J2sUJmuHZQ", /* sceKernelGetProcessTime */
    "1jfXLRVzisc", /* sceKernelUsleep */
    "s-J66ar9g50", /* sceHmdInitialize315 */
    "NB1Y2kA2jCY", /* sceHmdSetupDialogInitialize */
    "NNgiV4T+akU", /* sceHmdSetupDialogOpen */
    "d2g5Ij7EUzo", /* sceHmdOpen */
    "K4KnH0QkT2c", /* sceHmdInitialize */
    "thDt9upZlp8", /* sceHmdGetDeviceInformation */
    "YndgXqQVV7c", /* scePadReadState */
};
#define NTRAPS (sizeof k_trap_nids / sizeof k_trap_nids[0])

/* see find_unresolved */
static uint64_t g_unresolved;

static int trap_index(uint64_t rip) {
  if (rip < TRAP_BASE || (rip - TRAP_BASE) % 0x1000 || (rip - TRAP_BASE) / 0x1000 >= NTRAPS)
    return -1;
  return (int)((rip - TRAP_BASE) / 0x1000);
}

/* Call fn(a0) on the stopped thread: a fresh stack 1 KiB below the red zone, returning to
 * address 0, where the fault stops it again; then the thread's registers are put back. */
static long call_in_game(pid_t pid, uint64_t fn, uint64_t a0) {
  struct reg bak, r;
  if (pt_getregs(pid, &bak) != 0)
    return -1;
  r = bak;
  uint64_t sp = ((bak.r_rsp - 1024) & ~(uint64_t)0xf) - 8, zero = 0;
  if (pt_copyin(pid, &zero, sp, 8) != 0)
    return -1;
  r.r_rsp = sp;
  r.r_rip = fn;
  r.r_rdi = a0;
  if (pt_setregs(pid, &r) != 0)
    return -1;
  long ret = -1;
  for (int i = 0; i < 50; i++) {
    if (pt_continue(pid, 0) != 0)
      break;
    int st = 0, waited = 0;
    for (; waited < 3000; waited++) {
      pid_t w = waitpid(pid, &st, WNOHANG);
      if (w == pid)
        break;
      usleep(1000);
    }
    if (waited >= 3000) {
      say("call_in_game: no return after 3 s");
      kill(pid, SIGSTOP);
      waitpid(pid, &st, 0);
      break;
    }
    if (!WIFSTOPPED(st))
      break;
    pt_getregs(pid, &r);
    if (r.r_rip == 0) {
      ret = r.r_rax;
      break;
    }
    say("call_in_game: stop with signal %d at 0x%lx, continuing", WSTOPSIG(st), (long)r.r_rip);
  }
  pt_setregs(pid, &bak);
  return ret;
}

/* ---- loading ---- */

/* a symbol's address in the agent (0: none), and its size if asked */
static uint64_t agent_sym_size(const char *name, uint64_t *size) {
  const Elf64_Ehdr *eh = (const Elf64_Ehdr *)g_agent;
  const Elf64_Shdr *sh = (const Elf64_Shdr *)(g_agent + eh->e_shoff);
  for (int i = 0; i < eh->e_shnum; i++) {
    if (sh[i].sh_type != SHT_SYMTAB)
      continue;
    const Elf64_Sym *sy = (const Elf64_Sym *)(g_agent + sh[i].sh_offset);
    const char *str = (const char *)(g_agent + sh[sh[i].sh_link].sh_offset);
    for (size_t j = 0; j < sh[i].sh_size / sizeof *sy; j++)
      if (strcmp(str + sy[j].st_name, name) == 0) {
        if (size)
          *size = sy[j].st_size;
        return sy[j].st_value;
      }
  }
  return 0;
}
static uint64_t agent_sym(const char *name) { return agent_sym_size(name, 0); }

/* test.txt (tools/testrun.sh): a run without a player. Its words: `record` (the controller to
 * the log, as `pad:` lines), `replay` (the controller from pad.txt: the same lines without the
 * prefix), `worn` (the headset counted as on a head); head.txt the head's pose the same way
 * (`head:` lines). The agent's pads.c, vr.c and vr2.c. */
static void test_settings(uint64_t lo) {
#ifdef RELEASE
  (void)lo;
  return; /* a release build: no test mode (test.txt, pad.txt, head.txt are not read) */
#endif
  /* each table filled no further than the agent's own (an agent.elf of another build may hold
   * fewer steps than TEST_*_STEPS): its symbol's size */
  uint64_t ssz = 0, hsz = 0;
  uint64_t va = agent_sym("g_test"), sva = agent_sym_size("g_pad_script", &ssz),
           nva = agent_sym("g_pad_script_n");
  uint32_t pad_max = ssz / 16 < TEST_PAD_STEPS ? (uint32_t)(ssz / 16) : TEST_PAD_STEPS;
  char line[64] = "";
  FILE *f = fopen(OUT_DIR "/test.txt", "r");
  if (!va || !f) {
    if (f)
      fclose(f);
    return;
  }
  if (!fgets(line, sizeof line, f))
    line[0] = 0;
  fclose(f);
  uint32_t test = (strstr(line, "record") ? 1u : 0) | (strstr(line, "replay") ? 2u : 0) |
                  (strstr(line, "worn") ? 4u : 0);
  uint32_t n = 0;
  if ((test & 2u) && sva && nva && (f = fopen(OUT_DIR "/pad.txt", "r"))) {
    struct {
      uint32_t t, b;
      uint8_t v[8];
    } *s = (void *)(g_mirror + sva - lo); /* the agent's struct pad_step */
    unsigned t, b, v[6];
    while (n < pad_max &&
           fscanf(f, "%u %x %x %x %x %x %x %x", &t, &b, v, v + 1, v + 2, v + 3, v + 4, v + 5) == 8) {
      s[n].t = t, s[n].b = b;
      for (int i = 0; i < 6; i++)
        s[n].v[i] = (uint8_t)v[i];
      s[n].v[6] = s[n].v[7] = 0;
      n++;
    }
    fclose(f);
    memcpy(g_mirror + nva - lo, &n, 4);
  }
  uint32_t nh = 0;
  uint64_t hva = agent_sym_size("g_head_script", &hsz), hnva = agent_sym("g_head_script_n");
  uint32_t head_max = hsz / 32 < TEST_HEAD_STEPS ? (uint32_t)(hsz / 32) : TEST_HEAD_STEPS;
  if ((test & 2u) && hva && hnva && (f = fopen(OUT_DIR "/head.txt", "r"))) {
    struct {
      uint32_t t;
      float v[7];
    } *s = (void *)(g_mirror + hva - lo); /* the agent's struct head_step */
    unsigned t;
    float v[7];
    while (nh < head_max && fscanf(f, "%u %f %f %f %f %f %f %f", &t, v, v + 1, v + 2, v + 3, v + 4,
                               v + 5, v + 6) == 8) {
      s[nh].t = t;
      memcpy(s[nh].v, v, sizeof v);
      nh++;
    }
    fclose(f);
    memcpy(g_mirror + hnva - lo, &nh, 4);
  }
  memcpy(g_mirror + va - lo, &test, 4);
  say("test: %s%s%s%u steps of pad.txt, %u of head.txt",
      test & 1 ? "recording the controller and the head, " : "",
      test & 2 ? "replaying the controller and the head, " : "",
      test & 4 ? "the headset counted as worn, " : "", n, nh);
}

static int menu_file_value(const char *key, unsigned *v);
static unsigned long cvar_value(const char *name, unsigned long def);
static int load_agent(pid_t pid) {
  const Elf64_Ehdr *eh = (const Elf64_Ehdr *)g_agent;
  const Elf64_Phdr *ph = (const Elf64_Phdr *)(g_agent + eh->e_phoff);
  const Elf64_Shdr *sh = (const Elf64_Shdr *)(g_agent + eh->e_shoff);
  uint64_t lo = ~0ull, hi = 0;
  for (int i = 0; i < eh->e_phnum; i++) {
    if (ph[i].p_type != PT_LOAD)
      continue;
    if (ph[i].p_vaddr < lo)
      lo = ph[i].p_vaddr;
    if (ph[i].p_vaddr + ph[i].p_memsz > hi)
      hi = ph[i].p_vaddr + ph[i].p_memsz;
  }
  lo &= ~(uint64_t)(PAGE - 1);
  g_size = (hi - lo + PAGE - 1) & ~(uint64_t)(PAGE - 1);
  free(g_mirror); /* (a load tried before in this game: the traps fire again after a failed one) */
  g_mirror = calloc(1, g_size);
  if (!g_mirror)
    return -1;
  intptr_t base = pt_mmap(pid, 0, g_size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  if (base == -1 || base == 0) {
    /* (no pt_perror: it calls __error in the game, at address 0 when that is not resolved) */
    say("agent: mmap of %zu bytes in the game failed (%ld)", g_size, (long)base);
    return -1;
  }
  g_base = base;
  say("agent: %zu bytes at 0x%lx in pid %d", g_size, (long)g_base, pid);
  for (int i = 0; i < eh->e_phnum; i++)
    if (ph[i].p_type == PT_LOAD && ph[i].p_filesz)
      memcpy(g_mirror + ph[i].p_vaddr - lo, g_agent + ph[i].p_offset, ph[i].p_filesz);
  int nrel = 0;
  for (int i = 0; i < eh->e_shnum; i++) {
    if (sh[i].sh_type != SHT_RELA)
      continue;
    const Elf64_Rela *r = (const Elf64_Rela *)(g_agent + sh[i].sh_offset);
    for (size_t j = 0; j < sh[i].sh_size / sizeof *r; j++) {
      if (ELF64_R_TYPE(r[j].r_info) != R_X86_64_RELATIVE) {
        say("agent: unsupported relocation type %lu", (unsigned long)ELF64_R_TYPE(r[j].r_info));
        return -1;
      }
      *(uint64_t *)(g_mirror + r[j].r_offset - lo) = g_base + r[j].r_addend;
      nrel++;
    }
  }
  /* astro_hdr from the symbol table */
  uint64_t hdr_va = 0;
  for (int i = 0; i < eh->e_shnum && !hdr_va; i++) {
    if (sh[i].sh_type != SHT_SYMTAB)
      continue;
    const Elf64_Sym *sy = (const Elf64_Sym *)(g_agent + sh[i].sh_offset);
    const char *str = (const char *)(g_agent + sh[sh[i].sh_link].sh_offset);
    for (size_t j = 0; j < sh[i].sh_size / sizeof *sy; j++)
      if (strcmp(str + sy[j].st_name, "astro_hdr") == 0)
        hdr_va = sy[j].st_value;
  }
  struct a_header *hdr = (struct a_header *)(g_mirror + hdr_va - lo);
  g_hdroff = hdr_va - lo;
  if (!hdr_va || hdr->magic != AGENT_MAGIC) {
    say("agent: no header");
    return -1;
  }
  hdr->firmware = g_fw->version;
  hdr->game_base = eboot_mapbase(pid);
  /* A picture every refresh, the game's ticks kept at 60 a second by the agent (hooks.c,
   * tick_frame): the default since 2026-11-13 (the user: "120 just felt so much better").
   * agent.fps120=0 (cvars.txt, or the settings menu's "120 fps" off; read here at the game's
   * start): the game's own 60 fps, a frame every two of the headset's refreshes. */
  unsigned fps120 = (unsigned)cvar_value("agent.fps120", 1); /* (on by default: the user, 2026-11-13) */
  menu_file_value("agent.fps120", &fps120); /* the settings menu's line wins */
  hdr->frame_pace = fps120 ? 1 : 2;
  if (hdr->frame_pace == 1)
    say("agent: a picture every refresh (agent.fps120=1 or the menu's 120 fps), 60 ticks a second");
  say("agent: %d relocations, header at +0x%lx, %u imports", nrel, (long)hdr_va, hdr->nimports);
  FILE *ipd = fopen(OUT_DIR "/ipd", "r");
  if (ipd) {
    unsigned um = 0;
    if (fscanf(ipd, "%u", &um) == 1 && um >= 50000 && um <= 80000)
      hdr->ipd_um = um;
    fclose(ipd);
  }
  test_settings(lo);
  if (hdr->gcn && g_gcn.magic == GCN_MAGIC) {
    uint32_t *dst = (uint32_t *)G2M(hdr->gcn);
    dst[0] = g_gcn.magic, dst[1] = g_gcn.dwords, dst[2] = g_gcn.rsrc1_vgprs,
    dst[3] = g_gcn.rsrc1_sgprs;
    memcpy(dst + 4, g_gcn_code, g_gcn.dwords * 4);
  }

  /* imports */
  struct a_import *imp = (struct a_import *)G2M(hdr->imports);
  int bad = 0, late = 0;
  for (uint32_t i = 0; i < hdr->nimports; i++) {
    const char *mod = (const char *)G2M(imp[i].module);
    const char *nid = (const char *)G2M(imp[i].nid);
    uint32_t h;
    uint64_t addr = 0;
    if (kernel_dynlib_handle(pid, mod, &h) != 0) {
      late++; /* a PS5 library the agent loads later (CMD_RESOLVE) */
    } else if (!(addr = kernel_dynlib_resolve(pid, h, nid))) {
      say("agent: import %s %s not found", mod, nid);
      bad++;
    }
    *(uint64_t *)G2M(imp[i].slot) = addr;
  }

  /* hooks */
  if (read_eboot_imports(pid) != 0) {
    say("agent: can't read the eboot's imports");
    return -1;
  }
  struct a_hook *hk = (struct a_hook *)G2M(hdr->hooks);
  int nhooks = 0, nfound = 0;
  for (; hk[nhooks].nid; nhooks++) {
    const char *nid = (const char *)G2M(hk[nhooks].nid);
    uint64_t slot = slot_of(nid);
    hk[nhooks].slot = slot;
    if (!slot) {
      say("agent: hook %s: not imported by the game", nid);
      continue;
    }
    uint64_t cur = 0;
    if (pt_copyout(pid, slot, &cur, 8) != 0) { /* (no original to pass on to: left unhooked) */
      say("agent: hook %s: its slot 0x%lx not readable: left unhooked", nid, (long)slot);
      hk[nhooks].slot = 0;
      continue;
    }
    *(uint64_t *)G2M(hk[nhooks].orig) = cur;
    nfound++;
  }
  say("agent: %d of %d hooks found in %d import slots; %d imports missing, %d resolved later",
      nfound, nhooks, g_nslots, bad, late);

  /* copy in, protect, patch */
  if (pt_copyin(pid, g_mirror, g_base, g_size) != 0) {
    say("agent: copyin failed");
    return -1;
  }
  /* Same order as hbldr's elfldr: kernel_mprotect sets a whole VM map entry, so every
   * segment is (re)protected in turn, and pt_mprotect on the others splits the entry. */
  for (int i = 0; i < eh->e_phnum; i++) {
    if (ph[i].p_type != PT_LOAD || ph[i].p_memsz == 0)
      continue;
    uint64_t a = (g_base + ph[i].p_vaddr - lo) & ~(uint64_t)(PAGE - 1);
    uint64_t e = (g_base + ph[i].p_vaddr - lo + ph[i].p_memsz + PAGE - 1) & ~(uint64_t)(PAGE - 1);
    int prot = ((ph[i].p_flags & PF_R) ? PROT_READ : 0) | ((ph[i].p_flags & PF_W) ? PROT_WRITE : 0) |
               ((ph[i].p_flags & PF_X) ? PROT_EXEC : 0);
    int r = (ph[i].p_flags & PF_X) ? kernel_mprotect(pid, a, e - a, prot)
                                   : pt_mprotect(pid, a, e - a, prot);
    if (r != 0) {
      say("agent: protect 0x%lx-0x%lx 0x%x failed", (long)a, (long)e, prot);
      return -1;
    }
  }
  for (int i = 0; i < nhooks; i++) {
    if (!hk[i].slot)
      continue;
    if (pt_copyin(pid, &hk[i].hook, hk[i].slot, 8) != 0)
      say("agent: patching slot 0x%lx failed", (long)hk[i].slot);
  }
  if (hdr->boot) {
    long r = call_in_game(pid, hdr->boot, 0);
    say("agent: boot -> %ld", r);
  }
  return 0;
}

/* Resolve every import slot that is still zero whose module is loaded now (modules the agent
 * loaded itself, and the game's own loaded later). */
static int resolve_missing(pid_t pid) {
  struct a_header *hdr = NULL;
  for (uint64_t off = 0; off + sizeof *hdr <= g_size; off += 8)
    if (*(uint64_t *)(g_mirror + off) == AGENT_MAGIC) {
      hdr = (struct a_header *)(g_mirror + off);
      break;
    }
  if (!hdr)
    return -1;
  struct a_import *imp = (struct a_import *)G2M(hdr->imports);
  int n = 0;
  for (uint32_t i = 0; i < hdr->nimports; i++) {
    uint64_t cur = 0;
    kernel_proc_copyout(pid, imp[i].slot, &cur, 8);
    if (cur)
      continue;
    const char *mod = (const char *)G2M(imp[i].module);
    const char *nid = (const char *)G2M(imp[i].nid);
    uint32_t h;
    uint64_t addr = 0;
    if (kernel_dynlib_handle(pid, mod, &h) != 0)
      continue; /* its module isn't loaded yet: a later request gets it */
    if (!(addr = kernel_dynlib_resolve(pid, h, nid))) {
      say("late import %s %s not found", mod, nid);
      continue;
    }
    kernel_proc_copyin(pid, &addr, imp[i].slot, 8);
    n++;
  }
  return n;
}

/* ---- imports of the modules the agent loaded ---- */
static int stub_unbound(pid_t pid, const uint64_t handles[4], uint64_t stub) {
  intptr_t proc = kernel_get_proc(pid);
  uint64_t obj = 0;
  int patched = 0;
  if (proc) /* (0: the game is gone) */
    kernel_copyout(proc + 0x3e8, &obj, 8);
  for (int n = 0; obj && n < 400; n++) {
    uint8_t o[0x180];
    if (kernel_copyout(obj, o, sizeof o) != 0)
      break;
    uint64_t next = *(uint64_t *)&o[0], handle = *(uint64_t *)&o[0x28],
             mapbase = *(uint64_t *)&o[0x30], dynsec = *(uint64_t *)&o[0x148];
    int want = 0;
    for (int i = 0; i < 4; i++)
      want |= handles[i] && handles[i] == handle;
    uint64_t s[0x60 / 8];
    if (want && (dynsec >> 48) == 0xffff && kernel_copyout(dynsec, s, sizeof s) == 0) {
      uint64_t symsz = s[6], strsz = s[8], jmpsz = s[10];
      uint8_t *sym = malloc(symsz), *str = malloc(strsz + 1), *jr = malloc(jmpsz);
      if (sym && str && jr && !kernel_copyout(s[5], sym, symsz) &&
          !kernel_copyout(s[7], str, strsz) && !kernel_copyout(s[9], jr, jmpsz)) {
        str[strsz] = 0;
        for (uint64_t k = 0; k + 24 <= jmpsz; k += 24) {
          uint64_t off = *(uint64_t *)(jr + k), info = *(uint64_t *)(jr + k + 8), cur = 0;
          uint64_t slot = mapbase + off;
          if (kernel_proc_copyout(pid, slot, &cur, 8) != 0)
            continue;
          if (cur >= 0x10000 && (cur >> 47) == 0)
            continue;
          uint64_t si = info >> 32;
          uint32_t sn = (si + 1) * 24 <= symsz ? *(uint32_t *)(sym + si * 24) : (uint32_t)strsz;
          const char *name = sn < strsz ? (const char *)str + sn : "?";
          kernel_proc_copyin(pid, &stub, slot, 8);
          if (patched++ < 40)
            say("stub: module %lu import %s (was 0x%lx)", (unsigned long)handle, name,
                (unsigned long)cur);
        }
      }
      free(sym);
      free(str);
      free(jr);
    }
    obj = next;
  }
  return patched;
}

/* Point module `handle`'s import slot(s) for `nid` at `value`. */
static int override_import(pid_t pid, uint64_t handle, const char *nid, uint64_t value) {
  intptr_t proc = kernel_get_proc(pid);
  uint64_t obj = 0;
  int patched = 0;
  if (proc) /* (0: the game is gone) */
    kernel_copyout(proc + 0x3e8, &obj, 8);
  for (int n = 0; obj && n < 400; n++) {
    uint8_t o[0x180];
    if (kernel_copyout(obj, o, sizeof o) != 0)
      break;
    uint64_t next = *(uint64_t *)&o[0], dynsec = *(uint64_t *)&o[0x148];
    uint64_t s[0x60 / 8];
    if (*(uint64_t *)&o[0x28] == handle && (dynsec >> 48) == 0xffff &&
        kernel_copyout(dynsec, s, sizeof s) == 0) {
      uint64_t mapbase = *(uint64_t *)&o[0x30], symsz = s[6], strsz = s[8], jmpsz = s[10];
      uint8_t *sym = malloc(symsz), *str = malloc(strsz + 1), *jr = malloc(jmpsz);
      if (sym && str && jr && !kernel_copyout(s[5], sym, symsz) &&
          !kernel_copyout(s[7], str, strsz) && !kernel_copyout(s[9], jr, jmpsz)) {
        str[strsz] = 0;
        size_t nl = strlen(nid);
        for (uint64_t k = 0; k + 24 <= jmpsz; k += 24) {
          uint64_t off = *(uint64_t *)(jr + k), info = *(uint64_t *)(jr + k + 8), si = info >> 32;
          uint32_t sn = (si + 1) * 24 <= symsz ? *(uint32_t *)(sym + si * 24) : (uint32_t)strsz;
          if (sn >= strsz)
            continue;
          const char *name = (const char *)str + sn;
          if (strncmp(name, nid, nl) != 0 || (name[nl] && name[nl] != '#'))
            continue;
          kernel_proc_copyin(pid, &value, mapbase + off, 8);
          patched++;
        }
      }
      free(sym);
      free(str);
      free(jr);
    }
    obj = next;
  }
  say("override: module %lu import %s -> 0x%lx (%d slots)", (unsigned long)handle, nid,
      (unsigned long)value, patched);
  return patched;
}

static struct a_header *mirror_header(void) {
  for (uint64_t off = 0; off + sizeof(struct a_header) <= g_size; off += 8)
    if (*(uint64_t *)(g_mirror + off) == AGENT_MAGIC)
      return (struct a_header *)(g_mirror + off);
  return NULL;
}

/* Re-point slots that something re-bound (a module load). */
static void repatch(pid_t pid) {
  struct a_header *hdr = NULL;
  /* the header address in the mirror never moves */
  for (uint64_t off = 0; off + sizeof *hdr <= g_size; off += 8) {
    if (*(uint64_t *)(g_mirror + off) == AGENT_MAGIC) {
      hdr = (struct a_header *)(g_mirror + off);
      break;
    }
  }
  if (!hdr)
    return;
  struct a_hook *hk = (struct a_hook *)G2M(hdr->hooks);
  int n = 0;
  for (int i = 0; hk[i].nid; i++) {
    if (!hk[i].slot)
      continue;
    uint64_t cur = 0;
    if (kernel_proc_copyout(pid, hk[i].slot, &cur, 8) != 0 || cur == hk[i].hook)
      continue;
    /* keep the newly bound function as the original (orig is a game address) */
    kernel_proc_copyin(pid, &cur, hk[i].orig, 8);
    *(uint64_t *)G2M(hk[i].orig) = cur;
    kernel_proc_copyin(pid, &hk[i].hook, hk[i].slot, 8);
    n++;
  }
  if (n) /* one line a pass (one a slot was ~100 lines at every module the game loads) */
    say("repatched %d import slots a module load re-bound", n);
}

/* ---- one instance at a time: a newer injector asks the running one to leave ----
 * The control port listens on every interface: a connection saying "quit" ends the instance, one
 * saying nothing (a probe: is one resident?) is answered "yes". So a sender can ask the resident
 * instance to leave and wait for it to be gone BEFORE loading the next: loading a payload while
 * one is resident brought the console down (three times in fifteen, before the new one's first
 * line, which comes before it touches this port; two processes using the jailbreak's kernel
 * access at once, the resident one scanning for the game every 5 ms, is the likeliest cause).
 * A newer instance also asks the older one itself, and leaves if the port stays busy. */
#define CTL_PORT 9089
static int g_ctl = -1;

/* Test runs (tools/testrun.sh): the game started and closed from the control port, by the
 * system's launcher (libSceSystemService; the parameters as PS5 payloads that start titles
 * pass them). */
struct lnc_app_param {
  uint32_t size;
  int32_t user_id;
  uint32_t app_opt;
  uint64_t crash_report;
  uint64_t check_flag;
};
int sceLncUtilLaunchApp(const char *tid, const char *argv[], struct lnc_app_param *p);
int sceSystemServiceLaunchApp(const char *tid, const char *argv[], struct lnc_app_param *p);
uint32_t sceLncUtilGetAppIdOfRunningBigApp(void);
int sceLncUtilGetAppTitleId(uint32_t app_id, char *title_id);
int sceLncUtilKillApp(uint32_t app_id);
int sceLncUtilSuspendApp(uint32_t app_id, int flags);
int sceLncUtilResumeApp(uint32_t app_id, int flags);
int sceUserServiceInitialize(void *param);
int sceUserServiceGetForegroundUser(int *user_id);

/* On a thread of its own, the loop waiting for the game standing by until the launcher has
 * returned: a process it is still making can't be attached to (and is found by the loop
 * before that; the entry point is still ahead when the launcher returns). */
static volatile int g_launching;
static volatile time_t g_launched_at; /* the last launch from here (inject: its first image ends) */
static char g_launch_tid[10] = "CUSA04555"; /* `launch [title id]`: one of k_builds' */

static void *launch_game(void *arg) {
  (void)arg;
  int user = -1;
  sceUserServiceInitialize(NULL);
  int ur = sceUserServiceGetForegroundUser(&user);
  struct lnc_app_param p = {sizeof p, user, 0, 0, 0};
  const char *argv[] = {NULL};
  int rc = sceLncUtilLaunchApp(g_launch_tid, argv, &p);
  if (rc < 0)
    rc = sceSystemServiceLaunchApp(g_launch_tid, argv, &p);
  say("launch: %s, user %d (0x%x), rc 0x%x", g_launch_tid, user, (unsigned)ur, (unsigned)rc);
  g_launching = 0;
  return NULL;
}

static int start_launch(void) {
  pthread_t t;
  g_launching = 1;
  g_launched_at = time(NULL);
  int rc = pthread_create(&t, NULL, launch_game, NULL);
  if (rc == 0)
    pthread_detach(t);
  else
    g_launching = 0;
  return rc;
}

/* The processes (pid, parent, name) to the control port's `ps`: a stray injector shows here. */
static void list_procs(int c) {
  int mib[4] = {1, 14, 8, 0};
  size_t size = 0;
  if (sysctl(mib, 4, NULL, &size, NULL, 0) != 0)
    return;
  uint8_t *buf = malloc(size);
  if (!buf || sysctl(mib, 4, buf, &size, NULL, 0) != 0) {
    free(buf);
    return;
  }
  char line[64];
  for (uint8_t *p = buf; p + 512 <= buf + size;) {
    int structsize = *(int *)p;
    if (structsize <= 0)
      break;
    snprintf(line, sizeof line, "%d %d %.19s\n", *(pid_t *)&p[72], *(pid_t *)&p[76],
             (const char *)&p[447]);
    write(c, line, strlen(line));
    p += structsize;
  }
  free(buf);
}

/* (g_live_pid: the game being served, for kill_game's soft stop and the markers; above) */
struct a_shared_head;
static void relay_log(pid_t pid, uint64_t shared, const struct a_shared_head *sh);
static volatile uint64_t g_live_shared;
static int kill_game(void) {
  uint32_t id = sceLncUtilGetAppIdOfRunningBigApp();
  char tid[16] = "";
  int rc = -1;
  /* The PS5 VR stack inside the game shut down first (Hmd2's VR mode off, the hand tracker
   * stopped): killed outright, a game leaves the kernel whatever those keep, and four of the five
   * console crashes came around the tenth run after a jailbreak. The agent is told CMD_QUIT and
   * answers STOP_DONE in cmd_arg[3] (up to 3 s). */
  pid_t lp = g_live_pid;
  uint64_t ls = g_live_shared;
  if (lp > 0 && ls) {
    uint32_t quit = 1;
    uint64_t done = 0;
    kernel_proc_copyin(lp, &quit, ls + offsetof(struct a_shared_head, cmd), 4);
    for (int i = 0; i < 300; i++) {
      usleep(10000);
      if (kernel_proc_copyout(lp, ls + offsetof(struct a_shared_head, cmd_arg) + 24, &done, 8) != 0 ||
          done == 0x51554954ull)
        break;
    }
    say("kill: the agent's stop %s", done == 0x51554954ull ? "done" : "not answered in 3 s");
    struct a_shared_head sh;
    if (kernel_proc_copyout(lp, ls, &sh, sizeof sh) == 0)
      relay_log(lp, ls, &sh); /* its last lines (the VR mode's rc, the hand tracker's) */
  }
  if (id && sceLncUtilGetAppTitleId(id, tid) == 0 && title_known(tid))
    rc = sceLncUtilKillApp(id);
  say("kill: app 0x%x (%s), rc 0x%x", id, tid, (unsigned)rc);
  return rc;
}

static int ctl_open(void) {
  struct sockaddr_in a, lo;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons(CTL_PORT);
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  lo = a;
  lo.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  for (int i = 0; i < 100; i++) {
    int s = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (bind(s, (struct sockaddr *)&a, sizeof a) == 0 && listen(s, 4) == 0) {
      fcntl(s, F_SETFL, O_NONBLOCK);
      return s;
    }
    close(s);
    if (i == 0) { /* an older injector holds the port: ask it to quit */
      int c = socket(AF_INET, SOCK_STREAM, 0);
      if (connect(c, (struct sockaddr *)&lo, sizeof lo) == 0)
        write(c, "quit\n", 5);
      close(c);
    }
    usleep(100000);
  }
  return -1;
}

/* Someone wants this instance to leave: a connection that says "quit". A connection that says
 * nothing is a probe, answered "yes". Only asked where leaving is safe (not ptrace-attached). */
static int ctl_quit_requested(int remote_ok) {
  if (g_ctl < 0)
    return 0;
  struct sockaddr_in peer;
  socklen_t plen = sizeof peer;
  memset(&peer, 0, sizeof peer);
  int c = accept(g_ctl, (struct sockaddr *)&peer, &plen);
  if (c < 0)
    return 0;
  /* the accepted socket may be non-blocking like the listener: wait for what the client says */
  struct pollfd pfd = {c, POLLIN, 0};
  char buf[32] = {0};
  ssize_t n = poll(&pfd, 1, 300) > 0 ? recv(c, buf, sizeof buf - 1, 0) : -1;
  int quit = n >= 4 && memcmp(buf, "quit", 4) == 0;
  int local = peer.sin_addr.s_addr == htonl(INADDR_LOOPBACK);
  char reply[48];
#ifdef RELEASE
  /* a release build: the test runner's commands (launch, kill, ps) are not served; anyone on the
   * network reaches this port */
  if ((n >= 6 && memcmp(buf, "launch", 6) == 0) || (n >= 4 && memcmp(buf, "kill", 4) == 0) ||
      (n >= 2 && memcmp(buf, "ps", 2) == 0) || (n >= 7 && memcmp(buf, "suspend", 7) == 0) ||
      (n >= 6 && memcmp(buf, "resume", 6) == 0)) {
    write(c, "not in a release build\n", 23);
  } else
#endif
  if (n >= 6 && memcmp(buf, "launch", 6) == 0) {
    /* a test run: the game started here, while none is served; `launch CUSA14173` another of
     * the known titles (Fortune without one) */
    char want[16] = "CUSA04555";
    if (sscanf(buf + 6, " %15s", want) < 1)
      strcpy(want, "CUSA04555");
    if (strlen(want) != 9 || !title_known(want))
      snprintf(reply, sizeof reply, "launch: %.9s is not a known title\n", want);
    else if (remote_ok) {
      memcpy(g_launch_tid, want, 10);
      snprintf(reply, sizeof reply, "launch %s\n", start_launch() == 0 ? "started" : "failed");
    } else
      snprintf(reply, sizeof reply, "the game is running\n");
    write(c, reply, strlen(reply));
  } else if (n >= 4 && memcmp(buf, "kill", 4) == 0) {
    int p = 0;
    if (sscanf(buf + 4, "%d", &p) == 1 && p > 1) /* `kill <pid>`: a process, outright */
      snprintf(reply, sizeof reply, "kill %d: %d\n", p, kill(p, SIGKILL));
    else
      snprintf(reply, sizeof reply, "kill 0x%x\n", (unsigned)kill_game());
    write(c, reply, strlen(reply));
  } else if ((n >= 7 && memcmp(buf, "suspend", 7) == 0) || (n >= 6 && memcmp(buf, "resume", 6) == 0)) {
    /* a test run: the game suspended and resumed by the system's launcher, as switching apps does
     * (2026-11-14: the scenery lost after the PS5's home screen; a step toward reproducing it) */
    /* (a suspended game is no longer the running big app: resume takes the id the suspend saved) */
    static uint32_t suspended_id;
    int rc = -1, sus = buf[0] == 's';
    uint32_t id = sus ? sceLncUtilGetAppIdOfRunningBigApp() : suspended_id;
    char tid[16] = "";
    if (sus) {
      if (id && id != 0xffffffffu && sceLncUtilGetAppTitleId(id, tid) == 0 && title_known(tid) &&
          (rc = sceLncUtilSuspendApp(id, 0)) == 0)
        suspended_id = id;
    } else if (id) {
      strcpy(tid, "suspended");
      if ((rc = sceLncUtilResumeApp(id, 0)) == 0)
        suspended_id = 0;
    }
    say("%s: app 0x%x (%s), rc 0x%x", sus ? "suspend" : "resume", id, tid, (unsigned)rc);
    snprintf(reply, sizeof reply, "%s 0x%x\n", sus ? "suspend" : "resume", (unsigned)rc);
    write(c, reply, strlen(reply));
  } else if (n >= 2 && memcmp(buf, "ps", 2) == 0) {
    list_procs(c);
  } else if (quit && !local && !remote_ok) {
    /* a game is being served: a remote quit now would pull the libraries from under it */
    write(c, "the game is running\n", 20);
    quit = 0;
  } else if (!quit && n >= 0) {
    write(c, "yes\n", 4); /* a probe: one is resident (nothing on a connection that failed) */
  }
  close(c);
  return quit;
}

int sceKernelGetAppInfo(int pid, void *info);

/* A value from a param.sfo (string entries only). */
static int sfo_get(const uint8_t *sfo, size_t size, const char *key, char *out, size_t outsz) {
  if (size < 20 || memcmp(sfo, "\0PSF", 4) != 0)
    return -1;
  uint32_t keys = *(const uint32_t *)(sfo + 8), data = *(const uint32_t *)(sfo + 12);
  uint32_t n = *(const uint32_t *)(sfo + 16);
  for (uint32_t i = 0; i < n && 20 + i * 16 + 16 <= size; i++) {
    const uint8_t *e = sfo + 20 + i * 16;
    uint32_t koff = keys + *(const uint16_t *)e, len = *(const uint32_t *)(e + 4);
    uint32_t doff = data + *(const uint32_t *)(e + 12);
    if (koff >= size || doff > size || len > size - doff ||
        strncmp((const char *)sfo + koff, key, size - koff))
      continue;
    if (len >= outsz)
      len = outsz - 1;
    memcpy(out, sfo + doff, len);
    out[len] = 0;
    return 0;
  }
  return -1;
}

/* The title and version of a running app, from its param.sfo (/system_data/priv/appmeta). */
static int app_title(pid_t pid, char tid[10], char *title, size_t tsz, char *ver, size_t vsz) {
  uint8_t info[0x80];
  char path[96];
  memset(info, 0, sizeof info);
  if (sceKernelGetAppInfo(pid, info) != 0)
    return -1;
  memcpy(tid, info + 0x10, 9);
  tid[9] = 0;
  snprintf(path, sizeof path, "/system_data/priv/appmeta/%s/param.sfo", tid);
  static uint8_t sfo[16384];
  FILE *f = fopen(path, "rb");
  size_t n = f ? fread(sfo, 1, sizeof sfo, f) : 0;
  if (f)
    fclose(f);
  title[0] = 0, ver[0] = 0;
  sfo_get(sfo, n, "TITLE", title, tsz);
  /* Dead or Alive Xtreme 3: the update's level is APP_VER (01.19 and 01.08; VERSION stays
   * 01.00); VERSION only where there is no APP_VER */
  sfo_get(sfo, n, "APP_VER", ver, vsz);
  if (!ver[0])
    sfo_get(sfo, n, "VERSION", ver, vsz);
  return 0;
}

/* A game version this payload doesn't know: its files saved (common/dump.c) on a thread of their
 * own, as the payload goes on waiting for a game it knows (a dump takes about a minute; reading
 * and decrypting the game's files, writing only under DUMP_ROOT).
 * One at a time. */
static volatile int g_dumping;
struct game_dump_arg {
  char tid[16], ver[16];
};
static void *game_dump_thread(void *p) {
  struct game_dump_arg *a = p;
  struct dump_result r;
  dump_save(a->tid, &r);
  say("dump of %s %s: %u files saved, %u failed, " DUMP_ROOT "/%s%s", a->tid, a->ver, r.ok, r.fail, r.name,
      r.tar_rc == 0 ? ".tar" : " (no .tar)");
  if (r.tar_rc == 0 && r.fail == 0)
    notify("DOAX3VR2: the files for adding %s version %s are saved. Send " DUMP_ROOT "/%s.tar to the "
           "developer (docs/DUMP.md).", a->tid, a->ver, r.name);
  else
    notify("DOAX3VR2: the files for adding %s version %s were saved with %u missing (the game closed?). "
           "Send " DUMP_ROOT "/%s%s and its dump.log to the developer.", a->tid, a->ver, r.fail, r.name,
           r.tar_rc == 0 ? ".tar" : "");
  free(a);
  g_dumping = 0;
  return NULL;
}
static void game_dump_start(const char *tid, const char *ver) {
  if (g_dumping)
    return;
  struct game_dump_arg *a = calloc(1, sizeof *a);
  pthread_t t;
  if (!a)
    return;
  snprintf(a->tid, sizeof a->tid, "%.9s", tid);
  snprintf(a->ver, sizeof a->ver, "%s", ver);
  notify("DOAX3VR2: this Dead or Alive Xtreme 3 is %s version %s. Only " BUILDS_TEXT " work for now. "
         "Saving its files for adding it: keep the game open about a minute, until the next notice.", tid, ver);
  g_dumping = 1;
  if (pthread_create(&t, NULL, game_dump_thread, a) != 0) {
    say("dump: no thread");
    free(a);
    g_dumping = 0;
    return;
  }
  pthread_detach(t);
}

/* Another Dead or Alive Xtreme 3 (another region or version) is told about on screen once, as the payload
 * otherwise just waits. Checked a few seconds after it starts, when its image is certainly in
 * place. */
static void check_other_title(pid_t pid) {
  static pid_t seen[8];
  static time_t first[8];
  static unsigned next;
  time_t now = time(NULL);
  unsigned k;
  for (k = 0; k < 8 && seen[k] != pid; k++)
    ;
  if (k == 8) {
    k = next++ % 8;
    seen[k] = pid, first[k] = now;
    return;
  }
  if (first[k] == 0 || now - first[k] < 3)
    return;
  first[k] = 0; /* checked */
  char tid[10], title[128], ver[16];
  if (app_title(pid, tid, title, sizeof title, ver, sizeof ver) != 0 ||
      (!strcasestr(title, TITLE_WORD) && !title_known(tid)))
    return;
  say("%s (%s, version %s) is not the build this payload knows", title, tid, ver);
  const char *v = ver[0] == '0' && ver[1] ? ver + 1 : ver; /* "01.00" -> "1.00" */
  /* its files, for adding it, saved by themselves (once: not when its .tar is there already) */
  char name[96], tar[160];
  struct stat st;
  dump_name(tid, name, sizeof name);
  snprintf(tar, sizeof tar, DUMP_ROOT "/%s.tar", name);
  if (stat(tar, &st) == 0) {
    notify("DOAX3VR2: this Dead or Alive Xtreme 3 is %s version %s. Only " BUILDS_TEXT " work for now. "
           "Its files for adding it were saved before: send %s to the developer (docs/DUMP.md).", tid, v, tar);
    return;
  }
  game_dump_start(tid, v);
}

static int build_by_code(pid_t pid, uint64_t base, int want, const char *tid);
/* The game: the eboot.bin of a title in k_builds whose code is one of its builds' (build_by_code; the
 * version in its param.sfo the first guess), once its image is mapped (its first page readable,
 * holding the interpreter's name every PS4 eboot starts with), so that the match comes from the
 * first instructions on, before the game can open its setup dialog. Version and code are read once
 * per process. */
static pid_t find_game(pid_t skip) {
  static pid_t ver_pid = -1;
  static int ver_ok, ver_build = -1;
  int mib[4] = {1, 14, 8, 0};
  size_t size = 0;
  pid_t found = -1;
  if (sysctl(mib, 4, NULL, &size, NULL, 0) != 0)
    return -1;
  uint8_t *buf = malloc(size);
  if (!buf || sysctl(mib, 4, buf, &size, NULL, 0) != 0) {
    free(buf);
    return -1;
  }
  for (uint8_t *p = buf; p < buf + size;) {
    int structsize = *(int *)p;
    pid_t pid = *(pid_t *)&p[72];
    const char *name = (const char *)&p[447];
    if (structsize <= 0)
      break;
    p += structsize;
    uint8_t code[16], info[0x80];
    if (pid == skip || strcmp(name, "eboot.bin") != 0)
      continue;
    /* Every app is an eboot.bin. Its title, from the system, before any walk through its record
     * in the kernel: a process that is starting or ending has records being made or freed, and
     * reading those can bring the whole console down. */
    memset(info, 0, sizeof info);
    if (sceKernelGetAppInfo(pid, info) != 0 || !title_known((const char *)info + 0x10)) {
      check_other_title(pid);
      continue;
    }
    if (pid != ver_pid) {
      char tid[10], title[128], ver[16];
      ver_pid = pid;
      ver_build = app_title(pid, tid, title, sizeof title, ver, sizeof ver) == 0
                      ? build_index(tid, ver)
                      : -1;
      ver_ok = 1; /* a known title: its code decides (build_by_code), the version only its first guess */
      say("%s (%s version %s): %s", title, tid, ver, ver_build >= 0 ? "a build this payload knows"
                                                                    : "not a version this payload knows: its code is checked");
    }
    if (!ver_ok) {
      check_other_title(pid);
      continue;
    }
    /* its image is in place: every PS4 eboot begins with its interpreter's name */
    uint64_t base = eboot_mapbase(pid);
    if (base && kernel_proc_copyout(pid, base, code, sizeof code) == 0 &&
        memcmp(code, "/libexec/ld-elf", 15) == 0) {
      /* and its code is that build's: every address the agent is given is the build's, and an
       * update may keep the version the payload reads (once per process: its answer kept) */
      static pid_t code_pid = -1;
      static int code_build = -1;
      if (pid != code_pid) {
        code_build = build_by_code(pid, base, ver_build, (const char *)info + 0x10);
        if (code_build != -2)
          code_pid = pid; /* (else read again on the next pass) */
      }
      if (code_build == -2)
        continue;
      if (code_build < 0) {
        ver_ok = 0;
        check_other_title(pid);
        continue;
      }
      found = pid, g_build = code_build;
    }
  }
  free(buf);
  return found;
}

/* The system's AV control service lets an app switch the output to VR only when its app info
 * carries attribute3 bit 0x400 (PS VR2), which a PS4 title never has. The kernel keeps the app
 * info in the process record (proc+0x460 on FW 13.60): it is found by its contents and the bit
 * is set there, so the service sees a PS VR2 app. */
static void mark_vr2_app(pid_t pid) {
  uint8_t info[0x80];
  static uint8_t pr[0x1000];
  memset(info, 0, sizeof info);
  if (sceKernelGetAppInfo(pid, info) != 0) {
    say("appinfo: GetAppInfo failed");
    return;
  }
  intptr_t proc = kernel_get_proc(pid);
  if (!proc || kernel_copyout(proc, pr, sizeof pr) != 0)
    return;
  int at = -1, hits = 0;
  for (int off = 0x20; off + 0x30 <= (int)sizeof pr; off += 4)
    if (!memcmp(pr + off, info + 0x20, 0x30))
      at = off - 0x20, hits++;
  if (hits != 1) {
    say("appinfo: %d matches in the process record, not marked", hits);
    return;
  }
  uint32_t attr3 = *(uint32_t *)(pr + at + 0x4c), want = attr3 | 0x400;
  if (want != attr3)
    kernel_copyin(&want, proc + at + 0x4c, 4);
  memset(info, 0, sizeof info);
  sceKernelGetAppInfo(pid, info);
  say("appinfo: at proc+0x%x, attribute3 0x%x -> 0x%x", at, attr3, *(uint32_t *)(info + 0x4c));
}

static int game_alive(pid_t pid, intptr_t proc) {
  return kill(pid, 0) == 0 && kernel_get_proc(pid) == proc;
}

/* The AV control service (SceSysAvControl.elf, image at VA 0x400000) refuses the PS VR2 output
 * for a PS4 (BC) title in two places. Both are patched while the game runs and restored when the
 * game ends or this injector exits. Addresses below are FW 13.60's; each firmware profile
 * (common/firmware.h) carries its own sites and original bytes, checked before patching.
 * 1. Its layout handler rejects the VR layout ("param.json does not have flags") by one branch,
 *    `je 0x41749e` at 0x41739b: replaced by a 6-byte NOP so it falls through to the accept path.
 * 2. checkVideoModeForChangeOutput (0x431e70) then refuses the VR video mode (res 0xe1ffffff,
 *    2040p 119.88 Hz) against the app's capability flags (AvManager:5387, 0x802a000e). After its
 *    "is this mode valid at all" check, `xor eax,eax; jmp 0x431f64` at 0x431ed8 returns 0. */
#define k_av_sites (g_fw->av)
#define AV_NSITES 2u
static pid_t g_av_pid = -1;
static int g_av_patched;

static pid_t find_proc(const char *name) {
  int mib[4] = {1, 14, 8, 0};
  size_t size = 0;
  pid_t found = -1;
  if (sysctl(mib, 4, NULL, &size, NULL, 0) != 0)
    return -1;
  uint8_t *buf = malloc(size);
  if (!buf || sysctl(mib, 4, buf, &size, NULL, 0) != 0) {
    free(buf);
    return -1;
  }
  for (uint8_t *p = buf; p < buf + size;) {
    int structsize = *(int *)p;
    pid_t pid = *(pid_t *)&p[72];
    const char *n = (const char *)&p[447];
    if (structsize <= 0)
      break;
    p += structsize;
    if (strcmp(n, name) == 0)
      found = pid;
  }
  free(buf);
  return found;
}

static void patch_av_service(void) {
  if (g_av_patched)
    return;
  g_av_pid = find_proc("SceSysAvControl.elf");
  if (g_av_pid < 0) {
    say("avservice: not found");
    return;
  }
  /* all sites must hold the expected bytes (or our patch, after a crash) before any is written */
  for (unsigned i = 0; i < AV_NSITES; i++) {
    const struct av_site *t = &k_av_sites[i];
    uint8_t cur[8];
    if (kernel_proc_copyout(g_av_pid, t->va, cur, t->len) != 0) {
      say("avservice: read 0x%lx failed", (unsigned long)t->va);
      return;
    }
    if (memcmp(cur, t->orig, t->len) != 0 && memcmp(cur, t->patch, t->len) != 0) {
      say("avservice: 0x%lx holds unexpected bytes (%02x %02x %02x ...) - not patching",
          (unsigned long)t->va, cur[0], cur[1], cur[2]);
      return;
    }
  }
  for (unsigned i = 0; i < AV_NSITES; i++)
    kernel_proc_copyin(g_av_pid, k_av_sites[i].patch, k_av_sites[i].va, k_av_sites[i].len);
  g_av_patched = 1;
  say("avservice: pid %d VR layout and output-mode gates opened", g_av_pid);
}

/* FW 11.40/11.60 (av_record): the AV service builds its per-app record (pid, appid, flags; 12 bytes) once, at
 * launch, before attribute3 is marked above, so it sees a PS VR1 title (flag 0x200000, no 0x10)
 * and keeps the headset in the cinematic USB layout. Once the game runs, the record for its pid
 * is found in the service's writable memory and changed to a PS VR2 title: 0x200000 cleared,
 * 0x10 set (the two bits that differ from a native PS VR2 title's record, besides an unrelated
 * SDK-version bit). Only that one record is touched and it disappears with the app. The service
 * is not patched for this and nothing is persisted. Returns 1 once a record was changed. */
#define AVREC_VR1 0x200000u
#define AVREC_VR2 0x10u
static int g_avrec_done;

#define AVREC_MAX 2 /* the service keeps two copies of the record (its data and its heap) */

static int av_mark_vr2_record(pid_t game) {
  /* On FW 13.60 marking attribute3 is enough, so its record is left alone. */
  if (g_avrec_done || g_av_pid < 0 || !g_fw->av_record)
    return g_avrec_done;
  struct {
    uint64_t va;
    uint32_t appid, flags;
  } cand[AVREC_MAX + 1];
  int mib[4] = {1, 14, 32, g_av_pid}; /* KERN_PROC_VMMAP */
  size_t size = 0;
  if (sysctl(mib, 4, NULL, &size, NULL, 0) != 0 || !size) {
    say("avrecord: vmmap query failed (errno %d)", errno);
    return 0;
  }
  size += size / 4;
  uint8_t *map = malloc(size);
  static uint8_t chunk[0x100000 + 16];
  if (!map || sysctl(mib, 4, map, &size, NULL, 0) != 0) {
    free(map);
    say("avrecord: vmmap read failed (errno %d)", errno);
    return 0;
  }
  int hits = 0;
  unsigned long scanned = 0;
  for (uint8_t *e = map; e + 64 <= map + size;) {
    int es = *(int *)e;
    if (es <= 0)
      break;
    uint64_t start = *(uint64_t *)(e + 8), end = *(uint64_t *)(e + 16);
    int prot = *(int *)(e + 56);
    e += es;
    if ((prot & 3) != 3 || end <= start || end - start > 0x10000000 || scanned > 0x20000000)
      continue;
    for (uint64_t a = start; a < end; a += 0x100000) {
      size_t n = end - a > 0x100000 ? 0x100000 : (size_t)(end - a);
      size_t rd = n + 16 < end - a ? n + 16 : (size_t)(end - a);
      if (kernel_proc_copyout(g_av_pid, a, chunk, rd) != 0)
        continue;
      scanned += n;
      for (size_t o = 0; o + 12 <= rd && o < n; o += 4) {
        uint32_t pid = *(uint32_t *)(chunk + o), fl = *(uint32_t *)(chunk + o + 8);
        if (pid != (uint32_t)game || (fl & 0x2e0000) != 0x2e0000 || !(fl & AVREC_VR1) ||
            (fl & AVREC_VR2))
          continue;
        if (hits <= AVREC_MAX)
          cand[hits].va = a + o + 8, cand[hits].appid = *(uint32_t *)(chunk + o + 4),
          cand[hits].flags = fl;
        hits++;
      }
    }
  }
  free(map);
  if (!hits) {
    say("avrecord: no record for pid %d in %lu bytes", game, scanned);
    return 0;
  }
  /* Nothing is written unless every match is the same app's record: a stray match elsewhere in
   * the service would otherwise be corrupted. */
  int same = hits <= AVREC_MAX;
  for (int i = 1; same && i < hits; i++)
    same = cand[i].appid == cand[0].appid && cand[i].flags == cand[0].flags;
  if (!same) {
    say("avrecord: %d candidate records for pid %d disagree or are too many, not changed", hits,
        game);
    g_avrec_done = 1; /* don't rescan: the result would be the same */
    return 0;
  }
  for (int i = 0; i < hits; i++) {
    uint32_t want = (cand[i].flags & ~AVREC_VR1) | AVREC_VR2, back = 0;
    kernel_proc_copyin(g_av_pid, &want, cand[i].va, 4);
    kernel_proc_copyout(g_av_pid, cand[i].va, &back, 4);
    say("avrecord: pid %d record at 0x%lx (appid 0x%x): flags 0x%x -> 0x%x", game,
        (unsigned long)(cand[i].va - 8), cand[i].appid, cand[i].flags, back);
  }
  g_avrec_done = 1;
  return 1;
}

static void restore_av_service(void) {
  g_avrec_done = 0;
  if (!g_av_patched || g_av_pid < 0)
    return;
  if (kill(g_av_pid, 0) != 0) {
    g_av_patched = 0;
    return;
  }
  for (unsigned i = 0; i < AV_NSITES; i++) {
    /* only over our own patch: the service may have been restarted, its pid another process's now */
    const struct av_site *t = &k_av_sites[i];
    uint8_t cur[8];
    if (kernel_proc_copyout(g_av_pid, t->va, cur, t->len) != 0 || memcmp(cur, t->patch, t->len) != 0) {
      say("avservice: 0x%lx no longer holds the patch: left as it is", (unsigned long)t->va);
      continue;
    }
    kernel_proc_copyin(g_av_pid, t->orig, t->va, t->len);
  }
  say("avservice: gates restored");
  g_av_patched = 0;
}

static void reset_game_state(void) {
  free(g_mirror);
  g_mirror = NULL;
  free(g_slots);
  g_slots = NULL;
  g_nslots = 0;
  g_base = g_size = g_hdroff = 0;
  pt_tid = 0;
}

/* ---- when no thread is in game code: traps ----
 * Once the game has opened the PS VR setup dialog (if the agent wasn't in yet), its threads wait
 * in system calls and an attach doesn't catch one in game code. The eboot's import slots of
 * functions the game calls every frame, and of its first PS VR calls, are then pointed at
 * unmapped addresses and the game runs on: the first such call faults on its trap, which stops
 * that thread in user mode at a call boundary in game code, as the agent's mapping and boot
 * call need. Then every slot is put back, and each thread at a trap goes on to what its slot
 * now holds (the agent's hook, or the original function). */
static void move_off_traps(pid_t pid, const uint64_t *slot, const uint64_t *orig) {
  lwpid_t lwps[512];
  int n = pt_lwps(pid, lwps, 512);
  pid_t keep = pt_tid;
  for (int i = 0; i < n; i++) {
    struct reg r;
    int k;
    pt_tid = lwps[i];
    if (pt_getregs(pid, &r) == 0 && (k = trap_index(r.r_rip)) >= 0 && slot[k]) {
      /* on to what the slot holds now: the agent's hook once the agent is in, so the call that
       * fired (often the game's first PS VR call) is answered by the agent */
      uint64_t now = 0;
      if (pt_copyout(pid, slot[k], &now, 8) != 0 || trap_index(now) >= 0 || !now)
        now = orig[k];
      r.r_rip = now;
      pt_setregs(pid, &r);
    }
  }
  pt_tid = keep;
}

/* A thread that took a trap at the same time as the one that fired still has its SIGSEGV
 * queued; delivered after the detach, it would kill the game (rel4: the fault address a trap,
 * the thread long since moved on). Let the game run traced for a moment and drop every fault on
 * a trap page; other signals go through. Returns with the game stopped, ready to detach. */
static void drop_trap_faults(pid_t pid, const uint64_t *slot, const uint64_t *orig) {
  int pass = 0, stopping = 0;
  for (int round = 0; round < 64; round++) {
    pt_tid = 0;
    if (pt_continue(pid, pass) != 0)
      return;
    pass = 0;
    int st = 0;
    pid_t w = 0;
    for (int t = 0; !stopping && t < 200 && (w = waitpid(pid, &st, WNOHANG)) == 0; t++)
      usleep(1000);
    if (w == 0) { /* quiet for 200 ms: stop it to detach */
      kill(pid, SIGSTOP);
      stopping = 1;
      w = waitpid(pid, &st, 0);
    }
    if (w != pid || !WIFSTOPPED(st))
      return;
    int sig = WSTOPSIG(st);
    uint64_t addr = 0;
    if (sig == SIGSEGV && pt_event_siginfo(pid, &addr) == SIGSEGV && trap_index(addr) >= 0) {
      say("trap: dropped a queued fault on the %s trap", k_trap_nids[trap_index(addr)]);
      move_off_traps(pid, slot, orig);
      continue;
    }
    if (sig == SIGSTOP) {
      if (stopping)
        return;
      continue; /* the system's own stops */
    }
    pass = sig;
  }
}

/* Puts back the slots that still hold their trap. One the dynamic linker filled in meanwhile (its
 * module loaded while the traps were armed) is left as it is: the value saved when arming is the
 * "not resolved" stub then, and writing it back crashes the game (PRX_NOT_RESOLVED_FUNCTION). */
static void put_slots_back(pid_t pid, const uint64_t *slot, const uint64_t *orig, int stopped) {
  for (unsigned i = 0; i < NTRAPS; i++) {
    uint64_t now = 0, trap = TRAP_BASE + i * 0x1000;
    if (!slot[i])
      continue;
    if (stopped ? pt_copyout(pid, slot[i], &now, 8) != 0
                : kernel_proc_copyout(pid, slot[i], &now, 8) != 0)
      continue;
    if (now != trap)
      continue;
    if (stopped)
      pt_copyin(pid, &orig[i], slot[i], 8);
    else
      kernel_proc_copyin(pid, &orig[i], slot[i], 8);
  }
}

/* The game is attached and stopped. 0 = agent loaded, -1 = no trap fired in 20 s or the load
 * failed, -3 = the game ended. Leaves the game stopped (unless it ended). */
static int inject_by_trap(pid_t pid) {
  uint64_t slot[NTRAPS], orig[NTRAPS];
  int armed = 0;
  for (unsigned i = 0; i < NTRAPS; i++) {
    uint64_t trap = TRAP_BASE + i * 0x1000;
    slot[i] = slot_of(k_trap_nids[i]);
    orig[i] = 0;
    if (!slot[i] || pt_copyout(pid, slot[i], &orig[i], 8) != 0 ||
        (g_unresolved && orig[i] == g_unresolved) || /* its module isn't loaded yet */
        pt_copyin(pid, &trap, slot[i], 8) != 0) {
      slot[i] = 0;
      continue;
    }
    armed++;
  }
  say("trap: %d import slots armed (unresolved stub 0x%lx)", armed, (long)g_unresolved);
  int sig = 0, hit = -1, st = 0;
  for (int waited = 0; hit < 0;) {
    pt_tid = 0;
    if (pt_continue(pid, sig) != 0)
      break;
    pid_t w = 0;
    for (; waited < 20000; waited++) {
      if ((w = waitpid(pid, &st, WNOHANG)) == -1 && errno == EINTR)
        w = 0; /* a signal to this payload, not the game's news */
      if (w != 0)
        break;
      usleep(1000);
    }
    if (w == 0) { /* nothing fired: stop the game again to put the slots back */
      int still = 0;
      for (unsigned i = 0; i < NTRAPS; i++) {
        uint64_t now = 0;
        still += slot[i] && kernel_proc_copyout(pid, slot[i], &now, 8) == 0 &&
                 now == TRAP_BASE + i * 0x1000;
      }
      say("trap: nothing fired in 20 s (%d of %d traps still in place)", still, armed);
      kill(pid, SIGSTOP);
      for (int t = 0; t < 3000 && (w = waitpid(pid, &st, WNOHANG)) == 0; t++)
        usleep(1000);
      if (w == 0) { /* it can't be stopped: put the slots back without stopping it */
        put_slots_back(pid, slot, orig, 0);
        say("trap: the game didn't stop, slots put back");
        return -2;
      }
      break;
    }
    if (w != pid || !WIFSTOPPED(st)) {
      if (w == pid)
        say("trap: game ended (status 0x%x)", st);
      else
        say("trap: game lost (wait errno %d)", errno);
      return -3;
    }
    sig = WSTOPSIG(st);
    pt_tid = pt_event_lwp(pid);
    struct reg r;
    if (pt_tid > 0 && pt_getregs(pid, &r) == 0 && (hit = trap_index(r.r_rip)) >= 0) {
      say("trap: thread %d called %s", pt_tid, k_trap_nids[hit]);
      break;
    }
    if (sig == SIGSTOP) /* the system's own stops while the game starts */
      sig = 0;
  }
  put_slots_back(pid, slot, orig, 1);
  /* Every thread at a trap goes on to the real function before anything runs: left there, one
   * the agent's boot call lets run would fault again and again. */
  move_off_traps(pid, slot, orig);
  int rc = hit >= 0 ? load_agent(pid) : -1;
  move_off_traps(pid, slot, orig);
  drop_trap_faults(pid, slot, orig);
  pt_tid = 0;
  return rc;
}

/* inject: the agent goes in at the eboot's entry point (inject_at_entry). When that has passed
 * (this payload was sent with the game running), it goes in through the traps (inject_by_trap):
 * the import slots of the game's first PS VR calls and of calls it makes all the time point at
 * unmapped pages, and the first thread that makes one stops in game code at a call boundary,
 * where the agent is mapped and booted, and that call goes on into the agent's hook. If the
 * game's dialog is up by then, the agent's dialog hooks close it. 0 = in, -2 = the game can't be
 * stopped (too late), -1 = failed. */
/* The value an import slot holds while its module isn't loaded yet (g_unresolved): the "not
 * resolved" stub, the one value many slots share (a bound slot holds its own function). */

static int cmp_u64(const void *a, const void *b) {
  uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
  return x < y ? -1 : x > y;
}

static void find_unresolved(pid_t pid) {
  g_unresolved = 0;
  uint64_t *v = calloc(g_nslots ? g_nslots : 1, 8);
  int n = 0;
  if (!v)
    return;
  for (int k = 0; k < g_nslots; k++)
    if (kernel_proc_copyout(pid, g_slots[k].slot, &v[n], 8) == 0)
      n++;
  qsort(v, n, 8, cmp_u64);
  int best = 0;
  for (int i = 0, j; i < n; i = j) {
    for (j = i; j < n && v[j] == v[i]; j++)
      ;
    if (j - i > best && v[i])
      best = j - i, g_unresolved = v[i];
  }
  if (best < 4)
    g_unresolved = 0;
  free(v);
}

/* the game's own calls are bound: sceKernelUsleep's slot no longer holds the stub */
static int slots_bound(pid_t pid) {
  uint64_t v = 0, s = slot_of("1jfXLRVzisc"); /* sceKernelUsleep */
  if (!s)
    s = slot_of("4J2sUJmuHZQ"); /* sceKernelGetProcessTime */
  if (!s || pt_copyout(pid, s, &v, 8) != 0 || v < 0x100000000ull)
    return 0;
  find_unresolved(pid);
  return v != g_unresolved;
}

/* ---- at the entry point ----
 * The first attach comes while the game is still in libkernel's start code, with its modules
 * already linked. Traps armed there never fired in 1.0.2 (on two consoles): the game reached its
 * "connect PS VR" dialog first, and the agent only got in 20 s later on the re-armed traps, with
 * ShellUI's dialog on screen. A breakpoint on the eboot's entry point is code, not an import
 * slot: the main thread stops there before any of the game's own code has run, the agent goes in
 * on it, and its dialog hooks answer the game's first dialog call, so no real dialog opens. */
static uint64_t eboot_entry(pid_t pid) {
  uint8_t info[0x80], b[0x1000];
  char tid[10], path[96];
  memset(info, 0, sizeof info);
  if (sceKernelGetAppInfo(pid, info) != 0)
    return 0;
  memcpy(tid, info + 0x10, 9);
  tid[9] = 0;
  /* A patched game runs the patch's eboot, mounted beside the base game's as -patch0 (its version
   * is then not 1.00, and the game is refused) */
  snprintf(path, sizeof path, "/mnt/sandbox/pfsmnt/%s-patch0/eboot.bin", tid);
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    snprintf(path, sizeof path, "/mnt/sandbox/pfsmnt/%s-app0/eboot.bin", tid);
    fd = open(path, O_RDONLY);
  }
  if (fd < 0)
    return 0;
  say("entry: from %s", path);
  ssize_t n = read(fd, b, sizeof b);
  close(fd);
  size_t eo = 0;
  if (n >= 0x20 && *(uint32_t *)b == 0x1D3D154F) /* SELF: the ELF header follows its segment table */
    eo = 0x20 + 32 * (size_t) * (uint16_t *)&b[0x18];
  if (n < 0 || eo + 0x40 > (size_t)n || memcmp(b + eo, "\x7f" "ELF", 4) != 0)
    return 0;
  const Elf64_Ehdr *eh = (const Elf64_Ehdr *)(b + eo);
  uint64_t lo = ~0ull;
  for (int i = 0; i < eh->e_phnum; i++) {
    size_t po = eo + eh->e_phoff + (size_t)i * sizeof(Elf64_Phdr);
    if (po + sizeof(Elf64_Phdr) > (size_t)n)
      return 0;
    const Elf64_Phdr *ph = (const Elf64_Phdr *)(b + po);
    if (ph->p_type == PT_LOAD && ph->p_vaddr < lo)
      lo = ph->p_vaddr;
  }
  uint64_t base = eboot_mapbase(pid);
  return !base || lo == ~0ull ? 0 : base + eh->e_entry - lo;
}

/* The settings menu's file, menu-<key>.txt (the agent writes it, CMD 14: the lines the player
 * changed from the defaults, in cvars.txt's names; it is read after cvars.txt and the game's own,
 * so its lines win): the number of the line `key=` in it, 1 when there is one. Both builds: the
 * debug menu holds the settings menu's lines too (said once in a debug build, as the file can
 * override a test's cvars.txt or resolution.txt). */
static int menu_file_value(const char *key, unsigned *v) {
  char path[96], line[128];
  FILE *f = game_file(path, sizeof path, "menu") ? fopen(path, "r") : NULL;
  if (!f)
    return 0;
  int found = 0;
  size_t k = strlen(key);
  while (fgets(line, sizeof line, f))
    if (strncmp(line, key, k) == 0 && line[k] == '=')
      *v = (unsigned)strtoul(line + k + 1, NULL, 10), found = 1;
#ifndef RELEASE
  static int said;
  if (!said++)
    say("%s: the settings menu's file is there: its lines win over cvars.txt's and resolution.txt", path + sizeof OUT_DIR);
#endif
  fclose(f);
  return found;
}
/* The build the game's code is, among its title's in k_builds: `want` (its version's) when its code
 * is that build's, else the one other build of the title whose code it is; -1 for none. A build's
 * code: its three signature stretches (k_builds' sig: two calls to imports and the VR setup's one
 * differing instruction, each at the build's own place). Read from the game's image before
 * anything is written to it. */
/* 1 the build's code, 0 not, -1 the image not readable (yet) */
static int build_code_is(pid_t pid, uint64_t base, int b) {
  const struct game_build *gb = &k_builds[b];
  for (unsigned i = 0; i < 3; i++) {
    uint8_t cur[13];
    if (kernel_proc_copyout(pid, base + gb->sig[i].at, cur, gb->sig[i].len) != 0)
      return -1;
    if (memcmp(cur, gb->sig[i].bytes, gb->sig[i].len) != 0)
      return 0;
  }
  return 1;
}
/* (-2: the image not readable yet, asked again on the next pass) */
static int build_by_code(pid_t pid, uint64_t base, int want, const char *tid) {
  int r = want >= 0 ? build_code_is(pid, base, want) : 0;
  if (r)
    return r > 0 ? want : -2;
  int found = -1, n = 0;
  for (int i = 0; i < N_BUILDS; i++) {
    if (i == want || strncmp(k_builds[i].title_id, tid, 9) != 0)
      continue;
    if ((r = build_code_is(pid, base, i)) < 0)
      return -2;
    if (r)
      found = i, n++;
  }
  if (n == 1)
    say("the game's code is %s %s's (its version said %s): taken as that build", k_builds[found].name,
        k_builds[found].version, want >= 0 ? k_builds[want].version : "none known");
  else
    say("the game's code is no build this payload knows (%d match): left alone", n);
  return n == 1 ? found : -1;
}

static void log_vmmap(pid_t game, uint64_t floor, const char *when);

/* resolution.txt (or resolution-<key>.txt, the game's own): the width of an eye view in pixels.
 * Dead or Alive Xtreme 3 makes its four views (each eye's wide and near one, slices of one array
 * texture, two sets) at a size read once at its boot from two constants: 940.8 x 1052.8 in the
 * PS4 Pro mode it runs in on a PS5 (truncated to 940 x 1052). Written here at the entry point,
 * before any of the game's code has run, over the stock values only: the width asked, the height
 * in the game's own proportion. "stock" in it: the game's own; no file: EYE_W_DEFAULT. */
#define EYE_W_MIN 672u
/* (2400 since 2026-11-13: supersampling, the composed eye kept 3848x4096, the wider views thinned
 * out by the compose pass and the game's FXAA blur with them. 2400 measured clean in Scarlet at 60
 * fps: every frame composed and shown, the game's GPU 5.2-6.0 ms, compose 0.9 ms. 2720 left the
 * compose pass without memory ("496 MB": off) and the headset showing nothing new: logs/perf/ss-*.log) */
#ifdef RELEASE
#define EYE_W_MAX 2720u /* (2720 for 60 fps, the menu's top: taken out 2026-11-14 morning for the scenery it
                         * lost, back that evening: that was the game's GPU fallback (patch_gpufallback); with
                         * it fixed, the user's recording at 60 fps: 600 a 10 s, every frame shown, GPU 8.9 ms,
                         * the scenery kept; logs/perf/stage47.out m-2720) */
#else
#define EYE_W_MAX 3200u /* (a debug build: the widths above 2400 tried by resolution.txt) */
#endif
/* Without a file: 2048, the most (EYE_W_MAX). Swept 2026-11-11 with PS VR2 started (logs/sweep, a
 * Photo Paradise pose scene by the sea): 1600 and 1800 every frame shown, the game's GPU 4.7 / 5.0
 * ms, compose 0.54 / 0.70 ms. 2048 first showed the eye's middle empty: the agent composed each eye
 * 4096x4368, and more than 4096 rows shows the wide view's black disc (the near view was drawn: its
 * slice shot, 2026-11-12). With the composed eye kept to 4096 rows (3848x4096) 2048 is whole in both
 * games, every frame composed, the game's GPU 5.0 ms, compose 0.78 ms. The composed eye then holds
 * the near view at 0.9 of its density: wider views would only be thinned out again. */
#define EYE_W_DEFAULT 2048u /* (the 60 fps default preset's eye width, RES_SS2048) */

/* agent.vdensity=P (the Resolution preset's, RES_SHARPP's 96; a debug build's cvars.txt too; P from
 * 80 to 99, percent; the caller passes g_res.vd): every VR view's height P% of the
 * game's, for an even pixel density both ways (logs/research/gpu120/DENSITY-FXAA.md). The game keeps
 * PS VR's 672:752 for its views while their projections are built from tangents alone (the camera's
 * aspect too: (f0+f1)/(f2+f3), 0x6f07a5 Scarlet), so its pixels are 12% (near) to 17% (wide) denser
 * up and down than across; PS VR2's panel is about 1.07. P = 96 brings the near views to about the
 * panel's 1.07, 89 to square pixels. Three heights follow P together:
 * - render mode 3 (the near views; the wide ones too without agent.widew): the 752 of the immediate
 *   `movabsq $0x2f0000002a0, %rax` (672 | 752 << 32, Scarlet 0x4ae415, Fortune 0x4ad7a5; the boot's
 *   one write of mode 3's size, before the Pro factor scales both and the table is built, read once)
 *   made round(752 P / 100);
 * - the eye slices (patch_eye_size): that height times the eye width's factor (w + 0.5) / 672, the
 *   same arithmetic as the game's for mode 3, so an eye width equal to agent.renderw stays 1:1;
 * - render mode 1 with agent.widew (patch_viewsize): agent.vdensitywide=Q if given, else P.
 * Everything that reads a view's size per axis follows on its own (the masks, the hole, the FXAA
 * pass's texel size, the compose pass's uv and output size, the viewports to Hmd2: the DENSITY
 * report's list); widths, the caps (vs_cap, with the game's taller height) and the projections are
 * left. The immediate's bytes are compared first; written at the entry point, before any of the
 * game's code has run. */
static unsigned g_vd_h3; /* mode 3's height before the Pro factor with agent.vdensity, 0: the game's 752 */
#define VD_MIN 80u
#define VD_MAX 99u
static unsigned vd_base(unsigned pct) { return (unsigned)(752.0 * pct / 100.0 + 0.5); }
static const struct {
  const char *title;
  uint64_t va; /* movabsq $0x2f0000002a0, %rax: mode 3's 672x752 */
} k_vdensity[] = {
    {"CUSA14173", 0x4ae415},
    {"CUSA14831", 0x4ac955},
    {"CUSA04555", 0x4ad7a5},
};
static void patch_vdensity(pid_t pid, unsigned p) {
  if (g_build < 0 || !p)
    return;
  if (p < VD_MIN || p > VD_MAX) {
    say("vdensity: agent.vdensity is from %u to %u (percent of the views' height): the game's own", VD_MIN, VD_MAX);
    return;
  }
  for (int i = 0; i < (int)(sizeof k_vdensity / sizeof k_vdensity[0]); i++) {
    if (strcmp(k_builds[g_build].title_id, k_vdensity[i].title) != 0)
      continue;
    uint64_t base = eboot_mapbase(pid);
    static const uint8_t orig[10] = {0x48, 0xb8, 0xa0, 0x02, 0x00, 0x00, 0xf0, 0x02, 0x00, 0x00};
    uint8_t buf[10];
    uint32_t h3 = vd_base(p);
    if (!base || pt_copyout(pid, base + k_vdensity[i].va, buf, 10) != 0 || memcmp(buf, orig, 10) != 0) {
      say("vdensity: mode 3's size at VA 0x%lx not as expected: left as it is", (unsigned long)k_vdensity[i].va);
      return;
    }
    if (pt_copyin(pid, &h3, base + k_vdensity[i].va + 6, 4) != 0) {
      say("vdensity: the patch could not be written");
      return;
    }
    g_vd_h3 = h3;
    say("vdensity: the views %u%% of the game's height: mode 3 672x%u before the Pro factor (the game's "
        "672x752); the eye slices follow", p, h3);
  }
}


/* The settings menu's "Resolution*" (agent.res, a preset; docs/SETTINGS.md): the game's real render
 * size (agent.renderw: mode 3, the sharp near views, and the wide ones too without widew), the sharp
 * area's angle (agent.nearn), the wide views' own size (agent.widew: mode 1) and the eye width (the
 * last pass, FXAA's upsample into the eye's slices: patch_eye_size), all patched at the game's
 * start. Measured on the user's worn recording replayed at 120 fps (logs/perf/stage30.out): renderw
 * 1280 + widew 940 + nearn 40 + eye 1280-1440 clean (11 of 12 10 s windows): RES_SHARP, the default
 * at 120 fps. The 60 fps presets (supersampled eye widths) at 120 fps are RES_SHARP. Where it comes
 * from, the first there is: the menu's file's agent.res; an older menu's agent.resolution=N (the
 * eye width it had, mapped: res_legacy); cvars.txt's agent.res; the default (RES_SHARP at 120 fps,
 * RES_SS2048 at 60, the old default eye width with the sharp render size). resolution.txt (an eye
 * width) still sets the eye width when the menu's file has no line. A debug build's cvars.txt
 * agent.renderw / agent.nearn / agent.widew (the raw sizes) win over the preset's, each on its own
 * (0: that one off); a release build ignores them (RES-MENU.md: logs/research/gpu120). The eye
 * width follows the render size: never less than renderw, within the 120 fps cap.
 * RES_SHARPP, "sharp+ (120 fps)" (appended: the older presets keep their numbers): render 1440 on
 * mode 0 at 2688x1512 (patch_mode0: the game's boot-time base, flat screens drawn at that size
 * too), the heights at 96% (the sharp views) and 91% (the wide ones) of the game's (patch_vdensity:
 * the panel's 1.07 pixel shape, fewer rows to draw), the wide views at 700, the eye = the render
 * size (1440, let through the 120 fps cap of 1400: that cap is for eye widths past the render
 * size). Measured on the user's recording at 120 fps (logs/perf/chain37.out, combo1440b, with
 * agent.nearcull=1 too, which barely engaged): 11 of 12 windows clean, 19 repeats, GPU 6.62 ms on
 * average, as steady as RES_SHARP (logs/research/gpu120/SHARPPLUS.md). Not the default until worn.
 * A preset's mode0 / vdensity are in a release build too (the raw cvars only in a debug one); if
 * mode 0 cannot be patched the preset falls back to RES_SHARP whole (res_mode0_check). */
enum { RES_GAME, RES_SHARP, RES_WIDE, RES_SS2048, RES_SS2400, RES_SHARPP, RES_SS2720, RES_N };
static const struct {
  const char *name;
  unsigned renderw, nearn, widew, eye, only60; /* eye 0: the game's own */
  unsigned vd, vdw, mode0; /* agent.vdensity, agent.vdensitywide, agent.mode0 (0: the game's own) */
} k_res[RES_N] = {
    {"the game's own", 0, 0, 0, 0, 0, 0, 0, 0},
    {"sharp (120 fps)", 1280, 40, 940, 1400, 0, 0, 0, 0},
    {"sharp, wide area (60 fps)", 1280, 0, 0, 1400, 1, 0, 0, 0}, /* (at 120 fps 5 of 12 windows clean, 7.2 ms: p2b) */
    {"sharp + 2048 eye (60 fps)", 1280, 0, 0, EYE_W_DEFAULT, 1, 0, 0, 0},
    {"sharp + 2400 eye (60 fps)", 1280, 0, 0, 2400, 1, 0, 0, 0},
    {"sharp+ (120 fps)", 1440, 40, 700, 1440, 0, 96, 91, 2688},
    {"sharp + 2720 eye (60 fps)", 1280, 0, 0, 2720, 1, 0, 0, 0}, /* (logs/perf/stage47.out m-2720) */
};
static struct {
  int done, preset;
  unsigned renderw, nearn, widew, eye; /* eye 0: the game's own */
  unsigned vd, vdw, mode0;             /* (0: off; vdw 0: the wide views follow vd) */
} g_res;
/* an older menu's eye width (agent.resolution=N, the menu's "Eye width*" before 2026-11-14) as a
 * preset: 0 the game's own; up to 1400 (all that 120 fps gave) the sharp one; 1600-2048 and more the
 * 60 fps ones (RES_SHARP at 120 fps, as anything over 1400 was 1400 there) */
static int res_legacy(unsigned w) {
  return !w ? RES_GAME : w <= 1400 ? RES_SHARP : w <= 2048 ? RES_SS2048 : w <= 2400 ? RES_SS2400 : RES_SS2720;
}
static int fps120_on(void) {
  unsigned v = (unsigned)cvar_value("agent.fps120", 1); /* (on by default) */
  menu_file_value("agent.fps120", &v);                  /* the settings menu's line wins */
  return v != 0;
}
static void res_settings(void) {
  if (g_res.done)
    return;
  g_res.done = 1;
  int fps120 = fps120_on(), p, menu_line = 1, def = fps120 ? RES_SHARPP : RES_SS2048; /* (sharp+ at 120 by default: the user, 2026-11-14) */
  unsigned v = 0;
  const char *from;
  unsigned long c;
  if (menu_file_value("agent.res", &v))
    p = (int)v, from = "the settings menu";
  else if (menu_file_value("agent.resolution", &v))
    p = res_legacy(v), from = "an older menu's eye width, mapped";
  else if ((c = cvar_value("agent.res", ~0ul)) != ~0ul)
    p = (int)c, menu_line = 0, from = "cvars.txt";
  else
    p = def, menu_line = 0, from = "the default";
  if (p < 0 || p >= RES_N) {
    say("resolution: %d is not a preset (0 to %d): the default", p, RES_N - 1);
    p = def, from = "the default";
  }
  if (fps120 && k_res[p].only60) {
    say("resolution: %s is for 60 fps: at 120 fps %s", k_res[p].name, k_res[RES_SHARP].name);
    p = RES_SHARP;
  }
  g_res.preset = p;
  g_res.renderw = k_res[p].renderw, g_res.nearn = k_res[p].nearn, g_res.widew = k_res[p].widew;
  g_res.eye = k_res[p].eye;
  g_res.vd = k_res[p].vd, g_res.vdw = k_res[p].vdw, g_res.mode0 = k_res[p].mode0;
  say("resolution: %s (agent.res=%d, %s; %u fps): render %u, sharp area %u/100, wide views %u, eye %u (0: the "
      "game's own)", k_res[p].name, p, from, fps120 ? 120 : 60, g_res.renderw, g_res.nearn ? g_res.nearn : 50,
      g_res.widew, g_res.eye);
  if (g_res.mode0 || g_res.vd)
    say("resolution: %s also: mode 0 %u (0: the game's 1920), heights %u%% sharp / %u%% wide (0: the game's)",
        k_res[p].name, g_res.mode0, g_res.vd, g_res.vdw ? g_res.vdw : g_res.vd);
  if (!menu_line) { /* resolution.txt (the game's own, else the common one): the eye width */
    char path[96], line[64] = {0};
    FILE *f = game_file(path, sizeof path, "resolution") ? fopen(path, "r") : NULL;
    if (!f)
      f = fopen(OUT_DIR "/resolution.txt", "r");
    if (f) {
      unsigned w = 0;
      if (fgets(line, sizeof line, f) && (strncmp(line, "stock", 5) == 0 || sscanf(line, "%u", &w) == 1)) {
        g_res.eye = w;
        say("resolution: resolution.txt's eye width %u (0: the game's own)", w);
      }
      fclose(f);
    }
  }
#ifndef RELEASE
  /* a debug build: cvars.txt's raw sizes over the preset's */
  static const char *const k_raw[6] = {"agent.renderw", "agent.nearn",         "agent.widew",
                                       "agent.vdensity", "agent.vdensitywide", "agent.mode0"};
  unsigned *raw[6] = {&g_res.renderw, &g_res.nearn, &g_res.widew, &g_res.vd, &g_res.vdw, &g_res.mode0};
  for (int i = 0; i < 6; i++)
    if ((c = cvar_value(k_raw[i], ~0ul)) != ~0ul && (unsigned)c != *raw[i]) {
      say("resolution: %s=%u (cvars.txt) over the preset's %u", k_raw[i], (unsigned)c, *raw[i]);
      *raw[i] = (unsigned)c;
    }
#endif
}
static void patch_eye_size(pid_t pid) {
  if (g_build < 0 || !k_builds[g_build].eye_w_at)
    return;
  const struct game_build *gb = &k_builds[g_build];
  res_settings();
  unsigned w = g_res.eye;
  if (g_res.renderw > w) { /* (the eye follows the render size: an eye narrower than the views thins them) */
    if (w)
      say("resolution: eye %u is narrower than the render size %u: %u", w, g_res.renderw, g_res.renderw);
    w = g_res.renderw;
  }
  if (!w) {
    if (g_vd_h3) { /* (agent.vdensity: the game's width, the height as mode 3's at the Pro factor 1.4) */
      uint64_t base = eboot_mapbase(pid);
      float cur = 0, want = (float)(unsigned)((double)g_vd_h3 * 1.4) + 0.5f;
      if (!base || pt_copyout(pid, base + gb->eye_h_at, &cur, 4) != 0 || cur != 1052.8f ||
          pt_copyin(pid, &want, base + gb->eye_h_at, 4) != 0) {
        say("resolution: the game's eye height at 0x%x not the stock one or not written: the game's own "
            "940x1052 (agent.vdensity's mode 3 drawn shorter all the same)", gb->eye_h_at);
        return;
      }
      say("resolution: the game's own eye width, eye views 940x%u (agent.vdensity)", (unsigned)want);
      return;
    }
    say("resolution: the game's own eye views (940x1052)");
    return;
  }
  if (w > EYE_W_MAX && w <= 4096) { /* (a menu's 2720 from before 2026-11-14: the most there is now) */
    say("resolution: %u is more than %u: %u", w, EYE_W_MAX, EYE_W_MAX);
    w = EYE_W_MAX;
  }
  if (w < EYE_W_MIN || w > EYE_W_MAX) {
    say("resolution: %u is not a view width from %u to %u: the game's own", w, EYE_W_MIN, EYE_W_MAX);
    return;
  }
  /* at 120 fps, 1400 the most (2026-11-14, the user's worn recording replayed, the sharp view at 1280,
   * 2 min in a scene: 1280 11 of 12 10 s windows clean, 12 repeats, GPU 6.6 ms on average; 1440 11 of
   * 12, 24 repeats, 6.8 ms; 1600 5 of 12, 61; 2048 2 of 12, 969 repeats, 7.3 ms, 10.3 at the most:
   * the stutter the user felt worn; logs/perf/stage30.out). agent.eyecap120=0 (cvars.txt) lets a
   * debug run past it. The cap is for eye widths past the render size: a preset whose eye is its
   * render size (RES_SHARPP: 1440 = 1440, measured clean at 120 fps, logs/perf/chain37.out
   * combo1440b) raises it to that eye */
  unsigned fps120 = (unsigned)fps120_on(), cap = (unsigned)cvar_value("agent.eyecap120", 1), lim = 1400;
  if (k_res[g_res.preset].eye > lim && k_res[g_res.preset].eye == k_res[g_res.preset].renderw)
    lim = k_res[g_res.preset].eye;
  if (fps120 && cap && w > lim) {
    say("resolution: %u at 120 fps is more than the GPU keeps up with: %u (the most at 120 fps%s)", w, lim,
        lim > 1400 ? ", the preset's eye = its render size" : "");
    w = lim;
  }
  uint64_t base = eboot_mapbase(pid);
  /* the height in the game's proportion; with agent.vdensity mode 3's shorter one times the width's
   * factor, as the game makes mode 3's (so eye width = agent.renderw stays 1:1) */
  unsigned eh = g_vd_h3 ? (unsigned)((double)g_vd_h3 * (((double)w + 0.5) / 672.0))
                        : (unsigned)((float)w * 1052.8f / 940.8f);
  float cur[2] = {0, 0}, want[2] = {(float)w + 0.5f, (float)eh + 0.5f};
  const float stock[2] = {940.8f, 1052.8f};
  const uint32_t at[2] = {gb->eye_w_at, gb->eye_h_at};
  for (int i = 0; i < 2; i++)
    if (!base || pt_copyout(pid, base + at[i], &cur[i], 4) != 0 || cur[i] != stock[i]) {
      say("resolution: the game's eye size constant at 0x%x is not the stock one: left alone", at[i]);
      return;
    }
  for (int i = 0; i < 2; i++)
    if (pt_copyin(pid, &want[i], base + at[i], 4) != 0) {
      say("resolution: writing the eye size at 0x%x failed", at[i]);
      return;
    }
  say("resolution: eye views %ux%u (the game's own 940x1052), from the game's boot%s", w, (unsigned)want[1],
      g_vd_h3 ? " (agent.vdensity)" : "");
}

/* The cut features re-enabled, in both games: items and poses that are in the game's data but
 * switched off for this platform, by a per-platform marker in the unlock rows and an availability
 * byte in the item master (Scarlet: 60 rows, the fans and skincare among them; Fortune: 3). The
 * data is unpacked onto the heap, so the eboot's tests are patched instead. Everything is let
 * through, the other platforms' items too (asked for: to be tried, not hidden). */
/* cvars.txt (or the game's own cvars file) line agent.cutfeatures=0: the cut features left as the
 * game has them (a stock comparison); anything else, or no line: patched. */
/* a cvar's value from cvars.txt and the game's own cvars file (the later line wins); def without one */
static unsigned long cvar_value(const char *name, unsigned long def) {
  char line[128], game[96];
  size_t n = strlen(name);
  const char *paths[2] = {OUT_DIR "/cvars.txt", game_file(game, sizeof game, "cvars")};
  for (int i = 0; i < 2; i++) {
    FILE *f = paths[i] ? fopen(paths[i], "r") : NULL;
    if (!f)
      continue;
    while (fgets(line, sizeof line, f))
      if (strncmp(line, name, n) == 0 && line[n] == '=')
        def = strtoul(line + n + 1, NULL, 10);
    fclose(f);
  }
  return def;
}
static int cut_features_wanted(void) { return cvar_value("agent.cutfeatures", 1) != 0; }
static void log_sync_read(void) {
  char line[128], game[96];
  int on = 0;
  const char *paths[2] = {OUT_DIR "/cvars.txt", game_file(game, sizeof game, "cvars")};
  for (int i = 0; i < 2; i++) {
    FILE *f = paths[i] ? fopen(paths[i], "r") : NULL;
    if (!f)
      continue;
    while (fgets(line, sizeof line, f))
      if ((strncmp(line, "agent.hands=", 12) == 0 && strtoul(line + 12, NULL, 10)) ||
          (strncmp(line, "agent.netlog=", 13) == 0 && strtoul(line + 13, NULL, 10)) ||
          (strncmp(line, "agent.handload=", 15) == 0 && strtoul(line + 15, NULL, 10)) ||
          (strncmp(line, "agent.handstep=", 15) == 0 && strtoul(line + 15, NULL, 10)))
        on = 1;
    fclose(f);
  }
#ifdef RELEASE
  on = 0; /* (no log port in a release build: its log stays on the storage) */
#endif
  if (on)
    say("log: to port %d only while the game runs, none to the storage (agent.hands or agent.netlog; "
        "a client %s)", NETLOG_PORT, g_netfd >= 0 ? "connected" : "NOT connected");
  g_netonly = on;
}
#ifndef RELEASE
/* the log port's listener: one client at a time, the newest */
static void *netlog_thread(void *arg) {
  (void)arg;
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons(NETLOG_PORT);
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  int s = socket(AF_INET, SOCK_STREAM, 0), one = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  if (bind(s, (struct sockaddr *)&a, sizeof a) != 0 || listen(s, 2) != 0) {
    close(s);
    return NULL;
  }
  for (;;) {
    int c = accept(s, NULL, NULL);
    if (c < 0) {
      usleep(100000);
      continue;
    }
    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    pthread_mutex_lock(&g_netlock);
    if (g_netfd >= 0)
      close(g_netfd);
    g_netfd = c;
    pthread_mutex_unlock(&g_netlock);
  }
  return NULL;
}
#endif
static void patch_cut_features(pid_t pid) {
  if (g_build < 0)
    return;
  if (!cut_features_wanted()) {
    say("cut features: agent.cutfeatures=0: left as the game has them");
    return;
  }
  /* The unlock rows' platform marker (byte +4), tested at six sites: Scarlet skips markers 4, 5
   * and 6 (mov; add -4; movzx; cmp 3), Fortune 4 and 5 (mov; and 0xfe; movzx; cmp 4). The cmp's
   * immediate, each sequence's third byte from its end, is changed to a value the test never
   * meets (Scarlet 0: nothing is below it; Fortune 1: the masked byte is even), so no row is
   * skipped: Scarlet's fans, skincare and 13 poses, Fortune's Venus swimsuit and Tilt-Sensing
   * Camera, the Silver Fan in both. Then the item master's availability byte (cmp byte
   * [r15+N], 0; je): the je NOPed. */
  static const struct site { uint32_t va; uint8_t len, expect[14]; } scarlet[] = {
    {0x4134a3, 13, {0x8a,0x41,0x04, 0x04,0xfc, 0x0f,0xb6,0xc0, 0x83,0xf8,0x03, 0x73,0x07}},
    {0x40bbb4, 14, {0x8a,0x5a,0x04, 0x80,0xc3,0xfc, 0x0f,0xb6,0xf3, 0x83,0xfe,0x03, 0x72,0x1e}},
    {0x3ad15e, 14, {0x8a,0x51,0x04, 0x80,0xc2,0xfc, 0x0f,0xb6,0xd2, 0x83,0xfa,0x03, 0x72,0x23}},
    {0x3df3f7, 14, {0x8a,0x51,0x04, 0x80,0xc2,0xfc, 0x0f,0xb6,0xd2, 0x83,0xfa,0x03, 0x72,0x23}},
    {0x420f56, 14, {0x8a,0x51,0x04, 0x80,0xc2,0xfc, 0x0f,0xb6,0xd2, 0x83,0xfa,0x03, 0x72,0x23}},
    {0x554acf, 14, {0x8a,0x51,0x04, 0x80,0xc2,0xfc, 0x0f,0xb6,0xd2, 0x83,0xfa,0x03, 0x72,0x23}},
  }, scarlet100[] = { /* CUSA14831 1.00: the same bytes */
    {0x412013, 13, {0x8a,0x41,0x04, 0x04,0xfc, 0x0f,0xb6,0xc0, 0x83,0xf8,0x03, 0x73,0x07}},
    {0x40a724, 14, {0x8a,0x5a,0x04, 0x80,0xc3,0xfc, 0x0f,0xb6,0xf3, 0x83,0xfe,0x03, 0x72,0x1e}},
    {0x3acaae, 14, {0x8a,0x51,0x04, 0x80,0xc2,0xfc, 0x0f,0xb6,0xd2, 0x83,0xfa,0x03, 0x72,0x23}},
    {0x3ddf47, 14, {0x8a,0x51,0x04, 0x80,0xc2,0xfc, 0x0f,0xb6,0xd2, 0x83,0xfa,0x03, 0x72,0x23}},
    {0x41fb8c, 14, {0x8a,0x51,0x04, 0x80,0xc2,0xfc, 0x0f,0xb6,0xd2, 0x83,0xfa,0x03, 0x72,0x23}},
    {0x552fdf, 14, {0x8a,0x51,0x04, 0x80,0xc2,0xfc, 0x0f,0xb6,0xd2, 0x83,0xfa,0x03, 0x72,0x23}},
  }, fortune[] = {
    {0x3ad69e, 14, {0x8a,0x51,0x04, 0x80,0xe2,0xfe, 0x0f,0xb6,0xd2, 0x83,0xfa,0x04, 0x74,0x23}},
    {0x3decd7, 14, {0x8a,0x51,0x04, 0x80,0xe2,0xfe, 0x0f,0xb6,0xd2, 0x83,0xfa,0x04, 0x74,0x23}},
    {0x409894, 14, {0x8a,0x5a,0x04, 0x80,0xe3,0xfe, 0x0f,0xb6,0xf3, 0x83,0xfe,0x04, 0x74,0x1e}},
    {0x411183, 13, {0x8a,0x41,0x04, 0x24,0xfe, 0x0f,0xb6,0xc0, 0x83,0xf8,0x04, 0x75,0x07}},
    {0x41ecd6, 14, {0x8a,0x51,0x04, 0x80,0xe2,0xfe, 0x0f,0xb6,0xd2, 0x83,0xfa,0x04, 0x74,0x23}},
    {0x5549df, 14, {0x8a,0x51,0x04, 0x80,0xe2,0xfe, 0x0f,0xb6,0xd2, 0x83,0xfa,0x04, 0x74,0x23}},
  };
  /* Scarlet: 1.08 (CUSA14173) or 1.00 (CUSA14831, the same code at its own addresses) */
  int s100 = strcmp(k_builds[g_build].title_id, "CUSA14831") == 0;
  int is_scarlet = s100 || strcmp(k_builds[g_build].title_id, "CUSA14173") == 0;
  const struct site *sites = s100 ? scarlet100 : is_scarlet ? scarlet : fortune;
  uint8_t never = is_scarlet ? 0x00 : 0x01;
  uint32_t item_at = s100 ? 0x47923b : is_scarlet ? 0x47acfb : 0x478f4b;
  uint8_t item_expect[11] = {0x41,0x80,0x7f, is_scarlet ? 0x38 : 0x28, 0x00, 0x0f,0x84,0x31,0x01,0x00,0x00};
  uint64_t base = eboot_mapbase(pid);
  if (!base)
    return;
  int patched = 0;
  for (int i = 0; i < 6; i++) {
    uint8_t buf[14];
    if (pt_copyout(pid, base + sites[i].va, buf, sites[i].len) != 0 ||
        memcmp(buf, sites[i].expect, sites[i].len) != 0)
      continue;
    if (pt_copyin(pid, &never, base + sites[i].va + sites[i].len - 3, 1) == 0)
      patched++;
  }
  uint8_t item_buf[11];
  int item_ok = 0;
  if (pt_copyout(pid, base + item_at, item_buf, 11) == 0 && memcmp(item_buf, item_expect, 11) == 0) {
    uint8_t nop6[6] = {0x66,0x0f,0x1f,0x44,0x00,0x00};
    if (pt_copyin(pid, nop6, base + item_at + 5, 6) == 0)
      item_ok = 1;
  }
  /* The shop filter (Scarlet 0x190090, Fortune 0x190a90 area; logs/research/owner-shop/REPORT.md)
   * tests the availability byte itself, for the normal shops and the Owner Shop (cmpb $0,N(reg);
   * je): both je NOPed, so the items the cut-features patch lets through can be stocked too
   * (Scarlet: the fans, skincare, Softness Sensors; Fortune: the Silver Fan, the Chinese Tea Set).
   * Their Owner Level gates stay. */
  static const struct { uint32_t va; uint8_t cmp[4], rel; } shop_s[2] = {
    {0x190248, {0x80,0x7f,0x38,0x00}, 0xd8}, {0x190279, {0x80,0x7f,0x38,0x00}, 0xb7}},
  shop_f[2] = {
    {0x190c4b, {0x80,0x78,0x28,0x00}, 0xde}, {0x190c7f, {0x80,0x78,0x28,0x00}, 0xba}};
  int shop_ok = 0;
  for (int i = 0; i < 2; i++) {
    const uint8_t *c = is_scarlet ? shop_s[i].cmp : shop_f[i].cmp;
    uint32_t va = is_scarlet ? shop_s[i].va : shop_f[i].va;
    uint8_t want[10] = {c[0],c[1],c[2],c[3], 0x0f,0x84,is_scarlet ? shop_s[i].rel : shop_f[i].rel,0x00,0x00,0x00}, buf[10];
    uint8_t nop6[6] = {0x66,0x0f,0x1f,0x44,0x00,0x00};
    if (pt_copyout(pid, base + va, buf, 10) == 0 && memcmp(buf, want, 10) == 0 &&
        pt_copyin(pid, nop6, base + va + 4, 6) == 0)
      shop_ok++;
  }
  say("cut features: %d/6 unlock sites patched, item flag %s, shop filter %d/2 (%s)", patched,
      item_ok ? "cleared" : "not patched", shop_ok, k_builds[g_build].name);
  /* Scarlet's Special purchase refuses a swimsuit set (category 0x1a) paid in Zack Dollars:
   * cmp $0x1a / jne at 0x402a49, then "paying with Premium Tickets" (+0x57) or state 9 ("not
   * available"). Premium Tickets need PSN, so offline the sets could not be had at all: the jne made
   * a jmp, a set goes the Zack Dollar way as any item (Fortune's purchase has no such test). The
   * agent gives the sets a Zack Dollar price (agent.special). agent.special=0: left. */
  if (s100)
    say("special: swimsuit sets: 1.00 has no such refusal (an update's), nothing to patch");
  else if (is_scarlet && cvar_value("agent.special", 2)) {
    static const uint8_t want[19] = {0x83,0xf8,0x1a,0x75,0x0e,0x48,0x8b,0x43,0x08,0x80,0x78,0x57,0x00,
                                     0x0f,0x84,0x9b,0x04,0x00,0x00};
    uint8_t buf[19], jmp = 0xeb;
    int ok = pt_copyout(pid, base + 0x402a49, buf, 19) == 0 && memcmp(buf, want, 19) == 0 &&
             pt_copyin(pid, &jmp, base + 0x402a4c, 1) == 0;
    say("special: swimsuit sets for Zack Dollars %s", ok ? "allowed" : "not patched (bytes not as expected)");
  }
  /* Scarlet's owner swimsuits in group 6 (+0x1c): 279 of its own (the add-on swimsuit sets' suits,
   * "Swimsuit Set No. 1-12" and the Fortune add-on designs, modelled on its disc for Marie Rose,
   * Honoka, Helena, Kasumi) and the 45 ported ones. Group 6 is listed only by the Owner Shop's
   * Special tab (0x4249f0), from PSN sale data: none offline. The Swimsuits tab's two filters
   * (0x424020: the partner's, built at 0x4240b8; the other girls', 0x424201) ask for group 0
   * (movw $0, disp8(%rbp)): made 0xffff, any group. Only groups 0 and 6 have Owner Shop swimsuits,
   * so this adds exactly those, to the game's own daily draw. The rows stay group 6, which the
   * closet's insert (0x3833a1) needs for an id it has no node for. Off by default (the user,
   * 2026-11-12: restored things behave as in their uncensored games, not changed); agent.ownersuits=1. */
  if (is_scarlet && cvar_value("agent.ownersuits", 0)) {
    static const struct grp { uint32_t va; uint8_t disp; } grp108[2] = {{0x4240b8, 0xb0}, {0x424201, 0x80}},
                                                           grp100[2] = {{0x422de8, 0xb0}, {0x422f31, 0x80}};
    const struct grp *grp = s100 ? grp100 : grp108;
    int g_ok = 0;
    for (int i = 0; i < 2; i++) {
      uint8_t want[6] = {0x66, 0xc7, 0x45, grp[i].disp, 0x00, 0x00}, buf[6], any[2] = {0xff, 0xff};
      if (pt_copyout(pid, base + grp[i].va, buf, 6) == 0 && memcmp(buf, want, 6) == 0 &&
          pt_copyin(pid, any, base + grp[i].va + 4, 2) == 0)
        g_ok++;
    }
    say("owner suits: the Owner Shop's swimsuit filters take every group, %d/2 (the add-on and ported suits stocked)", g_ok);
  }
}

/* Scarlet's VR Passport entitlement: the game queries PSN for service entitlements
 * (libSceNpCommerce, /v1/users/me/entitlements); a failed query sets a flag at VA 0xaaf188
 * and a menu-item active byte at VA 0xaaf173 to 0. Two patches:
 *  1. Flag force: the mov-byte-0 at VA 0x42065d changed to mov-byte-1.
 *  2. Active byte force: the mov-byte-0 at VA 0x41ca61 changed to mov-byte-1 (the game's
 *     own "set active" path at VA 0x41cad8 also writes 1, so this matches the success case).
 * A third patch (je NOP at VA 0x420b65) was tried and removed: the je is in the menu
 * handler's setup code and NOPing it caused auto-navigation that skipped the main menu. */
/* agent.vrpassport (cvars.txt or the game's own file): 0 never patched, 1 always; no line (the
 * default): patched only when the add-on (00DLC00130000000, "VR Passport") is not installed in
 * /user/addcont/CUSA14173 (asked 2026-11-11: the user installed it; the game's own check is
 * then left to decide). */
static int vr_passport_wanted(const char *title, const char *addon) {
  char line[128], game[96];
  int want = -1;
  const char *paths[2] = {OUT_DIR "/cvars.txt", game_file(game, sizeof game, "cvars")};
  for (int i = 0; i < 2; i++) {
    FILE *f = paths[i] ? fopen(paths[i], "r") : NULL;
    if (!f)
      continue;
    while (fgets(line, sizeof line, f))
      if (strncmp(line, "agent.vrpassport=", 17) == 0)
        want = strtoul(line + 17, NULL, 10) != 0;
    fclose(f);
  }
  if (want >= 0) {
    say("vr passport: agent.vrpassport=%d", want);
    return want;
  }
  char path[96];
  struct stat st;
  snprintf(path, sizeof path, "/user/addcont/%s/%s/ac.pkg", title, addon);
  if (stat(path, &st) == 0) {
    say("vr passport: the add-on is installed: not patched (the game's own check)");
    return 0;
  }
  return 1;
}
/* Fortune 1.19 has the same two sites (found 2026-11-12 by the same code around them: the flag
 * 0xaa7098 cleared at 0x41e3a5 after the PSN check, the menu item's active byte 0xaa7083 cleared at
 * 0x41a741 before the owned-add-on lookup); its Passport add-on 10DLC00000000000. With the flag set
 * and no Passport, the game's load of the Passport's package finds none and skips it (0x41ea4c). */
static const struct {
  const char *title, *addon;
  uint64_t flag_va, active_va;
  uint8_t flag[7], active[7];
} k_passport[] = {
    {"CUSA14173", "00DLC00130000000", 0x42065d, 0x41ca61,
     {0xc6, 0x05, 0x24, 0xeb, 0x68, 0x00, 0x00}, {0xc6, 0x05, 0x0b, 0x27, 0x69, 0x00, 0x00}},
    {"CUSA14831", "00DLC00130000000", 0x41f24b, 0x41b5d1,
     {0xc6, 0x05, 0xf6, 0xfe, 0x68, 0x00, 0x00}, {0xc6, 0x05, 0x5b, 0x3b, 0x69, 0x00, 0x00}},
    {"CUSA04555", "10DLC00000000000", 0x41e3a5, 0x41a741,
     {0xc6, 0x05, 0xec, 0x8c, 0x68, 0x00, 0x00}, {0xc6, 0x05, 0x3b, 0xc9, 0x68, 0x00, 0x00}},
};
static void patch_vr_passport(pid_t pid) {
  if (g_build < 0)
    return;
  int k = -1;
  for (int i = 0; i < (int)(sizeof k_passport / sizeof k_passport[0]); i++)
    if (strcmp(k_builds[g_build].title_id, k_passport[i].title) == 0)
      k = i;
  if (k < 0 || !vr_passport_wanted(k_passport[k].title, k_passport[k].addon))
    return;
  uint64_t base = eboot_mapbase(pid);
  if (!base)
    return;
  int ok = 0;
  uint8_t buf[7], one = 0x01;
  /* 1. the flag: its mov byte ..., 0 made to write 1 */
  if (pt_copyout(pid, base + k_passport[k].flag_va, buf, 7) == 0 && memcmp(buf, k_passport[k].flag, 7) == 0) {
    if (pt_copyin(pid, &one, base + k_passport[k].flag_va + 6, 1) == 0)
      ok++;
  } else
    say("vr passport: flag at VA 0x%lx not as expected", (unsigned long)k_passport[k].flag_va);
  /* 2. the menu item's active byte: the same */
  if (pt_copyout(pid, base + k_passport[k].active_va, buf, 7) == 0 && memcmp(buf, k_passport[k].active, 7) == 0) {
    if (pt_copyin(pid, &one, base + k_passport[k].active_va + 6, 1) == 0)
      ok++;
  } else
    say("vr passport: active byte at VA 0x%lx not as expected", (unsigned long)k_passport[k].active_va);
  say("vr passport: %d/2 patches applied (%s)", ok, k_passport[k].title);
}

/* The game's own start-up message boxes (logs/research/boot/NOTES.md): the autosave notice before
 * the title (both games; MES_SYSTEM id 0x302bf), and over the title "Failed to download the
 * updated shop data" (id 0x302ed, after the shop's download fails offline; seen in Fortune, the
 * same code in Scarlet), from the title's update and from a second site (the same once flag).
 * Each site looks its text up in the game's message map, opens the box with it (0x3369e0 in both)
 * and, for the autosave notice, waits while a box is up. The 7-byte load that starts the lookup is
 * made a jump to the game's own "no such text" path (a null box handle stored, then on as if the
 * box were dismissed), so nothing is opened and the code goes on as after Next: the once flags
 * (the notice's, the shop message's) still set, the save-data setup and the shop's offline
 * fallback untouched. Not skipped with agent.bootnotes=1 (cvars.txt or the game's own file, a
 * release build too). Each site's bytes are checked first. */
static const struct {
  const char *title, *what;
  uint64_t va, to; /* the load (movq <map>(%rip),%rax) and the "no such text" path */
  uint8_t orig[7];
} k_bootnotes[] = {
    {"CUSA14173", "autosave notice", 0x54f3c8, 0x54f4a5, {0x48, 0x8b, 0x05, 0x71, 0x0c, 0x6a, 0x00}},
    {"CUSA14173", "shop data message (title)", 0x54c3b4, 0x54c484, {0x48, 0x8b, 0x05, 0x85, 0x3c, 0x6a, 0x00}},
    {"CUSA14173", "shop data message (2nd site)", 0x3b0184, 0x3b0283, {0x48, 0x8b, 0x05, 0xb5, 0xfe, 0x83, 0x00}},
    {"CUSA14831", "autosave notice", 0x54d8d8, 0x54d9b5, {0x48, 0x8b, 0x05, 0x21, 0x27, 0x6a, 0x00}},
    {"CUSA14831", "shop data message (title)", 0x54a974, 0x54aa44, {0x48, 0x8b, 0x05, 0x85, 0x56, 0x6a, 0x00}},
    {"CUSA14831", "shop data message (2nd site)", 0x3afad4, 0x3afbd3, {0x48, 0x8b, 0x05, 0x25, 0x05, 0x84, 0x00}},
    {"CUSA04555", "autosave notice", 0x54f258, 0x54f335, {0x48, 0x8b, 0x05, 0xe1, 0x8c, 0x69, 0x00}},
    {"CUSA04555", "shop data message (title)", 0x54b8d4, 0x54b9a4, {0x48, 0x8b, 0x05, 0x65, 0xc6, 0x69, 0x00}},
    {"CUSA04555", "shop data message (2nd site)", 0x3b06c4, 0x3b07c3, {0x48, 0x8b, 0x05, 0x75, 0x78, 0x83, 0x00}},
};
static void patch_bootnotes(pid_t pid) {
  if (g_build < 0)
    return;
  if (cvar_value("agent.bootnotes", 0) != 0) {
    say("boot notes: shown (agent.bootnotes=1)");
    return;
  }
  uint64_t base = eboot_mapbase(pid);
  if (!base)
    return;
  int ok = 0, n = 0;
  for (int i = 0; i < (int)(sizeof k_bootnotes / sizeof k_bootnotes[0]); i++) {
    if (strcmp(k_builds[g_build].title_id, k_bootnotes[i].title) != 0)
      continue;
    n++;
    uint8_t buf[7];
    if (pt_copyout(pid, base + k_bootnotes[i].va, buf, 7) != 0 || memcmp(buf, k_bootnotes[i].orig, 7) != 0) {
      say("boot notes: the %s's code at VA 0x%lx not as expected: shown", k_bootnotes[i].what,
          (unsigned long)k_bootnotes[i].va);
      continue;
    }
    int32_t rel = (int32_t)(k_bootnotes[i].to - (k_bootnotes[i].va + 5));
    uint8_t jmp[7] = {0xe9, 0, 0, 0, 0, 0x66, 0x90}; /* jmp rel32; a 2-byte nop */
    memcpy(jmp + 1, &rel, 4);
    if (pt_copyin(pid, jmp, base + k_bootnotes[i].va, 7) == 0)
      ok++;
    else
      say("boot notes: the %s's patch could not be written", k_bootnotes[i].what);
  }
  say("boot notes: %d/%d sites skipped (the autosave notice, the shop data message; agent.bootnotes=1 shows them)",
      ok, n);
}

/* The game's FXAA (its post-effect pass over every eye view: the console FXAA 3.11 shader, its
 * thresholds written each frame by one block of the eboot): with agent.fxaa=0 (cvars.txt or the
 * game's own file) the edge threshold's least value, 0.1, is made 1.7e38, so the shader's early-out
 * (the contrast under the threshold: the centre texel as it is) is taken for every pixel and the
 * pass is a plain copy. The same passes and targets, nothing else changed; it holds for the flat
 * game too. No line, or 1: the game's FXAA as it is (logs/research/fxaa/REPORT.md). */
static const struct {
  const char *title;
  uint64_t va;      /* movl $0x3dcccccd, <edgeThresholdMin>(%rip) */
  uint8_t orig[10];
} k_fxaa[] = {
    {"CUSA14173", 0x4fc8d3, {0xc7, 0x05, 0xa3, 0x5e, 0x72, 0x01, 0xcd, 0xcc, 0xcc, 0x3d}},
    {"CUSA14831", 0x4fae13, {0xc7, 0x05, 0x23, 0x79, 0x72, 0x01, 0xcd, 0xcc, 0xcc, 0x3d}},
    {"CUSA04555", 0x4fbcc3, {0xc7, 0x05, 0x93, 0xe9, 0x71, 0x01, 0xcd, 0xcc, 0xcc, 0x3d}},
};

static void patch_fxaa(pid_t pid) {
  if (g_build < 0 || cvar_value("agent.fxaa", 1) != 0)
    return;
  for (int i = 0; i < (int)(sizeof k_fxaa / sizeof k_fxaa[0]); i++) {
    if (strcmp(k_builds[g_build].title_id, k_fxaa[i].title) != 0)
      continue;
    uint64_t base = eboot_mapbase(pid);
    uint8_t buf[10];
    static const uint8_t huge[4] = {0x00, 0x00, 0x00, 0x7f}; /* 1.7e38 */
    if (!base || pt_copyout(pid, base + k_fxaa[i].va, buf, 10) != 0 || memcmp(buf, k_fxaa[i].orig, 10) != 0) {
      say("fxaa: the threshold's code at VA 0x%lx not as expected: left as it is", (unsigned long)k_fxaa[i].va);
      return;
    }
    if (pt_copyin(pid, huge, base + k_fxaa[i].va + 6, 4) == 0)
      say("fxaa: the game's FXAA made a plain copy (agent.fxaa=0)");
    else
      say("fxaa: the patch could not be written");
  }
}

/* The VR views' render sizes and what bounds them (logs/research/mode0/REPORT.md). The game's
 * render modes 1-3 make their colour, colour2 and depth targets as views of mode 0's memory
 * (1920x1080 unless agent.mode0 enlarges it): each view is laid out from its own size, and the
 * device allocator places it there when its bytes fit, else gives it memory of its own and sets a
 * bit in its fallback flags (the agent logs them) - and a mode's colour2 then no longer shares
 * its depth's memory (the effects that read depth likely wrong). The per-16-px tile buffers are
 * sized from mode 0 too. So a view of w x h is allowed when its bytes at the most (pitch rounded
 * to 256, rows to 128) are within mode 0's at the least (pitch to 128, rows to 64), and its 16-px
 * tiles within mode 0's: with 1920x1080, 1280 (x1432) the most (measured clean on the console);
 * with mode 0 at 2688x1512, 1792 (x2005). Widths have the game's own height,
 * trunc(752 * (w + 0.5) / 672) (its 672x752 times the factor (w + 0.5) / 672). */
static unsigned g_mode0_w = 1920, g_mode0_h = 1080; /* (agent.mode0 sets them, before the others) */
static unsigned vs_height(unsigned w) { return (unsigned)(752.0 * ((double)w + 0.5) / 672.0); }
static unsigned long vs_up(unsigned long v, unsigned long a) { return (v + a - 1) / a * a; }
static int vs_fits(unsigned w, unsigned h) {
  return vs_up(w, 256) * vs_up(h, 128) <= vs_up(g_mode0_w, 128) * vs_up(g_mode0_h, 64) &&
         (unsigned long)((w + 15) / 16) * ((h + 15) / 16) <=
             (unsigned long)((g_mode0_w + 15) / 16) * ((g_mode0_h + 15) / 16);
}
/* the widest view from `lo` on with every width from lo to it fitting mode 0 */
static unsigned vs_cap(unsigned lo) {
  unsigned n = lo;
  while (n < 4096 && vs_fits(n + 1, vs_height(n + 1)))
    n++;
  return n;
}

#ifndef RELEASE
/* agent.nohole=1 (a debug option, for the near view moved off the axis, agent/gaze.c): the wide
 * view's hole switched off. The wide views' masks are built on the CPU when VR starts (0x6ea580
 * Fortune / 0x6f1840 Scarlet); a tile is left undrawn (the hole) when all its corners are inside
 * (radius/tan_top)^2, a square that Pro mode multiplies by 0.98 (the constant's one use, 0x6ea657 /
 * 0x6f1917, after sceKernelIsNeoMode). Made 0.0, no tile is ever inside: the wide view drawn
 * everywhere within the lens circle (the near views' masks and the lens circle as they are). In
 * the base mode the hole would stay: the agent checks Pro mode before moving the near view
 * (logs/research/gaze/REPORT.md section 2). */
static const struct {
  const char *title;
  uint64_t va; /* the float 0.98 */
} k_nohole[] = {
    {"CUSA14173", 0x7bbc78},
    {"CUSA04555", 0x7b44e4},
}; /* Scarlet 1.00 (CUSA14831): none, its builder has no 0.98 */

static void patch_nohole(pid_t pid) {
  if (g_build < 0 || cvar_value("agent.nohole", 0) == 0)
    return;
  if (strcmp(k_builds[g_build].title_id, "CUSA14831") == 0)
    say("nohole: does not apply to Scarlet 1.00 (its wide masks have no Pro-mode 0.98): left as it is");
  for (int i = 0; i < (int)(sizeof k_nohole / sizeof k_nohole[0]); i++) {
    if (strcmp(k_builds[g_build].title_id, k_nohole[i].title) != 0)
      continue;
    uint64_t base = eboot_mapbase(pid);
    static const uint8_t orig[4] = {0x48, 0xe1, 0x7a, 0x3f}, zero[4] = {0, 0, 0, 0}; /* 0.98, 0.0 */
    uint8_t buf[4];
    if (!base || pt_copyout(pid, base + k_nohole[i].va, buf, 4) != 0 || memcmp(buf, orig, 4) != 0) {
      say("nohole: the hole's factor at VA 0x%lx not as expected: left as it is", (unsigned long)k_nohole[i].va);
      return;
    }
    if (pt_copyin(pid, zero, base + k_nohole[i].va, 4) == 0)
      say("nohole: the wide view's hole switched off (agent.nohole=1, Pro mode's 0.98 made 0)");
    else
      say("nohole: the patch could not be written");
  }
}
#endif

/* (widew and nearn: the settings menu's Resolution presets' too, so in both builds) */

/* agent.widew=M (the Resolution preset's, res_settings; a debug build's cvars.txt too): the wide views drawn at M x 752M/672 on the game's render
 * mode 1, the near views on its mode 3 at agent.renderw's size (940 without it); M from 320 to the
 * cap mode 0 gives (vs_cap: 1280 with mode 0 as it is; logs/research/gpu120/REPORT.md, candidate C).
 * The game draws every VR view at its render mode 3 (672x752 x the Pro factor 1.4 = 940x1052). It
 * has four modes, all built at its boot from one table of sizes (Scarlet 0x7c0de0, Fortune 0x7b9660,
 * read once, before VR): every mode's targets are views of mode 0's memory or of buffers made as
 * large as the largest mode, and every pass picks its mode's targets from the mode global at the
 * time it draws. So mode 1 (1440x810, unused: VR always takes mode 3, flat mode 0) is made
 * M x 752M/672 in the table, and at the view loop's head (the store of the view index, 7 bytes) a
 * call to code written past the end of the game's text (after .eh_frame_hdr, in the text's last
 * page) does that store and then, only with the four VR views (vr on, the 2D latch off), MSAA
 * index 0, the mode 3 or 1 and the main and resolve sets that mode's (grid + 0x498 + mode * 0x250),
 * sets the mode and the two sets to mode 1 for views 0 and 1 (wide L, R) and back to mode 3 for
 * views 2 and 3 (near L, R): the frame ends in the game's own mode 3. Two counters in the data
 * segment's last page, past its end (switched views, views refused by the checks), read by the
 * agent every 10 s (agent/viewsize.c). (The branch's agent.nearw is gone: it wrote the same
 * double as agent.renderw, which is now the one size of mode 3: the near views, and the wide ones
 * too without widew.) Every byte is compared first: the table, the call's 7 bytes, the code's
 * place (zeros, executable) and the counters' (zeros, writable); any difference and nothing is
 * written. */
static const struct vs_game {
  const char *title;
  uint64_t site;         /* movl %r8d, <view index>(%rip) at the view loop's head */
  uint8_t site_orig[7];
  uint64_t cave;         /* past the text's end (memsz), in its last page */
  uint64_t count;        /* past the data segment's end (memsz), in its last page: two u32 */
  uint64_t view, vr_on, latch2d; /* the view index; the loop's count is 4 with vr_on set, latch2d clear */
  uint64_t mode, msaa, main_set, resolve_set;
  uint64_t set0;         /* mode 0's main set (MSAA 0): grid + 0x498; mode m's is set0 + m * 0x250 */
  uint64_t sizes;        /* the four modes' sizes (u32 w, h) */
} k_vs[] = {
    {"CUSA14173", 0x4cdd46, {0x44, 0x89, 0x05, 0x5b, 0x8e, 0x92, 0x01}, 0x89ef90, 0x1df6c40, 0x1df6ba8, 0x1df6ba2,
     0x1df6bbc, 0x1c2da88, 0x1c2da8c, 0x1c2daa0, 0x1c2daa8, 0x1c2b8e8, 0x7c0de0},
    {"CUSA14831", 0x4cc286, {0x44, 0x89, 0x05, 0xdb, 0xa8, 0x92, 0x01}, 0x89d4e0, 0x1df6c00, 0x1df6b68, 0x1df6b62,
     0x1df6b7c, 0x1c2da48, 0x1c2da4c, 0x1c2da60, 0x1c2da68, 0x1c2b8a8, 0x7bf480},
    {"CUSA04555", 0x4cc3a6, {0x44, 0x89, 0x05, 0xdb, 0x26, 0x92, 0x01}, 0x895580, 0x1deeb20, 0x1deea88, 0x1deea82,
     0x1deea9c, 0x1c25968, 0x1c2596c, 0x1c25980, 0x1c25988, 0x1c237c8, 0x7b9660},
};
#define VS_MIN 320u

/* the protection of the game's map entry holding [a, a + len) (VM_PROT bits), -1 when none does */
static int vm_prot_at(pid_t game, uint64_t a, uint64_t len) {
  int mib[4] = {1, 14, 32, game};
  size_t size = 0;
  if (sysctl(mib, 4, NULL, &size, NULL, 0) != 0 || !size)
    return -1;
  size += size / 4;
  uint8_t *map = malloc(size);
  if (!map || sysctl(mib, 4, map, &size, NULL, 0) != 0) {
    free(map);
    return -1;
  }
  int prot = -1;
  for (uint8_t *e = map; e + 64 <= map + size;) {
    int es = *(int *)e;
    if (es <= 0)
      break;
    uint64_t start = *(uint64_t *)(e + 8), end = *(uint64_t *)(e + 16);
    if (a >= start && a + len <= end) {
      prot = *(int *)(e + 56);
      break;
    }
    e += es;
  }
  free(map);
  return prot;
}

/* the switch's code, assembled for its place (VAs: the game's, rel32s from them) */
struct vs_asm {
  uint8_t b[512]; /* (widew's code under 0x100 bytes; agent.nearcull's at its place + 0x100) */
  unsigned n;
  uint64_t va;
  struct { unsigned at; int label; } fix[12];
  unsigned nfix, label[4];
};
enum { VS_OK, VS_SET, VS_BAD, VS_OUT };
static void vs_bytes(struct vs_asm *a, const uint8_t *p, unsigned n) {
  for (unsigned i = 0; i < n && a->n < sizeof a->b; i++)
    a->b[a->n++] = p[i];
}
static void vs_rel(struct vs_asm *a, uint64_t target, unsigned tail) { /* a rip-relative disp32 */
  int32_t d = (int32_t)(int64_t)(target - (a->va + a->n + 4 + tail));
  vs_bytes(a, (const uint8_t *)&d, 4);
}
static void vs_op(struct vs_asm *a, const char *ops, unsigned n, uint64_t target, unsigned tail,
                  const char *imm, unsigned nimm) {
  vs_bytes(a, (const uint8_t *)ops, n);
  vs_rel(a, target, tail);
  vs_bytes(a, (const uint8_t *)imm, nimm);
}
static void vs_jump(struct vs_asm *a, const char *ops, unsigned n, int label) { /* jcc/jmp rel32 to a label */
  vs_bytes(a, (const uint8_t *)ops, n);
  if (a->nfix < 12)
    a->fix[a->nfix].at = a->n, a->fix[a->nfix++].label = label;
  static const uint8_t z[4];
  vs_bytes(a, z, 4);
}
static int vs_build(struct vs_asm *a, const struct vs_game *g) {
  a->n = 0, a->nfix = 0;
  a->va = g->cave;
  vs_op(a, "\x44\x89\x05", 3, g->view, 0, "", 0);         /* movl %r8d, view (the instruction replaced) */
  vs_bytes(a, (const uint8_t *)"\x50\x51", 2);             /* push %rax; push %rcx */
  vs_op(a, "\x80\x3d", 2, g->vr_on, 1, "\x00", 1);          /* cmpb $0, vr_on */
  vs_jump(a, "\x0f\x84", 2, VS_OUT);                       /* je out */
  vs_op(a, "\x80\x3d", 2, g->latch2d, 1, "\x00", 1);        /* cmpb $0, latch2d */
  vs_jump(a, "\x0f\x85", 2, VS_OUT);                       /* jne out */
  vs_bytes(a, (const uint8_t *)"\x41\x83\xf8\x03", 4);     /* cmpl $3, %r8d */
  vs_jump(a, "\x0f\x87", 2, VS_OUT);                       /* ja out (not one of the four views) */
  vs_op(a, "\x83\x3d", 2, g->msaa, 1, "\x00", 1);           /* cmpl $0, msaa */
  vs_jump(a, "\x0f\x85", 2, VS_BAD);                       /* jne bad */
  vs_op(a, "\x8b\x05", 2, g->mode, 0, "", 0);               /* movl mode, %eax */
  vs_bytes(a, (const uint8_t *)"\x83\xf8\x03", 3);         /* cmpl $3, %eax */
  vs_jump(a, "\x0f\x84", 2, VS_OK);                        /* je ok */
  vs_bytes(a, (const uint8_t *)"\x83\xf8\x01", 3);         /* cmpl $1, %eax */
  vs_jump(a, "\x0f\x85", 2, VS_BAD);                       /* jne bad */
  a->label[VS_OK] = a->n;
  vs_bytes(a, (const uint8_t *)"\x69\xc0\x50\x02\x00\x00", 6); /* imull $0x250, %eax, %eax */
  vs_op(a, "\x48\x8d\x0d", 3, g->set0, 0, "", 0);           /* leaq set0, %rcx */
  vs_bytes(a, (const uint8_t *)"\x48\x01\xc8", 3);         /* addq %rcx, %rax: the mode's set */
  vs_op(a, "\x48\x3b\x05", 3, g->main_set, 0, "", 0);       /* cmpq main_set, %rax */
  vs_jump(a, "\x0f\x85", 2, VS_BAD);
  vs_op(a, "\x48\x3b\x05", 3, g->resolve_set, 0, "", 0);    /* cmpq resolve_set, %rax */
  vs_jump(a, "\x0f\x85", 2, VS_BAD);
  vs_bytes(a, (const uint8_t *)"\xb8\x03\x00\x00\x00", 5); /* movl $3, %eax (the near views) */
  vs_bytes(a, (const uint8_t *)"\x41\x83\xf8\x02", 4);     /* cmpl $2, %r8d */
  vs_jump(a, "\x0f\x83", 2, VS_SET);                       /* jae set */
  vs_bytes(a, (const uint8_t *)"\xb8\x01\x00\x00\x00", 5); /* movl $1, %eax (the wide views) */
  a->label[VS_SET] = a->n;
  vs_op(a, "\x89\x05", 2, g->mode, 0, "", 0);               /* movl %eax, mode */
  vs_bytes(a, (const uint8_t *)"\x69\xc0\x50\x02\x00\x00", 6); /* imull $0x250, %eax, %eax */
  vs_bytes(a, (const uint8_t *)"\x48\x01\xc8", 3);         /* addq %rcx, %rax */
  vs_op(a, "\x48\x89\x05", 3, g->main_set, 0, "", 0);       /* movq %rax, main_set */
  vs_op(a, "\x48\x89\x05", 3, g->resolve_set, 0, "", 0);    /* movq %rax, resolve_set */
  vs_op(a, "\xff\x05", 2, g->count, 0, "", 0);              /* incl count[0] */
  vs_jump(a, "\xe9", 1, VS_OUT);                           /* jmp out */
  a->label[VS_BAD] = a->n;
  vs_op(a, "\xff\x05", 2, g->count + 4, 0, "", 0);          /* incl count[1] */
  a->label[VS_OUT] = a->n;
  vs_bytes(a, (const uint8_t *)"\x59\x58\xc3", 3);         /* pop %rcx; pop %rax; ret */
  if (a->n >= sizeof a->b || a->nfix != 10)
    return -1;
  for (unsigned i = 0; i < a->nfix; i++) {
    int32_t d = (int32_t)a->label[a->fix[i].label] - (int32_t)(a->fix[i].at + 4);
    memcpy(&a->b[a->fix[i].at], &d, 4);
  }
  return 0;
}

static void patch_viewsize(pid_t pid) {
  res_settings();
  unsigned ww = g_res.widew, cap = vs_cap(VS_MIN);
#ifndef RELEASE
  if (cvar_value("agent.nearw", 0))
    say("viewsize: agent.nearw is gone: agent.renderw sizes the near views (agent.widew the wide ones)");
#endif
  if (g_build < 0 || !ww)
    return;
  const struct vs_game *g = NULL;
  for (int i = 0; i < (int)(sizeof k_vs / sizeof k_vs[0]); i++)
    if (strcmp(k_builds[g_build].title_id, k_vs[i].title) == 0)
      g = &k_vs[i];
  uint64_t base = eboot_mapbase(pid);
  if (!g || !base) {
    say("viewsize: not this game's build: left as it is");
    return;
  }
  if (ww < VS_MIN || ww > cap) {
    say("viewsize: agent.widew is from %u to %u (within mode 0's %ux%u; agent.mode0 raises it): left as it is",
        VS_MIN, cap, g_mode0_w, g_mode0_h);
    return;
  }
  struct vs_asm a;
  if (vs_build(&a, g) != 0) {
    say("viewsize: the switch's code did not assemble: agent.widew not applied");
    return;
  }
  /* the height: the game's proportion, or agent.vdensitywide's (else agent.vdensity's) percent of it,
   * by mode 3's arithmetic (logs/research/gpu120/DENSITY-FXAA.md); the preset's, or a debug build's
   * cvars.txt over it (res_settings) */
  unsigned q = g_res.vdw ? g_res.vdw : g_res.vd;
  if (q && g_res.vd && !g_vd_h3) /* (mode 3's height was asked and refused: the wide views kept in the game's
                                  * proportion too; agent.vdensitywide alone, a debug build's, as before) */
    q = 0;
  if (q && (q < VD_MIN || q > VD_MAX)) {
    say("viewsize: agent.vdensitywide (or agent.vdensity) is from %u to %u: the wide views in the game's "
        "proportion", VD_MIN, VD_MAX);
    q = 0;
  }
  uint8_t site[7], cave[sizeof a.b], cnt[8];
  uint32_t mode1[2], want[2] = {ww, q ? (uint32_t)((double)vd_base(q) * (((double)ww + 0.5) / 672.0)) : vs_height(ww)},
           orig1[2] = {1440, 810};
  static const uint8_t zero[sizeof a.b];
  int tprot = vm_prot_at(pid, base + g->cave, a.n), dprot = vm_prot_at(pid, base + g->count, 8);
  if (pt_copyout(pid, base + g->site, site, 7) != 0 || memcmp(site, g->site_orig, 7) != 0 ||
      pt_copyout(pid, base + g->sizes + 8, mode1, 8) != 0 || memcmp(mode1, orig1, 8) != 0 ||
      pt_copyout(pid, base + g->cave, cave, a.n) != 0 || memcmp(cave, zero, a.n) != 0 ||
      pt_copyout(pid, base + g->count, cnt, 8) != 0 || memcmp(cnt, zero, 8) != 0 || tprot < 0 ||
      !(tprot & 4) || dprot < 0 || !(dprot & 2)) {
    say("viewsize: the view loop's head, mode 1's size, or the places for the code (prot %d) and the "
        "counters (prot %d) not as expected: agent.widew not applied", tprot, dprot);
    return;
  }
  uint8_t call[7] = {0xe8, 0, 0, 0, 0, 0x66, 0x90}; /* call cave; a 2-byte nop */
  int32_t rel = (int32_t)(int64_t)(g->cave - (g->site + 5));
  memcpy(call + 1, &rel, 4);
  if (pt_copyin(pid, a.b, base + g->cave, a.n) != 0 || pt_copyin(pid, want, base + g->sizes + 8, 8) != 0 ||
      pt_copyin(pid, call, base + g->site, 7) != 0) {
    say("viewsize: a write failed: agent.widew may be half applied (mode 1 is unused without the call)");
    return;
  }
  say("viewsize: the wide views on mode 1, %ux%u (agent.widew%s; %u bytes at VA 0x%lx, the call at 0x%lx)",
      want[0], want[1], q ? ", agent.vdensity" : "", a.n, (unsigned long)g->cave, (unsigned long)g->site);
}

#ifndef RELEASE
/* agent.uihold=1 (a debug option, off; logs/research/fps/TIMERS-120.md item 3, TIMERS-120-FIX.md):
 * at 120 fps the update's "seconds table" span (the UI managers' map walked with vtable[0x30](1/60
 * s), the layout manager, the boot logo's fade) runs on every loop, twice a tick. A 5-byte jmp at
 * its first instruction (movl <ring index>, %eax, 6 bytes: the 6th made a nop) to 38 bytes past the
 * text's end, at agent.widew's place + 0x400 (widew's code under 0x100, agent.nearcull's at + 0x100,
 * 512 at the most; all in the text's last 16 KB page):
 *     cmpb $0, flag(%rip); jne hold; movl <ring index>(%rip), %eax; jmp site + 6
 *   hold: movb $0, flag(%rip); incl count(%rip); jmp after
 * with `after` the span's end (the same movl again, right after its mutex unlock: the only way the
 * span ends; nothing outside it jumps into it but to its first instruction; the flags are dead at
 * both ends). The flag and the count are two u32 past the data segment's end, at widew's counters
 * + 0x20 (widew's two, then nearcull's two at + 8); the agent (vrreact.c uihold_tick) sets the
 * flag on a loop with no tick.
 * Every byte is compared first: the site, the span's end and the unlock call before it (their
 * operands where they lead), the code's place (zeros, executable), the flag's (zeros, writable);
 * any difference and nothing is written. Written at the entry point, before the game's first loop:
 * the code, then the jump. */
static const struct ui_game {
  const char *title;
  uint64_t site, after, unlock, ring, cave, flag;
  uint8_t site_orig[6], after_orig[6];
} k_uihold[] = {
    {"CUSA14173", 0x4f864a, 0x4f8a4b, 0x783440, 0xbf2348, 0x89ef90 + 0x400, 0x1df6c40 + 0x20,
     {0x8b, 0x05, 0xf8, 0x9c, 0x6f, 0x00}, {0x8b, 0x05, 0xf7, 0x98, 0x6f, 0x00}},
    {"CUSA14831", 0x4f6b8a, 0x4f6f8b, 0x781aa0, 0xbf2308, 0x89d4e0 + 0x400, 0x1df6c00 + 0x20,
     {0x8b, 0x05, 0x78, 0xb7, 0x6f, 0x00}, {0x8b, 0x05, 0x77, 0xb3, 0x6f, 0x00}},
    {"CUSA04555", 0x4f6c9a, 0x4f709b, 0x77bc80, 0xbea248, 0x895580 + 0x400, 0x1deeb20 + 0x20,
     {0x8b, 0x05, 0xa8, 0x35, 0x6f, 0x00}, {0x8b, 0x05, 0xa7, 0x31, 0x6f, 0x00}},
};

static void patch_uihold(pid_t pid) {
  if (g_build < 0 || cvar_value("agent.uihold", 0) == 0)
    return;
  const struct ui_game *g = NULL;
  for (int i = 0; i < (int)(sizeof k_uihold / sizeof k_uihold[0]); i++)
    if (strcmp(k_builds[g_build].title_id, k_uihold[i].title) == 0)
      g = &k_uihold[i];
  uint64_t base = eboot_mapbase(pid);
  if (!g || !base) {
    say("uihold: not this game's build: left as it is");
    return;
  }
  /* the table against itself: both movl's lead to the ring index, the call before `after` to the unlock */
  int32_t d1, d2, d3;
  memcpy(&d1, g->site_orig + 2, 4), memcpy(&d2, g->after_orig + 2, 4);
  if (g->site + 6 + (int64_t)d1 != g->ring || g->after + 6 + (int64_t)d2 != g->ring) {
    say("uihold: the table's operands do not lead to the ring index: left as it is (a mistake in main.c)");
    return;
  }
  /* the code */
  struct vs_asm a; /* (agent.widew's assembler: rip-relative operands from the cave's VA) */
  a.n = 0, a.nfix = 0, a.va = g->cave;
  vs_op(&a, "\x80\x3d", 2, g->flag, 1, "\x00", 1);          /* cmpb $0, flag */
  vs_bytes(&a, (const uint8_t *)"\x75\x0b", 2);            /* jne hold (+11) */
  vs_op(&a, "\x8b\x05", 2, g->ring, 0, "", 0);              /* movl ring, %eax (the instruction replaced) */
  vs_bytes(&a, (const uint8_t *)"\xe9", 1);                /* jmp site + 6 */
  vs_rel(&a, g->site + 6, 0);
  vs_op(&a, "\xc6\x05", 2, g->flag, 1, "\x00", 1);          /* hold: movb $0, flag */
  vs_op(&a, "\xff\x05", 2, g->flag + 4, 0, "", 0);          /* incl count */
  vs_bytes(&a, (const uint8_t *)"\xe9", 1);                /* jmp after */
  vs_rel(&a, g->after, 0);
  if (a.n != 38 || a.b[9] != 0x8b || a.b[20] != 0xc6) {
    say("uihold: the code did not assemble (%u bytes): left as it is", a.n);
    return;
  }
  static const uint8_t zero[38];
  uint8_t site[6], after[11], cave[38], flag[8];
  int tprot = vm_prot_at(pid, base + g->cave, a.n), dprot = vm_prot_at(pid, base + g->flag, 8);
  if (pt_copyout(pid, base + g->site, site, 6) != 0 || memcmp(site, g->site_orig, 6) != 0 ||
      pt_copyout(pid, base + g->after - 5, after, 11) != 0 || after[0] != 0xe8 ||
      memcmp(after + 5, g->after_orig, 6) != 0) {
    say("uihold: the span's first instruction at 0x%lx or its end at 0x%lx not as expected: left as it is",
        (unsigned long)g->site, (unsigned long)g->after);
    return;
  }
  memcpy(&d3, after + 1, 4);
  if (g->after + (int64_t)d3 != g->unlock) {
    say("uihold: the call before the span's end at 0x%lx is not the unlock 0x%lx: left as it is",
        (unsigned long)(g->after - 5), (unsigned long)g->unlock);
    return;
  }
  if (pt_copyout(pid, base + g->cave, cave, a.n) != 0 || memcmp(cave, zero, a.n) != 0 ||
      pt_copyout(pid, base + g->flag, flag, 8) != 0 || memcmp(flag, zero, 8) != 0 || tprot < 0 || !(tprot & 4) ||
      dprot < 0 || !(dprot & 2)) {
    say("uihold: the places for the code (prot %d) and the flag (prot %d) not as expected: left as it is", tprot,
        dprot);
    return;
  }
  uint8_t jmp[6] = {0xe9, 0, 0, 0, 0, 0x90}; /* jmp cave; a nop */
  int32_t rel = (int32_t)(int64_t)(g->cave - (g->site + 5));
  memcpy(jmp + 1, &rel, 4);
  if (pt_copyin(pid, a.b, base + g->cave, a.n) != 0) {
    say("uihold: the code could not be written: left as it is");
    return;
  }
  if (pt_copyin(pid, jmp, base + g->site, 6) != 0) {
    say("uihold: the jump could not be written (the code past the text's end is unused without it)");
    return;
  }
  say("uihold: the menu/message layer's span 0x%lx-0x%lx skipped on loops the agent marks (no tick); %u bytes "
      "at VA 0x%lx, the flag at 0x%lx", (unsigned long)g->site, (unsigned long)g->after, a.n,
      (unsigned long)g->cave, (unsigned long)g->flag);
}
#endif

/* agent.mode0=W (the Resolution preset's, RES_SHARPP's 2688; a debug build's cvars.txt too; the
 * caller passes g_res.mode0): the game's render mode 0 made W x 9W/16 in place of
 * 1920x1080 (W 2112, 2304, 2496 or 2688: a multiple of 192, so every size below stays whole), for
 * VR views past 1280 (agent.renderw, agent.widew: their caps follow, vs_cap; 1792 at 2688);
 * logs/research/mode0/REPORT.md option 1. Mode 0 is the one mode the game allocates memory for:
 * modes 1-3 are made as views of it, and the per-16-px tile buffers are sized from it. Its size
 * is the table's first entry (Scarlet 0x7c0de0, Fortune 0x7b9660: 80 07 00 00 38 04 00 00); the
 * boot (0x4ae3aa.. / 0x4ad73a.., one struct passed to 0x6a37d0 / 0x69e4e0, read once, before VR)
 * also sets the sizes of the game's smaller buffers in mode 0's terms - each mode's is that
 * spec x the mode's size / mode 0's (0x4af0f0.., 0x4b0326.., 0x4af5dd..) - and these are
 * scaled by W / 1920 with it, so every mode's buffers keep their sizes against it:
 * - the four immediates 480x270 and 960x540 (struct +0x38, +0x5c; movl $imm, disp32(%rsp), the
 *   value at +7: Scarlet 0x4ae51c, 0x4ae527, 0x4ae532, 0x4ae53d, Fortune 0x4ad8ac, 0x4ad8b7,
 *   0x4ad8c2, 0x4ad8cd);
 * - two 16-byte constants, each loaded once into the same struct (+0xa4 -> 0x1c21340, +0xcc ->
 *   0x1c1dca0 Scarlet): {640, 360, 960, 540} (Scarlet 0x7acca0, Fortune 0x7a5570) and
 *   {320, 180, 480, 270} (Scarlet 0x7accb0, Fortune 0x7a5580) (the mode0 report's option 1 had
 *   only the four immediates; these two were found reading the struct's copier).
 * The struct's other sizes (1280x720 at +0x2c, 64s) and the configuration's own (0x1c21340's
 * second writer, 0x6a9055) are left. Flat mode (title, Owner mode: mode 0) then draws W x 9W/16 and
 * is scaled to 1080p by the game's FXAA pass; the boot takes some 150-250 MB more (the report's
 * estimate). Every byte compared first, all left as they were on any difference; written before
 * the game's code runs (at its entry point). */
static const struct {
  const char *title;
  uint64_t table, imm[4], spec[2];
} k_mode0[] = {
    {"CUSA14173", 0x7c0de0, {0x4ae51c, 0x4ae527, 0x4ae532, 0x4ae53d}, {0x7acca0, 0x7accb0}},
    {"CUSA14831", 0x7bf480, {0x4aca5c, 0x4aca67, 0x4aca72, 0x4aca7d}, {0x7ab330, 0x7ab340}},
    {"CUSA04555", 0x7b9660, {0x4ad8ac, 0x4ad8b7, 0x4ad8c2, 0x4ad8cd}, {0x7a5570, 0x7a5580}},
};

static void patch_mode0(pid_t pid, unsigned w) {
  if (g_build < 0 || !w)
    return;
  if (w < 2112 || w > 2688 || w % 192) {
    say("mode0: agent.mode0 is 2112, 2304, 2496 or 2688: the game's own 1920x1080");
    return;
  }
  for (int i = 0; i < (int)(sizeof k_mode0 / sizeof k_mode0[0]); i++) {
    if (strcmp(k_builds[g_build].title_id, k_mode0[i].title) != 0)
      continue;
    uint64_t base = eboot_mapbase(pid);
    unsigned h = w * 9 / 16, k = w / 192; /* W = 192k, H = 108k */
    static const uint8_t table[32] = {0x80, 0x07, 0, 0, 0x38, 0x04, 0, 0, 0xa0, 0x05, 0, 0, 0x2a, 0x03, 0, 0,
                                      0x00, 0x05, 0, 0, 0xd0, 0x02, 0, 0, 0xa0, 0x02, 0, 0, 0xf0, 0x02, 0, 0};
    static const uint8_t ops[4][7] = {{0xc7, 0x84, 0x24, 0xe8, 0x01, 0, 0}, {0xc7, 0x84, 0x24, 0xec, 0x01, 0, 0},
                                      {0xc7, 0x84, 0x24, 0x0c, 0x02, 0, 0}, {0xc7, 0x84, 0x24, 0x10, 0x02, 0, 0}};
    static const uint32_t imm_orig[4] = {480, 270, 960, 540};
    static const uint32_t spec_orig[2][4] = {{640, 360, 960, 540}, {320, 180, 480, 270}};
    uint32_t size[2] = {w, h}, imm_new[4] = {48 * k, 27 * k, 96 * k, 54 * k},
             spec_new[2][4] = {{64 * k, 36 * k, 96 * k, 54 * k}, {32 * k, 18 * k, 48 * k, 27 * k}};
    uint8_t buf[32];
    int ok = base && pt_copyout(pid, base + k_mode0[i].table, buf, 32) == 0 && memcmp(buf, table, 32) == 0;
    for (int j = 0; ok && j < 4; j++)
      ok = pt_copyout(pid, base + k_mode0[i].imm[j], buf, 11) == 0 && memcmp(buf, ops[j], 7) == 0 &&
           memcmp(buf + 7, &imm_orig[j], 4) == 0;
    for (int j = 0; ok && j < 2; j++)
      ok = pt_copyout(pid, base + k_mode0[i].spec[j], buf, 16) == 0 && memcmp(buf, spec_orig[j], 16) == 0;
    if (!ok) {
      say("mode0: the size table, the four sizes' code or the two size constants not as expected: left as it is");
      return;
    }
    /* the derived sizes first, mode 0's size last: on a failed write all put back */
    int done = 0, failed = 0;
    for (int j = 0; !failed && j < 4; j++, done++)
      failed = pt_copyin(pid, &imm_new[j], base + k_mode0[i].imm[j] + 7, 4) != 0;
    for (int j = 0; !failed && j < 2; j++, done++)
      failed = pt_copyin(pid, spec_new[j], base + k_mode0[i].spec[j], 16) != 0;
    if (!failed)
      failed = pt_copyin(pid, size, base + k_mode0[i].table, 8) != 0;
    if (failed) {
      for (int j = 0; j < 4 && j < done; j++)
        pt_copyin(pid, &imm_orig[j], base + k_mode0[i].imm[j] + 7, 4);
      for (int j = 0; j < 2 && 4 + j < done; j++)
        pt_copyin(pid, spec_orig[j], base + k_mode0[i].spec[j], 16);
      say("mode0: a write failed: the game's own sizes put back");
      return;
    }
    g_mode0_w = w, g_mode0_h = h;
    say("mode0: the game's render mode 0 %ux%u (agent.mode0; its smaller buffers' sizes x %u/10: %ux%u, %ux%u, "
        "%ux%u, %ux%u): the VR views up to %u wide (agent.renderw, agent.widew)",
        w, h, k, imm_new[0], imm_new[1], imm_new[2], imm_new[3], spec_new[0][0], spec_new[0][1], spec_new[1][0],
        spec_new[1][1], vs_cap(941));
  }
}

/* after patch_mode0: a preset (or a debug build's agent.mode0) that asked for mode 0 at W and did not
 * get it (a refusal above, or not this game) would leave its render size past the cap (renderw's
 * refusal: the game's 940 under a 1440 eye). The preset is then RES_SHARP whole (its sizes fit
 * mode 0 as it is); the eye size and the heights are patched after this, from it. */
static void res_mode0_check(void) {
  if (!g_res.mode0 || g_mode0_w == g_res.mode0)
    return;
  say("resolution: mode 0 at %u was not applied: %s in place of %s", g_res.mode0, k_res[RES_SHARP].name,
      k_res[g_res.preset].name);
  g_res.preset = RES_SHARP;
  g_res.renderw = k_res[RES_SHARP].renderw, g_res.nearn = k_res[RES_SHARP].nearn;
  g_res.widew = k_res[RES_SHARP].widew, g_res.eye = k_res[RES_SHARP].eye;
  g_res.vd = k_res[RES_SHARP].vd, g_res.vdw = k_res[RES_SHARP].vdw, g_res.mode0 = k_res[RES_SHARP].mode0;
}

/* agent.nearn=F (the Resolution preset's, res_settings; a debug build's cvars.txt too; F in
 * hundredths, 25 to 49): the near view's half angle made
 * F/100 of the top tangent in place of the game's 0.5 (n = 0.5 x 1.329 = 0.6645 today), its pixels
 * the same: their density N / 2n up by 50/F (logs/research/gaze/REPORT.md section 1). The 0.5 is
 * one float read once (Scarlet 0x7bbc68 at 0x6f04bb, Fortune 0x7b44d4 at 0x6e91fb), by the
 * builder the game runs when VR starts, after the field of view: from that n it makes all that
 * depends on it - the near projections (1/n), hmd+0x214 (n) and +0x210 (n - 0.05), the wide views'
 * masks (the hole, radius n - 0.05), the near views' masks (in the view's own units: n cancels)
 * and the hand-over's near uv (1/2n) and blend (n - 0.05 to n) that compose.c takes. gaze.c checks
 * n against F x the top tangent (agent/viewsize.c reads F from the game). The wide views draw more (a smaller
 * hole) and more of the field is theirs. */
static const struct {
  const char *title;
  uint64_t va; /* the float 0.5 */
} k_nearn[] = {
    {"CUSA14173", 0x7bbc68},
    {"CUSA14831", 0x7ba318},
    {"CUSA04555", 0x7b44d4},
};

static void patch_nearn(pid_t pid) {
  res_settings();
  unsigned f = g_res.nearn;
  if (g_build < 0 || !f)
    return;
  if (f < 25 || f > 49) {
    say("nearn: agent.nearn is from 25 to 49 (hundredths of the top tangent): the game's own 0.5");
    return;
  }
  for (int i = 0; i < (int)(sizeof k_nearn / sizeof k_nearn[0]); i++) {
    if (strcmp(k_builds[g_build].title_id, k_nearn[i].title) != 0)
      continue;
    uint64_t base = eboot_mapbase(pid);
    static const uint8_t orig[4] = {0x00, 0x00, 0x00, 0x3f}; /* 0.5 */
    uint8_t buf[4];
    float v = (float)f / 100.0f;
    if (!base || pt_copyout(pid, base + k_nearn[i].va, buf, 4) != 0 || memcmp(buf, orig, 4) != 0) {
      say("nearn: the near view's 0.5 at VA 0x%lx not as expected: left as it is", (unsigned long)k_nearn[i].va);
      return;
    }
    if (pt_copyin(pid, &v, base + k_nearn[i].va, 4) == 0)
      say("nearn: the near view's half angle %u/100 of the top tangent (agent.nearn; the game's 50)", f);
    else
      say("nearn: the patch could not be written");
  }
}

#ifndef RELEASE
/* agent.nearcull=1 (a debug option, off): the near views culled with their own field of view
 * (logs/research/gpu120/NEAR.md). The game culls per view: 0x49f9b0 / 0x49ed40 (the view loop's
 * camera, once a view) builds the culling frustum (0x6dc470 / 0x6d7180: a full angle, the vertical
 * to horizontal ratio, near, far) into the camera block the sphere tests read (through the pointer
 * at 0x942d18; the test 0x509a00 and the others) - but for all four VR views from the headset's
 * wide field of view (hmd+0x21c x +0x220, 1 / +0x220), so the near views (+-0.53 tan at
 * agent.nearn=40) submit everything the wide view's frustum holds. Its two calls (Scarlet 0x4a0c1c,
 * 0x4a0df0; Fortune 0x49ffac, 0x4a0180; the second for the right eye's views) are made calls to
 * code past the game's text (agent.widew's place + 0x100) that, only for views 2 and 3 (the view
 * index the loop's head stores; not the frame's default camera, r14 = -1; vr on, the 2D latch off),
 * takes the near view's own projection as the game copied it into the camera block for this view
 * (rows 0-2: 1/n x, 1/n y, the off-centre -cx/n, -cy/n that agent/gaze.c writes) and passes the
 * square frustum that holds it, centred on the eye's axis: half tangent h = 1.02 x the larger of
 * (1 + |p20|) / p00 and (1 + |p21|) / p11 (n + |c| on each axis), the angle 2 atan(h) through the
 * game's own atanf (Scarlet 0x783640, Fortune 0x77be80: its hmd+0x21c's), ratio 1. The game's own
 * is kept when h is not from 0.05 to 4, atan's result not from 0 to h, or the angle not smaller
 * than the game's. Two counters after widew's (narrowed, kept by the checks; agent/viewsize.c logs
 * them). Every byte compared first: both calls, the code's place (zeros, executable), the counters
 * (zeros, writable).
 * It saves nothing as it is (NEAR.md §1b): the view loop builds its draw lists, with the object
 * sphere test, in view 0 only (Scarlet 0x4cf0ed, Fortune 0x4cd74d: views 1-3 jump to the replay of
 * the lists at the camera block + 0x598); the near views' frustum is read only by the few draws
 * that test themselves again (draw methods 0x4dbd00, 0x546e60). */
static const struct nc_game {
  const char *title;
  uint64_t site[2], frustum, atanf, cave, count, view, vr_on, latch2d;
  uint64_t proj; /* the view's projection in the camera block: row 0 (rows at +0x10) */
} k_nc[] = {
    {"CUSA14173", {0x4a0c1c, 0x4a0df0}, 0x6dc470, 0x783640, 0x89ef90 + 0x100, 0x1df6c40 + 8, 0x1df6ba8, 0x1df6ba2,
     0x1df6bbc, 0xabba20},
    {"CUSA14831", {0x49f15c, 0x49f330}, 0x6da2f0, 0x781ca0, 0x89d4e0 + 0x100, 0x1df6c00 + 8, 0x1df6b68, 0x1df6b62,
     0x1df6b7c, 0xabb9e0},
    {"CUSA04555", {0x49ffac, 0x4a0180}, 0x6d7180, 0x77be80, 0x895580 + 0x100, 0x1deeb20 + 8, 0x1deea88, 0x1deea82,
     0x1deea9c, 0xab3940},
};
enum { NC_GO, NC_BADH, NC_KEEP };          /* jump labels */
enum { NC_K1, NC_KM, NC_KLO, NC_KHI };     /* the constants after the code */

static int nc_build(struct vs_asm *a, const struct nc_game *g) {
  a->n = 0, a->nfix = 0;
  a->va = g->cave;
  unsigned lab[3], kfix[8], klab[8], nk = 0;
#define NC_K(ops, which)                                                                                       \
  do {                                                                                                         \
    vs_bytes(a, (const uint8_t *)(ops), 4);                                                                    \
    if (nk < 8)                                                                                                \
      kfix[nk] = a->n, klab[nk++] = (which);                                                                   \
    vs_bytes(a, (const uint8_t *)"\0\0\0\0", 4);                                                               \
  } while (0)
  vs_bytes(a, (const uint8_t *)"\x41\x83\xfe\xff", 4);         /* cmpl $-1, %r14d (the frame's default camera) */
  vs_jump(a, "\x0f\x84", 2, NC_GO);                            /* je go */
  vs_op(a, "\x80\x3d", 2, g->vr_on, 1, "\x00", 1);              /* cmpb $0, vr_on */
  vs_jump(a, "\x0f\x84", 2, NC_GO);
  vs_op(a, "\x80\x3d", 2, g->latch2d, 1, "\x00", 1);            /* cmpb $0, latch2d */
  vs_jump(a, "\x0f\x85", 2, NC_GO);
  vs_op(a, "\x8b\x05", 2, g->view, 0, "", 0);                   /* movl view, %eax */
  vs_bytes(a, (const uint8_t *)"\x83\xe8\x02\x83\xf8\x01", 6);   /* subl $2, %eax; cmpl $1, %eax */
  vs_jump(a, "\x0f\x87", 2, NC_GO);                            /* ja go (not a near view) */
  vs_op(a, "\x8b\x05", 2, g->proj + 0x20, 0, "", 0);            /* movl p20, %eax */
  vs_bytes(a, (const uint8_t *)"\x25\xff\xff\xff\x7f\xc5\xf9\x6e\xf0", 9); /* andl $0x7fffffff; vmovd -> %xmm6 */
  vs_op(a, "\x8b\x05", 2, g->proj + 0x24, 0, "", 0);            /* movl p21, %eax */
  vs_bytes(a, (const uint8_t *)"\x25\xff\xff\xff\x7f\xc5\xf9\x6e\xf8", 9); /* andl; vmovd -> %xmm7 */
  vs_op(a, "\xc5\xfa\x10\x25", 4, g->proj, 0, "", 0);           /* vmovss p00, %xmm4 */
  vs_op(a, "\xc5\xfa\x10\x2d", 4, g->proj + 0x14, 0, "", 0);    /* vmovss p11, %xmm5 */
  NC_K("\xc5\xca\x58\x35", NC_K1);                             /* vaddss 1.0, %xmm6, %xmm6 */
  NC_K("\xc5\xc2\x58\x3d", NC_K1);                             /* vaddss 1.0, %xmm7, %xmm7 */
  vs_bytes(a, (const uint8_t *)"\xc5\xca\x5e\xf4"                /* vdivss %xmm4, %xmm6, %xmm6 */
                               "\xc5\xc2\x5e\xfd"                /* vdivss %xmm5, %xmm7, %xmm7 */
                               "\xc5\xca\x5f\xf7", 12);          /* vmaxss %xmm7, %xmm6, %xmm6 */
  NC_K("\xc5\xca\x59\x35", NC_KM);                             /* vmulss 1.02, %xmm6, %xmm6: h */
  NC_K("\xc5\xf8\x2e\x35", NC_KLO);                            /* vucomiss 0.05, %xmm6 */
  vs_jump(a, "\x0f\x86", 2, NC_BADH);                          /* jbe badh (NaN too) */
  NC_K("\xc5\xf8\x2e\x35", NC_KHI);                            /* vucomiss 4.0, %xmm6 */
  vs_jump(a, "\x0f\x83", 2, NC_BADH);                          /* jae badh */
  vs_bytes(a, (const uint8_t *)"\x57\x56\x48\x83\xec\x38"        /* push rdi; push rsi; sub $0x38 (16-aligned) */
                               "\xc5\xfa\x11\x04\x24"            /* vmovss %xmm0, (%rsp) */
                               "\xc5\xfa\x11\x4c\x24\x04"        /* vmovss %xmm1, 4(%rsp) */
                               "\xc5\xfa\x11\x54\x24\x08"        /* vmovss %xmm2, 8(%rsp) */
                               "\xc5\xfa\x11\x5c\x24\x0c"        /* vmovss %xmm3, 12(%rsp) */
                               "\xc5\xfa\x11\x74\x24\x10"        /* vmovss %xmm6, 16(%rsp) */
                               "\xc5\xf8\x28\xc6"                /* vmovaps %xmm6, %xmm0 */
                               "\xe8", 40);                      /* call atanf */
  vs_rel(a, g->atanf, 0);
  vs_bytes(a, (const uint8_t *)"\xc5\xd8\x57\xe4\xc5\xf8\x2e\xc4", 8); /* vxorps %xmm4; vucomiss %xmm4, %xmm0 */
  vs_jump(a, "\x0f\x86", 2, NC_KEEP);                          /* jbe keep (<= 0, NaN) */
  vs_bytes(a, (const uint8_t *)"\xc5\xf8\x2e\x44\x24\x10", 6);   /* vucomiss 16(%rsp), %xmm0 */
  vs_jump(a, "\x0f\x83", 2, NC_KEEP);                          /* jae keep (>= h: not an atan) */
  vs_bytes(a, (const uint8_t *)"\xc5\xfa\x58\xc0"                /* vaddss %xmm0, %xmm0, %xmm0: the angle */
                               "\xc5\xf8\x2e\x04\x24", 9);       /* vucomiss (%rsp), %xmm0 */
  vs_jump(a, "\x0f\x83", 2, NC_KEEP);                          /* jae keep (not narrower than the game's) */
  NC_K("\xc5\xfa\x10\x0d", NC_K1);                             /* vmovss 1.0, %xmm1: square */
  vs_bytes(a, (const uint8_t *)"\xc5\xfa\x10\x54\x24\x08"        /* vmovss 8(%rsp), %xmm2 */
                               "\xc5\xfa\x10\x5c\x24\x0c"        /* vmovss 12(%rsp), %xmm3 */
                               "\x48\x83\xc4\x38\x5e\x5f", 18);  /* add $0x38; pop rsi; pop rdi */
  vs_op(a, "\xff\x05", 2, g->count, 0, "", 0);                  /* incl count[0]: narrowed */
  vs_jump(a, "\xe9", 1, NC_GO);
  lab[NC_KEEP] = a->n;                                         /* keep: the game's four arguments back */
  vs_bytes(a, (const uint8_t *)"\xc5\xfa\x10\x04\x24\xc5\xfa\x10\x4c\x24\x04"
                               "\xc5\xfa\x10\x54\x24\x08\xc5\xfa\x10\x5c\x24\x0c"
                               "\x48\x83\xc4\x38\x5e\x5f", 29);
  lab[NC_BADH] = a->n;
  vs_op(a, "\xff\x05", 2, g->count + 4, 0, "", 0);              /* incl count[1]: kept */
  lab[NC_GO] = a->n;
  vs_bytes(a, (const uint8_t *)"\xe9", 1);                     /* jmp the game's frustum builder */
  vs_rel(a, g->frustum, 0);
  while (a->n & 3)
    vs_bytes(a, (const uint8_t *)"\xcc", 1);
  unsigned kc = a->n;
  static const float kv[4] = {1.0f, 1.02f, 0.05f, 4.0f};
  vs_bytes(a, (const uint8_t *)kv, sizeof kv);
#undef NC_K
  if (a->n >= sizeof a->b || a->nfix != 10 || nk != 6)
    return -1;
  for (unsigned i = 0; i < a->nfix; i++) {
    int32_t d = (int32_t)lab[a->fix[i].label] - (int32_t)(a->fix[i].at + 4);
    memcpy(&a->b[a->fix[i].at], &d, 4);
  }
  for (unsigned i = 0; i < nk; i++) {
    int32_t d = (int32_t)(kc + 4 * klab[i]) - (int32_t)(kfix[i] + 4);
    memcpy(&a->b[kfix[i]], &d, 4);
  }
  return 0;
}

static void patch_nearcull(pid_t pid) {
  if (g_build < 0 || cvar_value("agent.nearcull", 0) == 0)
    return;
  const struct nc_game *g = NULL;
  for (int i = 0; i < (int)(sizeof k_nc / sizeof k_nc[0]); i++)
    if (strcmp(k_builds[g_build].title_id, k_nc[i].title) == 0)
      g = &k_nc[i];
  uint64_t base = eboot_mapbase(pid);
  static struct vs_asm a;
  if (!g || !base || nc_build(&a, g) != 0) {
    say("nearcull: not this game's build, or its code did not assemble: left as it is");
    return;
  }
  static uint8_t cave[sizeof a.b];
  static const uint8_t zero[sizeof a.b];
  uint8_t site[2][5], cnt[8];
  int ok = 1;
  for (int j = 0; j < 2 && ok; j++) {
    int32_t rel = (int32_t)(int64_t)(g->frustum - (g->site[j] + 5));
    ok = pt_copyout(pid, base + g->site[j], site[j], 5) == 0 && site[j][0] == 0xe8 &&
         memcmp(&site[j][1], &rel, 4) == 0;
  }
  int tprot = vm_prot_at(pid, base + g->cave, a.n), dprot = vm_prot_at(pid, base + g->count, 8);
  if (!ok || pt_copyout(pid, base + g->cave, cave, a.n) != 0 || memcmp(cave, zero, a.n) != 0 ||
      pt_copyout(pid, base + g->count, cnt, 8) != 0 || memcmp(cnt, zero, 8) != 0 || tprot < 0 || !(tprot & 4) ||
      dprot < 0 || !(dprot & 2)) {
    say("nearcull: the frustum's two calls, the place for the code (prot %d) or the counters (prot %d) not as "
        "expected: left as it is", tprot, dprot);
    return;
  }
  if (pt_copyin(pid, a.b, base + g->cave, a.n) != 0) {
    say("nearcull: the code could not be written: left as it is");
    return;
  }
  for (int j = 0; j < 2; j++) {
    uint8_t call[5] = {0xe8};
    int32_t rel = (int32_t)(int64_t)(g->cave - (g->site[j] + 5));
    memcpy(call + 1, &rel, 4);
    if (pt_copyin(pid, call, base + g->site[j], 5) != 0) {
      if (j == 1) /* (the first put back: both calls or neither) */
        pt_copyin(pid, site[0], base + g->site[0], 5);
      say("nearcull: a call could not be written: left as it is");
      return;
    }
  }
  say("nearcull: the near views culled with their own field of view (agent.nearcull; %u bytes at VA 0x%lx, the "
      "calls at 0x%lx and 0x%lx)",
      a.n, (unsigned long)g->cave, (unsigned long)g->site[0], (unsigned long)g->site[1]);
}
#endif

/* The game's real VR render size (logs/research/gpu120/REPORT.md): every VR view is drawn at its
 * render mode 3, 672x752 (an immediate: Scarlet 0x4ae415, Fortune 0x4ad7a5) times a PS4 Pro factor,
 * the double 1.4 (Scarlet 0x7bc0b0, Fortune 0x7b4968, each read once) = 940x1052, whatever the eye
 * width: the eye width only sizes the FXAA pass that enlarges that picture into the eye's slices.
 * agent.renderw=N (the Resolution preset's, res_settings; a debug build's cvars.txt too; 941..1280) makes the factor (N + 0.5) / 672: views of N x
 * 752N/672 - the near views, and the wide ones too unless agent.widew puts them on mode 1. The
 * views are made in mode 0's memory: N up to vs_cap (1280 with mode 0's 1920x1080; with the debug
 * agent.mode0 more, 1792 at 2688x1512). The eye width follows N (patch_eye_size). */
static const struct {
  const char *title;
  uint64_t va;
} k_renderw[] = {
    {"CUSA14173", 0x7bc0b0},
    {"CUSA14831", 0x7ba758},
    {"CUSA04555", 0x7b4968},
};

static void patch_renderw(pid_t pid) {
  res_settings();
  unsigned n = g_res.renderw;
  if (g_build < 0 || !n)
    return;
  /* (a VR view is made in mode 0's memory when its own bytes fit there; one that doesn't gets
   * memory of its own, and its depth and colour2 no longer share memory, which likely breaks the
   * effects that read depth: vs_cap, 1280x1432 with mode 0 as it is (measured clean); the agent
   * logs the allocator's fallback flags (at 120 fps or with agent.ticklog) to catch one) */
  unsigned cap = vs_cap(941);
  if (n < 941 || n > cap) {
    say("renderw: %u is not from 941 to %u (within mode 0's %ux%u): the game's own 940", n, cap, g_mode0_w,
        g_mode0_h);
    return;
  }
  for (int i = 0; i < (int)(sizeof k_renderw / sizeof k_renderw[0]); i++) {
    if (strcmp(k_builds[g_build].title_id, k_renderw[i].title) != 0)
      continue;
    uint64_t base = eboot_mapbase(pid);
    static const uint8_t orig[8] = {0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0xf6, 0x3f}; /* 1.4 */
    uint8_t buf[8];
    double f = ((double)n + 0.5) / 672.0;
    if (!base || pt_copyout(pid, base + k_renderw[i].va, buf, 8) != 0 || memcmp(buf, orig, 8) != 0) {
      say("renderw: the Pro factor at VA 0x%lx not as expected: left as it is", (unsigned long)k_renderw[i].va);
      return;
    }
    if (pt_copyin(pid, &f, base + k_renderw[i].va, 8) == 0)
      say("renderw: the VR views drawn at %ux%u (agent.renderw%s; the game's own 940x1052)", n,
          (unsigned)((g_vd_h3 ? (double)g_vd_h3 : 752.0) * f), g_vd_h3 ? ", agent.vdensity" : "");
    else
      say("renderw: the patch could not be written");
  }
}

/* The fade when the head goes into a girl (VR Paradise): the scene tests the head against her body
 * spheres with a radius of 0.11 and fades a black overlay in (to half) while it is inside; the
 * radius is one float used only there (Scarlet 0x7ba1e4, read at 0x4723ca; Fortune 0x7b2a44).
 * agent.headfade=0 (the default; cvars.txt, or the settings menu's "Fade inside her") makes it -1000: the test
 * never passes, the overlay never comes. The 0.09 test beside it (what the scene's reactions read)
 * is left as it is. Off by default (no line: 0); 1: the game's own (logs/research/touch/REPORT.md section 2.2).
 * The blackout 2 cm closer (2026-11-14 worn test: "the screen still goes to black when the headset
 * is too close"): the 0.09 test also stores 2·hit in a word (Scarlet 0x8d4f90, Fortune 0x8ccf94) that
 * the VR input function reads (`cmpl $2, word; je`, Scarlet 0x4be69e, Fortune 0x4bda2e) to set the VR
 * view mode to 2, which skips building and drawing the scene: a black view. The je made two NOPs
 * (the R3 and Square holds that set modes 2 and 1 are kept); the reactions read the 0.09 test's other
 * flag, untouched (logs/research/touch/FADE2.md). */
static const struct {
  const char *title;
  uint64_t va, cut; /* the radius; the `cmpl $2; je` of the blackout */
  uint8_t cmp[9];
} k_headfade[] = {
    {"CUSA14173", 0x7ba1e4, 0x4be69e, {0x83, 0x3d, 0xeb, 0x68, 0x41, 0x00, 0x02, 0x74, 0x22}},
    {"CUSA14831", 0x7b8894, 0x4bcbde, {0x83, 0x3d, 0xb3, 0x83, 0x41, 0x00, 0x02, 0x74, 0x22}},
    {"CUSA04555", 0x7b2a44, 0x4bda2e, {0x83, 0x3d, 0x5f, 0xf5, 0x40, 0x00, 0x02, 0x74, 0x22}},
};

/* The game's VR "GPU too slow" fallback (logs/research/scale/SCENERY.md): each VR frame its GPU time
 * (two frames back, against a 16.68 ms budget) over 0.93 takes a counter from 5 down by 1, over 0.96
 * by 3; at 0 the stage hides every node but 19 (the palms, huts, hills and boats go) and a second trip
 * cuts more; nothing steps it back until the scene ends. A suspend (the PS button, the play-area
 * setup) splits one frame's GPU timestamps across the gap and trips it; so did heavier eye sizes
 * (2720) and slow frames (2048 at 120 fps). The comparison's `jbe` (to the counter's reset to 5) made
 * a `jmp`: the counter stays at 5, the cut never comes. agent.gpufallback=1 keeps the game's own. */
static const struct {
  const char *title;
  uint64_t va;       /* vucomiss budget(%rip), %xmm0 (8 bytes); jbe +0x46 */
  uint8_t orig[10]; /* the ten bytes, the rip displacement too */
} k_gpufallback[] = {
    {"CUSA14173", 0x4a1f5c, {0xc5, 0xf8, 0x2e, 0x05, 0xc8, 0x84, 0x31, 0x00, 0x76, 0x46}},
    {"CUSA14831", 0x4a049c, {0xc5, 0xf8, 0x2e, 0x05, 0x38, 0x86, 0x31, 0x00, 0x76, 0x46}},
    {"CUSA04555", 0x4a12ec, {0xc5, 0xf8, 0x2e, 0x05, 0xa8, 0x19, 0x31, 0x00, 0x76, 0x46}},
};
static void patch_gpufallback(pid_t pid) {
  unsigned keep = (unsigned)cvar_value("agent.gpufallback", 0);
  if (g_build < 0 || keep)
    return;
  for (int i = 0; i < (int)(sizeof k_gpufallback / sizeof k_gpufallback[0]); i++) {
    if (strcmp(k_builds[g_build].title_id, k_gpufallback[i].title) != 0)
      continue;
    uint64_t base = eboot_mapbase(pid);
    uint8_t buf[10];
    static const uint8_t jmp = 0xeb;
    if (!base || pt_copyout(pid, base + k_gpufallback[i].va, buf, 10) != 0 ||
        memcmp(buf, k_gpufallback[i].orig, 10) != 0) {
      say("gpufallback: the code at VA 0x%lx not as expected: the game's own", (unsigned long)k_gpufallback[i].va);
      return;
    }
    if (pt_copyin(pid, &jmp, base + k_gpufallback[i].va + 8, 1) == 0)
      say("gpufallback: the game's VR scenery cut on slow frames off (agent.gpufallback=1 keeps it)");
    else
      say("gpufallback: the patch could not be written");
  }
}

static void patch_headfade(pid_t pid) {
  unsigned on = (unsigned)cvar_value("agent.headfade", 0);
  menu_file_value("agent.headfade", &on); /* the settings menu's line wins */
  if (g_build < 0 || on)
    return;
  for (int i = 0; i < (int)(sizeof k_headfade / sizeof k_headfade[0]); i++) {
    if (strcmp(k_builds[g_build].title_id, k_headfade[i].title) != 0)
      continue;
    uint64_t base = eboot_mapbase(pid);
    static const uint8_t orig[4] = {0xae, 0x47, 0xe1, 0x3d}, never[4] = {0x00, 0x00, 0x7a, 0xc4}; /* 0.11, -1000 */
    uint8_t buf[4];
    if (!base || pt_copyout(pid, base + k_headfade[i].va, buf, 4) != 0 || memcmp(buf, orig, 4) != 0) {
      say("headfade: the radius at VA 0x%lx not as expected: left as it is", (unsigned long)k_headfade[i].va);
      return;
    }
    if (pt_copyin(pid, never, base + k_headfade[i].va, 4) == 0)
      say("headfade: no fade when the head is inside a girl (agent.headfade=0)");
    else
      say("headfade: the patch could not be written");
    uint8_t code[9];
    static const uint8_t nops[2] = {0x90, 0x90};
    if (pt_copyout(pid, base + k_headfade[i].cut, code, 9) != 0 || memcmp(code, k_headfade[i].cmp, 9) != 0)
      say("headfade: the blackout's branch at VA 0x%lx not as expected: left as it is", (unsigned long)k_headfade[i].cut);
    else if (pt_copyin(pid, nops, base + k_headfade[i].cut + 7, 2) == 0)
      say("headfade: no blackout when the head is closer still (VA 0x%lx)", (unsigned long)k_headfade[i].cut + 7);
    else
      say("headfade: the blackout's patch could not be written");
  }
}

/* The world scale: each VR scene's start (four of them) sets two floats to 1.2, always together
 * and beside the VR flag's 1, by a pair of `movl $0x3f99999a, rip` (c7 05 disp32 imm32): the world
 * scale (Scarlet 0x8d4f80, Fortune 0x8ccf84: the model world times it is the space the seat
 * camera, the head and the controller live in; 160 readers, every one a mul, div or load, none
 * comparing it) and the scale its draw items multiply their world matrix by (Scarlet 0x8d5efc,
 * Fortune 0x8cdedc, one reader). The tracked head and the eye spacing go into that space 1:1 in
 * metres, so the girls and the set look 1.2x their model size and the player 1/1.2 of hers. Out
 * of VR both are 1.0 (six other pairs, left as they are). agent.worldscale=N (hundredths, from
 * cvars.txt or the settings menu's "World scale"): the eight immediates made N/100. No line, or
 * 120: the game's own, nothing written. logs/research/scale/REPORT.md section 3. */
static const struct {
  const char *title;
  uint64_t scale, draw;  /* the two floats */
  uint64_t va[8];        /* the instructions: the world scale's, the draw scale's, at four scene starts */
} k_worldscale[] = {
    {"CUSA14173", 0x8d4f80, 0x8d5efc,
     {0x30c426, 0x30c430, 0x30d79f, 0x30d7a9, 0x31c61f, 0x31c629, 0x552c3b, 0x552c45}},
    {"CUSA14831", 0x8d4f88, 0x8d5efc,
     {0x30c0f6, 0x30c100, 0x30d46f, 0x30d479, 0x31c2ef, 0x31c2f9, 0x55114b, 0x551155}},
    {"CUSA04555", 0x8ccf84, 0x8cdedc,
     {0x30c596, 0x30c5a0, 0x30d8ff, 0x30d909, 0x31cb6d, 0x31cb77, 0x552b3b, 0x552b45}},
};
#define WORLDSCALE_MIN 80u
#define WORLDSCALE_MAX 150u

static void patch_worldscale(pid_t pid) {
  unsigned n = (unsigned)cvar_value("agent.worldscale", 120); /* (1.2, the game's own, by default: the user, 2026-11-15) */
  menu_file_value("agent.worldscale", &n); /* the settings menu's line wins */
  if (g_build < 0 || n == 120)
    return;
  if (n < WORLDSCALE_MIN || n > WORLDSCALE_MAX) {
    say("worldscale: %u is not from %u to %u (hundredths): the game's own 1.2", n, WORLDSCALE_MIN, WORLDSCALE_MAX);
    return;
  }
  for (int i = 0; i < (int)(sizeof k_worldscale / sizeof k_worldscale[0]); i++) {
    if (strcmp(k_builds[g_build].title_id, k_worldscale[i].title) != 0)
      continue;
    uint64_t base = eboot_mapbase(pid);
    static const uint8_t imm12[4] = {0x9a, 0x99, 0x99, 0x3f}; /* 1.2 */
    /* every instruction checked before any is written: c7 05, its rip target, the 1.2 */
    for (int s = 0; s < 8; s++) {
      uint8_t buf[10];
      int32_t disp;
      uint64_t va = k_worldscale[i].va[s];
      if (!base || pt_copyout(pid, base + va, buf, 10) != 0) {
        say("worldscale: the code at VA 0x%lx not readable: left as it is", (unsigned long)va);
        return;
      }
      memcpy(&disp, buf + 2, 4);
      if (buf[0] != 0xc7 || buf[1] != 0x05 ||
          va + 10 + (uint64_t)(int64_t)disp != (s & 1 ? k_worldscale[i].draw : k_worldscale[i].scale) ||
          memcmp(buf + 6, imm12, 4) != 0) {
        say("worldscale: the write at VA 0x%lx not as expected: left as it is", (unsigned long)va);
        return;
      }
    }
    float f = (float)n / 100.0f;
    int ok = 1;
    for (int s = 0; s < 8; s++)
      ok &= pt_copyin(pid, &f, base + k_worldscale[i].va[s] + 6, 4) == 0;
    if (ok)
      say("worldscale: the VR scenes at %u.%02u (agent.worldscale=%u; the game's own 1.2)", n / 100, n % 100, n);
    else
      say("worldscale: the patch could not all be written (the scenes may mix it with 1.2)");
  }
}

/* The game is attached and stopped. 0 = the agent is in (the game stays stopped), 1 = the entry
 * point didn't come in wait_ms (the game had started: use the traps), -1 = the load failed,
 * -3 = the game ended. */
static int inject_at_entry(pid_t pid, int wait_ms) {
  uint64_t entry = eboot_entry(pid);
  uint8_t orig = 0, bp = 0xcc;
  if (!entry || pt_copyout(pid, entry, &orig, 1) != 0 || pt_copyin(pid, &bp, entry, 1) != 0) {
    say("entry: no breakpoint possible (entry 0x%lx)", (long)entry);
    return 1;
  }
  say("entry: breakpoint at 0x%lx", (long)entry);
  int sig = 0, st = 0, hit = 0;
  for (int waited = 0; !hit;) {
    pt_tid = 0;
    if (pt_continue(pid, sig) != 0)
      break;
    pid_t w = 0;
    for (; waited < wait_ms; waited++) {
      if ((w = waitpid(pid, &st, WNOHANG)) == -1 && errno == EINTR)
        w = 0;
      if (w != 0)
        break;
      usleep(1000);
    }
    if (w == 0) {
      say("entry: not reached in %d ms (the game had started)", wait_ms);
      kill(pid, SIGSTOP);
      while ((w = waitpid(pid, &st, 0)) == -1 && errno == EINTR)
        ;
      break;
    }
    if (w != pid || !WIFSTOPPED(st)) {
      say("entry: game ended (status 0x%x)", st);
      return -3;
    }
    sig = WSTOPSIG(st);
    pt_tid = pt_event_lwp(pid);
    struct reg r;
    if (sig == SIGTRAP && pt_tid > 0 && pt_getregs(pid, &r) == 0 && r.r_rip == entry + 1) {
      r.r_rip = entry;
      hit = pt_setregs(pid, &r) == 0;
      if (hit)
        say("entry: thread %d at the entry point", pt_tid);
    }
    if (hit || sig == SIGSTOP || sig == SIGTRAP) /* the system's own stops while it starts */
      sig = 0;
  }
  pt_copyin(pid, &orig, entry, 1);
  if (!hit)
    return 1;
  if (read_eboot_imports(pid) != 0 || !slots_bound(pid)) {
    say("entry: imports not bound yet");
    return 1;
  }
  int rc = load_agent(pid);
  pt_tid = 0;
  return rc == 0 ? 0 : -1;
}

static int inject(pid_t pid) {
  choose_agent();
  int unstoppable = 0, entry_tried = 0;
  struct stat fst;
  if (debug_file("trap_only", &fst)) { /* testing the late path */
    say("entry: skipped (" OUT_DIR "/trap_only)");
    entry_tried = 1;
  }
  for (int tries = 0; tries < 400; tries++) {
    int st = 0;
    int a = pt_attach_timed(pid, 300, &st);
    if (a == -1) { /* not attachable yet (exec in progress) */
      usleep(5000);
      continue;
    }
    if (a == -2) { /* attached but it never stopped */
      pt_detach(pid, 0);
      if (++unstoppable >= 20) {
        say("attach: game %d can't be stopped (its PS VR dialog is up?)", pid);
        return -2;
      }
      usleep(20000);
      continue;
    }
    if (a == -3) {
      if (time(NULL) - g_launched_at < 20) {
        /* launched from here: what ended is its first image, the game's is next */
        usleep(20000);
        continue;
      }
      say("attach: game %d ended (status 0x%x)", pid, st);
      return -1;
    }
    int bound = read_eboot_imports(pid) == 0 && slots_bound(pid);
    if (!entry_tried) {
      entry_tried = 1;
      /* bound: the kernel has linked it and the entry point is moments away (or long past,
       * when this payload was sent with the game running) */
      int er = inject_at_entry(pid, bound ? 5000 : 30000);
      if (er == -3)
        return -1;
      if (er == 0) {
        /* (each game start its own: the payload stays resident between games, and a second start
         * reused the first's preset, density and mode 0 size, 2026-11-14 logs/perf/p2.log) */
        memset(&g_res, 0, sizeof g_res);
        g_vd_h3 = 0, g_mode0_w = 1920, g_mode0_h = 1080;
        res_settings(); /* (the Resolution preset, and a debug build's raw sizes over it) */
        patch_mode0(pid, g_res.mode0); /* (first: it raises renderw's and widew's caps) */
        res_mode0_check();             /* (a preset's mode 0 not applied: RES_SHARP) */
        patch_vdensity(pid, g_res.vd); /* (before the eye size, which follows it) */
        patch_eye_size(pid);
        log_sync_read();
        patch_cut_features(pid);
        patch_vr_passport(pid);
        patch_fxaa(pid);
        patch_bootnotes(pid); /* (the autosave notice and the shop data message skipped) */
        patch_headfade(pid);
        patch_gpufallback(pid);
        patch_worldscale(pid);
        patch_renderw(pid); /* (the Resolution preset's render size, res_settings) */
#ifndef RELEASE
        patch_nohole(pid); /* (agent.nohole=1, a debug option) */
#endif
        patch_viewsize(pid); /* (the preset's wide views, agent.widew) */
        patch_nearn(pid); /* (the preset's sharp area, agent.nearn) */
#ifndef RELEASE
        patch_nearcull(pid); /* (agent.nearcull=1, a debug option) */
        patch_uihold(pid); /* (agent.uihold=1, a debug option: the menu/message layer's span held at 120 fps) */
#endif
        /* what is at libSceAgcDriver's fixed addresses (0xfe0000000 on) as the game starts:
         * read only (run 4's MAP_FIXED probe replaced whatever was there and killed the game) */
        log_vmmap(pid, 0xf00000000ull, "at the entry point");
        pt_detach(pid, 0);
        pt_tid = 0;
        say("agent in at the entry point");
        return 0;
      }
      if (er == -1) { /* (the load failed: the game let go, not left stopped and traced for good) */
        pt_tid = 0;
        pt_detach(pid, 0);
        return -1;
      }
      bound = read_eboot_imports(pid) == 0 && slots_bound(pid);
    }
    if (!bound) { /* still loading its modules */
      pt_detach(pid, 0);
      usleep(10000);
      continue;
    }
    /* Dead or Alive Xtreme 3 sets PS VR up once, at its boot (the headset and tracker
     * libraries, the reprojection and its start event), long before a trap would fire: an agent
     * put in later would never see that, and the game's first VR frame would wait for an event
     * nobody raises. Only from the entry point. */
    pt_detach(pid, 0);
    pt_tid = 0;
    say("the game had started before this payload: not gone into (its PS VR setup at boot is past)");
    return -1;
    int rc = inject_by_trap(pid);
    if (rc == -3)
      return -1;
    if (rc == -2) /* it stopped once but not again: the dialog came up */
      return -2;
    pt_detach(pid, 0);
    pt_tid = 0;
    if (rc == 0) {
      say("agent in through the traps (try %d)", tries);
      return 0;
    }
    usleep(20000);
  }
  say("no trap fired");
  return -1;
}

/* ---- hand tracking (libSceVrHand in the game) ---- */
/* CMD_CAMERA: libSceHmd2 opens the headset cameras (/dev/hmd2_image, /dev/hmd2_leddet) for
 * libSceVrHand, and an app's credentials are refused them (0x8a721003). The game gets this
 * payload's capabilities (its authid and attributes stay its own), as PS5VR's helper does for
 * PS5VR ("elevate <pid> 1"). Only asked for while hand tracking starts: cmd_arg[0] = 1 raises
 * them, 0 gives the game its own back (once the cameras are open). Answer in cmd_arg[0]. */
static uint8_t g_game_caps[16];
static pid_t g_game_caps_pid = -1;

static void open_cameras(pid_t pid, uint64_t shared, uint64_t raise) {
  uint8_t caps[16] = {0}, mine[16] = {0};
  int64_t rc = -1;
  if (!raise) {
    if (g_game_caps_pid == pid)
      rc = kernel_set_ucred_caps(pid, g_game_caps);
    if (rc == 0)
      g_game_caps_pid = -1;
    say("cameras: the game's own capabilities back (%ld)", (long)rc);
  } else if (kernel_get_ucred_caps(pid, caps) == 0 && kernel_get_ucred_caps(getpid(), mine) == 0) {
    if (g_game_caps_pid != pid) {
      memcpy(g_game_caps, caps, sizeof caps);
      g_game_caps_pid = pid;
    }
    rc = kernel_set_ucred_caps(pid, mine);
    say("cameras: game capabilities %016lx%016lx -> the payload's (%ld)",
        *(unsigned long *)(caps + 8), *(unsigned long *)caps, (long)rc);
  }
  kernel_proc_copyin(pid, &rc, shared + offsetof(struct a_shared_head, cmd_arg), 8);
}

/* The game's memory map from `floor` up (KERN_PROC_VMMAP entries: start, end, protection), to the
 * log: libSceAgcDriver maps its command buffers at fixed addresses near the top of the 64 GB
 * space (0xfe0000000 on), where run 3's fault was. */
static void log_vmmap(pid_t game, uint64_t floor, const char *when) {
  int mib[4] = {1, 14, 32, game};
  size_t size = 0;
  if (sysctl(mib, 4, NULL, &size, NULL, 0) != 0 || !size) {
    say("vmmap %s: query failed (errno %d)", when, errno);
    return;
  }
  size += size / 4;
  uint8_t *map = malloc(size);
  if (!map || sysctl(mib, 4, map, &size, NULL, 0) != 0) {
    free(map);
    say("vmmap %s: read failed (errno %d)", when, errno);
    return;
  }
  int n = 0, shown = 0;
  uint64_t top = 0;
  for (uint8_t *e = map; e + 64 <= map + size;) {
    int es = *(int *)e;
    if (es <= 0)
      break;
    uint64_t start = *(uint64_t *)(e + 8), end = *(uint64_t *)(e + 16);
    int prot = *(int *)(e + 56);
    const char *name = es > 0x80 ? (const char *)(e + 0x80) : "";
    e += es;
    n++;
    if (end > top)
      top = end;
    if (end > floor && shown++ < 40)
      say("vmmap %s: 0x%lx-0x%lx prot %d %.32s", when, (unsigned long)start, (unsigned long)end,
          prot, name);
  }
  say("vmmap %s: %d entries, the highest ends at 0x%lx", when, n, (unsigned long)top);
  free(map);
}

/* The game watched with ptrace while the agent loads the first PS5 library (run 1: the game died
 * inside that load with nothing in any log; /dev/klog has another reader on this console). Every
 * stop is logged with its thread, signal, fault address and registers and the signal passed on;
 * the watch ends when the agent's next request shows the load returned, when the game ends, or
 * after `ms`. The log relay pauses meanwhile. */
static void watch_game(pid_t pid, uint64_t shared, uint32_t seq_acked, int ms) {
  int st = 0, a = pt_attach_timed(pid, 500, &st);
  if (a != 0) {
    say("watch: attach -> %d (status 0x%x): not watched", a, st);
    if (a == -2)
      pt_detach(pid, 0);
    return;
  }
  say("watch: attached for the first library's load (up to %d ms)", ms);
  int sig = 0, waited = 0, stops = 0;
  for (;;) {
    pt_tid = 0;
    if (pt_continue(pid, sig) != 0) {
      say("watch: continue failed (errno %d)", errno);
      break;
    }
    sig = 0;
    pid_t w = 0;
    for (; waited < ms; waited++) {
      if ((w = waitpid(pid, &st, WNOHANG)) == -1 && errno == EINTR)
        w = 0;
      if (w != 0)
        break;
      struct a_shared_head sh;
      if (waited % 10 == 9 && kernel_proc_copyout(pid, shared, &sh, sizeof sh) == 0 &&
          sh.cmd_seq != seq_acked) {
        say("watch: the load returned (the agent's next request is up)");
        waited = ms;
        break;
      }
      usleep(1000);
    }
    if (w == 0) { /* time, or the load returned: stop it to detach */
      kill(pid, SIGSTOP);
      for (int t = 0; t < 2000 && (w = waitpid(pid, &st, WNOHANG)) == 0; t++)
        usleep(1000);
      if (w == pid && WIFSTOPPED(st))
        break;
      say("watch: the game did not stop for the detach (w %d, status 0x%x)", (int)w, st);
      if (w == pid && !WIFSTOPPED(st))
        return;
      break;
    }
    if (w != pid) {
      say("watch: wait failed (errno %d)", errno);
      return;
    }
    if (!WIFSTOPPED(st)) {
      say("watch: THE GAME ENDED: status 0x%x (%s %d)", st, WIFSIGNALED(st) ? "signal" : "exit code",
          WIFSIGNALED(st) ? WTERMSIG(st) : WEXITSTATUS(st));
      return;
    }
    sig = WSTOPSIG(st);
    pt_tid = pt_event_lwp(pid);
    if (sig == SIGSTOP) { /* the system's own stops */
      sig = 0;
      continue;
    }
    uint64_t addr = 0;
    int si = pt_event_siginfo(pid, &addr);
    struct reg r;
    memset(&r, 0, sizeof r);
    int gr = pt_tid > 0 ? pt_getregs(pid, &r) : -1;
    say("watch: thread %d stopped with signal %d (siginfo %d, address 0x%lx)%s: rip 0x%lx rsp 0x%lx "
        "rax 0x%lx rdi 0x%lx rsi 0x%lx",
        (int)pt_tid, sig, si, (unsigned long)addr, gr == 0 ? "" : " (no registers)",
        (unsigned long)r.r_rip, (unsigned long)r.r_rsp, (unsigned long)r.r_rax,
        (unsigned long)r.r_rdi, (unsigned long)r.r_rsi);
    if (++stops >= 40) {
      say("watch: %d stops: enough", stops);
      break;
    }
    /* the signal goes on to the game (a fatal one ends it, and the end is logged above) */
  }
  pt_tid = 0;
  pt_detach(pid, sig);
  say("watch: detached (%d stops)", stops);
}

/* Relay the agent's log and serve its requests until the game ends (0) or a newer injector
 * asks this one to leave (1). */
/* the agent's log ring (struct agent_shared: log[] follows the 0x40-byte head), what is new
 * since g_rpos, relayed to the console and the file; from the serve loop, and from kill_game
 * after the agent's stop (the loop is this thread, waiting in the kill then) */
static uint64_t g_rpos;
static void relay_log(pid_t pid, uint64_t shared, const struct a_shared_head *sh) {
  static char ring[LOG_SIZE];
  if (sh->log_wpos == g_rpos)
    return;
  if (sh->log_wpos - g_rpos > LOG_SIZE)
    g_rpos = sh->log_wpos - LOG_SIZE;
  uint64_t n = sh->log_wpos - g_rpos;
  uint64_t start = g_rpos % LOG_SIZE, first = n < LOG_SIZE - start ? n : LOG_SIZE - start;
  kernel_proc_copyout(pid, shared + 0x40 + start, ring, first);
  if (n > first)
    kernel_proc_copyout(pid, shared + 0x40, ring + first, n - first);
  fwrite(ring, 1, n, stdout);
  fflush(stdout);
  if (g_logf) {
    netlog(ring, n);
    if (log_to_storage()) {
      fwrite(ring, 1, n, g_logf);
      log_synced(g_logf);
    }
  }
  g_rpos = sh->log_wpos;
}

static int serve(pid_t pid, intptr_t proc, int adopted) {
  uint64_t shared = mirror_header()->shared;
  /* a new game: its pid for the parser, the level states not known until its lines say, the
   * markers' counts over (cam_new_game), the reading started over (g_msgbuf_gen), all before the
   * reader may act for it (g_live_pid) */
  pthread_mutex_lock(&g_cam_lock);
  cam_new_game(&g_cam, (uint32_t)pid, mono_us());
  g_msgbuf_gen++;
  g_adopt_history = adopted;
  pthread_mutex_unlock(&g_cam_lock);
  g_live_pid = pid, g_live_shared = shared;
  g_rpos = 0;
  int cam_ok = 0;
  if (adopted) /* the previous payload already relayed its log */
    kernel_proc_copyout(pid, shared, &g_rpos, 8);
  for (int ticks = 0;; ticks++) {
    struct a_shared_head sh;
    if (!game_alive(pid, proc) || kernel_proc_copyout(pid, shared, &sh, sizeof sh) != 0) {
      g_netonly = 0; /* (the storage again: the game's risky part is over) */
      say("game gone");
      g_live_pid = 0, g_live_shared = 0;
      return 0;
    }
    relay_log(pid, shared, &sh);
    if (ticks % 5 == 0)
      publish_cam_state(pid, shared, &cam_ok);
    if (sh.cmd >= 2 && sh.cmd <= 15 && sh.cmd_seq != sh.cmd_ack) {
      /* the ptrace watch of the first library's load (run 3) only with OUT_DIR/watch: it found
       * its crash, and the console went down once during that bring-up with it attached
       * (measurement 4, whose code patch turned out to be a no-op) */
      struct stat wst;
      int watch = sh.cmd == 7 && sh.cmd_arg[0] == 0 && sh.cmd_arg[1] == 0 && !adopted &&
                  debug_file("watch", &wst);
      if (sh.cmd == 2)
        say("resolved %d late imports", resolve_missing(pid));
      else if (sh.cmd == 5) {
        say("pointed %d unbound imports at the agent's stub",
            stub_unbound(pid, sh.cmd_arg, mirror_header()->unbound_stub));
        unmount_libs(0); /* the libraries are loaded */
      }
      else if (sh.cmd == 7)
        stage_lib(sh.cmd_arg[0], sh.cmd_arg[1]);
      else if (sh.cmd == 8) {
        FILE *f = fopen(OUT_DIR "/ipd", "w");
        if (f) {
          fprintf(f, "%u\n", (unsigned)sh.cmd_arg[0]);
          fclose(f);
        }
      }
      else if (sh.cmd == 9) {
        char msg[200] = {0};
        kernel_proc_copyout(pid, sh.cmd_arg[0], msg, sizeof msg - 1);
        notify("%s", msg);
      }
      else if (sh.cmd == 6) {
        char nid[16] = {0};
        kernel_proc_copyout(pid, sh.cmd_arg[1], nid, 15);
        override_import(pid, sh.cmd_arg[0], nid, sh.cmd_arg[2]);
      }
      else if (sh.cmd == 11)
        open_cameras(pid, shared, sh.cmd_arg[0]);
      else if (sh.cmd == 12) { /* cvars.txt (console variables by name) into the agent's buffer */
        /* the shared file, then the game's own (cvars-<key>.txt): applied in order, its lines win */
        static char text[4096];
        char game[96];
        size_t room = sh.cmd_arg[1] < sizeof text ? sh.cmd_arg[1] : sizeof text, n = 0, own = 0;
        memset(text, 0, sizeof text);
        const char *paths[2] = {OUT_DIR "/cvars.txt", game_file(game, sizeof game, "cvars")};
        for (int i = 0; i < 2 && room > 1; i++) {
          FILE *f = paths[i] ? fopen(paths[i], "r") : NULL;
          if (!f)
            continue;
          if (n && text[n - 1] != '\n' && n < room - 1)
            text[n++] = '\n';
          size_t got = n < room - 1 ? fread(text + n, 1, room - 1 - n, f) : 0;
          fclose(f);
          n += got;
          if (i == 1)
            own = got;
        }
        text[n] = 0;
        kernel_proc_copyin(pid, text, sh.cmd_arg[0], n + 1);
        if (own)
          say("cvars.txt and %s: %zu bytes for the agent (%zu the game's own)", game + sizeof OUT_DIR, n, own);
        else
          say("cvars.txt: %zu bytes for the agent", n);
      }
      else if (sh.cmd == 14) { /* the settings menu's file (menu-<key>.txt): cmd_arg[2] 0, read into
                                * the agent's buffer; 1, written from it (nothing: removed) */
        static char text[512];
        char game[96];
        const char *path = game_file(game, sizeof game, "menu");
        size_t n = sh.cmd_arg[1] < sizeof text ? sh.cmd_arg[1] : sizeof text;
        const char *fname = game + sizeof OUT_DIR;
        memset(text, 0, sizeof text);
        if (!path)
          say("menu: the game is not known: no settings file");
        else if (sh.cmd_arg[2] == 0) {
          FILE *f = fopen(path, "r");
          size_t got = f && n ? fread(text, 1, n - 1, f) : 0;
          if (f)
            fclose(f);
          text[got] = 0;
          if (n)
            kernel_proc_copyin(pid, text, sh.cmd_arg[0], got + 1); /* (got + 1 <= n: within the agent's buffer) */
          if (f)
            say("%s: %zu bytes for the agent (the settings menu)", fname, got);
        } else if (n == 0) {
          say("%s: %s (the settings menu: all at their defaults)", fname, unlink(path) == 0 ? "removed" : "none");
        } else {
          FILE *f = kernel_proc_copyout(pid, sh.cmd_arg[0], text, n) == 0 ? fopen(path, "w") : NULL;
          int ok = f && fwrite(text, 1, n, f) == n;
          if (f && fclose(f) != 0) /* (once: the write's error can come here) */
            ok = 0;
          if (ok)
            say("%s: %zu bytes saved (the settings menu)", fname, n);
          else
            say("%s: not saved", fname);
        }
      }
      else if (sh.cmd == 15) { /* a test tool: the agent's copy of the game's flat display buffer to
                                * shot.raw (written beside it, then renamed: never read half done) */
        static uint8_t chunk[65536];
        uint64_t at = sh.cmd_arg[0], left = sh.cmd_arg[1] <= (64u << 20) ? sh.cmd_arg[1] : 0;
        FILE *f = left ? fopen(OUT_DIR "/shot.tmp", "wb") : NULL;
        int ok = f != NULL;
        while (ok && left) {
          size_t n = left < sizeof chunk ? (size_t)left : sizeof chunk;
          ok = kernel_proc_copyout(pid, at, chunk, n) == 0 && fwrite(chunk, 1, n, f) == n;
          at += n, left -= n;
        }
        if (f && fclose(f) != 0)
          ok = 0;
        if (ok)
          ok = rename(OUT_DIR "/shot.tmp", OUT_DIR "/shot.raw") == 0;
        say("shot %u: %lu bytes %s", (unsigned)sh.cmd_arg[2], (unsigned long)sh.cmd_arg[1], ok ? "in shot.raw" : "not saved");
      }
      kernel_proc_copyin(pid, &sh.cmd_seq, shared + offsetof(struct a_shared_head, cmd_ack), 4);
      if (watch) {
        log_vmmap(pid, 0xf00000000ull, "before the load");
        watch_game(pid, shared, sh.cmd_seq, 8000);
        if (game_alive(pid, proc))
          log_vmmap(pid, 0xf00000000ull, "after the load");
      }
    }
    if (sh.repatch) {
      uint32_t zero = 0;
      repatch(pid);
      kernel_proc_copyin(pid, &zero, shared + offsetof(struct a_shared_head, repatch), 4);
    }
    if (ticks % 25 == 24) {
      if (!g_avrec_done && g_av_patched && ticks < 25 * 20)
        av_mark_vr2_record(pid);
      repatch(pid); /* the loader may bind the game's imports after the hooks went in */
      if (ctl_quit_requested(0)) {
        say("a newer injector took over");
        return 1;
      }
    }
    usleep(sh.repatch ? 1000 : 20000);
  }
}

/* Resident: waits for the game (launched from the home screen or anywhere else), puts the
 * agent in as the game starts, serves it until the game ends, then waits for the next start. */
int main(void) {
  signal(SIGPIPE, SIG_IGN); /* a control-port client gone away must not end the injector */
  mkdir(OUT_DIR, 0777);
  g_logf = fopen(OUT_DIR "/agent.log", "a");
  unsigned fw = kernel_get_fw_version() >> 16;
  g_fw = astro_firmware(fw);
  if (!g_fw || !g_fw->av[0].va) {
    /* Not supported yet, or the AV control service of this firmware is still missing: save
     * what a profile is made from, decrypted, for adding one (common/dump.c, the firmware part) */
    notify("DOAX3VR2 doesn't support firmware %x.%02x yet. Saving the files for adding it (about a "
           "minute; wait for the next notice).", fw >> 8, fw & 0xff);
    struct dump_result r;
    dump_save(NULL, &r);
    say("firmware %x.%02x unsupported: %u of %u libraries saved, %u failed, " DUMP_ROOT "/%s%s", fw >> 8,
        fw & 0xff, r.libs_ok, r.libs, r.fail, r.name, r.tar_rc == 0 ? ".tar" : " (no .tar)");
    if (r.tar_rc == 0 && r.libs_ok == r.libs)
      notify("DOAX3VR2: the files for adding firmware %s are saved. Send " DUMP_ROOT "/%s.tar to the "
             "developer (docs/DUMP.md).", r.fw, r.name);
    else
      notify("DOAX3VR2: the files for firmware %s couldn't all be saved (%u of %u libraries). Send the "
             "developer " DUMP_ROOT "/%s%s and its dump.log.", r.fw, r.libs_ok, r.libs, r.name,
             r.tar_rc == 0 ? ".tar" : "");
    return 1;
  }
  say("firmware %s", g_fw->name);
  if (prepare(OUT_DIR, g_fw) != 0 || load_gcn(OUT_DIR, &g_gcn, g_gcn_code, GCN_MAX_DWORDS) != 0) {
    notify("DOAX3VR2: the PS VR2 files couldn't be built. Details in " OUT_DIR "/agent.log.");
    return 1;
  }
  g_ctl = ctl_open();
  if (g_ctl < 0) {
    /* never two instances at once (both would attach to the game, patch the AV service and read
     * the kernel): this one leaves instead */
    say("the control port (%d) could not be opened for 10 s: another DOAX3VR2 still running, or "
        "the port not free. Not starting; send it again in a few seconds.", CTL_PORT);
    notify("DOAX3VR2: another copy is still running (or port %d is not free). Send it again in a "
           "few seconds.", CTL_PORT);
    return 1;
  }
  choose_agent();
  say("resident (pid %d), agent %ld bytes", (int)getpid(), (long)g_agent_size);
  msgbuf_start(); /* the kernel message buffer, for the palm tracker's gate (every build) */
#ifndef RELEASE
  {
    pthread_t nt;
    if (pthread_create(&nt, NULL, netlog_thread, NULL) == 0)
      pthread_detach(nt);
  }
  klog_start(); /* (a release build keeps no copy of the kernel log: written to storage all along) */
#endif
  if (find_game(-1) < 0) /* a re-send with the game open says nothing */
    notify("DOAX3VR2 is ready. Start " TITLE_NAME ".");
  pid_t done = -1;
  for (;;) {
    pid_t pid = g_launching ? -1 : find_game(done); /* not while the launcher makes it */
    if (pid < 0) {
      if (ctl_quit_requested(1)) {
        say("a newer injector took over");
        return 0;
      }
      usleep(5000);
      continue;
    }
    intptr_t proc = kernel_get_proc(pid);
    if (g_logf)
      fclose(g_logf);
    g_logf = fopen(OUT_DIR "/agent.log", "a");
    say("game pid %d (" TITLE_NAME ": %s %s)", pid, GAME_NAME,
        g_build < 0 ? "?" : k_builds[g_build].version);
    int adopted = adopt_agent(pid) == 0;
    if (adopted)
      log_sync_read(); /* (agent.hands / agent.netlog: no log to the storage, as at a game's start) */
    find_sandbox(); /* this game's (its title's): where its PS VR2 libraries are mounted */
    /* ps-patch-system's patch-server stops the game on every module it loads and walks and
     * patches its modules; with it running, the PS VR2 libraries' load took Ghost VR's console
     * down 4 times (its main.c). A new game is left alone; one the agent is already in is served
     * as before (its libraries may be up already, and an agent left without us would wait). */
    if (find_proc("patch-server.elf") >= 0) {
      if (!adopted) {
        notify("DOAX3VR2: ps-patch-system (patch-server) is running. It can crash the "
               "console when the PS VR2 libraries load, so DOAX3VR2 stays out of this game. "
               "Stop it in your payload manager and start " TITLE_NAME " again.");
        say("patch-server.elf is running: not injecting into pid %d", pid);
        done = pid;
        continue;
      }
      say("patch-server.elf is running (the agent is already in pid %d: served as before)", pid);
    }
    if (!adopted) {
      mark_vr2_app(pid);
      /* The game opens its "Connect PS VR" setup dialog early and polls it from its main loop,
       * so a thread passes through game code now and then; the agent's dialog hooks close it. */
      int ir = inject(pid);
      if (ir != 0) {
        if (game_alive(pid, proc))
          notify(ir == -2 ? "DOAX3VR2 came too late: the game's PS VR screen was already up. "
                            "Close " TITLE_NAME " and start it again (DOAX3VR2 stays ready)."
                          : "DOAX3VR2 couldn't get into the game. Close " TITLE_NAME
                            " and start it again.");
        say("load failed");
        done = pid;
        reset_game_state();
        continue;
      }
      say("hooks live");
      unlink(OUT_DIR "/test.txt"); /* a test run is one start: the next is an ordinary one */
      FILE *st = fopen(OUT_DIR "/agent_state", "w");
      if (st) {
        fprintf(st, "%d 0x%lx 0x%lx 0x%lx\n", pid, (long)g_base, (long)g_hdroff, (long)g_size);
        fclose(st);
      }
    }
    patch_av_service();
    int quit = serve(pid, proc, adopted);
    restore_av_service();
    unmount_libs(1);
    done = pid;
    reset_game_state();
    if (quit)
      return 0;
  }
}
