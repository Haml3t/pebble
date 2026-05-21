// v1 of "Does It Doom?" has no PKJS dependencies — the bundled-level
// build runs entirely on the watch with no companion. This file exists only
// because the Pebble build expects a PKJS entry point.
Pebble.addEventListener('ready', function () {
  console.log('[doom] PKJS ready (no-op v1)');
});
