## ⚠️ Alpha — read this first

This is pre-release software. A six-hour continuous soak test has been run end
to end — 3,661 segments, over 15 GB, zero retries and zero upload failures, 10
lagged frames in 658,837 — but it has still not carried a real congregation's
event. Interfaces, settings and the storage protocol may change without a
migration path. There is no warranty, no support contract and no uptime
guarantee.

If you put this in front of a congregation, do it with a tested fallback in
place, a technical person on hand, and the assumption that any given event may
have to go ahead without it.

## Before you start: set a retention rule

**Nothing in this project deletes anything.** The plugins only write and read.
Expiry is a bucket lifecycle rule you configure once in your storage provider's
console, and without one every event you broadcast stays for ever — roughly
**2.7 GB per hour** at 6 Mbps.

Add a rule for the prefix `events/` and another for `rooms/`, both deleting
objects after the same number of days. Seven days is the design default, and
**the rule is also your DVR depth**: a campus can timeslip back only as far as
retention allows.

## Licence

This project is **GPL-3.0-or-later** (it moved from MIT at v0.1.5-alpha).
Releases up to and including v0.1.4-alpha were MIT, and that grant cannot be
withdrawn: anyone holding those versions keeps their MIT rights to that code.
Third-party terms are set out in `COPYRIGHT`.

## Fixed since v0.1.6-alpha

**The campus player leaked memory on every idle-screen redraw.** The new
identity screen's FreeType text renderer opened and parsed a font file on
every redraw and never released it — one `FT_Library` and one font face,
abandoned each time. Not a single leak and not bounded by anything an
operator does: the idle screen redraws whenever its content changes, which
includes the box's own IP address, so even a DHCP renewal on an otherwise
quiet box would trigger it. A box left running for days between events
would have accumulated this the whole time.

Confirmed both ways before calling it fixed: a standalone harness built
against the released v0.1.6-alpha source called the renderer 3,000 times and
watched memory climb continuously, still rising when the run ended; the same
harness against this fix plateaus within two calls and stays flat for the
rest. If you have installed v0.1.6-alpha's campus player on a box you intend
to leave running, update it.

Nothing else changes in this release. v0.1.6-alpha's own notes are below.

## What's new since v0.1.5-alpha

### The campus player got a proper identity screen

The Raspberry Pi appliance's idle screen — the first thing a room sees — has
been rebuilt:

- **A QR code to the control page.** Point a phone at the screen and it opens
  the operator UI in the browser. No typing, no laptop; the address is still
  printed alongside for anyone who prefers it.
- **A five-second boot splash.** On power-up the identity screen now shows for
  five seconds whatever is configured — even with auto-play and a live event
  already arriving — so the box visibly proves it is alive before the picture
  takes over. (The box comes in a few seconds into the event; it sits minutes
  behind live anyway.)
- **A modern look.** The screen has a gradient background and anti-aliased text
  rendered from the system font through FreeType, instead of the 5×7 bitmap
  font. A build without FreeType falls back to the bitmap font, so nothing
  here is a hard dependency.

### The one-line installer is now reliable

`curl …/install.sh | sudo bash` has been hardened after GitHub's raw CDN took a
few days off, and after a branch-switch bug made an update fail without a
message:

- The installer **retries its GitHub fetch** and reports what is wrong instead
  of stopping silently — the failure that previously looked like "nothing
  happened".
- It **switches branches correctly** (a plain fetch wrote only `FETCH_HEAD`, so
  the first change of branch failed).
- It **force-updates its tracking ref**, so a shallow clone no longer rejects an
  update it cannot see as a fast-forward.

The command is unchanged in shape, just sturdier:

```bash
curl -fsSL --retry 5 https://raw.githubusercontent.com/stageaudioworks/obs-multisite/main/scripts/player/install.sh | sudo bash
```

### The documentation is restructured

The README was an 800-line wall that tried to be the pitch, the operator
manual, the appliance guide and the developer guide at once. It is now a
landing page that points at four focused guides under `docs/` — the operator,
choosing a satellite and the appliance, public streaming, and building and
testing. Nothing was deleted: the long-form material was moved, and the old
links still resolve.

### Decoder and relay fixes

- **The decoder dock's clock was a third short.** The time base was set by the
  last segment listed, which for a finished recording is the partial fragment
  the broadcast ended on. "Behind live", the timeline axis, the rewindable
  figures and a recording's total length were all wrong by the same factor; the
  estimator now uses the median segment, which one atypical sample cannot move.
  Three smaller faults found while tracing it — a stale playback head across an
  event change, a snapshot that bypassed its own clamp, clipped axis labels —
  are fixed too, and a test locks the estimator to the numbers from the
  recording that exposed it.
- **The playing clock no longer pairs two different fragments.** A jump could
  label the new position with the old fragment's time base; it is latched now.
- **The relay reports its version.** `--version`, the startup log line and
  `/api/status` now say what is actually deployed — which matters for a
  container you cannot simply look at.
- **ffmpeg's stderr is redacted.** The command line was redacted, but ffmpeg
  echoed the full output URL — stream key and SRT passphrase included — back in
  its error message, which reached the browser and the container log. It no
  longer does. The message keeps the hostname and the reason, so nobody has to
  open a support call to find out where the problem was.

## Installing / upgrading

**The plugins.** Builds attach to each release, one per platform, built against
OBS **32.2.2**. A different major version may not load them.

- **Windows** — unzip and copy `obs-plugins` and `data` into the OBS install
  directory (typically `C:\Program Files\obs-studio\`), merging with what is
  there.
- **macOS** (Apple Silicon) — move `obs-multisite.plugin` into
  `~/Library/Application Support/obs-studio/plugins/`, then clear the
  quarantine flag before restarting OBS (these builds are not signed yet):

  ```sh
  xattr -dr com.apple.quarantine ~/Library/Application\ Support/obs-studio/plugins/obs-multisite.plugin
  ```

- **Linux** — place `obs-multisite.so` in the OBS plugin directory
  (commonly `~/.config/obs-studio/plugins/obs-multisite/bin/64bit/`) with the
  contents of `data/` alongside.

Restart OBS. The encoder appears as an output and the decoder as a source, with
**Multisite Encoder** and **Multisite Decoder** docks under View → Docks.

You need an S3-compatible bucket and a key that can read and write it. For the
decoder's event list the key also needs `s3:ListBucket` — Cloudflare's "Object
Read & Write" token includes it, an object-scoped token does not, and the dock
will say so rather than showing an empty list.

**The campus player** installs and updates with one command on stock Raspberry
Pi OS Lite (64-bit):

```bash
curl -fsSL --retry 5 https://raw.githubusercontent.com/stageaudioworks/obs-multisite/main/scripts/player/install.sh | sudo bash
```

Updating is the same command again; the settings and the segment cache are
kept.

## Known gaps

- **Not yet used for a real event.** The soak covered sustained upload,
  timeslipping and playout. It did not cover a room full of people, a volunteer
  under pressure, or a venue's network on a Sunday.
- **The campus player has not carried an event either**, though it now holds
  30 fps on a Pi 5 through several runs, with a handful of dropped frames.
- **Alignment between separate audio tracks is unverified.** Audio stays locked
  to the picture — measured, and checked by ear — but nobody has confirmed that
  a click on one track lands at the same instant as the programme on another.
- **Routing packed channels to separate outputs is out of scope**, not
  pending. In OBS,
  [atkAudio's plugin suite](https://github.com/atkAudio/PluginForObsRelease)
  does it — and more — better than a de-interleaver of ours would have; it is a
  separate install under AGPL-3.0. On the appliance, one chosen track is played
  when fed multi-track, and packed channels go out of HDMI in order.
- **The relay has pushed live streams to YouTube** but has not been through a
  full event.
- **HEVC to a streaming site needs SRT.** RTMP cannot carry it and re-encoding
  is not built, so YouTube and Facebook remain H.264 only. The HEVC-over-SRT
  remux is verified against ffmpeg, but no event has yet been carried from a
  real HEVC encoder through the relay. Rehearse it before relying on it.
- **AV1 is refused by the relay on both protocols**, deliberately: too little
  of what would receive it can decode it yet.
- **The relay does not terminate TLS.** It binds to localhost and expects a
  proxy in front of it; a working Caddy config is included.
- **Replaying a past event is a proof of concept** — one at a time, started
  by hand, with no scheduling.
- **AV1 is carried but lightly exercised**; seeking is accurate to about a
  second, not to a frame.
- **The appliance is still missing DeckLink SDI output** and Pi 4
  hardware-decoder selection.

## A note on how this release was made

v0.1.6-alpha was produced under a different authoring workflow — VS Code with
Deepseek V4-Flash/Pro — and its memory leak is what this release exists to
fix, reviewed and confirmed back under Claude Code. Whichever tool drafts a
given release, the code, tests and documentation remain human-checked.

A good bug report is a real contribution — much of what works well here was
fixed because someone took the time to paste a log.
