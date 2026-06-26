// Glance metronome — local audio editor.
// All decoding/clipping happens in the browser via Web Audio API. The
// Python sidecar just lists files and serves them statically.
"use strict";

// ---- DOM handles --------------------------------------------------------
const $ = (sel) => document.querySelector(sel);
const filesEl       = $("#files");
const statusEl      = $("#status");
const filenameEl    = $("#filename");
const durationEl    = $("#duration");
const playBtn       = $("#play");
const playSelBtn    = $("#play-selection");
const clearSelBtn   = $("#clear-selection");
const saveBtn       = $("#save-clip");
const shareBtn      = $("#share-package");
const timeEl        = $("#time");
const selInfoEl     = $("#selection-info");
const refreshBtn    = $("#refresh");
const syncBtn       = $("#sync");
const bpmAxisEl     = $("#bpm-axis");
const backBtn       = $("#back-to-list");

const waveCanvas    = $("#waveform");
const rulerCanvas   = $("#ruler");
const waveOverlay   = $("#waveform-overlay");
const deleteBtn     = $("#delete-recording");
const exportBtn     = $("#export-file");
const addMarkerBtn  = $("#add-marker");
const renameBtn     = $("#rename-recording");

// ---- State --------------------------------------------------------------
let audioCtx = null;             // lazy-init on first user gesture
let audioBuffer = null;          // currently-loaded AudioBuffer
let currentFile = null;          // {name, ...} from /api/files
let sidecar = null;              // parsed JSON or null
let durationSec = 0;
let activeSource = null;         // current AudioBufferSourceNode
let playStartCtxTime = 0;        // audioCtx.currentTime at last play()
let playStartOffset = 0;         // seconds within audioBuffer at last play()
let isPlaying = false;
let selection = null;            // {start, end} in seconds, or null
let raf = null;                  // requestAnimationFrame handle for playhead
let markers = [];                // [{t_ms, note, source?}] for current file

// ---- Utilities ----------------------------------------------------------
function fmtTime(s) {
  if (!isFinite(s) || s < 0) s = 0;
  const m = Math.floor(s / 60);
  const r = s - m * 60;
  return `${m}:${r.toFixed(1).padStart(4, "0")}`;
}

// Recording filenames look like "2026-05-15_01-13-07_125bpm.m4a" or
// "2026-05-15_01-13-07_120-125bpm.m4a". Pull out a human-friendly
// "2026-05-15 01:13:07" and (optionally) the BPM range.
function parseRecordingName(name) {
  const m = name.match(/^(\d{4}-\d{2}-\d{2})_(\d{2})-(\d{2})-(\d{2})_(.+?)\.m4a$/);
  if (!m) return { stamp: name, bpm: "" };
  const stamp = `${m[1]} ${m[2]}:${m[3]}:${m[4]}`;
  // The last segment is the bpm tag — strip the literal "bpm" suffix for
  // display but keep the dash-separated range as-is ("120-125").
  const bpm = m[5].replace(/bpm$/, "");
  return { stamp, bpm };
}
function fmtBytes(n) {
  if (n < 1024) return `${n} B`;
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KB`;
  return `${(n / 1024 / 1024).toFixed(1)} MB`;
}

function escapeHtml(s) {
  // Used everywhere we interpolate user-provided strings (display names)
  // into our innerHTML templates. The display-name length cap on the
  // server (200 chars) bounds the worst case but doesn't strip markup.
  return String(s).replace(/[&<>"']/g, (c) => ({
    "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;",
  })[c]);
}

// Slugify a recording's display name into a filename-safe token, so exports
// carry the human name the user gave it via the rename prompt. Returns "" when
// the recording hasn't been renamed.
function displayNameSlug(displayName) {
  if (!displayName) return "";
  return String(displayName)
      .trim()
      .replace(/[\\/]/g, "_")       // path separators
      .replace(/[\x00-\x1f]/g, "")  // control chars
      .replace(/\s+/g, "_")         // whitespace → underscore
      .replace(/_+/g, "_")          // collapse underscore runs
      .replace(/^_+|_+$/g, "");     // trim leading/trailing underscores
}

// Insert the renamed display name right after the parent recording's stem —
// before any "_clip_..." suffix and before the extension. Keeping the leading
// stem intact matters: the server associates clips with their parent purely by
// filename prefix (glob(stem + "*.wav")), so the name must still start with the
// stem. Returns `name` unchanged when the recording hasn't been renamed.
function nameWithDisplay(name, displayName) {
  const slug = displayNameSlug(displayName);
  if (!slug) return name;
  const dot  = name.lastIndexOf(".");
  const ext  = dot >= 0 ? name.slice(dot) : "";
  const base = dot >= 0 ? name.slice(0, dot) : name;
  const clipIdx = base.indexOf("_clip_");
  if (clipIdx >= 0) {
    return base.slice(0, clipIdx) + "_" + slug + base.slice(clipIdx) + ext;
  }
  return base + "_" + slug + ext;
}

function ensureAudioCtx() {
  if (!audioCtx) audioCtx = new (window.AudioContext || window.webkitAudioContext)();
  return audioCtx;
}

// In Android WebView (and Safari) AudioContext starts in `suspended` state
// and produces no sound until resume() is called inside a user gesture.
// Call this from every button handler before scheduling audio.
async function resumeAudioCtxIfNeeded() {
  if (!audioCtx) return;
  if (audioCtx.state === "suspended") {
    try { await audioCtx.resume(); } catch (e) { setStatus("audio resume failed: " + e.message); }
  }
}

function setStatus(msg) { statusEl.textContent = msg; }

// ---- File list ----------------------------------------------------------
async function refreshFileList() {
  setStatus("loading file list…");
  try {
    const items = await fetch("/api/files").then(r => r.json());
    renderFileList(items);
    setStatus(`${items.length} recording${items.length === 1 ? "" : "s"}`);
  } catch (e) {
    setStatus("file list failed: " + e.message);
  }
}

function renderFileList(items) {
  filesEl.innerHTML = "";
  if (items.length === 0) {
    filesEl.innerHTML = "<li style='color:#6a6f79;padding:8px;cursor:default'>"
      + "no recordings — open the metronome on the watch, then click ↻ refresh</li>";
    return;
  }
  for (const item of items) {
    filesEl.appendChild(renderParentLi(item));
  }
}

function renderParentLi(item) {
  const li = document.createElement("li");
  if (!item.has_sidecar) li.classList.add("no-sidecar");
  const bpm = item.bpm_lo == null ? ""
            : item.bpm_lo === item.bpm_hi ? `${item.bpm_lo} bpm`
                                          : `${item.bpm_lo}–${item.bpm_hi} bpm`;
  const { stamp } = parseRecordingName(item.name);
  const hasClips = item.clips && item.clips.length > 0;
  const star = item.starred ? "★" : "☆";
  // Primary line: custom display name if set, else the parsed stamp.
  // When a display_name is present, the parsed stamp drops to a small
  // subtitle so the user keeps both anchors.
  const primary = item.display_name || stamp;
  const subtitleHtml = item.display_name
      ? `<span class="filename">${escapeHtml(stamp)}</span>`
      : "";
  const metaParts = [];
  if (bpm) metaParts.push(bpm);
  metaParts.push(fmtBytes(item.size));
  if (item.duration_sec) metaParts.push(fmtTime(item.duration_sec));
  if (hasClips) {
    metaParts.push(item.clips.length + " clip"
                   + (item.clips.length === 1 ? "" : "s"));
  }
  li.innerHTML = `
    <div class="row">
      <span class="caret" title="${hasClips ? 'expand clips' : ''}">${hasClips ? "▸" : "·"}</span>
      <span class="star" title="star">${star}</span>
      <span class="grow">
        <span class="name">${escapeHtml(primary)}</span>
        ${subtitleHtml}
        <span class="meta">${metaParts.join(" · ")}</span>
      </span>
    </div>`;
  const caret = li.querySelector(".caret");
  const starEl = li.querySelector(".star");
  starEl.addEventListener("click", (e) => {
    e.stopPropagation();
    toggleStar(item.name, !item.starred, starEl);
  });
  li.querySelector(".row").addEventListener("click", () => selectParent(li, item));
  if (hasClips) {
    const sub = document.createElement("ul");
    sub.className = "clips";
    sub.hidden = true;
    for (const clip of item.clips) sub.appendChild(renderClipLi(clip, item));
    li.appendChild(sub);
    caret.addEventListener("click", (e) => {
      e.stopPropagation();
      sub.hidden = !sub.hidden;
      caret.textContent = sub.hidden ? "▸" : "▾";
    });
  }
  return li;
}

function renderClipLi(clip, parent) {
  const cli = document.createElement("li");
  cli.className = "clip";
  const star = clip.starred ? "★" : "☆";
  // The clip name carries its own t-range; just show the suffix beyond the parent stem.
  const parentStem = parent.name.replace(/\.m4a$/, "");
  const label = clip.name.startsWith(parentStem + "_clip_")
              ? clip.name.slice((parentStem + "_clip_").length).replace(/\.wav$/, "")
              : clip.name;
  const primary = clip.display_name || label;
  const subtitleHtml = clip.display_name
      ? `<span class="filename">${escapeHtml(label)}</span>`
      : "";
  const metaParts = [fmtBytes(clip.size)];
  if (clip.duration_sec) metaParts.push(fmtTime(clip.duration_sec));
  cli.innerHTML = `
    <div class="row">
      <span class="caret"> </span>
      <span class="star">${star}</span>
      <span class="grow">
        <span class="name">${escapeHtml(primary)}</span>
        ${subtitleHtml}
        <span class="meta">${metaParts.join(" · ")}</span>
      </span>
      <button class="export-clip" title="download clip">⬇</button>
    </div>`;
  cli.querySelector(".star").addEventListener("click", (e) => {
    e.stopPropagation();
    toggleStar(clip.name, !clip.starred, cli.querySelector(".star"));
  });
  cli.querySelector(".export-clip").addEventListener("click", (e) => {
    e.stopPropagation();
    triggerExport(clip.name, clip.display_name);
  });
  cli.querySelector(".row").addEventListener("click", () => {
    document.querySelectorAll("#files li.active, #files li.clip.active").forEach(x => x.classList.remove("active"));
    cli.classList.add("active");
    document.body.classList.add("editor-open");
    backBtn.hidden = false;
    // Load a clip as a recording-like object — no sidecar / bpm range.
    loadRecording({ name: clip.name, size: clip.size, has_sidecar: false,
                    bpm_lo: null, bpm_hi: null, isClip: true });
  });
  return cli;
}

function selectParent(li, item) {
  document.querySelectorAll("#files li.active, #files li.clip.active").forEach(x => x.classList.remove("active"));
  li.classList.add("active");
  document.body.classList.add("editor-open");
  backBtn.hidden = false;
  loadRecording(item);
}

async function toggleStar(name, value, el) {
  try {
    const res = await fetch("/api/star?name=" + encodeURIComponent(name) + "&value=" + value,
                            { method: "POST" });
    if (!res.ok) throw new Error("server " + res.status);
    el.textContent = value ? "★" : "☆";
    el.classList.toggle("on", value);
    // Mutate the in-memory item so a click of the now-correct star toggles back.
    // Easier: refresh the list to re-sync from server.
    refreshFileList();
  } catch (e) {
    setStatus("star failed: " + e.message);
  }
}

function triggerExport(name, displayName) {
  // In Android WebView the EditorActivity injects window.GlanceBridge —
  // use its shareFile() to surface Android's share sheet (Gmail / Drive /
  // Messages / Save to Files etc). On a regular browser the bridge is
  // absent and we fall back to a same-origin anchor download.
  if (typeof GlanceBridge !== "undefined" && GlanceBridge.isAvailable && GlanceBridge.isAvailable()) {
    const ok = GlanceBridge.shareFile(name);
    if (ok) {
      setStatus("share sheet open · " + name);
    } else {
      setStatus("share failed — file is at /sdcard/Android/data/com.dsugarman.glance/files/Music/metronome/" + name);
    }
    return;
  }
  // If the recording was renamed, fold its display name into the saved
  // download filename (after the stem). This only affects what the browser
  // names the file — the on-disk recording is untouched.
  const downloadName = nameWithDisplay(name, displayName);
  const url = "/recordings/" + encodeURIComponent(name);
  const link = document.createElement("a");
  link.href = url;
  link.download = downloadName;
  document.body.appendChild(link);
  link.click();
  document.body.removeChild(link);
  setStatus("download → " + downloadName);
}

// ---- Load + decode ------------------------------------------------------
function buildFilenameLabel() {
  if (!currentFile) return "No recording loaded";
  if (currentFile.display_name) return currentFile.display_name;
  if (currentFile.isClip || currentFile.name.endsWith(".wav")) {
    return "clip · " + currentFile.name;
  }
  const { stamp, bpm } = parseRecordingName(currentFile.name);
  return bpm ? `${stamp}  ·  ${bpm} bpm` : stamp;
}

async function loadRecording(item) {
  currentFile = item;
  filenameEl.textContent = buildFilenameLabel();
  durationEl.textContent = "decoding…";
  setStatus("loading " + item.name);
  stopPlayback();
  selection = null;
  updateSelectionUI();

  ensureAudioCtx();
  try {
    const ab = await fetch("/recordings/" + encodeURIComponent(item.name))
                 .then(r => r.arrayBuffer());
    audioBuffer = await audioCtx.decodeAudioData(ab);
    durationSec = audioBuffer.duration;
    // Cache duration server-side so the file list can show it for every
    // recording without each viewer having to decode the audio first.
    reportDurationIfNeeded(item.name, durationSec, item.duration_sec);
  } catch (e) {
    setStatus("decode failed: " + e.message);
    durationEl.textContent = "decode failed";
    return;
  }

  sidecar = null;
  if (item.has_sidecar) {
    try {
      sidecar = await fetch("/recordings/" + encodeURIComponent(
                  item.name.replace(/\.m4a$/, ".json")))
                  .then(r => r.json());
    } catch (e) {
      console.warn("sidecar load failed:", e);
    }
  }

  // Markers — server merges editor-saved markers with watch UP-press
  // markers from the sidecar event log so both kinds show up here.
  markers = [];
  try {
    const data = await fetch("/api/markers?audio=" + encodeURIComponent(item.name))
                       .then(r => r.json());
    markers = (data && data.markers) || [];
  } catch (e) {
    console.warn("markers load failed:", e);
  }

  durationEl.textContent = fmtTime(durationSec);
  setStatus(`${audioBuffer.sampleRate.toLocaleString()} Hz · ${audioBuffer.numberOfChannels} ch`);
  playBtn.disabled = false;
  // Clips don't have a sidecar to package — disable share for them.
  shareBtn.disabled = !!(currentFile && (currentFile.isClip || currentFile.name.endsWith(".wav")));
  deleteBtn.disabled = false;
  exportBtn.disabled = false;
  addMarkerBtn.disabled = false;
  renameBtn.disabled = false;

  resizeCanvases();
  redrawAll();
  updateTimeUI();
}

// ---- Canvases -----------------------------------------------------------
function resizeCanvases() {
  for (const c of [waveCanvas, rulerCanvas]) {
    // Match canvas backing-store size to its CSS size, accounting for DPR.
    const dpr = window.devicePixelRatio || 1;
    const cssW = c.clientWidth;
    const cssH = c.clientHeight;
    c.width  = Math.max(1, Math.round(cssW * dpr));
    c.height = Math.max(1, Math.round(cssH * dpr));
    const ctx = c.getContext("2d");
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  }
}

function pxToSec(px) {
  return (px / waveCanvas.clientWidth) * durationSec;
}
function secToPx(sec) {
  return (sec / durationSec) * waveCanvas.clientWidth;
}

// ---- Waveform + BPM overlay render --------------------------------------
function drawWaveform() {
  const ctx = waveCanvas.getContext("2d");
  const w = waveCanvas.clientWidth;
  const h = waveCanvas.clientHeight;
  ctx.clearRect(0, 0, w, h);
  if (!audioBuffer) return;

  // ---- BPM-derived background bands first (under the audio) -----------
  if (sidecar && sidecar.events && durationSec > 0) {
    const durationMs = durationSec * 1000;
    let tickStart = null;
    ctx.fillStyle = "rgba(80, 200, 120, 0.10)";
    for (const e of sidecar.events) {
      if (e.type === "tick_start") tickStart = e.t_ms;
      else if (e.type === "tick_stop" && tickStart !== null) {
        const x0 = (tickStart / durationMs) * w;
        const x1 = (e.t_ms / durationMs) * w;
        ctx.fillRect(x0, 0, x1 - x0, h);
        tickStart = null;
      }
    }
    if (tickStart !== null) {
      const x0 = (tickStart / durationMs) * w;
      ctx.fillRect(x0, 0, w - x0, h);
    }
  }

  // ---- Audio waveform peaks ------------------------------------------
  const data = audioBuffer.getChannelData(0);
  const samplesPerPixel = Math.max(1, Math.floor(data.length / w));
  ctx.fillStyle = "#3a8a5a";
  for (let x = 0; x < w; x++) {
    let min = 0, max = 0;
    const start = x * samplesPerPixel;
    const end = Math.min(data.length, start + samplesPerPixel);
    for (let i = start; i < end; i++) {
      const v = data[i];
      if (v < min) min = v;
      if (v > max) max = v;
    }
    const y0 = (1 - max) * h / 2;
    const y1 = (1 - min) * h / 2;
    ctx.fillRect(x, y0, 1, Math.max(1, y1 - y0));
  }

  // Centerline
  ctx.strokeStyle = "#2c3038";
  ctx.lineWidth = 1;
  ctx.beginPath();
  ctx.moveTo(0, h / 2);
  ctx.lineTo(w, h / 2);
  ctx.stroke();

  // ---- BPM step line on top of the waveform --------------------------
  if (!sidecar || !sidecar.events) {
    bpmAxisEl.textContent = "(no sidecar)";
    return;
  }
  if (durationSec <= 0) return;
  const durationMs = durationSec * 1000;

  // Y-axis bounds: pad ±5 BPM around the session range, clamped to [30, 240].
  let bpmMin = sidecar.bpm_min != null ? sidecar.bpm_min : 60;
  let bpmMax = sidecar.bpm_max != null ? sidecar.bpm_max : 120;
  if (bpmMin === bpmMax) { bpmMin -= 5; bpmMax += 5; }
  bpmMin = Math.max(30, bpmMin - 5);
  bpmMax = Math.min(240, bpmMax + 5);
  const bpmRange = bpmMax - bpmMin;
  bpmAxisEl.textContent = `${bpmMin}–${bpmMax} bpm`;

  function bpmToY(bpm) {
    return h - ((bpm - bpmMin) / bpmRange) * h;
  }

  // Trace the BPM step-line into a Path2D, then stroke it twice: a fat
  // black halo first for contrast against the bright-green waveform, then
  // the amber line over it. Pure-yellow over green is otherwise muddy.
  const path = new Path2D();
  let curBpm = null;
  for (const e of sidecar.events) {
    if (!e.bpm) continue;
    const x = (e.t_ms / durationMs) * w;
    const y = bpmToY(e.bpm);
    if (curBpm === null) {
      path.moveTo(0, y);
      path.lineTo(x, y);
    } else {
      const yPrev = bpmToY(curBpm);
      path.lineTo(x, yPrev);
      path.lineTo(x, y);
    }
    curBpm = e.bpm;
  }
  if (curBpm !== null) {
    path.lineTo(w, bpmToY(curBpm));
  }
  ctx.lineJoin = "round";
  ctx.lineCap  = "round";
  ctx.strokeStyle = "rgba(0,0,0,0.7)";
  ctx.lineWidth = 5;
  ctx.stroke(path);
  ctx.strokeStyle = "#ffd060";
  ctx.lineWidth = 2;
  ctx.stroke(path);
}

// Kept as a no-op stub so old call sites in redrawAll() still work; the
// BPM render is now folded into drawWaveform().
function drawBpm() {}

// ---- Ruler --------------------------------------------------------------
function drawRuler() {
  const ctx = rulerCanvas.getContext("2d");
  const w = rulerCanvas.clientWidth;
  const h = rulerCanvas.clientHeight;
  ctx.clearRect(0, 0, w, h);
  if (durationSec <= 0) return;

  // Pick a tick spacing aiming for ~80px between labels.
  const targetPx = 80;
  const targetSec = durationSec * targetPx / w;
  const niceSteps = [1, 2, 5, 10, 15, 30, 60, 120, 300, 600];
  let step = niceSteps[niceSteps.length - 1];
  for (const s of niceSteps) if (s >= targetSec) { step = s; break; }

  ctx.strokeStyle = "#3a3f49";
  ctx.fillStyle = "#8a8f99";
  ctx.font = "10px system-ui";
  ctx.lineWidth = 1;
  for (let t = 0; t <= durationSec; t += step) {
    const x = (t / durationSec) * w;
    ctx.beginPath();
    ctx.moveTo(x + 0.5, 0);
    ctx.lineTo(x + 0.5, 6);
    ctx.stroke();
    ctx.fillText(fmtTime(t).replace(/\.0$/, ""), x + 3, 14);
  }
}

// ---- Overlays (selection + playhead) ------------------------------------
function drawOverlay(overlayEl, color) {
  overlayEl.innerHTML = "";
  if (!audioBuffer) return;

  // Selection rectangle
  if (selection) {
    const x0 = secToPx(Math.min(selection.start, selection.end));
    const x1 = secToPx(Math.max(selection.start, selection.end));
    const sel = document.createElement("div");
    sel.style.cssText = `
      position:absolute; left:${x0}px; top:0; width:${x1 - x0}px; height:100%;
      background: rgba(252, 204, 102, 0.18);
      border-left: 1px solid #fc6; border-right: 1px solid #fc6;`;
    overlayEl.appendChild(sel);
  }

  // Playhead line
  const playheadSec = currentPlayheadSec();
  if (playheadSec != null) {
    const x = secToPx(playheadSec);
    const ph = document.createElement("div");
    ph.style.cssText = `
      position:absolute; left:${x}px; top:0; width:0; height:100%;
      border-left: 1px solid ${color};`;
    overlayEl.appendChild(ph);
  }
}

function drawMarkers(overlayEl) {
  if (!markers || markers.length === 0 || durationSec <= 0) return;
  for (let i = 0; i < markers.length; i++) {
    const m = markers[i];
    const sec = m.t_ms / 1000;
    if (sec < 0 || sec > durationSec) continue;
    const x = secToPx(sec);
    const wrap = document.createElement("div");
    wrap.className = "marker" + (m.source === "watch" && !m.note ? " unannotated" : "");
    wrap.style.left = x + "px";
    wrap.title = `${fmtTime(sec)}${m.note ? " — " + m.note : " (no note)"}`;
    const flag = document.createElement("div");
    flag.className = "marker-flag";
    flag.textContent = String(i + 1);
    wrap.appendChild(flag);
    wrap.addEventListener("click", (e) => {
      e.stopPropagation();
      editMarker(i);
    });
    overlayEl.appendChild(wrap);
  }
}

function redrawOverlays() {
  drawOverlay(waveOverlay, "#fff");
  drawMarkers(waveOverlay);
}

function redrawAll() {
  drawWaveform();
  drawBpm();
  drawRuler();
  redrawOverlays();
}

// ---- Playback -----------------------------------------------------------
function currentPlayheadSec() {
  if (!audioBuffer) return null;
  if (!isPlaying) return playStartOffset;
  return playStartOffset + (audioCtx.currentTime - playStartCtxTime);
}

function startPlayback(fromSec, untilSec) {
  if (!audioBuffer) return;
  stopPlayback();
  const offset = Math.max(0, Math.min(audioBuffer.duration, fromSec));
  const remaining = audioBuffer.duration - offset;
  const playDuration = (untilSec != null)
      ? Math.max(0, Math.min(remaining, untilSec - offset))
      : remaining;
  const src = audioCtx.createBufferSource();
  src.buffer = audioBuffer;
  src.connect(audioCtx.destination);
  src.onended = () => {
    if (activeSource === src) {
      isPlaying = false;
      activeSource = null;
      playStartOffset = (untilSec != null) ? untilSec : audioBuffer.duration;
      playBtn.textContent = "▶ play";
      cancelAnimationFrame(raf);
      updateTimeUI();
      redrawOverlays();
    }
  };
  src.start(0, offset, playDuration);
  activeSource = src;
  playStartCtxTime = audioCtx.currentTime;
  playStartOffset = offset;
  isPlaying = true;
  playBtn.textContent = "❚❚ pause";
  tickPlayhead();
}

function stopPlayback() {
  if (activeSource) {
    try { activeSource.onended = null; activeSource.stop(); } catch (e) {}
    activeSource = null;
  }
  if (isPlaying) {
    playStartOffset = playStartOffset + (audioCtx.currentTime - playStartCtxTime);
    if (playStartOffset > audioBuffer.duration) playStartOffset = audioBuffer.duration;
  }
  isPlaying = false;
  playBtn.textContent = "▶ play";
  cancelAnimationFrame(raf);
  updateTimeUI();
  redrawOverlays();
}

async function togglePlay() {
  if (!audioBuffer) return;
  await resumeAudioCtxIfNeeded();
  if (isPlaying) { stopPlayback(); return; }
  // Smart default: if there's a selection, play the selection. Avoids the
  // common "I selected a clip but the main play button still played the
  // whole file from the start" frustration.
  if (selection) {
    const a = Math.min(selection.start, selection.end);
    const b = Math.max(selection.start, selection.end);
    if (b - a >= 0.1) { startPlayback(a, b); return; }
  }
  startPlayback(playStartOffset);
}

function tickPlayhead() {
  updateTimeUI();
  redrawOverlays();
  if (isPlaying) raf = requestAnimationFrame(tickPlayhead);
}

function updateTimeUI() {
  const t = currentPlayheadSec() || 0;
  timeEl.textContent = `${fmtTime(t)} / ${fmtTime(durationSec)}`;
}

// ---- Selection ----------------------------------------------------------
function updateSelectionUI() {
  if (!selection) {
    selInfoEl.textContent = "";
    playSelBtn.disabled = true;
    clearSelBtn.disabled = true;
    saveBtn.disabled = true;
    return;
  }
  const a = Math.min(selection.start, selection.end);
  const b = Math.max(selection.start, selection.end);
  selInfoEl.textContent = `selection ${fmtTime(a)}–${fmtTime(b)} (${(b - a).toFixed(2)}s)`;
  playSelBtn.disabled = false;
  clearSelBtn.disabled = false;
  saveBtn.disabled = false;
}

function attachSelectionHandlers(canvas) {
  // Pointer Events (over mouse events) so touch drag fires `pointermove`
  // continuously — `mousemove` only fires on mouse-up on touchscreens, so
  // the previous implementation could never paint a selection range.
  // touch-action:none on the canvases (in CSS) also stops the browser from
  // hijacking the gesture for pan/scroll.
  let dragging = false;
  let activePointerId = null;
  canvas.addEventListener("pointerdown", (e) => {
    if (!audioBuffer) return;
    activePointerId = e.pointerId;
    canvas.setPointerCapture(e.pointerId);
    const rect = canvas.getBoundingClientRect();
    const anchorSec = pxToSec(e.clientX - rect.left);
    selection = { start: anchorSec, end: anchorSec };
    dragging = true;
    updateSelectionUI();
    redrawOverlays();
    e.preventDefault();
  });
  canvas.addEventListener("pointermove", (e) => {
    if (!dragging || e.pointerId !== activePointerId) return;
    const rect = canvas.getBoundingClientRect();
    const sec = Math.max(0, Math.min(durationSec, pxToSec(e.clientX - rect.left)));
    selection.end = sec;
    updateSelectionUI();
    redrawOverlays();
  });
  const endDrag = (e) => {
    if (!dragging || (e && e.pointerId !== activePointerId)) return;
    dragging = false;
    if (activePointerId !== null) {
      try { canvas.releasePointerCapture(activePointerId); } catch (_) {}
    }
    activePointerId = null;
    // A trivially-short drag (single tap) collapses into a playhead seek
    // instead of a zero-width selection — feels more natural.
    if (selection && Math.abs(selection.end - selection.start) < 0.05) {
      playStartOffset = selection.start;
      if (isPlaying) startPlayback(playStartOffset);
      selection = null;
      updateSelectionUI();
      updateTimeUI();
      redrawOverlays();
    }
  };
  canvas.addEventListener("pointerup", endDrag);
  canvas.addEventListener("pointercancel", endDrag);
}

// ---- WAV export ---------------------------------------------------------
function encodeWAV(buffer, startSec, endSec) {
  const sr = buffer.sampleRate;
  const numCh = buffer.numberOfChannels;
  const startSample = Math.floor(startSec * sr);
  const endSample   = Math.floor(endSec   * sr);
  const length = Math.max(0, endSample - startSample);

  const bytesPerSample = 2; // 16-bit PCM
  const byteLength = 44 + length * numCh * bytesPerSample;
  const ab = new ArrayBuffer(byteLength);
  const view = new DataView(ab);

  // RIFF header
  writeStr(view, 0, "RIFF");
  view.setUint32(4, byteLength - 8, true);
  writeStr(view, 8, "WAVE");
  // fmt chunk
  writeStr(view, 12, "fmt ");
  view.setUint32(16, 16, true);          // chunk size
  view.setUint16(20, 1, true);            // PCM
  view.setUint16(22, numCh, true);
  view.setUint32(24, sr, true);
  view.setUint32(28, sr * numCh * bytesPerSample, true); // byte rate
  view.setUint16(32, numCh * bytesPerSample, true);      // block align
  view.setUint16(34, 16, true);                          // bits per sample
  // data chunk
  writeStr(view, 36, "data");
  view.setUint32(40, length * numCh * bytesPerSample, true);

  // Interleave + convert to int16
  const channelData = [];
  for (let c = 0; c < numCh; c++) channelData.push(buffer.getChannelData(c));
  let offset = 44;
  for (let i = 0; i < length; i++) {
    for (let c = 0; c < numCh; c++) {
      let s = channelData[c][startSample + i] || 0;
      s = Math.max(-1, Math.min(1, s));
      view.setInt16(offset, s < 0 ? s * 0x8000 : s * 0x7FFF, true);
      offset += 2;
    }
  }
  return ab;
}

function writeStr(view, off, str) {
  for (let i = 0; i < str.length; i++) view.setUint8(off + i, str.charCodeAt(i));
}

// ---- Display name + duration -------------------------------------------
async function reportDurationIfNeeded(name, sec, cached) {
  // Skip if the server already has the duration within ~half a second
  // (decoder vs. cached value sometimes disagree past the third decimal).
  if (cached && Math.abs(cached - sec) < 0.5) return;
  try {
    await fetch("/api/duration?name=" + encodeURIComponent(name)
                + "&seconds=" + encodeURIComponent(sec.toFixed(3)),
                { method: "POST" });
  } catch (e) {
    console.warn("duration cache failed", e);
  }
}

async function renameRecording() {
  if (!currentFile) return;
  const current = currentFile.display_name || "";
  const next = prompt(
      "Display name (blank to clear and fall back to the timestamp / clip range):",
      current);
  if (next === null) return;
  try {
    const res = await fetch("/api/rename?name=" + encodeURIComponent(currentFile.name)
                            + "&display=" + encodeURIComponent(next),
                            { method: "POST" });
    if (!res.ok) throw new Error("server " + res.status);
    const data = await res.json();
    currentFile.display_name = data.display_name || null;
    filenameEl.textContent = buildFilenameLabel();
    setStatus(currentFile.display_name
              ? ("renamed → " + currentFile.display_name)
              : "display name cleared");
    refreshFileList();
  } catch (e) {
    setStatus("rename failed: " + e.message);
  }
}

// ---- Markers ------------------------------------------------------------
async function persistMarkers() {
  if (!currentFile) return;
  try {
    const res = await fetch("/api/markers?audio=" +
                            encodeURIComponent(currentFile.name), {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ markers }),
    });
    if (!res.ok) throw new Error("server " + res.status);
  } catch (e) {
    setStatus("marker save failed: " + e.message);
  }
}

async function addMarkerAtPlayhead() {
  if (!audioBuffer || !currentFile) return;
  const t = currentPlayheadSec() || 0;
  const note = prompt(`Marker at ${fmtTime(t)}\nNote (blank ok):`, "");
  if (note === null) return;
  markers.push({ t_ms: Math.round(t * 1000), note: note.trim() });
  markers.sort((a, b) => a.t_ms - b.t_ms);
  await persistMarkers();
  redrawOverlays();
  setStatus("marker @ " + fmtTime(t));
}

async function editMarker(idx) {
  const m = markers[idx];
  if (!m) return;
  const t = m.t_ms / 1000;
  const result = prompt(
      `Marker ${idx + 1} at ${fmtTime(t)}\nEdit note (or type "delete" to remove):`,
      m.note || "");
  if (result === null) return;
  if (result.trim().toLowerCase() === "delete") {
    markers.splice(idx, 1);
    setStatus("marker " + (idx + 1) + " removed");
  } else {
    m.note = result.trim();
    setStatus("marker " + (idx + 1) + " saved");
  }
  await persistMarkers();
  redrawOverlays();
}

async function saveSelection() {
  if (!selection || !audioBuffer || !currentFile) return;
  const a = Math.min(selection.start, selection.end);
  const b = Math.max(selection.start, selection.end);
  // Default name uses the parent's stem so the new .wav slots in as a
  // child of the parent recording in the file list (server-side glob
  // matches `<parent_stem>*.wav`). The user gets to tweak it via prompt
  // — preserve the .wav extension if they strip it.
  let parentStem = currentFile.name.replace(/\.m4a$/, "").replace(/\.wav$/, "");
  // If the source is itself a clip, derive parent from the bit before
  // "_clip_" so the new save still slots under the original recording.
  const clipIdx = parentStem.indexOf("_clip_");
  if (clipIdx > 0) parentStem = parentStem.slice(0, clipIdx);
  // Fold the renamed display name in after the parent stem (before "_clip_")
  // so the saved clip still globs under its parent recording.
  const defaultName = nameWithDisplay(
      `${parentStem}_clip_${a.toFixed(1)}-${b.toFixed(1)}s.wav`,
      currentFile.display_name);
  const userName = prompt(
      `Save selection (${a.toFixed(1)}s – ${b.toFixed(1)}s, length ${(b - a).toFixed(1)}s) as:`,
      defaultName);
  if (userName == null || userName.trim() === "") {
    setStatus("save cancelled");
    return;
  }
  let fname = userName.trim();
  if (!fname.endsWith(".wav")) fname += ".wav";
  // Slashes / .. stripped server-side, but pre-empt the error here.
  fname = fname.replace(/[\\/]/g, "_");

  const wavBytes = encodeWAV(audioBuffer, a, b);
  const blob = new Blob([wavBytes], { type: "audio/wav" });
  setStatus("saving " + fname + "…");
  try {
    const res = await fetch("/api/save-clip?name=" + encodeURIComponent(fname), {
      method: "POST",
      headers: { "Content-Type": "audio/wav" },
      body: blob,
    });
    if (!res.ok) throw new Error("server " + res.status);
    const info = await res.json();
    setStatus("saved " + info.name + " (" + info.bytes + " bytes)");
    refreshFileList();
  } catch (e) {
    setStatus("save failed: " + e.message);
  }
}

// ---- Wire-up ------------------------------------------------------------
playBtn.addEventListener("click", togglePlay);
playSelBtn.addEventListener("click", async () => {
  if (!selection) return;
  await resumeAudioCtxIfNeeded();
  const a = Math.min(selection.start, selection.end);
  const b = Math.max(selection.start, selection.end);
  startPlayback(a, b);
});
clearSelBtn.addEventListener("click", () => {
  selection = null;
  updateSelectionUI();
  redrawOverlays();
});
saveBtn.addEventListener("click", saveSelection);
shareBtn.addEventListener("click", sharePackage);
deleteBtn.addEventListener("click", deleteRecording);
exportBtn.addEventListener("click", () => {
  if (currentFile) triggerExport(currentFile.name, currentFile.display_name);
});
addMarkerBtn.addEventListener("click", addMarkerAtPlayhead);
renameBtn.addEventListener("click", renameRecording);
// Keyboard: 'm' drops a marker at the playhead, mirroring the watch's UP
// button. Skip when typing in a prompt-style input.
window.addEventListener("keydown", (e) => {
  if (e.key !== "m" && e.key !== "M") return;
  if (e.target && e.target.matches && e.target.matches("input, textarea")) return;
  if (!audioBuffer) return;
  e.preventDefault();
  addMarkerAtPlayhead();
});
refreshBtn.addEventListener("click", refreshFileList);
// The Sync button drives `adb push/pull` against the phone, which only
// makes sense from the laptop side. The Android WebView injects
// GlanceBridge; when that's present we're already on the phone with
// direct file access, so sync is meaningless — hide the button.
if (typeof GlanceBridge !== "undefined" && GlanceBridge.isAvailable && GlanceBridge.isAvailable()) {
  syncBtn.hidden = true;
}
syncBtn.addEventListener("click", async () => {
  syncBtn.disabled = true;
  setStatus("syncing with phone…");
  try {
    const res = await fetch("/api/sync", { method: "POST" });
    const data = await res.json();
    if (!data.ok) {
      setStatus("sync failed: " + (data.error || "unknown"));
    } else {
      const parts = [];
      if (data.pulled.length) parts.push("pulled " + data.pulled.length);
      if (data.pushed.length) parts.push("pushed " + data.pushed.length);
      if (data.merged.length) parts.push("merged " + data.merged.length);
      setStatus("sync ok · " + (parts.join(" · ") || "no changes"));
      refreshFileList();
      // If a recording is open, re-fetch its markers in case the merge
      // brought in something new from the phone.
      if (currentFile) {
        const md = await fetch("/api/markers?audio=" +
                               encodeURIComponent(currentFile.name)).then(r => r.json());
        markers = (md && md.markers) || [];
        redrawOverlays();
      }
    }
  } catch (e) {
    setStatus("sync failed: " + e.message);
  } finally {
    syncBtn.disabled = false;
  }
});
backBtn.addEventListener("click", () => {
  stopPlayback();
  document.body.classList.remove("editor-open");
  backBtn.hidden = true;
  // Wider screens still show the editor — that's fine; clearing the list
  // selection visually communicates "no longer focused on this recording".
  document.querySelectorAll("#files li.active").forEach(x => x.classList.remove("active"));
});

async function deleteRecording() {
  if (!currentFile) return;
  const { stamp } = parseRecordingName(currentFile.name);
  if (!confirm(`Delete ${stamp}? The .m4a and its sidecar are removed permanently.`)) {
    return;
  }
  setStatus("deleting " + currentFile.name + "…");
  try {
    const res = await fetch("/api/delete?name=" + encodeURIComponent(currentFile.name),
                            { method: "POST" });
    if (!res.ok) throw new Error("server " + res.status);
    setStatus("deleted " + currentFile.name);
    stopPlayback();
    currentFile = null;
    audioBuffer = null;
    sidecar = null;
    durationSec = 0;
    selection = null;
    // Disable buttons and reset the editor pane.
    [playBtn, playSelBtn, clearSelBtn, saveBtn, shareBtn, deleteBtn, exportBtn,
     addMarkerBtn, renameBtn].forEach(b => b.disabled = true);
    markers = [];
    filenameEl.textContent = "No recording loaded";
    durationEl.textContent = "";
    bpmAxisEl.textContent = "";
    redrawAll();
    refreshFileList();
    // On phone, drop back to the list view.
    if (window.matchMedia("(max-width: 700px)").matches) {
      document.body.classList.remove("editor-open");
      backBtn.hidden = true;
    }
  } catch (e) {
    setStatus("delete failed: " + e.message);
  }
}

async function sharePackage() {
  if (!currentFile) return;
  setStatus("building package…");
  try {
    const res = await fetch("/api/package?name=" + encodeURIComponent(currentFile.name),
                            { method: "POST" });
    if (!res.ok) throw new Error("server " + res.status);
    const info = await res.json();
    // On phone: hand the zip to Android's share sheet so the user can
    // pick Gmail / Messages / Drive / etc. without leaving the editor.
    if (typeof GlanceBridge !== "undefined" && GlanceBridge.isAvailable && GlanceBridge.isAvailable()) {
      if (GlanceBridge.shareFile(info.name)) {
        setStatus(`share sheet open · ${info.name} (${(info.bytes/1024).toFixed(1)} KB)`);
        return;
      }
    }
    // Laptop: surface the on-disk path so the user can attach it themselves.
    const where = info.path || info.abs_path || info.name;
    setStatus(`package ready · ${(info.bytes/1024).toFixed(1)} KB · ${where}`);
  } catch (e) {
    setStatus("package failed: " + e.message);
  }
}
window.addEventListener("resize", () => {
  if (audioBuffer) { resizeCanvases(); redrawAll(); }
});
attachSelectionHandlers(waveCanvas);

resizeCanvases();
refreshFileList();
