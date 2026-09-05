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
- **Satellites can be appliances.** A campus that only needs to *receive* runs a
  headless Linux decoder box driving SDI/HDMI out, controlled from a phone or
  tablet over the local network. No OBS to learn, nothing to misconfigure, and
  it starts on power-up. Campuses that also mix local cameras or graphics run
  the OBS source plugin instead; both share the same core.

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

- **Two kinds of satellite, one core.** The receive logic (discovery, cache,
  timeslipping, decode) is a library with no dependency on OBS or Qt. It is
  driven either by the **OBS source plugin** (for campuses that mix locally) or
  by a **headless appliance** (for campuses that just play the feed out). The
  appliance is the expected deployment for most sites.
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

### 4.3 Production audio: packed multi-channel (primary mode)

The primary delivery mode for production audio is a **single multi-channel
audio track** — one 8-channel AAC stream carrying main mix, mic ISOs and click
in fixed channel positions.

- **Sample-accurate by construction.** One stream, one clock, start to finish.
  Nothing can drift between the click and the programme because they are
  channels of the same track, not separate tracks with independent buffering.
- **Channel order is the interface.** Channel 4 must be the click at both ends.
  The mapping is therefore *published* in `event.json` and `manifest.json` as
  `channel_labels`, so a satellite routes by name rather than guessing.
  Positional meanings from the speaker layout (FL/FR/LFE/…) are deliberately
  ignored — the layout is only a channel-count carrier.
- **Prerequisite:** both encoder and satellite OBS must be set to **7.1** in
  Settings → Audio → Channels. OBS resamples every source to its global layout,
  so at a narrower setting the extra channels are downmixed and destroyed. Both
  plugins detect this and log an explicit error rather than failing silently.
- **Capacity:** 8 channels total, e.g. stereo main mix + 6 mono ISOs.
- **Audio interfaces:** ASIO or Blackmagic DeckLink devices, which is where
  multi-channel capture and playout actually come from in production. A
  companion de-interleaver output plugin maps packed channels onto device output
  channels at the satellite (later phase).

The multi-track mode below remains supported for sites without a multi-channel
interface, at the cost of per-track buffering (tens of milliseconds of possible
drift between tracks) rather than sample-lock.

### 4.3.1 Multi-track production audio (alternative mode)

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

- Retention is handled by a **bucket lifecycle rule keyed on prefix and age**:
  delete objects under `events/` older than 7 days. Set-and-forget, configured
  once in the storage provider's console.
- Object *tagging* is deliberately not used. S3 supports it, but Cloudflare R2
  rejects requests carrying `x-amz-tagging`, so tag-driven expiry is not
  portable. Tagging remains available as an option for stores that support it,
  off by default.
- Every `PUT` carries `Cache-Control: max-age=604800` for any CDN in front of
  the bucket.
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

## 7.5 Finished events: video-on-demand

An event that has ended is not a failure state — it is a complete recording,
and a satellite must be able to load and play it exactly like a live feed that
happens not to be advancing.

- **Loading a finished event starts at the beginning**, not at the live edge.
  Treating a completed service as "live" meant loading it and landing seconds
  from the close.
- **Ending a broadcast mid-playback changes nothing for the satellite**: it
  keeps playing through the remaining segments to the end. Nothing is cut off.
- **The reported time never runs past the end of the recording.** Once playback
  passes the last segment the playhead points at a position that does not
  exist; the displayed clock is clamped to the true end.
- **The UI switches vocabulary.** For a live event it reports how far behind
  live the campus is. For a finished one it reports **position out of total
  length** the way a media player does — "24:15 / 1:24:30" — because that is
  what an operator needs when deciding whether a recording fits the slot.
  "Behind live" means nothing once there is no live edge.
- **A finished recording's timeline spans its whole length**, from the moment it
  started to its true end, and does not move during playback. While live the
  right edge is the live edge and necessarily grows; once ended it must not,
  or positions on the bar mean nothing.
- The manifest lists only a rolling window of segments, so neither the length
  nor the timeline bounds may be derived from it. The event's start time and
  last sequence give the true extent.
- Distinguishing a *clean end* from a *lost connection* matters: a clean end is
  reported as a finished recording, while a manifest that simply stops
  advancing is reported as offline after the stale threshold.
- **"Ended" covers two situations that must not read the same.** A broadcast
  that finished *while the satellite was watching* is reported as
  "BROADCAST ENDED" — the service has just closed and the recording is playing
  out. An event that was *already finished when loaded* is reported as
  "RECORDING (not live)" — this is a past service, and nothing has just
  happened. The satellite remembers whether it ever saw the event live, and
  the memory resets when the event changes.

### 7.5.1 Event browsing (planned)

When the event list arrives (it needs `ListObjectsV2`, section 4.x), each entry
must carry **its own current state**, not just a date:

- **LIVE** — this event is the one `live.json` points at and its manifest is
  still advancing. At most one event is live at a time.
- **RECORDING** — a finished event, playable as video-on-demand.
- **INTERRUPTED** — the manifest stopped advancing without a clean end, i.e.
  the encoder died. Still playable up to wherever it got to, but the operator
  should know it is incomplete.

A campus will usually see one live event among many recordings, so the state is
what makes the list scannable — the date alone does not say which one is
happening now. Entries should be labelled by start date and time, newest
first, with the live one pinned to the top.

## 8. User interface

Two Qt docks, plus hotkeys. The core reliability and media path work with no UI
at all, which is what lets the same engine drive the planned appliance.

**Encoder dock (main site)**

- Storage settings, saved as they are edited so credentials are never retyped.
- Video encoder chosen from what the machine actually has (x264, NVENC,
  QuickSync, AMF), hardware first.
- **Go live / End broadcast**, with failures shown in the dock rather than left
  in the log.
- Marker buttons, named by the operator.
- The reliability readout that matters mid-service: how much of the service has
  been sent, how much is waiting, retries, and link health.

**Decoder dock (satellite)**

- **Load** then **Play**: loading fills the buffer, Play puts it to air.
- A timeline in clock time showing what is in storage, what is downloaded here,
  the playhead and markers. Hovering reports the recorded time under the
  cursor; clicking goes there.
- Hold picture / Continue / Catch up to now, jog in ±1 s to ±1 min steps, and
  "stay behind live by N minutes".
- **Lock**, to stop anything being changed by accident during a service.
- Position and state in plain language, switching vocabulary between a live
  event and a finished recording.

**Language.** The interface never mentions segments, buffers in the abstract,
or live edges. It reports times ("Showing 10:41:03"), durations ("Could
broadcast for 12 min") and plain states. The audience is a volunteer, not the
person who wrote it.

**Hotkeys** cover play, stop, hold, resume, catch-up, jog and marker drops, and
work without Qt — useful for an operator running the service from the keyboard,
and the fallback when a build has no docks.

---

## 8.1 Satellite appliance (headless decoder)

The primary satellite deployment: a small Linux box at the campus that receives,
decodes and plays out, with no operator-facing desktop software.

**Why an appliance rather than a workstation.** A receive-only campus gains
nothing from a full OBS install and loses a great deal: scene collections to
corrupt, updates that change the UI, a desktop that can be left in the wrong
state, and a volunteer expected to understand a production tool. An appliance
boots into its job, restarts itself on failure, and presents one simple screen.

**Shape**

Two tiers, sharing one build:

- **Low cost (ARM64 / Raspberry Pi):** HDMI out, for a site that needs the feed
  on a screen and into a small console. This is the expected volume case.
- **Production (x86 mini-PC + DeckLink):** SDI with embedded audio, genlock,
  and a professional signal path for larger campuses.

**ARM64 / Raspberry Pi notes**

- The portable core cross-compiles for ARM64 today, and CI builds and tests it
  on an ARM64 runner so a regression is caught before it reaches hardware.
- **Video decode.** The Pi 5 (BCM2712) has **no H.264 hardware decoder** — only
  HEVC 4K60. Its NEON software H.264 decoder is reportedly faster than the old
  hardware block and handles 1080p comfortably, so a 1080p30 contribution feed
  is well within it. The Pi 4 *does* have H.264 hardware decode, capped at
  1080p. The decoder should therefore prefer a hardware decoder when one
  exists (`h264_v4l2m2m` on Pi 4, HEVC on Pi 5) and fall back to software
  rather than assuming either.
- This also strengthens the HEVC step on the codec roadmap: HEVC is exactly
  what the Pi 5 accelerates.
- **Audio.** HDMI carries up to 8 channels of LPCM, which suits the packed
  multi-channel layout: an inexpensive HDMI audio de-embedder recovers the
  individual channels at the campus. That gives a third audio path alongside
  ASIO and DeckLink, and is what makes the low-cost tier viable for production
  audio rather than stereo only.
- **Storage.** The segment cache writes roughly 3 GB per hour at 6 Mbps. That
  will wear out an SD card, so a USB SSD is required rather than recommended,
  and the cache location must be configurable.
- **Thermals.** Sustained decode needs active cooling; a passively cooled case
  will throttle during a long service.
- **Output:** DeckLink (video plus embedded multichannel audio, which suits the
  packed channel layout), or DRM/KMS for direct display. Audio to ALSA/JACK or
  embedded in SDI, with the channel de-interleaver applied on the way out.
- **Control:** a small built-in web server. Operators use a phone, tablet or any
  browser on the church network — hold, resume, catch up to now, jump to a
  moment, and see what is playing and how far behind. No app to install.
- **Operation:** starts on power-up (systemd), restarts on failure, keeps its
  local cache across reboots so a restart mid-service resumes rather than
  restarting.

**What it reuses.** Everything in the receive path: room and event discovery,
the durable segment cache, checksum verification, timeslipping (the playback
head, pause/resume, catch-up, scrub), markers, and the CMAF decoder. These are
already free of OBS and Qt and are covered by the existing tests, so the
appliance is a new *output and control* layer rather than a second
implementation.

**Web UI.** The decoder dock is already a thin view over a status snapshot; the
same snapshot serialises to JSON, so the browser UI is that view rendered in
HTML with a WebSocket for live updates. It should carry the same plain language
— clock times, "hold picture", "catch up to now" — and never mention segments.

**Open questions to settle before building**

- Which output path first: DeckLink SDI, or HDMI/DRM for the simplest sites?
- Does the appliance need a local slate or holding image when the feed is
  offline, and should it fall back to one automatically?
- Distribution: a disk image for supported hardware, or a package plus an
  install script on a stock distro?

## 9. Capability overview

| Capability | Commercial multisite platforms | This project |
|---|---|---|
| Resilient store-and-forward | yes | yes |
| Multisite to many campuses | yes | yes (unlimited; storage-cost only) |
| Markers / service cues | yes | yes |
| Pause & hold at a campus | yes | yes (timeslipping) |
| Per-campus independent DVR position | partial | yes |
| Multi-track production audio (main/ISOs/click) | partial | yes, up to 6 tracks |
| Dedicated receive appliance | yes (hardware decoder) | planned (Phase 6) |
| Self-hosted / own your storage | no (SaaS) | yes |
| Open protocol, no vendor lock-in | no | yes |
| Web/mobile simulcast from same files | yes | planned (CMAF makes it feasible) |
| Scheduling / auto-go-live | yes | planned |

Further directions to explore: web/mobile simulcast served directly from the
bucket, multi-bucket mirroring for redundancy, and local insertion windows for
campus announcements.

---

## 10. Delivery phases

Each phase leaves the project in a testable, usable state. Phases 1–5 are
built; 6 and 7 are not started.

- **Phase 1 — Reliability core.** ✅ Durable upload queue, retry/backoff, checksums,
  resume-after-crash, decoder cache with verification, and stale detection. This
  is format-agnostic and lands before the media format work.
- **Phase 2 — Format, namespace & audio.** ✅ FFmpeg CMAF muxing (`init.mp4` +
  `.m4s`), codec-agnostic wrapper (H.264 and HEVC both tested end to end; AV1
  carried but less exercised), packed multi-channel production audio, the
  `rooms/live.json` + `events/{ulid}` model, keyframe-aligned segments,
  prefix/age lifecycle, and generalized S3 endpoint configuration.
- **Phase 3 — Timeslipping.** ✅ Decoder DVR: playback head vs live edge, deep local
  cache, pause/resume/jump-to-live/scrub, behind-live indicator, restart
  recovery.
- **Phase 4 — Markers & cues.** ✅ Authoring from the encoder, consumption and
  jump-to-marker at the satellite. Decoder-side cue authoring is not built.
- **Phase 5 — User interface.** ✅ Encoder and decoder Qt docks, hotkeys, and
  plain-language status. Event browsing (section 7.5.1) is the outstanding
  piece and needs bucket listing.
- **Phase 6 — Satellite appliance.** ⬜ Headless Linux decoder with SDI/HDMI
  output and a browser-based operator UI (section 8.1). Reuses the existing
  receive core; adds the output layer, the channel de-interleaver, the web
  control surface, and the boot/restart behaviour that makes it an appliance.
- **Phase 7 — Extensions.** ⬜ Web/mobile simulcast, scheduling, redundancy, and
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
