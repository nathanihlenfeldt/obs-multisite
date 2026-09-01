# obs-multisite

A free, open-source, self-hosted platform for distributing a live service from a
main campus to any number of satellite campuses **reliably**, over commodity
hardware and unreliable venue internet. It runs as OBS Studio plugins and uses
nothing but an S3-compatible bucket you control — no central server, no database,
no vendor.

See `PROJECT-SCOPE.md` for the full design.

## Status: Phase 1 — reliability core (complete, tested)

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
- **Phase 2 — format, namespace & audio.** FFmpeg CMAF muxing, multi-track
  production audio, the `rooms/live.json` + `events/{ulid}` layout, and wiring
  the core into an OBS output plugin.
- **Phase 3 — timeslipping** (per-campus live-DVR).
- **Phase 4 — markers & cues.**
- **Phase 5 — user interface** (encoder Tools panel, decoder DVR dock).
- **Phase 6 — extensions** (web simulcast, scheduling, redundancy).

## License

MIT. Vendored `nlohmann/json` is MIT.
