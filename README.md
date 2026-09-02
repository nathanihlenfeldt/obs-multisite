# obs-multisite

A free, open-source, self-hosted platform for distributing a live service from a
main campus to any number of satellite campuses **reliably**, over commodity
hardware and unreliable venue internet. It runs as OBS Studio plugins and uses
nothing but an S3-compatible bucket you control — no central server, no database,
no vendor.

See `PROJECT-SCOPE.md` for the full design.

## Status

- **Phase 1 — reliability core:** complete and tested.
- **Phase 2 — format, namespace & audio:** CMAF muxer, multi-track audio,
  publishing layer (`live.json` / `event.json` / `manifest.json` / `markers.json`),
  and the S3 transport are all in. The OBS plugin module is written and ready to
  compile once `libobs` is available (`-DBUILD_OBS_PLUGIN=ON`).

Reliability is the top priority, so it's built and proven first. This is the
store-and-forward spine that guarantees no segment is ever lost, independent of
the media format (so it stands unchanged when CMAF muxing lands in Phase 2).

What's in `src/core/`:

- `spool_queue` — a durable, crash-safe on-disk queue. Each finished segment is
  written to disk (atomic write-then-rename) before upload, so nothing is lost if
  the network drops, OBS crashes, or the machine loses power. Segments drain in
  strict sequence order and are removed only once confirmed in the bucket.
- `retry_uploader` — drains the spool with exponential backoff + jitter, retrying
  indefinitely while live, in order, with health tracking (healthy / degraded /
  offline). A permanent error (e.g. bad credentials) surfaces instead of spinning.
- `model` — the storage protocol types: `live.json`, `event.json`, the rolling
  `manifest.json` (with per-segment checksums, video info, and multi-track audio),
  and `markers.json`.
- `checksum` — SHA-256 integrity for every segment, recorded in the manifest and
  verified on download.
- `aws_sigv4` + `crypto_*` — the AWS SigV4 request signer (validated
  byte-for-byte against botocore) with a platform crypto backend: Windows CNG
  (bcrypt, no external dependency) or OpenSSL elsewhere.

### Phase 2 so far — `cmaf_muxer`

`src/core/cmaf_muxer.cpp` wraps FFmpeg's fragmented-MP4 muxer to produce CMAF:
one `init.mp4` (ftyp+moov) plus media fragments (moof+mdat) cut on video
keyframes at the target duration. Video and **every enabled audio track are
multiplexed into the same fragment**, so the main mix, mic ISOs, and click stay
locked to the video and to each other. It is codec-agnostic — HEVC/AV1 need only
a different codec id and extradata.

Validated by `tests/test_cmaf.cpp`: a real 1-video + 2-audio source is fed
through the muxer, and every emitted fragment is re-assembled with the init
segment and checked with ffprobe/ffmpeg — all audio tracks present, video frames
intact, **zero decode errors**.

### Publishing layer — `session`

`src/core/session.cpp` ties muxer → spool → uploader → manifest and enforces the
protocol's central invariant: **a segment is only listed in `manifest.json`
after the object store confirms it durable.** It publishes the whole object
layout (`rooms/{room}/live.json`, `events/{ulid}/event.json`, `init.mp4`,
`segments/{seq:08d}.m4s`, `manifest.json`, `markers.json`), maintains the rolling
window, heartbeats `live.json`, appends markers, and continues the sequence when
resuming an interrupted event.

`tests/test_session.cpp` verifies the invariant *continuously* — the mock store
checks every manifest write against what it actually holds — including through a
simulated network outage and across a crash-and-resume.

### S3 transport

`src/core/s3_transport.cpp` is the production transport: libcurl + the validated
SigV4 signer, against any S3-compatible endpoint (R2, AWS S3, MinIO, B2, Wasabi).
It tags every object for lifecycle expiry, sets `Cache-Control`, distinguishes
retryable failures (network, 5xx, 429) from permanent ones (bad credentials), and
offers `self_test()` for a write/read-back credential check.

### OBS plugin module — compiles and links against real libobs

`src/obs/` registers `multisite_output`: it takes OBS's encoded packets (video +
up to 6 audio tracks via `OBS_OUTPUT_MULTI_TRACK`), muxes them to CMAF, and hands
fragments to the session. The OBS encode thread never blocks on the network —
publishing only writes to the local durable spool. It offers resume automatically
when a previous event was interrupted.

Verified against real OBS headers (libobs 30.0.2): the module compiles warning-
free, links, and exports all of OBS's required entry points
(`obs_module_load`, `obs_module_ver`, etc.). Build it with:

```
cmake -S . -B build -DBUILD_OBS_PLUGIN=ON
cmake --build build
```

`.github/workflows/obs-plugin.yml` does this on every push: a Linux job builds
against distro `libobs-dev` and asserts the entry-point symbols exist, and a
Windows job builds `libobs` from the pinned OBS 31.1.1 release plus obs-deps and
produces a downloadable `obs-multisite.dll` artifact.

### Decoder core — timeslipping (`decoder_session`, `segment_cache`)

`src/core/decoder_session.cpp` is the satellite receive path. The **playback
head is independent of the live edge**: downloads run ahead into a local disk
cache no matter where playback sits, so a campus can pause to hold for its own
welcome, sit behind live, or scrub backwards without losing anything. It polls
`live.json` + `manifest.json`, verifies every segment against its manifest
checksum before caching it, detects a dead encoder via manifest staleness, and
stalls rather than skipping when a segment is missing.

`tests/test_decoder.cpp` drives a simulated encoder publishing into a fake
bucket and checks the behaviours a campus depends on: prebuffer, cache filling
*while paused*, resume from the exact paused position, jump-to-live, scrub
bounds, corrupt-segment rejection and re-fetch, stale-encoder detection, and
no silent skipping across a gap.

### What's proven

`tests/test_reliability.cpp` runs on every push (Linux and Windows) and checks:

- a durable spool survives a simulated crash — a fresh process sees all pending
  work and reports the event as resumable;
- ordered draining through a simulated network outage confirms every segment,
  in order, with zero loss, once the link recovers;
- checksums detect corruption;
- a cleanly-ended event is not offered for resume;
- the rolling manifest window trims correctly and round-trips (including audio
  tracks and per-segment checksums);
- a permanent (403) error stops the drain and keeps the segment spooled rather
  than dropping it.

The same suite is cross-compiled to a Windows binary and passes there too.

## Build & test locally

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Requires CMake ≥ 3.16 and a C++17 compiler. On Linux/macOS you also need OpenSSL
dev headers; on Windows the crypto backend uses built-in bcrypt (no OpenSSL).

## Cloud build & test (no local setup)

Push to GitHub and the workflow in `.github/workflows/core-tests.yml` builds and
runs the test suite on both Ubuntu and Windows automatically. Watch it under the
repository's **Actions** tab. For a public repository this is free.

## Roadmap

- **Phase 1 — reliability core.** ✅ this.
- **Phase 2 — format, namespace & audio.** ✅ CMAF muxing, multi-track audio,
  publishing layer, S3 transport, and the OBS output module (pending a first
  compile against libobs).
- **Phase 3 — timeslipping** (per-campus live-DVR): decoder core ✅ (cache,
  download-ahead, pause/resume/jump-to-live/scrub, stale detection); OBS source
  wiring still to do.
- **Phase 4 — markers & cues.**
- **Phase 5 — user interface** (encoder Tools panel, decoder DVR dock).
- **Phase 6 — extensions** (web simulcast, scheduling, redundancy).

## License

MIT. Vendored `nlohmann/json` is MIT.
