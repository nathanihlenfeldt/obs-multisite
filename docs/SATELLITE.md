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
