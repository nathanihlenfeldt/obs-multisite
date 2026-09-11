# Bugs and short-term build items

Working list, meant to survive a change of machine or a change of agent —
each entry has enough context to act on without anyone having been in the
room when it was written. Delete an entry once it's fixed and released;
this file is not a changelog.

Last updated: 2026-09-11.

---

## Open

### 0. v0.1.12-alpha is written but deliberately not tagged yet

**Status: waiting on write-ups for the work merged after the last release. Do
not tag `v*` from `main` until this is resolved.**

`main` is 13 commits past `v0.1.11-alpha`. The Pi player's hold fix
(`screen_action()`, a held picture outranks the idle screen) is one of them, at
`96c5d8c`, and it is the only one with notes: a section under "What's new in
v0.1.12-alpha" in `.github/RELEASE-NOTES.md`, alongside the `VERSION 0.1.12`
bump. Every other change to the code merged since the last release has **no
entry in `RELEASE-NOTES.md`** — its reasoning lives in the entries below, and
this file is explicitly not a changelog.

Most of that is decoder/lipsync measurement (delivery queue bounding, playout
clock, drift measurement, `test_playout_clock`), which changes no operator's day.
Two commits do:

- `814cacf` **Make Stop actually stop the picture** and `4baad6a` **Stop returns
  the source to a neutral state** — operator-visible, and not describable as a
  fix to something that looked broken on screen. Stop now cancels in-flight
  requests, stops the poll loop issuing new ones, and releases the decoder,
  while deliberately keeping the cache so that Play re-enters from disk instead
  of re-satisfying `start_buffer_seconds`. `stopped` became a state of its own
  rather than a reading of `!playing`, because loading an event is also not
  playing and loading has to download. `S3Transport::cancel_pending()` was
  documented as one-way and now has a `resume_pending()`, without which a
  stopped source could never download again. The decode dock leads with
  "Stopped — nothing downloading", new strings are in `en-US.ini` and
  `en-GB.ini`, and the Hold button's label — "Hold picture (keeps recording)" —
  finally means what it implies by contrast.

- `d3cb13a` **Split the state chip** — operator-visible, and the second half of
  the Stop change above: the decode dock's header was one label multiplexing the
  main site's state, this decoder's playback state and the venue's link health
  behind a precedence chain, so only one could show. Playing a finished
  recording read "BROADCAST ENDED" with nothing to say it was playing, and
  **Hold on a finished recording showed no indication at all** — `paused` was
  only tested inside the room-state switch's Live branch, which an ended event
  never reaches. Now two labels: playback (`STOPPED`/`LOADING…`/`BUFFERING…`/
  `HELD`/`PLAYING`/`READY`) and source (`LIVE`/`BROADCAST ENDED`/…). `READY` is
  new and names what Load leaves behind. New strings in both locale files; the
  web remote already had the split and needed only `stopped` in the status JSON.

One wording collision to settle while writing that up: the hold-fix note says
"**Stop** and waiting are unchanged", which is true of the *screen rule* it is
discussing and reads as a claim about Stop. Shipping both in one release without
saying which is meant would contradict itself.

Tagging `v0.1.12-alpha` from `main` right now would publish the release (a
Windows plugin build and a relay container image) shipping that work while the
notes mention only the hold fix — the discrepancy lands in front of anyone
installing it.

**To finish the release:** the decoder work is two separate decisions — the
lipsync measurement, and the Stop behaviour. Write up whatever is ready under
`v0.1.12-alpha` alongside the hold fix, then tag. If the Stop work is not ready
to describe, hold the tag; the alternative is reverting it, as `0bc1036` did for
a night's decoder UX, because a release whose notes omit a change to Stop is the
discrepancy this entry exists to prevent. The hold fix alone is already
delivered to anyone tracking `main`, which is what the appliance installer does.

**If you do tag, tag the current `main`**, not the fix commit in isolation — the
fix is not a branch and the tree only builds as a whole.

A tag pushed from a commit that is not an ancestor of `main` builds the plugin
and the container against that commit and creates the GitHub release as soon as
the tag lands, so a mistake here is public before it is noticed: `gh release
delete <tag> --yes --cleanup-tag` plus `git push origin :refs/tags/<tag>` is the
way back, as happened once on 2026-09-11 before the tag was cut properly.

---

### 1. Pi player: playback can stall indefinitely while downloads keep succeeding

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

### 2. Encoder can hang the same way the decoder used to on shutdown

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

### 3. The Windows install instructions point at the location OBS has deprecated

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

## Recently landed (context, not action items)

- **`658cd5f`** — the decoder shutdown-hang fix and `test_s3_cancel`, above.
- **`ca41a99`** — `m_head` wasn't reset when the session's followed event
  changed, only the "is it seated" flag was — so switching events (or a
  room going live again after ending) could report the OLD event's
  position against the NEW event's manifest. This is shared code
  (`src/core/decoder_session.cpp`), so it applies to the OBS decoder and
  the Pi player both. Confirmed working correctly on the Pi player: after
  a `BROADCAST ENDED` → `LIVE` transition, `head` correctly reset to `0`
  rather than staying frozen at the old event's position (separate from
  the still-open stall in item 1 above, which happens *before* any such
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
