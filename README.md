# obs-multisite

Distribute a live church event from a main campus to any number of satellite
campuses, reliably, over ordinary venue internet — using nothing but an
S3-compatible bucket you control.

**New here? [QUICKSTART.md](QUICKSTART.md) gets you broadcasting in twenty
minutes.** This README is the overview: what this is, how far along it is, and
where it falls short. The long-form material lives under
[Where to go next](#where-to-go-next).

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
> [Status](#status)), but it has not yet carried a real congregation's event.
> Interfaces, settings and the storage protocol may still change without a
> migration path, and there is no support contract, warranty or uptime
> guarantee of any kind.
>
> Production use comes with caveats. Run it only with a tested fallback in
> place, a technical person on hand, and the assumption that any given event
> may have to go ahead without it. Treat a successful rehearsal as necessary
> rather than sufficient.

---

## Where to go next

| You want to… | Start here |
|---|---|
| Get broadcasting in about twenty minutes | [QUICKSTART.md](QUICKSTART.md) |
| Install, configure and operate in depth | [Operator guide](docs/OPERATOR.md) |
| Choose between a PC and the Pi box | [Choosing a satellite](docs/SATELLITE.md) |
| Send the event to YouTube or Facebook | [Streaming to the public](docs/STREAMING.md) |
| Build, test or contribute | [Developer guide](docs/DEVELOPER.md) |
| Read the design and storage protocol | [PROJECT-SCOPE.md](PROJECT-SCOPE.md) |

What works, what does not yet, and what is planned next is in
[Status](#status), [Known gaps](#known-gaps) and [Roadmap](#roadmap) below.

---

## Why this exists

<details>
<summary>The longer story — who this is for, what it deliberately is not, what
it asks of your network, and the licensing position.</summary>

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

**It is not a managed event.** The commercial products are, and that is worth
paying for. Someone answers the phone. Someone watches the infrastructure.
Someone ships you a decoder that boots and works. If your church can afford one
and it is available where you are, you should probably buy it.

This is a set of tools instead. Setting it up requires a reasonably technical
person, or support from an integrator with the relevant expertise. There is no
support contract, no uptime guarantee, and no one to call. What there is
instead: you own your storage, you own your content, your ongoing cost is a few
dollars a month of object storage, and nothing can be taken away from you or
priced beyond your reach later.

**It is not low latency, and it is not two-way.** This carries an event from
one site to others with a delay measured in tens of seconds. It cannot support a
live conversation between campuses, a two-way interview, or anything else where
people need to respond to each other in real time. For that, use SRT or WebRTC:
both are in OBS already, and there are many good hardware products built on
them. Those approaches trade differently, sitting much closer to the raw
condition of the connection at the moment you need it. (The relay can *send*
SRT — see [Streaming to the public](docs/STREAMING.md) — but it sends
from the bucket, minutes behind, so it inherits this project's trade rather
than SRT's own.)

This project takes the opposite trade deliberately. Content is written to disk
before it is sent, sent again until the storage confirms it, and buffered deeply
at the far end before it is played. Minutes of the event can be held at the
satellite in advance, so an outage part-way through is something the
congregation never sees. Latency is the price, and for an event being relayed
rather than a conversation being held, it is a price worth paying.

### What it asks of your network

Very little, and this is deliberate. Everything moves over ordinary HTTPS to
object storage. There are no inbound connections, no port forwarding, no static
IP, no VPN, and no firewall rules to negotiate with a building's IT.

That means it works on connections that would defeat a direct stream: mobile
data, LEO satellite, consumer fibre, and networks behind carrier-grade NAT. If a
laptop at the site can load a web page, it can usually send or receive a
event.

### On intellectual property

This is a clean-room implementation built on published, open standards: CMAF
fragmented MP4, the S3 object API, and OBS Studio's public plugin interface. It
is not derived from, and does not reverse-engineer, any commercial product.

Where our design resembles existing products, it is because we are solving the
same problem under the same constraints and arriving at similar answers, or
because we have deliberately followed conventions that operators already
understand. Familiarity is a feature in a room where a volunteer is running the
event.

It is released under the **GPLv3** in support of kingdom expansion and the
enabling of local churches: free for any church to run, adapt and keep running
— and if you distribute a changed version, those changes have to reach the next
church too. That is the whole point of the choice. There is no intent to tread
on anyone's intellectual property. If you believe something here does, please
raise it with us and we will address it properly.

### Contributing

If this is useful to your church, use it. If you improve it, we would be glad to
see the change come back. If it fails you in an interesting way, a good bug
report is a real contribution: much of what works well here was fixed because
someone took the time to paste a log.

</details>

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

It has still **not carried a real congregation's event** — a soak test on
looping media is not a Sunday morning with people in the room.

A campus can receive in either of two ways — the OBS decoder on a PC, or the
Raspberry Pi appliance — and both are built. See
[Choosing a satellite](docs/SATELLITE.md). The appliance has not run a
event either.

The public simulcast relay is built and is the first piece of Phase 7. It has
pushed live streams to YouTube, sends over SRT as well as RTMP, and survives
having its encoder killed mid-stream — but it has not yet been through a full
event. HEVC can now go out over SRT; that path is verified against ffmpeg
but has not yet carried real encoder output.

**What works**

- Durable store-and-forward upload: nothing is lost through an outage, a crash,
  or a mid-event restart.
- CMAF segments from any OBS encoder — H.264 or HEVC via x264, NVENC, QuickSync
  or AMF.
- Satellite receive with a deep local buffer, checksum verification, and
  timeslipping — hold, resume, catch up, scrub, jump to a marker. Playback
  waits until a minute of the event is buffered before starting, so the
  picture never chases the live edge.
- **Multi-track production audio.** Up to 6 OBS tracks — main mix, ISOs, click —
  travel in the same fragment and are exposed at the satellite as separate
  sources, sharing one download and one playout clock.
- **Event browsing.** The decoder lists what a room has recorded, shows which is
  on air, which are finished recordings and which were cut short by an encoder
  that died, and plays any of them back.
- Finished *and interrupted* events play as video-on-demand from the beginning —
  an event whose encoder crashed is still watchable afterwards.
- Operator docks in plain language, plus hotkeys.
- **Public simulcast.** A separate container reads the same segments and pushes
  them to YouTube, Facebook or any RTMP destination — or over SRT, to a
  broadcast partner, a hardware decoder or a contribution CDN — a few minutes
  behind on purpose. See [Streaming to the public](docs/STREAMING.md).

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
                segments to YouTube, Facebook or any RTMP destination, or over
                SRT. Uses the core; the core knows nothing about it.
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

## Known gaps

<details>
<summary>What does not work yet, and what has not yet been proven. The short
version: never carried a real event, packed-channel routing is deliberately
out of scope, and the relay cannot re-encode.</summary>

- **Routing packed channels to separate outputs is not our job.** A packed
  feed arrives as one multi-channel stream, and in OBS
  [atkAudio's plugin suite](https://github.com/atkAudio/PluginForObsRelease)
  already does the routing — to ASIO, CoreAudio or Windows Audio devices, with
  VST3/AU hosting alongside. A de-interleaver of our own was planned and has
  been dropped: it would have been a worse version of something that exists.
  Multi-track audio needs none of it, since each track is already its own
  source. On the appliance the packed channels go out of HDMI in order, which
  is what an eight-channel de-embedder expects.
- **AV1 is carried but lightly exercised**, unlike H.264 and HEVC.
- **Seeking is accurate to about a second**, not to a frame.
- **The relay will not send an HEVC feed to a streaming site.** Those want
  H.264 over RTMP, and re-encoding on the way out is not built. An SRT
  destination carries HEVC unchanged, so this is no longer a straight trade
  against streaming publicly — but it does mean YouTube and Facebook stay out
  of reach for an HEVC site. H.264 remains the default and decodes fine
  everywhere, Raspberry Pi campuses included.
- **HEVC over SRT has not carried real encoder output.** The remux is verified
  — ffmpeg copies HEVC into MPEG-TS correctly and it reads back as HEVC at the
  far end — but no event has yet gone out that way from an actual HEVC
  encoder. Rehearse it before relying on it.
- **SRT in listener mode needs a port opened**, and nothing is shipped to help.
  Publish it on the container and open it on the firewall yourself; unlike the
  web interface there is no proxy in front of it.
- **The relay cannot split packed multi-channel audio**, and cannot start
  itself on a schedule or when the encoder goes live.
- **Replaying a past event is a proof of concept.** One at a time, started by
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
- **Not yet used for a real event.** A six-hour soak has been run (see
  [Status](#status)) but no congregation has watched anything through this. The
  soak covered sustained upload, timeslipping and playout; it did not cover a
  room full of people, a volunteer under pressure, or a venue's actual network
  on a Sunday. The relay has run 44 minutes unattended without a fault, which
  is encouraging and is not an event.

</details>

---

## Roadmap

<details>
<summary>What is planned next: finishing the appliance (Phase 6), the relay's
remaining extensions (Phase 7), and an external control API (Phase 8).</summary>

- **Phase 6 — Satellite appliance.** Built and installable for the ARM64 /
  Raspberry Pi HDMI tier (see above), and now proven on a Pi 5 — though not
  yet through an event. Still to come: DeckLink SDI output for the production
  tier, and hardware-decoder selection on Pi 4. The packed-channel
  de-interleaver has been dropped rather than deferred; see
  [Known gaps](#known-gaps).
- **Phase 7 — Extensions.** The public simulcast relay is built (see
  [Streaming to the public](docs/STREAMING.md)) and has not yet carried a
  event. SRT output, caller and listener, is in and carrying a real client.
  Still to come there: re-encoding, so an HEVC feed can reach a streaming site
  too; splitting packed multi-channel audio; signing in to YouTube instead of
  pasting a stream key; and starting automatically when the encoder goes live.
  Not started at all: a hosted streaming provider (Mux or Cloudflare Stream)
  for a persistent web player embed, web and mobile simulcast served straight
  from the bucket, redundancy, and local insertion.

- **Phase 8 — External control API.** So an event can be run from a physical
  button rather than a dock. The plan: both plugins expose their commands as
  **obs-websocket vendor requests**, using the same names and payloads as the
  appliance's existing HTTP routes — one API, two transports, and a control
  surface written against either works against both. obs-websocket ships with
  OBS 28 and later, so there is nothing extra to install, and the plugin gains
  no dependency: if it is absent, control simply is not there. Then a
  **Bitfocus Companion** module for buttons that light up and displays that
  show how far behind live a campus is — that needs a real module, because
  Companion's generic vendor-request action can send commands but cannot read
  state back.

  **Already possible, today:** the plugins register ten named hotkeys, and
  Companion's OBS module can trigger hotkeys by id — so play, stop, hold,
  resume, catch-up, jog and drop-marker work from a Stream Deck now, without
  parameters or feedback. Worth wiring up before any of the above is built.

</details>

---

## License

**GPL-3.0-or-later** — see [LICENSE](LICENSE), and [COPYRIGHT](COPYRIGHT) for
the notice and the third-party components. Copyright (C) 2026 Stage Audio
Works.

What it means in practice: run it, adapt it, install it for as many churches as
you like. If you distribute a modified version — as a binary or as source —
those modifications are GPLv3 too, and recipients get the source. It places no
condition on the events you broadcast with it, or on anything in your bucket.

Releases up to and including **v0.1.4-alpha were MIT**, and that grant cannot
be withdrawn: anyone who has those versions keeps their MIT rights to them.

This is compatible with OBS, which is **GPL-2.0-or-later** — the "or later" is
what makes a GPLv3 plugin lawful in a GPLv2 host. Vendored `nlohmann/json`
stays MIT, which is GPL-compatible and not ours to relicense.
