# Choosing a satellite

A campus can receive in one of two ways, and they suit different rooms.

**OBS on a PC** — the decoder is a *source in a scene*, so the campus can
produce around the relayed event. **The Pi appliance** — a fixed-function box
that plays the event and nothing else. Both are built; neither has yet run a
real event.

### What running the decoder in OBS makes possible

Because the relayed programme is an ordinary source, everything OBS does applies
to it. This is the reason to choose a PC over the appliance, and for many
churches it is the deciding factor.

**Local content over the relayed event**

- Lower thirds, campus announcements, scripture graphics, a countdown before the
  event, a logo bug — keyed over the incoming picture with OBS's normal
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

**More than one camera angle, in guaranteed sync**

The protocol carries exactly one video stream per event — there is no
equivalent of multi-track audio on the video side, and adding one would be a
much bigger change than audio tracks were, which just reuse the fragment
multiplexing that was already there. So a room that needs several angles
delivered to a satellite — a wide shot and a stage-left ISO, say, or four SDI
sources for a video wall — has to get them there some other way.

Our recommendation: **composite the feeds into one canvas at the main site**
before they ever reach the encoder, and split them back apart at the
satellite. Two SDI inputs side by side make a 3840×1080 canvas; four in a
grid make 3840×2160. That single wide frame is the one thing this pipeline
sends and guarantees in sync — every camera is a region of the same decoded
picture, so there is no possibility of the angles drifting apart the way
independently-encoded streams could.

At the decoder machine, add the Multisite Source once and pull the angles
back apart onto their own outputs with OBS's own **Transform** and **Crop/Pad**
filters — one scene item per angle, each cropped to its region of the
composite and routed to wherever it needs to go (a DeckLink output, a
separate program feed, a video wall processor). Nothing here needs a plugin;
crop and transform are built into OBS.

> **Planned.** Pulling the composite apart is intended to become a property of
> the decoder rather than a filter chain built by hand: a tile layout declared at
> the main site would expose each region as its own source, already cropped, so a
> 2×1 or 2×2 feed could be assigned straight to discrete fullscreen or SDI
> outputs. That is Phase 10 in [PROJECT-SCOPE.md](../PROJECT-SCOPE.md#10-delivery-phases).
> Until it exists, the filters below are how to do it.

The cost is bandwidth: a 3840×2160 canvas costs roughly what a single 4K
stream does, whatever number of cameras are inside it. Worth it for a room
that needs several angles kept in lock-step; not worth building for a room
that only ever needs one.

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
named above is someone else's project, on its own release schedule.

If the main site sends *packed* multi-channel rather than separate tracks, the
channels arrive as one stream and something has to route them to their
destinations. That is not built here, deliberately — it is a solved problem in
OBS. [atkAudio's plugin suite](https://github.com/atkAudio/PluginForObsRelease)
hosts VST3/AU/LV2 plugins, mixes OBS sources, and routes audio to ASIO,
CoreAudio and Windows Audio devices, which covers channel mapping better than a
narrow de-interleaver of our own would. It is a separate install under the
AGPL-3.0 licence and nothing here depends on it; a packed feed carries eight
channels through this pipeline with the channel order intact either way.

### Any location can be the origin

Both plugins are one module, so any machine running OBS can take either role.
What originates an event is a laptop with OBS on it, so a broadcast can start
anywhere someone can run it:

- a guest speaker or travelling pastor, publishing from wherever they are;
- a conference or camp venue, for a week, and then never again;
- a second campus hosting this week's combined event, with the usual main
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
event, it does not hold a conversation between sites.

Keep rooms separate — a guest publishes to `guest-speaker`, not to
`main-auditorium` — so an occasional broadcast can never be mistaken for the
main programme.

### When the appliance is the better answer

The appliance gives all of that up on purpose. No scene, no overlays, no local
sources: it plays the relayed event, on a box that costs less than a monitor,
boots into the event on power-up, and is driven from a phone with no desktop
to leave in the wrong state.

Choose it where a campus needs the event on a screen and nothing more — an
overflow room, a chapel, a plant meeting in a school hall. Choose OBS where the
campus produces around the relay, or where it has to reach existing SDI, NDI or
Dante infrastructure.

---

## The campus player (satellite appliance)

The alternative to running the decoder in OBS: a small box at a campus that
receives, decodes and plays out, with no operator-facing desktop software. See
[Choosing a satellite](#choosing-a-satellite) for which suits a given room. On
stock **Raspberry Pi OS Lite (64-bit)**:

```bash
curl -fsSL --retry 5 https://raw.githubusercontent.com/stageaudioworks/obs-multisite/main/scripts/player/install.sh | sudo bash
```

That installs the dependencies, builds the player, installs it as an event
that starts on power-up, and puts a screen up on the HDMI output showing the
box's own address and a QR code of it: point a phone at the screen and the
control page opens, nothing to type. Everything else is done from a phone or
tablet on the same network — storage credentials, which room to follow, the
output resolution and frame rate, the sound device, the clock, and the
transport controls during an event.

- **It owns the display.** The player sets the KMS mode itself, so the output
  resolution and frame rate are exactly what was asked for and there is no
  desktop to be left in the wrong state. Pi OS Lite is the right image.
- **Production audio over HDMI.** Up to eight channels of LPCM, recovered at
  the campus with a de-embedder. If the device will not take every channel the
  feed carries, it says so loudly rather than silently dropping the click. This
  is the one place *packed* multi-channel is the better mode: eight channels in
  one stream map straight onto HDMI's eight, in order, with nothing to route.
  An appliance fed multi-track plays one chosen track — the first by default,
  with a picker in Settings; combining several tracks onto output channels
  there is not built, and packed is the answer for a campus that needs more
  than one.
- **Hold beats the idle screen.** `idle_mode` says what the box shows when
  there is nothing to play — the box's own details, black, or a holding slide.
  Pressing **Hold picture** is not that: it is an operator asking for the frame
  in front of them to stay, so the held frame stays up whatever `idle_mode` is
  set to, and changing the idle screen mid-event cannot replace a picture that
  was deliberately frozen. **Stop** and waiting for the main site are the
  deliberate acts the idle screen is for, and both still show it. On
  `idle_mode: "hold"` with nothing ever decoded there is no frame to hold, so
  the identity screen comes up rather than a blank nobody can explain.
- **The preview is a copy, not a second output.** The web UI shows the picture
  going out, refreshed at a rate the browser chooses. Watching it does not
  change or interrupt what is on the screen in the room — it is the same moment,
  sampled a few times a second. There is one playhead, so the preview cannot
  look ahead of the picture it mirrors.
- **The cache belongs on a USB SSD.** It writes roughly 3 GB an hour, which
  will wear an SD card out. The installer looks for a USB drive and uses it;
  if there is none, both it and the interface say so.
- **Remote access, so the box does not need a visit.** What makes a wrong
  setting at a campus expensive is that fixing it means driving there. The
  installer brings up two optional tools and either can be changed later from
  Settings → Remote access. **ZeroTier** puts the box on a private network that
  follows it, so it is reachable from the office wherever it is plugged in:
  pass `ZT_NETWORK_ID=…` to the installer or type it at its prompt. **cloudflared**
  publishes this control page on a public hostname with no port-forward and no
  static address: pass `CF_TUNNEL_TOKEN=…`. The box's ZeroTier address is put on
  its own screen, labelled **REMOTE ACCESS IP** and kept well apart from the
  in-room addresses — those are typed into a phone standing in the building,
  this one is not, and confusing the two is the mistake worth designing out.
  Neither tool is required to play an event; a box with neither says nothing
  about remote access and behaves exactly as before.

Run it by hand while setting one up:

```bash
sudo multisite-player --config /etc/multisite-player/config.json --verbose
```

`journalctl -u multisite-player -f` is the whole diagnostic story; the last few
hundred lines are also in the interface, under Log, for an operator with a
phone and no SSH.

### AES67 audio on the network

Out of the box the sound leaves on the HDMI socket with the picture, so it
reaches whatever is plugged into the Pi and nothing else. A campus that wants
the sound on its own console — a separate feed, its own level control, working
whether or not a screen is attached — needs it on the network, and on a church
network that means AES67. `scripts/player/aes67.sh` installs Digisynthetic's
virtual sound card so the player's audio arrives as an AES67 stream instead of
staying inside the picture. The vendor's package has to be on the box first,
and their licence with it:

```bash
# put linux-vsc-aarch64-1.0.1-20260529.zip and the licence on the box, then:
sudo bash scripts/player/aes67.sh \
    --package /home/pi/linux-vsc-aarch64-1.0.1-20260529.zip \
    --license-file /home/pi/license.dat
```

Their package is served from a host in China, which is sometimes reachable from
a campus and sometimes not, so `--package` — a zip copied across on a stick or
by `scp` — is the reliable path; `--url` (or `VSC_URL=https://…`) downloads it
instead when the box can reach it. On a box that has never seen this repository,
fetch the script the way the player installer is fetched:

```bash
VSC_URL=https://…/linux-vsc-aarch64-1.0.1-20260529.zip \
AES67_LICENSE_FILE=/home/pi/license.dat \
  curl -fsSL https://raw.githubusercontent.com/stageaudioworks/obs-multisite/main/scripts/player/aes67.sh \
  | sudo -E bash
```

Add `--with-player` to install the player in the same run, on a box that has
neither yet. Every option that takes a value can also be passed as `VAR=value`,
so the whole thing can be run without a terminal.

- **One run does all of it.** The script builds their kernel module, registers
  it with DKMS so a kernel update rebuilds it, installs their `DigiAes67Proc`
  daemon under systemd, stores the licence, and points the player at the new
  card. It is safe to run again: every step checks before it acts, and an
  existing player configuration is edited rather than replaced. It writes a
  report of everything it could check to
  `/var/tmp/multisite-aes67-<timestamp>/report.txt`, and says out loud which
  checks it could not make.
- **The player converts to what the card takes.** The card accepts only S32_LE
  samples while the decoder hands out floating point, so the player asks for
  float first, then 32-bit, then 16-bit integers, and converts when it has to.
  Choosing the `hw:` entry from the device menu in the interface used to end
  with `will not take floating-point audio: Invalid argument` and no sound,
  because `hw:` has no plugin to convert for it; that now opens and plays. The
  installer still writes `plughw:CARD=…`, which continues to work and does the
  same conversion one layer down. The driver gives the card no id, so ALSA
  truncates its name to fifteen characters (`Digisyn_vSndCar`); the script reads
  the real name back from `/proc/asound/cards` rather than guessing.
- **The player keeps a queue of its own in front of the card, and that is why
  the 8 ms buffer no longer matters.** The vendor driver fixes the card's buffer
  at one millisecond per period and only as many periods as `bufMs` — eight, by
  default — so the card itself holds 8 ms while a single decoded frame is about
  21 ms. Asking the card for a bigger buffer does not help: the driver clamps it,
  silently, and the player now says so as it opens:
  `gave a 8 ms buffer, not the 500 ms asked for`. So the player buffers the audio
  itself and feeds the card a period at a time from its own thread, which also
  takes the blocking write off the thread that presents the picture. The depth is
  `audio_buffer_ms` in `/etc/multisite-player/config.json` (60 ms by default) and
  is the one number to change if sound still gaps — deeper buys continuity and
  costs lip sync, since nothing yet compensates for the delay it adds. It is only
  used when the card's buffer is smaller than a frame, so HDMI installs are
  untouched. Written up as
  [BUGS.md point 5](../BUGS.md#3-aes67-audio-works-on-the-bench-unproven-over-an-event).
  If you still see "sound has broken up" lines after this, it is *not* the power
  supply, the SD card or the network, though the message suggests all three.
- **The order of two processes decides whether there is sound at all.** The card
  has no rate and no channel count of its own; both are read out of a page of
  shared memory that the *daemon* fills in. A player that starts first opens a
  card advertising zero channels at zero hertz, refuses it, and carries on
  running **silently**. A systemd drop-in starts the daemon first and orders the
  player after it. Do not remove it.
- **The daemon's settings are written the way their own binary writes them.**
  The script fills in the sample rate, channel count, buffer, PTP domain and
  interface in the format the daemon itself uses, then checks the result with
  `--status` rather than assuming it took. To change one later, use
  `sudo /usr/local/bin/DigiAes67Proc --setup`, and keep any change of rate or
  channel count in step with `/etc/multisite-player/config.json`.
- **A kernel update rebuilds the driver.** The vendor's own instructions make
  that a hand step, which on an unattended box in a church means the sound
  quietly disappears one Tuesday and nothing on the screen explains why. DKMS is
  what prevents that; `--no-dkms` turns it off for a box where a manual rebuild
  is preferred.
- **The far end still has to be told what to listen for.** This is the one to
  settle before a site goes live. Their Pi-side configuration flow asks for a
  rate, a channel count, a buffer, a PTP domain and an interface, and never asks
  for a multicast address, a port or a channel map — those live behind their
  separate route tool, a PC-side program, and land in
  `/etc/DigiAes67Proc/_route`. So a Pi can install perfectly and still transmit
  a stream no receiver has been told about, which from the other end of the
  building looks exactly like a broken driver. Confirm how the destination is
  specified before trusting an install that only reports the card appeared.
- **What is verified, and what is not.** On a bench Pi the module built, the
  daemon came up, the card appeared, the player opened it, and eight channels of
  audio arrived — continuously broken up, for the buffer reason above, which the
  player's own queue now works around. Not yet verified: that the picture and the
  sound stay together across a two-hour service. Timing is done entirely by
  scheduling each frame for a moment and holding it until then; nothing reads how
  far behind the card actually is, so the sound sits later than the picture by
  however much audio is queued, and ten seconds of test tone cannot settle
  whether that is acceptable — a queue that empties every frame was not a fair test
  of it either. Also unverified: how accurate PTP becomes, since a Pi's network
  interface does no hardware timestamping, so it is whatever the software
  manages. Measure that at the receiver, not on the Pi. The detail is in
  [BUGS.md entry 3](../BUGS.md#3-aes67-audio-works-on-the-bench-unproven-over-an-event).

## Remote control from a phone

The decoder in OBS serves the same page the appliance does — play, hold, catch
up, jog, stay behind live, the recordings list, and the readout that says how
long this campus could keep playing through an outage. It is configured in
**Settings → Remote control** in the dock, which shows the address to type into
a phone, and is on by default on port **8080**.

The page reaches the same controls the hotkeys use, so a page and a keypress
cannot disagree about what they did. **Lock** in the top bar refuses anything
that would change what is on air, for the tablet left on a music stand; the
dock's own Lock is shown as well, because "why will this not respond" has two
different answers.

There is no password and no TLS — the building's network is the guard, exactly
as for the appliance's page. Switch it off in the same group if that is not the
trust you want, and allow it through the Windows firewall on private networks
the first time, or nothing else on the LAN will reach it.
