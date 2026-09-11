# Bugs and short-term build items

Working list, meant to survive a change of machine or a change of agent —
each entry has enough context to act on without anyone having been in the
room when it was written. Delete an entry once it's fixed and released;
this file is not a changelog.

Last updated: 2026-09-11.

---

## Open

### 0. Pi player: playback can stall indefinitely while downloads keep succeeding

**Status: root cause not found. Needs a live thread-dump on next repro.**

Seen on `rpi5-nathan` running `multisite player 0.1.7`, following room
`main-auditorium`. The player's own status line (`src/appliance/player.cpp`,
logged every 60s) showed, for an 11-minute stretch:

```
20:37:12  playing head=16 live=81 behind=388s buffered=245s cached=26 downloaded=160 frames_out=23580 fps=0.0 dropped=3
20:39:04  playing head=16 live=81 behind=388s buffered=245s cached=31 downloaded=165 frames_out=23580 fps=0.0 dropped=3
...
20:48:12  playing head=16 live=81 behind=388s buffered=346s cached=65 downloaded=199 frames_out=23580 fps=0.0 dropped=3
```

`head` and `frames_out` are frozen the entire time; `cached`/`downloaded`
keep climbing throughout. `live=81` staying fixed is *correct* here — the
room had just gone to `BROADCAST ENDED`, so that event's manifest is
genuinely final — that part is not the bug.

**What that rules out:**
- Not a network/download problem. Segments keep arriving (`cached`,
  `downloaded` climbing) the whole time.
- Not the delivery queue's drop-under-backpressure path
  (`Player::enqueue()`, bounded to a 250ms wait before dropping a frame
  rather than blocking forever). If that path were engaging repeatedly for
  11 minutes, `dropped` would be in the thousands; it sits flat at `3`
  throughout. So `on_video`/`on_audio` aren't even being called — decode
  isn't happening at all, not "happening but being dropped."

**Where that points:** `Player::feed_loop()` (`src/appliance/player.cpp`)
calls `sess->next_segment()` then `dec->push_fragment(seg->media)` — the
latter is documented as "blocks when the decoder is full." If `feed_loop`
is stuck inside that call, `next_segment()` never runs again, which is
exactly why `m_head` freezes while everything upstream (poll, prefetch)
keeps working. Not confirmed — this is the leading hypothesis, not a
diagnosis.

**Correlating detail, cause or symptom, not yet known which:**
`sound has broken up 10 times` / `20 times` (ALSA xrun warnings, from
`src/appliance/alsa_output.cpp`) appears right as each stall begins.

**Next step, the moment this reproduces again** (box still up, stall
ongoing):
```sh
gdb -p $(pgrep multisite-player) -batch -ex "thread apply all bt"
```
That single command turns the hypothesis above into a diagnosis — it will
show exactly which call `feed_loop` (and the decode thread) are parked in.
Save the output into this entry before doing anything else.

**Files likely involved once the stack trace is in hand:**
`src/appliance/player.cpp` (`feed_loop`, `deliver_loop`, `enqueue`),
`src/core/cmaf_decoder.cpp` (`push_fragment`), `src/appliance/alsa_output.cpp`.

---

### 1. Encoder can hang the same way the decoder used to on shutdown

**Status: known, not fixed. Fix shape is understood; not built.**

`658cd5f` fixed a real bug: the OBS decoder's `stop_workers()` joined
`poll_thread` synchronously (usually on OBS's own UI thread, during source
teardown or Quit) while `poll()` could be mid-network-call with a
30-second timeout and nothing to cancel it — so tearing a source down
while a request was in flight froze OBS for as long as that request had
left. Long enough that an operator watching Quit do nothing force-quit it,
which OBS then reported as a crash. Fixed via `S3Transport::cancel_pending()`
— an atomic flag read by a libcurl progress callback, wired into all four
request paths (put/get/list/object_size), called before any of the three
thread joins in `stop_workers()`. Proven with `tests/test_s3_cancel.cpp`
(a stalling TCP listener) and against the real crash logs.

**The same hazard exists on the encoder.** `RetryUploader::stop()`
(`src/core/retry_uploader.cpp`) joins its own upload thread the same way,
through a blocking `Transport::put()`/`object_size()` call with the same
30-second timeout and nothing to cancel it.

**Why it wasn't folded into the same fix:** `RetryUploader` holds
`Transport& m_transport` — the *abstract* base class
(`src/core/transport.h`), not the concrete `S3Transport`.
`cancel_pending()` only exists on `S3Transport`. The abstract `Transport`
interface is also implemented by the mock transport the whole core test
suite runs against (`tests/test_reliability.cpp`, `tests/test_session.cpp`,
others), so adding `cancel_pending()` there means:

1. Add `virtual void cancel_pending() {}` to `Transport` (no-op default —
   the mock needs no real behaviour, it never blocks).
2. Override it in `S3Transport` to do what it already does (just move the
   existing method up to satisfy the virtual).
3. Call it from `RetryUploader::stop()` before `m_thread.join()`.
4. Rebuild and rerun the full suite — the mock's default no-op must not
   change any existing test's behaviour.

Small, well-understood change. Flagged rather than built same-session as
the decoder fix, to avoid quietly widening what was meant to be one fix.

---

### 2. The Windows install instructions point at the location OBS has deprecated

`docs/OPERATOR.md` tells Windows operators to copy the `obs-plugins` and `data`
folders into the OBS install directory, "typically `C:\Program Files\obs-studio\`",
merging with what is there. That is the legacy layout. OBS's own plugins guide
now recommends `C:\ProgramData\obs-studio\plugins` — one directory per plugin
containing `bin\64bit\<name>.dll` and `data\locale\` — and says of the old
location: *"Plugins in this location will stop working in a future version of
OBS."* So this is a documented install path with an expiry date on it, and an
operator following our guide today installs somewhere OBS intends to stop
looking.

**It is not only a documentation edit**, which is why it is here rather than
fixed on sight. The artifact layout is what has to change with it: the Windows
zip is staged as `obs-plugins/64bit/<name>.dll` plus
`data/obs-plugins/obs-multisite/…` (`obs-plugin.yml`, "Stage plugin with its
dependencies"), which matches the legacy layout and not the recommended one. So
the staging step and the guide move together, and the release notes should say
so for anyone currently installed the old way.

**Worth doing before the update work in Phase 15** (README roadmap), because the
recommended layout is what makes updating a Windows install tractable at all: it
puts the files an updater must replace into application data it can own, rather
than inside `Program Files`, where writing needs elevation.

---

### 3. AES67 audio: built, never run on a Pi

**Status: written, not exercised. Four things are unverified, listed worst first.**

`scripts/player/aes67.sh` installs Digisynthetic's virtual sound card so the
player's audio goes onto the network as AES67 instead of staying inside the
HDMI picture. It builds their kernel module, registers it with DKMS so a
kernel update rebuilds it, installs their `DigiAes67Proc` daemon under
systemd, stores the licence, and points the player at the new card. It has
been dry-run on a laptop and its parts have been tested in isolation; it has
not been run against the hardware.

The vendor package it was written against is
`linux-vsc-aarch64-1.0.1-20260529.zip`, sha256
`f7c8e99f510cf362e7b15d736bb164ef7d547b8590b610ac502448544da46c31`, pinned in
the script. Nothing from that package or any licence is committed.

**1. Nothing tells the far end what to receive.** Their `--setup` asks for a
sample rate, a channel count, a buffer, a PTP domain and an interface, and
never asks for a multicast address, a port, a payload type or a channel map —
those live behind their separate route tool, which looks like a PC-side
configurator, and `_route` in `/etc/DigiAes67Proc/` is where it lands. So a Pi
that installs cleanly may still transmit a stream no receiver has been told
about, which from the other end of the building looks exactly like a broken
driver. **Ask the vendor how the destination is specified**, before believing
any "it works" report that only says the card appeared.

**2. The card accepts only S32_LE; the player asks for FLOAT_LE.** Handled by
using `plughw:` so alsa-lib's plug layer converts, and `plughw:` is already
allow-listed by the web interface, so no C++ changed. Unverified: whether the
plug layer's buffer negotiation satisfies the driver's exact-match check on
`buffer_bytes_max`. If the player logs `audio out failed` with the daemon
healthy, this is the first suspect — the escape hatch is an
`/etc/asound.conf` with a pinned `pcm.plug` naming format, rate, channels,
`period_size` and `buffer_size`, deliberately not shipped yet.

**3. Start order decides whether the card works at all.** The card's rate and
channel count are not in the module; both come from a page of shared memory
the daemon fills in. A player that starts first opens a card advertising zero
channels at zero hertz, refuses it, logs `audio out failed`, sets
`m_audio_open = true` to avoid a retry storm, and **keeps running silently**.
Encoded as a `multisite-player.service.d/aes67.conf` drop-in with
`Wants=`/`After=DigiAes67Proc.service`. Unverified: that this is early enough
in practice.

**4. Lip sync and PTP.** The card reports playback position from the daemon's
millisecond counter and the player corrects from `snd_pcm_delay()`. Whether
that holds sync over a two-hour event is not knowable from ten seconds of test
tone. Separately, AES67 wants both ends within a millisecond and a Pi's NIC
does no hardware timestamping, so the achievable PTP accuracy is whatever the
software manages — measure it on the receiver, not on the Pi.

**Next step:** run `sudo bash scripts/player/aes67.sh --package <zip>` on the
Pi, then work through the four above in that order, with a real AES67 receiver
on the network. The script writes a report of everything it could check to
`/var/tmp/multisite-aes67-<timestamp>/report.txt` and says out loud which
checks it could not make.

---

## Recently landed (context, not action items)

- **Decoder seek accuracy and the timeline readout** — released in
  `v0.1.13-alpha`. Two parts are operator-visible. **Seeking now lands on the moment asked for and says
  so**, within a millisecond, verified against a live event: asked 14:40:43.768,
  reported 14:40:43.769. It previously started playing from the position just
  left, ran on for seconds, then jumped somewhere else and reported a time up to
  a whole segment early. **"Behind live" no longer swings by six seconds** — it
  was a count of segments on both sides and is now the gap between two real
  times, so a two-minute delay reads as two minutes instead of flicking between
  1:54 and 2:00. Also: the decode dock's playhead is interpolated between state
  refreshes rather than stepping twice a second, and its header is split into
  what this box is doing (`PLAYING`/`HELD`/`STOPPED`/`READY`) and what the main
  site is doing — Hold on a finished recording previously showed no indication
  at all.

  Getting there took five broken attempts in one afternoon, each found by the
  operator rather than the suite, because nothing tested this path. The rules
  now live in `src/core/playout_timeline.h` as one ordered decision the delivery
  loop calls, with `tests/test_playout_timeline.cpp` failing on every one of
  those five. **Known residual, not fixed:** a segment's `at_ms` and its media
  pts disagree by up to ~61s on an event whose encoder has restarted, so
  `seq x duration` is not interchangeable with `at_ms`. Seeks are unaffected —
  `at_ms` is used consistently at both ends — but three places still fall back
  to the estimate for segments outside the manifest window, and that is a
  minute-scale error waiting for a seek into one of them.

- **`v0.1.12-alpha`** — released 2026-09-11, tagged on `3bca35c`. The release
  body is `.github/RELEASE-NOTES.md` read at the tag, so the notes commit has to
  land *before* the tag: writing the notes and tagging in one push publishes the
  previous release's body, with the new sections in the tagged tree but absent
  from the release anyone reads. Tag the current `main`, never a commit in
  isolation — the tree only builds as a whole. A tag that has to be withdrawn is
  `gh release delete <tag> --yes --cleanup-tag` plus `git push origin
  :refs/tags/<tag>`, and the re-cut is public a second time, as happened on
  2026-09-11 before this one was cut properly.
- **`96c5d8c`** — the Pi player's hold fix (a held picture outranks the idle
  screen) and its four-boolean rule, `test_idle_screen`.
- **`814cacf`, `4baad6a`** — Stop releases the picture: in-flight requests
  cancelled, the poll loop stopped, the decoder released, the cache deliberately
  kept. `resume_pending()` undoes `cancel_pending()`; `stopped` became a state of
  its own rather than a reading of `!playing`, because loading is also not playing
  and loading has to download.
- **`61b03fd`** — the decode dock's header split into two labels, playback and
  source, so neither can misstate the other. `READY` is new.
- **`9361cca`, `6b5c812`, `bd60b61`, `207182d`** — decoder/lipsync measurement:
  the delivery queue bounded per stream, the delivery lead measured at the
  handout, A/V drift measured rather than reasoned about, and the unsigned cast
  cleared as a suspect (it wraps in the cast and again in the addition; the two
  cancel bit-for-bit). The lipsync cause itself is still open.
- **`0bc1036`** — reverted `a40aad5` and `0444fe9`, a night's decoder UX. Nathan
  saw a frozen timer, a state reading stopped while video played, and Play doing
  nothing, on a build carrying them, and the live status endpoint showed a
  healthy core by every measure available. Known-good beats a build nobody can
  account for. Two things from that session are worth re-landing deliberately:
  Play is disabled precisely because playback is already running, and the badge
  for a pinned recording reports the *event's* state rather than the *playback*
  state.
- **`658cd5f`** — the decoder shutdown-hang fix and `test_s3_cancel`.
- **`ca41a99`** — `m_head` wasn't reset when the session's followed event
  changed, only the "is it seated" flag was — so switching events (or a
  room going live again after ending) could report the OLD event's
  position against the NEW event's manifest. This is shared code
  (`src/core/decoder_session.cpp`), so it applies to the OBS decoder and
  the Pi player both. Confirmed working correctly on the Pi player: after
  a `BROADCAST ENDED` → `LIVE` transition, `head` correctly reset to `0`
  rather than staying frozen at the old event's position (separate from
  the still-open stall in item 0 above, which happens *before* any such
  transition, while continuing to play out the ended recording).
- **`96ef6d0`** — the playing clock could pair a fragment's wall time with
  a different fragment's pts after a jump. Fixed with a once-per-decoder
  latch. Not yet exercised by a real jump-and-check in the field — if a
  jump-to-marker ever shows a position that looks wrong again, this is
  the first place to look.

---

## Design questions raised but not decided (not bugs — separate from the above)

Kept here only as pointers so they aren't lost; each needs a decision, not
a fix.

- **Bucket-mediated status/control**: should campus *appliances* report
  status the same way OBS decoders would, and is the relay meant to be the
  only control plane, or should the encoder's own dock see campuses too?
  Answering the second question decides whether a `desired.json`-per-target
  scheme needs compare-and-swap on its revision from the start.
- **DASH manifest for browser preview/playback**: operator-only preview
  (behind the relay's existing login, no public bucket needed) vs. public
  viewing; live vs. "last Sunday, downloadable" only.
- **Product names** for the three shipped components (OBS plugin pair, Pi
  player, simulcast portal/relay) — versions now all derive from one
  `PROJECT_VERSION`; names are still whatever the code happens to call
  them.
