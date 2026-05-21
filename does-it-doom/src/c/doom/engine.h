#pragma once

// Does It Doom? — raycaster engine.
//
// API contract: main.c calls engine_init() once at window load, then on
// every frame calls engine_tick(input) followed by engine_render(out).
// `out` is a width*height byte buffer in Pebble's AARRGGBB-2222 format,
// ready to memcpy into the OS framebuffer rows.
//
// The engine owns the texture cache and game state. main.c owns the
// shadow buffer (so the framebuffer allocator stays in one place).

#include <pebble.h>
#include <stdbool.h>
#include <stdint.h>

#define VIEWPORT_W 200
#define VIEWPORT_H 178   // 50px statbar lives in the remaining display area
#define STATBAR_H  50
#define SHADOW_W   VIEWPORT_W
#define SHADOW_H   (VIEWPORT_H + STATBAR_H)   // full display height

#define TILEMAP_W  32
#define TILEMAP_H  32
#define TEXTURE_DIM 64

typedef enum {
  TEX_NONE = 0,
  TEX_STARTAN3,    // primary wall texture (128x128 composite, real E1M1 wall)
  TEX_NUKAGE1,     // green liquid (used for interior decoration walls)
  TEX_STEP1,       // gray step (used for doorway pillars)
  TEX_COUNT
} tex_id_t;

typedef struct {
  bool fire;          // SELECT this frame (edge-triggered)
  bool up_held;       // continuous walk forward
  bool down_held;     // continuous walk back
  bool back_held;     // strafe modifier
  int16_t accel_y;    // raw accel Y; engine integrates into turn rate
} engine_input_t;

void engine_init(void);

// One tick: advance player position/angle based on input, step any
// world animation (none yet in v0).
void engine_tick(const engine_input_t *in);

// Render one frame into `out` (size SHADOW_W * SHADOW_H bytes — full
// display: 3D viewport on top, statbar below).
void engine_render(uint8_t *out);

// Debug snapshot for PERF logs.
typedef struct {
  int player_x_q8;    // Q8.8 player x
  int player_y_q8;
  int player_angle_deg;
  uint8_t current_tile;
  uint16_t last_render_us;
} engine_debug_t;
void engine_get_debug(engine_debug_t *out);
