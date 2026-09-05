# obs-multisite

Distribute a live church service from a main campus to any number of satellite
campuses, reliably, over ordinary venue internet — using nothing but an
S3-compatible bucket you control.

Two OBS Studio plugins in one module: an **encoder** at the main site that
publishes the programme as CMAF segments, and a **decoder** at each satellite
that receives, buffers deeply, and plays it out with per-campus timeslipping.
There is no central server, no database and no vendor. The bucket is a dumb file
store; all the intelligence is at the edges.

Design priority, in order: **reliability**, then quality, then simplicity, and
**latency last** — a satellite that is a minute behind but never drops is worth
far more than one that is two seconds behind and stutters.

---

## Status

Phases 1–5 are built and running against real Cloudflare R2: capture, upload,
the storage protocol, receive, timeslipping, markers, and the operator UI. It
has been exercised end to end between two Windows machines but **has not yet run
a real service** — a full-length soak test is the most valuable outstanding
task.

The satellite appliance (Phase 6) is built and installable on a Raspberry Pi —
see below — but has not yet run a service either. Extensions (Phase 7) are not
started.

**What works**

- Durable store-and-forward upload: nothing is lost through an outage, a crash,
  or a mid-service restart.
- CMAF segments with multi-channel production audio, H.264 or HEVC, from any OBS
  encoder (x264, NVENC, QuickSync, AMF).
- Satellite receive with a deep local buffer, checksum verification, and
  timeslipping — hold, resume, catch up, scrub, jump to a marker.
- Finished events play as video-on-demand from the beginning.
- Operator docks in plain language, plus hotkeys.

**What does not, yet** — see [Known gaps](#known-gaps).

---

## How it works

The encoder muxes OBS's encoded frames into CMAF fragments and writes them to a
bucket. The decoder polls two small JSON files to discover what is live, then
downloads segments ahead of playback into a local cache.

```
rooms/{room_id}/live.json        which event is live in this room
events/{ulid}/event.json         codec config, audio layout, start time
events/{ulid}/init.mp4           codec configuration for the event
events/{ulid}/segments/…m4s      the media
events/{ulid}/manifest.json      rolling window of confirmed segments
events/{ulid}/markers.json       cues dropped by the main site
```

**The invariant that makes it reliable:** a segment is listed in the manifest
only *after* the bucket has confirmed it stored. If a decoder can see an entry,
the object exists. Everything else — retries, crash resume, deep buffering —
builds on that.

For the full design, see [PROJECT-SCOPE.md](PROJECT-SCOPE.md).

---

## Using it

### Main site (encoder)

1. Open the **Multisite Encoder** dock (View → Docks).
2. **Settings…** — enter your bucket details, choose a video encoder, name your
   markers. Settings are saved as you type.
3. **Go live.** Watch the status readout: how much of the service has been sent,
   how much is waiting, and link health.

### Satellite (decoder)

1. Open the **Multisite Decoder** dock and enter the same bucket details under
   **Settings…**. These are stored per machine, so every source you add
   afterwards is already configured.
2. Add a **Multisite Source (Decoder)** to a scene.
3. **Load event**, let the buffer fill, then **Play** when you are ready. Use
   **Lock** during the service so nothing can be clicked by accident.

Hotkeys for play, stop, hold, resume, catch-up, jog and markers are in
Settings → Hotkeys.

---

## Build and test

Core and tests, no OBS required:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Requires CMake ≥ 3.16 and a C++17 compiler. On Linux and macOS you also need
OpenSSL headers; on Windows the crypto backend uses the built-in bcrypt, so
OpenSSL is not needed.

With the OBS plugin (adds libobs and FFmpeg):

```sh
cmake -S . -B build -DBUILD_OBS_PLUGIN=ON
```

With the operator docks (adds Qt6 and obs-frontend-api):

```sh
cmake -S . -B build -DBUILD_OBS_PLUGIN=ON -DENABLE_QT=ON
```

CI builds and tests the core on Linux x86, **Linux ARM64** and Windows, and
produces an installable Windows plugin. The ARM64 job exists because the planned
appliance runs there, so a regression is caught in CI rather than on hardware.

---

## Repository layout

```
src/core/       the portable engine: protocol, reliability, muxing, decoding.
                No OBS, no Qt. Shared with the appliance.
src/obs/        the OBS bindings: output, source, hotkeys, settings.
src/obs/ui/     the Qt docks. The only place Qt appears.
src/appliance/  the headless campus player: DRM/KMS and ALSA output, the
                splash and idle screens, and the web control surface.
src/appliance/web/  the operator interface. No framework, no CDN — a campus
                box often has no internet.
tests/          every guarantee above has a test.
scripts/player/ the install script and systemd unit for the appliance.
scripts/        optional Lua control script, superseded by the encoder dock.
```

**`src/core/` must stay free of OBS and Qt.** It is the shared engine behind
both the plugin and the headless appliance, and a test enforces this on every
build rather than trusting the convention. The appliance is what proves the
rule holds: it is a new output and control layer over the same receive core,
not a second implementation.

---

## The campus player (satellite appliance)

A small box at a campus that receives, decodes and plays out, with no
operator-facing desktop software. On stock **Raspberry Pi OS Lite (64-bit)**:

```bash
curl -fsSL https://raw.githubusercontent.com/StageAudioWorks/obs-multisite/main/scripts/player/install.sh | sudo bash
```

That installs the dependencies, builds the player, installs it as a service
that starts on power-up, and puts a screen up on the HDMI output showing the
box's own address. Everything else is done from a phone or tablet on the same
network — storage credentials, which room to follow, the output resolution and
frame rate, the sound device, the clock, and the transport controls during a
service.

- **It owns the display.** The player sets the KMS mode itself, so the output
  resolution and frame rate are exactly what was asked for and there is no
  desktop to be left in the wrong state. Pi OS Lite is the right image.
- **Production audio over HDMI.** Up to eight channels of LPCM, recovered at
  the campus with a de-embedder. If the device will not take every channel the
  feed carries, it says so loudly rather than silently dropping the click.
- **The preview is not the output.** The web UI shows the incoming picture at
  a rate the browser chooses, independently of what is on the screen in the
  room — so a cue can be lined up while the picture is held.
- **The cache belongs on a USB SSD.** It writes roughly 3 GB an hour, which
  will wear an SD card out. The installer looks for a USB drive and uses it;
  if there is none, both it and the interface say so.

Run it by hand while setting one up:

```bash
sudo multisite-player --config /etc/multisite-player/config.json --verbose
```

`journalctl -u multisite-player -f` is the whole diagnostic story; the last few
hundred lines are also in the interface, under Log, for an operator with a
phone and no SSH.

---

## What the tests cover

Nine suites, all runnable without OBS:

| suite | what it proves |
|---|---|
| `reliability` | durability across a crash, ordered drain through an outage, checksum rejection, permanent-failure handling |
| `session` | the write-ordering invariant holds continuously, including across a crash and resume; packed multi-channel audio round-trips |
| `decoder` | timeslipping: the cache fills while paused, resume continues exactly where it stopped, markers, seek-by-time, VOD playback, and that a gap stalls rather than silently skipping |
| `responsive` | UI queries stay fast while downloading — the property that keeps OBS usable during a service |
| `cmaf`, `cmaf_hevc` | the muxer produces decodable fragments for H.264 and HEVC, with multi-track audio |
| `cmaf_decode` | the round trip: what the muxer wrote, the decoder plays back |
| `s3_url` | endpoint and bucket values survive being pasted with schemes, slashes and whitespace |
| `core_portable` | the core has not acquired an OBS or Qt dependency |

---

## Known gaps

- **No event browsing.** The decoder follows `live.json`, so it plays whichever
  event is newest. Choosing an older recording needs `ListObjectsV2` in the
  transport — see scope §7.5.1, which also specifies showing each event's state
  (live / recording / interrupted).
- **Multi-track mode plays only the first audio track** at the satellite. The
  primary path is packed multi-channel, which carries end to end; separate
  audio-only sources for the multi-track alternative are not built.
- **No channel de-interleaver.** Packed channels arrive as one multi-channel
  stream. Routing them to individual outputs is planned as part of the
  appliance, where it is a channel map rather than an OBS plugin.
- **AV1 is carried but lightly exercised**, unlike H.264 and HEVC.
- **Seeking is accurate to about a second**, not to a frame.
- **No soak test yet.** Sustained behaviour over a full service is untested and
  is the highest-value thing that is not code.

---

## Roadmap

- **Phase 6 — Satellite appliance.** Built for the ARM64 / Raspberry Pi HDMI
  tier (see above). Still to come: the packed-channel de-interleaver, DeckLink
  SDI output for the production tier, and hardware-decoder selection on Pi 4.
- **Phase 7 — Extensions.** Web and mobile simulcast from the same files,
  scheduling, redundancy, and local insertion.

---

## License

MIT. Vendored `nlohmann/json` is MIT.
