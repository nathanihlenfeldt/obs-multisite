# Simulcast relay

Sends a service that is already being distributed to your campuses out to
YouTube, Facebook, or any other streaming site, from a small server you run
yourself.

It reads the same segments the campuses read, so the main site uploads once
however many places the service goes. On a venue connection that will not
carry a second upload, that is the whole point. It also means the public
stream inherits the buffering the campus feed already has: a wobble at the
main site delays the public stream rather than breaking it.

> **Alpha.** Like the rest of this project, this has not yet carried a real
> service. Run it with a fallback and someone technical on hand.

---

## What it does, and what it does not

**Does**

- Reads a room's segments from your bucket and pushes them to one or more
  RTMP destinations.
- Sends one chosen audio track per destination, picked by the name the main
  site published — "Main Mix", "Sermon ISO".
- Sits a configurable time behind the service (three minutes by default), so
  a dropout at the main site is absorbed instead of reaching the public.
- Supervises each stream: if the connection dies it reconnects by itself and
  resumes from where it stopped, without skipping anything.
- Ends each stream deliberately when the service ends, rather than letting it
  time out.

**Does not, yet**

- **Re-encode.** Everything is a straight copy, which is why this runs on a
  tiny server. If the main site records in HEVC, streaming sites cannot take
  it over RTMP, and the relay refuses to send it rather than pushing something
  that looks fine here and is dead at the far end. Set the main site's encoder
  to H.264 for services you want to stream publicly.
- **Split up packed multi-channel audio.** If the main site sends its sound as
  one multi-channel track with the mix, the microphones and the click inside
  it, the relay refuses rather than guessing which channels are the programme.
  Send separate audio tracks instead. (See `PROJECT-SCOPE.md` §4.3.)
- **Sign in to YouTube.** You paste a stream key. Creating the broadcast still
  happens in YouTube's own page.
- **Start by itself** at a scheduled time or when the encoder goes live.

---

## Before anything else: it has a login, and it needs one

This service decides where your services are sent. Anyone who can reach it can
point your stream at their own server, take it off air mid-sermon, or read your
storage settings. So:

- **The published port is bound to the host's localhost.** Publishing it to
  the internet is a decision you make, not something that happens by running
  it. That control lives in the `ports:` line of `docker-compose.yml` —
  `127.0.0.1:8080:8080` — not inside the container, where the process listens
  on all interfaces because a container's own loopback is unreachable from
  anywhere, port mapping included.
- **Every part of it requires a login** except the sign-in page itself. The
  first person to open an unclaimed relay sets the username and password, so
  set yours before anyone else can reach it — or pass `RELAY_USER` and
  `RELAY_PASSWORD` and it is claimed before it ever starts.
- **Put HTTPS in front of it.** A password sent over plain HTTP crosses the
  network readable, and so does everything else. `Caddyfile.example` in this
  directory is a complete working config; Caddy gets and renews the
  certificate by itself. If you would rather not expose it at all, leave it on
  localhost and reach it over an SSH tunnel:

  ```bash
  ssh -L 8080:localhost:8080 you@your-server
  ```

  then open `http://localhost:8080`.

Passwords are stored as PBKDF2-HMAC-SHA256 over a random salt, so the database
is not a list of passwords. Sessions live in memory, so restarting signs
everyone out.

---

## Running it

You need a server with a public internet connection and enough upload
bandwidth for every stream you intend to send at once. A $5/month VPS is the
target: one 6 Mbps stream is well within a box that size, because nothing is
being re-encoded.

```bash
docker run -d --name multisite-relay \
  -p 8080:8080 \
  -v multisite-relay-data:/data \
  -e RELAY_ROOM=main-auditorium \
  ghcr.io/stageaudioworks/multisite-relay:latest
```

Then open `http://your-server:8080`, put the bucket details on the Settings
page, and add a destination.

Everything except the port can be set in the browser. The environment
variables below exist so an integrator can hand over a box that is already
pointing at the right bucket. They seed the settings **on first run only** —
after that what is in the database wins, so a value corrected in the browser
is not overwritten by a stale variable on the next restart.

| Variable | What it is |
|---|---|
| `RELAY_ROOM` | The feed name, matching the main site. |
| `RELAY_BUCKET` | Bucket name. |
| `RELAY_R2_ACCOUNT_ID` | For Cloudflare R2. |
| `RELAY_ENDPOINT_HOST` | For anything that is not R2, e.g. `s3.eu-west-1.amazonaws.com`. |
| `RELAY_ACCESS_KEY_ID` | Read-only credentials are enough and are what you should use. |
| `RELAY_SECRET_ACCESS_KEY` | |
| `RELAY_REGION` | `auto` for R2, a real region for AWS. |
| `RELAY_USE_HTTPS` | `0` only for storage on your own network with no certificate. |
| `RELAY_PORT` | Defaults to 8080. |
| `RELAY_BIND` | Which interface the process listens on. The image sets `0.0.0.0`, which is correct: inside a container that is not what decides exposure — the host side of the port mapping is. Running outside a container it defaults to `127.0.0.1`. |
| `RELAY_USER` / `RELAY_PASSWORD` | Claims the relay on first run, so the login is set before anyone can reach it. Ignored once a login exists. |
| `RELAY_DATA_DIR` | Defaults to `/data`. |

`docker-compose.yml` in this directory does the same thing if you prefer.

**Mount `/data`.** It holds the database (your destinations and their stream
keys) and the segment cache. Without it, replacing the container loses your
setup.

### What it needs from your bucket

Read access only — `GetObject` on the room and event prefixes. The relay never
writes anything. Give it its own read-only credentials rather than reusing the
encoder's.

### How much bandwidth and disk

Roughly, for one destination at the main site's own bitrate:

| | Per hour | A two-hour service |
|---|---|---|
| Down from the bucket | bitrate × 1 | 6 Mbps → about 2.7 GB → 5.4 GB |
| Up to the destination | bitrate × 1 per destination | 6 Mbps → about 2.7 GB → 5.4 GB |

Four services a month to two destinations is roughly 43 GB out and 22 GB in —
comfortable on any VPS transfer allowance. The download happens **once** no
matter how many destinations you add; only the upload multiplies.

Disk: the cache holds the delay window plus room to run ahead, so at 6 Mbps
with a three-minute delay expect a few hundred megabytes. A 10 GB volume is
generous.

---

## Past services

Everything the room has recorded is listed under **Past services**, with the
same states a campus sees: on air now, finished, or cut short by an encoder
that died. A service still going out can be neither downloaded nor replayed —
it has no end yet.

**Download.** A finished service downloads as one MP4, streamed straight from
storage as you download it. Nothing is assembled on the server first, so a
two-hour service costs no disk and no CPU, and several people can download at
once without the relay noticing.

The file carries **every audio track the main site sent**, not just the one
being streamed — so the mic ISOs and the click are there for whoever edits it
afterwards. It is a fragmented MP4, which plays in VLC, in browsers, and in
DaVinci Resolve and Premiere. Some older tools want a classic MP4 index; if
yours does, `ffmpeg -i service.mp4 -c copy -movflags +faststart out.mp4` will
convert it without re-encoding.

**Replay (proof of concept).** A finished service can be played out to a
destination as though it were happening now — for a second congregation in a
different time zone, or an evening repeat. It plays at normal speed from the
beginning and ends by itself at the end of the recording.

This is a first version and is honest about it: one replay at a time, started
by hand, to a destination that is not currently carrying the live service. What
it does not do yet is schedule itself, loop, start part-way in, or run several
at once.

---

## Choosing the delay

The delay is how far behind the actual service the public stream runs. It is
the single most useful setting here.

The relay holds that much of the service in hand. If the main site's internet
drops for less than the delay, the public stream never notices — it keeps
playing out of what has already been banked, and the gap is absorbed. If the
outage outlasts the delay, the stream stops and reconnects when the service
comes back, which splits the recording on the destination into two parts.

- **Three minutes** (the default) covers the outages that actually happen on a
  venue connection.
- **Shorter** if somebody in the building is also watching the public stream
  and the offset would be confusing.
- **Longer** on a connection you do not trust. Ten minutes is not unreasonable
  and costs nothing except being ten minutes behind.

A short interruption — under about forty-five seconds — is ridden out without
even dropping the connection, and the service resumes exactly where it left
off with nothing missing.

---

## When something goes wrong

Everything the relay knows is on the page, including the reason a stream is
not running. The Log tab has the detail.

| What you see | What it means |
|---|---|
| **Waiting for the service to start** | Nothing is on air at the main site, or the opening data is still downloading. Normal before a service. |
| **Nothing coming from the main site** | The main site has stopped sending for more than a few seconds. The connection is held open for forty-five seconds before giving up. |
| **Reconnecting** | The connection dropped. It retries by itself, backing off up to fifteen seconds between attempts. |
| **Cannot send this service** | Something about the feed means it must not be sent — usually the wrong video format, or a sound feed that no longer exists. The message says which. |
| **Finishing off** | The service ended; the last of it is still going out. |

Stream keys are never shown in the interface or written to the log, and the
database file is not readable by other users on the machine. They are stored
as given rather than encrypted: encrypting them beside the key that decrypts
them would protect nobody, and pretending otherwise is worse than saying so.
Treat the `/data` volume as a secret.

---

## Building it without Docker

```bash
cmake -B build -DMULTISITE_BUILD_RELAY=ON -DBUILD_PLAYER=OFF
cmake --build build --target multisite-relay
```

Needs libcurl, OpenSSL and SQLite development packages, plus the `ffmpeg`
command at runtime. The relay is off by default in the main build: it is a
server-side service, and a plugin build should not be made to find SQLite for
something it does not use.

Run the tests with `ctest -R "stream_plan|relay_state|config_store"`. CI also
builds the image and drives the running container — that it answers on its
published port, that the interface loads, and that nothing behind the login is
reachable without one. Every relay fault that has reached a user so far got
through by not being run rather than by failing to compile, so that job exists
to run it. They cover what can be
proved without a destination: which feeds may be sent and which must be
refused, the audio-track mapping, and the whole lifecycle including the stall
grace period and reconnection. What they deliberately do not cover is ffmpeg
itself — that is what a real soak test is for, and no amount of mocking a
subprocess substitutes for one.

---

## How it works

One process. One downloader per room, feeding one ffmpeg per destination.

```
  bucket  ──▶  downloader  ──▶  verified cache  ──┬──▶ ffmpeg ──▶ YouTube
             (one per room)                       └──▶ ffmpeg ──▶ Facebook
```

The downloader is the same code a campus decoder runs — event discovery, the
cache, checksum verification, and the live/ended/interrupted classification —
so the relay and a campus can never disagree about whether a service is still
running.

Each destination has its own ffmpeg child, fed one fragment at a time through
a pipe on a schedule. That schedule is what paces the stream: ffmpeg sends as
fast as it is fed, so feeding it one fragment per fragment-duration holds it
at real time without any pacing flag. It is also how a stall is detected —
ffmpeg given a pipe that goes quiet will block silently and hold the socket
open indefinitely without reporting anything, so nothing downstream can be
relied on to notice.
