// Metronome watchapp has no phone-side logic — the Android companion handles
// recording and stats over PebbleKit Android directly. This file exists only
// because the Pebble build expects a PKJS entry point.
Pebble.addEventListener('ready', function () {
  console.log('[metronome] PKJS ready (no-op)');
});
