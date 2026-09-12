# Streaming to the public

The campuses are not always the only audience. `relay/` is a small self-hosted
event that reads the same segments and pushes them out to YouTube, Facebook,
or any RTMP destination — and over SRT to anywhere that prefers it.

It relays from the bucket rather than adding a second output to OBS, which
matters twice over. The main site uploads once whether the event is going to
two campuses or to two campuses and the internet — often the difference between
possible and not on a venue connection. And the public stream inherits the
buffering the campus feed already has: it runs a few minutes behind on purpose,
so a dropout at the main site delays it rather than breaking it.

```bash
docker run -d --name multisite-relay \
  -p 8080:8080 \
  -v multisite-relay-data:/data \
  -e RELAY_ROOM=main-auditorium \
  ghcr.io/stageaudioworks/multisite-relay:latest
```

`:latest` follows `main` and is rebuilt whenever the relay changes; tagged
releases also get a `vX.Y.Z` image. `relay/` builds the same image locally if
you would rather not pull it.

Then open it in a browser, put in the bucket details, and add a destination.
A $5/month VPS is the target rather than a stretch, because nothing is being
re-encoded.

- **It has a login, and binds to localhost by default.** This event decides
  where your events are sent, so exposing it is a decision rather than a
  default. Put HTTPS in front of it; `relay/Caddyfile.example` (or
  `relay/nginx.conf.example` if you run nginx) is a working
  config.
- **One chosen sound feed per destination**, picked by the name the main site
  gave it — "Main Mix", "Sermon ISO" — never a track number. A future
  "clean feed to Facebook, main mix to YouTube" is just two destinations.
- **A delay you choose**, three minutes by default. This is the setting worth
  understanding: it is how much of the event the relay holds in hand, and so
  how long an outage at the main site can last before the public sees it.
- **It reconnects by itself** and resumes from where it stopped, so nothing is
  skipped. A silence under 45 seconds is ridden out without even dropping the
  connection. It watches both directions: content failing to arrive from the
  main site and content failing to leave for the destination look identical to
  ffmpeg, which reports neither, so the relay notices both itself and says
  which one happened.
- **RTMP or SRT, decided by the address you paste.** There is no protocol
  setting: `rtmp://` and `srt://` are unmistakable, and asking a volunteer
  which one they were given is asking them to get it wrong. SRT can also
  *listen*, for a broadcast partner or hardware decoder that pulls from you
  rather than being pushed to — written down by leaving the host out of the
  address, `srt://:9000`, which is deliberately the only way to ask for one,
  because it opens a port on a machine otherwise kept closed.
- **HEVC goes out over SRT.** RTMP means FLV, and FLV means H.264 — which is
  why choosing HEVC for the campuses used to cost a church its public stream
  outright. SRT means MPEG-TS, which carries HEVC properly, so that trade is
  no longer forced. It still cannot go to YouTube.
- **It refuses rather than guesses.** AV1, HEVC to an RTMP destination, and
  packed multi-channel audio are all declined with a plain explanation,
  because sending any of them onward would mean a stream the destination
  rejects, or a mic ISO going out to the public.

It also does two things with events that have already finished:

- **Download one as an MP4**, streamed straight from storage — nothing is
  assembled on the server, so a two-hour event costs no disk. The file
  carries every audio track the main site sent, not just the streamed one, so
  the ISOs and the click are there for whoever edits it.
- **Replay one to a destination** as though it were happening now, for a
  second congregation or an evening repeat. This is a proof of concept: one at
  a time, started by hand, no scheduling yet.

Full deployment notes, including bandwidth and disk, are in
[relay/README.md](../relay/README.md).
