#include "engine.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

// M_PI is a glibc extension; Pebble's SDK uses -std=c99 which omits it.
#ifndef M_PI
#define M_PI    3.14159265358979323846f
#endif
#ifndef M_PI_2
#define M_PI_2  1.57079632679489661923f
#endif
#define TWO_PI  (2.0f * (float)M_PI)

// Sanitize any float that's about to feed sinf/cosf. Pebble's softfloat
// __ieee754_rem_pio2f crashes when given values much larger than 2π — even
// finite ones, due to a buggy/undersized large-argument-reduction path.
// We've seen this fault repeatedly at PC=0x2030 in production on the
// emery watch. Calling this just before every trig call costs ~5 cycles
// but eliminates a whole class of crashes (stack-overflow corruption,
// uninitialized float, NaN/Inf propagation).
static inline float sanitize_angle(float a) {
  if (!isfinite(a)) return M_PI_2;          // NaN/Inf -> reset to south
  while (a < 0)        a += TWO_PI;
  while (a >= TWO_PI)  a -= TWO_PI;
  return a;
}

// === Sin/cos lookup table =================================================
//
// Pebble's libc sinf/cosf go through __ieee754_rem_pio2f, which has been the
// fault PC every time we've crashed in production (PC=0x2030, LR=0x2021).
// Eliminating the libc trig call removes the entire crash vector. A 512-
// entry LUT (4 KB total for sin+cos) has ~0.7° resolution — plenty for a
// raycaster where the next visible quantization is the column step (60° /
// 200 cols = 0.3° per column).
//
// The LUT is built ONCE at engine_init using libc sinf/cosf with strictly
// bounded inputs i*TWO_PI/512. Game runtime never calls libc trig again.

#define LUT_RES   512
#define LUT_MASK  (LUT_RES - 1)
#define LUT_SCALE (LUT_RES / TWO_PI)
static float s_sin_lut[LUT_RES];
static float s_cos_lut[LUT_RES];

static void init_trig_lut(void) {
  // Load the precomputed LUT from Pebble resources. We cannot compute it
  // on-watch: Pebble's emery libc sinf/cosf both crash inside
  // __ieee754_rem_pio2f (confirmed at PCs 0x2030 and 0x2194). The resource
  // contains 512 little-endian floats of sin, then 512 of cos, matching
  // the layout this code expects (no header, just raw floats).
  ResHandle h = resource_get_handle(RESOURCE_ID_TRIG_LUT);
  size_t sz = resource_size(h);
  size_t expected = sizeof(s_sin_lut) + sizeof(s_cos_lut);
  if (sz != expected) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "trig_lut size %u expected %u",
            (unsigned)sz, (unsigned)expected);
    return;
  }
  resource_load_byte_range(h, 0, (uint8_t *)s_sin_lut, sizeof(s_sin_lut));
  resource_load_byte_range(h, sizeof(s_sin_lut),
                           (uint8_t *)s_cos_lut, sizeof(s_cos_lut));
}

static inline int angle_to_lut_idx(float a) {
  a = sanitize_angle(a);
  int idx = (int)(a * LUT_SCALE);
  return idx & LUT_MASK;
}

static inline float lut_sin(float a) { return s_sin_lut[angle_to_lut_idx(a)]; }
static inline float lut_cos(float a) { return s_cos_lut[angle_to_lut_idx(a)]; }

// === Tunables =============================================================

#define MOVE_SPEED   0.06f      // tiles/tick at FRAME_MS=50 -> 1.2 tiles/sec
#define STRAFE_SPEED 0.045f
#define TURN_DEADZONE_MG 100    // accel Y below this -> no turning
#define TURN_GAIN    0.00015f   // rad/(mg*tick); ~9°/sec at 1g tilt
#define FOV_RAD      1.047f     // 60° horizontal FOV (Doom's classic)
#define MAX_RAY_STEPS 24        // far-clip; tilemap is 32 wide so plenty

// === Hardcoded v0 tilemap (E1M1 starting room quote) =====================
//
// 32x32 grid. Each byte is a tex_id_t; 0 = empty space, others are wall
// texture IDs. The map is a 10x10 room with a single doorway south that
// opens into a small corridor. Quotes E1M1's "hangar" feel.

static const uint8_t s_tilemap[TILEMAP_H][TILEMAP_W] = {
  /* y=0 (north) */
  {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
  {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
  {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
  {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
  {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
  {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
  {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
  {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
  /* y=8: top wall of starting room */
  {0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0},
  {0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0},
  {0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0},
  {0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,2,2,0,0,0,0,0,0,0,0,0,0,1,0,0,0},
  {0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,2,2,0,0,0,0,0,0,0,0,0,0,1,0,0,0},
  {0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0},
  {0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0},
  {0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0},
  /* y=16: bottom wall of starting room, with south doorway at x=18..19 */
  {0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,0,0,1,1,1,1,1,1,1,1,1,0,0,0},
  {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,3,0,0,0,0,0,0,0,0,0,0,0,0},
  {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,3,0,0,0,0,0,0,0,0,0,0,0,0},
  {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,3,0,0,0,0,0,0,0,0,0,0,0,0},
  {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,3,0,0,0,0,0,0,0,0,0,0,0,0},
  {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,3,0,0,0,0,0,0,0,0,0,0,0,0},
  {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
  {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
  {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
  {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
  {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
  {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
  {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
  {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
  {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
  {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
};

// === Textures (loaded from Pebble resources at init) =====================
//
// Each texture is w*h bytes in AARRGGBB-2222 format
// (already palette-mapped by wad2res.py). The first 4 bytes of the resource
// file are u16 width, u16 height (always 64,64); we skip those and keep a
// pointer to the pixel data only.

// Textures are variable-size (flats are 64x64, wall TEXTUREs are 128x128).
// Each resource is u16 w, u16 h, then w*h bytes of AARRGGBB-2222 pixels.
typedef struct {
  uint16_t w, h;
  uint8_t *pixels;   // owned; malloc'd at engine_init.
} texture_t;

static texture_t s_textures[TEX_COUNT];

// === Sprites =============================================================
//
// Each sprite is loaded once at init from its Pebble resource and kept on
// the heap as { u16 w, u16 h, pixels[] }. Pixels are AARRGGBB-2222 with
// TRANSPARENT_BYTE (0x00) marking empty columns/posts (set by wad2res.py
// when decoding the Doom picture format).
//
// World-space sprite entity list is fixed for v0: one imp facing front,
// stationary, near the south doorway of the starting room.

#define TRANSPARENT_BYTE 0x00

typedef struct {
  uint16_t w, h;
  uint8_t *pixels;   // owned
} sprite_t;

typedef enum {
  SPR_IMP = 0,
  SPR_PISTOL,
  SPR_PISTOL_FIRE,    // PISFA0 — pistol with muzzle flash
  SPR_FACE,
  SPR_NUM0, SPR_NUM1, SPR_NUM2, SPR_NUM3, SPR_NUM4,
  SPR_NUM5, SPR_NUM6, SPR_NUM7, SPR_NUM8, SPR_NUM9,
  SPR_KIND_COUNT,
} sprite_kind_t;

static sprite_t s_sprites[SPR_KIND_COUNT];

static const uint32_t s_spr_resource_ids[SPR_KIND_COUNT] = {
  RESOURCE_ID_SPR_TROOA1,
  RESOURCE_ID_SPR_PISGA0,
  RESOURCE_ID_SPR_PISFA0,
  RESOURCE_ID_SPR_STFST01,
  RESOURCE_ID_SPR_STTNUM0, RESOURCE_ID_SPR_STTNUM1, RESOURCE_ID_SPR_STTNUM2,
  RESOURCE_ID_SPR_STTNUM3, RESOURCE_ID_SPR_STTNUM4, RESOURCE_ID_SPR_STTNUM5,
  RESOURCE_ID_SPR_STTNUM6, RESOURCE_ID_SPR_STTNUM7, RESOURCE_ID_SPR_STTNUM8,
  RESOURCE_ID_SPR_STTNUM9,
};

static void load_sprite(int kind, uint32_t res_id) {
  ResHandle h = resource_get_handle(res_id);
  size_t sz = resource_size(h);
  if (sz < 4) return;
  uint8_t header[4];
  resource_load_byte_range(h, 0, header, 4);
  uint16_t w = header[0] | (header[1] << 8);
  uint16_t hgt = header[2] | (header[3] << 8);
  size_t px_bytes = (size_t)w * hgt;
  if (sz != 4 + px_bytes) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "spr %d size mismatch", kind);
    return;
  }
  s_sprites[kind].pixels = malloc(px_bytes);
  if (!s_sprites[kind].pixels) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "spr %d malloc fail", kind);
    return;
  }
  s_sprites[kind].w = w;
  s_sprites[kind].h = hgt;
  resource_load_byte_range(h, 4, s_sprites[kind].pixels, px_bytes);
}

static void load_sprites(void) {
  memset(s_sprites, 0, sizeof(s_sprites));
  for (int i = 0; i < SPR_KIND_COUNT; i++)
    load_sprite(i, s_spr_resource_ids[i]);
}

// World entity. Sprite_kind selects which bitmap; position is in tiles.
typedef struct {
  float x, y;
  sprite_kind_t kind;
  bool alive;
} entity_t;

#define MAX_ENTITIES 8
static entity_t s_entities[MAX_ENTITIES];

static void entities_init(void) {
  memset(s_entities, 0, sizeof(s_entities));
  // Imp directly south of player so it appears centered in initial view.
  s_entities[0] = (entity_t){ .x = 18.5f, .y = 14.5f, .kind = SPR_IMP, .alive = true };
  // Second one further south past the doorway — tests depth-scaling.
  s_entities[1] = (entity_t){ .x = 19.5f, .y = 19.0f, .kind = SPR_IMP, .alive = true };
}

// Column z-buffer: perpendicular wall distance per column. Walls write
// it; sprite renderer reads it to mask occluded columns.
static float s_zbuffer[VIEWPORT_W];

static const uint32_t s_tex_resource_ids[TEX_COUNT] = {
  0,                              // TEX_NONE — never sampled
  RESOURCE_ID_WALL_STARTAN3,
  RESOURCE_ID_TEX_NUKAGE1,
  RESOURCE_ID_TEX_STEP1,
};

static void load_textures(void) {
  memset(s_textures, 0, sizeof(s_textures));
  for (int t = 1; t < TEX_COUNT; t++) {
    ResHandle h = resource_get_handle(s_tex_resource_ids[t]);
    size_t sz = resource_size(h);
    if (sz < 4) {
      APP_LOG(APP_LOG_LEVEL_WARNING, "tex %d resource size %u too small",
              t, (unsigned)sz);
      continue;
    }
    uint8_t header[4];
    resource_load_byte_range(h, 0, header, 4);
    uint16_t w = header[0] | (header[1] << 8);
    uint16_t hh = header[2] | (header[3] << 8);
    size_t px_bytes = (size_t)w * hh;
    if (sz != 4 + px_bytes) {
      APP_LOG(APP_LOG_LEVEL_WARNING, "tex %d size mismatch (res=%u w=%u h=%u)",
              t, (unsigned)sz, (unsigned)w, (unsigned)hh);
      continue;
    }
    s_textures[t].w = w;
    s_textures[t].h = hh;
    s_textures[t].pixels = malloc(px_bytes);
    if (!s_textures[t].pixels) {
      APP_LOG(APP_LOG_LEVEL_ERROR, "tex %d malloc(%u) failed, heap=%u",
              t, (unsigned)px_bytes, (unsigned)heap_bytes_free());
      continue;
    }
    resource_load_byte_range(h, 4, s_textures[t].pixels, px_bytes);
    APP_LOG(APP_LOG_LEVEL_INFO, "tex %d loaded %ux%u (%u B)",
            t, w, hh, (unsigned)px_bytes);
  }
}

// === Player ==============================================================

typedef struct {
  float x, y;        // world coordinates in tiles (player size 0, point-like)
  float angle;       // radians, 0 = +x axis (east)
  int16_t turn_baseline;  // accel-Y baseline for drift correction
  uint16_t turn_baseline_age_ticks;
  uint16_t fire_flash_ticks;  // remaining frames of muzzle-flash sprite
  uint16_t kills;             // bumped on each successful imp kill
} player_t;

#define FIRE_FLASH_FRAMES 5      // ~250ms at 50ms/frame; longer than v0's 3
                                 // so the flash is visible even if we drop
                                 // to ~8 fps on the watch.
#define FIRE_RANGE_TILES  10.0f  // hitscan max distance

static player_t s_player;

static void player_init(void) {
  // Center of the starting room (tile 18,12) facing south toward the doorway.
  s_player.x = 18.5f;
  s_player.y = 12.5f;
  s_player.angle = (float)M_PI_2;   // +y is south on the tilemap
  s_player.turn_baseline = 0;
  s_player.turn_baseline_age_ticks = 0;
}

static inline bool is_wall(int mx, int my) {
  if (mx < 0 || my < 0 || mx >= TILEMAP_W || my >= TILEMAP_H) return true;
  return s_tilemap[my][mx] != 0;
}

static inline uint8_t tile_at(int mx, int my) {
  if (mx < 0 || my < 0 || mx >= TILEMAP_W || my >= TILEMAP_H) return TEX_STARTAN3;
  return s_tilemap[my][mx];
}

static void player_move(float dx, float dy) {
  // Simple AABB-vs-grid collision: step each axis independently, reject if
  // destination tile is a wall. Player radius treated as 0 for v0.
  float nx = s_player.x + dx;
  if (!is_wall((int)nx, (int)s_player.y)) s_player.x = nx;
  float ny = s_player.y + dy;
  if (!is_wall((int)s_player.x, (int)ny)) s_player.y = ny;
}

// === Public API ==========================================================

static engine_debug_t s_debug;

void engine_init(void) {
  init_trig_lut();   // MUST be first — everything else may call lut_sin/cos
  memset(s_textures, 0, sizeof(s_textures));
  load_textures();
  load_sprites();
  entities_init();
  player_init();
  memset(&s_debug, 0, sizeof(s_debug));
}

// Hitscan: from (px, py) along the player's facing direction, return the
// index of the closest entity within FIRE_RANGE_TILES that's within a
// half-FOV cone, OR -1 if nothing hit. v0 cone test is a small lateral
// offset threshold against the ray (player aims at screen center, but the
// hitbox is the whole imp width, so we're forgiving).
static int hitscan(void) {
  float a = sanitize_angle(s_player.angle);
  s_player.angle = a;
  float fwd_x = lut_cos(a), fwd_y = lut_sin(a);
  int best = -1;
  float best_dist = FIRE_RANGE_TILES;
  for (int i = 0; i < MAX_ENTITIES; i++) {
    if (!s_entities[i].alive) continue;
    float rx = s_entities[i].x - s_player.x;
    float ry = s_entities[i].y - s_player.y;
    float along = fwd_x * rx + fwd_y * ry;
    if (along <= 0.0f || along > best_dist) continue;
    // Lateral offset (perpendicular distance) — keep within ~0.45 tile
    // half-width (imp is ~0.7 tile wide so this is forgiving).
    float right_x = -fwd_y, right_y = fwd_x;
    float lateral = right_x * rx + right_y * ry;
    if (lateral < -0.5f || lateral > 0.5f) continue;
    best = i;
    best_dist = along;
  }
  return best;
}

void engine_tick(const engine_input_t *in) {
  // Fire: if SELECT this frame, swap pistol to muzzle-flash for N frames
  // and run hitscan. Closest imp in cone dies on hit.
  if (in->fire && s_player.fire_flash_ticks == 0) {
    s_player.fire_flash_ticks = FIRE_FLASH_FRAMES;
    int hit = hitscan();
    if (hit >= 0) {
      s_entities[hit].alive = false;
      s_player.kills++;
      APP_LOG(APP_LOG_LEVEL_INFO, "kill entity=%d kills=%u",
              hit, (unsigned)s_player.kills);
    } else {
      APP_LOG(APP_LOG_LEVEL_DEBUG, "fire miss");
    }
  }
  if (s_player.fire_flash_ticks > 0) s_player.fire_flash_ticks--;

  // Turning: drift-corrected accel-Y. Baseline is rolling — if accel stays
  // near zero for ~1.5s we re-baseline (player repositioned their wrist).
  int dy = in->accel_y - s_player.turn_baseline;
  // Clamp accel-driven turn input to ±2000 mg before scaling. The raw accel
  // sensor can briefly spike to ±16000+ on a hard wrist-whip; without this
  // clamp dy*TURN_GAIN can overshoot 2π/frame and trigger the libc trig
  // crash described above.
  if (dy >  2000) dy =  2000;
  if (dy < -2000) dy = -2000;
  if (abs(dy) < TURN_DEADZONE_MG) {
    s_player.turn_baseline_age_ticks++;
    if (s_player.turn_baseline_age_ticks > 30) {
      s_player.turn_baseline = in->accel_y;
      s_player.turn_baseline_age_ticks = 0;
    }
  } else {
    s_player.turn_baseline_age_ticks = 0;
    s_player.angle += dy * TURN_GAIN;
  }
  // Keep angle in [0, 2π). MUST be a while loop, not a single if: aggressive
  // wrist-whip can push dy*TURN_GAIN past 2π in a single tick (max accel ~2g
  // → dy~4000 → angle delta ~0.6 rad normally, but a clamp failure or large
  // baseline drift can grow this), and a single wrap leaves angle unbounded.
  // Once |angle| reaches hundreds of radians, libc's __ieee754_rem_pio2f
  // (sinf/cosf reduction helper) crashes on Pebble's softfloat path.
  while (s_player.angle < 0)        s_player.angle += TWO_PI;
  while (s_player.angle >= TWO_PI)  s_player.angle -= TWO_PI;
  // Also clamp dy itself so a single frame can't blow past one wrap. Belt
  // and suspenders — even with the while loop above, capping per-frame turn
  // rate to ~17° prevents the player from feeling a "warp" on whip-tilt.

  // Movement — sanitize once more, since the same value will be reused
  // by engine_render's trig calls this frame.
  s_player.angle = sanitize_angle(s_player.angle);
  float fwd_x = lut_cos(s_player.angle), fwd_y = lut_sin(s_player.angle);
  float side_x = -fwd_y, side_y = fwd_x;
  float walk = 0.0f, strafe = 0.0f;
  if (in->back_held) {
    if (in->up_held)   strafe -= STRAFE_SPEED;   // BACK+UP strafes left
    if (in->down_held) strafe += STRAFE_SPEED;   // BACK+DOWN strafes right
  } else {
    if (in->up_held)   walk += MOVE_SPEED;
    if (in->down_held) walk -= MOVE_SPEED;
  }
  player_move(fwd_x * walk + side_x * strafe, fwd_y * walk + side_y * strafe);
}

void engine_render(uint8_t *out) {
  // v0: solid floor (lower half) and ceiling (upper half) colors as
  // placeholders. The textures will replace these once we have a span
  // renderer (v1+).
  // AARRGGBB-2222: alpha=0b11, then 2 bits each of R/G/B. For Doom-brown
  //   ceiling   alpha=3 R=1 G=0 B=0  -> 0b11_01_00_00 = 0xD0
  //   floor     alpha=3 R=2 G=1 B=0  -> 0b11_10_01_00 = 0xE4
  const uint8_t CEIL_COLOR  = 0xD0;
  const uint8_t FLOOR_COLOR = 0xE4;
  for (int y = 0; y < VIEWPORT_H / 2; y++)
    memset(out + y * VIEWPORT_W, CEIL_COLOR, VIEWPORT_W);
  for (int y = VIEWPORT_H / 2; y < VIEWPORT_H; y++)
    memset(out + y * VIEWPORT_W, FLOOR_COLOR, VIEWPORT_W);

  // DDA raycaster. Classic textbook: for each column compute a ray direction
  // and step the grid until we hit a wall, then draw a texture-scaled column.
  const float px = s_player.x, py = s_player.y;
  const float pa = sanitize_angle(s_player.angle);
  // Cache sin/cos once per frame. We use the precomputed LUT instead of
  // libc sinf/cosf to avoid the __ieee754_rem_pio2f crash bug on Pebble's
  // softfloat path. ~0.7° resolution — invisible at our column count.
  const float cos_pa_w = lut_cos(pa);
  const float sin_pa_w = lut_sin(pa);
  // tanf(constant) is itself a constant once optimized; but the compiler
  // doesn't always fold it, and tanf can be expensive. Hoist explicitly.
  static const float fov_half_tan = 0.5773502692f;   // tan(60°/2) = tan(π/6)
  // Pre-scaled side-offset components for the camera-plane vector.
  const float plane_dx = -sin_pa_w * fov_half_tan;
  const float plane_dy =  cos_pa_w * fov_half_tan;

  // Pre-init zbuffer to far. Columns without a wall hit keep this value
  // so sprites in empty space still appear (occlusion test == nothing).
  for (int col = 0; col < VIEWPORT_W; col++) s_zbuffer[col] = 1e30f;

  for (int col = 0; col < VIEWPORT_W; col++) {
    // camera_x in [-1, +1], offset of this column from center
    float camera_x = 2.0f * col / VIEWPORT_W - 1.0f;
    float ray_dx = cos_pa_w + plane_dx * camera_x;
    float ray_dy = sin_pa_w + plane_dy * camera_x;

    int map_x = (int)px;
    int map_y = (int)py;

    // delta_dist = how far the ray must travel to cross one full grid unit
    // in each axis. inf-protected by the fact that ray_d* near zero means
    // huge delta which we'll never reach within MAX_RAY_STEPS anyway.
    float delta_dist_x = (ray_dx == 0.0f) ? 1e30f : fabsf(1.0f / ray_dx);
    float delta_dist_y = (ray_dy == 0.0f) ? 1e30f : fabsf(1.0f / ray_dy);

    int step_x, step_y;
    float side_dist_x, side_dist_y;
    if (ray_dx < 0) { step_x = -1; side_dist_x = (px - map_x) * delta_dist_x; }
    else            { step_x =  1; side_dist_x = (map_x + 1.0f - px) * delta_dist_x; }
    if (ray_dy < 0) { step_y = -1; side_dist_y = (py - map_y) * delta_dist_y; }
    else            { step_y =  1; side_dist_y = (map_y + 1.0f - py) * delta_dist_y; }

    int side = 0;            // 0 = x-axis wall, 1 = y-axis wall
    uint8_t hit_tex = 0;
    int steps = 0;
    while (steps++ < MAX_RAY_STEPS) {
      if (side_dist_x < side_dist_y) {
        side_dist_x += delta_dist_x;
        map_x += step_x;
        side = 0;
      } else {
        side_dist_y += delta_dist_y;
        map_y += step_y;
        side = 1;
      }
      uint8_t t = tile_at(map_x, map_y);
      if (t != 0) { hit_tex = t; break; }
    }
    if (hit_tex == 0) continue;   // no wall in range — keep floor/ceiling

    // Perpendicular wall distance (standard raycaster formula — uses the
    // axis-aligned component to avoid fisheye at column edges).
    float perp_dist = (side == 0)
      ? (side_dist_x - delta_dist_x)
      : (side_dist_y - delta_dist_y);
    if (perp_dist < 0.05f) perp_dist = 0.05f;
    s_zbuffer[col] = perp_dist;

    int line_h = (int)(VIEWPORT_H / perp_dist);
    int draw_start = -line_h / 2 + VIEWPORT_H / 2;
    int draw_end   =  line_h / 2 + VIEWPORT_H / 2;
    if (draw_start < 0) draw_start = 0;
    if (draw_end > VIEWPORT_H) draw_end = VIEWPORT_H;

    texture_t *tex = (hit_tex < TEX_COUNT) ? &s_textures[hit_tex] : NULL;
    if (!tex || !tex->pixels) continue;
    int tw = tex->w, th = tex->h;

    // Texture U coordinate: where along the tile edge we hit.
    float wall_x = (side == 0)
      ? py + perp_dist * ray_dy
      : px + perp_dist * ray_dx;
    wall_x -= floorf(wall_x);
    int tex_x = (int)(wall_x * tw);
    if ((side == 0 && ray_dx > 0) || (side == 1 && ray_dy < 0))
      tex_x = tw - tex_x - 1;
    if (tex_x < 0) tex_x = 0;
    if (tex_x >= tw) tex_x = tw - 1;

    // Texture V step (Q16 fixed-point so we avoid a divide per row).
    uint32_t step = ((uint32_t)th << 16) / (line_h > 0 ? line_h : 1);
    uint32_t tex_pos = ((draw_start - VIEWPORT_H / 2 + line_h / 2)) * step;

    // Column write — sequential bytes per row in shadow buffer (stride
    // VIEWPORT_W). Hot loop; keep it tight.
    uint8_t *col_dst = out + draw_start * VIEWPORT_W + col;
    const uint8_t *col_src_base = &tex->pixels[tex_x];   // tex column base
    // Walls hit on y-side render darker (cheap fake "Doom diminished light").
    // For Pebble64 we just nudge the row index lookup — keeping a single
    // texture buffer avoids carrying a darkened LUT. v0 simplification:
    // emit raw color for x-walls, darken-by-XOR for y-walls.
    int th_mask = th - 1;  // only meaningful for power-of-two heights
    for (int y = draw_start; y < draw_end; y++) {
      int tex_y = (tex_pos >> 16) & th_mask;
      uint8_t c = col_src_base[tex_y * tw];
      if (side == 1) {
        // Cheap shadow: knock both R and G down by one quantum each
        // (each channel is 2 bits in AARRGGBB-2222 starting at bits 4/2).
        // Subtract 0x14 if both R>0 and G>0 to avoid underflow; else as-is.
        uint8_t r = (c >> 4) & 0x3;
        uint8_t g = (c >> 2) & 0x3;
        if (r > 0) r--;
        if (g > 0) g--;
        c = (c & 0xC3) | (r << 4) | (g << 2);
      }
      *col_dst = c;
      col_dst += VIEWPORT_W;
      tex_pos += step;
    }
  }

  // === Sprite pass =======================================================
  //
  // For each live entity: transform world -> camera space (inverse of the
  // player's rotation). If transform_y < 0.1 the sprite is behind us;
  // skip. Otherwise project to screen X using the same fov_half_tan that
  // walls use. Sprite size scales with 1/transform_y exactly like a wall
  // at that distance. Per-column zbuffer test occludes columns hidden
  // behind a closer wall.
  //
  // No depth sort yet — only one sprite. When MAX_ENTITIES grows we'll
  // sort indices by transform_y descending (far-to-near painter's).

  // Reuse the cached trig values from the wall loop above.
  const float cos_pa = cos_pa_w, sin_pa = sin_pa_w;
  // Same projection constant the wall path uses to derive line_h. Sprites
  // at the same perp distance as a wall must scale to the same column
  // height, so reuse VIEWPORT_H / dist.
  for (int e = 0; e < MAX_ENTITIES; e++) {
    if (!s_entities[e].alive) continue;
    float rx = s_entities[e].x - px;
    float ry = s_entities[e].y - py;
    // Inverse rotation: bring world delta into camera frame where +y is
    // forward (depth, along the player's facing dir) and +x is camera-right.
    // Player forward = (cos(pa), sin(pa)); right = (-sin(pa), cos(pa)).
    // So  cam_y = forward · (rx,ry) =  cos*rx + sin*ry
    //     cam_x = right   · (rx,ry) = -sin*rx + cos*ry
    float cam_y =  cos_pa * rx + sin_pa * ry;
    float cam_x = -sin_pa * rx + cos_pa * ry;
    if (cam_y < 0.1f) continue;   // behind player or too close

    sprite_t *spr = &s_sprites[s_entities[e].kind];
    if (!spr->pixels) continue;

    // Project center to screen X. (cam_x / cam_y) is tan(off-angle).
    int screen_x = (int)((VIEWPORT_W / 2)
                       + (cam_x / cam_y) * (VIEWPORT_W / (2.0f * fov_half_tan)));

    // Sprite "size" in pixels = VIEWPORT_H / cam_y, same scale as walls.
    int spr_size = (int)(VIEWPORT_H / cam_y);
    if (spr_size <= 0) continue;
    // Aspect ratio: most Doom sprites are tall (h>w). Preserve aspect:
    int spr_screen_w = spr_size * spr->w / spr->h;
    int spr_screen_h = spr_size;

    // Vertical: bottom of sprite at floor line (VIEWPORT_H/2 + spr_size/2),
    // so feet sit on the floor. Doom is more nuanced (sprite_offset_y) but
    // we don't expose that yet.
    int draw_x0 = screen_x - spr_screen_w / 2;
    int draw_x1 = screen_x + spr_screen_w / 2;
    int draw_y0 = VIEWPORT_H / 2 + spr_screen_h / 2 - spr_screen_h;
    int draw_y1 = VIEWPORT_H / 2 + spr_screen_h / 2;
    if (draw_y0 < 0) draw_y0 = 0;
    if (draw_y1 > VIEWPORT_H) draw_y1 = VIEWPORT_H;

    for (int sx = draw_x0; sx < draw_x1; sx++) {
      if (sx < 0 || sx >= VIEWPORT_W) continue;
      if (cam_y >= s_zbuffer[sx]) continue;   // occluded by wall
      int tex_x = (sx - draw_x0) * spr->w / spr_screen_w;
      if (tex_x < 0 || tex_x >= spr->w) continue;
      // Vertical step (Q16) to walk the texture column.
      uint32_t step_v = ((uint32_t)spr->h << 16) / (uint32_t)(spr_screen_h > 0 ? spr_screen_h : 1);
      uint32_t tex_pos_v = (uint32_t)(draw_y0 - (VIEWPORT_H / 2 + spr_screen_h / 2 - spr_screen_h)) * step_v;
      uint8_t *dst = out + draw_y0 * VIEWPORT_W + sx;
      const uint8_t *src_col = &spr->pixels[tex_x];
      for (int sy = draw_y0; sy < draw_y1; sy++) {
        int tex_y = (tex_pos_v >> 16);
        if (tex_y >= 0 && tex_y < spr->h) {
          uint8_t c = src_col[tex_y * spr->w];
          if (c != TRANSPARENT_BYTE) *dst = c;
        }
        dst += VIEWPORT_W;
        tex_pos_v += step_v;
      }
    }
  }

  // === HUD weapon overlay ================================================
  //
  // The player's pistol is drawn as a static sprite anchored to the bottom
  // of the viewport, centered horizontally. No depth scaling — it's
  // screen-space. TRANSPARENT_BYTE pixels pass through to whatever the
  // raycaster drew underneath.

  // Swap to the muzzle-flash sprite during the fire animation window.
  sprite_t *pistol = &s_sprites[
    s_player.fire_flash_ticks > 0 ? SPR_PISTOL_FIRE : SPR_PISTOL];
  if (pistol->pixels) {
    int px_screen = (VIEWPORT_W - pistol->w) / 2;
    int py_screen = VIEWPORT_H - pistol->h;
    if (py_screen < 0) py_screen = 0;
    int rows = pistol->h;
    int cols = pistol->w;
    if (py_screen + rows > VIEWPORT_H) rows = VIEWPORT_H - py_screen;
    if (px_screen + cols > VIEWPORT_W) cols = VIEWPORT_W - px_screen;
    if (px_screen < 0) { cols += px_screen; px_screen = 0; }
    for (int y = 0; y < rows; y++) {
      uint8_t *dst = out + (py_screen + y) * VIEWPORT_W + px_screen;
      const uint8_t *src = pistol->pixels + y * pistol->w
                         + (px_screen >= 0 ? 0 : -px_screen);
      for (int x = 0; x < cols; x++) {
        uint8_t c = src[x];
        if (c != TRANSPARENT_BYTE) dst[x] = c;
      }
    }
  }

  // === Statbar ===========================================================
  //
  // Bottom 50 pixels: solid steel-gray background, with the face (STFST01)
  // centered horizontally + vertically, flanked by 3-digit red numerals
  // (health on the left, ammo on the right). Values are placeholders for
  // v0 — the engine doesn't track HP/ammo yet.
  //
  // Pebble64 colors:
  //   steel gray  alpha=3 R=1 G=1 B=1  -> 0b11_01_01_01 = 0xD5
  //   dark steel  alpha=3 R=1 G=1 B=0  -> 0b11_01_01_00 = 0xD4 (top edge)

  uint8_t *statbar_top = out + VIEWPORT_H * SHADOW_W;
  // Single-row top edge (just a subtle horizontal line).
  memset(statbar_top, 0xD4, SHADOW_W);
  // Body of the statbar
  for (int y = 1; y < STATBAR_H; y++)
    memset(statbar_top + y * SHADOW_W, 0xD5, SHADOW_W);

  // Helper to blit a sprite to (sx, sy) in the full-display buffer with
  // TRANSPARENT_BYTE skipping. Used for face + digits.
  // (Defined inline-ish to keep the engine file small.)

  #define DRAW_SPRITE_TO_HUD(spr_kind, sx, sy) do { \
    sprite_t *_s = &s_sprites[spr_kind]; \
    if (_s->pixels) { \
      int _x0 = (sx), _y0 = (sy); \
      int _w = _s->w, _h = _s->h; \
      if (_y0 + _h > SHADOW_H) _h = SHADOW_H - _y0; \
      if (_x0 + _w > SHADOW_W) _w = SHADOW_W - _x0; \
      if (_x0 < 0 || _y0 < 0) break; \
      for (int _yy = 0; _yy < _h; _yy++) { \
        uint8_t *_d = out + (_y0 + _yy) * SHADOW_W + _x0; \
        const uint8_t *_ss = _s->pixels + _yy * _s->w; \
        for (int _xx = 0; _xx < _w; _xx++) { \
          uint8_t _c = _ss[_xx]; \
          if (_c != TRANSPARENT_BYTE) _d[_xx] = _c; \
        } \
      } \
    } \
  } while (0)

  // Face centered horizontally in the statbar.
  sprite_t *face = &s_sprites[SPR_FACE];
  if (face->pixels) {
    int fx = (SHADOW_W - face->w) / 2;
    int fy = VIEWPORT_H + (STATBAR_H - face->h) / 2;
    DRAW_SPRITE_TO_HUD(SPR_FACE, fx, fy);
  }

  // Placeholder values: health=100 on left, ammo=50 on right. Numerals
  // are drawn right-aligned (Doom convention). STTNUM* digits are ~14px
  // wide each.
  static const uint8_t hp = 100, ammo = 50;
  static const sprite_kind_t s_digit_kinds[10] = {
    SPR_NUM0, SPR_NUM1, SPR_NUM2, SPR_NUM3, SPR_NUM4,
    SPR_NUM5, SPR_NUM6, SPR_NUM7, SPR_NUM8, SPR_NUM9,
  };
  sprite_t *digit0 = &s_sprites[SPR_NUM0];
  int dw = digit0->pixels ? digit0->w : 14;
  int dy = VIEWPORT_H + (STATBAR_H - (digit0->pixels ? digit0->h : 16)) / 2;
  // Health on left side
  {
    int v = hp;
    int rx = 6 + dw * 3;  // right edge of 3-digit field
    for (int i = 0; i < 3; i++) {
      int d = v % 10; v /= 10;
      DRAW_SPRITE_TO_HUD(s_digit_kinds[d], rx - (i + 1) * dw, dy);
      if (v == 0 && i >= 0 && i < 2 && (hp / (10 * (i + 1))) == 0) break;
    }
  }
  // Ammo on right side
  {
    int v = ammo;
    int rx = SHADOW_W - 6;
    for (int i = 0; i < 3; i++) {
      int d = v % 10; v /= 10;
      DRAW_SPRITE_TO_HUD(s_digit_kinds[d], rx - (i + 1) * dw, dy);
      if (v == 0 && i >= 0 && i < 2 && (ammo / (10 * (i + 1))) == 0) break;
    }
  }

  #undef DRAW_SPRITE_TO_HUD

  // Update debug snapshot
  s_debug.player_x_q8 = (int)(s_player.x * 256);
  s_debug.player_y_q8 = (int)(s_player.y * 256);
  s_debug.player_angle_deg = (int)(s_player.angle * 57.2958f) % 360;
  s_debug.current_tile = s_tilemap[(int)s_player.y][(int)s_player.x];
}

void engine_get_debug(engine_debug_t *out) { *out = s_debug; }
