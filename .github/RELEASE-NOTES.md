## ⚠️ Alpha — read this first

This is pre-release software. It has been exercised end to end between two
machines and against real Cloudflare R2, but **it has not yet carried a live
service**. Interfaces, settings and the storage protocol may change without a
migration path. There is no warranty, no support contract and no uptime
guarantee.

If you put this in front of a congregation, do it with a tested fallback in
place, a technical person on hand, and the assumption that any given service may
have to go ahead without it. A successful rehearsal is necessary, not
sufficient.

## What this is

Two OBS Studio plugins in one module. An **encoder** at the main site publishes
the service as CMAF segments to an S3-compatible bucket you control; a
**decoder** at each satellite receives, buffers deeply, and plays it out with
per-campus timeslipping. No central server, no database, no vendor — the bucket
is a dumb file store and all the intelligence sits at the edges.

Reliability is the design priority and latency is deliberately last. Segments
are written to disk before they are sent, resent until the store confirms them,
and buffered for minutes at the far end. A satellite that is a minute behind but
never drops is worth more than one that is two seconds behind and stutters.

See the [README](../blob/main/README.md) for the full picture, including what
this is **not** — it is not a managed service, and it is not low-latency or
two-way.

## In this release

- Durable store-and-forward upload: nothing is lost through an outage, a crash
  or a mid-service restart.
- CMAF segments with multi-channel production audio; H.264 or HEVC from any OBS
  encoder (x264, NVENC, QuickSync, AMF).
- Satellite receive with a deep local buffer, checksum verification, and
  timeslipping — hold, resume, catch up, scrub, jump to a marker.
- Event browsing: list what a room has recorded, see which is on air, which are
  finished recordings and which were cut off by an encoder that died, and play
  any of them back.
- Finished and interrupted events play as video-on-demand from the beginning.
- Operator docks in plain language, plus hotkeys.

## Installing

**Windows** — unzip `obs-multisite-*-windows-x64.zip` and copy the
`obs-plugins` and `data` folders into your OBS Studio install directory
(typically `C:\Program Files\obs-studio\`), merging with what is already there.
Built against OBS **32.2.2**; a different major version may not load it.

**Linux** — extract `obs-multisite-*-linux-x86_64.tar.gz` and place
`obs-multisite.so` in your OBS plugin directory (commonly
`~/.config/obs-studio/plugins/obs-multisite/bin/64bit/`), with the contents of
`data/` alongside. The Linux build links the system FFmpeg and libcurl.

Then restart OBS. The encoder appears as an output and the decoder as a source,
with **Multisite Encoder** and **Multisite Decoder** docks under View → Docks.

You will need an S3-compatible bucket and a key that can read and write it. For
the decoder's event list the key also needs `s3:ListBucket` — Cloudflare's
"Object Read & Write" token includes it, but an object-scoped token does not.

## Known gaps

- **No full-length soak test.** Sustained behaviour across a real service is the
  highest-value thing that is not yet code.
- **The satellite appliance** (Raspberry Pi) is built and installable but has
  not run a service either.
- **AV1 is carried but lightly exercised**, unlike H.264 and HEVC.
- **Seeking is accurate to about a second**, not to a frame.

A good bug report is a real contribution — much of what works well here was
fixed because someone took the time to paste a log.
