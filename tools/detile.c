// SPDX-License-Identifier: GPL-3.0-or-later
/* detile RAW OUT.ppm [WIDTH HEIGHT [PIPES [SCALE [MODE [PITCH [OFFSET]]]]]]: a PS4 display buffer as the GPU holds it
 * (agent.shot's shot.raw: 32 bits a pixel, the "display, 2D thin" tile mode) to a picture.
 * PIPES: 16 (a PS4 Pro's layout, which these games get on a PS5; found and checked on Fortune's
 * screens, 2026-11-10) or 8 (a base PS4's, untested). SCALE: 1 of every SCALE pixels kept (2).
 *
 * The layout at 16 pipes, as measured (edge matching of the tiles of real screens; AMD's addrlib
 * in outline, its pipe bits as found): 8x8 micro tiles of 256 bytes, their pixels in the
 * displayable order (x0 x1 y0 x2 y1 y2); macro tiles of 128x128 pixels (16 pipes x 16 banks, one
 * micro tile each), in rows of pitch / 128; within one, the pipe (the address's bits 8-11) is
 * {x3^x4^y3, x4^y4, x5^y5, x6^y5} and the bank (bits 12-15) {y6^X0, y6^y5^X1, y4^X2, y3}, x and y
 * the pixel's bits, X the macro tile's column. A surface's rows go up to a multiple of 128.
 * MODE "thin": a render target's or texture's 2D thin tiling (tile mode 14: the eye textures),
 * which differs in the pixels' order within a micro tile (x0 y0 x1 y1 x2 y2). PITCH: the
 * surface's (default: WIDTH up to a multiple of 64). OFFSET: where the surface starts in the file
 * (a slice of an array: slice x PITCH x HEIGHT-up-to-128 x 4), and SLICE its number (argument 10).
 *   cc -O2 -o tools/detile tools/detile.c */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned bit(unsigned v, unsigned n) { return (v >> n) & 1; }

static int g_thin;
static unsigned g_rot, g_rot_xor;
static unsigned g_slice; /* an array's slice: its banks are xored with (3 x slice) & 7 (addrlib's slice rotation, as measured) */
static uint64_t addr(unsigned x, unsigned y, unsigned pitch, unsigned pipes) {
  const unsigned banks = 16, bank_w = 1, bank_h = 1, aspect = pipes == 16 ? 1 : 2, bpp = 32;
  /* the pixel within its micro tile (displayable, 32 bpp) */
  unsigned pix = g_thin ? bit(x, 0) | bit(y, 0) << 1 | bit(x, 1) << 2 | bit(y, 1) << 3 | bit(x, 2) << 4 |
                              bit(y, 2) << 5
                        : bit(x, 0) | bit(x, 1) << 1 | bit(y, 0) << 2 | bit(x, 2) << 3 | bit(y, 1) << 4 |
                              bit(y, 2) << 5;
  unsigned pipe, bank;
  if (pipes == 16)
    pipe = (bit(x, 3) ^ bit(y, 3) ^ bit(x, 4)) | (bit(x, 4) ^ bit(y, 4)) << 1 | (bit(x, 5) ^ bit(y, 5)) << 2 |
           (bit(x, 6) ^ bit(y, 5)) << 3;
  else
    pipe = (bit(x, 3) ^ bit(y, 3) ^ bit(x, 4)) | (bit(x, 4) ^ bit(y, 4)) << 1 |
           (bit(x, 5) ^ bit(y, 5)) << 2;
  unsigned tx = x / 8 / (bank_w * pipes), ty = y / 8 / bank_h;
  bank = (bit(ty, 3) ^ bit(tx, 0)) | (bit(ty, 3) ^ bit(ty, 2) ^ bit(tx, 1)) << 1 |
         (bit(ty, 1) ^ bit(tx, 2)) << 2 | bit(ty, 0) << 3;
  bank = g_rot_xor ? bank ^ g_rot : (bank + g_rot) & (banks - 1);
  unsigned mt_pitch = 8 * bank_w * pipes * aspect, mt_height = 8 * bank_h * banks / aspect;
  uint64_t mt_bytes = (uint64_t)mt_pitch * mt_height * bpp / 8 / (pipes * banks);
  uint64_t mt_per_row = pitch / mt_pitch;
  uint64_t mt_off = ((uint64_t)(y / mt_height) * mt_per_row + x / mt_pitch) * mt_bytes;
  unsigned tile_row = (y / 8) % bank_h, tile_col = ((x / 8) / pipes) % bank_w;
  uint64_t total = mt_off + (uint64_t)(tile_row * bank_w + tile_col) * 256 + pix * 4;
  unsigned pipe_bits = pipes == 16 ? 4 : 3, bank_bits = 4;
  uint64_t bankpipe = pipe | bank << pipe_bits;
  return (total >> 8) << (8 + pipe_bits + bank_bits) | bankpipe << 8 | (total & 0xff);
}

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "detile RAW OUT.ppm [WIDTH HEIGHT [PIPES [SCALE [display|thin [PITCH [OFFSET]]]]]]\n");
    return 2;
  }
  unsigned w = argc > 4 ? atoi(argv[3]) : 1920, h = argc > 4 ? atoi(argv[4]) : 1080;
  unsigned pipes = argc > 5 ? atoi(argv[5]) : 16, scale = argc > 6 ? atoi(argv[6]) : 2;
  unsigned pitch = argc > 8 ? atoi(argv[8]) : (w + 63) & ~63u;
  uint64_t base = argc > 9 ? strtoull(argv[9], 0, 0) : 0;
  g_thin = argc > 7 && argv[7][0] == 't';
  g_slice = argc > 10 ? atoi(argv[10]) : 0;
  g_rot = (g_slice * 3) & 7, g_rot_xor = 1; /* (measured on Fortune's eye array: slices 0-3 -> 0, 3, 6, 1) */
  if (argc > 11) /* (a search: the rotation given, added or, with a 13th argument, xored) */
    g_rot = atoi(argv[11]), g_rot_xor = argc > 12;
  FILE *f = fopen(argv[1], "rb");
  if (!f)
    return 1;
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *raw = malloc(size);
  if (!raw || fread(raw, 1, size, f) != (size_t)size)
    return 1;
  fclose(f);
  { /* how well rows of tiles join: the mean step across tile rows' edges (stderr) */
    unsigned long sum = 0, n = 0;
    for (unsigned y = 7; y + 1 < h; y += 8)
      for (unsigned x = 0; x < w; x += 3) {
        uint64_t a = base + addr(x, y, pitch, pipes), b = base + addr(x, y + 1, pitch, pipes);
        if (a + 4 <= (uint64_t)size && b + 4 <= (uint64_t)size)
          sum += abs(raw[a + 1] - raw[b + 1]), n++;
      }
    fprintf(stderr, "row joins: %.2f\n", n ? (double)sum / n : 0.0);
  }
  FILE *o = fopen(argv[2], "wb");
  if (!o)
    return 1;
  fprintf(o, "P6\n%u %u\n255\n", w / scale, h / scale);
  unsigned long outside = 0;
  for (unsigned y = 0; y + scale <= h; y += scale)
    for (unsigned x = 0; x + scale <= w; x += scale) {
      uint64_t a = base + addr(x, y, pitch, pipes);
      uint8_t px[3] = {0, 0, 0};
      if (a + 4 <= (uint64_t)size)
        px[0] = raw[a + 2], px[1] = raw[a + 1], px[2] = raw[a]; /* B8G8R8A8 in memory */
      else
        outside++;
      fwrite(px, 1, 3, o);
    }
  fclose(o);
  if (outside)
    fprintf(stderr, "%lu pixels addressed past the file (%ld bytes)\n", outside, size);
  return 0;
}
