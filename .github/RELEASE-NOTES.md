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

## 🔑 The licence has changed: MIT → GPLv3

**From this release the project is GPL-3.0-or-later.** Free for any church to
run, adapt and keep running — and if you distribute a changed version, those
changes have to reach the next church too. That is the whole point of the
choice. It places no condition on the services you broadcast with it, or on
anything in your bucket.

**Releases up to and including v0.1.4-alpha were MIT, and that grant cannot be
withdrawn:** anyone holding those versions keeps their MIT rights to that code.

This is compatible with OBS, which is GPL-2.0-**or-later** — the "or later" is
what makes a GPLv3 plugin lawful in a GPLv2 host. Third-party terms are set out
in `COPYRIGHT`.

## What's new since v0.1.4-alpha

### The relay speaks SRT, so an HEVC service can go out unchanged

RTMP means FLV, and FLV means H.264 — which is why choosing HEVC for the
campuses has until now cost a church its public stream outright. SRT means
MPEG-TS, which carries HEVC properly, so that trade is no longer forced.

- **There is no protocol setting.** `rtmp://` and `srt://` are unmistakable, so
  the address you paste decides. Asking a volunteer to declare which one they
  pasted is asking them to get it wrong.
- **Paste the whole thing.** An address arriving with the stream id, passphrase
  and latency already in the query is pulled apart on save, and the secrets are
  stored and treated exactly as a stream key always has been — never shown
  again, never written to the log.
- **Latency defaults to 2000 ms**, not ffmpeg's 120 ms. The relay already sits
  three minutes behind the service, so two seconds is invisible and buys
  recovery across a far longer path. SRT writes this in millionths of a second,
  so a vendor page saying `latency=2000` is refused with the unit spelled out
  rather than quietly taken as 2 ms.
- **Listener mode** — for a broadcast partner or a hardware decoder that
  connects to you rather than being pushed to — is asked for by leaving the
  host out: `srt://:9000`. That is deliberately the only way to request it,
  because it opens an inbound port on a machine otherwise kept closed.

AV1 is still refused on both protocols: too little of what would receive it can
decode it yet, and a stream that looks healthy here and is rejected at the far
end is the exact failure this design exists to avoid.

**The banner above the destinations no longer lies.** It answered "can this
service be sent at all?" with a single RTMP probe, so an HEVC service showed
"streaming sites need H.264" across the top of the page while an SRT
destination was busy sending it. There are three answers now — nothing can take
this service, anything can, or some can and some cannot — and the third is
shown as information rather than as an obstacle.

### The encoder dock offered two encoders out of seven

On a machine with NVENC, QuickSync and x264, the dock offered the two AV1
encoders and nothing else. The list was built while the module loaded, and OBS
loads modules alphabetically — so `obs-ffmpeg` had registered and `obs-nvenc`,
`obs-qsv11` and `obs-x264` had not. Even the x264 safety net failed, for the
same reason. The list is now built when Settings is opened, which is always
long after loading has finished.

If your encoder is still missing, the log now says which encoders were offered
and which were declined, with the reason for each.

### A machine can be set to one role

Both halves ship in one module, which is what lets any laptop originate a
broadcast — but a campus that only ever receives had an encoder dock to learn
to ignore, on the same screen as a volunteer one click from going live.

Under Settings, a machine can be **Both**, **main site only**, or **campus
only**. It hides panels and nothing else: the Multisite source and output are
always registered, so this cannot break a scene collection that already uses
one. Takes effect when OBS restarts.

### You can see what the link is doing

- **Both OBS docks** now show the plugin version on screen and a **Bucket**
  line: the Cloudflare edge serving the feed and the observed transfer rate —
  upload at the main site, download at a campus. A site in Johannesburg served
  from Amsterdam explains a latency nothing local would find.
- **The campus player** has a Storage panel: reachable or not, the colo, and
  the link speed. The figures come from the segment traffic already flowing, so
  opening it costs nothing; "Check now" spends one read-only request to settle
  it, which a satellite's read-only key can make.
- Figures that have not been measured show as `—`, never as zero. A link speed
  of 0 Mbps reads as a dead connection when it means "nothing measured yet".

### The campus player told the truth about itself

- **It reported the wrong version.** A second version string defaulted to
  "0.1.0" and nothing ever set it, so every box built since then said 0.1.0.
- **A frozen playhead read as perfect health.** One status line repeated
  identically for 112 minutes: playback had reached the end of a recording and
  stopped, but `behind=0s buffered=0s` is also what a campus keeping up
  perfectly reports. Every line now begins with what the player is doing —
  playing, held, stopped, or at-end.
- **A failed seek always blamed storage.** Five separate causes were all
  reported as "that moment is no longer available in storage". For the common
  case of scrubbing near the right-hand edge that was not vague but wrong:
  nothing had been removed. It now says which bound was hit.
- **The audio warning blamed the wrong thing.** It attributed sound breaking up
  to heat or power unconditionally; it now asks the board, which publishes both
  under-voltage and throttling, and says plainly when neither is the cause.

### An empty timeline no longer looks broken

The decoder dock's timeline drew a blank strip whenever it had no span — which
is the ordinary state before an event is loaded. "No source in the scene",
"nothing loaded yet", "nothing on air" and "something failed" all looked
identical. It now says which. The status row labelled "Now showing" is the
current marker rather than the position, and is named "Current cue".

### Underneath

- **CI never compiled the campus player's display or audio output.** The
  `libdrm` and ALSA development packages were not installed, so CMake quietly
  built a player with no display and no audio — around 850 lines, including the
  audio path whose fault made a player run at five frames a second, compiled by
  no job at all. Both are installed now, and a missing one fails the build
  rather than silently reverting to a headless one.
- The CMAF fragment tests shelled out to `cat`, `wc -l` and `test`, so they
  could never pass on Windows — and nobody noticed, because the Windows runner
  has no FFmpeg to reach them with. They run on all three platforms now.
- **A quick start.** The README is the long form; `QUICKSTART.md` is twenty
  lines to a first broadcast.
- The repository moved to `stageaudioworks/obs-multisite`.

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
- **The campus player has not carried a service either**, though it now holds
  30 fps on a Pi 5 through several runs, with a handful of dropped frames.
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
  full service.
- **HEVC to a streaming site needs SRT.** RTMP cannot carry it and re-encoding
  is not built, so YouTube and Facebook remain H.264 only. The HEVC-over-SRT
  remux is verified against ffmpeg — it reads back as HEVC at the far end — but
  **no service has yet been carried from a real HEVC encoder through the
  relay.** The transport is proven; that path is not. Rehearse it before
  relying on it.
- **AV1 is refused by the relay on both protocols**, deliberately: too little
  of what would receive it can decode it yet.
- **The relay does not terminate TLS.** It binds to localhost and expects a
  proxy in front of it; a working Caddy config is included.
- **Replaying a past service is a proof of concept** — one at a time, started
  by hand, with no scheduling.
- **AV1 is carried but lightly exercised**; seeking is accurate to about a
  second, not to a frame.

A good bug report is a real contribution — much of what works well here was
fixed because someone took the time to paste a log.
