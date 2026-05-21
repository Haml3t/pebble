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
  TEX_FLOOR,       // floor flat (FLOOR4_1, 64x64) — drawn under the horizon
  TEX_CEIL,        // ceiling flat (CEIL3_5, 64x64) — drawn above the horizon
  TEX_COUNT
} tex_id_t;

typedef struct {
  bool fire;          // SELECT this frame (edge-triggered)
  bool up_held;       // continuous walk forward
  bool down_held;     // continuous walk back
  bool back_held;     // strafe modifier
  // raw accel X-axis = side-to-side wrist roll. Engine integrates this
  // into the turn rate (left tilt -> turn left, right tilt -> turn right).
  // The previous version used Y (pitch), which the player kept hitting
  // while reaching to press buttons — felt jittery and unintentional.
  int16_t accel_x;
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
  uint8_t player_health;
  uint8_t kills;
  uint16_t last_render_us;
} engine_debug_t;
void engine_get_debug(engine_debug_t *out);

// Player health checks. main.c polls this to drive the STAGE_DEAD UI;
// it doesn't otherwise need to know the engine's internal state.
bool engine_player_dead(void);
