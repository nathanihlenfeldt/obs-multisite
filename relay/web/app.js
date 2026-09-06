// app.js — the operator's page.
//
// Polls rather than holding a socket open: the status is small, once a second
// is plenty, and a page that reconnects by itself after the phone sleeps is
// worth more here than immediacy.

const $ = (s) => document.querySelector(s);
const $$ = (s) => Array.from(document.querySelectorAll(s));

let audioLabels = [];
let lastStatus = null;

// ── tabs ────────────────────────────────────────────────────────────────────
$$('.tab').forEach((t) => {
  t.onclick = () => {
    $$('.tab').forEach((x) => x.classList.toggle('is-on', x === t));
    $$('.panel').forEach((p) =>
      p.classList.toggle('is-on', p.id === 'tab-' + t.dataset.tab));
    if (t.dataset.tab === 'log') refreshLog();
  };
});

// ── helpers ─────────────────────────────────────────────────────────────────
async function api(method, path, body) {
  const res = await fetch(path, {
    method,
    headers: body ? { 'Content-Type': 'application/json' } : {},
    body: body ? JSON.stringify(body) : undefined,
  });
  let data = {};
  try { data = await res.json(); } catch (e) { /* empty body is fine */ }
  if (!res.ok) throw new Error(data.error || 'Something went wrong.');
  return data;
}

function duration(s) {
  if (s < 60) return s + 's';
  const m = Math.floor(s / 60), h = Math.floor(m / 60);
  if (h > 0) return h + 'h ' + (m % 60) + 'm';
  return m + 'm ' + (s % 60) + 's';
}

function rate(kbps) {
  if (!kbps || kbps < 1) return '—';
  if (kbps >= 1000) return (kbps / 1000).toFixed(1) + ' Mbps';
  return Math.round(kbps) + ' kbps';
}

function behind(s) {
  if (s === undefined || s < 0) return '—';
  if (s < 90) return Math.round(s) + 's';
  return Math.round(s / 60) + ' min';
}

function esc(t) {
  const d = document.createElement('div');
  d.textContent = t == null ? '' : String(t);
  return d.innerHTML;
}

// ── status ──────────────────────────────────────────────────────────────────
async function refresh() {
  let s;
  try {
    s = await api('GET', '/api/status');
  } catch (e) {
    $('#room-state').textContent = 'Cannot reach the relay';
    $('#room-detail').textContent = '';
    return;
  }
  lastStatus = s;
  audioLabels = s.audio_labels || [];

  $('#room-state').textContent = s.room_state_text || 'Nothing is on air';
  const bits = [];
  if (s.room_id) bits.push('Feed: ' + s.room_id);
  if (s.video) bits.push(s.video);
  $('#room-detail').textContent = bits.join(' · ');

  // One warning line, showing whichever problem actually stops a stream.
  const warn = $('#warning');
  let message = '';
  if (!s.storage_configured) {
    message = 'Storage is not set up yet. Open Settings and fill in the '
            + 'bucket details from the main site.';
  } else if (s.storage_error) {
    message = s.storage_error;
  } else if (s.cannot_send_reason) {
    message = s.cannot_send_reason;
  }
  warn.hidden = !message;
  warn.textContent = message;

  renderDestinations(s.destinations || []);

  const anyLive = (s.destinations || []).some((d) => d.live);
  $('#totals').hidden = !anyLive;
  $('#total-rate').textContent = rate(s.total_out_kbps);
}

function renderDestinations(list) {
  const host = $('#destinations');
  if (!list.length) {
    host.innerHTML = '<div class="empty">Nothing is being sent anywhere yet.</div>';
    return;
  }
  host.innerHTML = list.map((d) => `
    <div class="card">
      <div class="dest-head">
        <span class="dest-name">${esc(d.name)}</span>
        <span class="pill ${esc(d.state)}">${esc(d.state_text)}</span>
      </div>
      ${d.detail ? `<div class="dest-detail">${esc(d.detail)}</div>` : ''}
      ${d.live ? `
      <div class="stats">
        <div>${esc(duration(d.uptime_s))}<span>on air</span></div>
        <div>${esc(rate(d.bitrate_kbps))}<span>going out</span></div>
        <div>${esc(behind(d.behind_live_s))}<span>behind the service</span></div>
        ${d.restarts ? `<div>${d.restarts}<span>reconnections</span></div>` : ''}
      </div>` : ''}
      ${d.error ? `<div class="dest-error">${esc(d.error)}</div>` : ''}
      <div class="row">
        ${d.enabled
          ? `<button class="stop" data-stop="${d.id}">Stop sending</button>`
          : `<button class="primary" data-start="${d.id}">Start sending</button>`}
        <button class="danger" data-del="${d.id}" ${d.enabled ? 'disabled' : ''}>Remove</button>
      </div>
    </div>`).join('');

  host.querySelectorAll('[data-start]').forEach((b) => {
    b.onclick = async () => {
      b.disabled = true;
      try {
        await api('POST', '/api/destinations/start?id=' + b.dataset.start);
      } catch (e) {
        alert(e.message);
      }
      refresh();
    };
  });
  host.querySelectorAll('[data-stop]').forEach((b) => {
    b.onclick = async () => {
      b.disabled = true;
      try { await api('POST', '/api/destinations/stop?id=' + b.dataset.stop); }
      catch (e) { alert(e.message); }
      refresh();
    };
  });
  host.querySelectorAll('[data-del]').forEach((b) => {
    b.onclick = async () => {
      if (!confirm('Remove this destination? It will stop being sent to.')) return;
      try { await api('POST', '/api/destinations/delete?id=' + b.dataset.del); }
      catch (e) { alert(e.message); }
      refresh();
    };
  });
}

// ── adding a destination ────────────────────────────────────────────────────
$('#add-open').onclick = () => {
  const sel = $('#add-audio');
  // Offer the names the main site published. With none yet, the operator can
  // still add the destination and the main mix is used.
  sel.innerHTML = audioLabels.length
    ? audioLabels.map((l) => `<option value="${esc(l)}">${esc(l)}</option>`).join('')
    : '<option value="">The main mix</option>';
  $('#add-form').hidden = false;
  $('#add-open').hidden = true;
};

$('#add-cancel').onclick = () => {
  $('#add-form').hidden = true;
  $('#add-open').hidden = false;
  $('#add-error').hidden = true;
};

$('#add-form').onsubmit = async (e) => {
  e.preventDefault();
  const f = new FormData(e.target);
  const err = $('#add-error');
  err.hidden = true;
  try {
    await api('POST', '/api/destinations', {
      name: f.get('name'),
      url: f.get('url'),
      stream_key: f.get('stream_key'),
      audio_label: f.get('audio_label') || '',
      delay_s: Math.round(Number(f.get('delay_min') || 3) * 60),
    });
    e.target.reset();
    $('#add-cancel').click();
    refresh();
  } catch (ex) {
    err.textContent = ex.message;
    err.hidden = false;
  }
};

// ── settings ────────────────────────────────────────────────────────────────
async function loadConfig() {
  const c = await api('GET', '/api/config');
  const form = $('#config-form');
  ['room_id', 'r2_account_id', 'endpoint_host', 'bucket', 'access_key_id',
   'region'].forEach((k) => { if (form[k]) form[k].value = c[k] || ''; });
  form.use_https.checked = c.use_https !== false;
  if (c.has_secret) form.secret_access_key.placeholder = 'unchanged';
}

$('#config-form').onsubmit = async (e) => {
  e.preventDefault();
  const f = new FormData(e.target);
  const err = $('#config-error'), good = $('#config-ok');
  err.hidden = true; good.hidden = true;
  try {
    await api('PUT', '/api/config', {
      room_id: f.get('room_id'),
      r2_account_id: f.get('r2_account_id'),
      endpoint_host: f.get('endpoint_host'),
      bucket: f.get('bucket'),
      access_key_id: f.get('access_key_id'),
      secret_access_key: f.get('secret_access_key'),
      region: f.get('region'),
      use_https: f.get('use_https') === 'on',
    });
    good.textContent = 'Saved.';
    good.hidden = false;
    e.target.secret_access_key.value = '';
    refresh();
  } catch (ex) {
    err.textContent = ex.message;
    err.hidden = false;
  }
};

$('#test-storage').onclick = async () => {
  const err = $('#config-error'), good = $('#config-ok');
  err.hidden = true; good.hidden = true;
  const btn = $('#test-storage');
  btn.disabled = true;
  btn.textContent = 'Testing…';
  try {
    const r = await api('POST', '/api/storage/test');
    if (r.ok) { good.textContent = 'Storage works.'; good.hidden = false; }
    else { err.textContent = r.error; err.hidden = false; }
  } catch (ex) {
    err.textContent = ex.message;
    err.hidden = false;
  }
  btn.disabled = false;
  btn.textContent = 'Test storage';
};

// ── log ─────────────────────────────────────────────────────────────────────
async function refreshLog() {
  try {
    const lines = await api('GET', '/api/log?lines=300');
    $('#log').textContent = lines.map((l) => {
      const t = new Date(l.at_ms).toLocaleTimeString();
      return `${t}  ${l.text}`;
    }).join('\n');
    $('#log').scrollTop = $('#log').scrollHeight;
  } catch (e) { /* the page will try again */ }
}

loadConfig().catch(() => {});
refresh();
setInterval(refresh, 1000);
setInterval(() => {
  if ($('#tab-log').classList.contains('is-on')) refreshLog();
}, 3000);
