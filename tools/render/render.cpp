// SPDX-License-Identifier: GPL-3.0-only
// Renders README screenshots with the real firmware drawing code.
//
// Builds src/main.cpp for the PC (BUBBLE_HOST leaves out the hardware), plays
// scripted scenes at the device's 30 fps, and writes each scene as raw 8-bit
// frames (240x320, one byte per pixel) plus the palette. make_media.py turns
// them into GIFs and PNGs.
//
//   g++ -std=gnu++17 -O2 -DBUBBLE_HOST -Itools/render/shim -Isrc tools/render/render.cpp -o /tmp/bubble-render
//   /tmp/bubble-render <out-dir>
#include <Arduino.h>
uint32_t g_hostMillis = 0;
#include "../../src/main.cpp"

#include <string>

static std::string g_dir;
static float g_t = 0;
static const float DT = 1 / 30.f;

static void step() {
  g_hostMillis = (uint32_t)(g_t * 1000);
  helpers_feed(g_t, DT);
  frame(g_t, DT);
  g_t += DT;
}
static void run(float secs) { for (int i = 0, n = (int)(secs * 30); i < n; i++) step(); }

struct Scene {
  FILE *f; int every, n = 0, frames = 0;
  Scene(const char *name, int every_) : every(every_) { f = fopen((g_dir + "/" + name + ".raw").c_str(), "wb"); }
  void capture(float secs) {   // step for `secs`, keeping every `every`-th frame
    for (int i = 0, k = (int)(secs * 30); i < k; i++) { step(); if (n++ % every == 0) { for (int y = 0; y < H; y++) fwrite(fb[y], 1, W, f); frames++; } }
  }
  void still() { for (int y = 0; y < H; y++) fwrite(fb[y], 1, W, f); frames++; }
  ~Scene() { fclose(f); printf("  %d frames\n", frames); }
};

static void pin(Mode m) { ST.src = SRC_PINNED; setMode(m, g_t); }

int main(int argc, char **argv) {
  g_dir = argc > 1 ? argv[1] : ".";
  for (int s0 = 0; s0 < H; s0 += STRIP) {
    uint8_t *strip = (uint8_t *)malloc(W * STRIP);
    for (int y = 0; y < STRIP; y++) fbRows[0][s0 + y] = strip + y * W;
  }
  memcpy(fbRows[1], fbRows[0], sizeof fbRows[0]); fb = fbs[0];
  palettes(); helper_palettes(); scene_init();
  g_t = 20; pin(THINK); run(3);   // let the motes settle

  { puts("hero"); Scene s("hero", 2);
    pin(THINK); s.capture(2.6); pin(WORK); s.capture(4.0); pin(ASK); s.capture(2.8); pin(SLEEP); s.capture(2.8); pin(THINK); s.capture(.6); }
  { puts("states"); Scene s("states", 1);
    for (Mode m : {SLEEP, THINK, WORK, ASK, ERR}) { pin(m); run(3.2); s.still(); } }
  { puts("handoff + whisper"); pin(WORK); run(2); demoT0 = g_t;
    { Scene s("handoff", 2); s.capture(5.2); }
    run(10.7 - 5.2);
    { Scene s("whisper", 2); s.capture(5.6); }
    run(26); }
  { puts("tickle"); pin(THINK); run(2); Scene s("tickle", 2);
    s.capture(.6);
    TP.downAt = g_t;
    for (int i = 0; i < 70; i++) { TP.down = true; TP.x = 120 + 18 * sinf(g_t * 14); TP.y = 168; TP.last = g_t; s.capture(DT); }
    TP.down = false; s.capture(1.8); }
  { puts("pet"); pin(SLEEP); run(2); Scene s("pet", 2);
    s.capture(.5); TP.downAt = g_t;
    for (int i = 0; i < 90; i++) { TP.down = true; TP.x = 128; TP.y = 150; TP.last = g_t; s.capture(DT); }
    TP.down = false; s.capture(1.2); }

  FILE *p = fopen((g_dir + "/palette.bin").c_str(), "wb");
  for (int i = 0; i < npal; i++) { uint8_t c[3] = {(uint8_t)(palRGB[i] >> 16), (uint8_t)(palRGB[i] >> 8), (uint8_t)palRGB[i]}; fwrite(c, 1, 3, p); }
  fclose(p);
  printf("palette %d colours\n", npal);
}
