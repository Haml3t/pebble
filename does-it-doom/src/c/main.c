#include <pebble.h>
#include "doom/engine.h"

// === Does It Doom? — Pebble Time 2 (emery) port ===========================
//
// Path C engine: raycaster using shareware DOOM1.WAD textures + palette.
// Architecture rationale lives in
// ~/.claude/projects/.../memory/project_does_it_doom.md.
//
// This file owns: window/layer setup, input collection, the AppTimer-driven
// frame loop, the shadow buffer, and the blit from shadow buffer to
// Pebble's framebuffer. All game state + raycaster math lives in
// src/c/doom/engine.c.

#define FRAME_MS 50              // 20 fps target
#define PERF_LOG_INTERVAL_MS 1000
#define TITLE_AUTOPLAY_MS 5000   // also doubles as appstore-screenshot demo mode

// Display: 200x228 emery. Engine renders 200x178 viewport, leaving 50px
// at the bottom for a future statbar (placeholder solid color in v0).
#define DISPLAY_W 200
#define DISPLAY_H 228
#define STATBAR_Y (DISPLAY_H - STATBAR_H)

typedef enum {
  STAGE_TITLE = 0,   // splash on boot; SELECT enters play
  STAGE_PLAY,        // engine is ticking + rendering
  STAGE_DEAD,        // player hp hit 0; "YOU DIED"; SELECT/BACK returns to title
} stage_t;

static const char *stage_name(stage_t s) {
  switch (s) {
    case STAGE_TITLE: return "TITLE";
    case STAGE_PLAY:  return "PLAY";
    case STAGE_DEAD:  return "DEAD";
    default:          return "?";
  }
}

static Window  *s_window;
static Layer   *s_screen_layer;
static AppTimer *s_frame_timer;

static stage_t  s_stage = STAGE_TITLE;
static uint32_t s_title_enter_ms;    // wall-clock when we entered TITLE
static uint32_t s_frame_count;       // since last PERF log
static uint32_t s_total_frames;
static uint32_t s_last_perf_ms;

// Input state. Updated by button handlers; consumed once per tick by
// engine. SELECT fire is edge-triggered (cleared after each tick).
static engine_input_t s_input;
static bool s_select_edge;
static int16_t s_accel_x;

// Shadow buffer: SHADOW_W * SHADOW_H bytes (200*228 = 45,600). Static BSS
// rather than heap-allocated — the size is fixed at compile time, and
// keeping it out of the heap avoids fragmenting around the engine's many
// small per-asset allocations (textures + sprites). Costs ~46KB of the
// 128 KB app slot in BSS; leaves ~82 KB for assets + Z_Zone.
static uint8_t s_shadow[SHADOW_W * SHADOW_H];

// === Time helpers =========================================================

static uint32_t now_ms(void) {
  time_t s; uint16_t ms; time_ms(&s, &ms);
  return (uint32_t)s * 1000u + ms;
}

// === Drawing ==============================================================

static void draw_title(GContext *ctx, GRect b) {
  graphics_context_set_fill_color(ctx, GColorDarkCandyAppleRed);
  graphics_fill_rect(ctx, b, 0, GCornerNone);
  graphics_context_set_text_color(ctx, GColorWhite);
  GFont big = fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD);
  graphics_draw_text(ctx, "DOES IT", big, GRect(0, 30, b.size.w, 32),
    GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
  graphics_draw_text(ctx, "DOOM?", big, GRect(0, 62, b.size.w, 32),
    GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
  GFont small = fonts_get_system_font(FONT_KEY_GOTHIC_18);
  graphics_draw_text(ctx, "SELECT to play", small, GRect(0, 130, b.size.w, 22),
    GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
  char heap[24];
  snprintf(heap, sizeof(heap), "heap %u", (unsigned)heap_bytes_free());
  graphics_draw_text(ctx, heap, small, GRect(0, 160, b.size.w, 22),
    GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
}

static void draw_play(GContext *ctx, GRect b) {
  // Blit shadow buffer (now static BSS) to the framebuffer rows that
  // correspond to the viewport. Pattern proven in Glance: capture, write
  // per-row via gbitmap_get_data_row_info, release.
  GBitmap *fb = graphics_capture_frame_buffer(ctx);
  if (!fb) return;
  // Engine writes the full display (viewport + statbar) into s_shadow.
  for (int y = 0; y < SHADOW_H && y < DISPLAY_H; y++) {
    GBitmapDataRowInfo row = gbitmap_get_data_row_info(fb, y);
    int copy_w = (SHADOW_W < (row.max_x - row.min_x + 1))
      ? SHADOW_W : (row.max_x - row.min_x + 1);
    memcpy(row.data + row.min_x, s_shadow + y * SHADOW_W, copy_w);
  }
  graphics_release_frame_buffer(ctx, fb);
}

static void draw_dead(GContext *ctx, GRect b) {
  graphics_context_set_fill_color(ctx, GColorDarkCandyAppleRed);
  graphics_fill_rect(ctx, b, 0, GCornerNone);
  graphics_context_set_text_color(ctx, GColorWhite);
  GFont big = fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD);
  graphics_draw_text(ctx, "YOU", big, GRect(0, 50, b.size.w, 32),
    GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
  graphics_draw_text(ctx, "DIED", big, GRect(0, 82, b.size.w, 32),
    GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
  GFont small = fonts_get_system_font(FONT_KEY_GOTHIC_18);
  graphics_draw_text(ctx, "BACK to retry", small,
    GRect(0, 140, b.size.w, 22),
    GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
}

static void screen_update_proc(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  if (s_stage == STAGE_TITLE)      draw_title(ctx, b);
  else if (s_stage == STAGE_DEAD)  draw_dead(ctx, b);
  else                             draw_play(ctx, b);
}

// === Frame loop ===========================================================

// Forward decl: enter_play is defined below the input section but
// referenced from frame_cb for the auto-advance.
static void enter_play(void);

static void emit_perf_if_due(void) {
  uint32_t now = now_ms();
  if (s_last_perf_ms == 0) { s_last_perf_ms = now; return; }
  if (now - s_last_perf_ms < PERF_LOG_INTERVAL_MS) return;
  uint32_t fps = s_frame_count * 1000u / (now - s_last_perf_ms);

  engine_debug_t d; engine_get_debug(&d);
  APP_LOG(APP_LOG_LEVEL_INFO,
    "PERF fps=%lu heap=%u state=%s "
    "px=%d py=%d pa=%d tile=%u",
    (unsigned long)fps, (unsigned)heap_bytes_free(), stage_name(s_stage),
    d.player_x_q8 >> 8, d.player_y_q8 >> 8,
    d.player_angle_deg, (unsigned)d.current_tile);
  s_frame_count = 0;
  s_last_perf_ms = now;
}

static void frame_cb(void *unused) {
  s_frame_timer = NULL;
  s_frame_count++;
  s_total_frames++;
  emit_perf_if_due();
  uint32_t frame_start_ms = now_ms();
  // Auto-advance from TITLE after a few seconds — acts as appstore demo
  // mode and also unblocks emulator iteration (QEMU button injection
  // doesn't propagate to the app on this emery build).
  if (s_stage == STAGE_TITLE
      && (now_ms() - s_title_enter_ms) > TITLE_AUTOPLAY_MS) {
    enter_play();
  }
  if (s_stage == STAGE_PLAY) {
    s_input.fire = s_select_edge;
    s_select_edge = false;
    s_input.accel_x = s_accel_x;
    engine_tick(&s_input);
    engine_render(s_shadow);
    // Death promotion happens after the tick so the final hp=0 frame
    // still gets rendered (player sees the imp landing the last hit).
    if (engine_player_dead()) {
      engine_debug_t d; engine_get_debug(&d);
      APP_LOG(APP_LOG_LEVEL_INFO, "player died, kills=%u", (unsigned)d.kills);
      s_stage = STAGE_DEAD;
    }
  }
  // Watchdog log: any render+tick that took >100ms is a serious stall and
  // a possible cause of system app-kill. Reporting these lets us notice
  // them in the PERF history rather than waiting for the next compile.
  uint32_t frame_ms = now_ms() - frame_start_ms;
  if (frame_ms > 100) {
    APP_LOG(APP_LOG_LEVEL_WARNING,
      "STALL frame=%lu ms state=%s",
      (unsigned long)frame_ms, stage_name(s_stage));
  }
  layer_mark_dirty(s_screen_layer);
  s_frame_timer = app_timer_register(FRAME_MS, frame_cb, NULL);
}

// === Input ================================================================

static void enter_play(void) {
  if (s_stage == STAGE_PLAY) return;
  APP_LOG(APP_LOG_LEVEL_INFO,
    "enter_play heap_before_init=%u", (unsigned)heap_bytes_free());
  engine_init();
  APP_LOG(APP_LOG_LEVEL_INFO,
    "enter_play heap_after_init=%u", (unsigned)heap_bytes_free());
  s_stage = STAGE_PLAY;
}

static void btn_select_click(ClickRecognizerRef r, void *ctx) {
  if (s_stage == STAGE_TITLE) { enter_play(); return; }
  if (s_stage == STAGE_DEAD)  { enter_play(); return; }
  // In play: fire is edge-triggered; held-fire is fine because rapid
  // re-clicks still set the edge each frame.
  s_select_edge = true;
}

static void btn_back_click(ClickRecognizerRef r, void *ctx) {
  if (s_stage == STAGE_PLAY || s_stage == STAGE_DEAD) {
    s_stage = STAGE_TITLE;
    s_title_enter_ms = now_ms();
    return;
  }
  window_stack_pop_all(true);
}

static void btn_back_long(ClickRecognizerRef r, void *ctx) {
  APP_LOG(APP_LOG_LEVEL_INFO, "BTN_BACK_LONG (pause menu todo)");
}

static void btn_raw_down(ClickRecognizerRef r, void *ctx) {
  ButtonId b = click_recognizer_get_button_id(r);
  if (b == BUTTON_ID_UP)   s_input.up_held = true;
  if (b == BUTTON_ID_DOWN) s_input.down_held = true;
  if (b == BUTTON_ID_BACK) s_input.back_held = true;
}

static void btn_raw_up(ClickRecognizerRef r, void *ctx) {
  ButtonId b = click_recognizer_get_button_id(r);
  if (b == BUTTON_ID_UP)   s_input.up_held = false;
  if (b == BUTTON_ID_DOWN) s_input.down_held = false;
  if (b == BUTTON_ID_BACK) s_input.back_held = false;
}

static void click_config_provider(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_SELECT, btn_select_click);
  window_single_click_subscribe(BUTTON_ID_BACK,   btn_back_click);
  window_long_click_subscribe(BUTTON_ID_BACK, 1500, btn_back_long, NULL);
  // Raw subscribers maintain held-state for movement + strafe chord. SELECT
  // stays on single_click so its callback isn't eaten by a raw subscriber
  // (Pebble click-router gotcha).
  window_raw_click_subscribe(BUTTON_ID_UP,   btn_raw_down, btn_raw_up, NULL);
  window_raw_click_subscribe(BUTTON_ID_DOWN, btn_raw_down, btn_raw_up, NULL);
  window_raw_click_subscribe(BUTTON_ID_BACK, btn_raw_down, btn_raw_up, NULL);
}

// === Accel ================================================================

static void accel_handler(AccelData *data, uint32_t num_samples) {
  if (num_samples == 0) return;
  s_accel_x = data[num_samples - 1].x;
}

// === Window lifecycle =====================================================

static void window_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  GRect b = layer_get_bounds(root);
  s_screen_layer = layer_create(b);
  layer_set_update_proc(s_screen_layer, screen_update_proc);
  layer_add_child(root, s_screen_layer);
}

static void window_unload(Window *w) {
  layer_destroy(s_screen_layer);
  s_screen_layer = NULL;
  // s_shadow is BSS — no free needed.
}

static void init(void) {
  APP_LOG(APP_LOG_LEVEL_INFO,
    "INIT_OK heap=%u inbox_max=%u outbox_max=%u",
    (unsigned)heap_bytes_free(),
    (unsigned)app_message_inbox_size_maximum(),
    (unsigned)app_message_outbox_size_maximum());
  s_window = window_create();
  window_set_background_color(s_window, GColorBlack);
  window_set_click_config_provider(s_window, click_config_provider);
  window_set_window_handlers(s_window, (WindowHandlers){
    .load = window_load, .unload = window_unload,
  });
  window_stack_push(s_window, true);
  accel_data_service_subscribe(5, accel_handler);
  accel_service_set_sampling_rate(ACCEL_SAMPLING_25HZ);
  s_title_enter_ms = now_ms();
  s_frame_timer = app_timer_register(FRAME_MS, frame_cb, NULL);
}

static void deinit(void) {
  if (s_frame_timer) app_timer_cancel(s_frame_timer);
  accel_data_service_unsubscribe();
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
