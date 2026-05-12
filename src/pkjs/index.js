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
};

var STORAGE = {
  weatherKey: 'cfg_weather_key',
  googleRefreshToken: 'cfg_google_refresh',
  googleAccessToken: 'cfg_google_access',
  googleAccessExpiry: 'cfg_google_expiry',
  googleClientId: 'cfg_google_client_id',
  googleClientSecret: 'cfg_google_client_secret',
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
        reportCalError('oauth ' + (data.error_description || data.error));
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
    + '?minAccessRole=reader&fields=items(id,selected,deleted)');
  xhr.setRequestHeader('Authorization', 'Bearer ' + token);
  xhr.onload = function () {
    try {
      var data = JSON.parse(xhr.responseText);
      if (data.error) { reportCalError('list ' + data.error.message); cb([]); return; }
      var ids = (data.items || [])
        .filter(function (c) { return c.selected !== false && !c.deleted; })
        .map(function (c) { return c.id; });
      cb(ids);
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

function fetchNextCalendarEvent() {
  refreshGoogleToken(function (token) {
    if (!token) { log('no google token'); return; }
    var now = new Date();
    var soon = new Date(now.getTime() + 24 * 60 * 60 * 1000);
    var timeMin = now.toISOString();
    var timeMax = soon.toISOString();

    fetchCalendarList(token, function (calIds) {
      if (calIds.length === 0) {
        sendToWatch({ CAL_TITLE: 'No upcoming event', CAL_TIME: '' });
        return;
      }
      var pending = calIds.length;
      var bestEvent = null;
      var bestTs = Infinity;
      calIds.forEach(function (calId) {
        fetchNextFromCalendar(token, calId, timeMin, timeMax, function (event) {
          if (event) {
            var startStr = event.start.dateTime || event.start.date;
            var ts = new Date(startStr).getTime();
            if (ts < bestTs) { bestTs = ts; bestEvent = event; }
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
  var hh = d.getHours();
  var mm = d.getMinutes();
  var label = ((hh % 12) || 12) + ':' + (mm < 10 ? '0' + mm : mm)
    + (hh < 12 ? 'a' : 'p');
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

Pebble.addEventListener('ready', function () {
  log('PKJS ready');
  refreshAll();
});

Pebble.addEventListener('appmessage', function (e) {
  if (e.payload && e.payload.REQUEST_REFRESH) refreshAll();
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
  var weatherKey  = pull(MK.CFG_WEATHER_KEY);
  var googRefresh = pull(MK.CFG_GOOGLE_REFRESH);
  var googClientId = pull(MK.CFG_GOOGLE_CLIENT_ID);
  var googSecret  = pull(MK.CFG_GOOGLE_CLIENT_SECRET);

  if (weatherKey)  localStorage.setItem(STORAGE.weatherKey, weatherKey);
  if (googRefresh) localStorage.setItem(STORAGE.googleRefreshToken, googRefresh);
  if (googClientId) localStorage.setItem(STORAGE.googleClientId, googClientId);
  if (googSecret)  localStorage.setItem(STORAGE.googleClientSecret, googSecret);

  // Invalidate cached access token so the next refresh uses fresh creds.
  localStorage.removeItem(STORAGE.googleAccessToken);
  localStorage.removeItem(STORAGE.googleAccessExpiry);
  refreshAll();
});
