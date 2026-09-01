# Self-Hosted Multisite Streaming Platform — Project Scope

A free, open-source, self-hosted platform for distributing a live service from a
main campus to any number of satellite campuses **reliably**, over commodity
hardware and unreliable venue internet. It runs as a pair of OBS Studio plugins
and uses nothing but an S3-compatible bucket you control — no central server, no
database, no vendor. Intelligence lives entirely in the edge plugins; the bucket
is a dumb file store.

---

## 1. Design priorities (ranked)

1. **Reliability above all.** A live service must not drop frames at a campus
   because the main site's internet hiccupped. Every segment is durable,
   retried, and verifiable; nothing is silently lost.
2. **Feature completeness** for how services actually run — production audio
   distribution, markers/cues, pause-and-hold, resume-after-crash, multisite.
3. **Simplicity of operation.** Decentralized and file-based. An operator's whole
   mental model is "hit Go Live" at the main site and "add the source" at a
   campus.
4. **Latency is last.** Minutes of latency are acceptable. We buffer heavily and
   trade latency for resilience at every decision point.

This ranking is the tie-breaker for every design choice.

---

## 2. Headline capabilities

- **Store-and-forward delivery.** The encoder writes each segment to a durable
  local queue first, then uploads with retry. If the network drops, capture keeps
  queuing to disk; on reconnect the queue drains in order. Nothing is lost.
- **Timeslipping (per-campus live-DVR).** Each receiving site can **pause** the
  incoming feed — to hold for its own welcome or announcements — and later
  **resume from exactly where it paused**, while the plugin keeps downloading the
  live feed into a local cache the whole time. Sites can also **jump to live**,
  **scrub** within what's retained, and see **how far behind live** they are.
- **Multi-track production audio.** Delivers up to all 6 OBS audio tracks — main
  mix, ISOs of specific mics, click track — muxed together and sample-accurately
  synced, then exposed at each far site as separate audio buses for local mixing,
  monitoring, and in-ears.
- **Markers / cues.** The main site drops named markers into the stream ("Sermon
  Start", "Offering", "Go to local"), manually or on a schedule; satellites see
  them, jump to them, and can trigger local automation.
- **Crash & outage resilience.** OBS crash or power loss at either end is
  recoverable: the encoder resumes the same event and sequence; decoders hold the
  last frame and resume seamlessly when the feed returns.
- **Bring-your-own storage.** Works with any S3-compatible endpoint — Cloudflare
  R2, AWS S3, Backblaze B2, Wasabi, or self-hosted MinIO. Cost is just storage.

---

## 3. Architecture

```
 MAIN CAMPUS (encode)          S3-COMPATIBLE BUCKET (dumb store)        SATELLITE CAMPUSES (decode)
┌─────────────────────┐                                              ┌─────────────────────┐
│ OBS + output plugin │  PUT init.mp4 / .m4s / manifest / markers    │ OBS + source plugin │
│  capture → encode   │ ───────────────────────────────────────────▶│  poll → download →  │
│  → CMAF segment     │        rooms/{room}/live.json                │  verify → cache →   │
│  → checksum         │        events/{ulid}/…                       │  local DVR playout  │
│  → durable queue    │◀── polled + GET + verified by every decoder ─│  → Projector → HDMI │
│  → upload w/ retry  │                                              │  (pause/resume/live)│
└─────────────────────┘                                              └─────────────────────┘
```

- **Decentralized.** No control plane. The media path and the signaling path are
  the same path: objects in a bucket.
- **Read-only decoders.** Satellites need only `GetObject` + `ListBucket`.
- **Addressable by number.** Deterministic segment names (`{seq:08d}`) let any
  node fetch any segment without a directory service.

---

## 4. Storage protocol

### 4.1 Namespace (single bucket per organization)

```
rooms/{room_id}/
    live.json                     # pointer to the current live event_id (+ heartbeat)
events/{event_id}/                # event_id = ULID minted by the encoder at "Go Live"
    event.json                    # static: video + audio-track layout, codecs, first_seq
    init.mp4                      # ONE CMAF init: video + every enabled audio track
    segments/{seq:08d}.m4s        # ONE fragment per seq carrying video + all audio tracks
    manifest.json                 # rolling window of recent segments (live edge)
    markers.json                  # append-only cue/marker list
```

- `room_id` is the broadcast source (e.g. `main-auditorium`).
- `event_id` is a ULID minted locally by the encoder at "Go Live".
- Decoders need only read access; the encoder needs write access scoped to its
  own room/event paths.

### 4.2 Segment format & codecs

- **Container: CMAF / fragmented MP4** (`init.mp4` + independent `.m4s`
  fragments). It seeks cleanly (needed for timeslipping), carries video plus
  multiple audio tracks in one fragment, and is the native format for HLS/DASH —
  so the same objects can later feed a browser/mobile simulcast with no
  re-packaging.
- **Muxing uses FFmpeg's fMP4 muxer** for correctness; the decode path uses
  FFmpeg as well.
- **Duration: 6 s default (configurable 2–15 s).** The keyframe interval strictly
  equals the segment duration. Longer segments mean fewer requests, better
  compression, and fewer opportunities to drop a request — all reliability wins.
- **Video codec roadmap:** **H.264** first (universal decode), then **HEVC** for
  bandwidth, then **AV1**. Codec identity lives in `event.json`/`manifest.json`,
  and the muxer wrapper and decoder are codec-agnostic, so adding a codec is a
  capability change rather than a rewrite.

### 4.3 Multi-track production audio

The audio system exists to move a full **production audio bus** to the far sites
— **main mix, ISOs of specific mics, and a click track** — not to offer a
listener a choice of one feed.

- **All enabled OBS audio tracks (up to 6) are muxed into the segment together**
  and delivered as a unit, so nothing arrives partially.
- **Sample-accurate sync is a hard requirement.** Because every track shares one
  segment and one playback clock, all audio tracks stay locked to the video and
  to each other — a click that drifts from the program is worthless.
- **Per-track channel layout is preserved:** a click or ISO may be mono while the
  main mix is stereo or 5.1.
- **Codec:** AAC to start (handles multichannel); Opus is a later option.
- **Far-site output:** each delivered track is exposed as its **own audio bus in
  OBS** at the satellite. A main decoder source carries video + main mix;
  lightweight companion audio-only sources carry the ISOs and click. All of them
  **share one download cache**, so a segment is fetched once regardless of how
  many tracks a campus uses. The local engineer routes the buses to mixer tracks,
  monitor sends, or in-ears and builds a local mix.
- On the encode side the OBS output accepts one audio encoder per enabled track
  and the muxer maps each track to an audio stream inside the fMP4.

### 4.4 Manifest (live-edge discovery)

`manifest.json` carries a rolling window of recent segments plus the range
metadata a timeslipping decoder needs:

```json
{
  "event_id": "01J8XG7QK3ZC9F8P6M2R4T5V7W",
  "status": "live",
  "updated_at_ms": 1719484800000,
  "first_available_seq": 300,   // oldest segment still retained (for timeslip)
  "window_start_seq": 412,      // oldest listed in this manifest
  "latest_seq": 462,            // live edge
  "init": "init.mp4",
  "video": { "codec": "h264", "width": 1920, "height": 1080, "fps": 30 },
  "audio_tracks": [             // streams inside each segment
    { "idx": 0, "label": "Main Mix",   "codec": "aac", "channels": 2, "sample_rate": 48000 },
    { "idx": 1, "label": "Sermon ISO", "codec": "aac", "channels": 1, "sample_rate": 48000 },
    { "idx": 2, "label": "Click",      "codec": "aac", "channels": 1, "sample_rate": 48000 }
    // … up to 6, one per enabled OBS track
  ],
  "segments": [ /* last ~50: seq, duration, checksum (one file carries all tracks) */ ]
}
```

Decoders discover the live edge from the window but can address any segment from
`first_available_seq` to `latest_seq` by deterministic key, so playback is not
limited to the manifest window — essential for timeslipping.

### 4.5 Markers

`markers.json` is append-only:

```json
{ "markers": [
  { "seq": 420, "at_ms": 1719484860000, "type": "cue", "label": "Sermon Start", "id": "01J8…" }
] }
```

### 4.6 Lifecycle / retention

- Every `PUT` carries an object tag (`MultisiteExpiry=7d`) and a
  `Cache-Control: max-age=604800` header.
- The organization sets **one bucket lifecycle rule**: delete objects tagged
  `MultisiteExpiry=7d` after 7 days. Set-and-forget.
- Old segments are removed by lifecycle expiry, not active deletion, so a
  paused or behind-live decoder can still fetch older segments for the full
  retention window — enabling deep DVR rather than a short buffer.

### 4.7 Write-ordering invariant

A segment is never listed in the manifest until it is durably in storage:
`write local → checksum → PUT segment (+tag) → await 200 OK → update manifest →
PUT manifest`. If a decoder can see a manifest entry, the segment is guaranteed
to exist.

---

## 5. Reliability

- **Durable encoder queue.** Segments are written to a local store (SQLite or an
  on-disk WAL) before upload, bounded only by disk. Survives OBS crash and power
  loss.
- **Retry with backoff.** Failed uploads retry with exponential backoff and
  jitter, in strict sequence order, for as long as the event is live. No segment
  is abandoned.
- **Checksums.** Each segment's hash is recorded in the manifest; decoders verify
  after download and re-fetch on mismatch.
- **Resume-after-crash.** Event state (event_id, last sequence, queue) is
  persisted. On restart the operator is prompted: *Resume previous event, or
  start new?*
- **Decoder-side durability.** Downloads are cached locally and verified;
  missing or corrupt segments are re-requested. A gap causes a wait-and-retry,
  never a crash.
- **Stale detection.** If a room's `live.json`/manifest has not updated within a
  threshold (e.g. 10 minutes), decoders treat the room as **Offline** instead of
  polling a dead event forever.
- **Sequence-driven sync.** All ordering and synchronization is by integer `seq`;
  campus wall clocks are never assumed to agree.

---

## 6. Timeslipping (per-campus live-DVR)

Each decoder maintains a **playback head** independent of the **live edge**:

- **Local cache & download-ahead.** The decoder continuously downloads new
  segments into a local cache regardless of where playback currently sits.
  Paused or behind live, it keeps filling.
- **Pause / Resume.** Pause freezes the playback head (holds the last frame); the
  cache keeps filling. Resume continues from the exact paused position.
- **Jump to Live.** Snaps the head to the live edge, with a configurable catch-up
  (hard cut by default, or a gentle speed-up).
- **Scrub / seek.** Move the head anywhere between `first_available_seq` and
  `latest_seq` — i.e. anywhere still retained, up to the full retention window.
- **Behind-live indicator.** Always shows how far behind live the campus is.
- **Restart recovery.** The playback position is persisted; after an OBS restart
  a campus resumes where it was or jumps to live (configurable).

Backed by durable object storage, this is "pause live TV," per campus.

---

## 7. Markers & cues

- **Authoring (main site).** The operator drops markers live (button/hotkey) or
  from a pre-loaded schedule; each is appended to `markers.json` keyed by `seq`.
- **Consumption (satellites).** Decoders display upcoming and passed markers on a
  timeline, can jump to a marker, and can fire local automation from one (e.g. a
  "Go to local" marker triggering a campus scene switch). Markers ride the same
  durable object path as the media.

---

## 8. User interface

- **Encoder (main site):** a Tools-menu panel for global storage config
  (endpoint, keys, bucket, room) and audio-track labelling; **Go Live / End**; a
  live status readout (queue depth, upload rate, retries); marker controls; and
  the resume-or-new prompt.
- **Decoder (satellite):** the source appears in OBS as usual, with a dedicated
  control dock for the DVR — Pause / Resume / Jump-to-Live, a scrub bar with
  markers, the behind-live indicator, and an Offline state.
- Tools-menu items and the dock use OBS's frontend API and Qt. The core
  reliability and media path work without any UI, so they land first; the UI is
  layered on afterward.

---

## 9. Capability overview

| Capability | Commercial multisite platforms | This project |
|---|---|---|
| Resilient store-and-forward | yes | yes |
| Multisite to many campuses | yes | yes (unlimited; storage-cost only) |
| Markers / service cues | yes | yes |
| Pause & hold at a campus | yes | yes (timeslipping) |
| Per-campus independent DVR position | partial | yes |
| Multi-track production audio (main/ISOs/click) | partial | yes, up to 6 tracks |
| Self-hosted / own your storage | no (SaaS) | yes |
| Open protocol, no vendor lock-in | no | yes |
| Web/mobile simulcast from same files | yes | planned (CMAF makes it feasible) |
| Scheduling / auto-go-live | yes | planned |

Further directions to explore: web/mobile simulcast served directly from the
bucket, multi-bucket mirroring for redundancy, and local insertion windows for
campus announcements.

---

## 10. Delivery phases

Each phase leaves the project in a testable, usable state.

- **Phase 1 — Reliability core.** Durable upload queue, retry/backoff, checksums,
  resume-after-crash, decoder cache with verification, and stale detection. This
  is format-agnostic and lands before the media format work.
- **Phase 2 — Format, namespace & audio.** FFmpeg CMAF muxing (`init.mp4` +
  `.m4s`), codec-agnostic wrapper (H.264 now; HEVC/AV1 later), multi-track
  production audio (up to 6 OBS tracks, muxed and sample-accurate), the
  `rooms/live.json` + `events/{ulid}` model, 6 s keyframe-aligned segments,
  lifecycle tags, and generalized S3 endpoint configuration.
- **Phase 3 — Timeslipping.** Decoder DVR: playback head vs live edge, deep local
  cache, pause/resume/jump-to-live/scrub, behind-live indicator, restart
  recovery.
- **Phase 4 — Markers & cues.** Authoring, consumption, jump-to-marker, and local
  automation hooks.
- **Phase 5 — User interface.** Encoder Tools-menu and status panel; decoder Qt
  control dock.
- **Phase 6 — Extensions.** Web/mobile simulcast, scheduling, redundancy, and
  local insertion.

---

## 11. Baseline configuration

- **Retention window:** 7 days (sets how far back campuses can timeslip).
- **Segment length:** 6 seconds default, configurable.
- **Video codecs:** H.264 first; HEVC then AV1 on the roadmap; codec-agnostic
  pipeline.
- **Audio:** every enabled OBS output track is delivered (up to 6), each labelled
  once by the operator; multichannel/surround allowed per track; AAC to start.
- **Pause behaviour:** hold the last frame.
- **First storage target:** Cloudflare R2 (any S3-compatible store supported).
- **Platform order:** Windows first, macOS second.
- **Security:** encoders use write access scoped to their room/event paths;
  decoders use read-only access to the bucket.
