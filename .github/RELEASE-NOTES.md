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

## What's new in v0.1.13-alpha

Two things an operator does constantly — scrubbing to a moment, and reading how
far behind the main site they are — now do what they say. Both were wrong in
ways that are easy to work around once you know, and hard to trust around if
you don't.

### Scrubbing to a time goes there, and the clock agrees

Choosing a time on the timeline used to start playing immediately from the
position you had just left, run on for several seconds, jump somewhere else,
and settle showing a time that was not the one you picked — on a long recording,
minutes out. The picture and the readout disagreed, so neither could be
trusted for lining up a cue.

Underneath, the seek itself had been picking the right segment all along. What
went wrong was everything built on top of it. A decoder holds several seconds
of already-decoded pictures, and clearing the queue on a seek did not stop it
handing those over — so the position you had left kept playing, and worse, the
first of those frames defined the clock that every displayed time was then
measured from. Because that mapping is learned once and kept, a single frame
from the wrong place put the whole readout out for as long as playback
continued.

A frame now carries which timeline it belongs to, and anything from a position
already left is discarded rather than believed. The clock is fixed to the
fragment the seek landed on, which is what makes the reported time the
requested one: measured against a live event, asking for 14:40:43.768 now
reports 14:40:43.769.

Seeking within a segment is also honest now. Segments are six seconds long and
landing part-way into one means skipping the frames before your moment; the
readout used to describe the start of the segment rather than where it had
actually landed, up to six seconds early.

### "Behind live" holds still

The delay readout swung by six seconds while nothing about the delay had
changed. It was counting whole segments on both sides, and each side steps
independently as segments publish and playback advances.

It is now the gap between two real times, so setting a two-minute delay reads
as two minutes and stays there instead of flicking between 1:54 and 2:00. The
main site's content advances continuously; we simply learn about it in six
second pieces, so the live edge is carried forward between updates rather than
waiting for the next one.

### Smaller things in the decode dock

The playhead is redrawn between state updates instead of stepping twice a
second, so the scrub bar moves the way one should.

The header is now two readings rather than one: what this box is doing —
**PLAYING**, **HELD**, **STOPPED**, **READY** — and, separately, what the main
site is doing. A single chip could only ever show one of them, so playing a
finished recording read **BROADCAST ENDED** with nothing to say it was playing,
and **Hold** on a finished recording showed no indication at all.

### For anyone working on the decoder

The rules governing what a decoded frame may do, and in what order, are now one
tested component (`src/core/playout_timeline.h`) that the delivery loop calls,
rather than several blocks whose order mattered and was not written down.
Seeking broke five times in one afternoon getting here, every one of them found
by an operator rather than the suite, because nothing tested that path;
`tests/test_playout_timeline.cpp` fails on all five.

One known limit, unchanged by this release: on an event whose encoder has been
restarted, a segment's recorded wall-clock time and its position in the media
can disagree by up to a minute. Seeking is unaffected, because the same measure
is used at both ends, but the two are not interchangeable and a few places still
estimate one from the other for segments outside the current manifest window.

## What's new in v0.1.12-alpha

### Holding the picture no longer hands the screen to the box's own address

On a campus player, pressing **Hold picture** could be followed a second or two
later by the identity screen — hostname, IP, room — appearing in place of the
picture the operator had just frozen. On the screen in the room it reads as the
player having died at the exact moment somebody asked it to hold.

The cause was a collision between two things that meant different things by the
same word. Holding the picture was recognised only when the box's own idle mode
was set to *Hold the last picture*, so on a box left on the default idle screen
the poll loop found neither frames arriving nor a reason to leave the picture
alone, and fell through to drawing the identity screen. Everything else was
working: the hold itself, the audio, the cache, and — reassuringly — the
preview, which is rendered from the last decoded frame and so kept showing the
frozen picture the screen no longer had.

A held picture now outranks the idle screen whatever the idle mode is set to.
An idle screen is for having nothing to show: waiting for the main site, or
stopped, and both still put one up as before. **Stop** and waiting are
unchanged *in this respect*; on *hold the last picture* with nothing ever
decoded the identity screen still comes up, because there is no frame to hold
and a blank screen says nothing about which box has come up empty.

The rule is now a function of four booleans rather than a chain of conditions
inside the poll loop, and a new test checks all of its combinations — including
the default one on the identity screen — with no display, no decoder and no
network. That combination could only be reproduced by hand with a live event and
an HDMI socket attached, which is how it reached a congregation in the first
place.

### Stopping, and the header above it, say what they mean

Two changes to the decode dock, both about a control or a readout reporting
something other than what it was doing.

**Stop now releases the picture.** It was already ending playback, but the
source kept polling and kept pulling in flight, so a stopped decoder went on
downloading. Now Stop cancels the requests already in flight, stops the poll
loop issuing new ones, and lets go of the decoder. It deliberately keeps the
cache: Play resumes from disk rather than sitting through the full start-up
buffer again, which is what you want from a button you press and press back.
The header reads **Stopped — nothing downloading**, and Hold is labelled **Hold
picture (keeps recording)** so that its contrast with Stop is on the button.

**The header no longer hides one answer behind another.** It had been a single
label carrying three unrelated facts — what the main site is doing, what this
decoder is doing, and how the link is — behind a precedence order, so only one
could ever be visible. Playing a finished recording therefore read **BROADCAST
ENDED**, which was true of the venue and silent about the playback, and holding
a finished recording showed nothing about the hold at all. There are now two
labels, each answering one question:

- **Playback** — `STOPPED`, `LOADING…`, `BUFFERING…`, `HELD`, `PLAYING`,
  `READY`
- **Source** — `LIVE`, `BROADCAST ENDED`, `INTERRUPTED`, `RECORDING (not live)`,
  `OFFLINE`, `CONNECTING`, `CONNECTION LOST`

Playback sits first because it is what you are acting on. Neither can now
misstate the other, because neither can express the other's facts. `READY` is
new and names what **Load** leaves behind — buffered, not on air, waiting for
**Play** on cue, which previously had no name and read as whatever the room
happened to be doing.

Neither is a setting and nothing moves; the strings are in both locale files,
and the web remote — which already separated the two — needed only the stopped
state to be added to its status.

## What's new in v0.1.11-alpha

### Markers reach a control surface as they happen

The vendor events the plugin pushes carried the transport state but not the
markers, so a cue dropped at the main site reached an obs-websocket client only
on the next poll — up to five seconds later. `markers` and `marker_labels` are
now part of what the state event watches, so a cue reaches a Stream Deck's marker
button as it is dropped rather than when something else happens to change.

Nothing to configure, and nothing changes for a client that does not use it: the
same events, carrying the same document.

This is what the Companion module's generated marker buttons rely on. Every cue
the main site is configured with becomes a button of its own, and a new cue is
offered within about a second.

## What's new in v0.1.10-alpha

### Control it from a Stream Deck

Every command the plugin offers is now also an **obs-websocket vendor request**
under the vendor `obs-multisite`, and the plugin emits **vendor events** when the
state changes. Any obs-websocket client can therefore drive it — a script, an
automation system, or a Stream Deck through Bitfocus Companion.

There is nothing to switch on. obs-websocket ships with OBS 28 and later and is
enabled in Tools → WebSocket Server Settings; with it off, or absent, the plugin
logs one line and everything else carries on exactly as before.

The commands are the ones the plugin's own remote-control pages already use,
under the same names, so the two cannot drift — the list lives in one place and a
test pins it. Go live, End and the marker buttons for a main site; play, stop,
hold, resume, catch up, jog, seek, delay, markers, recordings and return-to-live
for a campus; a status request and events for everything watching.

### A Companion module

There is now a purpose-built Bitfocus Companion module —
[companion-module-obs-multisite](https://github.com/stageaudioworks/companion-module-obs-multisite)
— so those commands arrive as buttons that light up: on air, held, buffering,
behind live, link offline, with variables for the same figures and two preset
banks to drag straight onto a page.

> **Alpha, and not in the Companion store yet.** Until it is listed, Companion
> loads it as a *developer module* — the module's README has the steps. It needs
> **Companion 4.0 or later**, and this release of the plugin.

Tested against a real OBS, but not yet through a whole event.

## What's new in v0.1.9-alpha

For v0.1.8's users: the Manage storage tool arrived in the previous
release; this one is what makes it usable on a full bucket.

### Storage you can actually see and clear

**Manage storage…** in the encoder dock lists a room's events and lets them be
deleted — one, or everything older than a chosen number of days — with a
confirmation and a check afterwards. On a full bucket it used to sit on
"Looking…" for minutes while it walked every object of every event to add up
bytes, one request after another, and it logged nothing at all while it did.

It is two halves now. The events are listed first, from the manifests and the
live pointer, so something appears at once. Each event's size is then measured
beside it, six at a time, each row filling in as it lands — so the wait is the
largest single event rather than the sum of all of them. A size that cannot be
measured is reported as unknown rather than as zero, because "0 B" on an event
holding gigabytes is not cosmetic in a window whose whole purpose is deleting
things. Closing the window cancels the work in flight, and every listing and
sizes pass is logged with its counts and elapsed time.

### The documentation

The storage-management description, the layout notes and the counts in the
guides had drifted from the code; they are corrected. PROJECT-SCOPE.md now
carries a roadmap for the next milestones: storage redundancy, output
routing, headless appliances, ABR and low latency.

## What's new in v0.1.8-alpha

### The encoder and the decoder can be run from a phone

Both halves of the plugin now serve an operator page on the church network: the
same interface the Raspberry Pi appliance has, out of OBS itself. One page,
polled twice a second, in the plain language of an event rather than of a video
pipeline.

- **Sending side** — Go live and End the broadcast, the editable event name, the
  four marker buttons, and the reliability readout an operator watches
  mid-event: confirmed pieces, what is waiting to send, retries, bytes sent, the
  measured upload rate, the Cloudflare edge serving the bucket, and the last
  error.
- **Receiving side** — play, hold picture, catch up to now, jog, stay behind
  live, a timeline that can be clicked, the recordings list, and the readout
  that says how long this campus could keep playing through an outage.
- **Both** — the log, so somebody with a phone and no access to the desk can see
  why nothing is happening, and a Lock, so a tablet left on a music stand cannot
  stop a broadcast by being leant on.

It is the appliance's page on purpose: somebody who has learned one should not
have to learn the other, and the words on it are the words of an event. There is
no password and no TLS, exactly as for the appliance — the building's own network
is the guard — and it is on by default on port **8080**. It can be switched off
or moved to another port in **Settings → Remote control** in either dock, which
also shows the address to type into a phone.

Which pages exist follows the machine's role, as the docks already do: a main
site has no decoder routes at all, and a satellite none of the encoder's.

### One HTTP server, three users

The appliance's small HTTP server moved into the shared core and learned to
speak Winsock, so the plugin, the relay and the player now run one
implementation rather than three. It is covered by a new test
(`tests/test_http_server.cpp`) that speaks real HTTP over loopback, on every
platform CI builds: routing, verbs, the static web root, keep-alive, a handler
that throws, and the refusal of a path that climbs out of the web root.

### The campus player can be reached without a drive to the campus

What makes a wrong setting at a campus so expensive is that fixing it means
somebody driving there. The player now ships with the two optional tools that
remove that drive, and either can be set up from its own web page:

- **ZeroTier** puts the box on a private network that follows it, so it is
  reachable from the office wherever it is plugged in. The network key is given
  during setup — passed as `ZT_NETWORK_ID=…` alongside the installer, or typed
  at its prompt — and can be changed later under Settings → Remote access.
- **cloudflared** publishes the operator page on a public hostname with no
  port-forward and no static address. Give the installer `CF_TUNNEL_TOKEN=…`,
  or paste the token into the same panel.
- **The box's ZeroTier address is printed on its screen**, clearly labelled
  **REMOTE ACCESS IP**, underneath the room's own addresses. Those are a
  different thing — they work only inside the building and are what a phone in
  the room types — and the label exists precisely so the two are not confused.

Both are optional and neither is required to play an event. A box with neither
installed behaves exactly as before, and its screen says nothing about remote
access at all.

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
- **A size in *Manage storage…* can read "unknown".** A measurement that failed
  is reported that way deliberately rather than as `0 B`. Refresh the listing to
  try again, and note that nothing is ever offered for deletion on the strength
  of a size that could not be measured.

## A note on how this release was made

v0.1.6-alpha was produced under a different authoring workflow — VS Code with
Deepseek V4-Flash/Pro — and its memory leak is what this release exists to
fix, reviewed and confirmed back under Claude Code. Whichever tool drafts a
given release, the code, tests and documentation remain human-checked.

A good bug report is a real contribution — much of what works well here was
fixed because someone took the time to paste a log.
