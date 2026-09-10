# Self-Hosted Multisite Streaming Platform — Project Scope

A free, open-source, self-hosted platform for distributing a live event from a
main campus to any number of satellite campuses **reliably**, over commodity
hardware and unreliable venue internet. It runs as a pair of OBS Studio plugins
and uses nothing but an S3-compatible bucket you control — no central server, no
database, no vendor. Intelligence lives entirely in the edge plugins; the bucket
is a dumb file store.

> **⚠️ Alpha — development build.** This is pre-release software under active
> development. A six-hour continuous soak has been run end to end (see
> the README's Status section), but it has not yet carried a real congregation's event.
> Interfaces, settings and the storage protocol may still change without a
> migration path, and there is no support contract, warranty or uptime
> guarantee of any kind.
>
> Production use comes with caveats. Run it only with a tested fallback in
> place, a technical person on hand, and the assumption that any given event
> may have to go ahead without it. Treat a successful rehearsal as necessary
> rather than sufficient.

---

## 1. Design priorities (ranked)

1. **Reliability above all.** A live event must not drop frames at a campus
   because the main site's internet hiccupped. Every segment is durable,
   retried, and verifiable; nothing is silently lost.
2. **Feature completeness** for how events actually run — production audio
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
  mix, ISOs of specific mics, click track — muxed into the same fragment and so
  locked to the picture and to each other. At an OBS satellite each track is a
  separate source, all fed by one decoder, for local mixing, monitoring and
  in-ears. This is the primary mode (§4.3); packed multi-channel (§4.3.1)
  remains available for sites whose output is a single multi-channel device.
- **Markers / cues.** The main site drops named markers into the stream ("Sermon
  Start", "Offering", "Go to local"), manually or on a schedule; satellites see
  them, jump to them, and can trigger local automation.
- **Crash & outage resilience.** OBS crash or power loss at either end is
  recoverable: the encoder resumes the same event and sequence; decoders hold the
  last frame and resume seamlessly when the feed returns.
- **Out to the public, from the same upload.** A small self-hosted event
  reads the segments already in the bucket and pushes them to YouTube, Facebook
  or any RTMP destination, so the main site uploads once whether the event is
  going to two campuses or to two campuses and the internet. It runs a few
  minutes behind on purpose, so a wobble at the main site delays the public
  stream rather than breaking it (§8.2).
- **Bring-your-own storage.** Works with any S3-compatible endpoint — Cloudflare
  R2, AWS S3, Backblaze B2, Wasabi, or self-hosted MinIO. Cost is just storage.
- **Satellites can be appliances.** A campus that only needs to *receive* runs a
  headless Linux decoder box driving SDI/HDMI out, controlled from a phone or
  tablet over the local network. No OBS to learn, nothing to misconfigure, and
  it starts on power-up. Campuses that also mix local cameras or graphics run
  the OBS source plugin instead; both share the same core.
- **The origin is not tied to hardware.** Encoder and decoder ship in one module,
  so any machine running OBS can take either role, and what originates an event
  is a laptop with OBS on it. A broadcast can come from a guest speaker's laptop, a
  conference venue for one week, a campus hosting this week's combined event,
  or a site set up at short notice; adding an origin costs a room name and a key
  that can write to it. Nothing ships, clears customs, or is licensed per
  location. Store-and-forward matters *more* for an occasional origin than a
  permanent one: a speaker on hotel wifi or a phone hotspot has the worst
  connection in the chain and can least afford a dropout mid-sermon, and a
  broadcast written to disk and resent survives a link that would kill a direct
  stream.

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
  node fetch any segment without a directory event.

---

## 4. Storage protocol

### 4.1 Namespace (single bucket per organization)

```
rooms/{room_id}/
    live.json                     # pointer to the current live event_id (+ heartbeat)
    events/{event_id}.json        # per-room index entry, written once at "Go Live"
events/{event_id}/                # event_id = ULID minted by the encoder at "Go Live"
    event.json                    # static: video + audio-track layout, codecs, first_seq
    init.mp4                      # ONE CMAF init: video + every enabled audio track
    segments/{seq:08d}.m4s        # ONE fragment per seq carrying video + all audio tracks
    manifest.json                 # rolling window of recent segments (live edge)
    markers.json                  # append-only cue/marker list
```

- `room_id` is the broadcast source (e.g. `main-auditorium`).
- The per-room index exists because `events/` is a flat global namespace:
  nothing in an event's key says which room it belongs to, only `event.json`
  does. Without the index, listing one room's events means listing every event
  ever recorded and reading each descriptor to discard most of them. Writing it
  is deliberately non-fatal — an event must not be held off air because an
  index entry failed.
- **The index is a shortcut, not the register.** An event with no entry — one
  recorded before the index existed, or one whose entry failed to write — must
  still list, so discovery is the *union* of the index and a scan of `events/`
  rather than the index when it has anything and the scan when it does not.
  Treating a non-empty index as the whole truth hid every older event the
  moment one indexed event appeared. The scan costs a descriptor read only
  for the ids the index did not already name.
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

### 4.3 Production audio: multi-track (primary mode)

The primary delivery mode is **one audio stream per enabled OBS track** — main
mix, mic ISOs, click — all muxed into the same fragment and delivered as a unit.

- **Native to how OBS already works.** An operator assigns sources to tracks in
  Advanced Audio Properties, exactly as they would for a multi-track recording.
  Nothing new to learn, and the encoder side needs no special configuration.
- **No global layout requirement.** Each track is its own stream with its own
  channel count: a stereo main mix beside a mono click beside mono ISOs. OBS can
  stay in plain stereo at both ends.
- **Per-track channel layout is preserved** — a click or ISO may be mono while
  the main mix is stereo or wider.
- **Sync is structural, not incidental.** Every track shares one fragment, one
  timeline and one playback clock. They cannot drift from the video or from each
  other, because they are demuxed from the same object and played from one
  playout base.
- **Capacity:** up to 6 tracks (OBS's limit).
- **Codec:** AAC to start; Opus is a later option.
- **Far-site output:** the Multisite Source carries the video plus one chosen
  track — track 1 by default, so a campus that only wants the programme does
  nothing. Each further track is exposed by adding a **Multisite Audio Track**
  source, which attaches to the decoder already following that room rather than
  opening its own. The segment is therefore downloaded once and decoded once
  however many tracks a campus uses, and every track is emitted from the same
  playout base. The local engineer routes those sources to mixer tracks, monitor
  sends or in-ears.

Why this is the primary mode and packed is not: OBS resamples every source to
its **global** layout, so packed multi-channel requires both ends to be set to
7.1, and a satellite that is not silently downmixes — summing the ISOs and the
click into the programme. That failure destroys production audio without anyone
noticing until it is on air, and no amount of warning text makes it a good
default. Multi-track has no such mode.

### 4.3.1 Production audio: packed multi-channel (alternative mode)

A **single multi-channel track** — one 8-channel AAC stream carrying main mix,
ISOs and click in fixed channel positions. Supported for sites with a
multi-channel interface that would rather have one stream than several.

- **Sample-accurate by construction.** One stream, one clock. This is a real
  property, but it is not an advantage over multi-track *here*: our tracks
  already share a fragment and a clock.
- **Channel order is the interface.** Channel 4 must be the click at both ends,
  so the mapping is *published* in `event.json` and `manifest.json` as
  `channel_labels` and a satellite routes by name rather than guessing.
  Positional meanings from the speaker layout (FL/FR/LFE/…) are deliberately
  ignored — the layout is only a channel-count carrier.
- **Prerequisite, and the reason this is not the default:** both encoder and
  satellite OBS must be set to **7.1** in Settings → Audio → Channels. Both
  plugins detect a narrower layout and log an explicit error, because otherwise
  the extra channels are downmixed and destroyed silently.
- **Capacity:** 8 channels total, e.g. stereo main mix + 6 mono ISOs.
- **Audio interfaces:** ASIO or Blackmagic DeckLink devices. Mapping packed
  channels onto device output channels at the satellite is **out of scope**,
  not merely unbuilt: in OBS,
  [atkAudio's plugin suite](https://github.com/atkAudio/PluginForObsRelease)
  already routes audio to ASIO, CoreAudio and Windows Audio devices and hosts
  VST3/AU/LV2 plugins alongside, which is a superset of what a de-interleaver
  of ours would have done. It is a separate install under AGPL-3.0; nothing
  here links against it or requires it. Verified 2026-09-07: an 8-channel /
  7.1 feed carried on one track survives this pipeline with its channel order
  intact, which is the part that is ours to get right.

**Which mode suits which satellite.** The two modes are not competing for the
same sites. An **OBS satellite** wants multi-track: OBS routes sources
independently, so separate tracks land on separate destinations with no
routing plugin needed at all. An **appliance** driving HDMI or SDI wants packed: its output
is one multi-channel device, and eight channels in one stream map straight onto
HDMI's eight (§8.1). The encoder can send either; the choice belongs to the
receiving end, which is why both remain supported.

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
- **The rule must cover `rooms/` as well as `events/`.** The per-room index
  entry for an event is a few hundred bytes and outlives nothing on its own, so
  a rule that expires only the media leaves the event list advertising events
  whose segments have gone. The catalog handles it — such an event is counted
  as skipped rather than offered — but the list degrades over time for no
  reason. Same age on both prefixes.
- Retention is the set-and-forget default. The encoder UI can additionally
  delete specific events on demand — one at a time, or everything older than a
  chosen number of days — with a confirmation and a verification pass, but the
  codebase otherwise does not delete as it goes, so a paused or behind-live
  campus can still fetch older segments for the whole retention window. The
  event live.json currently names is never deletable.

### 4.7 Write-ordering invariant

A segment is never listed in the manifest until it is durably in storage:
`write local → checksum → PUT segment (+tag) → await 200 OK → update manifest →
PUT manifest`. If a decoder can see a manifest entry, the segment is guaranteed
to exist.

---

## 5. Reliability

- **Durable encoder queue.** Segments are written to disk before upload, bounded
  only by disk. Survives OBS crash and power loss. Plain files with atomic
  write-then-rename, deliberately not a database: that gives the durability
  guarantee needed here and is trivial to reason about and to test.
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
  Treating a completed event as "live" meant loading it and landing seconds
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
  "BROADCAST ENDED" — the event has just closed and the recording is playing
  out. An event that was *already finished when loaded* is reported as
  "RECORDING (not live)" — this is a past event, and nothing has just
  happened. The satellite remembers whether it ever saw the event live, and
  the memory resets when the event changes.

### 7.5.1 Event browsing

Built. The decoder lists a room's events and each entry carries **its own
current state**, not just a date:

- **LIVE** — this event is the one `live.json` points at and its manifest is
  still advancing. At most one event is live at a time.
- **RECORDING** — a finished event, playable as video-on-demand.
- **INTERRUPTED** — the manifest stopped advancing without a clean end, i.e.
  the encoder died. Still playable up to wherever it got to, but the operator
  should know it is incomplete.

A campus will usually see one live event among many recordings, so the state is
what makes the list scannable — the date alone does not say which one is
happening now. Entries are labelled by start date and time, newest first, with
the live one pinned to the top.

Choosing an event pins playback to it. An event starting mid-watch does **not**
steal the playback; the operator is told something is live and offered the
switch, because being pulled out of a recording part-way through is worse than
being told about it.

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
- The reliability readout that matters mid-event: how much of the event has
  been sent, how much is waiting, retries, and link health.

**Decoder dock (satellite)**

- **Load** then **Play**: loading fills the buffer, Play puts it to air.
- A timeline in clock time showing what is in storage, what is downloaded here,
  the playhead and markers. Hovering reports the recorded time under the
  cursor; clicking goes there.
- Hold picture / Continue / Catch up to now, jog in ±1 s to ±1 min steps, and
  "stay behind live by N minutes".
- **Lock**, to stop anything being changed by accident during an event.
- Position and state in plain language, switching vocabulary between a live
  event and a finished recording.

**Language.** The interface never mentions segments, buffers in the abstract,
or live edges. It reports times ("Showing 10:41:03"), durations ("Could
broadcast for 12 min") and plain states. The audience is a volunteer, not the
person who wrote it.

**Hotkeys** cover play, stop, hold, resume, catch-up, jog and marker drops, and
work without Qt — useful for an operator running the event from the keyboard,
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
  audio rather than stereo only. Packed is therefore the appliance's mode even
  though multi-track is the primary one overall (§4.3): an appliance has one
  output device, not a mixer. Fed a multi-track event it plays **one chosen
  track** — the first by default, selectable in Settings for a campus whose
  origin puts the house mix elsewhere. Distributing several tracks across
  output channels is not built, and playing all of them is not a fallback: six
  tracks into a device that accepts one is six times real time of audio, and
  the back-pressure starves the video sharing that thread.
- **Storage.** The segment cache writes roughly 3 GB per hour at 6 Mbps. That
  will wear out an SD card, so a USB SSD is required rather than recommended,
  and the cache location must be configurable.
- **Thermals.** Sustained decode needs active cooling; a passively cooled case
  will throttle during a long event.
- **Output:** DeckLink (video plus embedded multichannel audio, which suits the
  packed channel layout), or DRM/KMS for direct display. Audio to ALSA/JACK or
  embedded in SDI. Packed channels go out in the order they arrived, which is
  what an eight-channel HDMI de-embedder or an SDI de-embedder expects — there
  is nothing to de-interleave when the output device is itself
  multi-channel.
- **Control:** a small built-in web server. Operators use a phone, tablet or any
  browser on the church network — hold, resume, catch up to now, jump to a
  moment, and see what is playing and how far behind. No app to install.
- **Operation:** starts on power-up (systemd), restarts on failure, keeps its
  local cache across reboots so a restart mid-event resumes rather than
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

**Settled while building**

- **HDMI/DRM first.** The appliance claims the KMS connector itself and sets
  the output mode, with no desktop involved. That is what gives exact control
  of resolution and frame rate, removes a desktop that can be left in the
  wrong state, and suits Raspberry Pi OS Lite, which is the right image for a
  box that only ever does one thing. DeckLink SDI follows for the production
  tier.
- **Yes to a holding screen, and it is the default.** The first problem with
  an appliance is finding it: until somebody knows its address there is
  nothing to type into a phone. So the first thing it does with the display is
  put its own address, name, room and state on it. Idle behaviour is
  configurable — black, hold the last picture, the identity screen, or a
  campus-supplied slide — because a screen a congregation can see is not
  always best left showing the last frame of an event.
- **A script on a stock distribution, not an image.** One command on stock
  Raspberry Pi OS installs the dependencies, builds, installs the event and
  enables it. It works from day one with no release infrastructure, updates
  are the same command again, and it does not tie the project to particular
  hardware. A prebuilt package can follow once the player has settled.
- **The preview is a copy of the output, not a second head.** The web UI shows
  the picture going out, refreshed a few times a second, so an operator can
  watch from a phone without changing what is on the screen in the room. There
  is one playhead, so the preview mirrors it rather than looking ahead; a
  genuinely decoupled look-ahead preview would need a second decode path and is
  deliberately out of scope for the single-box appliance.

## 8.2 Public simulcast relay

The same segments that feed the campuses, pushed out to YouTube, Facebook or
any RTMP destination. A separate sub-project in `relay/`, deployed as one
Docker container on a small VPS. It is not part of the plugins and the core
knows nothing about it.

**Why relay from the bucket rather than add a second OBS output.** The main
site uploads once however many places the event goes, which is what makes
this possible at all on a venue connection that will not carry a second
upload. The public stream also inherits the buffering the campus feed already
has: the relay deliberately runs a configurable time behind the event —
three minutes by default — so a dropout at the main site is absorbed instead
of reaching air. It is the same trade as §1, applied to the public stream:
latency spent to buy resilience.

**Two protocols, told apart by the address alone.** RTMP is what every public
streaming site accepts, so one mechanism covers YouTube, Facebook and a
church's own server. SRT is what broadcast partners, hardware decoders and the
better contribution CDNs ask for, and it is what a lossy path between the VPS
and the destination wants: it retransmits lost packets instead of letting them
become a glitch. There is no protocol setting and no radio button — `rtmp://`
and `srt://` are unmistakable, and asking a volunteer to declare which one
they pasted is asking them to get it wrong.

The two differ in exactly one way that matters upward: RTMP means FLV, and FLV
means H.264. SRT means MPEG-TS, which carries HEVC properly — a standardised
stream type decoders have handled for years, not FLV's after-the-fact extension
that half the receiving end has never heard of. So **the codec rule below is
written per protocol**, and an HEVC feed that cannot go to YouTube can go to an
SRT destination unchanged.

That matters more than it sounds. Until SRT existed here, choosing HEVC for the
campuses cost a church its public stream outright, which made a real bandwidth
saving unusable for anyone who also streams. It now costs them the *RTMP*
destinations only.

**SRT settles for a longer latency than ffmpeg's own.** Its `latency` is the
window in which a lost packet can be asked for again; ffmpeg's default of 120ms
is enough only on a path short enough that the answer comes back almost
immediately. The relay sends 2000ms unless told otherwise. That is §1 applied
where it is cheapest — the relay is already sitting three minutes behind the
event, so two seconds is invisible, and it buys recovery across a path many
times longer than the default can manage. It is on the form, under Advanced,
for the case where it is not enough.

**SRT can also be listened for rather than called out to,** for a broadcast
partner or a hardware decoder that pulls from us. It is never the default and
is never inferred from a setting: an address with nothing before the port —
`srt://:9000` — is how one is written down, and writing it that way is how one
is asked for. It does mean opening an inbound port on a machine we have
otherwise been careful to keep closed, which is why it takes a deliberately
odd-looking address to get one.

A listener nobody has attached to yet is the reason §8.2's supervision grew a
second half. The finding it was built on is that ffmpeg says nothing when it
is *starved*; the same is true when the far end stops *reading*. Both have to
be noticed by watching, and they mean opposite things depending on which end
opened the connection. A destination we called that stops taking content has
gone wrong and is dropped and rebuilt like any other lost connection. A
listener that has never carried anything is simply waiting, possibly for the
whole first half of an event, and is neither reported nor acted on as a
failure — it keeps taking up position behind the live edge while it waits, so
whoever finally attaches gets the event as it is now rather than the forty
minutes they missed. Once a listener has carried content, losing it is a fault
like any other: the distinction is whether anything ever went out, not the
mode.

Watching the outbound side at all is new with SRT and fixes a latent hole in
the RTMP path too — before it, a destination that quietly stopped reading was
fed for ever into a pipe nobody was emptying.

**Copy remux, never a silent transcode.** Segments are pushed on unchanged: no
decode, no encode, no quality loss, and little enough CPU that the cheapest VPS
tier is the target rather than a stretch. What cannot be sent that way is
refused rather than adapted, in the two cases where adapting it silently would
put the wrong thing on air:

- **HEVC over RTMP, and AV1 over either.** RTMP wants H.264. ffmpeg will mux
  either of the others into FLV and report success, producing a well-formed
  stream the destination then rejects — measured, not assumed — so nothing
  downstream can be relied on to notice. The relay refuses and says which
  encoder setting to change, and now also points at the way there is to send
  it on unchanged: an SRT destination, where MPEG-TS carries HEVC properly.

  AV1 is refused on both. MPEG-TS has a mapping for it, but ffmpeg's support
  and the receiving end's support are each patchy enough that the likely
  outcome is the same well-formed-but-rejected stream this rule exists to
  prevent — so it is refused until that stops being true, rather than allowed
  on the strength of the specification.

  H.264 remains the default and the roadmap's first codec precisely because it
  decodes everywhere, a Pi 5 included (§8.1: software decode handles 1080p
  comfortably), so a site that has not gone out of its way to change codec can
  stream publicly with nothing to reconsider. Re-encoding on the way out is
  still the eventual answer for the RTMP case, is not built, and would end the
  $5-a-month claim when it is.
- **Packed multi-channel audio (§4.3.1),** where the mix, the ISOs and the
  click share one track. Selecting a pair out of it is not built, and sending
  it unchanged would put a mic ISO or the click out to the public. Multi-track
  events (§4.3, the primary mode) are handled: each destination carries one
  track, chosen by the label the main site published.

**Supervision is the point, not a refinement.** Most destinations end a
broadcast after roughly a minute without data, so an unattended relay that
cannot recover is worse than none. Each destination has one ffmpeg child and
one thread that owns it; a child that dies is restarted and resumes from the
segment it was on, so nothing is skipped. A silence shorter than 45 seconds is
ridden out without dropping the connection at all — fragment timestamps are
absolute, so content resumes exactly where it stopped and a destination that
tolerates the pause never knows. Beyond that the connection is dropped
deliberately and rebuilt, which splits the recording at the far end and is
reported as such.

Detecting that silence is the relay's own job: ffmpeg given a pipe that stops
producing blocks quietly and holds the socket open indefinitely without
reporting anything, so waiting for the child to complain is waiting for ever.

**What it reuses.** The receive path, unchanged: event discovery, the durable
cache, checksum verification, and the live/ended/interrupted classification of
§7.5. It is the same code a campus runs, so the relay and a campus can never
disagree about whether an event is still running — and an event that ends
cleanly is played out to its last segment and then closed deliberately, rather
than being cut off or left to time out.

**Finished events.** The relay also does two things with events that have
already ended, both gated on the event actually being finished (§7.5.1's
classification, so it and a campus agree on what "finished" means):

- **Download as one MP4**, streamed from storage as it is requested rather than
  assembled on the server, so a two-hour event costs no disk and several
  people can download at once. It carries every audio track, not just the
  streamed one — the ISOs and the click are what a post-production edit needs.
- **Replay to a destination**, playing a finished event out at normal speed
  as though it were live, for a second congregation or an evening repeat. This
  falls out of §7.5 rather than being new machinery: a finished event already
  plays and then ends, which is what a replay is. Proof of concept — one at a
  time, started by hand.

**Access.** The relay can change where a church broadcasts, so unlike the
campus appliance it cannot rely on being on a trusted network. It requires a
login on every endpoint but the sign-in itself, stores the password as
PBKDF2-HMAC-SHA256 over a random salt, and binds to localhost so that exposing
it is a decision. It does not terminate TLS: a proxy in front of it does, and
one is shipped as a working example.

**Not built.** Re-encoding; splitting packed audio; SRT in listener mode being
reachable through anything (the port has to be published, and nothing is
shipped to help); signing in to YouTube (a stream key is pasted, and the
broadcast is still created in YouTube's own page); and starting by itself,
either on a schedule or when the encoder goes live. Scheduling matters most,
because events start late — the intended trigger is `live.json` actually
going live, optionally bounded by a time window, and `markers.json` makes
"start the public stream at Sermon Start" possible.

## 8.2.1 A hosted streaming provider (planned)

A church that wants a player on its own website currently has to put the
event on YouTube or Facebook and embed theirs. The alternative is a hosted
video API — **Mux** and **Cloudflare Stream** are the two worth supporting,
and the point of naming both is that neither becomes the answer: the relay
would carry a provider interface, and a church would choose.

What it would buy, in the order it is worth having:

- **A persistent player embed.** A hosted provider issues one playback
  identifier that outlives every broadcast. A church embeds it once, and the
  relay can change what is behind it — this week's event, a replay, a
  holding card — without anybody editing the website again. That is the whole
  feature; everything else is machinery for it. The page itself would live in
  the bucket the church already has rather than on the relay, so the public
  page does not depend on the $5 VPS being up and the relay gains no public,
  unauthenticated surface.
- **A control panel** in the relay's own page: stream state as the provider
  reports it, the playback identifier, past recordings, and a way to reset the
  key.
- **Provider-side simulcast.** Both providers will push onward to YouTube,
  Facebook and the rest on the church's behalf. The VPS then sends *one*
  stream out however many places the event goes, instead of one ffmpeg child
  and one full upload per destination — which on a small VPS is the difference
  between two destinations and six.

What it would cost, stated here because it is the part that would otherwise be
discovered late:

- **Per-destination audio selection and per-destination delay cannot survive
  provider-side simulcast.** The provider resends what it received, so every
  onward destination carries the same track and the same delay. Today a church
  can send the main mix to one place and a different feed to another. So this
  would be a per-destination choice — "sent by this relay" or "sent by the
  provider" — with our own remaining the default, rather than a switch that
  quietly flattens the two.
- **Failure reporting moves off the ffmpeg child** and onto polling the
  provider's API, so §8.2's supervision story does not cover it and would need
  its own equivalent before it could be trusted unattended.
- **It is not free, and the rest of this project is.** Both providers bill per
  minute ingested and per minute delivered. That has to be said on the page
  where the credentials are typed, not in a document.
- **The provider's access token is a different class of secret** from a stream
  key: it outlives the session and grants far more than one broadcast. It
  belongs with §8.2's OAuth note — encrypted at rest — rather than with the
  stream keys.

## 8.3 External control API (planned)

Operators reach for a physical button, not a dock. A volunteer running a
event on a Stream Deck should be able to hold, resume and catch up without
finding a window, and the main site should be able to go live from a button.
The target is **Bitfocus Companion**, which is what churches in this bracket
actually use.

**One surface, two transports.** The appliance already exposes its controls as
HTTP routes (§8.1): `/api/play`, `/api/hold`, `/api/seek`, `/api/status` and the
rest. The OBS plugins will expose *the same command names with the same
payloads* over **obs-websocket vendor requests**. One API to learn and document,
two ways in, and a control surface written against either works against both.

**Why vendor requests rather than a server inside the plugin.** obs-websocket
ships with OBS 28 and later, so there is nothing for a church to install. Its
`obs-websocket-api.h` is header-only and works through OBS's proc handler, so
the plugin gains no link dependency, and if obs-websocket is absent every call
returns false after one log line — control disappears, nothing breaks.
Authentication, the listening socket and TLS are already solved there. A
bespoke HTTP server inside the plugin would duplicate all of it and open a
second port on a machine we have otherwise been careful to keep closed.

**Both ends, equal weight.** The encoder needs go-live, stop, drop-marker and
status; the decoder needs play, stop, hold, resume, catch-up, seek, jog, delay,
load-event, follow-live, audio-track selection, status and the event list. The
command set already exists as `EncoderControls` and `DecoderControls`, so the
API layer is an adapter over what the docks and hotkeys already call — no new
control logic, and no second path to keep in step.

**Two phases, because Companion needs more than actions.**

1. **The vendor API.** Every command registered as a vendor request, plus vendor
   events on state change and a `status` request for polling. Companion can
   drive all of it immediately through its OBS module's *Custom Vendor Request*
   action, and so can any obs-websocket client — scripts, Stream Deck plugins,
   another automation system.
2. **A Companion module.** *Custom Vendor Request* is an action only: it cannot
   light a button red while an event is live, or show "12 s behind" on a
   display. Feedbacks, variables and presets need a purpose-built Companion
   module (Node.js, submitted to Bitfocus) subscribing to the vendor events.
   That is a separate deliverable in a separate repository, and it depends on
   phase 1 existing first.

**Available before any of this:** the plugins already register ten named
hotkeys, and Companion's OBS module can trigger hotkeys by id. Play, stop, hold,
resume, catch-up, jog and drop-marker are therefore controllable from a Stream
Deck today, without parameters or feedback. Worth wiring up before building
anything, both because it is free and because it will show which commands
operators actually reach for.

---

## 9. Capability overview

What this project does, and where each piece stands. Status is against the
codebase, not against anything else on the market — where a commercial platform
is the better answer for a given church, section 12 says so plainly.

| Capability | Status |
|---|---|
| Resilient store-and-forward upload | built |
| Multisite to any number of campuses (storage cost only) | built |
| Markers / event cues | built |
| Pause & hold at a campus, resuming exactly where it stopped | built |
| Per-campus independent DVR position | built |
| Multi-track production audio (main / ISOs / click), up to 6 tracks | built — one source per track at the satellite (§4.3) |
| Event browsing with live / recording / interrupted state | built |
| Video-on-demand playback of past and interrupted events | built |
| Any OBS machine can originate a broadcast | built |
| Dedicated receive appliance (Raspberry Pi / mini-PC) | built, not yet run through an event |
| Self-hosted, on storage you own | built |
| Open protocol, no vendor lock-in | by design — the whole protocol is §4 |
| Public simulcast to YouTube / Facebook / RTMP or SRT | built and pushing live to YouTube; not yet through a full event. H.264 over either; HEVC over SRT only, and not yet from real encoder output (§8.2) |
| SRT output, caller or listener | built and receiving on a real client; not yet run through a full event (§8.2) |
| HEVC out over SRT | built, and the remux verified against ffmpeg — but not yet carried from a real HEVC encoder (§8.2) |
| Download a finished event as an MP4, all audio tracks | built (§8.2) |
| Replay a finished event to a destination | proof of concept — one at a time, by hand (§8.2) |
| Per-channel routing of packed audio at an OBS satellite | out of scope — use [atkAudio's OBS plugins](https://github.com/atkAudio/PluginForObsRelease) (§4.3.1) |
| Re-encoding an HEVC feed for a streaming site | not built; an SRT destination carries HEVC unchanged instead (§8.2) |
| Hosted streaming provider (Mux / Cloudflare Stream), persistent player embed, provider-side simulcast | planned (§8.2.1) |
| External control API (obs-websocket vendor requests, §8.3) | planned |
| Bitfocus Companion module (buttons, feedbacks, variables) | planned; needs the API first |
| Control from a Stream Deck via OBS hotkey triggers | available now, no parameters or feedback |
| Web / mobile simulcast from the same files | planned; CMAF makes it feasible |
| Scheduling / auto-go-live | planned — for the relay as well as the encoder |

Further directions to explore: web/mobile simulcast served directly from the
bucket (which needs no relay at all — the CMAF objects are already the right
shape for it), multi-bucket mirroring for redundancy, and local insertion
windows for campus announcements.

---

## 10. Delivery phases

Each phase leaves the project in a testable, usable state. Phases 1–5 are
built and have been run end to end. Phases 6 and 7 are built but have not yet
carried an event; phase 8 has not been started.

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
  plain-language status, plus event browsing (section 7.5.1), which needs
  bucket listing.
- **Phase 6 — Satellite appliance.** 🟨 Headless Linux decoder with HDMI output
  and a browser-based operator UI (section 8.1), built on the existing receive
  core. Built: the player engine, DRM/KMS display output with its own
  modesetting, ALSA multichannel audio, the splash and idle screens, the web
  control surface (decoder controls, event list, storage and system settings,
  decoupled preview), and the systemd/install path that makes it start on
  power-up, **proven on a Pi 5 on 2026-09-07** though not yet through a
  event. Outstanding: DeckLink SDI output for the production tier, and
  hardware-decoder selection on Pi 4. The channel de-interleaver has been
  dropped from scope rather than deferred (§4.3.1).
- **Phase 7 — Extensions.** 🟨 Built: the public simulcast relay (§8.2), as a
  separate container in `relay/` — copy remux to one or more RTMP **or SRT**
  destinations, per-destination audio selection, a delay buffer, and
  supervised reconnection, with its own browser UI. Like the appliance it has
  not yet carried an event. HEVC now has a route out, over SRT, where RTMP
  can carry only H.264; that remux is verified against ffmpeg but has not yet
  carried real encoder output. Not started: re-encoding, web/mobile simulcast
  served from the bucket, scheduling and auto-go-live, redundancy, and local
  insertion.

- **Phase 8 — External control API.** ⬜ The command surface of both plugins as
  obs-websocket vendor requests, mirroring the appliance's existing HTTP routes
  name for name (§8.3), then a Bitfocus Companion module for buttons with
  feedbacks and variables. Hotkey-based control from Companion already works
  and needs nothing.

---

## 11. Baseline configuration

- **Retention window:** 7 days (sets how far back campuses can timeslip).
- **Segment length:** 6 seconds default, configurable.
- **Video codecs:** H.264 first; HEVC then AV1 on the roadmap; codec-agnostic
  pipeline.
- **Audio:** every enabled OBS output track is delivered (up to 6), each labelled
  once by the operator; multichannel/surround allowed per track; AAC to start.
  A satellite plays track 1 unless told otherwise, so a stereo-only site needs
  no audio configuration at either end.
- **Pause behaviour:** hold the last frame.
- **First storage target:** Cloudflare R2 (any S3-compatible store supported).
- **Platform order:** Windows first, macOS second.
- **Security:** encoders use write access scoped to their room/event paths;
  decoders use read-only access to the bucket.

---

## 12. Why this exists

This project is developed by the projects team at **Stage Audio Works**, a
worship AVL integrator working across Africa, to support churches that are
growing into multiple locations. Multisite streaming is a solved problem for a
large church in a well-connected part of the world; the commercial platforms
that solve it are good, and largely unavailable or unaffordable where these
churches are.

The full argument — what this is and is not, why it is not a managed event,
why latency is traded for resilience, what it asks of a venue's network, and
the clean-room and licensing position — is in the README, under
[Why this exists](README.md#why-this-exists). It was duplicated here word for
word, which meant two places to keep in step and one of them silently going
stale. This document covers the design; the README covers the case for it.
