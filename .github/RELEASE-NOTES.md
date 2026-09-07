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

## What's new since v0.1.3-alpha

A fix-only release. Three faults in the OBS decoder, three in the campus
player, and the test gap that let one of them ship.

### The event list showed only the newest service

**If you have used this for more than one service, this is the one that
matters.** The per-room index arrived during v0.1.0-alpha, so any service
recorded before it has media in the bucket and no index entry. Discovery
treated a non-empty index as the whole truth and only scanned the bucket when
the index was completely empty — so the first service recorded with an index
made every earlier one invisible. Services were still in storage and still
playable; nothing would list them.

Discovery is now the union of the index and a scan, so an event with no entry
lists alongside those that have one. It affects the **relay's Past services
list too**, which shared the same code, and both are fixed by the same change.

Nothing needs re-uploading and nothing was lost — your older services should
simply appear again.

### Jump to live and seeking gave no sign they had worked

Jump to live did none of the bookkeeping a seek does, so the dock had nothing
to show and the operator got no acknowledgement that the button had done
anything. Both now report **LOADING…** and **BUFFERING…** the same way, and the
indication appears whatever state the decoder is in — including a recording
that is loaded but not yet playing, which is exactly when someone is lining up
a cue and needs to know their click landed.

### Stop left the picture on air

Stop halted the feed but never cleared the source, and OBS holds the last frame
it was given indefinitely, so the programme stayed on the campus screen and the
button looked broken. Stop now takes the picture off air. Holding the picture is
what **Hold** is for, and it remains a separate control.

### Campus player: the five-frames-a-second problem

Three fixes, and the first is the cause:

- **One audio track is played, not all of them.** A six-track service had every
  track going into the same stereo device — six times real time of audio into
  an output that accepts one. ALSA applied back-pressure on the same thread
  that presents video, so the picture starved behind it. Track 0 by default,
  with a picker in Settings ("Track to play") for a campus whose origin puts
  the house mix somewhere else.
- **Video decodes on more than one core.** FFmpeg defaults to a single thread
  and nothing set otherwise, so a Pi 5 was decoding on one of four cores. The
  log now also says which decoder FFmpeg chose and how many threads it opened.
- **The status line reports frame rate and dropped frames**, not a running
  total — 29042 and 23 look equally healthy until you divide by the interval.
  With `--verbose` it also reports how long presenting a frame takes, which
  separates a slow display path from frames that are not arriving.

### Campus player: selecting an event lost audio

Choosing an event in the web UI tore the decoder down without telling the
session, so the new decoder never received the init segment while the session
believed it had already sent one. The result was a burst of "first segment
arrived with no init segment" and a silent hole in the programme.

### Campus player: the service and the login prompt fought over tty1

Raspberry Pi OS Lite starts a login prompt on tty1, and the two took turns
evicting each other — the player got SIGHUP, died, restarted three seconds
later, and threw the login prompt off again. The unit now claims tty1
exclusively. `StartLimitIntervalSec` also moved to `[Unit]`, where systemd
actually reads it.

### Testing and documentation

The CMAF fragment tests shelled out to `cat`, `wc -l` and `test`, so they could
never pass on Windows — and nobody noticed, because the Windows CI runner has
no FFmpeg and never reached them. They run on all three platforms now.

The event-listing test only covered a bucket with *no* index entries, which is
precisely why the fault above shipped; the mixed case is covered now.

Documentation: the scope document claimed the encoder queue used SQLite (it
uses plain files, deliberately), said phases 6 and 7 were "not started"
directly above entries marking both built, and reproduced the README's "Why
this exists" word for word. Fixed, and about seventy duplicated lines removed.

## Installing

**macOS (Apple Silicon)** — unzip, move `obs-multisite.plugin` into
`~/Library/Application Support/obs-studio/plugins/`, then clear the download
quarantine flag before restarting OBS:

```sh
xattr -dr com.apple.quarantine ~/Library/Application\ Support/obs-studio/plugins/obs-multisite.plugin
```

**That step is not optional.** These builds are not code-signed or notarised,
and macOS refuses to load a quarantined unsigned bundle — silently. OBS starts
normally with no Multisite source, output or docks, and the log does not say
why.

The bundle links nothing OBS does not already ship: FFmpeg, Qt and libobs all
resolve out of the running OBS.app, at the versions OBS pins. Built against
OBS **32.2.2** and obs-deps `2026-07-15`; the exact versions it expects are
listed in `INSTALL-MACOS.txt` inside the zip. Apple Silicon only.

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

**The campus player** installs with one command on stock Raspberry Pi OS Lite
(64-bit); see the README. Updating is the same command again.

## Known gaps

- **Not yet used for a real service.** The soak covered sustained upload,
  timeslipping and playout. It did not cover a room full of people, a volunteer
  under pressure, or a venue's network on a Sunday.
- **The campus player has not carried a service either**, and its frame rate on
  a Pi 5 has not been re-measured since the audio and threading fixes above.
- **Alignment between separate audio tracks is unverified.** Audio stays locked
  to the picture — measured, and checked by ear — but nobody has confirmed that
  a click on one track lands at the same instant as the programme on another.
- **Routing packed channels to separate outputs is out of scope**, not
  pending. In OBS,
  [atkAudio's plugin suite](https://github.com/atkAudio/PluginForObsRelease)
  does it — and more — better than a de-interleaver of ours would have; it is
  a separate install under AGPL-3.0. A packed 8-channel / 7.1 feed carries
  through this pipeline with its channel order intact. On the appliance, one
  chosen track is played when fed multi-track, and packed channels go out of
  HDMI in order.
- **The relay has pushed live streams to YouTube** but has not been through a
  full service, and it **will not send an HEVC feed** — streaming sites want
  H.264 over RTMP and re-encoding is not built.
- **The relay does not terminate TLS.** It binds to localhost and expects a
  proxy in front of it; a working Caddy config is included.
- **Replaying a past service is a proof of concept** — one at a time, started
  by hand, with no scheduling.
- **AV1 is carried but lightly exercised**; seeking is accurate to about a
  second, not to a frame.

A good bug report is a real contribution — much of what works well here was
fixed because someone took the time to paste a log.
