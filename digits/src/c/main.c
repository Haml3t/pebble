#include <pebble.h>

// Digits — jot down a phone number on the watch when no phone is handy.
//
// Pure on-watch app: no PebbleKit JS, no Android companion, no AppMessage.
// Three screens:
//   * List   — saved numbers + a "+ New number" row (MenuLayer).
//   * Entry  — pick one digit at a time with UP/DOWN, SELECT locks & advances.
//   * Review — navigate a saved number, modify a digit in place, or
//              delete-shift a digit (drops back into Entry to refill the tail).
//
// v1 is US-only: a constant "1" country-code prefix (display only, not
// entered) + 10 entered digits, formatted as  1 (AAA) PPP-LLLL.

#define MAX_NUMBERS 20
#define NUM_DIGITS  10

// Persist layout: a count, then one 10-byte blob per saved number.
#define PERSIST_KEY_COUNT     1
#define PERSIST_KEY_NUM_BASE  100   // number i lives at key 100 + i

// Button feel. Repeat rate matches metronome's ±BPM feel; long-press
// thresholds are short enough to feel deliberate without "did I miss it?".
#define PICK_REPEAT_MS    150
#define BACK_EXIT_HOLD_MS 500
#define SEL_DEL_HOLD_MS   500

// Digit-row geometry (200px-wide emery display). One row: a "1" prefix cell,
// then 10 digit cells in 3-3-4 visual groups separated by GROUP_GAP.
#define LAYOUT_X0   5
#define CELL_W      15
#define CELL_H      30
#define PREFIX_GAP  9
#define GROUP_GAP   8

// ---- model ---------------------------------------------------------------

static uint8_t s_numbers[MAX_NUMBERS][NUM_DIGITS];  // each byte 0..9
static int     s_count = 0;

// Entry buffer.
static uint8_t s_entry[NUM_DIGITS];
static int     s_entry_pos = 0;     // 0..NUM_DIGITS (next slot to fill)
static int     s_cur_digit = 0;     // 0..9 currently being scrolled
static int     s_edit_target = -1;  // index being refilled after a delete-shift, else -1

// Review state.
static int  s_review_index = -1;
static int  s_review_cursor = 0;    // 0..NUM_DIGITS-1
static bool s_review_editing = false;
static uint8_t s_review_edit_orig = 0;  // value to restore if an edit is cancelled

// List delete-confirm target.
static int s_pending_delete = -1;

// ---- windows / layers ----------------------------------------------------

static Window    *s_list_window;
static MenuLayer *s_menu_layer;

static Window *s_entry_window;
static Layer  *s_entry_layer;

static Window *s_review_window;
static Layer  *s_review_layer;

static Window    *s_confirm_window;
static TextLayer *s_confirm_num_layer;
static TextLayer *s_confirm_prompt_layer;
static TextLayer *s_confirm_hint_layer;
static char       s_confirm_num_text[24];

// ---- forward decls -------------------------------------------------------

static void open_review(int idx);
static void start_new_entry(void);

// ---- persistence ---------------------------------------------------------

static void persist_save_one(int idx) {
  persist_write_int(PERSIST_KEY_COUNT, s_count);
  persist_write_data(PERSIST_KEY_NUM_BASE + idx, s_numbers[idx], NUM_DIGITS);
}

static void load_all(void) {
  s_count = persist_exists(PERSIST_KEY_COUNT) ? persist_read_int(PERSIST_KEY_COUNT) : 0;
  if (s_count < 0) s_count = 0;
  if (s_count > MAX_NUMBERS) s_count = MAX_NUMBERS;
  for (int i = 0; i < s_count; i++) {
    if (persist_exists(PERSIST_KEY_NUM_BASE + i)) {
      persist_read_data(PERSIST_KEY_NUM_BASE + i, s_numbers[i], NUM_DIGITS);
    }
  }
}

static void delete_number(int idx) {
  if (idx < 0 || idx >= s_count) return;
  for (int k = idx; k < s_count - 1; k++) {
    memcpy(s_numbers[k], s_numbers[k + 1], NUM_DIGITS);
  }
  s_count--;
  persist_write_int(PERSIST_KEY_COUNT, s_count);
  for (int k = idx; k < s_count; k++) {
    persist_write_data(PERSIST_KEY_NUM_BASE + k, s_numbers[k], NUM_DIGITS);
  }
  persist_delete(PERSIST_KEY_NUM_BASE + s_count);  // drop the now-trailing key
}

// ---- formatting ----------------------------------------------------------

// Full formatted number, e.g. "1 (415) 555-0192".
static void format_full(const uint8_t *d, char *out, size_t out_sz) {
  snprintf(out, out_sz, "1 (%d%d%d) %d%d%d-%d%d%d%d",
           d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], d[8], d[9]);
}

// X-origin of digit cell i within the row (i in 0..9).
static int digit_cell_x(int i) {
  int x = LAYOUT_X0 + CELL_W + PREFIX_GAP;  // skip the "1" prefix cell
  for (int k = 0; k < i; k++) {
    x += CELL_W;
    if (k == 2 || k == 5) x += GROUP_GAP;   // gaps after area code and prefix groups
  }
  return x;
}

// Draws the "1 (AAA) PPP-LLLL" row as individual cells so the active digit
// can be highlighted precisely. `filled` = how many leading digits are real;
// `active` = which cell to highlight (-1 for none); `editing` tints the
// highlight orange (value being changed) vs blue (just selected).
static void draw_digit_row(GContext *ctx, int row_y, const uint8_t *d,
                           int filled, int active, bool editing) {
  GFont f = fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);
  GRect text_box;

  // Leading constant "1".
  graphics_context_set_text_color(ctx, GColorWhite);
  text_box = GRect(LAYOUT_X0, row_y - 4, CELL_W, CELL_H);
  graphics_draw_text(ctx, "1", f, text_box, GTextOverflowModeFill,
                     GTextAlignmentCenter, NULL);

  for (int i = 0; i < NUM_DIGITS; i++) {
    int x = digit_cell_x(i);
    bool is_active = (i == active);

    if (is_active) {
      GColor accent = editing ? GColorOrange : GColorVividCerulean;
      graphics_context_set_fill_color(ctx, accent);
      graphics_fill_rect(ctx, GRect(x - 1, row_y, CELL_W + 2, CELL_H), 3, GCornersAll);
    }

    text_box = GRect(x, row_y - 4, CELL_W, CELL_H);
    if (i < filled) {
      char c[2] = { (char)('0' + d[i]), 0 };
      graphics_context_set_text_color(ctx, is_active ? GColorBlack : GColorWhite);
      graphics_draw_text(ctx, c, f, text_box, GTextOverflowModeFill,
                         GTextAlignmentCenter, NULL);
    } else {
      // Empty slot: a baseline underscore.
      graphics_context_set_stroke_color(ctx, is_active ? GColorBlack : GColorDarkGray);
      int uy = row_y + CELL_H - 7;
      graphics_draw_line(ctx, GPoint(x + 2, uy), GPoint(x + CELL_W - 3, uy));
    }
  }
}

// ==========================================================================
// List window (home)
// ==========================================================================

static uint16_t menu_get_num_rows(MenuLayer *menu, uint16_t section, void *ctx) {
  return s_count + 1;  // saved numbers + "+ New number"
}

static void menu_draw_row(GContext *ctx, const Layer *cell, MenuIndex *idx, void *context) {
  if (idx->row < s_count) {
    char buf[24];
    format_full(s_numbers[idx->row], buf, sizeof(buf));
    menu_cell_basic_draw(ctx, cell, buf, NULL, NULL);
  } else {
    menu_cell_basic_draw(ctx, cell, "+ New number", NULL, NULL);
  }
}

static void menu_select(MenuLayer *menu, MenuIndex *idx, void *context) {
  if (idx->row < s_count) {
    open_review(idx->row);
  } else {
    start_new_entry();
  }
}

static void menu_select_long(MenuLayer *menu, MenuIndex *idx, void *context) {
  if (idx->row < s_count) {
    s_pending_delete = idx->row;
    format_full(s_numbers[idx->row], s_confirm_num_text, sizeof(s_confirm_num_text));
    text_layer_set_text(s_confirm_num_layer, s_confirm_num_text);
    window_stack_push(s_confirm_window, true);
  }
}

static void list_window_appear(Window *window) {
  menu_layer_reload_data(s_menu_layer);
}

static void list_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

  s_menu_layer = menu_layer_create(bounds);
  menu_layer_set_callbacks(s_menu_layer, NULL, (MenuLayerCallbacks) {
    .get_num_rows = menu_get_num_rows,
    .draw_row = menu_draw_row,
    .select_click = menu_select,
    .select_long_click = menu_select_long,
  });
  menu_layer_set_normal_colors(s_menu_layer, GColorBlack, GColorWhite);
  menu_layer_set_highlight_colors(s_menu_layer, GColorVividCerulean, GColorWhite);
  menu_layer_set_click_config_onto_window(s_menu_layer, window);
  layer_add_child(root, menu_layer_get_layer(s_menu_layer));
}

static void list_window_unload(Window *window) {
  menu_layer_destroy(s_menu_layer);
}

// ==========================================================================
// Entry window
// ==========================================================================

static void commit_entry(void) {
  int idx;
  if (s_edit_target >= 0) {
    idx = s_edit_target;
  } else {
    if (s_count >= MAX_NUMBERS) {       // list full — bail, no add
      vibes_double_pulse();
      s_edit_target = -1;
      window_stack_pop(true);
      return;
    }
    idx = s_count;
    s_count++;
  }
  memcpy(s_numbers[idx], s_entry, NUM_DIGITS);
  persist_save_one(idx);
  s_edit_target = -1;

  window_stack_pop(false);  // back to list (no animation under the push below)
  open_review(idx);
}

static void start_new_entry(void) {
  s_edit_target = -1;
  s_entry_pos = 0;
  s_cur_digit = 0;
  memset(s_entry, 0, NUM_DIGITS);
  window_stack_push(s_entry_window, true);
}

static void entry_up(ClickRecognizerRef rec, void *ctx) {
  s_cur_digit = (s_cur_digit + 1) % 10;
  layer_mark_dirty(s_entry_layer);
}

static void entry_down(ClickRecognizerRef rec, void *ctx) {
  s_cur_digit = (s_cur_digit + 9) % 10;
  layer_mark_dirty(s_entry_layer);
}

static void entry_select(ClickRecognizerRef rec, void *ctx) {
  s_entry[s_entry_pos] = s_cur_digit;
  s_entry_pos++;
  if (s_entry_pos >= NUM_DIGITS) {
    commit_entry();
    return;
  }
  s_cur_digit = s_entry[s_entry_pos];  // preload forward value (0 for fresh slots)
  layer_mark_dirty(s_entry_layer);
}

static void entry_back(ClickRecognizerRef rec, void *ctx) {
  if (s_entry_pos > 0) {
    s_entry_pos--;
    s_cur_digit = s_entry[s_entry_pos];  // re-pick the erased digit
    layer_mark_dirty(s_entry_layer);
  } else {
    window_stack_pop(true);  // nothing entered → exit to list
  }
}

static void entry_back_long(ClickRecognizerRef rec, void *ctx) {
  window_stack_pop(true);  // bail to list from anywhere
}

static void entry_click_config(void *context) {
  window_single_repeating_click_subscribe(BUTTON_ID_UP, PICK_REPEAT_MS, entry_up);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, PICK_REPEAT_MS, entry_down);
  window_single_click_subscribe(BUTTON_ID_SELECT, entry_select);
  window_single_click_subscribe(BUTTON_ID_BACK, entry_back);
  window_long_click_subscribe(BUTTON_ID_BACK, BACK_EXIT_HOLD_MS, entry_back_long, NULL);
}

static void entry_update(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, b, 0, GCornerNone);

  // Big current digit.
  char big[2] = { (char)('0' + s_cur_digit), 0 };
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, big, fonts_get_system_font(FONT_KEY_LECO_42_NUMBERS),
                     GRect(0, 6, b.size.w, 50), GTextOverflowModeFill,
                     GTextAlignmentCenter, NULL);

  // Digit row, with the active slot previewing the scrolled value.
  uint8_t tmp[NUM_DIGITS];
  memcpy(tmp, s_entry, NUM_DIGITS);
  int filled = s_entry_pos;
  if (s_entry_pos < NUM_DIGITS) {
    tmp[s_entry_pos] = s_cur_digit;
    filled = s_entry_pos + 1;
  }
  draw_digit_row(ctx, 88, tmp, filled, s_entry_pos, false);

  // Progress.
  char prog[16];
  snprintf(prog, sizeof(prog), "%d of %d", s_entry_pos, NUM_DIGITS);
  graphics_context_set_text_color(ctx, GColorLightGray);
  graphics_draw_text(ctx, prog, fonts_get_system_font(FONT_KEY_GOTHIC_18),
                     GRect(0, 132, b.size.w, 22), GTextOverflowModeFill,
                     GTextAlignmentCenter, NULL);

  // Hint.
  graphics_draw_text(ctx, "UP/DN pick   SEL lock\nBACK erase  hold=exit",
                     fonts_get_system_font(FONT_KEY_GOTHIC_14),
                     GRect(4, 184, b.size.w - 8, 44), GTextOverflowModeWordWrap,
                     GTextAlignmentCenter, NULL);
}

static void entry_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  s_entry_layer = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_entry_layer, entry_update);
  layer_add_child(root, s_entry_layer);
}

static void entry_window_unload(Window *window) {
  layer_destroy(s_entry_layer);
}

// ==========================================================================
// Review window
// ==========================================================================

static void open_review(int idx) {
  s_review_index = idx;
  s_review_cursor = 0;
  s_review_editing = false;
  window_stack_push(s_review_window, true);
}

static void review_up(ClickRecognizerRef rec, void *ctx) {
  if (s_review_editing) {
    uint8_t *v = &s_numbers[s_review_index][s_review_cursor];
    *v = (*v + 1) % 10;
  } else {
    s_review_cursor = (s_review_cursor + NUM_DIGITS - 1) % NUM_DIGITS;  // move left
  }
  layer_mark_dirty(s_review_layer);
}

static void review_down(ClickRecognizerRef rec, void *ctx) {
  if (s_review_editing) {
    uint8_t *v = &s_numbers[s_review_index][s_review_cursor];
    *v = (*v + 9) % 10;
  } else {
    s_review_cursor = (s_review_cursor + 1) % NUM_DIGITS;  // move right
  }
  layer_mark_dirty(s_review_layer);
}

static void review_select(ClickRecognizerRef rec, void *ctx) {
  if (s_review_editing) {
    s_review_editing = false;             // commit
    persist_save_one(s_review_index);
  } else {
    s_review_editing = true;              // start editing this digit
    s_review_edit_orig = s_numbers[s_review_index][s_review_cursor];
  }
  layer_mark_dirty(s_review_layer);
}

// Long-press SELECT = delete-shift: drop the highlighted digit, shift the rest
// left into the entry buffer (9 digits), and refill the freed tail slot. The
// stored number is left untouched until Entry commits, so backing out is safe.
static void review_select_long(ClickRecognizerRef rec, void *ctx) {
  if (s_review_editing) return;

  memset(s_entry, 0, NUM_DIGITS);
  int j = 0;
  for (int k = 0; k < NUM_DIGITS; k++) {
    if (k != s_review_cursor) s_entry[j++] = s_numbers[s_review_index][k];
  }
  s_entry_pos = NUM_DIGITS - 1;   // slot 9 needs refilling
  s_cur_digit = 0;
  s_edit_target = s_review_index;

  window_stack_pop(false);                 // leave review
  window_stack_push(s_entry_window, true); // refill in entry
}

static void review_back(ClickRecognizerRef rec, void *ctx) {
  if (s_review_editing) {
    s_numbers[s_review_index][s_review_cursor] = s_review_edit_orig;  // cancel edit
    s_review_editing = false;
    layer_mark_dirty(s_review_layer);
  } else {
    window_stack_pop(true);  // back to list
  }
}

static void review_click_config(void *context) {
  window_single_repeating_click_subscribe(BUTTON_ID_UP, PICK_REPEAT_MS, review_up);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, PICK_REPEAT_MS, review_down);
  window_single_click_subscribe(BUTTON_ID_SELECT, review_select);
  window_long_click_subscribe(BUTTON_ID_SELECT, SEL_DEL_HOLD_MS, review_select_long, NULL);
  window_single_click_subscribe(BUTTON_ID_BACK, review_back);
}

static void review_update(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, b, 0, GCornerNone);

  graphics_context_set_text_color(ctx, GColorLightGray);
  graphics_draw_text(ctx, s_review_editing ? "Editing digit" : "Review",
                     fonts_get_system_font(FONT_KEY_GOTHIC_18),
                     GRect(0, 14, b.size.w, 24), GTextOverflowModeFill,
                     GTextAlignmentCenter, NULL);

  draw_digit_row(ctx, 78, s_numbers[s_review_index], NUM_DIGITS,
                 s_review_cursor, s_review_editing);

  const char *hint = s_review_editing
      ? "UP/DN change   SEL keep\nBACK cancel"
      : "UP/DN move   SEL edit\nhold SEL delete  BACK done";
  graphics_context_set_text_color(ctx, GColorLightGray);
  graphics_draw_text(ctx, hint, fonts_get_system_font(FONT_KEY_GOTHIC_14),
                     GRect(4, 178, b.size.w - 8, 50), GTextOverflowModeWordWrap,
                     GTextAlignmentCenter, NULL);
}

static void review_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  s_review_layer = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_review_layer, review_update);
  layer_add_child(root, s_review_layer);
}

static void review_window_unload(Window *window) {
  layer_destroy(s_review_layer);
}

// ==========================================================================
// Delete-confirm window
// ==========================================================================

static void confirm_select(ClickRecognizerRef rec, void *ctx) {
  if (s_pending_delete >= 0) {
    delete_number(s_pending_delete);
    s_pending_delete = -1;
  }
  window_stack_pop(true);  // back to list (reloads on appear)
}

static void confirm_back(ClickRecognizerRef rec, void *ctx) {
  s_pending_delete = -1;
  window_stack_pop(true);
}

static void confirm_click_config(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, confirm_select);
  window_single_click_subscribe(BUTTON_ID_BACK, confirm_back);
}

static void confirm_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect b = layer_get_bounds(root);
  window_set_background_color(window, GColorBlack);

  s_confirm_prompt_layer = text_layer_create(GRect(6, 30, b.size.w - 12, 30));
  text_layer_set_background_color(s_confirm_prompt_layer, GColorClear);
  text_layer_set_text_color(s_confirm_prompt_layer, GColorWhite);
  text_layer_set_text_alignment(s_confirm_prompt_layer, GTextAlignmentCenter);
  text_layer_set_font(s_confirm_prompt_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_text(s_confirm_prompt_layer, "Delete?");
  layer_add_child(root, text_layer_get_layer(s_confirm_prompt_layer));

  s_confirm_num_layer = text_layer_create(GRect(6, 90, b.size.w - 12, 30));
  text_layer_set_background_color(s_confirm_num_layer, GColorClear);
  text_layer_set_text_color(s_confirm_num_layer, GColorVividCerulean);
  text_layer_set_text_alignment(s_confirm_num_layer, GTextAlignmentCenter);
  text_layer_set_font(s_confirm_num_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  layer_add_child(root, text_layer_get_layer(s_confirm_num_layer));

  s_confirm_hint_layer = text_layer_create(GRect(6, 170, b.size.w - 12, 50));
  text_layer_set_background_color(s_confirm_hint_layer, GColorClear);
  text_layer_set_text_color(s_confirm_hint_layer, GColorLightGray);
  text_layer_set_text_alignment(s_confirm_hint_layer, GTextAlignmentCenter);
  text_layer_set_font(s_confirm_hint_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18));
  text_layer_set_text(s_confirm_hint_layer, "SEL delete   BACK keep");
  layer_add_child(root, text_layer_get_layer(s_confirm_hint_layer));
}

static void confirm_window_unload(Window *window) {
  text_layer_destroy(s_confirm_prompt_layer);
  text_layer_destroy(s_confirm_num_layer);
  text_layer_destroy(s_confirm_hint_layer);
}

// ==========================================================================
// App lifecycle
// ==========================================================================

static void init(void) {
  load_all();

  s_list_window = window_create();
  window_set_window_handlers(s_list_window, (WindowHandlers) {
    .load = list_window_load,
    .appear = list_window_appear,
    .unload = list_window_unload,
  });

  s_entry_window = window_create();
  window_set_background_color(s_entry_window, GColorBlack);
  window_set_click_config_provider(s_entry_window, entry_click_config);
  window_set_window_handlers(s_entry_window, (WindowHandlers) {
    .load = entry_window_load,
    .unload = entry_window_unload,
  });

  s_review_window = window_create();
  window_set_background_color(s_review_window, GColorBlack);
  window_set_click_config_provider(s_review_window, review_click_config);
  window_set_window_handlers(s_review_window, (WindowHandlers) {
    .load = review_window_load,
    .unload = review_window_unload,
  });

  s_confirm_window = window_create();
  window_set_click_config_provider(s_confirm_window, confirm_click_config);
  window_set_window_handlers(s_confirm_window, (WindowHandlers) {
    .load = confirm_window_load,
    .unload = confirm_window_unload,
  });

  window_stack_push(s_list_window, true);
}

static void deinit(void) {
  window_destroy(s_list_window);
  window_destroy(s_entry_window);
  window_destroy(s_review_window);
  window_destroy(s_confirm_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
