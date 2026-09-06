# Self-Hosted Multisite Streaming Platform — Project Scope

A free, open-source, self-hosted platform for distributing a live service from a
main campus to any number of satellite campuses **reliably**, over commodity
hardware and unreliable venue internet. It runs as a pair of OBS Studio plugins
and uses nothing but an S3-compatible bucket you control — no central server, no
database, no vendor. Intelligence lives entirely in the edge plugins; the bucket
is a dumb file store.

> **⚠️ Alpha — development build.** This is pre-release software under active
> development. It has been exercised end to end but has not yet carried a real
> service. Interfaces, settings and the storage protocol may still change
> without a migration path, and there is no support contract, warranty or
> uptime guarantee of any kind.
>
> Production use comes with caveats. Run it only with a tested fallback in
> place, a technical person on hand, and the assumption that any given service
> may have to go ahead without it. Treat a successful rehearsal as necessary
> rather than sufficient.

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
- **Out to the public, from the same upload.** A small self-hosted service
  reads the segments already in the bucket and pushes them to YouTube, Facebook
  or any RTMP destination, so the main site uploads once whether the service is
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
  so any machine running OBS can take either role, and what originates a service
  is a laptop with OBS on it. A broadcast can come from a guest speaker's laptop, a
  conference venue for one week, a campus hosting this week's combined service,
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
  node fetch any segment without a directory service.

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
  is deliberately non-fatal — a service must not be held off air because an
  index entry failed — and events recorded before it existed are still found by
  that slower scan.
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
- **Audio interfaces:** ASIO or Blackmagic DeckLink devices. A de-interleaver
  that maps packed channels onto device output channels at the satellite is not
  built (see §9).

**Which mode suits which satellite.** The two modes are not competing for the
same sites. An **OBS satellite** wants multi-track: OBS routes sources
independently, so separate tracks land on separate destinations with no
de-interleaving. An **appliance** driving HDMI or SDI wants packed: its output
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

Choosing an event pins playback to it. A service starting mid-watch does **not**
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
  audio rather than stereo only. Packed is therefore the appliance's mode even
  though multi-track is the primary one overall (§4.3): an appliance has one
  output device, not a mixer. Fed a multi-track event it plays the first track;
  distributing several tracks across output channels is not built.
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
  always best left showing the last frame of a service.
- **A script on a stock distribution, not an image.** One command on stock
  Raspberry Pi OS installs the dependencies, builds, installs the service and
  enables it. It works from day one with no release infrastructure, updates
  are the same command again, and it does not tie the project to particular
  hardware. A prebuilt package can follow once the player has settled.
- **The preview is decoupled from the output on purpose.** An operator lining
  up a cue needs to see what is coming while the screen in the room holds the
  last picture. If the preview were the output there would be no way to look
  ahead without putting it to air — the thing a satellite campus most needs to
  avoid.

## 8.2 Public simulcast relay

The same segments that feed the campuses, pushed out to YouTube, Facebook or
any RTMP destination. A separate sub-project in `relay/`, deployed as one
Docker container on a small VPS. It is not part of the plugins and the core
knows nothing about it.

**Why relay from the bucket rather than add a second OBS output.** The main
site uploads once however many places the service goes, which is what makes
this possible at all on a venue connection that will not carry a second
upload. The public stream also inherits the buffering the campus feed already
has: the relay deliberately runs a configurable time behind the service —
three minutes by default — so a dropout at the main site is absorbed instead
of reaching air. It is the same trade as §1, applied to the public stream:
latency spent to buy resilience.

**Why RTMP.** Every destination accepts it, so one mechanism covers YouTube,
Facebook and a church's own server without two code paths. The cost is that
RTMP is H.264 in practice, which the codec rule below exists to handle.

**Copy remux, never a silent transcode.** Segments are pushed on unchanged: no
decode, no encode, no quality loss, and little enough CPU that the cheapest VPS
tier is the target rather than a stretch. What cannot be sent that way is
refused rather than adapted, in the two cases where adapting it silently would
put the wrong thing on air:

- **HEVC and AV1.** RTMP wants H.264. ffmpeg will mux either of the others into
  FLV and report success, producing a well-formed stream the destination then
  rejects — measured, not assumed — so nothing downstream can be relied on to
  notice. The relay refuses and says which encoder setting to change.
  Re-encoding on the way out is the eventual answer, is not built, and would
  end the $5-a-month claim when it is.

  This costs less than it might appear. H.264 is the default and the roadmap's
  first codec precisely because it decodes everywhere, a Pi 5 included (§8.1:
  software decode handles 1080p comfortably), so a site that has not gone out
  of its way to change codec can stream publicly with nothing to reconsider.
  What it does mean is that HEVC is not a free bandwidth saving for a site that
  also streams to the public: choosing it for the campuses currently costs the
  public stream.
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
disagree about whether a service is still running — and an event that ends
cleanly is played out to its last segment and then closed deliberately, rather
than being cut off or left to time out.

**Not built.** Re-encoding; splitting packed audio; signing in to YouTube (a
stream key is pasted, and the broadcast is still created in YouTube's own
page); and starting by itself, either on a schedule or when the encoder goes
live. Scheduling matters most of the three, because services start late — the
intended trigger is `live.json` actually going live, optionally bounded by a
time window, and `markers.json` makes "start the public stream at Sermon
Start" possible.

## 9. Capability overview

What this project does, and where each piece stands. Status is against the
codebase, not against anything else on the market — where a commercial platform
is the better answer for a given church, section 12 says so plainly.

| Capability | Status |
|---|---|
| Resilient store-and-forward upload | built |
| Multisite to any number of campuses (storage cost only) | built |
| Markers / service cues | built |
| Pause & hold at a campus, resuming exactly where it stopped | built |
| Per-campus independent DVR position | built |
| Multi-track production audio (main / ISOs / click), up to 6 tracks | built — one source per track at the satellite (§4.3) |
| Event browsing with live / recording / interrupted state | built |
| Video-on-demand playback of past and interrupted services | built |
| Any OBS machine can originate a broadcast | built |
| Dedicated receive appliance (Raspberry Pi / mini-PC) | built, not yet run through a service |
| Self-hosted, on storage you own | built |
| Open protocol, no vendor lock-in | by design — the whole protocol is §4 |
| Public simulcast to YouTube / Facebook / RTMP | built, not yet run through a service — H.264 feeds only (§8.2) |
| Per-channel routing of packed audio (de-interleaver) | not built |
| Re-encoding an HEVC feed for a streaming site | not built (§8.2) |
| Web / mobile simulcast from the same files | planned; CMAF makes it feasible |
| Scheduling / auto-go-live | planned — for the relay as well as the encoder |

Further directions to explore: web/mobile simulcast served directly from the
bucket (which needs no relay at all — the CMAF objects are already the right
shape for it), multi-bucket mirroring for redundancy, and local insertion
windows for campus announcements.

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
- **Phase 6 — Satellite appliance.** 🟨 Headless Linux decoder with HDMI output
  and a browser-based operator UI (section 8.1), built on the existing receive
  core. Built: the player engine, DRM/KMS display output with its own
  modesetting, ALSA multichannel audio, the splash and idle screens, the web
  control surface (decoder controls, event list, storage and system settings,
  decoupled preview), and the systemd/install path that makes it start on
  power-up. Outstanding: the channel de-interleaver, DeckLink SDI output for
  the production tier, and hardware-decoder selection on Pi 4.
- **Phase 7 — Extensions.** 🟨 Built: the public simulcast relay (§8.2), as a
  separate container in `relay/` — copy remux to one or more RTMP
  destinations, per-destination audio selection, a delay buffer, and
  supervised reconnection, with its own browser UI. Like the appliance it has
  not yet carried a service, and it will not send an HEVC feed. Not started:
  web/mobile simulcast served from the bucket, scheduling and auto-go-live,
  redundancy, and local insertion.

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
growing into multiple locations.

Multisite streaming is a solved problem if you are a large church in a
well-connected part of the world. The commercial platforms that solve it are
good, and the teams behind them have earned their place. But they are largely
unavailable outside the developed world, and where they are available the
recurring cost is out of reach for a congregation whose entire annual AV budget
is smaller than a year of subscription.

### 12.1 What this is not

**It is not a managed service.** The commercial products are, and that is worth
paying for. Someone answers the phone. Someone watches the infrastructure.
Someone ships you a decoder that boots and works. If your church can afford one
and it is available where you are, you should probably buy it.

This is a set of tools instead. Setting it up requires a reasonably technical
person, or support from an integrator with the relevant expertise. There is no
support contract, no uptime guarantee, and no one to call. What there is
instead: you own your storage, you own your content, your ongoing cost is a few
dollars a month of object storage, and nothing can be taken away from you or
priced beyond your reach later.

**It is not low latency, and it is not two-way.** This carries a service from
one site to others with a delay measured in tens of seconds. It cannot support a
live conversation between campuses, a two-way interview, or anything else where
people need to respond to each other in real time. For that, use SRT or WebRTC:
both are in OBS already, and there are many good hardware products built on
them. Those approaches trade differently, sitting much closer to the raw
condition of the connection at the moment you need it.

This project takes the opposite trade deliberately. Content is written to disk
before it is sent, sent again until the storage confirms it, and buffered deeply
at the far end before it is played. Minutes of the service can be held at the
satellite in advance, so an outage part-way through is something the
congregation never sees. Latency is the price, and for a service being relayed
rather than a conversation being held, it is a price worth paying.

### 12.2 What it asks of your network

Very little, and this is deliberate. Everything moves over ordinary HTTPS to
object storage. There are no inbound connections, no port forwarding, no static
IP, no VPN, and no firewall rules to negotiate with a building's IT.

That means it works on connections that would defeat a direct stream: mobile
data, LEO satellite, consumer fibre, and networks behind carrier-grade NAT. If a
laptop at the site can load a web page, it can usually send or receive a
service.

### 12.3 On intellectual property

This is a clean-room implementation built on published, open standards: CMAF
fragmented MP4, the S3 object API, and OBS Studio's public plugin interface. It
is not derived from, and does not reverse-engineer, any commercial product.

Where our design resembles existing products, it is because we are solving the
same problem under the same constraints and arriving at similar answers, or
because we have deliberately followed conventions that operators already
understand. Familiarity is a feature in a room where a volunteer is running the
service.

It is released under the MIT licence in support of kingdom expansion and the
enabling of local churches. There is no intent to tread on anyone's
intellectual property. If you believe something here does, please raise it with
us and we will address it properly.

### 12.4 Contributing

If this is useful to your church, use it. If you improve it, we would be glad to
see the change come back. If it fails you in an interesting way, a good bug
report is a real contribution: much of what works well here was fixed because
someone took the time to paste a log.
