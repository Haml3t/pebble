#include <pebble.h>

// Metronome watchapp: tap-only metronome with session tracking.
// The Android companion (Glance NP) handles audio recording and persists
// per-day minute totals; the watch only renders UI and emits AppMessages
// describing OPEN / CLOSE / TICK_START / TICK_STOP / BPM_CHANGED.

#define MIN_BPM 30
#define MAX_BPM 240
#define DEFAULT_BPM 100

// Single-pulse vibration per beat. ~80ms is a crisp tap on Pebble's vibe
// motor — longer pulses don't increase amplitude (there is no public
// amplitude API), they just feel longer. Tweak here if a future firmware
// exposes more control.
#define VIBE_PULSE_MS 80

// Repeat rate for the +/- BPM buttons when held. ~150ms feels responsive
// without overshooting on quick adjustments.
#define BPM_REPEAT_MS 150

#define PERSIST_KEY_LAST_BPM 1

static Window *s_window;
static Layer *s_root_layer;

static TextLayer *s_bpm_layer;
static TextLayer *s_bpm_label_layer;
static TextLayer *s_action_layer;
static TextLayer *s_stats_layer;
static TextLayer *s_rec_layer;
static Layer     *s_beat_dots_layer;

static char s_bpm_text[8];
static char s_action_text[24];
static char s_stats_text[40];
static char s_rec_text[24];

static int  s_current_bpm = DEFAULT_BPM;
static bool s_ticking = false;
static AppTimer *s_tick_timer = NULL;
static int  s_beat_index = 0;       // 0..3, cycles for the dot indicator

static time_t s_app_start_ts;       // when the metronome app launched
static time_t s_tick_start_ts;      // when the current tick run began
static int    s_session_accum_secs; // seconds ticking this app-open (sum of runs)

// Today/week totals supplied by the Android companion. -1 = not yet received.
static int s_today_minutes = -1;
static int s_week_minutes  = -1;

static void send_event(uint32_t key, int32_t value);
static void schedule_next_beat(void);
static void update_stats_text(void);
static void update_rec_text(void);
static void update_action_text(void);
static void update_bpm_text(void);

// === Drawing =============================================================

static void beat_dots_update_proc(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  const int n = 4;
  const int r = 7;
  const int gap = 24;
  int total_w = n * (2 * r) + (n - 1) * (gap - 2 * r);
  int x0 = b.origin.x + (b.size.w - total_w) / 2 + r;
  int y  = b.origin.y + b.size.h / 2;
  for (int i = 0; i < n; i++) {
    GPoint p = GPoint(x0 + i * gap, y);
    if (s_ticking && i == s_beat_index) {
      graphics_context_set_fill_color(ctx, GColorYellow);
      graphics_fill_circle(ctx, p, r);
    } else {
      graphics_context_set_stroke_color(ctx, GColorWhite);
      graphics_draw_circle(ctx, p, r);
    }
  }
}

// === BPM control =========================================================

static void update_bpm_text(void) {
  snprintf(s_bpm_text, sizeof(s_bpm_text), "%d", s_current_bpm);
  text_layer_set_text(s_bpm_layer, s_bpm_text);
}

static void set_bpm(int new_bpm, bool emit) {
  if (new_bpm < MIN_BPM) new_bpm = MIN_BPM;
  if (new_bpm > MAX_BPM) new_bpm = MAX_BPM;
  if (new_bpm == s_current_bpm) return;
  s_current_bpm = new_bpm;
  update_bpm_text();
  persist_write_int(PERSIST_KEY_LAST_BPM, s_current_bpm);
  if (emit) send_event(MESSAGE_KEY_BPM_CHANGED, s_current_bpm);
  // If ticking, the next-beat timer's interval is already scheduled — let the
  // current beat fire as-is and the *next* scheduling will use the new BPM.
}

// === Tick loop ===========================================================

static void vibrate_tap(void) {
  static const uint32_t segments[] = { VIBE_PULSE_MS };
  VibePattern pat = { .durations = segments, .num_segments = 1 };
  vibes_enqueue_custom_pattern(pat);
}

static void tick_callback(void *ctx) {
  s_tick_timer = NULL;
  if (!s_ticking) return;
  vibrate_tap();
  s_beat_index = (s_beat_index + 1) & 0x3;
  layer_mark_dirty(s_beat_dots_layer);
  schedule_next_beat();
}

static void schedule_next_beat(void) {
  uint32_t interval = (uint32_t)(60000 / s_current_bpm);
  s_tick_timer = app_timer_register(interval, tick_callback, NULL);
}

static void start_ticking(void) {
  if (s_ticking) return;
  s_ticking = true;
  s_tick_start_ts = time(NULL);
  s_beat_index = 0;
  layer_mark_dirty(s_beat_dots_layer);
  update_action_text();
  // First beat fires immediately so the user gets confirmation; subsequent
  // beats are spaced by 60000/BPM.
  vibrate_tap();
  schedule_next_beat();
  send_event(MESSAGE_KEY_TICK_STARTED, s_current_bpm);
}

static void stop_ticking(void) {
  if (!s_ticking) return;
  s_ticking = false;
  if (s_tick_timer) {
    app_timer_cancel(s_tick_timer);
    s_tick_timer = NULL;
  }
  time_t now = time(NULL);
  if (s_tick_start_ts > 0 && now > s_tick_start_ts) {
    s_session_accum_secs += (int)(now - s_tick_start_ts);
  }
  s_tick_start_ts = 0;
  layer_mark_dirty(s_beat_dots_layer);
  update_action_text();
  send_event(MESSAGE_KEY_TICK_STOPPED, 0);
}

// === Buttons =============================================================

static void up_click(ClickRecognizerRef recognizer, void *context) {
  set_bpm(s_current_bpm + 1, true);
}

static void down_click(ClickRecognizerRef recognizer, void *context) {
  set_bpm(s_current_bpm - 1, true);
}

static void select_click(ClickRecognizerRef recognizer, void *context) {
  if (s_ticking) stop_ticking();
  else           start_ticking();
}

static void click_config_provider(void *context) {
  // Autorepeat on hold for ±1 BPM; tap is also handled as the first repeat.
  window_single_repeating_click_subscribe(BUTTON_ID_UP, BPM_REPEAT_MS, up_click);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, BPM_REPEAT_MS, down_click);
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click);
}

// === AppMessage ==========================================================

static void send_event(uint32_t key, int32_t value) {
  DictionaryIterator *out;
  if (app_message_outbox_begin(&out) != APP_MSG_OK) return;
  dict_write_int32(out, key, value);
  app_message_outbox_send();
}

static void send_open_event(void) {
  DictionaryIterator *out;
  if (app_message_outbox_begin(&out) != APP_MSG_OK) return;
  dict_write_int32(out, MESSAGE_KEY_METRONOME_OPENED, s_current_bpm);
  // Bundle a totals request so we can populate the stats line on first paint.
  dict_write_uint8(out, MESSAGE_KEY_TODAY_MINUTES_REQUEST, 1);
  app_message_outbox_send();
}

static void inbox_received_handler(DictionaryIterator *iter, void *context) {
  Tuple *t;
  bool stats_changed = false;
  if ((t = dict_find(iter, MESSAGE_KEY_TODAY_MINUTES))) {
    s_today_minutes = (int)t->value->int32;
    stats_changed = true;
  }
  if ((t = dict_find(iter, MESSAGE_KEY_WEEK_MINUTES))) {
    s_week_minutes = (int)t->value->int32;
    stats_changed = true;
  }
  if (stats_changed) update_stats_text();
}

// === UI text builders ====================================================

static void update_action_text(void) {
  snprintf(s_action_text, sizeof(s_action_text),
           s_ticking ? "SELECT to stop" : "SELECT to start");
  text_layer_set_text(s_action_layer, s_action_text);
}

static void update_stats_text(void) {
  // today: minutes only; week: H:MM if >=60, else just minutes.
  char today[12];
  char week[16];
  if (s_today_minutes < 0) snprintf(today, sizeof(today), "--");
  else                     snprintf(today, sizeof(today), "%dm", s_today_minutes);
  if (s_week_minutes < 0) {
    snprintf(week, sizeof(week), "--");
  } else if (s_week_minutes >= 60) {
    snprintf(week, sizeof(week), "%dh %dm",
             s_week_minutes / 60, s_week_minutes % 60);
  } else {
    snprintf(week, sizeof(week), "%dm", s_week_minutes);
  }
  snprintf(s_stats_text, sizeof(s_stats_text),
           "today %s  week %s", today, week);
  text_layer_set_text(s_stats_layer, s_stats_text);
}

static void format_mmss(int secs, char *buf, size_t n) {
  if (secs < 0) secs = 0;
  snprintf(buf, n, "%d:%02d", secs / 60, secs % 60);
}

static void update_rec_text(void) {
  time_t now = time(NULL);
  int rec_secs = (int)(now - s_app_start_ts);
  if (s_ticking) {
    int sess_secs = s_session_accum_secs;
    if (s_tick_start_ts > 0) sess_secs += (int)(now - s_tick_start_ts);
    char sess[10], rec[10];
    format_mmss(sess_secs, sess, sizeof(sess));
    format_mmss(rec_secs, rec, sizeof(rec));
    snprintf(s_rec_text, sizeof(s_rec_text),
             "session %s  REC %s", sess, rec);
  } else {
    char rec[10];
    format_mmss(rec_secs, rec, sizeof(rec));
    snprintf(s_rec_text, sizeof(s_rec_text), "REC %s", rec);
  }
  text_layer_set_text(s_rec_layer, s_rec_text);
}

// 1Hz UI tick — keeps the session/REC timers fresh without scheduling our
// own timer separately from the beat timer (which fires at BPM rate).
static void clock_tick_handler(struct tm *tm, TimeUnits changed) {
  update_rec_text();
}

// === Window lifecycle ====================================================

static void window_load(Window *window) {
  window_set_background_color(window, GColorBlack);
  s_root_layer = window_get_root_layer(window);
  GRect b = layer_get_bounds(s_root_layer);

  // Big BPM number — LECO_42_NUMBERS for parity with the Glance HR digit
  // style. Sits in the top half of the screen.
  s_bpm_layer = text_layer_create(GRect(0, 18, b.size.w, 60));
  text_layer_set_background_color(s_bpm_layer, GColorClear);
  text_layer_set_text_color(s_bpm_layer, GColorWhite);
  text_layer_set_font(s_bpm_layer, fonts_get_system_font(FONT_KEY_LECO_42_NUMBERS));
  text_layer_set_text_alignment(s_bpm_layer, GTextAlignmentCenter);
  layer_add_child(s_root_layer, text_layer_get_layer(s_bpm_layer));

  s_bpm_label_layer = text_layer_create(GRect(0, 78, b.size.w, 22));
  text_layer_set_background_color(s_bpm_label_layer, GColorClear);
  text_layer_set_text_color(s_bpm_label_layer, GColorLightGray);
  text_layer_set_font(s_bpm_label_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18));
  text_layer_set_text_alignment(s_bpm_label_layer, GTextAlignmentCenter);
  text_layer_set_text(s_bpm_label_layer, "BPM");
  layer_add_child(s_root_layer, text_layer_get_layer(s_bpm_label_layer));

  // 4-dot beat indicator. Kept in roughly the same place across idle/ticking
  // so the user's eye doesn't track to a new spot on tick-start.
  s_beat_dots_layer = layer_create(GRect(0, 110, b.size.w, 22));
  layer_set_update_proc(s_beat_dots_layer, beat_dots_update_proc);
  layer_add_child(s_root_layer, s_beat_dots_layer);

  s_action_layer = text_layer_create(GRect(0, 138, b.size.w, 22));
  text_layer_set_background_color(s_action_layer, GColorClear);
  text_layer_set_text_color(s_action_layer, GColorWhite);
  text_layer_set_font(s_action_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18));
  text_layer_set_text_alignment(s_action_layer, GTextAlignmentCenter);
  layer_add_child(s_root_layer, text_layer_get_layer(s_action_layer));

  s_stats_layer = text_layer_create(GRect(0, 178, b.size.w, 18));
  text_layer_set_background_color(s_stats_layer, GColorClear);
  text_layer_set_text_color(s_stats_layer, GColorLightGray);
  text_layer_set_font(s_stats_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
  text_layer_set_text_alignment(s_stats_layer, GTextAlignmentCenter);
  layer_add_child(s_root_layer, text_layer_get_layer(s_stats_layer));

  s_rec_layer = text_layer_create(GRect(0, 202, b.size.w, 22));
  text_layer_set_background_color(s_rec_layer, GColorClear);
  text_layer_set_text_color(s_rec_layer, GColorRed);
  text_layer_set_font(s_rec_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  text_layer_set_text_alignment(s_rec_layer, GTextAlignmentCenter);
  layer_add_child(s_root_layer, text_layer_get_layer(s_rec_layer));

  update_bpm_text();
  update_action_text();
  update_stats_text();
  update_rec_text();
}

static void window_unload(Window *window) {
  text_layer_destroy(s_bpm_layer);
  text_layer_destroy(s_bpm_label_layer);
  text_layer_destroy(s_action_layer);
  text_layer_destroy(s_stats_layer);
  text_layer_destroy(s_rec_layer);
  layer_destroy(s_beat_dots_layer);
}

static void init(void) {
  s_app_start_ts = time(NULL);
  s_current_bpm = persist_exists(PERSIST_KEY_LAST_BPM)
                      ? persist_read_int(PERSIST_KEY_LAST_BPM)
                      : DEFAULT_BPM;
  if (s_current_bpm < MIN_BPM || s_current_bpm > MAX_BPM) {
    s_current_bpm = DEFAULT_BPM;
  }

  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers){
                                           .load = window_load,
                                           .unload = window_unload,
                                       });
  window_set_click_config_provider(s_window, click_config_provider);
  window_stack_push(s_window, true);

  app_message_register_inbox_received(inbox_received_handler);
  app_message_open(256, 256);

  tick_timer_service_subscribe(SECOND_UNIT, clock_tick_handler);

  // Tell the Android companion the app is open *and* what BPM we're starting
  // at, so it can begin recording with the right filename basis and seed its
  // BPM-range tracker. Also requests current totals.
  send_open_event();
}

static void deinit(void) {
  // If the user backs out while ticking, treat it as a graceful stop so the
  // Android side flushes the session into today's total and closes the file.
  if (s_ticking) stop_ticking();
  send_event(MESSAGE_KEY_METRONOME_CLOSED, 0);

  tick_timer_service_unsubscribe();
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
