// M5Dial Playground Demo
#include <Arduino.h>
#include <M5Unified.h>
#include <M5Dial.h>
#include <algorithm>
#include <cstdio>
#include <math.h>

static inline uint16_t rgb(uint8_t r, uint8_t g, uint8_t b)
{
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

static inline uint16_t dim_color(uint16_t c, float k)
{
  // k in [0,1]: 0 -> black, 1 -> original
  uint8_t r5 = (c >> 11) & 0x1F; uint8_t g6 = (c >> 5) & 0x3F; uint8_t b5 = c & 0x1F;
  uint8_t r8 = r5 << 3; uint8_t g8 = g6 << 2; uint8_t b8 = b5 << 3;
  r8 = (uint8_t)(r8 * k); g8 = (uint8_t)(g8 * k); b8 = (uint8_t)(b8 * k);
  return rgb(r8, g8, b8);
}

// All configurable constants live here
namespace Config
{
  // Rotary encoder / brightness
  static constexpr int BrightMax = 100;
  static constexpr int BrightStep = 10; // percent per detent
  static constexpr int EncDiv = 4;     // counts per logical step

  // Audio: click on rotation
  static constexpr uint16_t ClickUpFreq = 1800;   // increase
  static constexpr uint16_t ClickDownFreq = 1000; // decrease
  static constexpr uint16_t ClickMs = 40;         // length

  // Audio: confirm tones for theme/mute actions
  static constexpr uint16_t ConfirmToneUpFreq = 1800;
  static constexpr uint16_t ConfirmToneDownFreq = 1200;
  static constexpr uint16_t ConfirmTone1Ms = 70;
  static constexpr uint16_t ConfirmTone2Ms = 90;
  static constexpr uint16_t ConfirmToneGapMs = 80;

  // Audio: invert (long-press) two-tone "be-boop"
  static constexpr uint16_t InvertTone1Freq = 900;
  static constexpr uint16_t InvertTone2Freq = 600;
  static constexpr uint16_t InvertTone1Ms = 80;
  static constexpr uint16_t InvertTone2Ms = 100;
  static constexpr uint16_t InvertToneGapMs = 70;

  // Starburst effect configuration
  static constexpr int StarburstRays = 16;
  static constexpr int StarburstSteps = 12;     // frames to full length
  static constexpr int StarburstDelayMs = 14;   // frame delay (~70 FPS)
  // Starburst sound (quick two-tone at start)
  static constexpr uint16_t StarburstTone1Freq = 1500;
  static constexpr uint16_t StarburstTone2Freq = 2100;
  static constexpr uint16_t StarburstTone1Ms = 60;
  static constexpr uint16_t StarburstTone2Ms = 70;
  static constexpr uint16_t StarburstToneGapMs = 50;

  // Audio: tap pop effect
  static constexpr uint16_t TapPop1Freq = 1200;
  static constexpr uint16_t TapPop2Freq = 1800;
  static constexpr uint16_t TapPop1Ms = 50;
  static constexpr uint16_t TapPop2Ms = 60;
  static constexpr uint16_t TapPopGapMs = 60;
  static constexpr uint8_t SpeakerVolume = 180;

  // Touch / gestures
  static constexpr uint16_t TouchHoldThreshMs = 1000;
  static constexpr uint16_t TouchFlickThresh = 18;
  static constexpr uint16_t LongPressInvertMs = 1000;
  static constexpr uint16_t TapMaxReleaseMs = 250; // legacy (not used for gating now)
  static constexpr uint16_t TapMaxMovePx = 20;      // max move to consider it a tap, not a drag

  // Crosshair overlay
  static constexpr int CrosshairRadius = 12;

  // Target ping animation
  static constexpr int PingStep = 12;       // radius increment per frame
  static constexpr int PingIntervalMs = 16; // frame time (~60fps)

  // Ring scale
  static constexpr int RingTicks = 100;
  static constexpr float RingStartDeg = -90.0f; // top
  static constexpr float RingSweepDeg = 360.0f; // full circle
  static constexpr int TickLenMajor = 18;
  static constexpr int TickLenMinor = 12;
  static constexpr int TickMajorEvery = 10;
  // Margin from physical circular edge (adjust if you see clipping)
  static constexpr int EdgeMargin = 6;
  // Debug (default ON, disabled by defining RELEASE_BUILD)
#ifdef RELEASE_BUILD
  static constexpr bool DebugTouch = false;
  static constexpr bool DebugHeartbeat = false;
  static constexpr bool DebugBtn = false;
  static constexpr bool DebugRot = false;
  static constexpr bool DebugPing = false;
#else
  static constexpr bool DebugTouch = true;
  static constexpr bool DebugHeartbeat = false;
  static constexpr bool DebugBtn = true;
  static constexpr bool DebugRot = true;
  static constexpr bool DebugPing = true;
#endif
}

struct Theme
{
  const char *name;
  uint16_t bg;
  uint16_t primary;
  uint16_t accent;
  uint16_t text;
  uint16_t ripple;
};

static Theme THEMES[] = {
    // name     bg              primary         accent           text (complementary)     ripple
    {"Carbon", rgb(8, 8, 8),    rgb(0, 180, 255), rgb(255, 255, 255), rgb(255, 160, 60),    rgb(255, 220, 0)},
    {"Neon",   rgb(10, 6, 18),  rgb(0, 255, 170), rgb(255, 0, 180),  rgb(255, 120, 200),   rgb(255, 255, 0)},
    {"Ember",  rgb(24, 10, 32), rgb(255, 110, 0), rgb(255, 40, 80),  rgb(80, 160, 255),    rgb(255, 200, 0)},
    {"Ocean",  rgb(4, 10, 26),  rgb(0, 150, 255), rgb(220, 240, 255),rgb(255, 160, 60),    rgb(0, 220, 255)},
    {"Lime",   rgb(12, 18, 12), rgb(140, 255, 80),rgb(255, 255, 255),rgb(255, 120, 180),   rgb(170, 255, 120)},
    {"Sunset", rgb(20, 6, 28),  rgb(255, 200, 0), rgb(255, 60, 60),  rgb(180, 120, 255),   rgb(255, 220, 120)},
};

static int theme_idx = 0;
static bool mute = false;
static bool invert_latched = false;

static int brightness_pct = 80; // 0..100
static int last_ring_brightness = -1;
static int last_ring_theme = -1;

static int enc_accum = 0;
static int32_t enc_total = 0;
// Instant click feedback

// Sound config
static constexpr uint16_t CLICK_UP_FREQ = 2600;   // encoder increase
static constexpr uint16_t CLICK_DOWN_FREQ = 1700; // encoder decrease
static constexpr uint16_t CLICK_MS = 24;          // longer to avoid glitchy feel
static constexpr uint16_t CONF_TONE1_MS = 70;     // first tone length
static constexpr uint16_t CONF_TONE2_MS = 90;     // second tone length
static constexpr uint16_t CONF_TONE_GAP_MS = 80;  // gap between tones

static uint32_t tone2_at = 0;
static uint16_t tone2_freq = 0;
static uint16_t tone2_dur = 0;

static uint32_t press_start_ms = 0;
static int16_t press_x0 = 0, press_y0 = 0;
static bool ripple_active = false;
static int16_t ripple_x = 0, ripple_y = 0, ripple_r = 10, ripple_prev_r = 0;
static uint32_t ripple_redraw_at = 0;
static bool touch_active = false; static bool touch_dragged = false; static int16_t touch_last_x = 0, touch_last_y = 0;
// Ping overlay background snapshot
static uint16_t *ping_bk = nullptr;
static int ping_w = 0, ping_h = 0;
static int ping_x0 = 0, ping_y0 = 0;

// Crosshair pointer state
static int16_t cross_cx = 0, cross_cy = 0, cross_r = Config::CrosshairRadius;
static int16_t cross_prev_cx = 0, cross_prev_cy = 0;
static bool cross_initialized = false;
// Crosshair overlay background snapshot
static uint16_t *cross_bk = nullptr;
static int cross_w = 0, cross_h = 0;
static int cross_prev_x = 0, cross_prev_y = 0;

static int cx = 120, cy = 120;
static int ring_ro = 102, ring_ri = 80;
static int play_area_r = 64;

static void draw_status();
static void draw_ring(bool force = false);
static void draw_center_label();
static void draw_scene(bool force = true);
static void draw_crosshair_overlay(int16_t x, int16_t y, bool recapture_only = false);
static void play_pop();
static void play_confirm_up();
static void play_confirm_down();
static void play_invert();
static void effect_starburst();
static void play_invert();

void setup()
{
  Serial.begin(115200);
  unsigned long start = millis();
  while (!Serial && millis() - start < 1500)
  {
    delay(10);
  }
  Serial.println();
  Serial.println("[BOOT] M5Dial Playground starting...");

  auto cfg = M5.config();
  M5Dial.begin(cfg, true, true);
  M5Dial.Touch.setHoldThresh(Config::TouchHoldThreshMs);
  M5Dial.Touch.setFlickThresh(Config::TouchFlickThresh);

  cx = M5Dial.Display.width() / 2;
  cy = M5Dial.Display.height() / 2;
  // Place outer tick ends close to the visible circular edge
  ring_ro = std::min(cx, cy) - Config::EdgeMargin;              // outer radius for ring clear
  ring_ri = ring_ro - Config::TickLenMajor;                      // inner radius so major ticks end at ring_ro
  play_area_r = ring_ri - 16;                                    // unused for clamp, but keep reasonable
  cross_cx = cx;
  cross_cy = cy;
  cross_prev_cx = cross_cx;
  cross_prev_cy = cross_cy;
  cross_initialized = true;

  M5Dial.Display.fillScreen(THEMES[theme_idx].bg);
  draw_scene(true);

  M5Dial.Speaker.setVolume(Config::SpeakerVolume);
  if (!mute)
    M5Dial.Speaker.tone(2000, 200);

  M5Dial.Encoder.readAndReset();
}

void loop()
{
  M5.update();
  M5Dial.update();

  constexpr int BRIGHT_MAX = Config::BrightMax;
  constexpr int BRIGHT_STEP = Config::BrightStep; // percent per detent

  // Encoder → brightness
  int32_t d = M5Dial.Encoder.readAndReset();
  if (d)
    enc_accum += d;
  int logical = 0;
  while (enc_accum >= Config::EncDiv)
  {
    enc_accum -= Config::EncDiv;
    ++logical;
  }
  while (enc_accum <= -Config::EncDiv)
  {
    enc_accum += Config::EncDiv;
    --logical;
  }
  if (logical)
  {
    enc_total += logical;
    int prev_b = brightness_pct;
    brightness_pct += logical * BRIGHT_STEP;
    if (brightness_pct < 0)
      brightness_pct = 0;
    if (brightness_pct > BRIGHT_MAX)
      brightness_pct = BRIGHT_MAX;
    int mapped = (brightness_pct * 255) / BRIGHT_MAX;
    M5Dial.Display.setBrightness(mapped);
    // Immediate click sound: higher pitch when increasing, lower when decreasing
    if (!mute && brightness_pct != prev_b)
    {
      M5Dial.Speaker.tone((brightness_pct > prev_b) ? Config::ClickUpFreq : Config::ClickDownFreq, Config::ClickMs);
    }
    if (Config::DebugRot && brightness_pct != prev_b)
    {
      int delta = brightness_pct - prev_b;
      if (delta < 0) delta = -delta;
      Serial.printf("[ROT] %s%d%% -> br=%d%%\n", (brightness_pct > prev_b) ? "+" : "-", delta, brightness_pct);
    }
    draw_ring(false);
  }

  // Touch handling with robust state (works even if edge events are missed)
  int touch_count = M5Dial.Touch.getCount();
  if (touch_count > 0)
  {
    const auto &t = M5Dial.Touch.getDetail(0);
    if (!touch_active)
    {
      press_start_ms = millis(); press_x0 = t.x; press_y0 = t.y; touch_dragged = false; touch_active = true;
      if (Config::DebugTouch) Serial.printf("[TOUCH] PRESS x=%d y=%d\n", t.x, t.y);
    }
    touch_last_x = t.x; touch_last_y = t.y;
    int mdx0 = t.x - press_x0; int mdy0 = t.y - press_y0; uint32_t move02 = (uint32_t)(mdx0*mdx0 + mdy0*mdy0);
    if (!touch_dragged)
    {
      uint32_t maxmove2 = (uint32_t)Config::TapMaxMovePx * (uint32_t)Config::TapMaxMovePx;
      if (move02 > maxmove2)
      {
        touch_dragged = true; if (Config::DebugTouch) Serial.printf("[TOUCH] DRAG start d2=%lu\n", (unsigned long)move02);
      }
    }
    // Update center label (so X,Y updates live), then overlay crosshair on top
    draw_center_label();
    int txi = t.x; if (txi < 0) txi = 0; int maxx = M5Dial.Display.width() - 1; if (txi > maxx) txi = maxx;
    int tyi = t.y; if (tyi < 0) tyi = 0; int maxy = M5Dial.Display.height() - 1; if (tyi > maxy) tyi = maxy;
    draw_crosshair_overlay((int16_t)txi, (int16_t)tyi);
  }
  else if (touch_active)
  {
    // Treat as release even if `wasReleased()` edge was missed
    uint32_t dur = millis() - press_start_ms;
    int mdx = touch_last_x - press_x0; int mdy = touch_last_y - press_y0; uint32_t move2 = (uint32_t)(mdx*mdx + mdy*mdy);
    uint32_t maxmove2 = (uint32_t)Config::TapMaxMovePx * (uint32_t)Config::TapMaxMovePx;
    if (!touch_dragged && dur > Config::LongPressInvertMs)
    {
      invert_latched = !invert_latched; M5Dial.Display.invertDisplay(invert_latched);
      if (!mute) play_invert();
      if (Config::DebugTouch) Serial.printf("[TOUCH] RELEASE dur=%lu invert (no-drag)\n", (unsigned long)dur);
    }
    else if (!touch_dragged && move2 <= maxmove2)
    {
      ripple_x = touch_last_x; ripple_y = touch_last_y; ripple_prev_r = 0; ripple_r = 6; ripple_active = true; ripple_redraw_at = millis() + Config::PingIntervalMs; if (!mute) play_pop();
      if (Config::DebugTouch) Serial.printf("[TOUCH] RELEASE dur=%lu TAP -> ping\n", (unsigned long)dur);
    }
    else
    {
      draw_ring(true);
      if (Config::DebugTouch) Serial.printf("[TOUCH] RELEASE dur=%lu drag refresh\n", (unsigned long)dur);
    }
    touch_active = false;
  }

  // Target ping animation (expanding circle overlay with background restore)
  if (ripple_active && millis() >= ripple_redraw_at)
  {
    auto &t = THEMES[theme_idx];
    // Restore previous frame background
    if (ping_bk && ping_w > 0 && ping_h > 0)
    {
      M5Dial.Display.pushImage(ping_x0, ping_y0, ping_w, ping_h, ping_bk);
    }
    // Compute new bounding rect for this radius
    int r = ripple_r;
    int x0 = ripple_x - (r + 1);
    int y0 = ripple_y - (r + 1);
    int w = (r + 1) * 2 + 1;
    int h = (r + 1) * 2 + 1;
    if (x0 < 0)
    {
      w += x0;
      x0 = 0;
    }
    if (y0 < 0)
    {
      h += y0;
      y0 = 0;
    }
    if (x0 + w > M5Dial.Display.width())
      w = M5Dial.Display.width() - x0;
    if (y0 + h > M5Dial.Display.height())
      h = M5Dial.Display.height() - y0;
    // Allocate/resize buffer as needed and snapshot background
    size_t need = (size_t)w * (size_t)h;
    if (need > (size_t)ping_w * (size_t)ping_h)
    {
      free(ping_bk);
      ping_bk = (uint16_t *)malloc(need * sizeof(uint16_t));
    }
    ping_w = (w > 0 ? w : 0);
    ping_h = (h > 0 ? h : 0);
    ping_x0 = x0;
    ping_y0 = y0;
    if (ping_bk && ping_w > 0 && ping_h > 0)
    {
      M5Dial.Display.readRect(x0, y0, ping_w, ping_h, ping_bk);
    }
    // Draw current ping as a thicker outline using theme color (visible on dark bg)
    if (ping_w > 0 && ping_h > 0)
    {
      uint16_t outline = t.ripple;
      M5Dial.Display.drawCircle(ripple_x, ripple_y, r, outline);
      M5Dial.Display.drawCircle(ripple_x, ripple_y, r + 1, outline);
    }
    ripple_prev_r = ripple_r;
    ripple_r += Config::PingStep;
    ripple_redraw_at = millis() + Config::PingIntervalMs;
    int maxr = std::max(M5Dial.Display.width(), M5Dial.Display.height());
    if (ripple_r > maxr)
    {
      // Restore last frame and finish
      if (ping_bk && ping_w > 0 && ping_h > 0)
      {
        M5Dial.Display.pushImage(ping_x0, ping_y0, ping_w, ping_h, ping_bk);
      }
      free(ping_bk);
      ping_bk = nullptr;
      ping_w = ping_h = 0;
      ripple_active = false;
      ripple_prev_r = 0;
      // Force full redraw to restore ticks, center text, and crosshair overlay
      draw_ring(true);
      if (Config::DebugPing) Serial.println("[PING] end");
    }
  }

  // BtnA: press cycles theme immediately; hold triggers starburst effect
  if (M5Dial.BtnA.wasPressed())
  {
    theme_idx = (theme_idx + 1) % (int)(sizeof(THEMES) / sizeof(THEMES[0]));
    draw_scene(true);
    play_confirm_up();
    if (Config::DebugBtn) Serial.printf("[BTN] A press -> theme %d\n", theme_idx+1);
  }
  if (M5Dial.BtnA.wasHold())
  {
    if (Config::DebugBtn) Serial.println("[BTN] A hold -> starburst");
    effect_starburst();
  }

  // Timed secondary tones (confirmation sounds)
  uint32_t now = millis();
  if (tone2_at && now >= tone2_at)
  {
    if (!mute && tone2_freq)
      M5Dial.Speaker.tone(tone2_freq, tone2_dur);
    tone2_at = 0;
    tone2_freq = 0;
    tone2_dur = 0;
  }

  // Debug heartbeat
  static uint32_t last_dbg = 0;
  if (Config::DebugHeartbeat && millis() - last_dbg > 1000)
  {
    last_dbg = millis();
    Serial.printf("[DBG] br=%d%% theme=%d touch=%d drag=%d x=%d y=%d inv=%d\n",
                  brightness_pct, theme_idx,
                  (int)touch_active, (int)touch_dragged,
                  cross_cx, cross_cy, (int)invert_latched);
  }
}

static void draw_status()
{
  // No top status bar; using centered instructions instead.
  (void)THEMES;
  (void)brightness_pct;
  (void)invert_latched;
  (void)mute;
}

static void draw_center_label()
{
  auto &t = THEMES[theme_idx];
  M5Dial.Display.setTextDatum(m5gfx::textdatum_t::middle_center);
  // Theme info in primary color (smaller to avoid ring overlap)
  M5Dial.Display.setTextColor(t.primary, t.bg);
  M5Dial.Display.setTextSize(2);
  const int total = (int)(sizeof(THEMES) / sizeof(THEMES[0]));
  char line[64];
  snprintf(line, sizeof(line), "%s  (%d/%d)", t.name, theme_idx + 1, total);
  M5Dial.Display.drawString(line, cx, cy - 14);
  // Instructions and crosshair coordinates in text color
  M5Dial.Display.setTextSize(1);
  M5Dial.Display.setTextColor(t.text, t.bg);
  char instr1[48]; snprintf(instr1, sizeof(instr1), "Rotate: brightness (%d%%)", Config::BrightStep);
  M5Dial.Display.drawString(instr1, cx, cy + 0);
  M5Dial.Display.drawString("Tap: ping  BtnA: theme  Hold: burst", cx, cy + 12);
  M5Dial.Display.drawString("Long press: invert", cx, cy + 24);
  char pos[32]; snprintf(pos, sizeof(pos), "X:%d  Y:%d", cross_cx, cross_cy);
  M5Dial.Display.drawString(pos, cx, cy + 36);
}

static void draw_ring(bool force)
{
  if (!force && last_ring_brightness == brightness_pct && last_ring_theme == theme_idx)
    return;
  auto &t = THEMES[theme_idx];
  // Draw ticks from the outer physical edge inward (watch-face style)
  int outer_r = std::min(cx, cy) - 1; // very edge inside the circular mask
  int clear_r = outer_r + 2;
  M5Dial.Display.fillCircle(cx, cy, clear_r, t.bg);
  const int ticks = Config::RingTicks;
  const float start_deg = Config::RingStartDeg;
  const float sweep_deg = Config::RingSweepDeg;
  const int tick_len_major = Config::TickLenMajor;
  const int tick_len_minor = Config::TickLenMinor;
  const int major_every = Config::TickMajorEvery;
  uint16_t dim_unlit = dim_color(t.text, 0.35f);
  for (int i = 0; i < ticks; ++i)
  {
    float a = (start_deg + sweep_deg * (i / (float)ticks)) * (float)M_PI / 180.0f;
    int len = (i % major_every == 0) ? tick_len_major : tick_len_minor;
    int x1 = cx + (int)(cosf(a) * outer_r);
    int y1 = cy + (int)(sinf(a) * outer_r);
    int x0 = cx + (int)(cosf(a) * (outer_r - len));
    int y0 = cy + (int)(sinf(a) * (outer_r - len));
    M5Dial.Display.drawLine(x0, y0, x1, y1, dim_unlit);
  }
  int lit = (int)(ticks * (brightness_pct / 100.0f) + 0.5f);
  if (lit > ticks)
    lit = ticks;
  if (lit < 0)
    lit = 0;
  for (int i = 0; i < lit; ++i)
  {
    float a = (start_deg + sweep_deg * (i / (float)ticks)) * (float)M_PI / 180.0f;
    int len = (i % major_every == 0) ? tick_len_major : tick_len_minor;
    int x1 = cx + (int)(cosf(a) * outer_r);
    int y1 = cy + (int)(sinf(a) * outer_r);
    int x0 = cx + (int)(cosf(a) * (outer_r - len));
    int y0 = cy + (int)(sinf(a) * (outer_r - len));
    M5Dial.Display.drawLine(x0, y0, x1, y1, t.primary);
  }
  M5Dial.Display.setTextDatum(m5gfx::textdatum_t::middle_center);
  M5Dial.Display.setTextColor(t.primary, t.bg);
  M5Dial.Display.setTextSize(3);
  char buf[16];
  snprintf(buf, sizeof(buf), "%d%%", brightness_pct);
  M5Dial.Display.drawString(buf, cx, cy - 40);
  last_ring_brightness = brightness_pct;
  last_ring_theme = theme_idx;
  // Ensure center instructions remain visible
  draw_center_label();
  // Repaint crosshair overlay after background redraw
  if (cross_initialized)
    draw_crosshair_overlay(cross_cx, cross_cy, true);
}

static void draw_scene(bool force)
{
  auto &t = THEMES[theme_idx];
  M5Dial.Display.fillScreen(t.bg);
  draw_ring(true);
  draw_center_label();
  if (cross_initialized)
    draw_crosshair_overlay(cross_cx, cross_cy, true);
}

static void draw_crosshair_overlay(int16_t x, int16_t y, bool recapture_only)
{
  auto &t = THEMES[theme_idx];
  int r = cross_r;
  int w = r * 2 + 1;
  int h = r * 2 + 1;
  int x0 = x - r;
  int y0 = y - r;
  if (x0 < 0)
    x0 = 0;
  if (y0 < 0)
    y0 = 0;
  if (x0 + w > M5Dial.Display.width())
    w = M5Dial.Display.width() - x0;
  if (y0 + h > M5Dial.Display.height())
    h = M5Dial.Display.height() - y0;

  // Restore previous background if present and we're moving
  if (!recapture_only && cross_bk && cross_w > 0 && cross_h > 0)
  {
    M5Dial.Display.pushImage(cross_prev_x, cross_prev_y, cross_w, cross_h, cross_bk);
  }

  // (Re)allocate buffer if size changed; keep the largest needed
  size_t need = (size_t)w * (size_t)h;
  if (need > (size_t)cross_w * (size_t)cross_h)
  {
    free(cross_bk);
    cross_bk = (uint16_t *)malloc(need * sizeof(uint16_t));
  }
  cross_w = w;
  cross_h = h;
  cross_prev_x = x0;
  cross_prev_y = y0;
  if (cross_bk)
  {
    M5Dial.Display.readRect(x0, y0, w, h, cross_bk);
  }

  // Draw crosshair lines directly on display
  uint16_t c = t.accent;
  M5Dial.Display.drawLine(x - r, y, x + r, y, c);
  M5Dial.Display.drawLine(x, y - r, x, y + r, c);

  cross_cx = x;
  cross_cy = y;
}

static void play_pop()
{
  if (mute)
    return;
  M5Dial.Speaker.tone(Config::TapPop1Freq, Config::TapPop1Ms);
  tone2_at = millis() + Config::TapPopGapMs;
  tone2_freq = Config::TapPop2Freq;
  tone2_dur = Config::TapPop2Ms;
}

static void play_confirm_up()
{
  if (mute)
    return;
  // Ascending chirp: lower (down) then higher (up)
  M5Dial.Speaker.tone(Config::ConfirmToneDownFreq, Config::ConfirmTone1Ms);
  tone2_at = millis() + Config::ConfirmToneGapMs;
  tone2_freq = Config::ConfirmToneUpFreq;
  tone2_dur = Config::ConfirmTone2Ms;
}

static void play_confirm_down()
{
  if (mute)
    return;
  // Descending chirp: higher (up) then lower (down)
  M5Dial.Speaker.tone(Config::ConfirmToneUpFreq, Config::ConfirmTone1Ms);
  tone2_at = millis() + Config::ConfirmToneGapMs;
  tone2_freq = Config::ConfirmToneDownFreq;
  tone2_dur = Config::ConfirmTone2Ms;
}

// moved below with other effect functions

static void effect_starburst()
{
  auto &t = THEMES[theme_idx];
  int outer_r = std::min(cx, cy) - 1;
  const int rays = Config::StarburstRays;
  const int steps = Config::StarburstSteps;      // frames expanding
  const int delay_ms = Config::StarburstDelayMs; // frame cadence

  // Optional: small upbeat chirp
  if (!mute) {
    M5Dial.Speaker.tone(Config::StarburstTone1Freq, Config::StarburstTone1Ms);
    tone2_at = millis() + Config::StarburstToneGapMs;
    tone2_freq = Config::StarburstTone2Freq;
    tone2_dur = Config::StarburstTone2Ms;
  }
  if (Config::DebugBtn) Serial.println("[EFFECT] Starburst start");

  int prev_len = 0;
  for (int s = 1; s <= steps; ++s)
  {
    int len = (outer_r * s) / steps;
    // Draw current rays
    for (int i = 0; i < rays; ++i)
    {
      float a = (2.0f * (float)M_PI * i) / rays;
      int x1 = cx + (int)(cosf(a) * len);
      int y1 = cy + (int)(sinf(a) * len);
      uint16_t col = (i % 2 == 0) ? t.primary : t.accent;
      M5Dial.Display.drawLine(cx, cy, x1, y1, col);
    }
    // Erase previous rays
    if (prev_len > 0)
    {
      for (int i = 0; i < rays; ++i)
      {
        float a = (2.0f * (float)M_PI * i) / rays;
        int x1 = cx + (int)(cosf(a) * prev_len);
        int y1 = cy + (int)(sinf(a) * prev_len);
        M5Dial.Display.drawLine(cx, cy, x1, y1, t.bg);
      }
    }
    prev_len = len;
    M5.update(); M5Dial.update();
    delay(0); // feed watchdog
    delay(delay_ms);
  }

  // Retract quickly
  for (int s = steps; s >= 0; --s)
  {
    // Erase current rays at prev_len
    for (int i = 0; i < rays; ++i)
    {
      float a = (2.0f * (float)M_PI * i) / rays;
      int x1 = cx + (int)(cosf(a) * prev_len);
      int y1 = cy + (int)(sinf(a) * prev_len);
      M5Dial.Display.drawLine(cx, cy, x1, y1, t.bg);
    }
    int len = (outer_r * s) / steps;
    for (int i = 0; i < rays; ++i)
    {
      float a = (2.0f * (float)M_PI * i) / rays;
      int x1 = cx + (int)(cosf(a) * len);
      int y1 = cy + (int)(sinf(a) * len);
      uint16_t col = (i % 2 == 0) ? t.primary : t.accent;
      M5Dial.Display.drawLine(cx, cy, x1, y1, col);
    }
    prev_len = len;
    M5.update(); M5Dial.update();
    delay(0);
    delay(delay_ms);
  }

  // Clean restore
  draw_ring(true);
  if (Config::DebugBtn) Serial.println("[EFFECT] Starburst end");
}
static void play_invert()
{
  if (mute)
    return;
  // Two-tone "be-boop" for invert
  M5Dial.Speaker.tone(Config::InvertTone1Freq, Config::InvertTone1Ms);
  tone2_at = millis() + Config::InvertToneGapMs;
  tone2_freq = Config::InvertTone2Freq;
  tone2_dur = Config::InvertTone2Ms;
}
