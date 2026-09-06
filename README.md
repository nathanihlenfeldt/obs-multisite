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
> development. A six-hour continuous soak has been run end to end (see
> [Status](#status)), but it has not yet carried a real congregation's service.
> Interfaces, settings and the storage protocol may still change without a
> migration path, and there is no support contract, warranty or uptime
> guarantee of any kind.
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
has been run between two Windows machines through a **six-hour continuous soak
test**: 3,661 segments, over 15 GB uploaded, **zero retries and zero upload
failures**, and 10 lagged frames out of 658,837 (0.0%). Six audio tracks were
carried throughout, timeslipping held a campus a steady two and then three
minutes behind live for hours, a scrub back nearly three hours and a return to
live both recovered cleanly, and the satellite played out to the end of the
recording when the broadcast stopped.

Audio and video stayed in sync across the whole run, checked by eye and ear as
well as by the reported A/V offset, which held between 0.005 s and 0.021 s
through several decoder restarts.

It has still **not carried a real congregation's service** — a soak test on
looping media is not a Sunday morning with people in the room.

A campus can receive in either of two ways — the OBS decoder on a PC, or the
Raspberry Pi appliance — and both are built. See
[Choosing a satellite](#choosing-a-satellite). The appliance has not run a
service either.

The public simulcast relay is built and is the first piece of Phase 7. It has
pushed live streams to YouTube and survives having its encoder killed
mid-stream, but it has not yet been through a full service, and it will not
send an HEVC feed.

**What works**

- Durable store-and-forward upload: nothing is lost through an outage, a crash,
  or a mid-service restart.
- CMAF segments from any OBS encoder — H.264 or HEVC via x264, NVENC, QuickSync
  or AMF.
- Satellite receive with a deep local buffer, checksum verification, and
  timeslipping — hold, resume, catch up, scrub, jump to a marker.
- **Multi-track production audio.** Up to 6 OBS tracks — main mix, ISOs, click —
  travel in the same fragment and are exposed at the satellite as separate
  sources, sharing one download and one playout clock.
- **Event browsing.** The decoder lists what a room has recorded, shows which is
  on air, which are finished recordings and which were cut short by an encoder
  that died, and plays any of them back.
- Finished *and interrupted* events play as video-on-demand from the beginning —
  a service whose encoder crashed is still watchable afterwards.
- Operator docks in plain language, plus hotkeys.
- **Public simulcast.** A separate container reads the same segments and pushes
  them to YouTube, Facebook or any RTMP destination, a few minutes behind on
  purpose. See [Streaming to the public](#streaming-to-the-public).

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

### First, a retention rule on the bucket

**Do this once, before your first broadcast.** Nothing in this project deletes
anything — the plugins only write and read. Expiry is a **bucket lifecycle
rule** you configure in your storage provider's console, and without one every
service you ever broadcast stays in the bucket and the bill grows without
limit. At 6 Mbps that is roughly **2.7 GB per hour** of service.

Two prefixes need a rule, both with the same age:

| Prefix | What it holds |
|---|---|
| `events/` | the media — all of the volume |
| `rooms/` | the per-room event index — tiny, but if it outlives the media the event list fills with recordings that cannot be played |

**Seven days is the design default, and the rule *is* your DVR depth** — a
campus can timeslip back only as far as retention allows, so this setting is
not merely housekeeping.

On **Cloudflare R2**: your bucket → Settings → Object lifecycle rules → Add
rule → prefix `events/`, delete objects 7 days after creation; then the same
for `rooms/`. (`rooms/{room}/live.json` is rewritten on every heartbeat, so it
stays fresh while a room is in use, and ageing out between services is
harmless — the next Go Live recreates it.)

On **AWS S3, MinIO, Backblaze B2 or Wasabi**: the equivalent lifecycle
configuration with an Expiration rule per prefix.

Object *tagging* is off by default and is deliberately not the mechanism: R2
rejects `x-amz-tagging`, and a tag never deletes anything by itself. Enable it
only if your store expires by tag and you have a rule that matches.

Expiry is passive on purpose. A paused or behind-live campus can still fetch
older segments for the whole retention window, which is what makes deep
timeslipping possible; an encoder that actively deleted as it went would take
that away.

### Main site (encoder)

1. Open the **Multisite Encoder** dock (View → Docks).
2. **Settings…** — enter your bucket details, choose a video encoder, name your
   markers. Settings are saved as you type.
3. **Go live.** Watch the status readout: how much of the service has been sent,
   how much is waiting, and link health.

**Production audio** is set up in OBS itself, not in the dock. In Settings →
Output → Recording, enable the audio tracks you intend to send; in Advanced
Audio Properties (right-click the mixer), assign each source to its tracks —
main mix on track 1, a click on its own track, ISOs on theirs. Name them under
**Settings… → Track labels** so satellites see "Click" rather than "Track 3".
Every enabled track travels in the same segment, locked to the picture.

Sending stereo only? Do nothing: track 1 is the default at both ends.

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

For production audio, the Multisite Source carries the video plus **one** audio
track (track 1 by default — the main mix). To bring in an ISO or the click as
well, add a **Multisite Audio Track (Decoder)** source for the same room and
pick the track. It attaches to the decoder already running, so it costs no extra
download: every track arrives in the same segment either way, and all of them
play from one clock.

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

Multi-track audio is what makes that practical: each track is a separate source
in OBS, so the main mix can go to the house system while the click goes to
in-ears, routed independently like any other source.

One caveat worth knowing before planning around this: every third-party plugin
named above is someone else's project, on its own release schedule. And if the
main site sends *packed* multi-channel rather than separate tracks, those
channels arrive as one stream — splitting them still needs the de-interleaver,
which is not built (see [Known gaps](#known-gaps)).

### Any location can be the origin

Both plugins are one module, so any machine running OBS can take either role.
What originates a service is a laptop with OBS on it, so a broadcast can start
anywhere someone can run it:

- a guest speaker or travelling pastor, publishing from wherever they are;
- a conference or camp venue, for a week, and then never again;
- a second campus hosting this week's combined service, with the usual main
  site receiving for once;
- a temporary or overflow site set up at short notice.

Adding an origin costs a room name and a key that can write to it. There is no
hardware to specify a year ahead, nothing to ship or clear through customs, and
nothing licensed per location — which matters most in exactly the places this
project is for.

The reliability argument is *stronger* for an occasional origin than for a
permanent one. A speaker broadcasting from a hotel, a phone hotspot or a venue
nobody surveyed has the worst connection anyone in the chain will have, and can
least afford a dropout halfway through a sermon. Because segments are written to
disk and resent until storage confirms them, that broadcast survives a link
which would kill a direct stream — it arrives whole or visibly incomplete, never
broken in the middle.

The latency rule is unchanged: tens of seconds each way means this relays a
service, it does not hold a conversation between sites.

Keep rooms separate — a guest publishes to `guest-speaker`, not to
`main-auditorium` — so an occasional broadcast can never be mistaken for the
main programme.

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

### macOS

Apple Silicon only, and the core needs no OpenSSL — it uses CommonCrypto from
libSystem, so a built plugin loads on a Mac that has never had Homebrew.
`ctest` should pass 13/13 with nothing installed but CMake and FFmpeg.

To build the plugin against an installed OBS, you need headers matching it
(they are not in the app) and `simde`, which OBS vendors as a submodule that
the source tarball omits:

```sh
brew install simde ffmpeg
curl -L https://github.com/obsproject/obs-studio/archive/refs/tags/32.2.2.tar.gz | tar xz
printf '#pragma once\n#define OBS_RELEASE_CANDIDATE 0\n#define OBS_BETA 0\n' > obsconfig.h
cmake -S . -B build -DBUILD_OBS_PLUGIN=ON \
  -DLIBOBS_INCLUDE_DIR=$PWD/obs-studio-32.2.2/libobs \
  -DLIBOBS_CONFIG_INCLUDE_DIR=$PWD \
  -DLIBOBS_LIBRARY=/Applications/OBS.app/Contents/Frameworks/libobs.framework/libobs \
  -DCMAKE_CXX_FLAGS=-I/opt/homebrew/include
cmake --build build --target obs-multisite
```

That produces `obs-multisite.plugin`, which goes in
`~/Library/Application Support/obs-studio/plugins/`. libobs resolves from
OBS.app at load time through `@rpath`, so the plugin carries no copy of it.

**One caveat before distributing such a build:** it will link Homebrew's
FFmpeg by absolute path, so it only loads on a machine with that exact
version installed. OBS ships its own FFmpeg in `OBS.app/Contents/Frameworks`,
and a release build has to link those instead — which is what obs-deps
provides and what a release job must use.


With the public simulcast relay (adds SQLite; needs the `ffmpeg` command at
run time, not at build time):

```sh
cmake -S . -B build -DMULTISITE_BUILD_RELAY=ON -DBUILD_PLAYER=OFF
cmake --build build --target multisite-relay
```

Or build the container, which runs the relay's tests as part of the image so a
broken build cannot become something somebody deploys:

```sh
docker build -f relay/Dockerfile -t multisite-relay .
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
relay/          the public simulcast relay: a container that pushes the same
                segments to YouTube, Facebook or any RTMP destination. Uses
                the core; the core knows nothing about it.
tests/          every guarantee above has a test.
scripts/player/ the install script and systemd unit for the appliance.
scripts/        optional Lua control script, superseded by the encoder dock.
```

**`src/core/` must stay free of OBS and Qt.** It is the shared engine behind
both the plugin and the headless appliance, and a test enforces this on every
build rather than trusting the convention. The appliance is what proves the
rule holds: it is a new output and control layer over the same receive core,
not a second implementation.

The relay is the same rule again, one step further out: it is a separate
sub-project that depends on the core and is never depended on by it. It builds
only when asked (`-DMULTISITE_BUILD_RELAY=ON`), so a plugin build is not made
to find SQLite for something it does not use.

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
  feed carries, it says so loudly rather than silently dropping the click. This
  is the one place *packed* multi-channel is the better mode: eight channels in
  one stream map straight onto HDMI's eight. An appliance fed multi-track plays
  the first track; routing several tracks onto output channels there is not
  built.
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

## Streaming to the public

The campuses are not always the only audience. `relay/` is a small self-hosted
service that reads the same segments and pushes them out to YouTube, Facebook,
or any RTMP destination.

It relays from the bucket rather than adding a second output to OBS, which
matters twice over. The main site uploads once whether the service is going to
two campuses or to two campuses and the internet — often the difference between
possible and not on a venue connection. And the public stream inherits the
buffering the campus feed already has: it runs a few minutes behind on purpose,
so a dropout at the main site delays it rather than breaking it.

```bash
docker run -d --name multisite-relay \
  -p 8080:8080 \
  -v multisite-relay-data:/data \
  -e RELAY_ROOM=main-auditorium \
  ghcr.io/stageaudioworks/multisite-relay:latest
```

Then open it in a browser, put in the bucket details, and add a destination.
A $5/month VPS is the target rather than a stretch, because nothing is being
re-encoded.

- **It has a login, and binds to localhost by default.** This service decides
  where your services are sent, so exposing it is a decision rather than a
  default. Put HTTPS in front of it; `relay/Caddyfile.example` is a working
  config.
- **One chosen sound feed per destination**, picked by the name the main site
  gave it — "Main Mix", "Sermon ISO" — never a track number. A future
  "clean feed to Facebook, main mix to YouTube" is just two destinations.
- **A delay you choose**, three minutes by default. This is the setting worth
  understanding: it is how much of the service the relay holds in hand, and so
  how long an outage at the main site can last before the public sees it.
- **It reconnects by itself** and resumes from where it stopped, so nothing is
  skipped. A silence under 45 seconds is ridden out without even dropping the
  connection.
- **It refuses rather than guesses.** An HEVC feed and packed multi-channel
  audio are both declined with a plain explanation, because sending either
  onward would mean a stream the destination rejects, or a mic ISO going out to
  the public.

It also does two things with services that have already finished:

- **Download one as an MP4**, streamed straight from storage — nothing is
  assembled on the server, so a two-hour service costs no disk. The file
  carries every audio track the main site sent, not just the streamed one, so
  the ISOs and the click are there for whoever edits it.
- **Replay one to a destination** as though it were happening now, for a
  second congregation or an evening repeat. This is a proof of concept: one at
  a time, started by hand, no scheduling yet.

Full deployment notes, including bandwidth and disk, are in
[relay/README.md](relay/README.md).

---

## What the tests cover

Twelve suites, all runnable without OBS (the `cmaf*` ones need FFmpeg):

| suite | what it proves |
|---|---|
| `reliability` | durability across a crash, ordered drain through an outage, checksum rejection, permanent-failure handling |
| `session` | the write-ordering invariant holds continuously, including across a crash and resume; packed multi-channel audio round-trips, channel order intact |
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

- **No channel de-interleaver**, which limits the *packed* mode only. Packed
  channels arrive as one multi-channel stream and cannot be split to individual
  outputs; it is planned as part of the appliance, where it is a channel map
  rather than an OBS plugin. Multi-track audio does not need it — each track is
  already its own source.
- **AV1 is carried but lightly exercised**, unlike H.264 and HEVC.
- **Seeking is accurate to about a second**, not to a frame.
- **The relay will not send an HEVC feed.** Streaming sites want H.264 over
  RTMP, and re-encoding on the way out is not built. H.264 is the default and
  decodes fine everywhere, Raspberry Pi campuses included, so this only bites a
  site that has chosen HEVC to save bandwidth — for which it is currently a
  straight trade against streaming publicly.
- **The relay cannot split packed multi-channel audio**, and cannot start
  itself on a schedule or when the encoder goes live.
- **Replaying a past service is a proof of concept.** One at a time, started by
  hand, and it cannot be scheduled, looped, or started part-way in.
- **The relay speaks plain HTTP** and relies on something in front of it for
  TLS. It binds to localhost so that is a deliberate choice rather than an
  accident, but it does not terminate TLS itself.
- **Alignment between separate audio tracks is unverified.** The soak confirmed
  audio stays locked to the *picture*, but not that a click on one track lands
  at the same instant as the programme on another. Each track is emitted by its
  own OBS source, and OBS buffers sources independently — the timestamps are
  derived from one anchor by construction, but nobody has measured the result.
  A few milliseconds would be inaudible against video and useless to a band.
  To test it: send identical audio on two tracks, play one through the main
  source and one through a companion into the same mix, and listen for comb
  filtering.
- **Not yet used for a real service.** A six-hour soak has been run (see
  [Status](#status)) but no congregation has watched anything through this. The
  soak covered sustained upload, timeslipping and playout; it did not cover a
  room full of people, a volunteer under pressure, or a venue's actual network
  on a Sunday. The relay has run 44 minutes
  unattended without a fault, which is encouraging and is not a service.

---

## Roadmap

- **Phase 6 — Satellite appliance.** Built and installable for the ARM64 /
  Raspberry Pi HDMI tier (see above), but not yet run through a service. Still
  to come: the packed-channel de-interleaver, DeckLink SDI output for the
  production tier, and hardware-decoder selection on Pi 4.
- **Phase 7 — Extensions.** The public simulcast relay is built (see
  [Streaming to the public](#streaming-to-the-public)) and has not yet carried
  a service. Still to come there: re-encoding, so an HEVC feed can be streamed;
  splitting packed multi-channel audio; signing in to YouTube instead of
  pasting a stream key; and starting automatically when the encoder goes live.
  Not started at all: web and mobile simulcast served straight from the bucket,
  redundancy, and local insertion.

- **Phase 8 — External control API.** So a service can be run from a physical
  button rather than a dock. The plan: both plugins expose their commands as
  **obs-websocket vendor requests**, using the same names and payloads as the
  appliance's existing HTTP routes — one API, two transports, and a control
  surface written against either works against both. obs-websocket ships with
  OBS 28 and later, so there is nothing extra to install, and the plugin gains
  no dependency: if it is absent, control simply is not there. Then a
  **Bitfocus Companion** module for buttons that light up and displays that show
  how far behind live a campus is — that needs a real module, because
  Companion's generic vendor-request action can send commands but cannot read
  state back.

  **Already possible, today:** the plugins register ten named hotkeys, and
  Companion's OBS module can trigger hotkeys by id — so play, stop, hold,
  resume, catch-up, jog and drop-marker work from a Stream Deck now, without
  parameters or feedback. Worth wiring up before any of the above is built.

---

## License

MIT — see [LICENSE](LICENSE). Copyright (c) 2026 Stage Audio Works.

Vendored `nlohmann/json` is MIT.
