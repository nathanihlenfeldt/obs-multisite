# Operator guide

The main campus publishes once; every satellite pulls the same files back down
and plays them out. The twenty-minute version of everything below is
[QUICKSTART.md](../QUICKSTART.md).

## Using it

**In a hurry?** [QUICKSTART.md](../QUICKSTART.md) is the short version of
everything below.

### Installing the plugin

Builds are attached to each [release](https://github.com/stageaudioworks/obs-multisite/releases),
one per platform. All of them are built against the OBS version named in the
release notes; a different major version of OBS may refuse to load them.

**macOS** (Apple Silicon) — unzip, move `obs-multisite.plugin` into
`~/Library/Application Support/obs-studio/plugins/`, then clear the download
quarantine flag before restarting OBS:

```sh
xattr -dr com.apple.quarantine ~/Library/Application\ Support/obs-studio/plugins/obs-multisite.plugin
```

That step is required because these builds are **not code-signed or
notarised**, and macOS refuses to load a quarantined unsigned bundle. What you
see if you skip it is nothing at all: OBS starts normally with no Multisite
source, output or docks, and its log does not say why. Signing is deferred
until there is a stable version to sign.

**Windows** — copy the `obs-plugins` and `data` folders into your OBS Studio
install directory (typically `C:\Program Files\obs-studio\`), merging with
what is there.

**Linux** — place `obs-multisite.so` in
`~/.config/obs-studio/plugins/obs-multisite/bin/64bit/` with the contents of
`data/` alongside. Links the system FFmpeg and libcurl.

Restart OBS. The encoder appears as an output and the decoder as a source,
with **Multisite Encoder** and **Multisite Decoder** docks under View → Docks.

You need an S3-compatible bucket and a key that can read and write it. For the
decoder's event list the key also needs `s3:ListBucket` — Cloudflare's "Object
Read & Write" token includes it, an object-scoped token does not, and the dock
says so rather than showing an empty list.

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

**Wanting the storage itself self-hosted, not only self-configured?** Every
provider above still means somebody else's servers. If that matters to you —
data residency, a church that already runs its own infrastructure, or simply
keeping a third party out of the chain entirely — the bucket only needs to
speak the S3 API, so a self-hosted object store works exactly like MinIO
does above. [Alarik](https://github.com/achtungsoftware/alarik) is one such
project: S3-compatible with lifecycle rules included, and it runs on hardware
you own — on premises, at a colo, wherever. It is a separate project, in
beta, and one we have not run this pipeline against ourselves; nothing here
depends on it, the same as any other storage provider on this page.

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
   how much is waiting, and the **Internet** line (green/amber/red). That line
   is live even before you go on air — the dock checks the bucket every few
   seconds — so a broken connection is visible before it costs you a service.

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

   The decoder holds playback until a whole minute of the service is buffered
   (set in **Settings… → Start after this much is ready**). The buffer fills
   first, then the picture starts — so it does not chase the live edge and
   stall after a single piece on a slow or uneven connection.

The **Internet** line in the Status box tells you whether the box can reach the
bucket, separately from whether anything is on air. If it flips to red
("no connection") mid-service, the **Could broadcast for** figure is how long
the picture will keep going from what is already downloaded — enough notice to
act, rather than a surprise when the picture freezes.

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
