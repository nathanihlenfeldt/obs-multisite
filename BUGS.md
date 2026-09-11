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

**Since this was written**, the delivery queue is bounded per stream rather
than as one total (`kMaxQueuedVideo` / `kMaxQueuedAudio`) and the status line
now splits drops — `dropped=N (V v / A a)`. The reasoning above that ruled out
the drop path still holds, and the split makes the next repro say which stream
is affected. It also removes one contributor worth knowing about: the delivery
thread both presents the picture and calls `snd_pcm_writei`, which blocks, and
under the old flat bound audio could take every slot while it was blocked,
leaving no room for a picture. That is the mechanism behind the ALSA xruns
correlating with each stall, so it may or may not have been part of this — the
thread dump is still what decides.

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

### 3. AES67 audio: works on the bench, unproven over an event

**Status: working on a bench Pi — eight channels of clean AES67 audio, with the
under-run solved (point 5). Points 1 and 2 still need a real event; points 3 and
4 were open questions the bench run has answered and are kept for the edges they
do not cover.**

`scripts/player/aes67.sh` installs Digisynthetic's virtual sound card so the
player's audio goes onto the network as AES67 instead of staying inside the
HDMI picture. It builds their kernel module, registers it with DKMS so a
kernel update rebuilds it, installs their `DigiAes67Proc` daemon under
systemd, stores the licence, and points the player at the new card. It has now
been run on a bench Pi: the module built, the daemon came up, the card appeared,
the player opened it, and eight channels of clean audio arrived. Getting there
took two things: the format negotiation in point 3, and the calendar write in
point 5 that stopped the audio gapping once per frame. That settles the
questions about whether the pieces fit together; what it does not settle is
anything that needs a real service, which is the first two points below. The
operator-facing version of this is
[docs/SATELLITE.md](docs/SATELLITE.md#aes67-audio-on-the-network).

The vendor package it was written against is
`linux-vsc-aarch64-1.0.1-20260529.zip`, sha256
`f7c8e99f510cf362e7b15d736bb164ef7d547b8590b610ac502448544da46c31`, pinned in
the script. Nothing from that package or any licence is committed.

**1. Nothing on the Pi tells the far end what to receive.** Their `--setup`
asks for a sample rate, a channel count, a buffer, a PTP domain and an
interface, and never asks for a multicast address, a port, a payload type or a
channel map — those live behind their separate route tool, which looks like a
PC-side configurator, and `_route` in `/etc/DigiAes67Proc/` is where it lands.
Audio did arrive on the bench, so a receiver can find the stream as it comes;
what is still open is how a *specific* destination is chosen, which is what a
site landing on a fixed address and port in an existing console will need.
**Ask the vendor how the destination is specified**, before commissioning a
site where the address matters.

**2. Lip sync and PTP.** The player's own contribution to audio latency is now
known exactly: `kDigisynLeadMs` in `digisyn_calendar.h` is 3 ms, so audio is
written into the calendar 3 ms ahead of the daemon's clock — small enough that
it should not read as a lip-sync error, but it is the number to check against
the picture on a real service. There is **no active correction** either way:
`AlsaOutput::delay_s()` exists and reads the card's playback position, but
nothing calls it, so the audio is not steered from the card's reported delay —
the earlier note here claiming it was has been corrected. Whether a fixed 3 ms
lead holds over a two-hour event (the daemon's clock and the player's decode rate
are independent, so they can drift apart) is not knowable from ten seconds of
test tone; Monday's lip-sync check over a full run is what settles it. Separately,
AES67 wants both ends within a millisecond and a Pi's NIC does no hardware
timestamping, so the achievable PTP accuracy is whatever the software manages —
measure it on the receiver, not on the Pi.

The two below were open questions when this script was first written, and the
bench run has since answered them in the common case. They stay here because
each has an edge it does not cover, and because a future failure will land on
one of them.

**3. The card accepts only S32_LE; the player asked for FLOAT_LE.** Now handled
in the player: `alsa_output.cpp` asks the card for float first and falls back to
S32_LE and then S16_LE, converting in `pcm_convert.h` when an integer format is
what it agreed to. That was reached by a bench box logging

```
audio output: hw:CARD=Default,DEV=0 will not take floating-point audio: Invalid argument
```

— the device list in the web interface offers `hw:` entries, so choosing the
card from the menu was enough to hit it, and `hw:` has no plug layer to convert.
The installer still writes `plughw:`; both now work. `tests/test_pcm_convert.cpp`
covers the arithmetic (rounding, saturation, channel padding), which is the part
that can be quietly wrong and needs no sound card to check.

The old escape hatch was `plughw:` and it is still the fallback: alsa-lib's plug
layer converts, and `plughw:` is allow-listed by the web interface. Still
unverified: that the driver's exact comparison of `buffer_bytes_max` holds at
rates, channel counts and buffer sizes other than 8 channels at 48 kHz — the
driver compares buffer sizes exactly, so a mismatch fails the open. If the
player logs `audio out failed` with the daemon healthy and a format this code
can send, this is the first suspect; the escape hatch is an `/etc/asound.conf`
with a pinned `pcm.plug` naming format, rate, channels, `period_size` and
`buffer_size`, deliberately not shipped yet.

**4. Start order decides whether the card works at all.** The card's rate and
channel count are not in the module; both come from a page of shared memory
the daemon fills in. A player that starts first opens a card advertising zero
channels at zero hertz, refuses it, logs `audio out failed`, sets
`m_audio_open = true` to avoid a retry storm, and **keeps running silently**.
Encoded as a `multisite-player.service.d/aes67.conf` drop-in with
`Wants=`/`After=DigiAes67Proc.service`. The ordering held across reboots on the
bench, so the drop-in works in the simple case. Still unverified: that it wins
the race on a machine slow to bring the daemon up under load.

**5. The buffer under the picture is 8 ms; one decoded frame is 21, so it
under-runs on every frame — solved by writing the card's calendar directly.**
This was the whole of the "sound is broken up" problem. The root cause is in the
vendor driver's own limits, below; the fix was to stop using ALSA on this card
at all and address the daemon's shared buffer the way the vendor does. It now
plays clean on the bench (see **The fix** at the end of this point).

Seen on the bench box alongside the format error in point 3, playing a 48 kHz
eight-channel feed:

```
sound has broken up 8060 times — the board reports neither under-voltage nor throttling, so this is most likely the feed or a burst of seeking rather than the hardware
sound has broken up 8070 times — …
```

climbing by ten a line, several lines a second — roughly forty under-runs a
second, sustained. The message is right that it is not the power supply, and
right that it is not the feed; it is arithmetic, and the arithmetic is here:

```c
// DigiAes67KoLib/Digisyn-vSndCard.c
hw->formats          = SNDRV_PCM_FMTBIT_S32_LE;
hw->period_bytes_min = bytes_1ms;      hw->period_bytes_max = bytes_1ms;
hw->periods_min      = dsp->bufMs;     hw->periods_max      = dsp->bufMs;
hw->buffer_bytes_max = bytes_1ms * hw->periods_max;
```

A period is exactly one millisecond, the number of periods is `bufMs`, and the
buffer is therefore `bufMs` milliseconds and cannot be anything else. The
player asks for half a second (`alsa_output.cpp`, `set_buffer_time_near`), and
the driver silently clamps it to 8. One AAC frame at 48 kHz is 1024 samples —
21.3 ms, and AC-3's 1536 samples is 32 ms. The delivery thread writes one whole
frame per iteration (`player.cpp`, `m_audio.write(item.audio)`), the card takes
8 ms of it, plays that, and sits empty for the remaining ~13 ms until the next
frame arrives. One under-run per frame, which is the log.

The ALSA interface this card presents is a **shim, not a sound card**, and that
is what decides the fix. From `Digisyn-vSndCard.c`:

```c
.prepare = dummy_pcm_prepare,   // return 0 — a no-op
.pointer = dummy_pcm_pointer,   // (sampleRate / 1000 * dsp->msIndex) % buffer_size
// no .copy op, no .ack op in the ops table
substream->runtime->dma_area = Dsp_getBuf_phyToNet(dsp);   // the ALSA buffer IS the daemon's shared memory
```

Three consequences, all of which matter:

- **`prepare` does nothing**, so `snd_pcm_prepare()` — the whole of ALSA's xrun
  recovery — resets the core's counters to zero while the driver's pointer
  immediately reports a non-zero position again. **Recovery cannot resync on
  this card.**
- **The pointer is a function of the daemon's millisecond clock**, free-running
  in real time, not of what was written. There is no DMA position to catch up
  to.
- **There is no `.copy`**, so `snd_pcm_writei` is a memcpy into memory the
  daemon can read, and `snd_pcm_period_elapsed()` is only ever called from the
  daemon's `ioctl` on its misc device. The vendor's supported data path is that
  mmap'd buffer plus `ioctl`, not the ALSA PCM device.

So the one under-run per frame is **structural**: one 21 ms frame written into
an 8 ms ring, with no working recovery behind it.

**The fix: write the calendar, not ALSA.** The mmap path is the one this landed
on. `DigiAes67Proc` and the driver share a page of memory the daemon transmits
from: a header of card facts (`sampleRate`, `chNum`, `bufMs`, and `msIndex`, a
free-running millisecond clock) followed by a calendar of `bufMs` one-millisecond
slots, which the daemon reads one slot per millisecond. There is no ring to
over- or under-run — only "write the slot for the millisecond you want heard,
before the daemon reaches it." So the player does exactly that:

- **`digisyn_calendar.h`** describes the header and computes the slot addresses,
  with no ALSA and no threads in it. `tests/test_digisyn_calendar.cpp` covers the
  slot arithmetic, including the wraparound — an off-by-one there loses a
  millisecond every time the calendar turns over, which is a click every 60 ms.
- **`audio_queue.h`** is the buffer the ALSA ring would otherwise have been: the
  decoded frame goes in there and the delivery thread returns, so it never waits
  on the card. `tests/test_audio_queue.cpp` covers it.
- **`alsa_output.cpp`** detects the AES67 card in `open()` and switches to this
  path for it alone, then runs a feeder thread that takes one millisecond out at
  a time and copies it into the slot `kDigisynLeadMs` (3 ms) ahead of the clock.

**Detection is by card identity, not by the config string**, so every way of
naming the card takes the branch — `hw:`, `plughw:`, `sysdefault:`, `default` —
and every other device, HDMI included, is left on the untouched ALSA path. This
part cost a whole bench session: the first attempt asked the PCM for its *name*
and the vendor driver sets that to `"Dummy PCM"` (`Digisyn-vSndCard.c`), so it
never matched and fell through to ALSA silently. It now matches on the card's id
and driver (`card_id_is_digisyn`), and logs the reason either way, so it cannot
fail silently again. If the calendar cannot be used it warns and falls back to
ALSA rather than going quiet — broken-up audio beats none.

**A feeder thread was written against ALSA first, tested on the bench, and
reverted — do not retry that shape of it.** Its reasoning was sound (feed 1 ms
periods from a ring so the card is never over- or under-fed, and take the write
off the thread that presents the picture) but it depends on `snd_pcm_prepare()`
resynchronising after an under-run, and on this driver that can never work. The
bench log said so plainly: under-runs went from ~30/second to ~200/second, and
kept climbing after playback had already `STOPPED` — the feeder's retry loop
re-entering `recover()` and incrementing the counter without ever returning to
check its stop flag. It was committed as `2f6fcb0` and reverted in the commit
immediately after; the history is kept rather than rewritten so the next person
can see what was tried and why it failed. The lesson carried into the calendar
path: it does **no** threading against ALSA and needs **no** xrun recovery — it
is a memcpy into the slot the daemon reads next.

What survives from that earlier work and is worth keeping: the format
negotiation and the granted-buffer warning (both from `29e0971`), and a reworded
under-run message that points at the real cause instead of at the feed:

```
sound has broken up N times — the board reports neither under-voltage nor
throttling, so this is not the hardware. If the card's buffer is smaller than a
decoded frame (the log says so as it opens), suspect that first; otherwise it is
the feed or a burst of seeking
```

**What is actually left, in order:**

1. **Ask the vendor about the destination.** Unchanged and still theirs to
   answer: `--setup` never asks for a multicast address, port, payload type or
   channel map — those live behind their separate route tool. Audio arrives on
   the bench, so a receiver can find the stream as it comes; what a site landing
   on a *fixed* address and port will need is how to choose it.
2. **Watch it over a full event**, on the picture and the sound together — the
   one thing a bench cannot stand in for, and what settles the lip-sync question
   in point 2 (the 3 ms lead, and whether it drifts over two hours).
3. **Do nothing else in C++ to the ALSA path.** Chunked writes, bigger rings and
   higher `audio_buffer_ms` were each considered; none change the fact that the
   card consumes from an 8 ms window it will not widen. The calendar write is
   the answer, and it is in.

**Next step:** Monday's lip-sync check over a full-length service, which is
point 2 above. The under-run that made that question unanswerable is fixed.

---

## Recently landed (context, not action items)

- **AES67 audio plays clean — v0.1.14-alpha.** The card was under-running on
  every decoded frame (`sound has broken up` climbing by ten several times a
  second) because an 8 ms ALSA buffer cannot hold a ~21 ms frame, and the
  vendor's `.prepare` is a no-op so ALSA's xrun recovery can never resync. Fixed
  by writing the daemon's shared calendar directly instead of through ALSA, and
  detecting the card by its id and driver so every way of naming it takes that
  branch while HDMI and every other device stay on the untouched ALSA path.
  Full account in point 5 below. Confirmed on the bench: clean eight-channel
  audio out of the network. **Still open: lip sync over a full event** (the write
  leads the daemon by a fixed 3 ms; whether that holds over two hours is
  Monday's check) and the vendor's multicast destination question.

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
