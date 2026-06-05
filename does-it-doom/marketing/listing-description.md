# Does It Doom?

Doom on your wrist. A raycaster homage to E1M1 — not the real BSP engine. Three connected rooms (the hangar, a pillared corridor, a nukage chamber), six imps, a working pistol with hitscan, and the kind of HUD that absolutely should not fit in 200×228.

## What's in

- One hand-crafted level inspired by E1M1's starting rooms
- STARTAN3 walls, FLOOR4_1 / CEIL3_5 flats, an uncrossable nukage pool you can shoot and see across
- Six imps that chase you, attack at melee range, and die in one shot
- Pistol with muzzle flash, hitscan damage in a narrow cone (SELECT to fire)
- Directional damage chevron at the viewport edge pointing at the last threat
- Doom-style statbar with face, HP, and ammo numerals (ammo is currently infinite — that's v2)

## What's NOT in

- The real BSP renderer. This is a Wolfenstein-style grid raycaster — flat floors and ceilings, no sector heights.
- The full DOOM1.WAD. Only the textures + sprites needed for this one level are bundled.
- More levels (v2 will stream them from a phone companion).
- Sound — the sandbox can't keep up with audio + 20 fps raycasting on this hardware.

## Controls

- **SELECT** — fire pistol
- **UP / DOWN** — walk forward / back
- **BACK + UP / DOWN** — strafe left / right
- **Tilt wrist left / right** — turn
- **BACK (short press)** — pause / quit

## Compatibility

Pebble Time 2 (emery) only. The engine's allocations and framebuffer assumptions are sized for the PT2 heap; earlier Pebbles don't have the headroom.

## Attribution

Sprite and texture assets © id Software, used under Doom 1 shareware redistribution terms. This app does not include the full WAD.
