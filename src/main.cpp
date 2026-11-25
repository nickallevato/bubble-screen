// SPDX-License-Identifier: GPL-3.0-only
// bubble-screen: firmware for the ESP32 "Cheap Yellow Display" (ESP32-2432S028R).
//
// One screen, one character: a Paperclip agent, or BUBBLES in demo mode. It is
// drawn into an 8-bit indexed framebuffer (240x320 = 75 KB) by plain C, then
// streamed to the ILI9341 in 16-row strips: each strip is palette-expanded to
// RGB565 while the previous one is still going out over DMA. No LVGL, no
// display library.
//
// Serial console (115200 baud):
//   live | demo                          choose the source (saved)
//   sleep | think | work | ask | error   pin a state
//   helpers                              play the helper demo once
//   set host|port|psk|ssid|pass <value>  service address, password, Wi-Fi (saved)
//   invert | flip                        panel colour inversion, rotate 180 (saved)
//   panel ili9341|st7789                 display controller (saved, reboots)
//   skew <seconds>                       jump the clock ahead (tests the rebase)

#include <Arduino.h>
#include <Preferences.h>
#include <math.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "net.h"

// ---------------------------------------------------------------- panel ---
#define PIN_MOSI 13
#define PIN_MISO 12
#define PIN_SCLK 14
#define PIN_CS   15
#define PIN_DC   2
#define PIN_BL   21

static const int W = 240, H = 320, SKY = 286, STRIP = 16;   // portrait
// BUBBLE_HOST builds the drawing code on a PC (tools/render) to make screenshots;
// everything that touches hardware is left out of that build.
#ifndef BUBBLE_HOST
static spi_device_handle_t lcd;
static uint16_t *dbuf[2];
static spi_transaction_t trs[2];
static bool g_invert = true, g_flip = false;
// Some 2432S028R revisions carry an ST7789 instead of the ILI9341: different init,
// SPI mode 3, RGB rather than BGR, and the glass mounted mirrored.
static bool g_st7789 = false;

static void IRAM_ATTR lcd_pre(spi_transaction_t *t) { gpio_set_level((gpio_num_t)PIN_DC, (int)(intptr_t)t->user); }
static void lcd_cmd(uint8_t c) {
  spi_transaction_t t; memset(&t, 0, sizeof t);
  t.length = 8; t.flags = SPI_TRANS_USE_TXDATA; t.tx_data[0] = c; t.user = (void *)0;
  spi_device_polling_transmit(lcd, &t);
}
static void lcd_data(const uint8_t *d, int n) {
  if (!n) return;
  spi_transaction_t t; memset(&t, 0, sizeof t);
  t.length = n * 8; t.user = (void *)1;
  if (n <= 4) { t.flags = SPI_TRANS_USE_TXDATA; memcpy(t.tx_data, d, n); } else t.tx_buffer = d;
  spi_device_polling_transmit(lcd, &t);
}
static void lcd_cmd_data(uint8_t c, std::initializer_list<uint8_t> d) {
  uint8_t b[16]; int n = 0; for (uint8_t v : d) b[n++] = v;
  lcd_cmd(c); lcd_data(b, n);
}
static void lcd_orient() {
  if (g_st7789) lcd_cmd_data(0x36, {(uint8_t)(g_flip ? 0xC0 : 0x00)});   // RGB; MX|MY when flipped
  else lcd_cmd_data(0x36, {(uint8_t)(g_flip ? 0x88 : 0x48)});   // MX (MY when flipped) | BGR: portrait, as the stand holds it
  lcd_cmd(g_invert ? 0x21 : 0x20);
}
static void lcd_init() {
  gpio_set_direction((gpio_num_t)PIN_DC, GPIO_MODE_OUTPUT);
  gpio_set_direction((gpio_num_t)PIN_BL, GPIO_MODE_OUTPUT);
  gpio_set_level((gpio_num_t)PIN_BL, 0);
  spi_bus_config_t bus = {};
  bus.mosi_io_num = PIN_MOSI; bus.miso_io_num = PIN_MISO; bus.sclk_io_num = PIN_SCLK;
  bus.quadwp_io_num = -1; bus.quadhd_io_num = -1; bus.max_transfer_sz = W * STRIP * 2 + 8;
  ESP_ERROR_CHECK(spi_bus_initialize(HSPI_HOST, &bus, SPI_DMA_CH_AUTO));
  spi_device_interface_config_t dev = {};
  dev.clock_speed_hz = SPI_MASTER_FREQ_80M; dev.mode = g_st7789 ? 3 : 0; dev.spics_io_num = PIN_CS;
  dev.queue_size = 3; dev.pre_cb = lcd_pre; dev.flags = SPI_DEVICE_NO_DUMMY;
  ESP_ERROR_CHECK(spi_bus_add_device(HSPI_HOST, &dev, &lcd));
  for (int i = 0; i < 2; i++) dbuf[i] = (uint16_t *)heap_caps_malloc(W * STRIP * 2, MALLOC_CAP_DMA);

  lcd_cmd(0x01); delay(g_st7789 ? 150 : 10);   // software reset (no reset pin on this board)
  if (g_st7789) {
    lcd_cmd(0x11); delay(120);           // sleep out
    lcd_cmd_data(0x3A, {0x55});          // 16-bit colour
    lcd_cmd_data(0xC6, {0x01});          // 111 Hz refresh, like the ILI9341's 119 Hz
    lcd_orient();
    lcd_cmd(0x29);
    return;
  }
  // The init table esp32_smartdisplay uses for the 2432S028Rv2.
  lcd_cmd_data(0xCF, {0x00, 0xC1, 0x30});
  lcd_cmd_data(0xED, {0x64, 0x03, 0x12, 0x81});
  lcd_cmd_data(0xE8, {0x85, 0x00, 0x78});
  lcd_cmd_data(0xCB, {0x39, 0x2C, 0x00, 0x34, 0x02});
  lcd_cmd_data(0xF7, {0x20});
  lcd_cmd_data(0xEA, {0x00, 0x00});
  lcd_cmd_data(0xC0, {0x10});
  lcd_cmd_data(0xC1, {0x00});
  lcd_cmd_data(0xC5, {0x30, 0x30});
  lcd_cmd_data(0xC7, {0xB7});
  lcd_cmd_data(0x3A, {0x55});
  lcd_cmd_data(0xB1, {0x00, 0x10});   // 119 Hz panel refresh: a tear lives for half as long
  lcd_cmd_data(0xB6, {0x08, 0x82, 0x27});
  lcd_cmd_data(0xF2, {0x00});
  lcd_cmd_data(0x26, {0x01});
  lcd_cmd_data(0xE0, {0x0F, 0x2A, 0x28, 0x08, 0x0E, 0x08, 0x54, 0xA9, 0x43, 0x0A, 0x0F, 0x00, 0x00, 0x00, 0x00});
  lcd_cmd_data(0xE1, {0x00, 0x15, 0x17, 0x07, 0x11, 0x06, 0x2B, 0x56, 0x3C, 0x05, 0x10, 0x0F, 0x3F, 0x3F, 0x0F});
  lcd_orient();
  lcd_cmd(0x11); delay(120);  // sleep out
  lcd_cmd(0x29);              // display on (backlight stays off until the first frame lands)
}
#endif  // BUBBLE_HOST

// ------------------------------------------------------- framebuffer -----
// Each framebuffer is 20 strips of 16 rows (3.8 KB each) addressed through a row
// table: with Wi-Fi linked in, the heap's largest free block is only ~34 KB.
static uint8_t *fbRows[2][H];
static uint8_t **fb, **fbs[2] = {fbRows[0], fbRows[1]};
static uint16_t pal565[256];
static uint32_t palRGB[256];
static int npal = 0;
static uint8_t C(uint32_t rgb) {
  for (int i = 0; i < npal; i++) if (palRGB[i] == rgb) return i;
  palRGB[npal] = rgb;
  uint16_t c = ((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x1F);
  pal565[npal] = (c >> 8) | (c << 8);   // byte-swapped: SPI sends MSB first
  return npal++;
}

#ifndef BUBBLE_HOST
static void lcd_push(uint8_t *const *rows) {
  lcd_cmd_data(0x2A, {0, 0, (uint8_t)((W - 1) >> 8), (uint8_t)((W - 1) & 255)});
  lcd_cmd_data(0x2B, {0, 0, (uint8_t)((H - 1) >> 8), (uint8_t)((H - 1) & 255)});
  lcd_cmd(0x2C);
  int inflight = 0;
  for (int s = 0; s < H / STRIP; s++) {
    int b = s & 1;
    if (inflight == 2) { spi_transaction_t *r; spi_device_get_trans_result(lcd, &r, portMAX_DELAY); inflight--; }
    const uint8_t *src = rows[s * STRIP]; uint16_t *dst = dbuf[b];   // one strip = one allocation
    for (int i = 0; i < W * STRIP; i++) dst[i] = pal565[src[i]];
    memset(&trs[b], 0, sizeof trs[b]);
    trs[b].length = W * STRIP * 16; trs[b].tx_buffer = dst; trs[b].user = (void *)1;
    spi_device_queue_trans(lcd, &trs[b], portMAX_DELAY); inflight++;
  }
  while (inflight--) { spi_transaction_t *r; spi_device_get_trans_result(lcd, &r, portMAX_DELAY); }
}
#endif  // BUBBLE_HOST

// --------------------------------------------------------- drawing -------
static const uint8_t BAYER[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
// newlib's sinf falls off a cliff for |x| > ~201 (slow argument reduction), and
// animation phases like t*1.3 + 40*K cross that a couple of minutes after boot.
// Reduce to [-pi, pi] first; animation needs no more precision than this.
static inline float wsin(float x) { x -= 6.2831853f * floorf(x * 0.15915494f + 0.5f); return sinf(x); }
static inline float wcos(float x) { return wsin(x + 1.5707963f); }
static inline int bay(int x, int y) { return BAYER[((y & 3) << 2) | (x & 3)]; }
static inline float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }
static inline int rnd(float v) { return (int)floorf(v + 0.5f); }
static uint32_t rs = 0x9E3779B9;
static inline float frand() { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return (rs & 0xFFFFFF) / 16777216.0f; }
static inline float hash3(int x, int y, int s) {
  uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + (uint32_t)s * 2246822519u;
  h = (h ^ (h >> 13)) * 1274126177u; h ^= h >> 16; return (h & 0xFFFF) / 65536.0f;
}

static inline void px(int x, int y, uint8_t c) { if ((unsigned)x < (unsigned)W && (unsigned)y < (unsigned)H) fb[y][x] = c; }
static inline void pxf(float x, float y, uint8_t c) { px(rnd(x), rnd(y), c); }
static void rect(float fx, float fy, int w, int h, uint8_t c) {
  int x = rnd(fx), y = rnd(fy);
  int x0 = max(0, x), y0 = max(0, y), x1 = min(W, x + w), y1 = min(H, y + h);
  for (int j = y0; j < y1; j++) memset(fb[j] + x0, c, max(0, x1 - x0));
}
// filled oval; rim<0 means no rim; checker leaves every other pixel
static void oval(float cx, float cy, float rx, float ry, uint8_t col, int rim = -1, bool checker = false) {
  int x0 = (int)floorf(cx - rx - 1), x1 = (int)ceilf(cx + rx + 1), y0 = (int)floorf(cy - ry - 1), y1 = (int)ceilf(cy + ry + 1);
  for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) {
    float dx = (x + .5f - cx) / rx, dy = (y + .5f - cy) / ry, d = dx * dx + dy * dy;
    if (d > 1) continue; if (checker && ((x + y) & 1)) continue;
    px(x, y, (rim >= 0 && d > .62f) ? (uint8_t)rim : col);
  }
}
static void line(float x0, float y0, float x1, float y1, uint8_t c) {
  int n = (int)fmaxf(fabsf(x1 - x0), fabsf(y1 - y0)); if (!n) n = 1;
  for (int s = 0; s <= n; s++) pxf(x0 + (x1 - x0) * s / n, y0 + (y1 - y0) * s / n, c);
}

// 5x7 font, uppercase
static const uint8_t F_AZ[26][7] = {
  {14,17,17,31,17,17,17},{30,17,17,30,17,17,30},{14,17,16,16,16,17,14},{30,17,17,17,17,17,30},
  {31,16,16,30,16,16,31},{31,16,16,30,16,16,16},{14,17,16,23,17,17,15},{17,17,17,31,17,17,17},
  {14,4,4,4,4,4,14},{7,2,2,2,2,18,12},{17,18,20,24,20,18,17},{16,16,16,16,16,16,31},
  {17,27,21,21,17,17,17},{17,17,25,21,19,17,17},{14,17,17,17,17,17,14},{30,17,17,30,16,16,16},
  {14,17,17,17,21,18,13},{30,17,17,30,20,18,17},{15,16,16,14,1,1,30},{31,4,4,4,4,4,4},
  {17,17,17,17,17,17,14},{17,17,17,17,17,10,4},{17,17,17,21,21,21,10},{17,17,10,4,10,17,17},
  {17,17,10,4,4,4,4},{31,1,2,4,8,16,31}};
static const uint8_t F_09[10][7] = {
  {14,17,19,21,25,17,14},{4,12,4,4,4,4,14},{14,17,1,2,4,8,31},{31,2,4,2,1,17,14},{2,6,10,18,31,2,2},
  {31,16,30,1,1,17,14},{6,8,16,30,17,17,14},{31,1,2,4,8,8,8},{14,17,17,14,17,17,14},{14,17,17,15,1,2,12}};
static const uint8_t *glyph(char ch) {
  static const uint8_t SP[7] = {0}, COL[7] = {0,12,12,0,12,12,0}, DOT[7] = {0,0,0,0,0,12,12}, DASH[7] = {0,0,0,31,0,0,0},
    Q[7] = {14,17,1,2,4,0,4}, EX[7] = {4,4,4,4,4,0,4}, SL[7] = {1,1,2,4,8,16,16}, AP[7] = {12,4,8,0,0,0,0},
    CM[7] = {0,0,0,0,12,4,8}, DOL[7] = {4,15,20,14,5,30,4}, PCT[7] = {24,25,2,4,8,19,3}, AMP[7] = {12,18,20,8,21,18,13}, HASH[7] = {10,10,31,10,31,10,10};
  if (ch >= 'a' && ch <= 'z') ch -= 32;
  if (ch >= 'A' && ch <= 'Z') return F_AZ[ch - 'A'];
  if (ch >= '0' && ch <= '9') return F_09[ch - '0'];
  switch (ch) { case ':': return COL; case '.': return DOT; case '-': return DASH; case '?': return Q; case '!': return EX;
    case '/': return SL; case '\'': return AP; case ',': return CM; case '$': return DOL; case '%': return PCT; case '&': return AMP; case '#': return HASH; }
  return SP;
}
static int textW(const char *s, int k = 1) { return (int)strlen(s) * 6 * k - k; }
static void text(const char *s, float fx, float fy, uint8_t col, int k = 1, int cx0 = 0, int cx1 = W) {
  int x = (int)fx, y = (int)fy;
  for (; *s; s++, x += 6 * k) {
    const uint8_t *g = glyph(*s);
    for (int r = 0; r < 7; r++) { if (!g[r]) continue;
      for (int c = 0; c < 5; c++) if (g[r] & (16 >> c))
        for (int j = 0; j < k; j++) for (int i = 0; i < k; i++) { int xx = x + c * k + i; if (xx >= cx0 && xx < cx1) px(xx, y + r * k + j, col); } }
  }
}
static void ticker(const char *s, int x, int y, int maxW, uint8_t col, float t) {
  int w = textW(s); if (w <= maxW) { text(s, x, y, col); return; }
  int period = w + 48, off = (int)fmodf(t * 15, (float)period);
  text(s, x - off, y, col, 1, x, x + maxW); text(s, x - off + period, y, col, 1, x, x + maxW);
}

// ------------------------------------------------------------ state ------
enum Mode { SLEEP, THINK, WORK, ASK, ERR };
static const char *LABEL[] = {"RESTING", "THINKING", "WORKING", "NEEDS YOU", "STUCK"};
// Example text for demo mode.
static const char *TASK[] = {"ALL CAUGHT UP - NEXT CHECK 7:00 AM", "PLANNING THE WEEK'S MEALS", "WEEKLY MEAL PLAN",
                             "OK TO PLACE THE GROCERY ORDER? $84.20", "CAN'T REACH THE STORE - RETRYING IN 5 MIN"};
static const float DUR[] = {7, 7, 14, 7, 7};
static const int ELAPSED_BASE[] = {2460, 71, 154, 38, 12};
struct Tool { int icon; const char *verb, *obj; };
static const Tool TOOLS[4] = {{0, "CHECKING CALENDAR", "3 DINNERS FREE THIS WEEK"}, {1, "WRITING THE MENU", "TACOS, CURRY, PASTA"},
                              {2, "FILLING THE CART", "GROCERY CART - 18 ITEMS"}, {3, "SENDING EMAIL", "MENU SENT TO THE HOUSE"}};
static const char *ICONS[4][12] = {
  {"..#......#..","############","############","#..........#","#.##.##.##.#","#.##.##.##.#","#..........#","#.##.##.##.#","#.##.##.##.#","#..........#","############","............"},
  {"##.########.","##..........","............","##.######...","##..........","............","##.#######..","##..........","............","##.#####....","##..........","............"},
  {"##..........",".#..........",".##########.",".#########..",".########...",".#######....",".#..........",".#########..","............","..##....##..","..##....##..","............"},
  {"............","############","##........##","#.#......#.#","#..#....#..#","#...#..#...#","#....##....#","#..........#","#..........#","############","............","............"}};
static uint8_t iconCells[4][144][2]; static int iconN[4];
static uint8_t qCells[35][2]; static int qN = 0;

// Where the state comes from: the bubble-screen service (live), the demo, or a state pinned from serial.
enum Source { SRC_LIVE, SRC_DEMO, SRC_PINNED };
struct { Mode mode = THINK, prev = THINK; float since = 0, changeAt = -10; Source src = SRC_LIVE; } ST;
static LiveStatus LS = {};          // last live status (valid when LS.have)
static uint32_t lsFetchMs = 0;      // millis() when LS arrived
static char g_agentName[13] = "BUBBLES";   // shown in the status strip; the live agent's Paperclip name, kept in NVS
static const uint32_t OFFLINE_MS = 120000;
static void setMode(Mode m, float t) { ST.prev = ST.mode; ST.mode = m; ST.since = t; ST.changeAt = t; }
static int toolIdx(float t) { return ((int)floorf((t - ST.since) / 3.5f)) % 4; }

struct Pal { uint8_t sky[3], puff, body, shade, hl, rim, cheek, mote, mote2, ant, hud, ink, chip, floor; };
static Pal P[5];
static uint8_t EYE, WHITE, MOUTH, TONGUE, SWEAT, STAR;
static void mkpal(Pal &p, std::initializer_list<uint32_t> v) {
  const uint32_t *c = v.begin();
  for (int i = 0; i < 3; i++) p.sky[i] = C(c[i]);
  p.puff = C(c[3]); p.body = C(c[4]); p.shade = C(c[5]); p.hl = C(c[6]); p.rim = C(c[7]); p.cheek = C(c[8]);
  p.mote = C(c[9]); p.mote2 = C(c[10]); p.ant = C(c[11]); p.hud = C(c[12]); p.ink = C(c[13]); p.chip = C(c[14]); p.floor = C(c[15]);
}
static void palettes() {
  //                 sky0      sky1      sky2      puff      body      shade     hl        rim       cheek     mote      mote2     ant       hud       ink       chip      floor
  mkpal(P[SLEEP], {0x0e1530, 0x182246, 0x24305c, 0x1d2850, 0xcfc3ee, 0x9d93c9, 0xece6ff, 0x6f68a0, 0xc99ad6, 0xc9d0ff, 0x5d6aa8, 0x8a93d6, 0x080c1f, 0xcfd6ff, 0x8a93d6, 0x0a1026});
  mkpal(P[THINK], {0x123049, 0x1b4a66, 0x2a6680, 0x21546f, 0xffe9cc, 0xe9b98f, 0xfff8ec, 0xb07a5a, 0xff9aa0, 0xc8f6ff, 0x5fb6cf, 0x7fe3ff, 0x0b1d2c, 0xe6f6ff, 0x7fe3ff, 0x16405a});
  mkpal(P[WORK],  {0x12303f, 0x1b4a55, 0x29655f, 0x215560, 0xffe9cc, 0xe9b98f, 0xfff8ec, 0xb07a5a, 0xff9aa0, 0xd4ffdc, 0x62b98a, 0x9cf2a8, 0x0b1f24, 0xe6fff0, 0x9cf2a8, 0x175045});
  mkpal(P[ASK],   {0x3a1f4f, 0x5a2d63, 0x83406f, 0x6a3569, 0xfff0dc, 0xf0b9a0, 0xffffff, 0xb06f6a, 0xff8fa8, 0xffe8a0, 0xd08a6a, 0xffd36b, 0x2a1236, 0xffeedd, 0xffd36b, 0x4a2456});
  mkpal(P[ERR],   {0x2a2118, 0x3f3020, 0x584229, 0x4a3825, 0xf2dfc4, 0xcfa780, 0xfff6e6, 0x8a6544, 0xe8a08a, 0xffd9a0, 0xa87a48, 0xffae5a, 0x1c150e, 0xf6e7d2, 0xffae5a, 0x33271a});
  EYE = C(0x2a1e2e); WHITE = C(0xffffff); MOUTH = C(0x5a2a3a); TONGUE = C(0xff8fa0); SWEAT = C(0x9fdcff); STAR = C(0xe6e9ff);
}

// ------------------------------------------------------------ touch ------
// XPT2046 on its own pins, bit-banged: a few hundred microseconds per read.
#ifndef BUBBLE_HOST
#define T_CLK  25
#define T_MOSI 32
#define T_MISO 39
#define T_CS   33
static void tp_init() { pinMode(T_CLK, OUTPUT); pinMode(T_MOSI, OUTPUT); pinMode(T_CS, OUTPUT); pinMode(T_MISO, INPUT);
  digitalWrite(T_CS, 1); digitalWrite(T_CLK, 0); }
static uint16_t tp_xfer(uint8_t cmd) {
  for (int i = 7; i >= 0; i--) { digitalWrite(T_MOSI, (cmd >> i) & 1); digitalWrite(T_CLK, 1); delayMicroseconds(1); digitalWrite(T_CLK, 0); delayMicroseconds(1); }
  digitalWrite(T_MOSI, 0);   // DIN must stay low while reading, or the chip sees a new start bit
  uint16_t v = 0;
  for (int i = 0; i < 16; i++) { digitalWrite(T_CLK, 1); delayMicroseconds(1); v = (v << 1) | digitalRead(T_MISO); digitalWrite(T_CLK, 0); delayMicroseconds(1); }
  return (v >> 3) & 0xFFF;
}
static bool tp_read(int &rx, int &ry, int &rz) {
  digitalWrite(T_CS, 0);
  int z1 = tp_xfer(0xB1), z2 = tp_xfer(0xC1); rz = z1 + 4095 - z2;
  int xs = 0, ys = 0; for (int i = 0; i < 3; i++) { xs += tp_xfer(0xD1); ys += tp_xfer(0x91); }
  tp_xfer(0xD0); digitalWrite(T_CS, 1);
  rx = xs / 3; ry = ys / 3;
  return rz > 400;
}
#endif  // BUBBLE_HOST
// Raw -> screen, from corner taps on this unit (2026-11-21): raw X runs
// right-to-left (~3500 at the left edge, ~630 at the right), raw Y top-to-bottom
// (~430 at the top, ~3470 just above the status strip).
static float tp_sx(int rx) { return clampf(120 - (rx - 2067) * 0.0768f, 0, 239); }
static float tp_sy(int ry) { return clampf(10 + (ry - 430) * 0.0888f, 0, 319); }
struct { bool down = false, was = false; float x = 0, y = 0, px = 0, py = 0, downAt = 0, last = -10; } TP;
static float tickle = 0, pet = 0, rub = 0, nextHeart = 0, nextHehe = 0;
struct Pop { float x, y, a, vx; bool on; bool heart; } pops[16];

// ------------------------------------------------------------ scene ------
static uint8_t rowB[SKY], rowF[SKY];
struct Star { int16_t x, y; float p; } stars[60];
struct Puff { float x, y, s, v; } puffs[4] = {{30, 34, 1, 3}, {170, 16, .7f, 2}, {200, 236, .9f, 4}, {10, 258, .8f, 5}};
struct Zz { float x, y, a; bool on; } zs[6];
static const int N = 240;
static float X[N], Y[N], VX[N], VY[N], A[N], U[N], K[N];
static int16_t TRX[N][2], TRY[N][2];
static uint8_t L[N], FR[N], VIS[N];
static float bx = 120, by = 160, toolAt = 0, nextZ = 0;
static int lastMode = -1, lastTool = -1;

static void puff_build(int k, float s);
static void scene_init() {
  for (int y = 0; y < SKY; y++) { float f = (float)y / SKY * 2; int b = min(1, (int)floorf(f)); rowB[y] = b; rowF[y] = rnd(clampf((f - b - .3f) / .4f, 0, 1) * 16); }
  for (auto &s : stars) { s.x = frand() * W; s.y = frand() * 210; s.p = frand() * 6.28f; }
  for (int i = 0; i < N; i++) { X[i] = frand() * W; Y[i] = frand() * SKY; A[i] = frand() * 6.2832f; U[i] = frand(); K[i] = frand();
    TRX[i][0] = TRX[i][1] = X[i]; TRY[i][0] = TRY[i][1] = Y[i]; }
  for (int k = 0; k < 4; k++) { iconN[k] = 0; for (int r = 0; r < 12; r++) for (int c = 0; c < 12; c++) if (ICONS[k][r][c] == '#') { iconCells[k][iconN[k]][0] = c; iconCells[k][iconN[k]][1] = r; iconN[k]++; } }
  for (int k = 0; k < 4; k++) puff_build(k, puffs[k].s);
  const uint8_t *g = glyph('?'); for (int r = 0; r < 7; r++) for (int c = 0; c < 5; c++) if (g[r] & (16 >> c)) { qCells[qN][0] = c; qCells[qN][1] = r; qN++; }
}

// Each far puff's fuzzy shape is fixed, so build it once as a list of offsets.
static int8_t *puffMask[4][2]; static int puffN[4];
static void puff_build(int k, float s) {
  static const float D[4][3] = {{0, 0, 10}, {11, -5, 13}, {24, 0, 10}, {12, 3, 10}};
  static uint8_t grid[80][60]; memset(grid, 0, sizeof grid);
  for (auto &d : D) { float r = d[2] * s, x0 = 10 + d[0] * s, y0 = 25 + d[1] * s;
    for (int y = (int)floorf(y0 - r - 2); y <= y0 + r + 2; y++) for (int x = (int)floorf(x0 - r - 2); x <= x0 + r + 2; x++) {
      if (x < 0 || y < 0 || x >= 80 || y >= 60) continue;
      if (hypotf(x - x0, y - y0) < r * (.85f + .3f * hash3(x, y, k + 1))) grid[x][y] = 1; } }
  int n = 0; for (int x = 0; x < 80; x++) for (int y = 0; y < 60; y++) n += grid[x][y];
  puffMask[k][0] = (int8_t *)malloc(n); puffMask[k][1] = (int8_t *)malloc(n); puffN[k] = 0;
  for (int x = 0; x < 80; x++) for (int y = 0; y < 60; y++) if (grid[x][y]) { puffMask[k][0][puffN[k]] = x - 10; puffMask[k][1][puffN[k]] = y - 25; puffN[k]++; }
}
static void puff(int k, float cx, float cy, uint8_t col) {
  int x0 = rnd(cx), y0 = rnd(cy); const int8_t *mx = puffMask[k][0], *my = puffMask[k][1];
  for (int i = 0; i < puffN[k]; i++) px(x0 + mx[i], y0 + my[i], col);
}
static void drawMotes(int layer, const Pal &cp) {
  for (int i = 0; i < N; i++) { if (!VIS[i] || FR[i] != layer) continue;
    int x = rnd(X[i]), y = rnd(Y[i]); if (y >= SKY) continue;
    if (((TRX[i][1] + TRY[i][1]) & 1) == 0 && TRY[i][1] < SKY) px(TRX[i][1], TRY[i][1], cp.mote2);
    if (TRY[i][0] < SKY) px(TRX[i][0], TRY[i][0], cp.mote2);
    px(x, y, cp.mote);
  }
}
static void targetGrid(const uint8_t cells[][2], int n, int i, int k, float x0, float y0, float &gx, float &gy) {
  int kk = k * k, j = (int)(((uint32_t)i * 7919u) % (uint32_t)(n * kk)), cell = j / kk, sub = j % kk;
  gx = x0 + cells[cell][0] * k + sub % k; gy = y0 + cells[cell][1] * k + sub / k;
}

// ------------------------------------------------------------ helpers -----
// The other agents in the company. The main agent tosses one a work order off the side,
// that edge fizzes while they work off-screen, then they pop in and whisper the
// result in his ear. Fed from LiveStatus.helpers, or the serial `helpers` demo.
struct HLook { uint8_t body, shade, rim, feat; float rx, ry; };   // feat: 0 none, 1 headset, 2 glasses, 3 hat, 4 leaf
static HLook LOOKS[7];
static uint8_t PAPER, PAPERLINE, LEAF;
static void helper_palettes() {
  LOOKS[0] = {C(0x9fe3e0), C(0x5fb3b8), C(0x2f6f78), 1, 11, 12};   // teal, headset
  LOOKS[1] = {C(0xffc46b), C(0xe09a3e), C(0x9a5e1e), 2, 10, 11};   // orange, glasses
  LOOKS[2] = {C(0xc8b6ff), C(0x9a86e0), C(0x5d4aa6), 3, 9, 14};    // lilac, top hat
  LOOKS[3] = {C(0xb6e388), C(0x7fb455), C(0x47702a), 4, 10, 12};   // green, leaf
  LOOKS[4] = {C(0xffb3c7), C(0xe07e9a), C(0x9a4560), 0, 10, 12};   // pink
  LOOKS[5] = {C(0xa9c8ff), C(0x6f95d8), C(0x3b5a96), 0, 11, 11};
  LOOKS[6] = {C(0xf3e08a), C(0xc9b052), C(0x87732a), 0, 10, 13};
  PAPER = C(0xf6f1e4); PAPERLINE = C(0xb9ab8c); LEAF = C(0x3f8f3a);
}
// Each helper keeps the same look, picked from a hash of its agent id.
static uint8_t lookFor(const char *id) {
  uint32_t h = 0; for (const char *p = id; *p; p++) h = h * 31 + (uint8_t)*p;
  return h % 7;
}
// Sized by model: Sonnet 1.1x the base sprite, Opus twice Sonnet, Haiku a little smaller.
static float modelScale(uint8_t model) { return model == 2 ? 2.2f : model == 0 ? 0.8f : 1.1f; }
static inline float seg(float t, float a, float b) { return clampf((t - a) / (b - a), 0, 1); }
static inline float easeOut(float t) { t = 1 - t; return 1 - t * t * t; }
static inline float backOut(float t) { const float c = 1.7f; t -= 1; return 1 + (c + 1) * t * t * t + c * t * t; }

static void drawHelper(const HLook &a, float k, float x, float y, float t, int dir, bool talk, const Pal &cp) {
  const float rx = a.rx * k, ry = a.ry * k, cy = rnd(y + wsin(t * 5 + x) * 1.2f * k);
  auto Rk = [&](float v) { return (int)lroundf(v * k); };
  oval(x, y + ry + 4 * k, rx * .8f, 2.5f * k, cp.floor, -1, true);
  for (int yy = (int)floorf(cy - ry - 1); yy <= cy + ry + 1; yy++) for (int xx = (int)floorf(x - rx - 1); xx <= x + rx + 1; xx++) {
    float dx = (xx + .5f - x) / rx, dy = (yy + .5f - cy) / ry, d = dx * dx + dy * dy; if (d > 1) continue;
    float th = bay(xx, yy) / 16.0f;
    px(xx, yy, d > .74f && (d - .74f) * 4 > th ? a.rim : (-dx * .4f - dy * .8f < -.25f - th * .3f ? a.shade : a.body));
  }
  const float ey = cy - Rk(2); const int ew = max(2, Rk(2)), eh = max(3, Rk(3)), g = max(1, Rk(1));
  for (int s = -1; s <= 1; s += 2) {
    float ex = x + s * 4 * k + dir * k;
    rect(ex - ew / 2.f, ey - eh / 2.f, ew, eh, EYE); rect(ex - ew / 2.f, ey - eh / 2.f, g, g, WHITE);
    if (a.feat == 2) { int n = (int)(16 * k); for (int q = 0; q < n; q++) { float an = q * 6.2832f / n; pxf(ex + wcos(an) * 3.2f * k, ey + wsin(an) * 3.2f * k, a.rim); } }
  }
  if (a.feat == 2) line(x - k + dir * k, ey, x + k + dir * k, ey, a.rim);
  const float my = ey + Rk(6);
  if (talk && fmodf(t * 8, 1) < .5f) oval(x + dir * k, my, 2 * k, 1.6f * k, MOUTH); else line(x - Rk(1) + dir * k, my, x + Rk(1) + dir * k, my, EYE);
  if (a.feat == 1) { int n = (int)(12 * k); for (int q = 0; q <= n; q++) { float an = 3.14159f + q * 3.14159f / n; pxf(x + wcos(an) * (rx + 1), cy - Rk(3) + wsin(an) * (ry + 1), EYE); }
    line(x + dir * (rx + 1), cy - Rk(3), x + dir * Rk(5), cy + Rk(5), EYE); rect(x + dir * Rk(5) - Rk(1), cy + Rk(4), max(1, Rk(2)), max(1, Rk(2)), EYE); }
  if (a.feat == 3) { rect(x - Rk(6), cy - ry - Rk(1), Rk(13), max(1, Rk(2)), EYE); rect(x - Rk(4), cy - ry - Rk(7), Rk(9), Rk(6), EYE); rect(x - Rk(4), cy - ry - Rk(3), Rk(9), g, a.shade); }
  if (a.feat == 4) { oval(x + 2 * k, cy - ry - 2 * k, 2.6f * k, 1.6f * k, LEAF); line(x, cy - ry, x + k, cy - ry - 2 * k, LEAF); }
  for (int s = -1; s <= 1; s += 2) oval(x + s * (rx + k), cy + Rk(4), 2.5f * k, 2.2f * k, a.body, a.rim);
}

// A short band of fizzing streaks, centred on one edge at the character's height.
static void streakBand(int side, float I, float t, const Pal &cp) {
  if (I <= .01f) return;
  const int ts = (int)floorf(t * 14), mid = 160, half = 58;
  for (int y = mid - half; y <= mid + half; y += 2) {
    float fade = 1 - fabsf((float)(y - mid)) / half, h = hash3(y, ts, side ? 2 : 1);
    if (h > .5f * I * fade) continue;
    int len = 2 + (int)(hash3(y, ts, 7) * 12 * I * (.4f + .6f * fade));
    float c3 = hash3(y, ts, 9); uint8_t col = c3 < .15f ? WHITE : c3 < .6f ? cp.mote : cp.mote2;
    int x0 = side == 0 ? 1 : W - 1 - len;
    for (int i = 0; i < len; i++) if (i < len - 2 || ((i + y) & 1)) px(x0 + i, y, col);
  }
  if (fmodf(t * 1.7f, 1) < .12f) {
    int y = mid - 30 + (int)(hash3((int)floorf(t * 1.7f), 3, side ? 4 : 3) * 60), len = 14 + (int)(hash3(y, 1, 1) * 8);
    for (int i = 0; i < len; i++) if (((i + y) & 1) || i < len / 2) px(side == 0 ? 1 + i : W - 2 - i, y, cp.mote);
  }
}

static void drawCard(float x, float y) {
  rect(x - 7, y - 8, 14, 16, PAPERLINE); rect(x - 6, y - 7, 12, 14, PAPER);
  rect(x - 4, y - 4, 8, 1, PAPERLINE); rect(x - 4, y - 1, 8, 1, PAPERLINE); rect(x - 4, y + 2, 5, 1, PAPERLINE);
}

static void wrap2(const char *s, char *a, char *b, int n);
// Whisper bubble: three dots, then the result types out over up to two lines.
static void drawBubble(const char *msg, float tt, float cx, float y, float tailx) {
  char l1[40], l2[40]; wrap2(msg, l1, l2, 34);
  char a[40] = "", b[40] = "";
  if (tt < .45f) { int d = 1 + ((int)(tt * 7)) % 3; memset(a, '.', d); a[d] = 0; }
  else { int n = (int)((tt - .45f) * 20), n1 = strlen(l1);
    strlcpy(a, l1, min(n, n1) + 1); if (n > n1) strlcpy(b, l2, min(n - n1, (int)strlen(l2)) + 1); }
  const int w = tt < .45f ? 18 : max(textW(l1), *l2 ? textW(l2) : 0) + 8, h = (tt >= .45f && *l2) ? 23 : 13;
  const int xx = (int)clampf(cx - w / 2.f, 2, W - w - 2), yy = (int)max(2.f, y - (h - 13));
  rect(xx + 1, yy, w - 2, h, WHITE); rect(xx, yy + 1, w, h - 2, WHITE);
  int tx = (int)clampf(tailx, xx + 2, xx + w - 3);
  px(tx, yy + h, WHITE); px(tx, yy + h + 1, WHITE); px(tx + (tailx < cx ? -1 : 1), yy + h + 2, WHITE);
  text(a, xx + 4, yy + 3, EYE); if (*b) text(b, xx + 4, yy + 13, EYE);
}

struct HelperEv { char name[13], id[9]; uint8_t look, model, side; char line[64]; };
static HelperEv visitQ[4], visit; static int visitN = 0;
static bool visiting = false, visitHeart = false; static float visitT0 = 0, visitTalk = 0, visitOut = 0;
static int tossQ[4], tossN = 0, tossSide = -1; static float tossT0 = -10;
static char prevWorking[4][9]; static int prevWorkingN = 0;
static char seenDone[8][24]; static int seenDoneI = 0;
static float noiseL = 0, noiseR = 0, demoT0 = -1000;
static char hudHelper[96] = "";

static void helpers_update(const Helper *hs, int n, float t, float dt) {
  bool wantL = false, wantR = false; hudHelper[0] = 0;
  char nowWorking[4][9]; int nw = 0;
  for (int i = 0; i < n; i++) {
    const Helper &h = hs[i];
    if (h.state == 0) {
      (h.side ? wantR : wantL) = true;
      if (nw < 4) strlcpy(nowWorking[nw++], h.id, 9);
      bool known = false; for (int j = 0; j < prevWorkingN; j++) if (!strcmp(prevWorking[j], h.id)) known = true;
      if (!known && tossN < 4) tossQ[tossN++] = h.side;
      if (!hudHelper[0]) snprintf(hudHelper, sizeof hudHelper, "%s IS ON: %s", h.name, *h.line ? h.line : "SOMETHING");
    } else {
      char key[24]; snprintf(key, sizeof key, "%s%lu", h.id, (unsigned long)h.at);
      bool seen = false; for (int j = 0; j < 8; j++) if (!strcmp(seenDone[j], key)) seen = true;
      if (!seen) {
        strlcpy(seenDone[seenDoneI], key, sizeof seenDone[0]); seenDoneI = (seenDoneI + 1) % 8;
        if (visitN < 4) { HelperEv &e = visitQ[visitN++]; strlcpy(e.name, h.name, sizeof e.name); strlcpy(e.id, h.id, sizeof e.id);
          e.look = lookFor(h.id); e.model = h.model; e.side = h.side; strlcpy(e.line, h.line, sizeof e.line);
          Serial.printf("[helper] %s back: %s\n", h.name, h.line); }
      }
    }
  }
  memcpy(prevWorking, nowWorking, sizeof nowWorking); prevWorkingN = nw;
  const float r = dt * 2;
  noiseL += clampf((wantL ? 1.f : 0.f) - noiseL, -r, r); noiseR += clampf((wantR ? 1.f : 0.f) - noiseR, -r, r);
  if (tossSide >= 0 && t - tossT0 > 1.4f) tossSide = -1;
  if (tossSide < 0 && tossN) { tossSide = tossQ[0]; memmove(tossQ, tossQ + 1, sizeof(int) * (--tossN)); tossT0 = t; }
  if (visiting && t > visitOut + .6f) visiting = false;
  if (!visiting && visitN) {
    visit = visitQ[0]; memmove(visitQ, visitQ + 1, sizeof(HelperEv) * (--visitN));
    visiting = true; visitHeart = false; visitT0 = t; visitTalk = t + .6f;
    visitOut = visitTalk + .45f + strlen(visit.line) * .05f + 1.6f + .8f;
  }
}

// The scripted demo (serial `helpers`): two BUBBLES helpers, Sonnet- and Opus-sized, take work and report back.
static int demoHelpers(float e, Helper *out) {
  int n = 0; const uint32_t base = (uint32_t)(demoT0 * 10);
  auto add = [&](const char *id, const char *name, uint8_t model, uint8_t side, uint8_t state, const char *line, uint32_t at) {
    Helper &h = out[n++]; strlcpy(h.id, id, 9); strlcpy(h.name, name, 13); h.model = model; h.side = side; h.state = state; strlcpy(h.line, line, 64); h.at = at; };
  // Every demo helper is a BUBBLES too: a Sonnet-sized one on the left, an Opus-sized one on the right.
  if (e >= .5f && e < 11) add("demo-a", "BUBBLES", 1, 0, 0, "SHARE THE MENU", base);
  if (e >= 2.5f && e < 15) add("demo-b", "BUBBLES", 2, 1, 0, "FILL THE GROCERY CART", base + 1);
  if (e >= 11 && e < 26) add("demo-a", "BUBBLES", 1, 0, 1, "SENT! EVERYONE'S IN", base + 2);
  if (e >= 15 && e < 26) add("demo-b", "BUBBLES", 2, 1, 1, "CART READY - 18 ITEMS", base + 3);
  return n;
}

static void helpers_feed(float t, float dt) {
  Helper demo[4];
  if (ST.src == SRC_DEMO && t - demoT0 > 34) demoT0 = t;   // the demo replays the helpers on a loop
  if (t - demoT0 < 26) helpers_update(demo, demoHelpers(t - demoT0, demo), t, dt);
  else if (ST.src == SRC_LIVE && LS.have) helpers_update(LS.helpers, LS.nHelpers, t, dt);
  else helpers_update(nullptr, 0, t, dt);
}

// Split s into two lines of at most n chars at a space; the rest is dropped.
static void wrap2(const char *s, char *a, char *b, int n) {
  int len = strlen(s); if (len <= n) { strlcpy(a, s, n + 1); *b = 0; return; }
  int cut = n; while (cut > 0 && s[cut] != ' ') cut--; if (cut == 0) cut = n;
  memcpy(a, s, cut); a[cut] = 0; const char *r = s + cut; while (*r == ' ') r++; strlcpy(b, r, n + 1);
}

static void frame(float t, float dt) {
  const Mode m = ST.mode; const float f = fminf(2.5f, dt * 60), p = clampf((t - ST.changeAt) / .7f, 0, 1);
  const Pal &pal = P[m], &old = P[ST.prev], &cp = p > .5f ? pal : old;
  if (m != lastMode) { lastMode = m; lastTool = -1; memset(L, 0, N); for (int i = 0; i < N; i++) { VX[i] += (frand() - .5f) * 3; VY[i] += (frand() - .5f) * 3; } }
  const bool live = ST.src == SRC_LIVE && LS.have;
  int ti = 0; if (m == WORK) { ti = live ? (int)LS.icon : toolIdx(t); if (ti != lastTool) { lastTool = ti; toolAt = t; memset(L, 0, N); } }
  const Tool &tl = TOOLS[live ? 0 : ti];
  const int icon = live ? (int)LS.icon : tl.icon;

  // touch: rubbing or tapping the character tickles him, holding still pets him,
  // touching anywhere else draws his eyes and the motes toward your finger.
  bool onBody = false;
  if (TP.down) {
    float ddx = TP.x - bx, ddy = TP.y - (by - 8); onBody = ddx * ddx + ddy * ddy < 54 * 54;
    if (!TP.was && onBody) tickle = fminf(1, tickle + .45f);
    if (TP.was) rub = rub * .8f + hypotf(TP.x - TP.px, TP.y - TP.py);
    if (onBody) {
      if (rub > 2.5f) tickle = fminf(1, tickle + .1f * f);
      else {   // a still finger: the tap's giggle fades and petting takes over
        tickle = fmaxf(0, tickle - dt * .9f);
        if (t - TP.downAt > .35f) pet = fminf(1, pet + dt * 2);
      }
    }
  } else rub = 0;
  if (!(TP.down && onBody)) { tickle = fmaxf(0, tickle - dt * .7f); pet = fmaxf(0, pet - dt * 1.5f); }
  TP.px = TP.x; TP.py = TP.y; TP.was = TP.down;
  const bool giggle = tickle > .08f, petting = !giggle && pet > .1f, aware = TP.down || t - TP.last < 1.5f;
  const bool vListen = visiting && t < visitOut - .8f, vHappy = visiting && !vListen; const int vSide = visiting ? (visit.side ? 1 : -1) : 0;

  // background: banded dither gradient, crossfaded on state change. The band dither
  // repeats every 4 pixels and the crossfade mask every 8, so each row is one 8-pixel
  // pattern filled two words at a time. (Filling per pixel during the crossfade cost
  // ~8 ms and dropped every frame of a state change.)
  const int pthr = (int)(p * 16);
  for (int y = 0; y < SKY; y++) { int b = rowB[y], fr = rowF[y]; uint32_t w[2] = {0, 0};
    for (int x = 0; x < 8; x++) { const uint8_t *s = bay(x >> 1, y >> 1) < pthr ? pal.sky : old.sky;
      w[x >> 2] |= (uint32_t)(fr > bay(x, y) ? s[b + 1] : s[b]) << (8 * (x & 3)); }
    uint32_t *o = (uint32_t *)fb[y]; for (int x = 0; x < W / 4; x += 2) { o[x] = w[0]; o[x + 1] = w[1]; } }
  if (m == SLEEP) for (auto &s : stars) if (wsin(t * 1.6f + s.p) > -.2f) px(s.x, s.y, STAR);
  for (int k = 0; k < 4; k++) { auto &c = puffs[k]; c.x -= c.v * dt; if (c.x < -50) c.x = W + 20; puff(k, c.x, c.y, cp.puff); }
  streakBand(0, noiseL, t, cp); streakBand(1, noiseR, t, cp);

  // body motion
  float tx = 120, ty = 166, sq = 1;
  if (m == SLEEP) { ty = 180; sq = .9f; } else if (m == WORK) { tx = 112; ty = 204; } else if (m == ASK) { tx = 100; ty = 176; } else if (m == ERR) { tx = 120 + wsin(t * .8f) * 2; ty = 172; }
  float e = fminf(1, dt * 3); bx += (tx - bx) * e; by += (ty - by) * e;
  float hop = 0, rx = 32, ry = 37;
  float br = m == SLEEP ? wsin(t * .7f) * .045f : wsin(t * 1.6f) * .025f;
  ry *= (1 + br) * sq; rx *= (1 - br * .6f) * (2 - sq);
  if (m == ASK) { float ph = fabsf(wsin(t * 4.5f)); hop = ph * 12; if (ph < .25f) { rx *= 1.06f; ry *= .94f; } }
  if (vHappy) hop = fabsf(wsin((t - (visitOut - .8f)) * 6)) * 5;
  if (giggle) { float w = wsin(t * 24) * tickle; ry *= 1 + w * .07f; rx *= 1 - w * .05f; }
  if (petting) ry *= 1 - .04f * pet;
  rx = roundf(rx * 2) / 2; ry = roundf(ry * 2) / 2;
  const float bxSave = bx;
  bx = rnd(bx + (giggle ? wsin(t * 30) * 2.2f * tickle : 0) + (petting ? clampf((TP.x - bx) * .08f, -4, 4) * pet : 0));
  const float cy = rnd(by - hop), footY = rnd(by) + ry * .88f;

  oval(bx, by + 37 * .88f + 4, 24 - hop * .7f, 4, cp.floor, -1, true);

  // motes
  const float hx = bx + rx + 4, hy = cy + 2, dd84 = powf(.84f, f), dd76 = powf(.76f, f), dd80 = powf(.8f, f);
  const float qx0 = rnd(bx + 64 - 10), qy0 = rnd(cy - 42 + wsin(t * 3) * 3 - 14);
  for (int i = 0; i < N; i++) {
    float amt = 1; uint8_t front = 1; const float ox = X[i], oy = Y[i]; bool reset = false;
    if (m == SLEEP) {
      VY[i] = -.12f - U[i] * .1f; VX[i] = wsin(t * .8f + K[i] * 30) * .15f; X[i] += VX[i] * f; Y[i] += VY[i] * f;
      if (Y[i] < -2) { Y[i] = SKY - 4; X[i] = frand() * W; reset = true; }
      amt = wsin(t * 1.3f + K[i] * 40) > .3f ? .9f : 0; front = 0; if (i > 70) amt = 0;
    } else if (m == ERR) {
      VY[i] = fminf(.55f, VY[i] + .02f * f); VX[i] = wsin(t * 2 + K[i] * 10) * .45f; X[i] += VX[i] * f; Y[i] += VY[i] * f;
      if (Y[i] > footY + 4) { Y[i] = cy - ry - 20 - U[i] * 40; X[i] = bx + (K[i] - .5f) * 150; VY[i] = 0; reset = true; }
      amt = i < 90 ? .8f : 0;
    } else {
      float gx, gy, k = .06f, d = .84f;
      if (m == THINK) { float a = A[i] + t * (.7f + .3f * U[i]);
        gx = bx + wcos(a) * (42 + U[i] * 9); gy = cy - ry - 2 + wsin(a) * (10 + U[i] * 3); front = wsin(a) > 0; if (i > 150) amt = 0; }
      else if (m == WORK) {
        targetGrid(iconCells[icon], iconN[icon], i, 4, 120 - 24, 62 - 24, gx, gy); k = .1f; d = .76f;
        if (t < toolAt + U[i] * .8f) { X[i] = hx; Y[i] = hy; VX[i] = VY[i] = 0; amt = 0; }
        else if (!L[i]) { L[i] = 1; VY[i] = -3 - U[i] * 2; VX[i] = 1 + U[i]; }
        gx += wsin(t * 3 + i) * .4f; gy += wcos(t * 2.6f + i) * .4f;
      } else { targetGrid(qCells, qN, i, 4, qx0, qy0, gx, gy); k = .08f; d = .8f; }
      if (amt > 0) { float dd = d == .84f ? dd84 : d == .76f ? dd76 : dd80; VX[i] = (VX[i] + (gx - X[i]) * k * f) * dd; VY[i] = (VY[i] + (gy - Y[i]) * k * f) * dd; X[i] += VX[i] * f; Y[i] += VY[i] * f; }
    }
    if (TP.down && !onBody) { float dx = TP.x - X[i], dy = TP.y - Y[i], dd = sqrtf(dx * dx + dy * dy);
      if (dd < 64 && dd > .5f) { float w = (1 - dd / 64) * f; X[i] += (dx * .05f - dy * .04f) * w; Y[i] += (dy * .05f + dx * .04f) * w; } }
    FR[i] = front; VIS[i] = amt > 0;
    if (reset || !VIS[i]) { TRX[i][0] = TRX[i][1] = rnd(X[i]); TRY[i][0] = TRY[i][1] = rnd(Y[i]); }
    else { TRX[i][1] = TRX[i][0]; TRY[i][1] = TRY[i][0]; TRX[i][0] = rnd(ox); TRY[i][0] = rnd(oy); }
  }
  if (m == WORK) {   // faint crisp icon underneath so it reads while motes land
    for (int c = 0; c < iconN[icon]; c++) rect(120 - 24 + iconCells[icon][c][0] * 4, 62 - 24 + iconCells[icon][c][1] * 4, 4, 4, cp.mote2);
  }
  drawMotes(0, cp);

  // feet + arms (behind the body)
  const float R = rx / 32;
  oval(bx - 12 * R, footY, 6, 3.5f, cp.shade, cp.rim); oval(bx + 12 * R, footY, 6, 3.5f, cp.shade, cp.rim);
  float lax = bx - rx + 1, lay = cy + 10, rax = bx + rx - 1, ray = cy + 10;
  if (m == WORK) { rax = bx + rx + 2; ray = cy + 2 + wsin(t * 9) * 1.2f; }
  if (m == ASK) { rax = bx + rx + 5; ray = cy - 14 + wsin(t * 9) * 4; }
  if (m == ERR || m == SLEEP) { lax = bx - rx + 3; lay = cy + 16; rax = bx + rx - 3; ray = cy + 16; }
  if (giggle) { lax = bx - rx - 1; lay = cy - 4 + wsin(t * 20) * 3; rax = bx + rx + 1; ray = cy - 4 - wsin(t * 20) * 3; }
  else if (petting) { lax = bx - rx + 3; lay = cy + 14; rax = bx + rx - 3; ray = cy + 14; }
  const bool tossing = tossSide >= 0 && t - tossT0 < 1.0f;
  if (tossing) { float up = wsin(seg(t, tossT0, tossT0 + 1) * 3.14159f);
    if (tossSide == 0) { lax = bx - 33 - 6 * up; lay = cy + 10 - 28 * up; } else { rax = bx + 33 + 6 * up; ray = cy + 10 - 28 * up; } }
  oval(lax, lay, 5.5f, 4.5f, cp.body, cp.rim); oval(rax, ray, 5.5f, 4.5f, cp.body, cp.rim);

  // body: lit oval, dithered rim, shimmering pixel-cloud edge
  const int tq = (int)floorf(t * 3);
  for (int y = (int)(cy - ry - 5); y <= cy + ry + 5; y++) for (int x = (int)(bx - rx - 5); x <= bx + rx + 5; x++) {
    float dx = x - bx, dy = y - cy, ryy = dy > 0 ? ry * .88f : ry, qx = dx / rx, qy = dy / ryy, d = sqrtf(qx * qx + qy * qy);
    if (d > 1.2f) continue;
    float n = hash3(x >> 1, y >> 1, tq);
    if (d > 1 + (n - .5f) * .07f) { if (d < 1.14f && n > .93f && dy < ryy * .5f) px(x, y, cp.mote); continue; }
    float th = bay(x, y) / 16.0f; uint8_t col;
    if (d > .9f && (d - .9f) * 10 > th) col = cp.rim;
    else { float v = -qx * .45f - qy * .8f; col = v > .42f + th * .22f ? cp.hl : v < -.18f - th * .35f ? cp.shade : cp.body; }
    px(x, y, col);
  }
  if (giggle || tossing || m == ASK || m == WORK) { oval(rax, ray, 5.5f, 4.5f, cp.body, cp.rim); if (giggle || tossing) oval(lax, lay, 5.5f, 4.5f, cp.body, cp.rim); }

  // antenna
  float sway = wsin(t * 1.5f) * 2 + wsin(t * 20) * 4 * tickle, abx = bx + 5, aby = cy - ry + 1, atx = bx + 11 + sway, aty = cy - ry - 14;
  float mxa = (abx + atx) / 2 + 2, mya = (aby + aty) / 2;
  line(abx, aby, mxa, mya, cp.rim); line(mxa, mya, atx, aty, cp.rim);
  bool on = true;
  if (m == THINK) on = wsin(t * 3) > -.2f; else if (m == WORK) on = fmodf(t * 4, 1) < .55f; else if (m == ERR) on = fmodf(t, 1.2f) < .6f;
  uint8_t tip = m == SLEEP ? cp.mote2 : on ? cp.ant : cp.rim;
  if (on && m != SLEEP) oval(atx, aty, 6, 6, cp.ant, -1, true);
  oval(atx, aty, 3.2f, 3.2f, tip); pxf(atx - 1, aty - 1, WHITE);
  if (m == ASK) for (int k = 0; k < 2; k++) { float r = fmodf(t * 18 + k * 12, 24) + 6;
    for (float a = 0; a < 6.283f; a += 1.4f / r) if (((int)(a * r)) & 1) pxf(atx + wcos(a) * r, aty + wsin(a) * r, cp.ant); }

  // face
  const float ex1 = bx - 11 * R, ex2 = bx + 11 * R, ey = cy - 4;
  static const int8_t LOOK[5][2] = {{0, 0}, {2, -3}, {3, -1}, {0, 0}, {0, 1}};
  const bool blink = fmodf(t, 4.6f) < .14f && m != SLEEP && m != ERR;
  for (int side = -1; side <= 1; side += 2) {
    float ex = side < 0 ? ex1 : ex2; int x = rnd(ex + LOOK[m][0]), y = rnd(ey + LOOK[m][1]);
    if (aware && !giggle && !petting) { x = rnd(ex + clampf((TP.x - ex) / 22, -3, 3)); y = rnd(ey + clampf((TP.y - ey) / 22, -3, 3)); }
    if (vListen && !TP.down) { x = rnd(ex + 3 * vSide); y = rnd(ey - 2); }
    if (giggle) { for (int k = 0; k < 3; k++) { int ox = -side * (2 - k); px(rnd(ex) + ox, rnd(ey) - 2 + k, EYE); px(rnd(ex) + ox, rnd(ey) + 4 - k, EYE);
                                                 px(rnd(ex) + ox + side, rnd(ey) - 2 + k, EYE); px(rnd(ex) + ox + side, rnd(ey) + 4 - k, EYE); } }
    else if (petting || vHappy) { for (int k = -3; k <= 3; k++) pxf(ex + k, ey + 2 - rnd((1 - (k / 3.f) * (k / 3.f)) * 2), EYE); }
    else if (m == SLEEP && !aware && !vListen) { for (int k = -3; k <= 3; k++) pxf(ex + k, ey + 1 + rnd((1 - (k / 3.f) * (k / 3.f)) * 1.6f), EYE); }
    else if (blink) rect(ex - 3, ey + 1, 7, 1, EYE);
    else {
      bool big = m == ASK || vListen; oval(x, y, big ? 4 : 3.5f, big ? 5.8f : m == ERR ? 4.2f : 5, EYE);
      rect(x - 2, y - 3, 2, 2, WHITE); px(x + 1, y + 2, WHITE);
      if (m == ERR) line(ex - 4 * side, ey - 10, ex + 3 * side, ey - 8, EYE);
    }
    for (int k = 0; k < 6; k++) for (int j = 0; j < 2; j++) if (giggle || petting || vHappy || ((k + j) & 1)) pxf(ex - 3 + k + side * 2, ey + 8 + j, cp.cheek);
  }
  const int mx = rnd(bx), my = rnd(cy + 9);
  auto pts = [&](std::initializer_list<std::pair<int, int>> l) { for (auto &q : l) px(mx + q.first, my + q.second, EYE); };
  if (giggle) { oval(mx, my + 1, 6, 4.4f, MOUTH); rect(mx - 6, my - 3, 13, 3, cp.body); rect(mx - 2, my + 2, 5, 2, TONGUE); }
  else if (vHappy) { oval(mx, my + 1, 5, 3.6f, MOUTH); rect(mx - 5, my - 2, 11, 2, cp.body); rect(mx - 2, my + 2, 4, 2, TONGUE); }
  else if (petting) pts({{-3, 0}, {-2, 1}, {-1, 1}, {0, 1}, {1, 1}, {2, 1}, {3, 0}});
  else if (vListen) oval(mx, my + 1, 1.5f, 1.5f, EYE);
  else if (m == ASK) { oval(mx, my + 1, 5, 3.6f, MOUTH); rect(mx - 5, my - 2, 11, 2, cp.body); rect(mx - 2, my + 2, 4, 2, TONGUE); }
  else if (m == WORK) { pts({{-3, 0}, {-2, 1}, {-1, 1}, {0, 1}, {1, 1}, {2, 0}}); rect(mx + 1, my + 2, 2, 2, TONGUE); }
  else if (m == THINK) pts({{2, 0}, {3, 0}, {1, 1}, {4, 1}, {2, 2}, {3, 2}});
  else if (m == ERR) pts({{-4, 1}, {-3, 0}, {-2, 0}, {-1, 1}, {0, 1}, {1, 0}, {2, 0}, {3, 1}});
  else { float o = wsin(t * .7f) > 0 ? 2 : 1; oval(mx, my + 1, o, o, EYE); }
  if (m == ERR) { float s = fmodf(t * 7, 16), sx = bx + rx * .62f, sy = cy - ry * .55f + s;
    pxf(sx, sy, SWEAT); rect(sx - 1, sy + 1, 3, 2, SWEAT); rect(sx, sy + 3, 1, 1, SWEAT); pxf(sx - 1, sy + 1, WHITE); }

  drawMotes(1, cp);

  if (tossing) { float q = seg(t, tossT0 + .35f, tossT0 + 1.1f); int sd = tossSide ? 1 : -1;
    if (q > 0 && q < 1) { float sx = bx + sd * 40, sy = cy - 16, ex = sd < 0 ? -12.f : W + 12.f, ey2 = cy - 40;
      drawCard(sx + (ex - sx) * q, sy + (ey2 - sy) * q - wsin(q * 3.14159f) * 34); } }
  if (visiting) {
    const HLook &a = LOOKS[visit.look]; const float k = modelScale(visit.model), arx = a.rx * k, ary = a.ry * k;
    const float inK = backOut(seg(t, visitT0, visitT0 + .6f)), outK = easeOut(seg(t, visitOut, visitOut + .6f));
    const float edge = vSide < 0 ? -arx - 8 : W + arx + 8, perch = bx + vSide * (35 + arx);
    const float hx = edge + (perch - edge) * inK + (edge - perch) * outK, hy = cy - 30 + ary * .4f;
    drawHelper(a, k, hx, hy, t, -vSide, t > visitTalk && t < visitOut - .4f, cp);
    if (t > visitTalk && t < visitOut) drawBubble(visit.line, t - visitTalk, bx + vSide * 8, hy - ary - 24, hx - vSide * arx * .3f);
    if (vHappy && !visitHeart) { visitHeart = true;
      for (auto &q : pops) if (!q.on) { q = {bx - 2, cy - ry - 6, 0, 0, true, true}; break; } }
  }

  if ((giggle || petting) && t > nextHeart) {
    for (auto &q : pops) if (!q.on) { q = {bx + (frand() - .5f) * 44, cy - ry * .5f, 0, (frand() - .5f) * 10, true, true}; break; }
    nextHeart = t + (giggle ? .5f / (.3f + tickle) : .45f);
  }
  if (giggle && tickle > .4f && t > nextHehe) {
    for (auto &q : pops) if (!q.on) { q = {bx + (frand() < .5f ? -60.f : 30.f), cy - ry - 6, 0, 0, true, false}; break; }
    nextHehe = t + .9f;
  }
  for (auto &q : pops) { if (!q.on) continue; q.a += dt; q.y -= (q.heart ? 22 : 14) * dt; q.x += q.vx * dt + wsin(q.a * 5) * .3f;
    if (q.a > (q.heart ? 1.6f : 1.1f)) { q.on = false; continue; }
    if (q.heart) { static const uint8_t HT[5] = {10, 31, 31, 14, 4};
      for (int r = 0; r < 5; r++) for (int c = 0; c < 5; c++) if (HT[r] & (16 >> c)) px(rnd(q.x) + c, rnd(q.y) + r, TONGUE); }
    else text("HEHE", q.x, q.y, cp.hl); }

  if (m == SLEEP && t > nextZ) { for (auto &z : zs) if (!z.on) { z = {atx + 4, aty - 4, 0, true}; break; } nextZ = t + 1.6f; }
  for (auto &z : zs) { if (!z.on) continue; z.a += dt; z.y -= 9 * dt; z.x += wsin(z.a * 2) * .3f;
    if (z.a > 4 || m != SLEEP) { z.on = false; continue; } text("Z", z.x, z.y, cp.hl, z.a < 1.6f ? 1 : 2); }
  if (m == WORK && !live) { text(tl.verb, 120 - textW(tl.verb) / 2, 96, cp.ink); text(tl.obj, 120 - textW(tl.obj) / 2, 108, cp.mote2); }
  if (m == WORK && live) {   // issue title wrapped to two centred lines, detail beneath
    char l1[40] = "", l2[40] = ""; wrap2(LS.title, l1, l2, 38);
    text(l1, 120 - textW(l1) / 2, 96, cp.ink); if (*l2) text(l2, 120 - textW(l2) / 2, 106, cp.ink);
    char d[40]; strlcpy(d, LS.detail, sizeof d); if (*d) text(d, 120 - textW(d) / 2, *l2 ? 118 : 108, cp.mote2);
  }

  // status strip
  rect(0, SKY, W, H - SKY, cp.hud);
  const char *who = ST.src == SRC_DEMO ? "BUBBLES" : g_agentName;
  text(who, 8, SKY + 5, cp.ink);
  const int chipX = 8 + textW(who) + 7, cw = textW(LABEL[m]) + 8; rect(chipX, SKY + 3, cw, 11, pal.chip); text(LABEL[m], chipX + 4, SKY + 5, pal.hud);
  const bool offline = ST.src == SRC_LIVE && (!LS.have || !LS.ok || millis() - LS.lastGoodMs > OFFLINE_MS);
  if (offline) text("OFFLINE", W - 8 - textW("OFFLINE"), SKY + 5, cp.mote2);
  else {
    int secs = live ? (int)(LS.now - LS.since) + (int)((millis() - lsFetchMs) / 1000) : (int)floorf(ELAPSED_BASE[m] + t - ST.since);
    if (secs < 0) secs = 0;
    char el[12]; if (secs >= 3600) snprintf(el, sizeof el, "%dH%02d", secs / 3600, secs / 60 % 60); else snprintf(el, sizeof el, "%02d:%02d", secs / 60, secs % 60);
    text(el, W - 8 - textW(el), SKY + 5, cp.ink);
  }
  char line2[160];
  if (live) {
    if (m == SLEEP && hudHelper[0]) snprintf(line2, sizeof line2, "%s", hudHelper);
    else if (m == SLEEP) { if (LS.waiting) snprintf(line2, sizeof line2, "%u WAITING ON YOU", LS.waiting);
                      else if (*LS.next) snprintf(line2, sizeof line2, "ALL CAUGHT UP - NEXT CHECK %s", LS.next);
                      else snprintf(line2, sizeof line2, "ALL CAUGHT UP"); }
    else if (m == WORK && *LS.detail) snprintf(line2, sizeof line2, "%s - %s", LS.title, LS.detail);
    else if (m == ERR) snprintf(line2, sizeof line2, "STUCK ON: %s", *LS.title ? LS.title : "LAST RUN");
    else snprintf(line2, sizeof line2, "%s", *LS.title ? LS.title : LABEL[m]);
  } else if (ST.src == SRC_LIVE) snprintf(line2, sizeof line2, "CONNECTING TO BUBBLE-SCREEN...");
  else if (m == WORK) snprintf(line2, sizeof line2, "%s - %s: %s", TASK[WORK], tl.verb, tl.obj); else snprintf(line2, sizeof line2, "%s", TASK[m]);
  ticker(line2, 8, SKY + 18, W - 16, cp.ink, t);
  bx = bxSave;
}

// ------------------------------------------------------------ main -------
#ifndef BUBBLE_HOST
// Two framebuffers: core 1 (loop) draws the next frame while core 0 streams
// the previous one to the panel.
static Preferences prefs;
static char cmdbuf[160]; static int cmdn = 0;
// Seconds since an epoch that is moved forward every ~5.5 h, so the float clock
// keeps ~2 ms resolution forever (micros() would wrap every 71.6 min, and a raw
// float second count turns animation jerky after a day or two).
static const int64_t REBASE_AT_US = 20000LL * 1000000, REBASE_BY_US = 10000LL * 1000000;
static int64_t g_epochUs = 0;
static float statT = 0;
static void rebaseTimes(float d) {
  ST.since -= d; ST.changeAt -= d; toolAt -= d; nextZ -= d; nextHeart -= d; nextHehe -= d;
  TP.downAt -= d; TP.last -= d; statT -= d;
  tossT0 -= d; visitT0 -= d; visitTalk -= d; visitOut -= d; demoT0 -= d;
}
static float nowS() {
  int64_t us = esp_timer_get_time() - g_epochUs;
  if (us > REBASE_AT_US) { g_epochUs += REBASE_BY_US; us -= REBASE_BY_US; rebaseTimes(REBASE_BY_US / 1e6f);
    Serial.printf("[clock] rebased, t=%.1f\n", us / 1e6f); }
  return us / 1e6f;
}
static volatile bool orientPending = false;
static volatile int showIdx = 0;
static volatile uint32_t pushUsLast = 0;
static SemaphoreHandle_t semPush, semDone;

static void pushTask(void *) {
  for (;;) {
    xSemaphoreTake(semPush, portMAX_DELAY);
    if (orientPending) { orientPending = false; lcd_orient(); }
    uint32_t a = micros(); lcd_push(fbs[showIdx]); pushUsLast = micros() - a;
    xSemaphoreGive(semDone);
  }
}

static void handleCmd(const char *c, float t) {
  static const char *names[] = {"sleep", "think", "work", "ask", "error"};
  for (int i = 0; i < 5; i++) if (!strcmp(c, names[i])) { ST.src = SRC_PINNED; setMode((Mode)i, t); Serial.printf("[state] pinned %s\n", c); return; }
  if (!strcmp(c, "demo") || !strcmp(c, "auto")) { ST.src = SRC_DEMO; prefs.putString("mode", "demo"); Serial.println("[state] demo (saved)"); return; }
  if (!strncmp(c, "skew ", 5)) { g_epochUs -= (int64_t)atol(c + 5) * 1000000; Serial.printf("[clock] skewed +%lds\n", atol(c + 5)); return; }   // test the rebase
  if (!strcmp(c, "helpers")) { demoT0 = t; Serial.println("[helpers] demo"); return; }
  if (!strcmp(c, "live")) { ST.src = SRC_LIVE; prefs.putString("mode", "live"); Serial.println("[state] live (saved)"); return; }
  if (!strncmp(c, "set ", 4)) { char k[8] = ""; const char *v = strchr(c + 4, ' ');
    if (v && v - (c + 4) < (int)sizeof k) { memcpy(k, c + 4, v - (c + 4)); net_set(k, v + 1); Serial.printf("[cfg] %s saved\n", k); return; } }
  if (!strcmp(c, "invert")) { g_invert = !g_invert; prefs.putBool("invert", g_invert); orientPending = true; Serial.printf("[cfg] invert=%d\n", g_invert); return; }
  if (!strncmp(c, "panel ", 6)) { bool st = !strcmp(c + 6, "st7789");
    if (!st && strcmp(c + 6, "ili9341")) { Serial.println("[cfg] panel must be ili9341 or st7789"); return; }
    prefs.putBool("st7789", st); if (!prefs.isKey("invert")) prefs.putBool("invert", !st);
    Serial.printf("[cfg] panel=%s, rebooting\n", st ? "st7789" : "ili9341"); Serial.flush(); delay(50); ESP.restart(); }
  if (!strcmp(c, "flip")) { g_flip = !g_flip; prefs.putBool("flip", g_flip); orientPending = true; Serial.printf("[cfg] flip=%d\n", g_flip); return; }
  if (*c) Serial.printf("[cmd] unknown: %s\n", c);
}

void setup() {
  uint32_t t0 = millis();
  Serial.begin(115200);
  // Inversion is per PANEL (two "identical" boards can disagree): serial `invert` flips and saves it.
  prefs.begin("bubble", false);
  g_st7789 = prefs.getBool("st7789", false);
  g_invert = prefs.isKey("invert") ? prefs.getBool("invert", true) : !g_st7789;
  g_flip = prefs.getBool("flip", false);
  if (prefs.isKey("agent")) prefs.getString("agent", g_agentName, sizeof g_agentName);
  if (!*g_agentName) strlcpy(g_agentName, "BUBBLES", sizeof g_agentName);
  { char mode[8] = ""; if (prefs.isKey("mode")) prefs.getString("mode", mode, sizeof mode);
    // Demo when asked for, or when there's no service to talk to.
    ST.src = (!strcmp(mode, "demo") || !net_configured()) ? SRC_DEMO : SRC_LIVE; }

  net_reserve();   // bring the Wi-Fi driver up first, so it gets its buffers before the framebuffers take the heap
  for (int i = 0; i < 1; i++) for (int s0 = 0; s0 < H; s0 += STRIP) {
    uint8_t *strip = (uint8_t *)heap_caps_malloc(W * STRIP, MALLOC_CAP_8BIT);
    if (!strip) { Serial.printf("[boot] framebuffer alloc FAILED (free %u, largest block %u)\n", (unsigned)ESP.getFreeHeap(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)); while (true) delay(1000); }
    for (int y = 0; y < STRIP; y++) fbs[i][s0 + y] = strip + y * W; }
  memcpy(fbRows[1], fbRows[0], sizeof fbRows[0]);   // one framebuffer: Wi-Fi leaves no room for a second
  fb = fbs[0];
  palettes(); helper_palettes(); scene_init();
  lcd_init(); tp_init();
  float t = nowS(); setMode(THINK, t); ST.prev = THINK;
  frame(t, 0.016f); lcd_push(fb);
  gpio_set_level((gpio_num_t)PIN_BL, 1);
  Serial.printf("[boot] bubble-screen, first frame at %lums, panel=%s invert=%d flip=%d palette=%d heap=%u largest=%u\n",
                (unsigned long)(millis() - t0), g_st7789 ? "st7789" : "ili9341", g_invert, g_flip, npal, (unsigned)ESP.getFreeHeap(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  semPush = xSemaphoreCreateBinary(); semDone = xSemaphoreCreateBinary(); xSemaphoreGive(semDone);
  xTaskCreatePinnedToCore(pushTask, "push", 4096, nullptr, 5, nullptr, 0);
  net_begin();
  fb = fbs[1];
}

void loop() {
  static uint32_t frames = 0, renderUs = 0, pushUs = 0, nextUs = micros();
  // Fixed 30 fps: even steps read as smooth motion; a varying rate reads as jitter.
  while ((int32_t)(micros() - nextUs) < 0) { if ((int32_t)(nextUs - micros()) > 2000) vTaskDelay(1); }
  nextUs += 33333; if ((int32_t)(micros() - nextUs) > 0) nextUs = micros() + 33333;
  { int rx, ry, rz; bool d = tp_read(rx, ry, rz); float tn = nowS();
    if (d) { TP.x = tp_sx(rx); TP.y = tp_sy(ry); if (!TP.down) { TP.downAt = tn; TP.px = TP.x; TP.py = TP.y;
      Serial.printf("[touch] raw=%d,%d z=%d -> %.0f,%.0f\n", rx, ry, rz, TP.x, TP.y); } TP.last = tn; }
    TP.down = d; }
  while (Serial.available()) { char ch = Serial.read();
    if (ch == '\n' || ch == '\r') { cmdbuf[cmdn] = 0; handleCmd(cmdbuf, nowS()); cmdn = 0; } else if (cmdn < (int)sizeof cmdbuf - 1) cmdbuf[cmdn++] = ch; }
  float t = nowS(); const float dt = 1 / 30.f;
  if (ST.src == SRC_DEMO && t - ST.since > DUR[ST.mode]) setMode((Mode)((ST.mode + 1) % 5), t);
  if (ST.src == SRC_LIVE) { LiveStatus s; if (net_snapshot(s)) {
      if (s.lastGoodMs != LS.lastGoodMs) { LS = s; lsFetchMs = millis();
        if (*LS.agent && strcmp(LS.agent, g_agentName)) { strlcpy(g_agentName, LS.agent, sizeof g_agentName); prefs.putString("agent", g_agentName); } }
      if ((Mode)LS.state != ST.mode) setMode((Mode)LS.state, t); } }
  helpers_feed(t, dt);
  xSemaphoreTake(semDone, portMAX_DELAY);           // single framebuffer: wait until the last frame is out
  pushUs += pushUsLast;
  uint32_t a = micros(); frame(t, dt); renderUs += micros() - a;
  showIdx = (fb == fbs[0]) ? 0 : 1; xSemaphoreGive(semPush);
  fb = (fb == fbs[0]) ? fbs[1] : fbs[0];
  frames++;
  if (t - statT > 10) {
    Serial.printf("[perf] fps=%.1f render=%.1fms push=%.1fms state=%s\n", frames / (t - statT), renderUs / 1000.f / frames, pushUs / 1000.f / frames, LABEL[ST.mode]);
    statT = t; frames = renderUs = pushUs = 0;
  }
}
#endif  // BUBBLE_HOST
