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

// Holding UP+DOWN together for this long toggles BPM-edit mode. Perf mode
// is the default — UP marks a section, DOWN stops recording. Edit mode
// reassigns UP/DOWN to ±1 BPM (the classic behavior).
#define MODE_HOLD_MS 2000

// Holding DOWN alone for this long stops recording. The hold gate exists
// so an accidental tap mid-practice doesn't kill the session — recording
// starts instantly on app-open and only stops on a deliberate hold.
#define REC_STOP_HOLD_MS 2000

// Holding SELECT alone for this long toggles beat-vibration mute. The
// metronome keeps ticking (animation continues, recording continues) — we
// just stop enqueuing the tactile pulse. Same 2s threshold as the other
// hold gates for muscle-memory consistency.
#define VIBE_MUTE_HOLD_MS 2000

// BACK long-click threshold. Shorter than the other hold gates — a 2s hold
// felt like "did I miss it?" in testing because BACK has no visual/tactile
// in-progress cue. 1s is unmistakably longer than a quick exit-tap but fast
// enough to fire while the user is still committing to the gesture.
#define HELP_HOLD_MS 1000

#define PERSIST_KEY_LAST_BPM       1
// Last-known today/week minutes — cached so the watch shows the previous
// totals instantly on app open, instead of "today -- week --" until the
// Android companion's first response arrives.
#define PERSIST_KEY_TODAY_MINUTES  2
#define PERSIST_KEY_WEEK_MINUTES   3

static Window *s_window;
static Layer *s_root_layer;

static TextLayer *s_bpm_layer;
static TextLayer *s_bpm_label_layer;
static TextLayer *s_action_layer;
static TextLayer *s_help_hint_layer;
static TextLayer *s_stats_layer;
static TextLayer *s_rec_layer;
static Layer     *s_beat_dots_layer;
static Layer     *s_rec_indicator_layer;

// Help screen — separate window, lazy-initialized when the user first
// holds BACK. Stays around between opens so we don't churn allocation.
static Window     *s_help_window = NULL;
static ScrollLayer *s_help_scroll_layer = NULL;
static TextLayer  *s_help_text_layer = NULL;

static char s_bpm_text[8];
static char s_action_text[24];
static char s_stats_text[40];
static char s_rec_text[24];

static int  s_current_bpm = DEFAULT_BPM;
static bool s_ticking = false;
static bool s_recording = false;    // mirrored from Android MediaRecorder state
static bool s_edit_mode = false;    // true → UP/DOWN adjust BPM; false → perf mode (UP=marker, DOWN=stop rec)
static bool s_vibe_muted = false;   // true → suppress beat vibrate; animation + recording continue
static AppTimer *s_tick_timer = NULL;
static int  s_beat_index = 0;       // 0..3, cycles for the dot indicator

// Both-button-held mode toggle: track each button's raw press state and
// schedule an AppTimer that only fires if both stay held for MODE_HOLD_MS.
// DOWN-alone hold (perf mode) drives a separate timer that stops recording
// after REC_STOP_HOLD_MS — mutually exclusive with the mode-toggle timer.
// UP+SELECT held together opens the help screen via a third timer.
static bool s_up_pressed = false;
static bool s_down_pressed = false;
static bool s_select_pressed = false;
static AppTimer *s_hold_timer = NULL;
static AppTimer *s_rec_stop_hold_timer = NULL;
static AppTimer *s_help_gesture_timer = NULL;
static bool s_help_gesture_fired = false;

static time_t s_app_start_ts;       // when the metronome app launched
static time_t s_tick_start_ts;      // when the current tick run began
static int    s_session_accum_secs; // seconds ticking this app-open (sum of runs)
static time_t s_rec_start_ts;       // when the current recording began (0 = no active rec)

// Today/week totals supplied by the Android companion. -1 = not yet received.
static int s_today_minutes = -1;
static int s_week_minutes  = -1;

static void send_event(uint32_t key, int32_t value);
static void schedule_next_beat(void);
static void update_stats_text(void);
static void update_rec_text(void);
static void update_action_text(void);
static void update_bpm_text(void);
static void update_bpm_label(void);
static void show_help(void);

// === Drawing =============================================================

static void rec_indicator_update_proc(Layer *layer, GContext *ctx) {
  if (!s_recording) return;
  GRect b = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, GColorRed);
  graphics_fill_rect(ctx, b, 0, GCornerNone);
}

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
      // Dim the active-beat fill while muted — a subtle visual cue that
      // the metronome is still pacing the user but silently.
      graphics_context_set_fill_color(ctx,
          s_vibe_muted ? GColorLightGray : GColorYellow);
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
  // Mute gate: keeps the beat-loop architecture intact (timer keeps firing,
  // s_beat_index keeps advancing, layer re-draws) — we just skip the vibe.
  if (s_vibe_muted) return;
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

static bool both_held(void) { return s_up_pressed && s_down_pressed; }

static void cancel_hold_timer(void) {
  if (s_hold_timer) {
    app_timer_cancel(s_hold_timer);
    s_hold_timer = NULL;
  }
}

static void cancel_rec_stop_hold_timer(void) {
  if (s_rec_stop_hold_timer) {
    app_timer_cancel(s_rec_stop_hold_timer);
    s_rec_stop_hold_timer = NULL;
  }
}

static void cancel_help_gesture_timer(void) {
  if (s_help_gesture_timer) {
    app_timer_cancel(s_help_gesture_timer);
    s_help_gesture_timer = NULL;
  }
}

static void mode_toggle_callback(void *ctx) {
  s_hold_timer = NULL;
  s_edit_mode = !s_edit_mode;
  // Distinct confirmation vibe — three short pulses, unmistakable vs a
  // single beat tap — so the user knows the mode flipped without looking.
  static const uint32_t segs[] = { 60, 80, 60, 80, 60 };
  VibePattern pat = { .durations = segs, .num_segments = 5 };
  vibes_enqueue_custom_pattern(pat);
  text_layer_set_text_color(s_bpm_layer,
                            s_edit_mode ? GColorYellow : GColorWhite);
  update_action_text();
}

static void rec_stop_hold_callback(void *ctx) {
  s_rec_stop_hold_timer = NULL;
  // Long single buzz — distinct from beat-tap and from the 5-pulse mode toggle
  // so the user can confirm the stop-recording fired even while looking away.
  static const uint32_t segs[] = { 250 };
  VibePattern pat = { .durations = segs, .num_segments = 1 };
  vibes_enqueue_custom_pattern(pat);
  send_event(MESSAGE_KEY_STOP_RECORDING, 1);
}

static void help_gesture_callback(void *ctx) {
  s_help_gesture_timer = NULL;
  s_help_gesture_fired = true;
  show_help();
}

// Decide which hold timer (if any) should be running based on current button
// state. Multi-button gestures are mutually exclusive: UP+SELECT (help) and
// UP+DOWN (mode toggle) both win over single-button holds, so a multi-press
// during recording doesn't accidentally fire STOP_RECORDING via the
// DOWN-alone path.
static void update_hold_timers(void) {
  if (s_up_pressed && s_select_pressed) {
    cancel_hold_timer();
    cancel_rec_stop_hold_timer();
    if (!s_help_gesture_timer && !s_help_gesture_fired) {
      s_help_gesture_timer = app_timer_register(
          HELP_HOLD_MS, help_gesture_callback, NULL);
    }
  } else if (s_up_pressed && s_down_pressed) {
    cancel_rec_stop_hold_timer();
    cancel_help_gesture_timer();
    if (!s_hold_timer) {
      s_hold_timer = app_timer_register(MODE_HOLD_MS, mode_toggle_callback, NULL);
    }
  } else if (s_down_pressed && !s_edit_mode && s_recording) {
    // Only arm the stop-hold while recording is *currently* on. Without this
    // guard, holding DOWN from a stopped state would fire START_RECORDING via
    // down_click (instant) and then STOP_RECORDING via the timer 2s later.
    cancel_hold_timer();
    cancel_help_gesture_timer();
    if (!s_rec_stop_hold_timer) {
      s_rec_stop_hold_timer = app_timer_register(
          REC_STOP_HOLD_MS, rec_stop_hold_callback, NULL);
    }
  } else {
    cancel_hold_timer();
    cancel_rec_stop_hold_timer();
    cancel_help_gesture_timer();
  }
}

static void up_raw_down(ClickRecognizerRef r, void *ctx) {
  // A fresh UP press without SELECT held means the user is starting a
  // new interaction, not continuing the previous help gesture. Drop the
  // latch so select_click won't keep suppressing start/stop ticking.
  if (!s_select_pressed) s_help_gesture_fired = false;
  s_up_pressed = true;
  update_hold_timers();
}
static void up_raw_up(ClickRecognizerRef r, void *ctx) {
  s_up_pressed = false;
  update_hold_timers();
}
static void down_raw_down(ClickRecognizerRef r, void *ctx) {
  s_down_pressed = true;
  update_hold_timers();
}
static void down_raw_up(ClickRecognizerRef r, void *ctx) {
  s_down_pressed = false;
  update_hold_timers();
}
static void select_raw_down(ClickRecognizerRef r, void *ctx) {
  // Same latch-reset rule as up_raw_down — see comment there.
  if (!s_up_pressed) s_help_gesture_fired = false;
  s_select_pressed = true;
  update_hold_timers();
}
static void select_raw_up(ClickRecognizerRef r, void *ctx) {
  s_select_pressed = false;
  update_hold_timers();
}

static void up_click(ClickRecognizerRef recognizer, void *context) {
  // While both are held we're mid-gesture toward a mode toggle — suppress
  // the per-button action. The first button's initial click can still slip
  // through if the user presses non-simultaneously; that's a tolerable
  // edge case (one stray ±1 BPM or one stray marker). Same for UP+SELECT
  // (help gesture) — suppress markers while SELECT is also down.
  if (both_held()) return;
  if (s_select_pressed) return;
  if (s_edit_mode) {
    set_bpm(s_current_bpm + 1, true);
  } else {
    send_event(MESSAGE_KEY_MARKER, 1);
  }
}

static void down_click(ClickRecognizerRef recognizer, void *context) {
  if (both_held()) return;
  if (s_edit_mode) {
    set_bpm(s_current_bpm - 1, true);
    return;
  }
  // Perf mode: DOWN is asymmetric — instant-on, deliberate-off. A tap while
  // recording is *off* re-arms recording immediately; stopping requires the
  // REC_STOP_HOLD_MS hold (rec_stop_hold_callback). This prevents an
  // accidental tap mid-practice from killing the session while still giving
  // a fast way to recover after a deliberate stop.
  if (!s_recording) {
    send_event(MESSAGE_KEY_START_RECORDING, 1);
  }
}

static void select_click(ClickRecognizerRef recognizer, void *context) {
  // Suppress start/stop tick when this release ended a UP+SELECT help
  // gesture (or is mid-gesture with UP still held). Without this, opening
  // the help screen would also start/stop the metronome.
  if (s_up_pressed || s_help_gesture_fired) return;
  if (s_ticking) stop_ticking();
  else           start_ticking();
}

static void select_long_click(ClickRecognizerRef recognizer, void *context) {
  // Toggle the beat-vibration mute. The metronome keeps running — only the
  // tactile pulse is silenced (or unsilenced). Confirmation pattern uses
  // vibes_enqueue_custom_pattern directly, bypassing the mute, so the user
  // always feels a buzz acknowledging the toggle.
  s_vibe_muted = !s_vibe_muted;
  static const uint32_t segs[] = { 80, 60, 80 };
  VibePattern pat = { .durations = segs, .num_segments = 3 };
  vibes_enqueue_custom_pattern(pat);
  update_bpm_label();
  layer_mark_dirty(s_beat_dots_layer);
}

// === Help screen =========================================================

// Single source of truth for the button reference. Kept terse — every line
// has to read fast at arm's length on a 200px screen.
static const char *HELP_TEXT =
    "Buttons\n"
    "\n"
    "SELECT\n"
    " tap: start/stop\n"
    " hold 2s: mute beat\n"
    "\n"
    "UP\n"
    " tap: mark spot\n"
    " edit: +1 BPM\n"
    "\n"
    "DOWN\n"
    " tap: start rec\n"
    " hold 2s: stop rec\n"
    " edit: -1 BPM\n"
    "\n"
    "UP + DOWN\n"
    " hold 2s: edit mode\n"
    "\n"
    "UP + SELECT\n"
    " hold: this help\n"
    "\n"
    "BACK\n"
    " tap: exit app";

static void help_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect b = layer_get_bounds(root);

  s_help_scroll_layer = scroll_layer_create(b);
  // Measure how tall the text actually wants to be so ScrollLayer knows
  // how far to allow scrolling. graphics_text_layout_get_content_size is
  // a layout-only call — no drawing — so it's safe to invoke at load time.
  GFont font = fonts_get_system_font(FONT_KEY_GOTHIC_18);
  GSize text_size = graphics_text_layout_get_content_size(
      HELP_TEXT, font,
      GRect(0, 0, b.size.w - 8, 2000),
      GTextOverflowModeWordWrap, GTextAlignmentLeft);

  s_help_text_layer = text_layer_create(
      GRect(4, 0, b.size.w - 8, text_size.h + 8));
  text_layer_set_text(s_help_text_layer, HELP_TEXT);
  text_layer_set_font(s_help_text_layer, font);
  text_layer_set_background_color(s_help_text_layer, GColorBlack);
  text_layer_set_text_color(s_help_text_layer, GColorWhite);
  text_layer_set_text_alignment(s_help_text_layer, GTextAlignmentLeft);

  scroll_layer_add_child(s_help_scroll_layer,
                         text_layer_get_layer(s_help_text_layer));
  scroll_layer_set_content_size(s_help_scroll_layer,
                                GSize(b.size.w, text_size.h + 8));
  // UP/DOWN scroll, SELECT/BACK fall through to default (pops the window).
  scroll_layer_set_click_config_onto_window(s_help_scroll_layer, window);

  layer_add_child(root, scroll_layer_get_layer(s_help_scroll_layer));
}

static void help_window_unload(Window *window) {
  text_layer_destroy(s_help_text_layer);
  scroll_layer_destroy(s_help_scroll_layer);
  s_help_text_layer = NULL;
  s_help_scroll_layer = NULL;
}

static void help_window_disappear(Window *window) {
  // Returning to main. Reset ALL press-state tracking, not just the
  // help-gesture latch: the user released UP/SELECT while help was on
  // top, so the main window's raw_up handlers never fired. Without this
  // reset, s_up_pressed / s_select_pressed stay stuck true and the next
  // select_click on main thinks the gesture is still in progress.
  s_help_gesture_fired = false;
  s_up_pressed = false;
  s_down_pressed = false;
  s_select_pressed = false;
  cancel_hold_timer();
  cancel_rec_stop_hold_timer();
  cancel_help_gesture_timer();
}

static void show_help(void) {
  if (!s_help_window) {
    s_help_window = window_create();
    window_set_background_color(s_help_window, GColorBlack);
    window_set_window_handlers(s_help_window, (WindowHandlers){
                                                  .load = help_window_load,
                                                  .unload = help_window_unload,
                                                  .disappear = help_window_disappear,
                                              });
  }
  window_stack_push(s_help_window, true);
}

// Help-screen gesture: UP+SELECT held together for HELP_HOLD_MS. BACK
// was tried first but the Pebble OS has its own long-BACK exit fail-safe
// that overrides app subscriptions, and raw-click alone on BACK doesn't
// reliably suppress the default exit. UP+SELECT is otherwise unused so
// it's a safe gesture to claim.

static void click_config_provider(void *context) {
  // single_repeating fires on press + every BPM_REPEAT_MS while held —
  // gives autorepeat for ±BPM in edit mode and "hold to mark many" in perf
  // mode. Raw subscriptions run alongside and only track press state for
  // the both-held gesture; they don't fire actions themselves.
  window_single_repeating_click_subscribe(BUTTON_ID_UP, BPM_REPEAT_MS, up_click);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, BPM_REPEAT_MS, down_click);
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click);
  // Long-click and single-click are mutually exclusive: presses shorter than
  // VIBE_MUTE_HOLD_MS fire select_click on release; presses that cross the
  // threshold fire select_long_click instead, never both.
  window_long_click_subscribe(BUTTON_ID_SELECT, VIBE_MUTE_HOLD_MS,
                              select_long_click, NULL);
  // BACK uses Pebble's default exit-on-press behavior — no subscription
  // here. We tried raw_click on BACK to make a hold-to-help gesture, but
  // the OS has a hard-exit failsafe that pre-empts app-level long-press
  // detection. Help is reached via UP+SELECT held instead (see
  // help_gesture_callback + update_hold_timers).
  window_raw_click_subscribe(BUTTON_ID_UP, up_raw_down, up_raw_up, NULL);
  window_raw_click_subscribe(BUTTON_ID_DOWN, down_raw_down, down_raw_up, NULL);
  window_raw_click_subscribe(BUTTON_ID_SELECT,
                             select_raw_down, select_raw_up, NULL);
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
    persist_write_int(PERSIST_KEY_TODAY_MINUTES, s_today_minutes);
    stats_changed = true;
  }
  if ((t = dict_find(iter, MESSAGE_KEY_WEEK_MINUTES))) {
    s_week_minutes = (int)t->value->int32;
    persist_write_int(PERSIST_KEY_WEEK_MINUTES, s_week_minutes);
    stats_changed = true;
  }
  if ((t = dict_find(iter, MESSAGE_KEY_RECORDING_STATE))) {
    bool now = t->value->int32 != 0;
    if (now != s_recording) {
      s_recording = now;
      // Anchor the on-watch REC timer to the moment we learned about the
      // transition. We don't know the exact android-side start instant, but
      // the message round-trip is short enough (~tens of ms) that the watch
      // timer matches the audio file's wall-clock duration to within a beat.
      s_rec_start_ts = now ? time(NULL) : 0;
      if (s_rec_indicator_layer) layer_mark_dirty(s_rec_indicator_layer);
      update_rec_text();
    }
  }
  if (stats_changed) update_stats_text();
}

// === UI text builders ====================================================

static void update_action_text(void) {
  if (s_edit_mode) {
    snprintf(s_action_text, sizeof(s_action_text), "+/- to set BPM");
  } else {
    snprintf(s_action_text, sizeof(s_action_text),
             s_ticking ? "SELECT to stop" : "SELECT to start");
  }
  text_layer_set_text(s_action_layer, s_action_text);
}

static void update_bpm_label(void) {
  // The label under the BPM number doubles as the mute indicator. Keeping
  // it in the existing layer avoids juggling another text layer for a
  // status that the user only needs to glance at.
  text_layer_set_text(s_bpm_label_layer, s_vibe_muted ? "BPM · silent" : "BPM");
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
  // The REC label tracks the *audio recording* lifecycle, not app-open time.
  // When the android side reports recording stopped, this freezes to "REC off"
  // so the watch UI matches the red square's state — previously the timer
  // kept counting even after DOWN stopped the recording, which read as a bug.
  if (s_recording && s_rec_start_ts > 0) {
    int rec_secs = (int)(time(NULL) - s_rec_start_ts);
    char rec[10];
    format_mmss(rec_secs, rec, sizeof(rec));
    snprintf(s_rec_text, sizeof(s_rec_text), "REC %s", rec);
  } else {
    snprintf(s_rec_text, sizeof(s_rec_text), "REC off");
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
  update_bpm_label();
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

  // Discoverability hint for the help screen — small + dim so it doesn't
  // compete with the action prompt above it, but always present so users
  // who don't read this comment can still find their way to the guide.
  // TODO: remove this on-screen hint once the app is published — the
  // gesture will live in the Pebble Android app / app-store listing
  // description instead, keeping the watch UI uncluttered for regular use.
  s_help_hint_layer = text_layer_create(GRect(0, 160, b.size.w, 18));
  text_layer_set_background_color(s_help_hint_layer, GColorClear);
  text_layer_set_text_color(s_help_hint_layer, GColorDarkGray);
  text_layer_set_font(s_help_hint_layer,
                      fonts_get_system_font(FONT_KEY_GOTHIC_14));
  text_layer_set_text_alignment(s_help_hint_layer, GTextAlignmentCenter);
  text_layer_set_text(s_help_hint_layer, "UP+SELECT hold · help");
  layer_add_child(s_root_layer, text_layer_get_layer(s_help_hint_layer));

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

  // Solid red square in the top-right corner — only painted when the Android
  // side reports MediaRecorder is actively running. Sized to be unambiguous
  // at a glance without crowding the BPM number.
  s_rec_indicator_layer = layer_create(GRect(b.size.w - 18, 6, 12, 12));
  layer_set_update_proc(s_rec_indicator_layer, rec_indicator_update_proc);
  layer_add_child(s_root_layer, s_rec_indicator_layer);

  update_bpm_text();
  update_action_text();
  update_stats_text();
  update_rec_text();
}

static void window_unload(Window *window) {
  text_layer_destroy(s_bpm_layer);
  text_layer_destroy(s_bpm_label_layer);
  text_layer_destroy(s_action_layer);
  text_layer_destroy(s_help_hint_layer);
  text_layer_destroy(s_stats_layer);
  text_layer_destroy(s_rec_layer);
  layer_destroy(s_beat_dots_layer);
  layer_destroy(s_rec_indicator_layer);
}

static void init(void) {
  s_app_start_ts = time(NULL);
  s_current_bpm = persist_exists(PERSIST_KEY_LAST_BPM)
                      ? persist_read_int(PERSIST_KEY_LAST_BPM)
                      : DEFAULT_BPM;
  if (s_current_bpm < MIN_BPM || s_current_bpm > MAX_BPM) {
    s_current_bpm = DEFAULT_BPM;
  }
  // Seed today/week from cache so the stats line renders immediately on
  // open. Fresh values from Android arrive a beat later and overwrite both
  // the in-memory value and the persisted one.
  if (persist_exists(PERSIST_KEY_TODAY_MINUTES)) {
    s_today_minutes = persist_read_int(PERSIST_KEY_TODAY_MINUTES);
  }
  if (persist_exists(PERSIST_KEY_WEEK_MINUTES)) {
    s_week_minutes = persist_read_int(PERSIST_KEY_WEEK_MINUTES);
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
  if (s_help_window) window_destroy(s_help_window);
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
