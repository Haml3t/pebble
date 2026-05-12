#include <pebble.h>

static Window *s_window;
static Layer *s_root_layer;

static Layer *s_hr_layer;
static Layer *s_weather_layer;
static Layer *s_now_playing_layer;
static Layer *s_calendar_layer;
static Layer *s_art_layer;

#define ART_W 100
#define ART_H 114
static uint8_t s_art_pixels[ART_W * ART_H];
static bool    s_art_has_data;

// Default photo baked in as a raw 100×114 palette-encoded resource; copied
// into s_art_pixels at startup and when a new song's transfer begins, so the
// background is never black/blank.
static void load_default_art(void) {
  ResHandle h = resource_get_handle(RESOURCE_ID_DEFAULT_ART);
  size_t n = resource_load(h, s_art_pixels, sizeof(s_art_pixels));
  if (n == sizeof(s_art_pixels)) s_art_has_data = true;
}

// Outlined text layer state, stored as layer data via layer_create_with_data.
// update_proc renders the text at every offset in a (2R+1)×(2R+1) grid in
// outline_color, then once at (0,0) in main_color, producing an R-pixel halo.
// Radius is per-layer because halos on adjacent letters merge into solid bars
// once the radius exceeds the kerning gap; small fonts need a smaller radius.
typedef struct {
  GFont font;
  GTextAlignment align;
  GColor main_color;
  GColor outline_color;
  uint8_t outline_radius;
  const char *text;
} OutlinedText;

static int s_screen_w, s_screen_h;
static GRect s_hr_rect, s_weather_rect, s_np_rect, s_cal_rect;

static char s_hr_buf[16];
static char s_weather_buf[32];
static char s_now_playing_buf[96];
static char s_calendar_buf[80];

static char s_now_title[48];
static char s_now_artist[48];

static void outlined_text_set_text(Layer *layer, const char *text) {
  OutlinedText *ot = (OutlinedText *)layer_get_data(layer);
  ot->text = text;
  layer_mark_dirty(layer);
}

static void outlined_text_set_colors(Layer *layer, GColor main, GColor outline) {
  OutlinedText *ot = (OutlinedText *)layer_get_data(layer);
  ot->main_color = main;
  ot->outline_color = outline;
  layer_mark_dirty(layer);
}

static void outlined_text_update_proc(Layer *layer, GContext *ctx) {
  OutlinedText *ot = (OutlinedText *)layer_get_data(layer);
  if (!ot->text || !ot->text[0]) return;
  GRect b = layer_get_bounds(layer);
  graphics_context_set_text_color(ctx, ot->outline_color);
  int r_max = ot->outline_radius;
  for (int dy = -r_max; dy <= r_max; dy++) {
    for (int dx = -r_max; dx <= r_max; dx++) {
      if (dx == 0 && dy == 0) continue;
      GRect r = GRect(b.origin.x + dx, b.origin.y + dy, b.size.w, b.size.h);
      graphics_draw_text(ctx, ot->text, ot->font, r,
                         GTextOverflowModeTrailingEllipsis, ot->align, NULL);
    }
  }
  graphics_context_set_text_color(ctx, ot->main_color);
  graphics_draw_text(ctx, ot->text, ot->font, b,
                     GTextOverflowModeTrailingEllipsis, ot->align, NULL);
}

static Layer *make_outlined_text(GRect frame, GFont font, GTextAlignment align,
                                 GColor main, GColor outline, uint8_t radius) {
  Layer *layer = layer_create_with_data(frame, sizeof(OutlinedText));
  OutlinedText *ot = (OutlinedText *)layer_get_data(layer);
  ot->font = font;
  ot->align = align;
  ot->main_color = main;
  ot->outline_color = outline;
  ot->outline_radius = radius;
  ot->text = "";
  layer_set_update_proc(layer, outlined_text_update_proc);
  layer_add_child(s_root_layer, layer);
  return layer;
}

// Cardio zone color thresholds (Karvonen, age 32, HRrest 68, HRmax 188).
static GColor hr_zone_color(long hr) {
  if (hr <= 0)   return GColorWhite;
  if (hr < 100)  return GColorWhite;   // resting / inactive
  if (hr < 140)  return GColorGreen;   // Z1
  if (hr < 152)  return GColorYellow;  // Z2
  if (hr < 176)  return GColorOrange;  // Z3–Z4
  return GColorRed;                    // Z5
}

static void update_heart_rate(void) {
  long hr = 0;
#if defined(PBL_HEALTH)
  hr = (long)health_service_peek_current_value(HealthMetricHeartRateBPM);
#endif
  if (hr > 0) {
    snprintf(s_hr_buf, sizeof(s_hr_buf), "%ld", hr);
  } else {
    snprintf(s_hr_buf, sizeof(s_hr_buf), "--");
  }
  outlined_text_set_text(s_hr_layer, s_hr_buf);
  // HR keeps a black outline against the dynamic zone color.
  outlined_text_set_colors(s_hr_layer, hr_zone_color(hr), GColorBlack);
}

static void render_now_playing(void) {
  if (s_now_title[0] == '\0') {
    snprintf(s_now_playing_buf, sizeof(s_now_playing_buf), "Nothing playing");
  } else if (s_now_artist[0] == '\0') {
    snprintf(s_now_playing_buf, sizeof(s_now_playing_buf), "%s", s_now_title);
  } else {
    snprintf(s_now_playing_buf, sizeof(s_now_playing_buf),
             "%s — %s", s_now_title, s_now_artist);
  }
  outlined_text_set_text(s_now_playing_layer, s_now_playing_buf);
}

#if defined(PBL_COLOR)
// Pebble 8-bit color: bits 7-6 alpha, 5-4 R, 3-2 G, 1-0 B (each 0-3).
// Luma scaled to roughly 0-255 using Rec.601 coefficients.
static int pebble_pixel_luma(uint8_t px) {
  int r = (px >> 4) & 0x3;
  int g = (px >> 2) & 0x3;
  int b = px & 0x3;
  return (r * 76 + g * 150 + b * 29) / 3;
}

static int region_avg_luma(GRect rect) {
  int sx0 = (rect.origin.x * ART_W) / s_screen_w;
  int sy0 = (rect.origin.y * ART_H) / s_screen_h;
  int sx1 = ((rect.origin.x + rect.size.w) * ART_W + s_screen_w - 1) / s_screen_w;
  int sy1 = ((rect.origin.y + rect.size.h) * ART_H + s_screen_h - 1) / s_screen_h;
  if (sx0 < 0) sx0 = 0;
  if (sy0 < 0) sy0 = 0;
  if (sx1 > ART_W) sx1 = ART_W;
  if (sy1 > ART_H) sy1 = ART_H;
  long sum = 0;
  int count = 0;
  for (int y = sy0; y < sy1; y++) {
    for (int x = sx0; x < sx1; x++) {
      sum += pebble_pixel_luma(s_art_pixels[y * ART_W + x]);
      count++;
    }
  }
  return count > 0 ? (int)(sum / count) : 0;
}

static void apply_dynamic_color(Layer *layer, GRect rect) {
  int avg = region_avg_luma(rect);
  if (avg > 128) {
    outlined_text_set_colors(layer, GColorBlack, GColorWhite);
  } else {
    outlined_text_set_colors(layer, GColorWhite, GColorBlack);
  }
}

static void recompute_text_contrast(void) {
  apply_dynamic_color(s_weather_layer, s_weather_rect);
  apply_dynamic_color(s_now_playing_layer, s_np_rect);
  apply_dynamic_color(s_calendar_layer, s_cal_rect);
  // HR stays zone-colored with a black outline (see update_heart_rate).
}

static void art_layer_update_proc(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  if (!s_art_has_data) {
    // Solid black so leftover framebuffer pixels (e.g., from a previous build
    // after `pebble install`) don't leak through in the gaps between text.
    graphics_context_set_fill_color(ctx, GColorBlack);
    graphics_fill_rect(ctx, b, 0, GCornerNone);
    return;
  }
  GBitmap *fb = graphics_capture_frame_buffer(ctx);
  if (!fb) return;
  GRect bounds = layer_get_bounds(layer);
  int dst_w = bounds.size.w;
  int dst_h = bounds.size.h;
  for (int y = 0; y < dst_h; y++) {
    int sy = (y * ART_H) / dst_h;
    int fb_y = y + bounds.origin.y;
    GBitmapDataRowInfo row = gbitmap_get_data_row_info(fb, fb_y);
    uint8_t *dst_row = row.data;
    for (int x = 0; x < dst_w; x++) {
      int fb_x = x + bounds.origin.x;
      if (fb_x < row.min_x || fb_x > row.max_x) continue;
      int sx = (x * ART_W) / dst_w;
      dst_row[fb_x] = s_art_pixels[sy * ART_W + sx];
    }
  }
  graphics_release_frame_buffer(ctx, fb);
}
#endif

static void handle_art_chunk(DictionaryIterator *iter) {
  Tuple *chunk = dict_find(iter, MESSAGE_KEY_ART_CHUNK);
  Tuple *off_t = dict_find(iter, MESSAGE_KEY_ART_OFFSET);
  Tuple *tot_t = dict_find(iter, MESSAGE_KEY_ART_TOTAL);
  if (!chunk || !off_t || !tot_t) return;
  uint16_t offset = off_t->value->uint16;
  uint16_t total  = tot_t->value->uint16;
  if (total != ART_W * ART_H) return;
  if (offset + chunk->length > sizeof(s_art_pixels)) return;
  // A new transfer (offset 0) repaints the default photo first so the screen
  // never shows mid-transfer pixels stitched from two different songs' covers.
  // Each subsequent chunk overwrites a slice of that default; visually this is
  // a top-down wipe from the photo to the new song's art.
  if (offset == 0) {
    load_default_art();
#if defined(PBL_COLOR)
    recompute_text_contrast();
#endif
    layer_mark_dirty(s_art_layer);
  }
  memcpy(&s_art_pixels[offset], chunk->value->data, chunk->length);
  if (offset + chunk->length >= total) {
    s_art_has_data = true;
#if defined(PBL_COLOR)
    recompute_text_contrast();
#endif
    layer_mark_dirty(s_art_layer);
  }
}

static void inbox_received_handler(DictionaryIterator *iter, void *context) {
  Tuple *t;
  bool weather_updated = false;
  int32_t temp = 0;
  const char *cond = NULL;

  if ((t = dict_find(iter, MESSAGE_KEY_WEATHER_TEMP))) {
    temp = t->value->int32;
    weather_updated = true;
  }
  if ((t = dict_find(iter, MESSAGE_KEY_WEATHER_COND))) {
    cond = t->value->cstring;
    weather_updated = true;
  }
  if (weather_updated) {
    snprintf(s_weather_buf, sizeof(s_weather_buf), "%ld° %s",
             (long)temp, cond ? cond : "");
    outlined_text_set_text(s_weather_layer, s_weather_buf);
  }

  if ((t = dict_find(iter, MESSAGE_KEY_CAL_TITLE))) {
    const char *title = t->value->cstring;
    Tuple *time_t = dict_find(iter, MESSAGE_KEY_CAL_TIME);
    if (time_t && time_t->value->cstring[0]) {
      snprintf(s_calendar_buf, sizeof(s_calendar_buf), "%s · %s",
               time_t->value->cstring, title);
    } else {
      snprintf(s_calendar_buf, sizeof(s_calendar_buf), "%s", title);
    }
    outlined_text_set_text(s_calendar_layer, s_calendar_buf);
  }

  bool np_updated = false;
  bool np_just_cleared = false;
  if ((t = dict_find(iter, MESSAGE_KEY_NOW_TITLE))) {
    bool was_playing = s_now_title[0] != '\0';
    strncpy(s_now_title, t->value->cstring, sizeof(s_now_title) - 1);
    s_now_title[sizeof(s_now_title) - 1] = '\0';
    np_updated = true;
    if (was_playing && s_now_title[0] == '\0') np_just_cleared = true;
  }
  if ((t = dict_find(iter, MESSAGE_KEY_NOW_ARTIST))) {
    strncpy(s_now_artist, t->value->cstring, sizeof(s_now_artist) - 1);
    s_now_artist[sizeof(s_now_artist) - 1] = '\0';
    np_updated = true;
  }
  if (np_updated) render_now_playing();
  if (np_just_cleared) {
    // Playback ended — the phone won't send fresh art, so restore the
    // default photo instead of leaving the last song's cover on screen.
    load_default_art();
#if defined(PBL_COLOR)
    recompute_text_contrast();
#endif
    layer_mark_dirty(s_art_layer);
  }

  if (dict_find(iter, MESSAGE_KEY_ART_CHUNK)) handle_art_chunk(iter);
}

static void request_refresh(void) {
  DictionaryIterator *out;
  AppMessageResult r = app_message_outbox_begin(&out);
  if (r != APP_MSG_OK) return;
  dict_write_uint8(out, MESSAGE_KEY_REQUEST_REFRESH, 1);
  app_message_outbox_send();
}

static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  update_heart_rate();
  if (units_changed & MINUTE_UNIT) {
    request_refresh();
  }
}

#if defined(PBL_HEALTH)
static void health_event_handler(HealthEventType event, void *context) {
  if (event == HealthEventHeartRateUpdate ||
      event == HealthEventSignificantUpdate) {
    update_heart_rate();
  }
}
#endif

static void window_load(Window *window) {
  window_set_background_color(window, GColorBlack);
  s_root_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(s_root_layer);
  int w = bounds.size.w;
  int h = bounds.size.h;
  s_screen_w = w;
  s_screen_h = h;

  s_hr_rect      = GRect(0, h * 0.06, w, 52);
  s_weather_rect = GRect(0, h * 0.32, w, 24);
  s_np_rect      = GRect(4, h - 64, w - 8, 24);
  s_cal_rect     = GRect(4, h - 32, w - 8, 30);

  // Seed the art buffer with the default photo so the screen is never blank.
  load_default_art();

  // Full-screen album art behind everything; nearest-neighbor stretched from ART_W×ART_H.
  s_art_layer = layer_create(bounds);
#if defined(PBL_COLOR)
  layer_set_update_proc(s_art_layer, art_layer_update_proc);
#endif
  layer_add_child(s_root_layer, s_art_layer);

  // 3px halo on the big LECO HR numbers (wide kerning, no merge risk);
  // 2px halos on the smaller body fonts to limit how often adjacent letters'
  // halos bridge into a solid bar.
  s_hr_layer = make_outlined_text(s_hr_rect,
      fonts_get_system_font(FONT_KEY_LECO_42_NUMBERS),
      GTextAlignmentCenter, GColorWhite, GColorBlack, 3);

  s_weather_layer = make_outlined_text(s_weather_rect,
      fonts_get_system_font(FONT_KEY_GOTHIC_24),
      GTextAlignmentCenter, GColorWhite, GColorBlack, 2);

  s_now_playing_layer = make_outlined_text(s_np_rect,
      fonts_get_system_font(FONT_KEY_GOTHIC_18),
      GTextAlignmentCenter, GColorWhite, GColorBlack, 2);

  s_calendar_layer = make_outlined_text(s_cal_rect,
      fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
      GTextAlignmentCenter, GColorWhite, GColorBlack, 2);

  outlined_text_set_text(s_hr_layer, "--");
  outlined_text_set_text(s_weather_layer, "—°");
  outlined_text_set_text(s_now_playing_layer, "Nothing playing");
  outlined_text_set_text(s_calendar_layer, "No upcoming event");

#if defined(PBL_COLOR)
  // Pick text colors that contrast with the default photo so the watchface
  // looks right before any song art has been received.
  recompute_text_contrast();
#endif
}

static void window_unload(Window *window) {
  layer_destroy(s_hr_layer);
  layer_destroy(s_weather_layer);
  layer_destroy(s_now_playing_layer);
  layer_destroy(s_calendar_layer);
  layer_destroy(s_art_layer);
}

static void init(void) {
  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers){
                                           .load = window_load,
                                           .unload = window_unload,
                                       });
  window_stack_push(s_window, true);

  update_heart_rate();

#if defined(PBL_HEALTH)
  // Force a fresh HR sample every 30s. Default is once every several minutes
  // so the displayed number can be very stale. 30s costs some battery but
  // matches a glance-style watchface.
  health_service_set_heart_rate_sample_period(30);
  health_service_events_subscribe(health_event_handler, NULL);
#endif

  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);

  app_message_register_inbox_received(inbox_received_handler);
  app_message_open(1024, 256);

  request_refresh();
}

static void deinit(void) {
#if defined(PBL_HEALTH)
  health_service_events_unsubscribe();
  health_service_set_heart_rate_sample_period(0);  // back to default
#endif
  tick_timer_service_unsubscribe();
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
