# Developer guide

Building the code yourself, running the tests, and knowing what is where. For
the project's state — what works and what is next — see the
[README](../README.md).

## Build and test

Core and tests, no OBS required:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Requires CMake ≥ 3.16 and a C++17 compiler. On Linux and macOS you also need
OpenSSL headers; on Windows the crypto backend uses the built-in bcrypt, so
OpenSSL is not needed.

With the OBS plugin (adds libobs and FFmpeg):

```sh
cmake -S . -B build -DBUILD_OBS_PLUGIN=ON
```

With the operator docks (adds Qt6 and obs-frontend-api):

```sh
cmake -S . -B build -DBUILD_OBS_PLUGIN=ON -DENABLE_QT=ON
```

### macOS

Apple Silicon only, and the core needs no OpenSSL — it uses CommonCrypto from
libSystem, so a built plugin loads on a Mac that has never had Homebrew.
`ctest` should pass 13/13 with nothing installed but CMake and FFmpeg.

For the **plugin**, the only real difficulty is ABI matching. OBS.app carries
its own FFmpeg, Qt and libobs, and a plugin has to use those exact copies. A
build against Homebrew's FFmpeg or Qt loads on the machine that built it and
fails elsewhere, because Homebrew tracks the latest version and OBS pins one —
at the time of writing that is libavcodec 63 against OBS's 62, and Qt 6.11.2
against 6.11.1. A second Qt is the worse of the two: the docks attach to the
host's `QApplication`, and a duplicate `QtCore` has none.

So take the dependencies from **obs-deps at the version OBS itself pins**,
which is in `CMakePresets.json` in the OBS source under the `dependencies`
preset. For OBS 32.2.2 that is `2026-07-15`:

```sh
OBS_TAG=32.2.2; DEPS_VER=2026-07-15
mkdir -p deps/root
for n in macos-deps-$DEPS_VER-arm64.tar.xz macos-deps-qt6-$DEPS_VER-arm64.tar.xz; do
  curl -L "https://github.com/obsproject/obs-deps/releases/download/$DEPS_VER/$n" | tar x -C deps/root
done
curl -L "https://github.com/obsproject/obs-studio/archive/refs/tags/$OBS_TAG.tar.gz" | tar xz
printf '#pragma once\n#define OBS_RELEASE_CANDIDATE 0\n#define OBS_BETA 0\n' > obsconfig.h

DEPS=$PWD/deps/root; OBS_SRC=$PWD/obs-studio-$OBS_TAG
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_OBS_PLUGIN=ON -DENABLE_QT=ON \
  -DCMAKE_PREFIX_PATH="$DEPS" -DQt6_DIR="$DEPS/lib/cmake/Qt6" \
  -DFORCE_FFMPEG_MANUAL_SEARCH=ON \
  -DFFMPEG_INCLUDE_DIR="$DEPS/include" \
  -DFFMPEG_avformat_LIBRARY="$DEPS/lib/libavformat.dylib" \
  -DFFMPEG_avcodec_LIBRARY="$DEPS/lib/libavcodec.dylib" \
  -DFFMPEG_avutil_LIBRARY="$DEPS/lib/libavutil.dylib" \
  -DFFMPEG_swresample_LIBRARY="$DEPS/lib/libswresample.dylib" \
  -DFFMPEG_swscale_LIBRARY="$DEPS/lib/libswscale.dylib" \
  -DLIBOBS_INCLUDE_DIR="$OBS_SRC/libobs" \
  -DLIBOBS_CONFIG_INCLUDE_DIR="$PWD" \
  -DLIBOBS_FRONTEND_INCLUDE_DIR="$OBS_SRC/frontend/api"
cmake --build build --target obs-multisite
```

Two things are worth knowing about that. Passing every FFmpeg path explicitly
and pinning `Qt6_DIR` is not belt-and-braces: if Homebrew's copies are
installed they are found first, and the result is the mismatched build this
recipe exists to avoid. And **no OBS binary is needed** — only headers. The
plugin is linked with `-undefined dynamic_lookup`, so libobs and
obs-frontend-api resolve out of the running OBS at load time. Qt *is* linked
for real, because those symbols are not OBS's to provide.

The result is `obs-multisite.plugin`, whose every versioned dependency is an
`@rpath` reference to something OBS already ships, with one rpath —
`@executable_path/../Frameworks`. A plugin has no executable of its own, so
`@executable_path` is the host: `OBS.app/Contents/MacOS`, making
`../Frameworks` OBS's own framework directory wherever OBS is installed.
Check a build with `otool -L` and `otool -l | grep -A2 LC_RPATH`; anything
that is not `@rpath`, `/System` or `/usr/lib` is a path from your machine and
will not exist on anybody else's. CI asserts exactly that.

With the public simulcast relay (adds SQLite; needs the `ffmpeg` command at
run time, not at build time):

```sh
cmake -S . -B build -DMULTISITE_BUILD_RELAY=ON -DBUILD_PLAYER=OFF
cmake --build build --target multisite-relay
```

Or build the container, which runs the relay's tests as part of the image so a
broken build cannot become something somebody deploys:

```sh
docker build -f relay/Dockerfile -t multisite-relay .
```

CI builds and tests the core on Linux x86, **Linux ARM64**, Windows and macOS,
and produces the installable Windows and macOS plugins. The ARM64 job exists
because the planned appliance runs there, so a regression is caught in CI
rather than on hardware. The macOS job asserts what makes a bundle loadable on
a machine other than the one that built it: package type `BNDL`, arm64, the
module entry points exported, exactly one rpath, no OpenSSL, and no absolute
path outside `/System` and `/usr/lib`.

---

## What the tests cover

Thirteen suites, all runnable without OBS (the `cmaf*` ones need FFmpeg and
`s3_url` needs libcurl; the rest need neither):

| suite | what it proves |
|---|---|
| `reliability` | durability across a crash, ordered drain through an outage, checksum rejection, permanent-failure handling |
| `session` | the write-ordering invariant holds continuously, including across a crash and resume; packed multi-channel audio round-trips, channel order intact |
| `decoder` | timeslipping: the cache fills while paused, resume continues exactly where it stopped, markers, seek-by-time, VOD playback, and that a gap stalls rather than silently skipping |
| `responsive` | UI queries stay fast while downloading — the property that keeps OBS usable during an event |
| `snapshot` | the figures the dock reads agree with the session they are built from |
| `s3_list` | a ListObjectsV2 response is read correctly, including pagination and an access-denied body; a signed query string is canonicalised the way S3 does it |
| `event_catalog` | events are classified as live / recording / interrupted, rooms stay separate, a listing failure is not shown as "no recordings", an event that recorded nothing is not offered, and an event with no room-index entry still lists alongside those that have one |
| `crypto` | SHA-256 and HMAC-SHA256 match the NIST and RFC 4231 vectors on whichever backend was compiled in — OpenSSL, Windows bcrypt or Apple CommonCrypto. Each CI platform runs its own, so all three are held to the same published answers and a signed request cannot differ by platform |
| `cmaf`, `cmaf_hevc` | the muxer produces decodable fragments for H.264 and HEVC, with multi-track audio |
| `cmaf_decode` | the round trip: what the muxer wrote, the decoder plays back |
| `s3_url` | endpoint and bucket values survive being pasted with schemes, slashes and whitespace |
| `core_portable` | the core has not acquired an OBS or Qt dependency |

Building with `-DMULTISITE_BUILD_RELAY=ON` adds three more, which the container
image runs as part of the build so a broken relay cannot become an image
somebody deploys on a Sunday morning:

| suite | what it proves |
|---|---|
| `stream_plan` | what may be sent onward and what must be refused — HEVC into FLV, AV1 anywhere, packed multi-channel audio, a sound feed that has vanished, a manifest whose track positions do not line up; that HEVC over SRT is allowed where it is not over RTMP; that a pasted SRT address is pulled apart with the secrets taken out of it; and that no secret survives redaction for the log |
| `relay_state` | the awkward cases without a destination or a wait: a stall ridden out and then given up on, an unexpected exit and its backoff, ending cleanly versus being cut short, an edit that rebuilds a stream without counting as a fault, and an SRT listener with nobody attached waiting indefinitely rather than being treated as broken |
| `config_store` | destinations and storage settings survive a restart, an invalid one is refused before it reaches the database, and an SRT destination's stream id, passphrase and latency round-trip intact |
