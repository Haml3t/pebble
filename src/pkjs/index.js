var Clay = require('pebble-clay');
var clayConfig = require('./config.json');
var clay = new Clay(clayConfig, null, { autoHandleEvents: false });

// Numeric message-key IDs (auto-assigned by Pebble build from package.json
// messageKeys order — see build/js/message_keys.json). Clay's getSettings()
// returns the dict keyed by these ints, not by the string names.
var MK = {
  CFG_WEATHER_KEY:         10010,
  CFG_GOOGLE_REFRESH:      10011,
  CFG_GOOGLE_CLIENT_ID:    10012,
  CFG_GOOGLE_CLIENT_SECRET: 10013,
  CFG_HR_LIVE:             10015,
  REQUEST_CONFIG:          10016,
};

var STORAGE = {
  weatherKey: 'cfg_weather_key',
  googleRefreshToken: 'cfg_google_refresh',
  googleAccessToken: 'cfg_google_access',
  googleAccessExpiry: 'cfg_google_expiry',
  googleClientId: 'cfg_google_client_id',
  googleClientSecret: 'cfg_google_client_secret',
  hrLive: 'cfg_hr_live',
};

function log() {
  console.log('[glance] ' + Array.prototype.slice.call(arguments).join(' '));
}

function sendToWatch(payload) {
  Pebble.sendAppMessage(payload,
    function () { log('sent', JSON.stringify(payload)); },
    function (e) { log('send failed', JSON.stringify(e)); });
}

// ---- Weather (OpenWeatherMap) -------------------------------------------

function fetchWeather() {
  var key = localStorage.getItem(STORAGE.weatherKey);
  if (!key) { log('no weather key configured'); return; }

  navigator.geolocation.getCurrentPosition(function (pos) {
    var url = 'https://api.openweathermap.org/data/2.5/weather'
      + '?lat=' + pos.coords.latitude
      + '&lon=' + pos.coords.longitude
      + '&units=imperial&appid=' + encodeURIComponent(key);
    var xhr = new XMLHttpRequest();
    xhr.open('GET', url);
    xhr.onload = function () {
      try {
        var data = JSON.parse(xhr.responseText);
        sendToWatch({
          WEATHER_TEMP: Math.round(data.main.temp),
          WEATHER_COND: data.weather[0].main,
        });
      } catch (e) {
        log('weather parse failed', e.message);
      }
    };
    xhr.onerror = function () { log('weather fetch failed'); };
    xhr.send();
  }, function (err) {
    log('geolocation failed', err.message);
  }, { timeout: 15000, maximumAge: 300000 });
}

// ---- Google Calendar -----------------------------------------------------
// We use a refresh_token obtained via the Clay config OAuth flow. Each
// refresh we exchange it for a short-lived access_token.

function reportCalError(msg) {
  log('cal error:', msg);
  sendToWatch({ CAL_TITLE: ('cal: ' + msg).substring(0, 40), CAL_TIME: '' });
}

function refreshGoogleToken(cb) {
  var refresh = localStorage.getItem(STORAGE.googleRefreshToken);
  var clientId = localStorage.getItem(STORAGE.googleClientId);
  var clientSecret = localStorage.getItem(STORAGE.googleClientSecret);
  if (!refresh || !clientId || !clientSecret) {
    reportCalError('no creds saved');
    cb(null); return;
  }

  var expiry = parseInt(localStorage.getItem(STORAGE.googleAccessExpiry) || '0', 10);
  var cached = localStorage.getItem(STORAGE.googleAccessToken);
  if (cached && Date.now() < expiry - 30000) { cb(cached); return; }

  var body = 'client_id=' + encodeURIComponent(clientId)
    + '&client_secret=' + encodeURIComponent(clientSecret)
    + '&refresh_token=' + encodeURIComponent(refresh)
    + '&grant_type=refresh_token';
  var xhr = new XMLHttpRequest();
  xhr.open('POST', 'https://oauth2.googleapis.com/token');
  xhr.setRequestHeader('Content-Type', 'application/x-www-form-urlencoded');
  xhr.onload = function () {
    try {
      var data = JSON.parse(xhr.responseText);
      if (data.error) {
        // invalid_grant = refresh token rejected (revoked, expired after the
        // 7-day Testing-mode TTL, password rotated, etc.). The raw Google
        // message is "Token has been expired or revoked." which truncates
        // mid-word on the watch — replace with an actionable string.
        var msg = data.error === 'invalid_grant'
          ? 'auth expired - re-auth on phone'
          : 'oauth ' + (data.error_description || data.error);
        reportCalError(msg);
        cb(null); return;
      }
      if (!data.access_token) {
        reportCalError('no access_token in resp');
        cb(null); return;
      }
      localStorage.setItem(STORAGE.googleAccessToken, data.access_token);
      localStorage.setItem(STORAGE.googleAccessExpiry,
        String(Date.now() + (data.expires_in || 3600) * 1000));
      cb(data.access_token);
    } catch (e) {
      reportCalError('oauth parse: ' + e.message);
      cb(null);
    }
  };
  xhr.onerror = function () { reportCalError('oauth network err'); cb(null); };
  xhr.send(body);
}

function fetchCalendarList(token, cb) {
  var xhr = new XMLHttpRequest();
  xhr.open('GET',
    'https://www.googleapis.com/calendar/v3/users/me/calendarList'
    + '?minAccessRole=reader&showHidden=true'
    + '&fields=items(id,summary,selected,hidden,deleted)');
  xhr.setRequestHeader('Authorization', 'Bearer ' + token);
  xhr.onload = function () {
    try {
      var data = JSON.parse(xhr.responseText);
      if (data.error) { reportCalError('list ' + data.error.message); cb([]); return; }
      // Strict include: `selected === true` only. The previous
      // `selected !== false` filter let calendars where Google omits the
      // field (a documented "default is False" case) leak through —
      // including ones the user explicitly unchecked, since Google
      // sometimes returns `undefined` instead of `false` for those.
      // `showHidden=true` so we see hidden calendars in the log too;
      // we still exclude them from the included set.
      var items = (data.items || []);
      items.forEach(function (c) {
        log('cal', c.summary || '(no summary)',
            'selected=' + JSON.stringify(c.selected),
            'hidden=' + JSON.stringify(c.hidden),
            'deleted=' + JSON.stringify(c.deleted));
      });
      var picks = items
        .filter(function (c) {
          return c.selected === true && !c.hidden && !c.deleted;
        })
        .map(function (c) { return { id: c.id, name: c.summary }; });
      cb(picks);
    } catch (e) { reportCalError('list parse: ' + e.message); cb([]); }
  };
  xhr.onerror = function () { reportCalError('list net err'); cb([]); };
  xhr.send();
}

function fetchNextFromCalendar(token, calId, timeMin, timeMax, cb) {
  var url = 'https://www.googleapis.com/calendar/v3/calendars/'
    + encodeURIComponent(calId) + '/events'
    + '?singleEvents=true&orderBy=startTime&maxResults=1'
    + '&timeMin=' + encodeURIComponent(timeMin)
    + '&timeMax=' + encodeURIComponent(timeMax);
  var xhr = new XMLHttpRequest();
  xhr.open('GET', url);
  xhr.setRequestHeader('Authorization', 'Bearer ' + token);
  xhr.onload = function () {
    try {
      var data = JSON.parse(xhr.responseText);
      cb(data && !data.error ? ((data.items || [])[0] || null) : null);
    } catch (e) { cb(null); }
  };
  xhr.onerror = function () { cb(null); };
  xhr.send();
}

// Lookahead window for the "next event" calendar widget. 7 days catches
// the common case where the next event is just past tomorrow (e.g. late
// at night looking ahead) without making the widget noisy with events
// two weeks out — the watch only ever surfaces the single earliest one.
var CAL_LOOKAHEAD_MS = 7 * 24 * 60 * 60 * 1000;

function fetchNextCalendarEvent() {
  refreshGoogleToken(function (token) {
    if (!token) { log('no google token'); return; }
    var now = new Date();
    var soon = new Date(now.getTime() + CAL_LOOKAHEAD_MS);
    var timeMin = now.toISOString();
    var timeMax = soon.toISOString();

    fetchCalendarList(token, function (cals) {
      if (cals.length === 0) {
        sendToWatch({ CAL_TITLE: 'No upcoming event', CAL_TIME: '' });
        return;
      }
      var pending = cals.length;
      var bestEvent = null;
      var bestTs = Infinity;
      cals.forEach(function (cal) {
        fetchNextFromCalendar(token, cal.id, timeMin, timeMax, function (event) {
          if (event) {
            log('cal-event', cal.name,
                '"' + (event.summary || '(no title)') + '"',
                event.start.dateTime || event.start.date);
            var startStr = event.start.dateTime || event.start.date;
            var ts = new Date(startStr).getTime();
            // TODO: filter out in-progress events. Google's timeMin filters
            // by an event's END time, so an event that started before now
            // but ends after now IS returned; its earlier startTime then
            // wins the orderBy sort and the widget shows the in-progress
            // event instead of the next future one. Fix: skip when ts <= now.
            if (ts < bestTs) { bestTs = ts; bestEvent = event; }
          } else {
            log('cal-event', cal.name, '(none in lookahead)');
          }
          if (--pending === 0) emitBestEvent(bestEvent);
        });
      });
    });
  });
}

function emitBestEvent(event) {
  if (!event) {
    sendToWatch({ CAL_TITLE: 'No upcoming event', CAL_TIME: '' });
    return;
  }
  var d = new Date(event.start.dateTime || event.start.date);
  var now = new Date();
  // Compare local calendar days, not 24h-aligned buckets, so a midnight
  // event on the next calendar day shows as "tomorrow" even if it's <24h
  // away. >=2 days out also gets a weekday prefix (Mon/Tue/...) so a
  // 7-day-out event is unambiguous.
  var startOfTodayMs = new Date(now.getFullYear(), now.getMonth(), now.getDate()).getTime();
  var dayDelta = Math.floor((d.getTime() - startOfTodayMs) / (24 * 60 * 60 * 1000));
  var hh = d.getHours();
  var mm = d.getMinutes();
  var time = ((hh % 12) || 12) + ':' + (mm < 10 ? '0' + mm : mm)
    + (hh < 12 ? 'a' : 'p');
  var weekdays = ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat'];
  var label = (dayDelta <= 0) ? time
            : (dayDelta === 1) ? ('Tmrw ' + time)
            : (weekdays[d.getDay()] + ' ' + time);
  sendToWatch({
    CAL_TITLE: (event.summary || '(no title)').substring(0, 48),
    CAL_TIME: label,
  });
}

// ---- Lifecycle -----------------------------------------------------------
// Now Playing comes from the Android companion app via PebbleKit Android,
// not from this layer.

function refreshAll() {
  fetchWeather();
  fetchNextCalendarEvent();
}

// Belt-and-suspenders throttle for tick-driven refreshes. The watchapp's
// tick_handler already gates REQUEST_REFRESH to every Nth minute, but a
// flurry of watchapp restarts each fires its own request_refresh from
// init(), and we don't want each restart to trigger a full cal+weather
// fetch. 4 min < watch gate of 5 min so legitimate periodic refreshes
// still pass. Bypassed by startup/config-change/recovery paths below.
var REFRESH_THROTTLE_MS = 4 * 60 * 1000;
var lastRefreshAllMs = 0;
function refreshAllThrottled() {
  var now = Date.now();
  if (now - lastRefreshAllMs >= REFRESH_THROTTLE_MS) {
    lastRefreshAllMs = now;
    refreshAll();
  } else {
    log('refresh throttled (' + Math.round((now - lastRefreshAllMs)/1000) + 's since last)');
  }
}

// Mirror our STORAGE keys into Clay's own settings store so the settings
// page pre-fills with the current values. Without this Clay shows empty
// inputs even when we have working credentials, and a user editing one
// field and hitting Save would write empty strings for the others — which
// our save handler mirrors as `persist_delete()` to the watch, wiping the
// flash copy. So this is also a safety fix against accidental data loss.
function syncToClay() {
  clay.setSettings({
    CFG_WEATHER_KEY:          localStorage.getItem(STORAGE.weatherKey)          || '',
    CFG_GOOGLE_REFRESH:       localStorage.getItem(STORAGE.googleRefreshToken)  || '',
    CFG_GOOGLE_CLIENT_ID:     localStorage.getItem(STORAGE.googleClientId)      || '',
    CFG_GOOGLE_CLIENT_SECRET: localStorage.getItem(STORAGE.googleClientSecret)  || '',
    CFG_HR_LIVE:              localStorage.getItem(STORAGE.hrLive) === '1'
  });
}

function sendHrLive() {
  // Toggle defaults to burst-on-tap (1Hz only during ~30s windows after a
  // wrist flick) — matches config.json defaultValue: false.
  var stored = localStorage.getItem(STORAGE.hrLive);
  var live = (stored === null) ? false : (stored === '1');
  sendToWatch({ CFG_HR_LIVE: live ? 1 : 0 });
}

// PKJS localStorage is unreliable — the Pebble companion app can wipe it on
// reinstall, PKJS-bundle changes, or sometimes spontaneously. The watchapp
// keeps a durable mirror in its per-UUID flash, so on every PKJS startup we
// ask the watch to send its copy back. Values fill localStorage only where
// we don't already have one, so an in-progress unsaved edit isn't clobbered
// by an older watch-side copy.
function applyRecovered(payload, msgKey, storageKey) {
  var v = payload[msgKey];
  if (typeof v !== 'string' || v === '') return false;
  if (localStorage.getItem(storageKey)) return false;
  localStorage.setItem(storageKey, v);
  return true;
}

Pebble.addEventListener('ready', function () {
  log('PKJS ready');
  syncToClay();
  sendHrLive();
  sendToWatch({ REQUEST_CONFIG: 1 });
  refreshAll();
});

Pebble.addEventListener('appmessage', function (e) {
  if (!e || !e.payload) return;
  if (e.payload.REQUEST_REFRESH) refreshAllThrottled();

  var filled = false;
  filled = applyRecovered(e.payload, 'CFG_WEATHER_KEY',
                          STORAGE.weatherKey)         || filled;
  filled = applyRecovered(e.payload, 'CFG_GOOGLE_REFRESH',
                          STORAGE.googleRefreshToken) || filled;
  filled = applyRecovered(e.payload, 'CFG_GOOGLE_CLIENT_ID',
                          STORAGE.googleClientId)     || filled;
  filled = applyRecovered(e.payload, 'CFG_GOOGLE_CLIENT_SECRET',
                          STORAGE.googleClientSecret) || filled;
  if (filled) {
    log('recovered config from watch flash; re-running fetches');
    syncToClay();
    // Drop any stale access token so the next call uses the recovered creds.
    localStorage.removeItem(STORAGE.googleAccessToken);
    localStorage.removeItem(STORAGE.googleAccessExpiry);
    refreshAll();
  }
});

Pebble.addEventListener('showConfiguration', function () {
  Pebble.openURL(clay.generateUrl());
});

Pebble.addEventListener('webviewclosed', function (e) {
  if (!e || !e.response) return;
  var dict;
  try { dict = clay.getSettings(e.response); }
  catch (err) { log('clay getSettings failed', err.message); return; }

  function pull(id) {
    var v = dict[id];
    if (v && typeof v === 'object' && 'value' in v) v = v.value;
    return (typeof v === 'string') ? v.trim() : '';
  }
  function pullBool(id) {
    var v = dict[id];
    if (v && typeof v === 'object' && 'value' in v) v = v.value;
    return !!v;
  }
  var weatherKey  = pull(MK.CFG_WEATHER_KEY);
  var googRefresh = pull(MK.CFG_GOOGLE_REFRESH);
  var googClientId = pull(MK.CFG_GOOGLE_CLIENT_ID);
  var googSecret  = pull(MK.CFG_GOOGLE_CLIENT_SECRET);
  var hrLive      = pullBool(MK.CFG_HR_LIVE);

  if (weatherKey)  localStorage.setItem(STORAGE.weatherKey, weatherKey);
  if (googRefresh) localStorage.setItem(STORAGE.googleRefreshToken, googRefresh);
  if (googClientId) localStorage.setItem(STORAGE.googleClientId, googClientId);
  if (googSecret)  localStorage.setItem(STORAGE.googleClientSecret, googSecret);
  localStorage.setItem(STORAGE.hrLive, hrLive ? '1' : '0');
  // Keep Clay's store in sync so the next openConfiguration shows current
  // values, not blanks (avoids accidental wipe via empty-fields Save).
  syncToClay();

  // Mirror config to watch flash. Empty strings are sent through so the
  // watch can clear its persisted copy when the user blanks a field.
  var mirror = { CFG_HR_LIVE: hrLive ? 1 : 0 };
  mirror.CFG_WEATHER_KEY          = weatherKey;
  mirror.CFG_GOOGLE_REFRESH       = googRefresh;
  mirror.CFG_GOOGLE_CLIENT_ID     = googClientId;
  mirror.CFG_GOOGLE_CLIENT_SECRET = googSecret;
  sendToWatch(mirror);

  // Invalidate cached access token so the next refresh uses fresh creds.
  localStorage.removeItem(STORAGE.googleAccessToken);
  localStorage.removeItem(STORAGE.googleAccessExpiry);
  refreshAll();
});
