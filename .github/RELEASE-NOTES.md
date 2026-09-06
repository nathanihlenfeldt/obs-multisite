## 🔴 If you are running v0.1.1-alpha's relay, read this first

**v0.1.1-alpha shipped the simulcast relay with no authentication of any kind,
and a `docker-compose.yml` that publishes it on every interface.** Anyone who
could reach that port could change a destination's address and stream key —
pointing a church's service at their own server — as well as stop it mid-service
and read the storage settings.

If you deployed it:

1. Close the port now (`docker compose down`, or firewall it), then update.
2. Treat the storage credentials it held as exposed and roll them, using
   read-only keys for the relay when you re-enter them.
3. Check the destinations list for anything you did not add yourself.

Fixed in this release: every endpoint but signing in now requires a login, and
the relay binds to localhost so publishing it is a decision rather than a
default. The campus appliance is not affected — it has always been a
LAN-only device and was never documented as internet-facing.

---

## ⚠️ Alpha — read this first

This is pre-release software. A **six-hour continuous soak test** has been run
end to end — 3,661 segments, over 15 GB, zero retries and zero upload failures,
10 lagged frames in 658,837 — but it **has not yet carried a real
congregation's service**. Interfaces, settings and the storage protocol may
still change without a migration path. There is no warranty, no support
contract and no uptime guarantee.

If you put this in front of a congregation, do it with a tested fallback in
place, a technical person on hand, and the assumption that any given service may
have to go ahead without it.

## Before you start: set a retention rule

**Nothing in this project deletes anything.** The plugins only write and read.
Expiry is a **bucket lifecycle rule** you configure once in your storage
provider's console, and without one every service you broadcast stays for ever
— roughly **2.7 GB per hour** at 6 Mbps.

Add a rule for the prefix `events/` and another for `rooms/`, both deleting
objects after the same number of days. Seven days is the design default, and
**the rule is also your DVR depth**: a campus can timeslip back only as far as
retention allows.

## What's new since v0.1.0-alpha

**Multi-track production audio, end to end.** Up to six OBS audio tracks — main
mix, mic ISOs, click — travel in the same fragment and are now delivered at the
satellite. The Multisite Source carries the video plus one chosen track (track 1
by default), and each further track is added as a **Multisite Audio Track
(Decoder)** source. Those attach to the decoder already following that room, so
a segment is downloaded once and decoded once however many tracks a campus
uses, and every track plays from one clock.

This is now the **primary** production-audio mode, with packed multi-channel as
the alternative. The reason is a failure mode: OBS resamples every source to its
global layout, so packed requires both ends set to 7.1, and a satellite that is
not silently downmixes — summing the ISOs and click into the programme with no
symptom until it is on air. Separate tracks each carry their own channel count,
so a stereo main mix beside a mono click works with OBS in plain stereo.

**Public simulcast relay.** `relay/` reads the same segments from the bucket and
pushes them to YouTube, Facebook or any RTMP destination, so the main site
uploads once whether the service goes to two campuses or to two campuses and the
internet. It runs a few minutes behind on purpose, so a dropout at the main site
delays the public stream rather than breaking it. Nothing is re-encoded, so a
$5/month VPS is the target rather than a stretch.

**A login on the relay, and it binds to localhost.** See the notice at the top:
this closes a real hole rather than hardening something already safe. Every
endpoint but signing in requires a session; passwords are stored as
PBKDF2-HMAC-SHA256 over a random salt; sessions are held in memory, so a restart
signs everyone out. A working Caddy config ships alongside for TLS, because a
password over plain HTTP protects nobody.

**Past services can be downloaded and replayed.** A finished service downloads
as one MP4, streamed from storage as it is requested rather than assembled on
the server, so a two-hour service costs no disk and several people can download
at once. It carries every audio track the main site sent, not just the streamed
one, so the ISOs and the click are there for whoever edits it afterwards. A
finished service can also be replayed out to a destination as though it were
live — a proof of concept: one at a time, started by hand, no scheduling yet.

**Fixes from the soak test.** Three defects showed up in six hours of logs:

- The packed-audio warning fired whenever more than two channel names were
  filled in, regardless of what was being sent — telling a correct six-track
  setup to switch OBS to 7.1, which would have widened every track for nothing.
  It now fires only when a track genuinely carries more channels than the global
  layout.
- The shutdown log reported the upload queue depth *before* draining it, so a
  clean stop that uploaded its last fragments still signed off "2 pending",
  reading as two segments lost when nothing had been.
- The companion audio source logged once per keystroke while typing a room name.

**`use_object_tags` is now `send_expiry_tag`.** The old name implied the encoder
managed expiry. It never did: a tag only gives a lifecycle rule something to
match, and Cloudflare R2 rejects tagging outright. The old settings key is still
read, so an encoder configured with tagging on keeps its setting.

**Documentation.** Why the project exists and who builds it; what it is *not*;
what running the decoder inside OBS makes possible (local overlays, DeckLink and
AJA, NDI, Dante); that any machine running OBS can originate a broadcast; and a
roadmap entry for an external control API aimed at Bitfocus Companion.

## Installing

**Windows** — unzip and copy the `obs-plugins` and `data` folders into your OBS
Studio install directory (typically `C:\Program Files\obs-studio\`), merging
with what is there. Built against OBS **32.2.2**; a different major version may
not load it.

**Linux** — extract and place `obs-multisite.so` in your OBS plugin directory
(commonly `~/.config/obs-studio/plugins/obs-multisite/bin/64bit/`) with the
contents of `data/` alongside. Links the system FFmpeg and libcurl.

Restart OBS. The encoder appears as an output and the decoder as a source, with
**Multisite Encoder** and **Multisite Decoder** docks under View → Docks.

You need an S3-compatible bucket and a key that can read and write it. For the
decoder's event list the key also needs `s3:ListBucket` — Cloudflare's "Object
Read & Write" token includes it, an object-scoped token does not, and the dock
will say so rather than showing an empty list.

## Known gaps

- **Not yet used for a real service.** The soak covered sustained upload,
  timeslipping and playout. It did not cover a room full of people, a volunteer
  under pressure, or a venue's network on a Sunday.
- **Alignment between separate audio tracks is unverified.** Audio stays locked
  to the picture — measured, and checked by ear — but nobody has confirmed that
  a click on one track lands at the same instant as the programme on another.
- **No channel de-interleaver**, which limits the packed mode only.
- **The appliance has not carried a service.** The relay has pushed live
  streams to YouTube but has not been through a full service either, and it
  will not send an HEVC feed.
- **The relay does not terminate TLS.** It binds to localhost and expects a
  proxy in front of it; a working Caddy config is included.
- **Replaying a past service is a proof of concept** — one at a time, started
  by hand, with no scheduling.
- **AV1 is carried but lightly exercised**; seeking is accurate to about a
  second, not to a frame.

A good bug report is a real contribution — much of what works well here was
fixed because someone took the time to paste a log.
