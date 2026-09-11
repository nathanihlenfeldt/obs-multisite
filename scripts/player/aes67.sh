#!/usr/bin/env bash
#
# aes67.sh — put the player's sound on the network instead of in the picture.
#
#   sudo bash scripts/player/aes67.sh --package ~/linux-vsc-aarch64-1.0.1-20260529.zip
#
# Or, on a box that has never seen this repository, with the vendor's download
# link and the licence already to hand:
#
#   VSC_URL=https://…/linux-vsc-aarch64-1.0.1-20260529.zip \
#   AES67_LICENSE_FILE=/home/pi/license.dat \
#     curl -fsSL https://raw.githubusercontent.com/stageaudioworks/obs-multisite/main/scripts/player/aes67.sh | sudo -E bash
#
# Why this exists
# ---------------
# The player puts the event's sound on the HDMI output, which means the sound is
# inside the picture and only reaches whatever is plugged into the Pi. A campus
# that wants the sound on its own audio console — a separate feed, with its own
# level control, working whether or not a screen is attached — needs the sound
# on the network, and on a church network that means AES67.
#
# Digisynthetic's virtual sound card is how that is done: a kernel module that
# registers an ALSA card whose buffers are shared with their DigiAes67Proc
# daemon, which sends and receives the AES67 streams. This script builds and
# installs both, stores the licence, and points the player at the new card.
#
# What this is, honestly
# ----------------------
# It has run on a bench Pi: the module builds, the daemon comes up, the card
# appears, and eight channels of audio arrive. That is a bench, not an event,
# so what follows is still the list of things handled here rather than things
# proven, three of them known from reading the vendor's package before any of
# it was run:
#
#   1. THE CARD DOES NOT ACCEPT FLOATING-POINT SAMPLES. Its only format is
#      S32_LE. The player asks for FLOAT_LE and gives up if it cannot have it,
#      so the card has to be opened as `plughw:` — alsa-lib's plug layer does
#      the conversion — and never as `hw:`, which will fail. This script finds
#      the card's name at run time and writes the `plughw:` form.
#
#   2. THE CARD HAS NO RATE AND NO CHANNEL COUNT OF ITS OWN. Both are read out
#      of a page of shared memory that the *daemon* fills in. Load the module
#      and open the card before the daemon has started and the card offers zero
#      channels at zero hertz. The daemon therefore starts first, and the player
#      is ordered after it by a systemd drop-in.
#
#   3. THE CARD DERIVES ITS PLAYBACK POSITION FROM THE DAEMON'S MILLISECOND
#      COUNTER, and the player corrects lip sync from snd_pcm_delay(). Whether
#      that delay is truthful enough to hold sync across a two-hour event
#      cannot be settled from here. Watch a long service before trusting it.
#
# It installs the module through DKMS. The vendor's instructions say that a
# kernel update means rebuilding the driver by hand, which on an unattended box
# in a church means the sound quietly disappears one Tuesday and nothing on the
# screen explains why. DKMS rebuilds it as part of the kernel upgrade instead.
#
# What it deliberately does not do: it does not choose the destination. Nothing
# in the vendor's configuration flow asks for a multicast address, a port, or a
# channel map — those live behind their own route tool, on a computer — so the
# Pi side can be perfect while a receiver that needs a particular address and
# port still has not been told. Audio did arrive on the bench, so a receiver can
# find the stream as it comes; what is open is aiming it somewhere specific. The
# notes printed at the end say so, because a silent success here would be the
# most expensive kind.
#
# Safe to run again: every step checks before it acts, and an existing player
# configuration is edited rather than replaced.
set -euo pipefail

# ── What this installs, and where ────────────────────────────────────────────
# The version is in the vendor's file name as well as in the archive, so the
# directory it unpacks to is derived from it rather than typed out twice.
VSC_VERSION="${VSC_VERSION:-1.0.1-20260529}"
VSC_ARCH="$(uname -m)"
VSC_ZIP_NAME="linux-vsc-$VSC_ARCH-$VSC_VERSION.zip"

# The vendor's download link is deliberately not baked in here. Their package
# is served from a host in China; sometimes that is reachable from the campus
# and sometimes the zip arrives on a USB stick instead. Either is accepted:
# VSC_URL, or --package with a path.
VSC_URL="${VSC_URL:-}"

# Pinned, because we are about to build a kernel module out of this file and
# load it into the running kernel. A truncated download, or a copy that has
# travelled through a Windows machine and picked up CRLF line endings, fails
# to build in a way that reads like a compiler problem rather than a file
# problem. Checking the digest first turns that into one clear sentence.
# This is the aarch64 1.0.1-20260529 package, verified by hand. Set
# VSC_SHA256= (empty) to accept a newer package from the vendor.
VSC_SHA256="${VSC_SHA256:-f7c8e99f510cf362e7b15d736bb164ef7d547b8590b610ac502448544da46c31}"

PACKAGE="${VSC_PACKAGE:-}"
BUILD_DIR="${BUILD_DIR:-/var/tmp/digisyn-vsc}"

# Where the sibling installer lives, so that --with-player can fetch it, and so
# that the closing notes can print a command that actually works.
RAW_BASE="${RAW_BASE:-https://raw.githubusercontent.com/stageaudioworks/obs-multisite/main}"
RAW_URL="$RAW_BASE/scripts/player/install.sh"

PREFIX_BIN="${PREFIX_BIN:-/usr/local/bin}"
DAEMON="$PREFIX_BIN/DigiAes67Proc"
DAEMON_CONF_DIR="/etc/DigiAes67Proc"
DAEMON_CONF="$DAEMON_CONF_DIR/DigiAes67Proc.conf"
DAEMON_LICENSE="$DAEMON_CONF_DIR/_li"
DAEMON_SERVICE="DigiAes67Proc"
MODULE_NAME="Digisyn-vSndCard"
MODULE_LOAD_CONF="/etc/modules-load.d/digisyn-vsndcard.conf"
DKMS_NAME="digisyn-vsndcard"
# DKMS reads a hyphen in a version as a separator between package and version,
# so the vendor's 1.0.1-20260529 is written 1.0.1.20260529 for DKMS alone. The
# file name and the vendor's own build script still use their spelling.
DKMS_VERSION="${VSC_VERSION//-/.}"
DKMS_SRC="/usr/src/$DKMS_NAME-$DKMS_VERSION"

PLAYER_CONFIG="/etc/multisite-player/config.json"
PLAYER_UNIT="multisite-player"
PLAYER_DROPIN_DIR="/etc/systemd/system/$PLAYER_UNIT.service.d"
PLAYER_DROPIN="$PLAYER_DROPIN_DIR/aes67.conf"

# ── The settings this writes into the daemon ─────────────────────────────────
# These are choices rather than discoveries, so they are named at the top where
# they can be seen and changed, instead of being buried in the flow below.
#
# 8 channels at 48 kHz is the shape this project's audio path is built for: the
# encoder packs the programme as 8 channels of 48 kHz in PROJECT-SCOPE.md §8.2,
# and the appliance expects to hand over that shape. The vendor's card does
# 48000/96000/192000 and nothing else, so a 44.1 kHz event cannot be carried
# natively — it would have to be converted, and if the script can work out the
# rate the feed is arriving at, it says so.
AES67_SAMPLE_RATE="${AES67_SAMPLE_RATE:-48000}"
AES67_CHANNELS="${AES67_CHANNELS:-8}"
# Milliseconds of buffer; the vendor's default is 8. Their 0 means mixing mode,
# which bypasses the system sound card entirely and is not what a player wants.
AES67_BUF_MS="${AES67_BUF_MS:-8}"
# The PTP domain. 0 is the AES67 default and what most consoles expect.
AES67_PTP_DOMAIN="${AES67_PTP_DOMAIN:-0}"
AES67_PTP_MODE="${AES67_PTP_MODE:-0}"
AES67_LOW_LATENCY="${AES67_LOW_LATENCY:-0}"
# The interface the AES67 traffic leaves by. Left empty, the script takes the
# one carrying the default route, which on a single-NIC Pi is the right answer.
AES67_IFACE="${AES67_IFACE:-}"

# Licensing. Neither of these is echoed, logged, or written anywhere this
# script keeps a copy of.
AES67_LICENSE_FILE="${AES67_LICENSE_FILE:-}"
AES67_LICENSE_CODE="${AES67_LICENSE_CODE:-}"

WITH_PLAYER=0
NO_DKMS=0
NO_PLAYER_CONFIG=0
INSTALL_TOOLS=0
DRY_RUN=0
ASSUME_YES="${ASSUME_YES:-0}"

say()  { printf '\n\033[1;36m==>\033[0m \033[1m%s\033[0m\n' "$*"; }
note() { printf '    %s\n' "$*"; }
warn() { printf '\033[1;33m    %s\033[0m\n' "$*"; }
die()  { printf '\n\033[1;31mThat did not work:\033[0m %s\n\n' "$*" >&2; exit 1; }

have() { command -v "$1" >/dev/null 2>&1; }

# A question is only ever asked when there is a terminal to ask on. Piped in
# from curl, stdin is this script's own text and reading it there would eat the
# rest of the installer — the same trap install.sh documents for the ZeroTier
# key. Off a terminal, the answer has to come in as an argument or a variable.
ask() {
  _prompt="$1"; _default="${2:-}"; _answer=""
  # -y means the defaults, not just "press enter through everything": a prompt
  # that is still put on the screen is still a prompt, and on a box with no
  # terminal it would block. So with ASSUME_YES the default comes straight back.
  [ "$ASSUME_YES" -eq 0 ] || { printf '%s' "$_default"; return 0; }
  if [ ! -r /dev/tty ]; then printf '%s' "$_default"; return 0; fi
  if [ -n "$_default" ]; then
    printf '    %s [%s]: ' "$_prompt" "$_default" > /dev/tty
  else
    printf '    %s: ' "$_prompt" > /dev/tty
  fi
  IFS= read -r _answer < /dev/tty || _answer=""
  [ -n "$_answer" ] || _answer="$_default"
  printf '%s' "$_answer"
}

# Read without echoing, so a licence key does not sit on the screen while
# somebody photographs the box, and never through a shell trace.
ask_secret() {
  _prompt="$1"; _answer=""
  [ "$ASSUME_YES" -eq 0 ] || { printf ''; return 0; }
  if [ ! -r /dev/tty ]; then printf ''; return 0; fi
  printf '    %s: ' "$_prompt" > /dev/tty
  have stty && stty -echo < /dev/tty 2>/dev/null || true
  IFS= read -r _answer < /dev/tty || _answer=""
  have stty && stty echo < /dev/tty 2>/dev/null || true
  printf '\n' > /dev/tty
  printf '%s' "$_answer"
}

usage() {
  cat <<'EOF'
usage: aes67.sh [options]

Installs Digisynthetic's AES67 virtual sound card on a campus player, so the
sound goes onto the network instead of staying inside the HDMI picture.

  --package PATH        the vendor's zip, already on this machine
  --url URL             where to download it from (or VSC_URL)
  --sha256 SUM          expected digest of that zip (empty to skip the check)
  --with-player         install or update the player first, in one go

  --license-file PATH   vendor licence file to install (or AES67_LICENSE_FILE)
  --license-code CODE   activation code, for online activation instead
                        (or AES67_LICENSE_CODE)

  --iface NAME          network interface for AES67 (default: default route)
  --sample-rate N       48000 (default), 96000 or 192000 — the card does no
                        other rate, and the licence has to allow it
  --channels N          channel count, default 8, limited by the licence
  --buf-ms N            buffer in milliseconds, 2-8, default 8
  --ptp-domain N        PTP domain, default 0
  --low-latency         ask the daemon for its low-latency poll mode

  --no-dkms             do not register the module with DKMS, which means a
                        kernel update has to be followed by a manual rebuild
  --no-player-config    install the card but leave the player's own settings
                        alone, so it is not pointed at the new card
  --install-tools       apt-get install alsa-utils, for the test tone
  --dry-run             print what would happen and change nothing
  -y, --yes             take the defaults rather than asking anything
  --help                this

Every option that takes a value can also be passed as VAR=value in the
environment, so this can be run without a terminal.
EOF
}

while [ $# -gt 0 ]; do
  case "$1" in
    --package)          PACKAGE="${2:-}"; shift 2 ;;
    --package=*)        PACKAGE="${1#*=}"; shift ;;
    --url)              VSC_URL="${2:-}"; shift 2 ;;
    --url=*)            VSC_URL="${1#*=}"; shift ;;
    --sha256)           VSC_SHA256="${2:-}"; shift 2 ;;
    --sha256=*)         VSC_SHA256="${1#*=}"; shift ;;
    --with-player)      WITH_PLAYER=1; shift ;;
    --license-file)     AES67_LICENSE_FILE="${2:-}"; shift 2 ;;
    --license-file=*)   AES67_LICENSE_FILE="${1#*=}"; shift ;;
    --license-code)     AES67_LICENSE_CODE="${2:-}"; shift 2 ;;
    --license-code=*)   AES67_LICENSE_CODE="${1#*=}"; shift ;;
    --iface)            AES67_IFACE="${2:-}"; shift 2 ;;
    --iface=*)          AES67_IFACE="${1#*=}"; shift ;;
    --sample-rate)      AES67_SAMPLE_RATE="${2:-}"; shift 2 ;;
    --sample-rate=*)    AES67_SAMPLE_RATE="${1#*=}"; shift ;;
    --channels)         AES67_CHANNELS="${2:-}"; shift 2 ;;
    --channels=*)       AES67_CHANNELS="${1#*=}"; shift ;;
    --buf-ms)           AES67_BUF_MS="${2:-}"; shift 2 ;;
    --buf-ms=*)         AES67_BUF_MS="${1#*=}"; shift ;;
    --ptp-domain)       AES67_PTP_DOMAIN="${2:-}"; shift 2 ;;
    --ptp-domain=*)     AES67_PTP_DOMAIN="${1#*=}"; shift ;;
    --low-latency)      AES67_LOW_LATENCY=1; shift ;;
    --no-dkms)          NO_DKMS=1; shift ;;
    --no-player-config) NO_PLAYER_CONFIG=1; shift ;;
    --install-tools)    INSTALL_TOOLS=1; shift ;;
    --dry-run)          DRY_RUN=1; shift ;;
    -y|--yes)           ASSUME_YES=1; shift ;;
    -h|--help)          usage; exit 0 ;;
    *) printf 'aes67.sh: unknown option: %s\n\n' "$1" >&2; usage >&2; exit 2 ;;
  esac
done

# ── The report ───────────────────────────────────────────────────────────────
# A test build that fails on somebody else's hardware is only worth anything if
# it brings back something to read, so every check is recorded and the whole
# run is left in one file to send back.
OUT_DIR="${OUT_DIR:-/var/tmp/multisite-aes67-$(date +%Y%m%d-%H%M%S)}"
REPORT=""
PASSES=0; FAILS=0; SKIPS=0

pass()   { PASSES=$((PASSES+1));   record "PASS|$*"; printf '  \033[1;32mPASS\033[0m  %s\n' "$*"; }
fail()   { FAILS=$((FAILS+1));     record "FAIL|$*"; printf '  \033[1;31mFAIL\033[0m  %s\n' "$*"; }
skip()   { SKIPS=$((SKIPS+1));     record "SKIP|$*"; printf '  \033[1;33mSKIP\033[0m  %s\n' "$*"; }
empty_() { record "EMPTY|$*";      printf '  \033[1;35mEMPTY\033[0m %s\n' "$*"; }
record() { [ -n "$REPORT" ] || return 0; printf '%s\n' "$*" >> "$REPORT"; }

section() {
  printf '\n\n=== %s ===\n' "$*" >> "${REPORT:-/dev/null}"
  say "$*"
}

# The report is opened here so that everything from the first section onwards
# is captured. A box where /var/tmp cannot be written is not something to stop
# for: the install still runs, and REPORT stays empty, which makes every record
# and section call a no-op.
if [ "$DRY_RUN" -eq 0 ] && mkdir -p "$OUT_DIR" 2>/dev/null; then
  REPORT="$OUT_DIR/report.txt"
  {
    printf 'multisite AES67 install report\n'
    printf 'date:    %s\n' "$(date -Is 2>/dev/null || date)"
    printf 'host:    %s\n' "$(hostname 2>/dev/null || echo unknown)"
    printf 'kernel:  %s\n' "$(uname -r)"
    printf 'arch:    %s\n' "$(uname -m)"
    printf 'package: %s\n' "$VSC_ZIP_NAME"
    printf 'sha256:  %s\n' "${VSC_SHA256:-<not checked>}"
  } > "$REPORT" 2>/dev/null || REPORT=""
fi

# A check that ran a command and now has to decide what that command's silence
# meant. A command that exits 0 and prints nothing has told us nothing, and a
# pipeline ending in `tail` reports tail's status rather than the real
# command's — so PASS is only claimed when something actually came back. A
# false PASS on a check reading "the driver is loaded" is worse than no check
# at all, because it sends somebody to a church to look at the wrong thing.
#
#   check <label> <timeout-seconds> <command>
check() {
  _label="$1"; _tmo="$2"; _cmd="$3"
  _out=""; _rc=0
  printf '\n--- %s ---\n$ %s\n' "$_label" "$_cmd" >> "${REPORT:-/dev/null}"
  if have timeout; then
    _out="$(timeout "$_tmo" sh -c "$_cmd" 2>&1)" || _rc=$?
  else
    _out="$(sh -c "$_cmd" 2>&1)" || _rc=$?
  fi
  printf '%s\n' "$_out" >> "${REPORT:-/dev/null}"

  if [ "$_rc" -ne 0 ]; then
    fail "$_label"
  elif [ -z "$(printf '%s' "$_out" | tr -d '[:space:]')" ]; then
    empty_ "$_label"
  elif printf '%s' "$_out" | grep -qiE '(command not found|no such file or directory|permission denied|operation not permitted|invalid argument)'; then
    fail "$_label — $(printf '%s' "$_out" | head -1)"
  else
    pass "$_label"
  fi
  [ -n "$_out" ] && printf '%s\n' "$_out" | sed 's/^/      /'
  return 0
}

# ═════════════════════════════════════════════════════════════════════════════
# Preflight
# ═════════════════════════════════════════════════════════════════════════════

# In a dry run on a laptop — a reasonable thing to do before driving to a church
# — the platform checks are reported rather than fatal, so the whole flow can be
# walked through without the hardware. On a real run they are fatal, because
# building a Pi's kernel module on the wrong machine is not a thing to attempt
# carefully.
fatal() {
  if [ "$DRY_RUN" -eq 1 ]; then
    warn "$1"
    warn "carrying on because this is a dry run"
    return 0
  fi
  die "$1"
}

# Reading a file that may not exist. `2>/dev/null` on the command inside does
# not suppress the shell's own complaint when it cannot open the file for the
# redirection, so the readable test comes first and nothing is attempted when it
# fails — which matters on the machines this gets run on by mistake.
read_file() {
  [ -r "$1" ] || return 0
  tr -d '\0' < "$1" 2>/dev/null | head -c 200
}

section "What this machine is"

MODEL="$(read_file /sys/firmware/devicetree/base/model)"
MODEL="${MODEL:-unknown machine}"
KERNEL="$(uname -r)"
note "machine: ${MODEL% }"
note "architecture: $VSC_ARCH"
note "kernel: $KERNEL"

if [ "$(id -u)" -ne 0 ]; then
  fatal "not running as root. Run this with sudo."
fi

case "$VSC_ARCH" in
  aarch64|arm64) ;;
  *)
    # The vendor publishes a package per architecture. The one this was written
    # against is aarch64; on anything else the file name will not match unless
    # --package or VSC_URL points at the right one.
    fatal "the vendor's package is published per architecture and this box is $VSC_ARCH. Pass --package or VSC_URL with the $VSC_ARCH package if the vendor has one."
    ;;
esac

# A box that has never had the player on it needs the player first: this script
# adds AES67 audio to a player, it is not a replacement for one. Offering both
# in one go is worth having, because a test build that needs two commands and a
# reboot between them is a test build that gets half done.
PLAYER_BIN="/usr/local/bin/multisite-player"
if [ ! -x "$PLAYER_BIN" ]; then
  if [ "$WITH_PLAYER" -eq 1 ]; then
    section "Installing the player first"
    if [ "$DRY_RUN" -eq 1 ]; then
      note "would fetch and run $RAW_URL"
    else
      have curl || die "curl is needed to fetch the player installer."
      curl -fsSL --retry 5 "$RAW_URL" > /var/tmp/multisite-install.sh \
        || die "could not fetch the player installer."
      bash /var/tmp/multisite-install.sh
    fi
  else
    warn "The player is not installed on this box."
    warn "This script adds AES67 audio to a player; it does not install one."
    warn "Either run it again with --with-player, or install the player first:"
    warn "    curl -fsSL $RAW_URL | sudo bash"
    fatal "no player found at $PLAYER_BIN"
  fi
else
  note "player: $("$PLAYER_BIN" --version 2>/dev/null || echo 'installed')"
fi

# ═════════════════════════════════════════════════════════════════════════════
# Kernel headers
# ═════════════════════════════════════════════════════════════════════════════
# The driver is compiled here, against the kernel that is running, because a
# module built against any other kernel will not load. This is the step the
# vendor's own instructions name as the usual failure, and on a Pi it has a
# particular shape: the headers in the Raspberry Pi OS archive are built for the
# kernel the archive ships, so a box running a kernel from rpi-update, or one
# that has updated but not yet rebooted, has no matching headers at all. That
# deserves one clear paragraph rather than a compiler error.
section "Kernel headers for $KERNEL"

KBUILD="/lib/modules/$KERNEL/build"

headers_ready() { [ -d "$KBUILD" ]; }

if headers_ready; then
  note "already present: $KBUILD"
elif [ "$DRY_RUN" -eq 1 ]; then
  note "not present yet: $KBUILD"
  note "would apt-get install linux-headers-$KERNEL, then raspberrypi-kernel-headers"
else
  export DEBIAN_FRONTEND=noninteractive
  have apt-get || die "no apt-get here, and no kernel headers at $KBUILD. Install the headers for kernel $KERNEL by hand and run this again."

  apt-get update -qq || warn "apt-get update failed — carrying on to see whether it was needed"

  # The exactly-matching package first, because it is the only one that is right.
  apt-get install -y --no-install-recommends "linux-headers-$KERNEL" >/dev/null 2>&1 || true
  if ! headers_ready; then
    # Raspberry Pi OS ships its kernel headers under this name instead, which
    # matches only when the box is running the kernel the archive ships.
    warn "no linux-headers-$KERNEL package — trying raspberrypi-kernel-headers"
    apt-get install -y --no-install-recommends raspberrypi-kernel-headers >/dev/null 2>&1 || true
  fi
fi

if headers_ready; then
  note "using $KBUILD"
elif [ "$DRY_RUN" -eq 0 ]; then
  warn ""
  warn "There are no headers for kernel $KERNEL, so the driver cannot be built,"
  warn "and there is no way around that from here. It usually means one of:"
  warn ""
  warn "  * this box is running a kernel apt does not have, typically after"
  warn "    rpi-update. Reboot into the stock kernel and run this again."
  warn "  * a kernel update is installed but the box has not rebooted. Reboot,"
  warn "    then run this again."
  warn "  * the headers are simply not installed. See what is available with:"
  warn "        apt-cache search linux-headers | grep -i rpi"
  warn ""
  warn "The vendor's own build script stops here too, for the same reason, which"
  warn "is how you know it is the kernel and not this script."
  die "cannot build a module for kernel $KERNEL without its headers"
fi
# ═════════════════════════════════════════════════════════════════════════════
# The vendor's package
# ═════════════════════════════════════════════════════════════════════════════
section "Getting the vendor's package"

sha256_of() {
  if have sha256sum; then sha256sum "$1" | awk '{print $1}'
  elif have shasum;  then shasum -a 256 "$1" | awk '{print $1}'
  else printf ''
  fi
}

# The package is checked before anything is built out of it, because we are
# about to load the result into the running kernel. An empty VSC_SHA256 is
# allowed deliberately, so that a newer package from the vendor does not mean
# editing this script — but it then says out loud that nothing was checked.
verify_package() {
  _file="$1"
  [ -f "$_file" ] || die "no such file: $_file"
  if [ -z "$VSC_SHA256" ]; then
    warn "not checking the digest of $_file, because VSC_SHA256 is empty"
    return 0
  fi
  _got="$(sha256_of "$_file")"
  if [ -z "$_got" ]; then
    warn "no sha256sum and no shasum on this box, so the digest was not checked"
    return 0
  fi
  if [ "$_got" != "$VSC_SHA256" ]; then
    warn "expected $VSC_SHA256"
    warn "      got $_got"
    die "$_file is not the package this script was written against, so do not build a kernel module out of it. If the vendor has published a newer one, check its digest yourself and pass it as --sha256."
  fi
  note "digest matches"
}

VSC_ZIP=""
SRC_UNPACKED=""

if [ -n "$PACKAGE" ] && [ -d "$PACKAGE" ]; then
  # A directory rather than a zip: accept it if it is already unpacked, which
  # is what it looks like after somebody has followed the vendor's README by
  # hand — and that is a reasonable way to test this.
  if [ -f "$PACKAGE/DigiAes67KoLib/Digisyn-vSndCard.c" ]; then
    SRC_UNPACKED="$PACKAGE"
  elif [ -f "$PACKAGE/linux-vsc-$VSC_ARCH-$VSC_VERSION/DigiAes67KoLib/Digisyn-vSndCard.c" ]; then
    SRC_UNPACKED="$PACKAGE/linux-vsc-$VSC_ARCH-$VSC_VERSION"
  else
    die "$PACKAGE is a directory but does not look like the vendor's package — there is no DigiAes67KoLib/Digisyn-vSndCard.c inside it."
  fi
  note "using the unpacked source already at $SRC_UNPACKED"
elif [ -z "$PACKAGE" ] && [ -z "$VSC_URL" ]; then
  # No package and no link. There is deliberately no guessed download URL in
  # this script: the vendor hands out a particular link, and inventing one would
  # be worse than asking.
  warn "No package was given and there is no download link to use."
  ANSWER="$(ask "Path to the vendor's zip, or its download URL (blank to stop)" "")"
  case "$ANSWER" in
    "")                  die "nothing to install. Get $VSC_ZIP_NAME from the vendor, then run this again with --package /path/to/it." ;;
    http://*|https://*)  VSC_URL="$ANSWER" ;;
    *)                   PACKAGE="$ANSWER" ;;
  esac
fi

if [ -z "$SRC_UNPACKED" ] && [ -n "$PACKAGE" ]; then
  note "package: $PACKAGE"
  verify_package "$PACKAGE"
  VSC_ZIP="$PACKAGE"
fi

if [ -z "$SRC_UNPACKED" ] && [ -n "$VSC_URL" ]; then
  note "downloading from $VSC_URL"
  if [ "$DRY_RUN" -eq 1 ]; then
    note "would download to $BUILD_DIR/$VSC_ZIP_NAME and check the digest"
    # Set it even though nothing is downloaded, so the message in the next
    # section names the file instead of printing an empty path.
    VSC_ZIP="$BUILD_DIR/$VSC_ZIP_NAME"
  else
    have curl || die "curl is needed to download the package. Put the zip on this box and use --package instead."
    mkdir -p "$BUILD_DIR"
    curl -fsSL --retry 5 "$VSC_URL" > "$BUILD_DIR/$VSC_ZIP_NAME" \
      || die "could not download $VSC_URL. Their download host is not always reachable from a campus network — put the zip on a USB stick and use --package."
    note "downloaded $(du -h "$BUILD_DIR/$VSC_ZIP_NAME" | awk '{print $1}')"
    verify_package "$BUILD_DIR/$VSC_ZIP_NAME"
    VSC_ZIP="$BUILD_DIR/$VSC_ZIP_NAME"
  fi
fi

# ═════════════════════════════════════════════════════════════════════════════
# Building the kernel module
# ═════════════════════════════════════════════════════════════════════════════
# The vendor's own buildKo.sh is used rather than a make call written here. It
# is their supported path, it already cleans before building, and it prints
# their own instructions when the kernel headers are missing — so a failure
# here looks the same as a failure in their documentation, which is what makes
# a support conversation possible.
section "Building the module for $KERNEL"

if [ -z "$SRC_UNPACKED" ]; then
  if [ "$DRY_RUN" -eq 1 ]; then
    note "would unpack $VSC_ZIP into $BUILD_DIR and run buildKo.sh"
    SRC_DIR_UNPACKED="$BUILD_DIR/linux-vsc-$VSC_ARCH-$VSC_VERSION"
  else
    have unzip || apt-get install -y --no-install-recommends unzip >/dev/null 2>&1 || true
    mkdir -p "$BUILD_DIR"
    rm -rf "$BUILD_DIR/linux-vsc-$VSC_ARCH-$VSC_VERSION"
    if have unzip; then
      unzip -q -o "$VSC_ZIP" -d "$BUILD_DIR" || die "could not unpack $VSC_ZIP"
    elif have python3; then
      # A box with no unzip is unusual but not impossible, and python3 is
      # already a dependency elsewhere in this project.
      python3 -c 'import sys,zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' \
        "$VSC_ZIP" "$BUILD_DIR" || die "could not unpack $VSC_ZIP"
    else
      die "neither unzip nor python3 is available here to unpack $VSC_ZIP"
    fi
    SRC_DIR_UNPACKED="$BUILD_DIR/linux-vsc-$VSC_ARCH-$VSC_VERSION"
    if [ ! -f "$SRC_DIR_UNPACKED/DigiAes67KoLib/Digisyn-vSndCard.c" ]; then
      # The directory inside the zip carries the version in its name, so if the
      # vendor changes their naming this is where it is noticed.
      SRC_DIR_UNPACKED="$BUILD_DIR/$(cd "$BUILD_DIR" && ls -d */ 2>/dev/null | head -1)"
    fi
    [ -f "$SRC_DIR_UNPACKED/DigiAes67KoLib/Digisyn-vSndCard.c" ] \
      || die "$VSC_ZIP unpacked, but there is no DigiAes67KoLib/Digisyn-vSndCard.c inside it. Check that you have the Linux package and not the Windows one."
    note "unpacked to $SRC_DIR_UNPACKED"
  fi
else
  SRC_DIR_UNPACKED="$SRC_UNPACKED"
fi

KO_PATH="$SRC_DIR_UNPACKED/Digisyn-vSndCard.ko"

if [ "$DRY_RUN" -eq 1 ]; then
  note "would run: cd $SRC_DIR_UNPACKED && ./buildKo.sh clean && ./buildKo.sh"
  note "would install the result to /lib/modules/$KERNEL/extra/"
else
  [ -f "$SRC_DIR_UNPACKED/buildKo.sh" ] || die "the vendor's buildKo.sh is missing from the package"
  [ -x "$SRC_DIR_UNPACKED/buildKo.sh" ] || chmod +x "$SRC_DIR_UNPACKED/buildKo.sh" 2>/dev/null || true

  # Building out-of-tree modules as root, after the vendor's own clean, so that
  # a stale object file from an earlier kernel cannot be linked into this one.
  # This is the step that takes real time on a Pi.
  _built=0
  ( cd "$SRC_DIR_UNPACKED" && ./buildKo.sh clean ) >/dev/null 2>&1 || true
  if ( cd "$SRC_DIR_UNPACKED" && ./buildKo.sh ); then
    _built=1
  fi
  if [ "$_built" -ne 1 ]; then
    warn ""
    warn "The driver did not compile. The compiler output is just above."
    warn "That output is what the vendor will need in order to help, because it"
    warn "names the kernel interface this driver and this kernel disagree about."
    warn "The whole run is saved in $OUT_DIR."
    die "the vendor's kernel module failed to build"
  fi

  if [ ! -f "$KO_PATH" ]; then
    # buildKo.sh copies the module to the package root, which is the same place
    # here; this only matters if the vendor moves it.
    _found="$(find "$SRC_DIR_UNPACKED" -maxdepth 2 -name 'Digisyn-vSndCard.ko' 2>/dev/null | head -1)"
    [ -n "$_found" ] || die "buildKo.sh reported success but produced no Digisyn-vSndCard.ko"
    KO_PATH="$_found"
  fi
  note "built $KO_PATH"

  mkdir -p "/lib/modules/$KERNEL/extra"
  install -m 0644 "$KO_PATH" "/lib/modules/$KERNEL/extra/Digisyn-vSndCard.ko"
  depmod -a "$KERNEL" 2>/dev/null || depmod -a || warn "depmod failed — the module may not load by name"
  note "installed to /lib/modules/$KERNEL/extra/"

  # The vendor's own documented way of loading it at boot, kept as theirs so
  # that the two sets of instructions continue to agree with each other.
  printf '%s\n' "$MODULE_NAME" > "$MODULE_LOAD_CONF"
  note "set to load at boot via $MODULE_LOAD_CONF"
fi

# ═════════════════════════════════════════════════════════════════════════════
# DKMS
# ═════════════════════════════════════════════════════════════════════════════
# Where this departs from the vendor's instructions, and why: their README says
# that a kernel update means carrying out the build steps again by hand. On a
# campus box that nobody logs into, that is the sound silently disappearing one
# morning after an unattended upgrade, with a line in the journal as the only
# clue — and the screen would carry on showing a picture, because that is a
# different part of the player altogether. DKMS makes the rebuild part of the
# kernel upgrade instead of something somebody has to remember.
section "Making kernel updates rebuild it"

if [ "$NO_DKMS" -eq 1 ]; then
  warn "skipped, because --no-dkms was given."
  warn "After a kernel update you must run this script again, or the sound stops."
elif [ "$DRY_RUN" -eq 1 ]; then
  note "would install dkms, copy the source to $DKMS_SRC with a dkms.conf,"
  note "and run: dkms install -m $DKMS_NAME -v $DKMS_VERSION"
else
  export DEBIAN_FRONTEND=noninteractive
  have dkms || apt-get install -y --no-install-recommends dkms >/dev/null 2>&1 || true

  if have dkms; then
    # The module source is kept under /usr/src in the layout DKMS expects, with
    # the vendor's own Makefile inside it — which is already a correct
    # external-module makefile, so nothing there needs adapting.
    rm -rf "$DKMS_SRC"
    mkdir -p "$DKMS_SRC"
    cp -a "$SRC_DIR_UNPACKED/DigiAes67KoLib/." "$DKMS_SRC/"

    cat > "$DKMS_SRC/dkms.conf" <<EOF
# Written by scripts/player/aes67.sh. The module itself is Digisynthetic's;
# see the header of Digisyn-vSndCard.c for its licence and provenance.
PACKAGE_NAME="$DKMS_NAME"
PACKAGE_VERSION="$DKMS_VERSION"
BUILT_MODULE_NAME[0]="$MODULE_NAME"
DEST_MODULE_LOCATION[0]="/extra"
MAKE="make -C \${kernel_source_dir} M=\${dkms_tree}/\${PACKAGE_NAME}/\${PACKAGE_VERSION}/build modules"
CLEAN="make -C \${kernel_source_dir} M=\${dkms_tree}/\${PACKAGE_NAME}/\${PACKAGE_VERSION}/build clean"
AUTOINSTALL="yes"
EOF
    note "source kept in $DKMS_SRC"

    if dkms add -m "$DKMS_NAME" -v "$DKMS_VERSION" >/dev/null 2>&1; then
      note "registered with DKMS as $DKMS_NAME/$DKMS_VERSION"
    else
      # Re-running this script lands here, because it is already registered.
      # That is the expected path rather than a failure.
      note "already registered with DKMS"
    fi

    if dkms install -m "$DKMS_NAME" -v "$DKMS_VERSION" --force >/dev/null 2>&1; then
      note "DKMS built and installed it for $KERNEL"
      note "it will rebuild itself for every kernel this box is given"
    else
      warn "DKMS could not build it, though the module built by hand just above."
      warn "The driver is installed and working; what is missing is only the"
      warn "automatic rebuild after a kernel update. Look at:"
      warn "    sudo dkms status"
      warn "    sudo dkms install -m $DKMS_NAME -v $DKMS_VERSION --force"
    fi
  else
    warn "dkms cannot be installed here, so the automatic rebuild is not set up."
    warn "After a kernel update, run this script again — otherwise the module is"
    warn "gone and the player is silent, with a picture still on the screen."
  fi
fi

# ═════════════════════════════════════════════════════════════════════════════
# The daemon
# ═════════════════════════════════════════════════════════════════════════════
# The card on its own transmits nothing. DigiAes67Proc is what reads and writes
# the shared buffers, does the RTP and the PTP, and holds the licence, so the
# two are installed together or not at all.
section "Installing the daemon"

if [ "$DRY_RUN" -eq 1 ]; then
  note "would install $SRC_DIR_UNPACKED/DigiAes67Proc/DigiAes67Proc to $DAEMON"
else
  _proc_src="$SRC_DIR_UNPACKED/DigiAes67Proc/DigiAes67Proc"
  [ -f "$_proc_src" ] || die "the package has no DigiAes67Proc/DigiAes67Proc in it"
  install -m 0755 "$_proc_src" "$DAEMON"
  note "installed $DAEMON ($(du -h "$DAEMON" | awk '{print $1}'))"

  # The vendor's binary is what does the licence checking, so the version it
  # reports is worth recording in the report: it is the first thing they ask.
  _ver="$("$DAEMON" --help 2>&1 | head -1 || true)"
  if [ -n "$_ver" ]; then
    note "it reports: $_ver"
  fi
fi

# ── The licence ──────────────────────────────────────────────────────────────
# Two things are true here and both shape this section. The vendor has no
# non-interactive way to activate — their `--setup` is a menu — so their own
# flow is run with the terminal attached rather than fed through a pipe, and
# the *result* is checked afterwards. And the online path posts the machine's
# identity to a fixed address on the internet, which a church network very
# often does not allow, so a licence file is the preferred route and a code is
# the fallback.
section "The licence"

if [ "$DRY_RUN" -eq 1 ]; then
  note "would show the machine's pcId, install the licence, and check it"
else
  mkdir -p "$DAEMON_CONF_DIR"
  chmod 0700 "$DAEMON_CONF_DIR"

  # The pcId is printed first, always, because it is the number the vendor
  # needs in order to issue a licence — and because a licence issued for a
  # different box fails in a way that looks like a bug everywhere except right
  # here. Their own note is that activation and configuration both run as root:
  # a different user reads the hardware identifiers differently and arrives at
  # a different pcId.
  PCID="$("$DAEMON" --show-pcid 2>/dev/null | head -1 || true)"
  if [ -n "$PCID" ]; then
    note "this machine's pcId: $PCID"
    note "quote that to the vendor when asking for a licence"
  else
    warn "the daemon printed no pcId. Find out why with:"
    warn "    sudo $DAEMON --show-pcid"
    warn "On a Pi it is derived from the board's serial number in the device tree."
  fi

  LICENCE_READY=""
  if [ -s "$DAEMON_LICENSE" ]; then
    note "a licence is already installed at $DAEMON_LICENSE"
    LICENCE_READY=1
  fi

  # Ask what the vendor gave you, unless it was passed in. Neither answer is
  # echoed, and neither is written anywhere this script keeps a copy of.
  if [ -z "$LICENCE_READY" ] && [ -z "$AES67_LICENSE_FILE" ] && [ -z "$AES67_LICENSE_CODE" ]; then
    note ""
    note "The card will not carry audio without a licence."
    note "If the vendor sent a licence file, give its path."
    note "If they sent an activation code instead, leave this blank."
    ANSWER_LIC="$(ask "Licence file path (blank for an activation code)" "")"
    if [ -n "$ANSWER_LIC" ]; then
      AES67_LICENSE_FILE="$ANSWER_LIC"
    elif [ "$ASSUME_YES" -eq 0 ]; then
      AES67_LICENSE_CODE="$(ask_secret "Activation code")"
    fi
  fi

  if [ -z "$LICENCE_READY" ] && [ -n "$AES67_LICENSE_FILE" ]; then
    [ -f "$AES67_LICENSE_FILE" ] || die "no licence file at $AES67_LICENSE_FILE"

    # Their import path is used rather than copying the file to _li, because
    # what lands in _li is their format and there is no way to know from
    # outside whether importing does more than a copy. Their flow is run with a
    # terminal attached; where there is no terminal, the file is copied as a
    # first attempt and the status check further down decides whether it took.
    #
    # Their settings prompts appear after a successful import, and the settings
    # this script wants are written in full a few sections further on. Whatever
    # is answered here is therefore replaced, so the answers only have to be
    # valid — they do not have to be the ones we want.
    if [ -r /dev/tty ]; then
      note ""
      note "Their setup flow is about to run. Choose 1 (import licence file), and"
      note "give it this path:"
      note "    $AES67_LICENSE_FILE"
      note ""
      note "If it then asks for settings, any valid answers will do — this script"
      note "writes its own in the next few steps, using:"
      note "    $AES67_CHANNELS channels at $AES67_SAMPLE_RATE Hz, ${AES67_BUF_MS} ms buffer"
      note ""
      ( exec < /dev/tty > /dev/tty 2>&1; "$DAEMON" --setup ) \
        || warn "their setup flow exited with an error"
    else
      install -m 0600 "$AES67_LICENSE_FILE" "$DAEMON_LICENSE" \
        && note "licence file copied to $DAEMON_LICENSE" \
        || warn "could not copy the licence into place"
    fi
    [ -s "$DAEMON_LICENSE" ] && LICENCE_READY=1
  fi

  if [ -z "$LICENCE_READY" ] && [ -n "$AES67_LICENSE_CODE" ]; then
    if [ -r /dev/tty ]; then
      note ""
      note "Their setup flow is about to run. Choose 2 (activate online with code)"
      note "and paste the code at the prompt, then give any valid answers to the"
      note "settings it asks for — this script writes its own immediately after."
      note ""
      note "That activation reaches a fixed address on the internet. A network that"
      note "blocks outbound traffic needs a licence file from the vendor instead."
      note ""
      ( exec < /dev/tty > /dev/tty 2>&1; "$DAEMON" --setup ) \
        || warn "their setup flow exited with an error"
    else
      warn "no terminal here to run their activation flow on."
      warn "Run it by hand afterwards, and this script can check the result:"
      warn "    sudo $DAEMON --setup"
    fi
    [ -s "$DAEMON_LICENSE" ] && LICENCE_READY=1
  fi

  if [ -z "$LICENCE_READY" ]; then
    warn ""
    warn "No licence is installed, so the card will load but will not carry audio."
    warn "That is a reasonable state to leave this in while waiting for the vendor:"
    warn "everything else is in place, and when the licence arrives it is one command"
    warn "to finish the job, which also asks for the settings again:"
    warn "    sudo $DAEMON --setup      # choose 1, import licence file"
    warn ""
    warn "Use the settings this script chose, since it writes them into the config"
    warn "in the next steps and into the player's settings after that:"
    warn "    $AES67_CHANNELS channels at $AES67_SAMPLE_RATE Hz, buffer"
    warn "    ${AES67_BUF_MS} ms, PTP domain $AES67_PTP_DOMAIN, interface $AES67_IFACE"
    warn ""
    warn "Ask the vendor for a licence against this pcId: ${PCID:-<see --show-pcid>}"
  fi
fi
# ═════════════════════════════════════════════════════════════════════════════
# How the stream is configured
# ═════════════════════════════════════════════════════════════════════════════
section "Choosing the interface and rate"

# The interface has to be one with an IPv4 address on it, because the daemon
# binds to that address rather than to the interface as such. On a Pi with one
# network connection the default route is the right answer and there is nothing
# to decide; on a box with a separate audio network there is, and that is what
# --iface is for.
if [ -z "$AES67_IFACE" ] && [ "$DRY_RUN" -eq 0 ]; then
  AES67_IFACE="$(ip -4 route show default 2>/dev/null | awk '/^default/ {for(i=1;i<=NF;i++) if ($i=="dev") {print $(i+1); exit}}')"
fi
if [ -z "$AES67_IFACE" ] && [ "$DRY_RUN" -eq 0 ]; then
  # No default route at all: a box on an isolated audio network is a legitimate
  # arrangement, so the interfaces that do have an address are offered.
  warn "no default route, so no interface could be chosen automatically"
  warn "here is what is available:"
  ip -4 -o addr show 2>/dev/null | awk '{split($4,a,"/"); printf "        %s  %s\n", $2, a[1]}' || true
  AES67_IFACE="$(ask "Interface for AES67" "eth0")"
fi
AES67_IFACE="${AES67_IFACE:-eth0}"

if [ "$DRY_RUN" -eq 0 ]; then
  if [ ! -e "/sys/class/net/$AES67_IFACE" ]; then
    die "there is no interface called $AES67_IFACE on this box. The daemon will not start with one that does not exist, and it will not tell you why."
  fi
  AES67_BIND_IP="$(ip -4 -o addr show dev "$AES67_IFACE" 2>/dev/null | awk '{split($4,a,"/"); print a[1]; exit}')"
  if [ -z "$AES67_BIND_IP" ]; then
    warn "interface $AES67_IFACE has no IPv4 address on it."
    warn "The daemon binds to the address, and PTP needs the interface to be up."
    warn "If this is the audio network, give it a static address first."
  else
    note "$AES67_IFACE, address $AES67_BIND_IP"
  fi
else
  note "would bind AES67 to $AES67_IFACE"
fi

case "$AES67_SAMPLE_RATE" in
  48000|96000|192000) ;;
  *) die "the card does 48000, 96000 and 192000 Hz and nothing else. $AES67_SAMPLE_RATE was asked for." ;;
esac
case "$AES67_BUF_MS" in
  0|2|3|4|5|6|7|8) ;;
  *) die "the buffer has to be 0 or between 2 and 8 milliseconds. $AES67_BUF_MS was asked for." ;;
esac
case "$AES67_CHANNELS" in
  ''|*[!0-9]*) die "the channel count has to be a number; $AES67_CHANNELS was given." ;;
esac
[ "$AES67_CHANNELS" -ge 2 ] || die "the card wants at least 2 channels; $AES67_CHANNELS was given."
if [ "$AES67_BUF_MS" -eq 0 ]; then
  warn "bufMs 0 is the vendor's mixing mode, which bypasses the system sound card."
  warn "The player will have no card to open. That is only right on a box where"
  warn "the sound is being mixed somewhere else."
fi
if [ "$AES67_SAMPLE_RATE" != "48000" ]; then
  warn "$AES67_SAMPLE_RATE Hz is not what the rest of this project's audio path is"
  warn "built for — the encoder packs 8 channels of 48 kHz — and the licence has to"
  warn "allow the rate you asked for."
fi
note "$AES67_CHANNELS channels at $AES67_SAMPLE_RATE Hz, ${AES67_BUF_MS} ms buffer, PTP domain $AES67_PTP_DOMAIN"

# ── Writing the daemon's configuration ───────────────────────────────────────
# The format is the vendor's own, taken from the strings their binary writes:
# one key=value per line, `ptpMode` unsigned and the rest plain integers, and
# `bindIf` holding the interface *name* rather than an address — the daemon
# looks the address up itself. Writing it here rather than driving their
# interactive menu means this script can be run unattended, and means the result
# can be checked: --status parses this very file and says whether it is valid.
LOW_LATENCY_VALUE="n"
if [ "$AES67_LOW_LATENCY" -eq 1 ]; then
  LOW_LATENCY_VALUE="y"
fi

if [ "$DRY_RUN" -eq 1 ]; then
  note "would write $DAEMON_CONF:"
  note "    sampleRate=$AES67_SAMPLE_RATE chNum=$AES67_CHANNELS bufMs=$AES67_BUF_MS"
  note "    domain=$AES67_PTP_DOMAIN ptpMode=$AES67_PTP_MODE bindIf=$AES67_IFACE"
  note "    lowLatencyPoll=$LOW_LATENCY_VALUE"
else
  mkdir -p "$DAEMON_CONF_DIR"
  chmod 0700 "$DAEMON_CONF_DIR"

  # Kept aside rather than simply overwritten, so that a box somebody has
  # already tuned by hand can be put back. Only the most recent is kept: this
  # is a convenience, not a backup system.
  if [ -f "$DAEMON_CONF" ]; then
    cp -f "$DAEMON_CONF" "$DAEMON_CONF.before-aes67.sh" 2>/dev/null || true
  fi

  cat > "$DAEMON_CONF" <<EOF
sampleRate=$AES67_SAMPLE_RATE
chNum=$AES67_CHANNELS
bufMs=$AES67_BUF_MS
domain=$AES67_PTP_DOMAIN
ptpMode=$AES67_PTP_MODE
bindIf=$AES67_IFACE
lowLatencyPoll=$LOW_LATENCY_VALUE
EOF
  chmod 0644 "$DAEMON_CONF"
  note "wrote $DAEMON_CONF"

  # Whether that was the right format is not something to assume: --status
  # parses it and prints `config=valid`, or says which field it disliked.
  _status="$("$DAEMON" --status 2>&1 || true)"
  printf '%s\n' "$_status" >> "${REPORT:-/dev/null}"
  if printf '%s' "$_status" | grep -q 'config=valid'; then
    pass "the daemon reads back the configuration as valid"
  else
    fail "the daemon did not accept the configuration written to $DAEMON_CONF"
    warn "$_status"
    warn "That means the file format guessed from the binary is wrong, and it is"
    warn "the one thing here that could not be checked before running it. The"
    warn "daemon's own menu takes the same values:"
    warn "    sudo $DAEMON --setup"
  fi
fi

# ═════════════════════════════════════════════════════════════════════════════
# Loading the module, and the two services
# ═════════════════════════════════════════════════════════════════════════════
section "Starting it up"

# The order here is the whole point, so it is done in order and said out loud:
# the module registers the card, the daemon fills in that card's rate and
# channel count and starts the streams, and only then does the player open it. A
# player that starts first finds a card advertising nothing and refuses it,
# logging `audio out failed` and carrying on silently — which is the failure
# this script exists in order not to leave behind.

if [ "$DRY_RUN" -eq 1 ]; then
  note "would modprobe $MODULE_NAME, install and start $DAEMON_SERVICE,"
  note "then order $PLAYER_UNIT after it and restart the player"
else
  # A module freshly built but already loaded in its old form would otherwise be
  # the copy left in memory. It cannot be unloaded while the daemon has the
  # device open, so the daemon is stopped first — which is why the daemon's unit
  # is written before the module is reloaded.
  if systemctl list-unit-files 2>/dev/null | grep -q "^$DAEMON_SERVICE.service"; then
    systemctl stop "$DAEMON_SERVICE" 2>/dev/null || true
  fi
  if lsmod 2>/dev/null | grep -q "^$MODULE_NAME"; then
    note "unloading the previous version of the module"
    modprobe -r "$MODULE_NAME" 2>/dev/null || warn "could not unload it — something still has it open"
  fi

  modprobe snd     2>/dev/null || true
  modprobe snd-pcm 2>/dev/null || true

  if modprobe "$MODULE_NAME" 2>/dev/null \
     || insmod "/lib/modules/$KERNEL/extra/Digisyn-vSndCard.ko" 2>/dev/null; then
    pass "the module loaded"
  else
    fail "the module would not load"
    warn "What the kernel said about it:"
    dmesg 2>/dev/null | tail -20 | sed 's/^/        /' || true
    warn "A module that builds but will not load is usually one built against a"
    warn "different kernel than the one running. Check which:"
    warn "    modinfo /lib/modules/$KERNEL/extra/Digisyn-vSndCard.ko | head -20"
    die "the module did not load"
  fi

  # /dev/Digisyn_vSndCard is the door the daemon goes through.
  _waited=0
  while [ ! -e /dev/Digisyn_vSndCard ] && [ "$_waited" -lt 5 ]; do
    sleep 1; _waited=$((_waited + 1))
  done
  if [ -e /dev/Digisyn_vSndCard ]; then
    pass "/dev/Digisyn_vSndCard exists"
  else
    fail "/dev/Digisyn_vSndCard did not appear"
    warn "The module loaded but made no device, so the daemon has nothing to"
    warn "talk to. This is the point to ask the vendor: send them the output of"
    warn "    dmesg | tail -40"
    warn "and the report at ${REPORT:-/var/tmp}."
  fi
fi

# ── The daemon's unit ────────────────────────────────────────────────────────
# The vendor's documentation starts the daemon with nohup from a login shell,
# which is not a thing that survives a power cut. This is the same program under
# systemd, where a failure is restarted and a silence has a reason attached to
# it in the journal.
if [ "$DRY_RUN" -eq 1 ]; then
  note "would write /etc/systemd/system/$DAEMON_SERVICE.service"
else
  cat > "/etc/systemd/system/$DAEMON_SERVICE.service" <<EOF
# $DAEMON_SERVICE.service — Digisynthetic's AES67 daemon.
#
# Written by scripts/player/aes67.sh in the obs-multisite repository. Editing
# this by hand is fine, but running that script again replaces it.
#
# The vendor's own instructions run this with nohup from a terminal. On an
# appliance that has to come back from a power cut on its own, this is the same
# program under systemd instead.

[Unit]
Description=Digisynthetic AES67 virtual sound card daemon
Documentation=https://github.com/stageaudioworks/obs-multisite
# The kernel module has to be in before the daemon can open its device, and the
# network has to be up before it can bind its address and find its PTP clock.
After=network-online.target
Wants=network-online.target
StartLimitIntervalSec=0

[Service]
Type=simple
# Loading the module here as well as at boot means the two cannot be started in
# the wrong order by hand, and that a daemon restarted after a module unload
# puts the module back by itself.
ExecStartPre=/sbin/modprobe $MODULE_NAME
ExecStart=$DAEMON
# It holds the licence, writes /etc/DigiAes67Proc, and opens a device node.
# Root is required for all three, exactly as the vendor's instructions say.
User=root
Restart=always
RestartSec=5
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
EOF
  note "wrote /etc/systemd/system/$DAEMON_SERVICE.service"
fi

# ── Telling the player to wait for it ────────────────────────────────────────
# A drop-in rather than an edit to the player's own unit, because the player's
# unit belongs to the player and install.sh rewrites it on every update. A
# drop-in survives that, which matters since this ordering is load-bearing.
if [ "$DRY_RUN" -eq 1 ]; then
  note "would write $PLAYER_DROPIN ordering $PLAYER_UNIT after $DAEMON_SERVICE"
else
  mkdir -p "$PLAYER_DROPIN_DIR"
  cat > "$PLAYER_DROPIN" <<EOF
# Written by scripts/player/aes67.sh. Do not edit here; run that script again.
#
# The sound card has no rate and no channel count of its own: both are read from
# a page of shared memory that $DAEMON_SERVICE fills in. A player that starts
# first opens a card offering zero channels at zero hertz, refuses it, and goes
# silent while still showing a picture. That is what this file prevents.

[Unit]
Wants=$DAEMON_SERVICE.service
After=$DAEMON_SERVICE.service
EOF
  note "wrote $PLAYER_DROPIN"
fi

if [ "$DRY_RUN" -eq 1 ]; then
  note "would enable and start $DAEMON_SERVICE"
else
  systemctl daemon-reload
  systemctl enable --quiet "$DAEMON_SERVICE" 2>/dev/null || true
  systemctl restart "$DAEMON_SERVICE"
  note "started $DAEMON_SERVICE"
fi

# ═════════════════════════════════════════════════════════════════════════════
# Pointing the player at the card
# ═════════════════════════════════════════════════════════════════════════════
section "Pointing the player at the new card"

# The card's name is discovered rather than written down. The driver never calls
# snd_card_new() with an id of its own, so ALSA derives one from the driver's
# shortname, `Digisyn_vSndCard`, and truncates it to ALSA's fifteen-character
# limit — `Digisyn_vSndCar`, missing its last letter. That truncation is a
# property of ALSA, not of this hardware, and it is also a name that changes if
# the vendor ever renames the driver or if another card already holds that id.
# So it is read back from the kernel every time, and never typed in.
card_id_from_proc() {
  awk '
    /^[[:space:]]*[0-9]+[[:space:]]*\[/ {
      if (index($0, "Digisyn") > 0) {
        match($0, /\[[^]]*\]/)
        if (RSTART > 0) {
          # ALSA prints the id left-justified in a 15-wide field — "%2i [%-15s]"
          # — so an id shorter than fifteen characters comes back padded with
          # spaces, and those spaces are not part of the name. The vendor id
          # happens to be exactly fifteen today, which hides this; the padding
          # is trimmed anyway so a renamed or shorter id still yields a name
          # that plughw:CARD= will resolve.
          s = substr($0, RSTART + 1, RLENGTH - 2)
          gsub(/[[:space:]]+$/, "", s)
          if (s != "") { print s; exit }
        }
      }
    }
  ' /proc/asound/cards 2>/dev/null
}

CARD_ID=""
if [ "$DRY_RUN" -eq 0 ]; then
  CARD_ID="$(card_id_from_proc)"
fi

if [ "$DRY_RUN" -eq 1 ]; then
  note "would read the card's name out of /proc/asound/cards and write"
  note "    alsa_device: plughw:CARD=<name>"
  note "into $PLAYER_CONFIG"
elif [ -z "$CARD_ID" ]; then
  fail "the card is not in /proc/asound/cards"
  note "/proc/asound/cards says:"
  cat /proc/asound/cards 2>/dev/null | sed 's/^/        /' || note "        (no /proc/asound/cards at all — is ALSA loaded?)"
  warn "Without the card the player cannot be pointed at it, so its settings are"
  warn "left alone on purpose rather than set to a name that would not resolve."
  warn "Restart the daemon and look again:"
  warn "    sudo systemctl restart $DAEMON_SERVICE"
  warn "    cat /proc/asound/cards"
  warn "If the card still is not there, that is a question for the vendor."
else
  # plughw, not hw: the card accepts nothing but S32_LE, and the player hands
  # over FLOAT_LE. The plug layer converts between them. This is also why
  # `hw:` is not offered as an option anywhere in this script — it cannot work,
  # and a box set up that way fails on the morning of a service.
  ALSA_DEVICE="plughw:CARD=$CARD_ID"
  note "card $CARD_ID → $ALSA_DEVICE"

  if [ "$NO_PLAYER_CONFIG" -eq 1 ]; then
    warn "--no-player-config was given, so the player's settings are untouched."
    warn "To do it by hand, set this in $PLAYER_CONFIG:"
    warn "    \"alsa_device\": \"$ALSA_DEVICE\""
  elif [ ! -f "$PLAYER_CONFIG" ]; then
    warn "there is no $PLAYER_CONFIG, so there is nothing to point at the card"
  elif ! have python3; then
    warn "python3 is not here, so the player's settings were not edited — the"
    warn "only reason to avoid a hand edit is that it would have to preserve the"
    warn "other settings, and doing that without a JSON parser is how settings"
    warn "get lost. Set this by hand in $PLAYER_CONFIG:"
    warn "    \"alsa_device\": \"$ALSA_DEVICE\""
  else
    # The same pattern install.sh uses for the ZeroTier key: read the file,
    # change the one key, write it back. Everything else in it — the room name,
    # the cache folder, the tunnel token — is left exactly as it was.
    _changed="$(python3 - "$PLAYER_CONFIG" "$ALSA_DEVICE" <<'PY'
import json, sys
path, device = sys.argv[1], sys.argv[2]
with open(path) as f:
    cfg = json.load(f)
was = cfg.get("alsa_device", "(unset)")
cfg["alsa_device"] = device
with open(path, "w") as f:
    json.dump(cfg, f, indent=2)
    f.write("\n")
print("alsa_device: %s -> %s" % (was, device))
if not cfg.get("audio_enabled", True):
    print("audio_enabled is false, so the player will still not use the card")
PY
)" || _changed=""
    if [ -n "$_changed" ]; then
      chmod 0600 "$PLAYER_CONFIG"
      printf '%s\n' "$_changed" | sed 's/^/        /'
      pass "the player's settings name the new card"
      case "$_changed" in
        *"audio_enabled is false"*)
          warn "audio_enabled is false in the player's settings, so the sound is"
          warn "still switched off. Turn it on in Settings on the web interface." ;;
      esac
    else
      fail "could not edit $PLAYER_CONFIG"
      warn "Set this by hand, keeping the rest of the file as it is:"
      warn "    \"alsa_device\": \"$ALSA_DEVICE\""
    fi
  fi
fi

# ═════════════════════════════════════════════════════════════════════════════
# Checking it
# ═════════════════════════════════════════════════════════════════════════════
section "Checking what can be checked"

if [ "$DRY_RUN" -eq 1 ]; then
  note "would check the card, the device node, the daemon's status, the licence,"
  note "a real one-second playback through plughw, and the player's own log"
else
  # The kernel's own view of the card: its name, its driver, and the device node
  # the daemon talks to. This is the pair of facts the daemon's status cannot
  # supply and the player's log cannot either.
  check "the card is visible to ALSA" 10 "cat /proc/asound/cards"
  check "the daemon's device node exists" 10 "ls -l /dev/Digisyn_vSndCard"

  # The daemon's own opinion, which parses the configuration file written above
  # and reads the licence. A failure here is the earliest honest sign that
  # something is wrong, and it is the vendor's own words rather than this
  # script's interpretation of them.
  check "the daemon accepts its configuration" 20 "$DAEMON --status"

  # A licence that does not cover the rate or the channel count is a real and
  # specific failure: the daemon says `license chNum=8 is lower than config
  # chNum=16` rather than refusing to start, so the licence line has to be read
  # rather than the exit status trusted.
  _lic="$("$DAEMON" --status 2>&1 | grep -i 'licen' || true)"
  printf 'licence: %s\n' "$_lic" >> "${REPORT:-/dev/null}"
  if [ -z "$_lic" ]; then
    skip "the daemon said nothing about a licence"
  elif printf '%s' "$_lic" | grep -qiE 'invalid|missing|not found|exceed'; then
    fail "the licence is not good for this configuration — $_lic"
    warn "A licence is issued against one machine's pcId and covers one rate and"
    warn "one channel count. Either this licence was issued for a different box,"
    warn "or it does not allow $AES67_CHANNELS channels at $AES67_SAMPLE_RATE Hz."
  else
    pass "the licence covers this configuration"
  fi

  # The one test that exercises the whole path: open the card the way the player
  # will open it, hand it a second of FLOAT_LE, and see whether it is accepted.
  # FLOAT_LE is deliberate — it is what src/appliance/alsa_output.cpp asks for,
  # and the card itself accepts only S32_LE, so this is precisely the plug-layer
  # negotiation the player will do. A failure here is the failure the player
  # would have had.
  if have aplay; then
    check "one second of FLOAT_LE through plughw" 30 \
      "aplay -t raw -D plughw:CARD=$CARD_ID -f FLOAT_LE -r $AES67_SAMPLE_RATE -c $AES67_CHANNELS -d 1 /dev/zero"
  elif [ "$INSTALL_TOOLS" -eq 1 ]; then
    apt-get install -y --no-install-recommends alsa-utils >/dev/null 2>&1 || true
    if have aplay; then
      check "one second of FLOAT_LE through plughw" 30 \
        "aplay -t raw -D plughw:CARD=$CARD_ID -f FLOAT_LE -r $AES67_SAMPLE_RATE -c $AES67_CHANNELS -d 1 /dev/zero"
    else
      skip "aplay is not installed, so the playback test was not run"
    fi
  else
    skip "aplay is not installed; --install-tools would fetch alsa-utils"
    note "the same test by hand is:"
    note "    aplay -D plughw:CARD=$CARD_ID -f FLOAT_LE -r $AES67_SAMPLE_RATE -c $AES67_CHANNELS -d 1 /dev/zero"
  fi

  # And what the card actually offers, so that the ranges are on the record.
  # This is also the only place the card's own limits — S32_LE, one rate, one
  # channel count — can be seen rather than assumed.
  if have aplay; then
    check "what the card says it can do" 20 \
      "aplay -t raw -D plughw:CARD=$CARD_ID --dump-hw-params -f FLOAT_LE -r $AES67_SAMPLE_RATE -c $AES67_CHANNELS -d 1 /dev/zero"
  fi

  # Finally the player itself, because everything above can be right while the
  # player is still pointed somewhere else. It is restarted rather than asked,
  # and then its own log is read: a player that cannot open the card says
  # `audio out failed` and carries on, so the absence of that line is the
  # evidence, not the exit status of anything.
  if [ "$DRY_RUN" -eq 0 ] && systemctl list-unit-files 2>/dev/null | grep -q "^$PLAYER_UNIT.service"; then
    systemctl restart "$PLAYER_UNIT" 2>/dev/null || warn "could not restart $PLAYER_UNIT"
    sleep 6
    _journal="$(journalctl -u "$PLAYER_UNIT" --since '2 minutes ago' --no-pager 2>/dev/null || true)"
    printf '%s\n' "$_journal" >> "${REPORT:-/dev/null}"
    if printf '%s' "$_journal" | grep -qi 'audio out failed'; then
      fail "the player could not open the card"
      printf '%s' "$_journal" | grep -i 'audio out' | tail -3 | sed 's/^/        /'
      warn "This is the failure the plughw: device and the start ordering exist to"
      warn "prevent, so something in that chain is not as expected. The two usual"
      warn "causes are the daemon not being up before the player, and the plug"
      warn "layer offering the card a buffer size it will not take."
    else
      pass "the player started without complaining about the audio"
    fi
    if systemctl is-active --quiet "$PLAYER_UNIT"; then
      pass "the player is running"
    else
      fail "the player is not running"
      journalctl -u "$PLAYER_UNIT" -n 20 --no-pager 2>/dev/null | sed 's/^/        /' || true
    fi
  else
    skip "the player is not installed here, so it was not restarted"
  fi
fi

# ═════════════════════════════════════════════════════════════════════════════
# What this cannot tell you
# ═════════════════════════════════════════════════════════════════════════════
# Every check above is something this box can answer about itself. The ones that
# matter most on the morning of a service are the ones it cannot, and saying so
# plainly here is the difference between an installer and a false sense of
# safety.
section "Done"

if [ "$DRY_RUN" -eq 1 ]; then
  note "This was a dry run: nothing above was done and nothing was changed."
fi

note "$PASSES passed, $FAILS failed, $SKIPS skipped"
[ -n "$REPORT" ] && note "the whole run is recorded in $REPORT"

echo
say "What this has not checked, and cannot from here"
note ""
note "The far end was not part of this run. On the bench the card appeared, the"
note "player opened it and eight channels arrived, so these pieces do work"
note "together — but that is a bench, not a service. Three things to watch, worst"
note "first:"
note ""
note "1. Whether the far end knows where to listen. The vendor's configuration"
note "   flow asks for a rate, a channel count, a buffer, a PTP domain and an"
note "   interface, and never asks for a multicast address, a port or a channel"
note "   map — those are set through their route tool, a separate program on a"
note "   computer. A receiver found the stream on the bench, but aiming it at a"
note "   particular address and port is not something this box does, and a"
note "   receiver that has not been told looks exactly like a broken driver from"
note "   the other end of the building."
note ""
note "2. Lip sync over a long event. The card reports its position from the"
note "   daemon's millisecond counter and the player corrects from snd_pcm_delay()."
note "   Whether that stays truthful over two hours is not something ten seconds of"
note "   test tone can show. Watch a full service on the picture and the sound"
note "   together before trusting it."
note ""
note "3. Whether PTP locks properly. AES67 wants the two ends within a"
note "   millisecond, and a Pi's network interface does not timestamp packets in"
note "   hardware, so the achievable accuracy is whatever the software gets. Watch"
note "   it on the receiver, not here."
echo

if [ "$FAILS" -eq 0 ] && [ "$DRY_RUN" -eq 0 ]; then
  note "If the far end is silent, the first thing to look at is item 1 above."
  echo
fi

note "To see the daemon:      journalctl -u $DAEMON_SERVICE -f"
note "To see the player:      journalctl -u $PLAYER_UNIT -f"
note "To change a setting:    sudo $DAEMON --setup   (then edit $PLAYER_CONFIG"
note "                        if you changed the rate or channel count)"
note "To run this again:      sudo bash scripts/player/aes67.sh"
echo

