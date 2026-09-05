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

## Why this exists

This project is developed by the projects team at **Stage Audio Works**, a
worship AVL integrator working across Africa, to support churches that are
growing into multiple locations.

Multisite streaming is a solved problem if you are a large church in a
well-connected part of the world. The commercial platforms that solve it are
good, and the teams behind them have earned their place. But they are largely
unavailable outside the developed world, and where they are available the
recurring cost is out of reach for a congregation whose entire annual AV budget
is smaller than a year of subscription.

### What this is not

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

### What it asks of your network

Very little, and this is deliberate. Everything moves over ordinary HTTPS to
object storage. There are no inbound connections, no port forwarding, no static
IP, no VPN, and no firewall rules to negotiate with a building's IT.

That means it works on connections that would defeat a direct stream: mobile
data, LEO satellite, consumer fibre, and networks behind carrier-grade NAT. If a
laptop at the site can load a web page, it can usually send or receive a
service.

### On intellectual property

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

### Contributing

If this is useful to your church, use it. If you improve it, we would be glad to
see the change come back. If it fails you in an interesting way, a good bug
report is a real contribution: much of what works well here was fixed because
someone took the time to paste a log.

---

## Status

Phases 1–5 are built and running against real Cloudflare R2: capture, upload,
the storage protocol, receive, timeslipping, markers, and the operator UI. It
has been exercised end to end between two Windows machines but **has not yet run
a real service** — a full-length soak test is the most valuable outstanding
task.

A campus can receive in either of two ways — the OBS decoder on a PC, or the
Raspberry Pi appliance — and both are built. See
[Choosing a satellite](#choosing-a-satellite). The appliance has not run a
service either. Extensions (Phase 7) are not started.

**What works**

- Durable store-and-forward upload: nothing is lost through an outage, a crash,
  or a mid-service restart.
- CMAF segments with multi-channel production audio, H.264 or HEVC, from any OBS
  encoder (x264, NVENC, QuickSync, AMF).
- Satellite receive with a deep local buffer, checksum verification, and
  timeslipping — hold, resume, catch up, scrub, jump to a marker.
- **Event browsing.** The decoder lists what a room has recorded, shows which is
  on air, which are finished recordings and which were cut short by an encoder
  that died, and plays any of them back.
- Finished *and interrupted* events play as video-on-demand from the beginning —
  a service whose encoder crashed is still watchable afterwards.
- Operator docks in plain language, plus hotkeys.

**What does not, yet** — see [Known gaps](#known-gaps).

---

## How it works

The encoder muxes OBS's encoded frames into CMAF fragments and writes them to a
bucket. The decoder polls two small JSON files to discover what is live, then
downloads segments ahead of playback into a local cache.

```
rooms/{room_id}/live.json        which event is live in this room
rooms/{room_id}/events/{ulid}.json   one entry per event, so a room's
                                 recordings list in a single request
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

To play something other than the live service, use the **Recordings** list:
pick a past service and press **Load recording**. Playback then stays on it —
if a new service starts mid-watch the dock offers the switch rather than taking
it, because being pulled out of a recording you are part-way through is worse
than being told. **Back to live** returns to following the room.

Hotkeys for play, stop, hold, resume, catch-up, jog and markers are in
Settings → Hotkeys.

> The event list needs the **`s3:ListBucket`** permission. Cloudflare's "Object
> Read & Write" token has it; an object-scoped or read-only token often does
> not, and the dock will say so rather than showing an empty list.

---

## Choosing a satellite

A campus can receive in one of two ways, and they suit different rooms.

**OBS on a PC** — the decoder is a *source in a scene*, so the campus can
produce around the relayed service. **The Pi appliance** — a fixed-function box
that plays the service and nothing else. Both are built; neither has yet run a
real service.

### What running the decoder in OBS makes possible

Because the relayed programme is an ordinary source, everything OBS does applies
to it. This is the reason to choose a PC over the appliance, and for many
churches it is the deciding factor.

**Local content over the relayed service**

- Lower thirds, campus announcements, scripture graphics, a countdown before the
  service, a logo bug — keyed over the incoming picture with OBS's normal
  sources and filters.
- Cut away entirely to a local camera for a campus host, a local worship set or
  notices, then back to the relay. The decoder keeps downloading while it is off
  screen, so returning does not mean re-buffering.
- Record the campus feed locally and simulcast it to YouTube or Facebook at the
  same time as it plays in the room.

**Video in and out**

- **Blackmagic DeckLink** and **AJA** are supported by OBS itself, in and out.
  A campus can take SDI to the house system and bring SDI in from a local
  camera on the same machine.
- **NDI** in and out through the DistroAV plugin (formerly obs-ndi), where the
  house system already runs NDI.
- Anything else OBS can see: HDMI capture cards, USB cameras, screen capture.

**Audio into the house system**

- **Dante** via Dante Virtual Soundcard or a Dante-enabled interface: OBS sees
  it as a normal output device, so the relayed programme lands on the Dante
  network alongside everything else the church already runs. The same approach
  works for AES67/AVB interfaces, USB interfaces, or an analogue break-out.
- Audio leaves OBS through its monitoring device, so whichever interface the
  room uses is the one to select there.

Two caveats worth knowing before planning around this. Every third-party plugin
named above is someone else's project, on its own release schedule. And packed
multi-channel audio currently arrives as a single multi-channel stream — routing
individual channels to separate destinations needs the de-interleaver, which is
not built (see [Known gaps](#known-gaps)).

### When the appliance is the better answer

The appliance gives all of that up on purpose. No scene, no overlays, no local
sources: it plays the relayed service, on a box that costs less than a monitor,
boots into the service on power-up, and is driven from a phone with no desktop
to leave in the wrong state.

Choose it where a campus needs the service on a screen and nothing more — an
overflow room, a chapel, a plant meeting in a school hall. Choose OBS where the
campus produces around the relay, or where it has to reach existing SDI, NDI or
Dante infrastructure.

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

The alternative to running the decoder in OBS: a small box at a campus that
receives, decodes and plays out, with no operator-facing desktop software. See
[Choosing a satellite](#choosing-a-satellite) for which suits a given room. On
stock **Raspberry Pi OS Lite (64-bit)**:

```bash
curl -fsSL https://raw.githubusercontent.com/nathanihlenfeldt/obs-multisite/main/scripts/player/install.sh | sudo bash
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

Twelve suites, all runnable without OBS (the `cmaf*` ones need FFmpeg):

| suite | what it proves |
|---|---|
| `reliability` | durability across a crash, ordered drain through an outage, checksum rejection, permanent-failure handling |
| `session` | the write-ordering invariant holds continuously, including across a crash and resume; packed multi-channel audio round-trips |
| `decoder` | timeslipping: the cache fills while paused, resume continues exactly where it stopped, markers, seek-by-time, VOD playback, and that a gap stalls rather than silently skipping |
| `responsive` | UI queries stay fast while downloading — the property that keeps OBS usable during a service |
| `snapshot` | the figures the dock reads agree with the session they are built from |
| `s3_list` | a ListObjectsV2 response is read correctly, including pagination and an access-denied body; a signed query string is canonicalised the way S3 does it |
| `event_catalog` | events are classified as live / recording / interrupted, rooms stay separate, a listing failure is not shown as "no recordings", and an event that recorded nothing is not offered |
| `cmaf`, `cmaf_hevc` | the muxer produces decodable fragments for H.264 and HEVC, with multi-track audio |
| `cmaf_decode` | the round trip: what the muxer wrote, the decoder plays back |
| `s3_url` | endpoint and bucket values survive being pasted with schemes, slashes and whitespace |
| `core_portable` | the core has not acquired an OBS or Qt dependency |

---

## Known gaps

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

- **Phase 6 — Satellite appliance.** Built and installable for the ARM64 /
  Raspberry Pi HDMI tier (see above), but not yet run through a service. Still
  to come: the packed-channel de-interleaver, DeckLink SDI output for the
  production tier, and hardware-decoder selection on Pi 4.
- **Phase 7 — Extensions.** Web and mobile simulcast from the same files,
  scheduling, redundancy, and local insertion.

---

## License

MIT — see [LICENSE](LICENSE). Copyright (c) 2026 Stage Audio Works.

Vendored `nlohmann/json` is MIT.
