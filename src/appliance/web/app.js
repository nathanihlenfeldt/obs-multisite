/*
 * app.js — the operator interface.
 *
 * One rule runs through all of it: the language is the language of a service,
 * not of a video pipeline. Times are clock times, durations are minutes, and
 * nothing here ever says "segment", "buffer" or "live edge". The audience is a
 * volunteer who has been handed a tablet, not the person who wrote it.
 *
 * The page polls one status document and draws everything from it, so no two
 * parts of the interface can ever disagree about what is happening.
 */

'use strict';

const $  = (sel) => document.querySelector(sel);
const $$ = (sel) => Array.from(document.querySelectorAll(sel));

// Matches multisite::RoomState.
const ROOM = { UNKNOWN: 0, OFFLINE: 1, LIVE: 2, ENDED: 3, INTERRUPTED: 4 };
// Matches multisite::EventState.
const EVENT = { UNKNOWN: 0, LIVE: 1, RECORDING: 2, INTERRUPTED: 3 };

let status = null;
let settings = null;
let systemInfo = null;
let pollTimer = null;
// The offset between this device's clock and the box's, so a phone with the
// wrong time still shows the same reading as the box does.
let clockSkewMs = 0;

/* ── Small helpers ───────────────────────────────────────────────────────── */

function hhmmss(ms) {
  if (!ms) return '--:--:--';
  const d = new Date(ms);
  return d.toLocaleTimeString('en-GB', { hour12: false });
}

function shortDateTime(ms) {
  if (!ms) return '';
  return new Date(ms).toLocaleString('en-GB', {
    weekday: 'short', day: 'numeric', month: 'short',
    hour: '2-digit', minute: '2-digit', hour12: false,
  });
}

// Durations as somebody would say them out loud.
function spoken(seconds) {
  seconds = Math.max(0, Math.round(seconds));
  if (seconds < 60) return seconds + ' s';
  const m = Math.floor(seconds / 60);
  if (m < 60) return m + ' min';
  const h = Math.floor(m / 60);
  const rem = m % 60;
  return rem ? `${h} h ${rem} min` : `${h} h`;
}

function elapsed(ms) {
  const s = Math.max(0, Math.round(ms / 1000));
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  const sec = s % 60;
  const pad = (n) => String(n).padStart(2, '0');
  return h ? `${h}:${pad(m)}:${pad(sec)}` : `${m}:${pad(sec)}`;
}

function bytes(n) {
  if (!n) return '—';
  const gb = n / 1e9;
  return gb >= 1 ? gb.toFixed(1) + ' GB' : (n / 1e6).toFixed(0) + ' MB';
}

async function api(method, path, body) {
  const opts = { method, headers: {} };
  if (body !== undefined) {
    opts.headers['Content-Type'] = 'application/json';
    opts.body = JSON.stringify(body);
  }
  const res = await fetch(path, opts);
  const text = await res.text();
  let data = null;
  try { data = text ? JSON.parse(text) : null; } catch (e) { /* not JSON */ }
  if (!res.ok) {
    const message = (data && data.error) || text || ('HTTP ' + res.status);
    throw new Error(message);
  }
  return data;
}

function notify(message, isError) {
  const box = $('#alert');
  if (!message) { box.hidden = true; return; }
  box.hidden = false;
  box.textContent = message;
  box.style.borderColor = isError ? 'var(--danger)' : 'var(--line)';
}

/* ── Status ──────────────────────────────────────────────────────────────── */

async function refreshStatus() {
  try {
    status = await api('GET', '/api/status');
    clockSkewMs = status.now_ms - Date.now();
    drawStatus();
  } catch (e) {
    $('#state').textContent = 'No answer';
    $('#state').className = 'state offline';
    $('#room').textContent = 'Cannot reach the player — is it still powered?';
  }
}

// What the banner says. A live service, a recording of a past one, and a
// broadcast that has just closed are three different things to an operator and
// must not read the same.
function bannerFor(s) {
  if (!s.configured) return { text: 'Not set up', cls: 'offline' };
  switch (s.room_state) {
    case ROOM.LIVE: return { text: 'Live', cls: 'live' };
    case ROOM.ENDED:
      return s.was_live
        ? { text: 'Broadcast ended', cls: 'ended' }
        : { text: 'Recording', cls: 'recording' };
    case ROOM.INTERRUPTED:
      return { text: 'Interrupted', cls: 'ended' };
    case ROOM.OFFLINE: return { text: 'Nothing on air', cls: 'offline' };
    default: return { text: 'Looking…', cls: 'offline' };
  }
}

function drawStatus() {
  const s = status;
  const banner = bannerFor(s);
  $('#state').textContent = banner.text;
  $('#state').className = 'state ' + banner.cls;

  let room = s.room_id || '';
  if (s.pinned_event_id) room += ' · playing a past service';
  if (s.live_elsewhere) room += ' · something is live now';
  $('#room').textContent = room;

  $('#lock').textContent = s.locked ? 'Locked' : 'Lock';
  $('#lock').className = 'lock' + (s.locked ? ' on' : '');

  // ── The reading that matters ───────────────────────────────────────────
  const clock = $('#clock');
  clock.textContent = hhmmss(s.playhead_ms);
  // While a jump is in flight the time shown is where playback is GOING, not
  // where the picture is. Say so rather than letting it read as fact.
  clock.classList.toggle('provisional', !!s.seek_target_ms);

  let sub;
  if (!s.configured) {
    sub = 'Open Settings and enter the storage details.';
  } else if (s.loading) {
    sub = 'Loading…';
  } else if (!s.playing) {
    sub = 'Not going out. Downloading in the background.';
  } else if (s.paused) {
    sub = 'Holding the picture. Still downloading.';
  } else if (s.buffering) {
    sub = 'Gathering enough to start…';
  } else if (s.ended) {
    // A finished recording has an end, so it reports position out of length
    // the way a media player does. "Behind live" means nothing here.
    const into = s.started_ms ? s.playhead_ms - s.started_ms : 0;
    sub = `${elapsed(into)} of ${elapsed(s.total_ms)}` +
          (s.at_end ? ' · at the end' : '');
  } else if (s.behind_live_s < 3) {
    sub = 'Right up to date';
  } else {
    sub = spoken(s.behind_live_s) + ' behind the main site';
  }
  if (s.current_marker) sub += ' · ' + s.current_marker;
  $('#position').textContent = sub;

  drawTimeline(s);
  drawTransport(s);
  drawCues(s);
  drawReadout(s);

  notify(s.last_error || '', true);
}

function drawTransport(s) {
  const live = !s.ended;
  $('#btn-play').disabled = !s.configured || (s.playing && !s.paused);
  $('#btn-hold').disabled = !s.playing || s.paused;
  $('#btn-stop').disabled = !s.playing;
  $('#btn-live').disabled = !live || !s.configured;
  $('#btn-play').textContent = s.paused ? 'Continue' : 'Play';
  $('#btn-live').textContent = live ? 'Catch up to now' : 'Go to the end';
  $$('.jog button').forEach((b) => { b.disabled = !s.configured; });
}

// The bar spans what storage still holds. For a finished recording that is the
// whole service and it must not move; while live the right-hand edge is the
// live edge and necessarily grows.
function drawTimeline(s) {
  const from = s.earliest_ms || s.started_ms;
  const to = s.ended ? (s.end_ms || s.live_ms) : s.live_ms;
  const el = $('#timeline');
  if (!from || !to || to <= from) {
    el.dataset.from = ''; el.dataset.to = '';
    $('#tl-stored').style.cssText = '';
    $('#tl-downloaded').innerHTML = '';
    $('#tl-markers').innerHTML = '';
    $('#tl-head').style.left = '0';
    $('#tl-left').textContent = '';
    $('#tl-right').textContent = '';
    return;
  }
  el.dataset.from = String(from);
  el.dataset.to = String(to);
  const span = to - from;
  const pct = (ms) => Math.max(0, Math.min(100, ((ms - from) / span) * 100));

  $('#tl-stored').style.left = '0';
  $('#tl-stored').style.right = '0';

  // What is actually on this box's disk — the part that would keep playing if
  // the connection died.
  $('#tl-downloaded').innerHTML = (s.cached_spans || []).map((sp) => {
    const a = pct(sp.from_ms), b = pct(sp.to_ms);
    return `<i style="left:${a}%;width:${Math.max(0.4, b - a)}%"></i>`;
  }).join('');

  $('#tl-markers').innerHTML = (s.markers || [])
    .filter((m) => m.at_ms >= from && m.at_ms <= to)
    .map((m) => `<i style="left:${pct(m.at_ms)}%" title="${escapeHtml(m.label)}"></i>`)
    .join('');

  $('#tl-head').style.left = pct(s.playhead_ms) + '%';
  $('#tl-left').textContent = hhmmss(from);
  $('#tl-right').textContent = s.ended ? hhmmss(to) : hhmmss(to) + ' (now)';
}

function drawCues(s) {
  const box = $('#cues');
  const markers = s.markers || [];
  if (!markers.length) { box.innerHTML = ''; return; }
  box.innerHTML = markers.map((m) => {
    const passed = m.at_ms && s.playhead_ms && m.at_ms <= s.playhead_ms;
    return `<button class="${passed ? 'passed' : ''}" data-marker="${escapeHtml(m.id)}">
              ${escapeHtml(m.label)} <span class="muted">${hhmmss(m.at_ms)}</span>
            </button>`;
  }).join('');
}

function drawReadout(s) {
  const cells = [];
  const push = (k, v, warn) =>
    cells.push(`<div class="cell${warn ? ' warn' : ''}"><div class="k">${k}</div>
                <div class="v">${v}</div></div>`);

  // The reliability figure that actually matters mid-service: how long this
  // campus could keep broadcasting if its connection died right now.
  push('Could keep going for', spoken(s.buffered_ahead_s),
       s.playing && !s.paused && s.buffered_ahead_s < 30);
  push('Ready on disk', s.cached_segments ? spoken(s.cached_segments * 6) : '—');
  if (!s.ended) push('Behind the main site', spoken(s.behind_live_s));
  push('Picture', s.video_width ? `${s.video_width}×${s.video_height}` : '—');
  push('Sound', s.audio_channels ? s.audio_channels + ' channels' : '—');
  push('Going out on', escapeHtml(s.output_description || '—'),
       !s.video_output_ok);
  if (s.download_failures || s.checksum_failures)
    push('Re-fetched', String(s.download_failures + s.checksum_failures), true);
  $('#readout').innerHTML = cells.join('');
}

function escapeHtml(s) {
  return String(s == null ? '' : s).replace(/[&<>"']/g, (c) => ({
    '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;',
  }[c]));
}

/* ── Controls ────────────────────────────────────────────────────────────── */

async function control(path) {
  try {
    status = await api('POST', path);
    drawStatus();
  } catch (e) {
    notify(e.message, true);
  }
}

$('#btn-play').onclick = () => control(status && status.paused ? '/api/continue' : '/api/play');
$('#btn-hold').onclick = () => control('/api/hold');
$('#btn-stop').onclick = () => control('/api/stop');
$('#btn-live').onclick = () => control('/api/catch-up');

$$('.jog button').forEach((b) => {
  b.onclick = () => control('/api/jog?seconds=' + encodeURIComponent(b.dataset.jog));
});

$('#btn-delay').onclick = () => {
  const minutes = Number($('#delay-min').value || 0);
  control('/api/delay?seconds=' + (minutes * 60));
};

$('#cues').addEventListener('click', (e) => {
  const b = e.target.closest('button[data-marker]');
  if (b) control('/api/marker?id=' + encodeURIComponent(b.dataset.marker));
});

$('#lock').onclick = async () => {
  const on = !(status && status.locked);
  try {
    status = await api('POST', '/api/lock?on=' + (on ? '1' : '0'));
    drawStatus();
  } catch (e) { notify(e.message, true); }
};

// Clicking the timeline goes to that moment. Hovering reports what is under
// the cursor first, so a click is never a guess.
const timeline = $('#timeline');
timeline.addEventListener('click', (e) => {
  const from = Number(timeline.dataset.from), to = Number(timeline.dataset.to);
  if (!from || !to) return;
  const r = timeline.getBoundingClientRect();
  const at = from + ((e.clientX - r.left) / r.width) * (to - from);
  control('/api/seek?ms=' + Math.round(at));
});
timeline.addEventListener('pointermove', (e) => {
  const from = Number(timeline.dataset.from), to = Number(timeline.dataset.to);
  if (!from || !to) { $('#tl-hover').textContent = ''; return; }
  const r = timeline.getBoundingClientRect();
  const at = from + ((e.clientX - r.left) / r.width) * (to - from);
  $('#tl-hover').textContent = hhmmss(at);
});
timeline.addEventListener('pointerleave', () => { $('#tl-hover').textContent = ''; });

/* ── Services (the event list) ───────────────────────────────────────────── */

async function refreshEvents() {
  try {
    const listing = await api('GET', '/api/events');
    const ul = $('#events');
    const note = $('#events-note');

    if (listing.loading && !listing.listed_once) note.textContent = 'Looking…';
    else if (listing.error) note.textContent = listing.error;
    else if (listing.fallback_scan) note.textContent =
      'These were found by scanning; older services may take a moment.';
    else if (listing.skipped) note.textContent =
      listing.skipped + ' could not be read and are not shown.';
    else note.textContent = '';

    if (!listing.events.length) {
      ul.innerHTML = listing.listed_once
        ? '<li class="muted">No services stored for this room yet.</li>' : '';
      return;
    }

    const playing = status ? (status.event_id || '') : '';
    ul.innerHTML = listing.events.map((e) => {
      const badge = e.state === EVENT.LIVE ? ['live', 'On air now']
                  : e.state === EVENT.INTERRUPTED ? ['interrupted', 'Cut short']
                  : e.state === EVENT.RECORDING ? ['', 'Recording']
                  : ['', 'Unknown'];
      const isPlaying = e.event_id === playing;
      return `<li class="${isPlaying ? 'playing' : ''}">
        <span class="badge ${badge[0]}">${badge[1]}</span>
        <span class="when">${escapeHtml(shortDateTime(e.started_ms))}
          <small>${e.duration_s ? spoken(e.duration_s) + ' long' : ''}</small></span>
        <button data-load="${escapeHtml(e.event_id)}">
          ${isPlaying ? 'Playing' : 'Load'}</button>
      </li>`;
    }).join('');
  } catch (e) {
    $('#events-note').textContent = e.message;
  }
}

$('#events').addEventListener('click', async (e) => {
  const b = e.target.closest('button[data-load]');
  if (!b) return;
  await control('/api/load?event=' + encodeURIComponent(b.dataset.load));
  refreshEvents();
});
$('#btn-refresh-events').onclick = async () => {
  $('#events-note').textContent = 'Looking…';
  await api('POST', '/api/events/refresh');
  setTimeout(refreshEvents, 1200);
};
$('#btn-follow-live').onclick = async () => {
  await control('/api/follow-live');
  refreshEvents();
};

/* ── Settings ────────────────────────────────────────────────────────────── */

async function loadSettings() {
  settings = await api('GET', '/api/config');
  const set = (sel, value) => { const el = $(sel); if (el) el.value = value; };
  set('#c-room', settings.room_id);
  set('#c-bucket', settings.bucket);
  set('#c-account', settings.r2_account_id);
  set('#c-endpoint', settings.endpoint_host);
  set('#c-region', settings.region);
  set('#c-key', settings.access_key_id);
  set('#c-secret', settings.secret_access_key);
  set('#c-buffer', settings.buffer_minutes);
  set('#c-prebuffer', settings.prebuffer_segments);
  set('#c-cache', settings.cache_dir);
  set('#c-idle', settings.idle_mode);
  set('#c-channels', settings.audio_channels);
  set('#c-audio-on', String(settings.audio_enabled));
  set('#c-autoplay', String(settings.auto_play));
  $('#idle-image-field').hidden = settings.idle_mode !== 'image';
  set('#c-idle-image', settings.idle_image_path);
  await loadOutputChoices();
}

$('#c-idle').addEventListener('change', (e) => {
  $('#idle-image-field').hidden = e.target.value !== 'image';
});

// The display and sound-card pickers list what the box actually has, so a
// setting cannot be typed that the hardware will refuse.
async function loadOutputChoices() {
  try {
    const outputs = await api('GET', '/api/outputs');
    const disp = $('#c-display');
    disp.innerHTML = '<option value="">First connected screen</option>' +
      (outputs.displays || []).map((d) =>
        `<option value="${escapeHtml(d.connector)}"${
          d.connector === settings.connector ? ' selected' : ''}>
           ${escapeHtml(d.connector)}${d.connected ? '' : ' (nothing plugged in)'}
         </option>`).join('');

    const modes = [];
    (outputs.displays || []).forEach((d) => {
      if (settings.connector && d.connector !== settings.connector) return;
      (d.modes || []).forEach((m) => modes.push(m));
    });
    const sel = $('#c-mode');
    sel.innerHTML = '<option value="0x0x0">Whatever the screen prefers</option>' +
      modes.map((m) => {
        const key = `${m.width}x${m.height}x${Math.round(m.refresh_mhz / 1000)}`;
        const chosen = settings.out_width === m.width &&
                       settings.out_height === m.height &&
                       settings.out_fps === Math.round(m.refresh_mhz / 1000);
        return `<option value="${key}"${chosen ? ' selected' : ''}>
                  ${m.width}×${m.height} at ${(m.refresh_mhz / 1000).toFixed(2)} Hz
                  ${m.preferred ? ' — the screen prefers this' : ''}</option>`;
      }).join('');

    const alsa = $('#c-alsa');
    alsa.innerHTML = '<option value="default">Follow the system</option>' +
      (outputs.audio_devices || []).map((d) =>
        `<option value="${escapeHtml(d.id)}"${
          d.id === settings.alsa_device ? ' selected' : ''}>
           ${escapeHtml(d.description)}</option>`).join('');
  } catch (e) {
    /* An older box, or no outputs to list. The text fields still work. */
  }
}

$('#settings-form').addEventListener('submit', async (e) => {
  e.preventDefault();
  const note = $('#settings-note');
  note.textContent = 'Saving…';

  const mode = ($('#c-mode').value || '0x0x0').split('x').map(Number);
  const body = {
    room_id: $('#c-room').value.trim(),
    bucket: $('#c-bucket').value.trim(),
    r2_account_id: $('#c-account').value.trim(),
    endpoint_host: $('#c-endpoint').value.trim(),
    region: $('#c-region').value.trim() || 'auto',
    access_key_id: $('#c-key').value.trim(),
    secret_access_key: $('#c-secret').value,
    buffer_minutes: Number($('#c-buffer').value),
    prebuffer_segments: Number($('#c-prebuffer').value),
    cache_dir: $('#c-cache').value.trim(),
    connector: $('#c-display').value,
    out_width: mode[0] || 0,
    out_height: mode[1] || 0,
    out_fps: mode[2] || 0,
    idle_mode: $('#c-idle').value,
    idle_image_path: $('#c-idle-image').value.trim(),
    audio_enabled: $('#c-audio-on').value === 'true',
    alsa_device: $('#c-alsa').value,
    audio_channels: Number($('#c-channels').value),
    auto_play: $('#c-autoplay').value === 'true',
  };

  try {
    settings = await api('PUT', '/api/config', body);
    note.textContent = 'Saved.';
    await loadSettings();
    setTimeout(() => { note.textContent = ''; }, 3000);
  } catch (err) {
    note.textContent = err.message;
  }
});

/* ── This box ────────────────────────────────────────────────────────────── */

async function loadSystem() {
  try {
    systemInfo = await api('GET', '/api/system');
  } catch (e) { return; }
  const s = systemInfo;

  const rows = [];
  const add = (k, v) => rows.push(`<dt>${k}</dt><dd>${escapeHtml(v)}</dd>`);

  add('Name', s.hostname);
  (s.interfaces || []).forEach((n) => {
    if (n.ipv4) add('Address (' + n.name + ')', n.ipv4 + ':' + location.port);
  });
  add('Player version', s.version);
  if (s.model) add('Hardware', s.model);
  if (s.os_version) add('System', s.os_version);
  add('Clock', s.time.local_time + ' (' + s.time.timezone + ')');
  add('Network time', s.time.ntp_enabled
      ? (s.time.ntp_synchronised ? 'on, in step' : 'on, not yet in step') : 'off');
  add('Running for', spoken(s.uptime_s));
  if (s.cpu_temp_c) add('Temperature', s.cpu_temp_c.toFixed(1) + ' °C');
  if (s.disk && s.disk.total_bytes)
    add('Cache disk', `${bytes(s.disk.free_bytes)} free of ${bytes(s.disk.total_bytes)}` +
        (s.disk.is_sd_card ? ' — this is the SD card' : ''));
  $('#facts').innerHTML = rows.join('');

  // Both of these explain a service that stutters, and neither is visible any
  // other way.
  const warnings = [];
  if (s.under_voltage) warnings.push('The power supply is not keeping up.');
  if (s.throttled) warnings.push('The box is running hot and slowing itself down.');
  if (s.disk && s.disk.is_sd_card)
    warnings.push('The cache is on the SD card, which it will wear out. ' +
                  'Move it to a USB SSD.');
  if (warnings.length) notify(warnings.join(' '), true);

  $('#s-ntp').value = String(!!s.time.ntp_enabled);

  if (!$('#s-tz').options.length) {
    try {
      const tz = await api('GET', '/api/system/timezones');
      $('#s-tz').innerHTML = tz.timezones
        .map((z) => `<option${z === s.time.timezone ? ' selected' : ''}>${escapeHtml(z)}</option>`)
        .join('');
    } catch (e) { /* leave it empty */ }
  }
}

$('#btn-tz').onclick = async () => {
  try {
    await api('POST', '/api/system/time?timezone=' + encodeURIComponent($('#s-tz').value));
    loadSystem();
  } catch (e) { notify(e.message, true); }
};
$('#btn-ntp').onclick = async () => {
  try {
    await api('POST', '/api/system/time?ntp=' + $('#s-ntp').value);
    loadSystem();
  } catch (e) { notify(e.message, true); }
};
$('#btn-settime').onclick = async () => {
  try {
    await api('POST', '/api/system/time?epoch_ms=' + Date.now());
    loadSystem();
  } catch (e) { notify(e.message, true); }
};

// The three that interrupt a service get a confirmation. Everything else is
// instant on purpose.
function confirmThen(question, path) {
  return async () => {
    if (!window.confirm(question)) return;
    try { await api('POST', path); notify('Asked the box to do that.', false); }
    catch (e) { notify(e.message, true); }
  };
}
$('#btn-restart').onclick =
  confirmThen('Restart the player? The picture will go away for a few seconds.',
              '/api/system/restart');
$('#btn-reboot').onclick =
  confirmThen('Reboot the box? It will be off air for about a minute.',
              '/api/system/reboot');
$('#btn-shutdown').onclick =
  confirmThen('Shut down? Somebody will have to press the power button to ' +
              'bring it back.', '/api/system/shutdown');

/* ── Log ─────────────────────────────────────────────────────────────────── */

async function refreshLog() {
  try {
    const data = await api('GET', '/api/log?lines=200');
    const el = $('#log');
    const atBottom = el.scrollTop + el.clientHeight >= el.scrollHeight - 40;
    el.innerHTML = data.lines.map((l) => {
      const t = new Date(l.at_ms).toLocaleTimeString('en-GB', { hour12: false });
      return `<span class="${l.level}">${t}  ${escapeHtml(l.text)}</span>`;
    }).join('\n');
    if (atBottom) el.scrollTop = el.scrollHeight;
  } catch (e) { /* the box will be back */ }
}
$('#btn-log-refresh').onclick = refreshLog;

/* ── Preview ─────────────────────────────────────────────────────────────── */
//
// Deliberately decoupled from the output: it may lag, it may be one frame a
// second, and it can be watched while the picture is held. Lining up a cue is
// exactly when those must not be the same thing.

let previewTimer = null;

function stopPreview() {
  if (previewTimer) { clearInterval(previewTimer); previewTimer = null; }
  $('#preview-img').removeAttribute('src');
}

function startPreview() {
  stopPreview();
  const rate = Number($('#preview-rate').value || 0);
  if (!rate || !$('#preview-box').open) return;
  const img = $('#preview-img');
  const tick = () => {
    // A cache-busting parameter rather than a stream, so a dropped connection
    // costs one frame instead of the whole preview.
    img.src = '/preview.jpg?t=' + Date.now();
  };
  img.onload = () => { $('#preview-none').hidden = true; };
  img.onerror = () => { $('#preview-none').hidden = false; };
  tick();
  previewTimer = setInterval(tick, 1000 / rate);
}

$('#preview-rate').addEventListener('change', startPreview);
$('#preview-box').addEventListener('toggle', startPreview);

/* ── Tabs and the poll loop ──────────────────────────────────────────────── */

$$('.tab').forEach((tab) => {
  tab.onclick = () => {
    $$('.tab').forEach((t) => t.classList.toggle('is-on', t === tab));
    $$('.panel').forEach((p) =>
      p.classList.toggle('is-on', p.id === 'tab-' + tab.dataset.tab));
    if (tab.dataset.tab === 'events') refreshEvents();
    if (tab.dataset.tab === 'settings') loadSettings();
    if (tab.dataset.tab === 'system') loadSystem();
    if (tab.dataset.tab === 'log') refreshLog();
    // The preview only runs on the tab that shows it.
    if (tab.dataset.tab === 'play') startPreview(); else stopPreview();
  };
});

function startPolling() {
  if (pollTimer) clearInterval(pollTimer);
  refreshStatus();
  pollTimer = setInterval(() => {
    refreshStatus();
    if ($('#tab-log').classList.contains('is-on') && $('#log-follow').checked)
      refreshLog();
  }, 500);
}

// A tablet left on a music stand should stop asking while its screen is off,
// and pick straight back up when somebody looks at it.
document.addEventListener('visibilitychange', () => {
  if (document.hidden) {
    if (pollTimer) { clearInterval(pollTimer); pollTimer = null; }
    stopPreview();
  } else {
    startPolling();
    if ($('#tab-play').classList.contains('is-on')) startPreview();
  }
});

startPolling();
loadSettings().catch(() => {});
refreshEvents();
