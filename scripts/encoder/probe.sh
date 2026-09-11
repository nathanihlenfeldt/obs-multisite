#!/usr/bin/env bash
#
# probe.sh — what can this board really capture and encode?
#
#   curl -fsSL https://raw.githubusercontent.com/stageaudioworks/obs-multisite/main/scripts/encoder/probe.sh | sudo bash
#
# (From a checkout it is simply: sudo bash scripts/encoder/probe.sh)
#
# A bring-up probe, not an installer. It asks the board and its kernel what
# they can do, tries one real capture and one real hardware encode, and writes
# one report to send back. It is the first step of the headless encoder
# appliance in PROJECT-SCOPE.md §8.5: the answers decide which OS that box
# ships on, and therefore most of what comes after.
#
# What it does not do: it installs nothing, edits no configuration, sets no
# EDID, and writes nothing outside its own output directory.
#
# It is deliberately NOT `set -e`. The whole job is to keep going when a probe
# fails, and on a board nobody has run this on yet most of them will. An absent
# tool or an absent signal is a line in the report, not the end of the run, and
# every command that could block is wrapped in a timeout, because nobody is
# watching this run.
#
#   --out DIR             where the report and the captures go
#                         (default /tmp/multisite-encoder-probe-<timestamp>)
#   --frames N            frames captured for the format test (default 30 — a
#                         1080p NV12 frame is 3 MB, and 30 is enough to measure
#                         a frame rate without filling the disk)
#   --encode-seconds N    sustained hardware encode, for the achieved-fps
#                         number and the thermal soak (default 30)
#   --thermal-minutes N   how long to watch temperature under load (default 10)
#   --no-thermal          skip the temperature watch
#   --install-tools       apt-get install v4l-utils and alsa-utils if missing;
#                         without it the script prints the command and carries on
#   --keep-raw            keep the captured frames (they are large)
#   --upload-url URL      PUT the finished tarball here when the run ends
#                         (or set PROBE_UPLOAD_URL) instead of a file drop
#   --no-upload           upload nothing; just write the tarball
#
# ── Getting the report back ───────────────────────────────────────────────────
#
# The board is in someone else's lab, most likely behind the Great Firewall,
# and asking them to find and email a tarball is how reports go missing.
#
# BY DEFAULT the script uploads the tarball to a public file drop and prints
# the link, for whoever ran it to send back. That is one short line instead of
# an attachment, it needs no account at either end, and — the reason it is the
# default rather than a flag — it needs no argument, so it works for a vendor
# who was already given the plain one-line command above.
#
# Two things follow from that, and both should be said to the vendor rather
# than discovered:
#
#   * the board sends a file out. Nothing else in this script touches the
#     network, so this is the exception, and --no-upload turns it off.
#   * the link is unguessable but NOT private. Anyone holding it can read the
#     report: board model, kernel, device lists, and any serial numbers the
#     probes happened to print.
#
# ── On the file drops, before changing the list ───────────────────────────────
#
# These were chosen by trying them, not by reputation, and the obvious
# candidates are the ones that failed. Tested 2026-09-11 from an ordinary
# connection:
#
#   0x0.st          REJECTS UPLOADS. Answers "uploads disabled because it's
#                   been almost nothing but AI botnet spam". Still the first
#                   result everyone reaches for; it does not work.
#   bashupload.com  does not resolve at all any more.
#   temp.sh         answered with an HTML error page rather than a link.
#   oshi.at         failed TLS here — but DO NOT simply re-add it if it comes
#                   back. It replies with a download link AND a management link
#                   that DELETES the file, in that order, and the management
#                   token is only a path segment, so nothing in the URL marks
#                   it as dangerous. Taking the wrong one hands the vendor the
#                   power to delete their own report. Take the first link.
#   litterbox       works, and expires (72h max), which is why it is tried
#                   first. Intermittent: the identical request has both
#                   succeeded and returned "No file!", most likely rate
#                   limiting. Uploads to litterbox.catbox.moe and serves the
#                   result back from litter.catbox.moe — a different host.
#   catbox.moe      works, and is the reliable one, but an anonymous upload
#                   CANNOT be deleted afterwards. Whatever lands there is
#                   public permanently. Fallback for that reason, not first.
#
# Whatever the list becomes, keep the two guards in upload_to_paste(). They are
# not defensive habit; each caught a real failure the first time it ran. curl's
# exit code is checked before its output is read at all, because when the TLS
# handshake against oshi.at failed, the URL inside curl's OWN error message
# matched and the script reported success with a link to curl's documentation —
# a wrong link nobody discovers until the report never arrives. And the link
# must be at the host that service actually serves from, which then immediately
# caught litterbox's two hostnames.
#
# Where a public drop is not acceptable, --upload-url PUTs to a pre-signed URL
# instead and nothing becomes public. Generate it beforehand:
#
#   export MULTISITE_S3_KEY=...  MULTISITE_S3_SECRET=...
#   scripts/encoder/presign.py --endpoint https://ACCOUNT.r2.cloudflarestorage.com \
#       --bucket BUCKET --key encoder-probe/BOARD-DATE.tar.gz
#
# (standard library only, because there is no aws CLI on the machines this is
# run from and a bring-up task should not start by installing a toolchain. It
# prints the whole command to send; --get signs a URL to fetch the report back.)
#
# That is a pre-signed PUT: it needs no account at the other end, no server of
# yours running anywhere, and it carries no credentials — it is permission to
# write one object, under one name, until it expires (seven days is the SigV4
# maximum). Give it to the vendor as part of the command:
#
#   curl -fsSL https://raw.githubusercontent.com/.../probe.sh \
#     | sudo bash -s -- --upload-url 'https://...'
#
# DO NOT commit a URL here or anywhere else in this repository. It is public,
# and a pre-signed URL is a write capability for as long as it lives. One URL
# per board, because the object name is part of what is signed and a second run
# against the same URL overwrites the first.
#
# Reachability is worth one command before a ten-minute run — from the board:
#
#   curl -sS -o /dev/null -w '%{http_code} %{time_total}s\n' -T /dev/null 'URL'
#
# The upload never decides whether the run succeeded. The tarball is always
# written locally and its path always printed, so a blocked or expired URL
# costs nothing but the walk to the machine.
#
# Safe to run again: every run writes a fresh directory. Roughly ten to fifteen
# minutes, most of it the temperature soak.

set -uo pipefail

PROBE_VERSION="1.0"

OUT_DIR=""
FRAMES=30
ENCODE_SECONDS=30
THERMAL_MINUTES=10
DO_THERMAL=1
INSTALL_TOOLS=0
KEEP_RAW=0
# Never defaulted, and never committed with a value: this repository is public
# and a pre-signed URL is a write capability. Env var so it can be passed
# without appearing in the vendor's shell history. When set, it is used instead
# of the paste services below.
UPLOAD_URL="${PROBE_UPLOAD_URL:-}"
# Uploading is ON by default, which is unusual for a script that otherwise
# touches nothing. It is deliberate: the command the vendor was given has no
# arguments, so anything that has to be switched on will never run. --no-upload
# turns it off, and the run is announced before anything leaves the machine.
DO_UPLOAD=1

usage() {
  cat <<'EOF'
usage: probe.sh [options]

  --out DIR             where the report and the captures go (default /tmp)
  --frames N            frames to capture for the format test (default 30)
  --encode-seconds N    sustained hardware encode (default 30)
  --thermal-minutes N   temperature watch under load (default 10)
  --no-thermal          skip the temperature watch
  --install-tools       apt-get install v4l-utils and alsa-utils if missing
  --keep-raw            keep the captured frames (they are large)
  --upload-url URL      PUT the finished tarball here (or set PROBE_UPLOAD_URL)
  --no-upload           do not upload anything; just write the tarball
  --help                this
EOF
}

while [ $# -gt 0 ]; do
  case "$1" in
    --out)               OUT_DIR="${2:-}"; shift 2 ;;
    --out=*)             OUT_DIR="${1#*=}"; shift ;;
    --frames)            FRAMES="${2:-}"; shift 2 ;;
    --frames=*)          FRAMES="${1#*=}"; shift ;;
    --encode-seconds)    ENCODE_SECONDS="${2:-}"; shift 2 ;;
    --encode-seconds=*)  ENCODE_SECONDS="${1#*=}"; shift ;;
    --thermal-minutes)   THERMAL_MINUTES="${2:-}"; shift 2 ;;
    --thermal-minutes=*) THERMAL_MINUTES="${1#*=}"; shift ;;
    --no-thermal)        DO_THERMAL=0; shift ;;
    --install-tools)     INSTALL_TOOLS=1; shift ;;
    --keep-raw)          KEEP_RAW=1; shift ;;
    --upload-url)        UPLOAD_URL="${2:-}"; shift 2 ;;
    --upload-url=*)      UPLOAD_URL="${1#*=}"; shift ;;
    --no-upload)         DO_UPLOAD=0; shift ;;
    -h|--help)           usage; exit 0 ;;
    *) printf 'probe.sh: unknown option: %s\n\n' "$1" >&2; usage >&2; exit 2 ;;
  esac
done

case "$FRAMES" in ''|*[!0-9]*) FRAMES=30 ;; esac
case "$ENCODE_SECONDS" in ''|*[!0-9]*) ENCODE_SECONDS=30 ;; esac
case "$THERMAL_MINUTES" in ''|*[!0-9]*) THERMAL_MINUTES=10 ;; esac

# ── Talking to the person running it ─────────────────────────────────────────

say()  { printf '\n\033[1;36m==>\033[0m \033[1m%s\033[0m\n' "$*"; }
note() { printf '    %s\n' "$*"; }
warn() { printf '\033[1;33m    %s\033[0m\n' "$*"; }

have() { command -v "$1" >/dev/null 2>&1; }

# macOS and some minimal images have no coreutils timeout. Say so once rather
# than pretending commands are bounded when they are not.
HAVE_TIMEOUT=0
have timeout && HAVE_TIMEOUT=1

run_timeout() {
  _t="$1"; shift
  if [ "$HAVE_TIMEOUT" -eq 1 ]; then timeout "$_t" "$@"; else "$@"; fi
}

# ── The report ───────────────────────────────────────────────────────────────

: "${OUT_DIR:=/tmp/multisite-encoder-probe-$(date +%Y%m%d-%H%M%S)}"
mkdir -p "$OUT_DIR" || { printf 'probe.sh: cannot create %s\n' "$OUT_DIR" >&2; exit 1; }
REPORT="$OUT_DIR/report.txt"
RESULTS="$OUT_DIR/.results"
TMPOUT="$OUT_DIR/.try.out"
: > "$RESULTS"

pass() { printf 'PASS|%s\n' "$*" >> "$RESULTS"; printf '  \033[1;32mPASS\033[0m  %s\n' "$*"; }
fail() { printf 'FAIL|%s\n' "$*" >> "$RESULTS"; printf '  \033[1;31mFAIL\033[0m  %s\n' "$*"; }
skip() { printf 'SKIP|%s\n' "$*" >> "$RESULTS"; printf '  \033[1;33mSKIP\033[0m  %s\n' "$*"; }
empty_() { printf 'EMPTY|%s\n' "$*" >> "$RESULTS"; printf '  \033[1;35mEMPTY\033[0m %s\n' "$*"; }

section() {
  printf '\n\n=== %s ===\n' "$*" >> "$REPORT"
  say "$*"
}

# Anything the script works out for itself, rather than by running a command.
record() { printf '%s\n' "$*" >> "$REPORT"; }

# A one- or two-line complaint from a tool that never got to do its job. Only
# short outputs are judged this way: a long kernel log that happens to contain
# the word "cannot" is data, not a tool failure.
tool_error() {
  _n=$(wc -l < "$TMPOUT" 2>/dev/null | tr -d ' ')
  case "$_n" in ''|*[!0-9]*) return 1 ;; esac
  [ "$_n" -le 2 ] || return 1
  grep -qiE '(^|: )(command not found|not found|Permission denied|Operation not permitted|No such file or directory|cannot|Invalid argument|unknown option)' "$TMPOUT" 2>/dev/null
}

# try <label> <timeout-seconds> <shell command>
#
# Runs one probe, appends its output to the report, and counts it. Returns 0
# only when the probe both succeeded and said something; anything else returns
# 1 so a caller can branch, but nothing here ever aborts the run.
try() {
  _label="$1"; _tmo="$2"; _cmd="$3"
  printf '\n--- %s ---\n$ %s\n' "$_label" "$_cmd" >> "$REPORT"
  if run_timeout "$_tmo" sh -c "$_cmd" > "$TMPOUT" 2>&1; then
    _st=PASS
  else
    _rc=$?
    _st=FAIL
  fi
  _sz=$(wc -c < "$TMPOUT" 2>/dev/null || echo 0)
  if [ "$_sz" -gt 100000 ]; then
    head -c 100000 "$TMPOUT" >> "$REPORT"
    printf '\n[output truncated — %s bytes in total]\n' "$_sz" >> "$REPORT"
  else
    cat "$TMPOUT" >> "$REPORT"
  fi

  # A probe that exited 0 but printed nothing has told us nothing, and a
  # pipeline ending in `tail` reports tail's status rather than the command's —
  # so PASS is claimed only when something actually came back. A false PASS
  # here would read as "the driver is present" when it is not.
  _said=0
  [ -n "$(tr -d '[:space:]' < "$TMPOUT" 2>/dev/null)" ] && _said=1
  _toolerr=0
  tool_error && _toolerr=1
  rm -f "$TMPOUT"

  if [ "$_st" = PASS ] && [ "$_said" -eq 1 ] && [ "$_toolerr" -eq 0 ]; then
    pass "$_label"
    return 0
  fi
  if [ "$_st" = PASS ] && [ "$_said" -eq 0 ]; then
    empty_ "$_label — succeeded but said nothing"
    return 1
  fi
  if [ "$_st" = PASS ]; then
    fail "$_label — the command could not run"
    return 1
  fi
  fail "$_label (exit ${_rc:-?})"
  return 1
}

# missing <tool> <package> <label>
missing() {
  printf '\n--- %s ---\nSKIP: %s is not installed (apt-get install -y %s)\n' "$3" "$1" "$2" >> "$REPORT"
  warn "$1 is not installed — install it with: apt-get install -y $2"
  skip "$3 — $1 is not installed"
}

checksum() {
  if have md5sum; then md5sum "$1" | cut -d' ' -f1
  elif have md5; then md5 -q "$1"
  else printf '(no md5 tool installed)'
  fi
}

# ── Before anything asks the hardware a question ─────────────────────────────

[ "$HAVE_TIMEOUT" -eq 1 ] || warn "No 'timeout' command on this system — probes are not time-bounded."
[ "$(id -u)" -eq 0 ] || {
  warn "Not running as root. dmesg and some device nodes may read back empty."
  warn "Re-run with sudo for a complete report."
}

if [ "$INSTALL_TOOLS" -eq 1 ]; then
  say "Installing the probe tools"
  if have apt-get; then
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq && apt-get install -y --no-install-recommends v4l-utils alsa-utils
  else
    warn "No apt-get here; install v4l-utils and alsa-utils however this system does it."
  fi
fi

{
  printf 'MULTISITE ENCODER BRING-UP PROBE — report\n'
  printf 'Generated   %s\n' "$(date -u '+%Y-%m-%d %H:%M:%SZ')"
  printf 'Host        %s\n' "$(hostname 2>/dev/null || echo unknown)"
  printf 'Kernel      %s\n' "$(uname -r)"
  printf 'Machine     %s\n' "$(uname -m)"
  printf 'Probe       version %s, running as uid %s\n' "$PROBE_VERSION" "$(id -u)"
  printf 'Directory   %s\n' "$OUT_DIR"
  cat <<'TXT'

WHAT TO SEND BACK
  The script tars this directory at the end and prints the path. Send that one
  file. Nothing in it is a secret: kernel versions, device names, timings and
  a checksum or two. It contains no captured picture — only how much of it
  arrived.

PLEASE ANSWER THESE SIX FIRST
  1. Which board is this, exactly? (R58 / R58X / R58 mini / R58S — a photo of
     the label if the model name is not printed anywhere)
  2. Which image was flashed? Board target, release, minimal or desktop — and
     is there another Armbian or Mekotronics image for this board that carries
     the Rockchip vendor kernel (linux-image-*-vendor-rk35xx)?
  3. Is the board's HDMI OUTPUT in use at the same time as its input?
  4. What is plugged into HDMI-in? Device, resolution, frame rate — and is it
     HDCP-protected (a cable box or a games console usually is)?
  5. Was audio from that source audible when it played through the board?
  6. Did the board do anything else while this ran — fans, a reboot, a blank
     screen, a kernel message?

COLLECTED DATA

TXT
} > "$REPORT"

say "Probing this machine — output in $OUT_DIR"

# ── 1. What machine is this ──────────────────────────────────────────────────

section "1. What machine is this"

try "operating system" 10 "cat /etc/os-release"
[ -f /etc/armbian-release ] && try "armbian release" 10 "cat /etc/armbian-release"
try "kernel" 10 "uname -a"
try "device tree model" 10 "tr '\0' '\n' < /proc/device-tree/model"
try "device tree compatible" 10 "tr '\0' '\n' < /proc/device-tree/compatible 2>/dev/null"
try "kernel command line" 10 "cat /proc/cmdline"
try "cpu" 15 "nproc; lscpu | head -20"
try "memory" 10 "free -h"
try "storage" 10 "df -h / /tmp /boot 2>/dev/null"
try "block devices" 15 "lsblk -o NAME,SIZE,TYPE,MOUNTPOINT 2>/dev/null | head -20"
try "board device trees installed" 15 "ls /boot/dtb*/rockchip/ 2>/dev/null | grep -iE 'r58|rk3588' | head -20"
try "boot configuration" 15 "cat /boot/armbianEnv.txt 2>/dev/null || head -30 /boot/extlinux/extlinux.conf 2>/dev/null"
try "top level device tree nodes" 10 "ls /proc/device-tree/ | head -60"

# ── 2. Does the kernel have the receiver at all ──────────────────────────────

section "2. Does the kernel have the HDMI receiver at all"

try "kernel config: the pieces a receiver needs" 20 \
  "{ zcat /proc/config.gz 2>/dev/null || cat /boot/config-\$(uname -r) 2>/dev/null; } | grep -iE 'HDMIRX|ROCKCHIP_SIP|VIDEO_ROCKCHIP|PHY_ROCKCHIP'"
try "loaded modules mentioning hdmi, vpu or mpp" 10 "lsmod | grep -iE 'hdmi|vpu|mpp|rga'"
if have modinfo; then
  # No `|| echo` fallback here on purpose: that would force a success status and
  # the summary would claim PASS for a module that is not there.
  try "the receiver module and its parameters" 20 \
    "modinfo rk_hdmirx 2>&1 || modinfo rockchip_hdmirx 2>&1"
else
  missing modinfo kmod "the receiver module and its parameters"
fi
try "kernel messages about the receiver" 20 "dmesg | grep -iE 'hdmirx|hdmi.?rx' | tail -60"
try "device tree nodes mentioning hdmi" 10 \
  "ls -d /proc/device-tree/*hdmi* 2>/dev/null; ls /proc/device-tree/*hdmi*/ 2>/dev/null | head -40"
try "sysfs classes and modules mentioning hdmi" 10 \
  "ls -d /sys/class/*hdmi* /sys/module/*hdmi* 2>/dev/null; ls /sys/module/*hdmi*/parameters/ 2>/dev/null"

# ── 3. The V4L2 capture node ─────────────────────────────────────────────────

section "3. The V4L2 capture node"

if have v4l2-ctl; then
  try "all v4l2 devices" 15 "v4l2-ctl --list-devices"
else
  missing v4l2-ctl v4l-utils "all v4l2 devices"
fi

# Find the receiver's node rather than assuming one: the driver has been named
# both rk_hdmirx and rockchip_hdmirx, and mainline calls it snps_hdmirx.
DEV=""
if have v4l2-ctl; then
  DEV="$(v4l2-ctl --list-devices 2>/dev/null \
         | awk '/hdmi/ && !/\/dev\// {f=1; next} f && /\/dev\/video/ {print $1; exit}')"
  if [ -z "$DEV" ]; then
    for _v in /dev/video*; do
      [ -e "$_v" ] || continue
      if v4l2-ctl -d "$_v" -D 2>/dev/null | grep -qi 'hdmirx'; then DEV="$_v"; break; fi
    done
  fi
fi

record ""
record "--- device discovery ---"
if [ -n "$DEV" ]; then
  record "Receiver node: $DEV"
  pass "found an HDMI receiver video node at $DEV"
else
  record "No HDMI receiver node found (/dev/video* was scanned with v4l2-ctl -D)."
  record "If this board does have a receiver, section 2 says whether its driver is there."
  skip "could not find an HDMI receiver V4L2 node"
fi

try "every registered video node" 20 \
  "for n in /sys/class/video4linux/video*; do printf '%s: ' \"\$n\"; cat \"\$n/name\" 2>/dev/null; done"

if [ -n "$DEV" ]; then
  try "the node's capabilities" 15 "v4l2-ctl -d $DEV -D"
  try "capabilities, current format and controls" 25 "v4l2-ctl -d $DEV --all | head -80"
  try "the formats and frame rates it offers" 25 "v4l2-ctl -d $DEV --list-formats-ext"
  try "its controls" 15 "v4l2-ctl -d $DEV -l | head -40"
  try "the timings it sees on the input right now" 15 "v4l2-ctl -d $DEV --get-dv-timings"
  try "input timings, the older spelling" 15 \
    "v4l2-ctl -d $DEV --query-dv-timings 2>&1 || v4l2-ctl -d $DEV --set-dv-bt-timings query 2>&1"
  try "EDID options this build of v4l-utils has" 15 "v4l2-ctl --help-edid 2>&1"
  if try "the EDID the board currently presents" 20 "v4l2-ctl -d $DEV --get-edid 2>&1 | head -60"; then
    :
  else
    record "(That failure is informative rather than wrong: the option's syntax"
    record " changed between v4l-utils versions. The --help-edid output above"
    record " says what this build wants.)"
  fi
fi

# ── 4. Can it actually capture ───────────────────────────────────────────────

section "4. Can it actually capture?"

CAPFILE="$OUT_DIR/cap_1080p_nv12.raw"
BGRFILE="$OUT_DIR/cap_1080p_bgr3.raw"
UHD4K="$OUT_DIR/cap_2160p_nv12.raw"
FRAME_1080_NV12=3110400

if [ -z "$DEV" ] || ! have v4l2-ctl; then
  record ""
  record "--- capture test ---"
  record "Skipped: there is no receiver node to capture from (see section 3)."
  skip "capture test — no receiver node"
else
  _t0=$(date +%s)
  # NV12 is what the encoder wants. If the driver only offers BGR24, that is a
  # finding in itself: it means a colour conversion before the encoder.
  if try "capture $FRAMES frames of 1080p NV12" 90 \
      "v4l2-ctl -d $DEV --set-fmt-video=width=1920,height=1080,pixelformat=NV12 --stream-mmap=4 --stream-count=$FRAMES --stream-to=$CAPFILE"; then
    :
  else
    rm -f "$CAPFILE"
    if try "capture $FRAMES frames of 1080p BGR3 instead" 90 \
        "v4l2-ctl -d $DEV --set-fmt-video=width=1920,height=1080,pixelformat=BGR3 --stream-mmap=2 --stream-count=$FRAMES --stream-to=$BGRFILE"; then
      record ""
      record "(NV12 failed but BGR3 worked: this driver offers no NV12, so a colour"
      record " conversion would sit in front of the encoder.)"
    fi
  fi
  _t1=$(date +%s)
  _el=$(( _t1 - _t0 ))
  [ "$_el" -gt 0 ] || _el=1

  record ""
  record "--- capture result ---"
  if [ -s "$CAPFILE" ]; then
    _sz=$(wc -c < "$CAPFILE" 2>/dev/null || echo 0)
    _fr=$(( _sz / FRAME_1080_NV12 ))
    record "whole 1080p NV12 frames captured: $_fr"
    record "bytes:                            $_sz"
    record "elapsed:                          ${_el}s (includes stream setup, so the"
    record "                                  frame rate below is a lower bound)"
    record "frame rate:                       $(( _fr / _el )) fps"
    record "md5:                              $(checksum "$CAPFILE")"
    pass "captured 1080p NV12 off the receiver"
  else
    record "Nothing arrived. Either there is no signal on HDMI-in at this moment, or"
    record "the receiver needs a timing set on it first, or it does not offer the"
    record "format that was asked for — --list-formats-ext in section 3 says which"
    record "formats and frame rates it will actually give."
    skip "no frames captured at 1080p"
  fi

  # The 4K question. The SoC's receiver is documented as handing over 2160p only
  # at 4:2:0, and it is worth knowing rather than assuming, because it decides
  # whether this board can ever be asked for a 2160p feed.
  try "capture 5 frames of 2160p NV12 (the 4K case)" 90 \
    "v4l2-ctl -d $DEV --set-fmt-video=width=3840,height=2160,pixelformat=NV12 --stream-mmap=2 --stream-count=5 --stream-to=$UHD4K"
fi

# ── 5. Audio in over HDMI ────────────────────────────────────────────────────

section "5. Audio in over HDMI"

try "sound cards" 10 "cat /proc/asound/cards"
try "pcm devices" 10 "cat /proc/asound/pcm"
if have arecord; then
  try "capture devices" 15 "arecord -l"
  try "device names mentioning hdmi" 15 "arecord -L | grep -i hdmi"
else
  missing arecord alsa-utils "capture devices"
fi

# rockchiphdmiin is the input card; rockchiphdmi is the output one, and it is
# easy to match the wrong one, so look for the "in" first.
CARD_IDX=""
if [ -r /proc/asound/cards ]; then
  CARD_IDX="$(awk '/hdmiin/ {print $1; exit}' /proc/asound/cards)"
  [ -n "$CARD_IDX" ] || CARD_IDX="$(awk '/hdmi/ {print $1; exit}' /proc/asound/cards)"
fi

record ""
record "--- audio discovery ---"
if [ -n "$CARD_IDX" ]; then
  record "Using card $CARD_IDX"
  pass "found an HDMI capture sound card (card $CARD_IDX)"
  try "the card's capture stream status" 10 "cat /proc/asound/card$CARD_IDX/pcm*c/sub0/status 2>&1"
else
  record "No sound card name mentions HDMI. If this image has no HDMI-in audio"
  record "card, then HDMI audio has to come from somewhere else."
  skip "no HDMI capture sound card"
fi

WAV="$OUT_DIR/hdmiin.wav"
if [ -n "$CARD_IDX" ] && have arecord; then
  _got=0
  for _adev in "hw:$CARD_IDX,0" "plughw:$CARD_IDX,0"; do
    if try "record 5 seconds of audio from $_adev" 30 \
        "arecord -D $_adev -f S16_LE -r 48000 -c 2 -d 5 -t wav $WAV 2>&1 >/dev/null; echo \"arecord exit: \$?\"; ls -l $WAV 2>&1"; then
      _got=1
      break
    fi
    rm -f "$WAV"
  done

  if [ "$_got" -eq 1 ] && [ -s "$WAV" ]; then
    # A dependency-free loudness check, so that "the card exists" and "the card
    # carried audio" are two different answers. od and awk are everywhere; sox
    # and python are not. Only the first ~2 seconds are measured.
    record ""
    record "--- audio level, first 2s at 48 kHz stereo ---"
    tail -c +45 "$WAV" | head -c 200000 | od -An -v -t d2 | awk '
      { for (i = 1; i <= NF; i++) { v = $i + 0; if (v < 0) v = -v; if (v > peak) peak = v; s += v * v; n++ } }
      END {
        if (n == 0) { print "  no samples found in the file"; exit }
        printf "  peak %d of 32768 (%.2f%% of full scale)\n", peak, peak * 100 / 32768
        printf "  rms  %.1f over %d samples\n", sqrt(s / n), n
        if (peak < 16) print "  => silence: the card is there but nothing came through it"
        else print "  => audio arrived: the card is carrying a signal"
      }' >> "$REPORT"
    record "  (keep the .wav if the peak was near zero and sound was expected)"
  else
    record ""
    record "Could not record from the HDMI capture card. The arecord error above says"
    record "why — most often the card carries no audio stream, or it is busy."
    skip "no audio recorded from the HDMI capture card"
  fi
fi

# ── 6. Hardware encoder ──────────────────────────────────────────────────────

section "6. Hardware encoder"

try "media processor devices" 10 "ls -l /dev/mpp_service /dev/rga /dev/dma_heap /dev/dri 2>&1"
try "kernel messages about the encoder" 20 "dmesg | grep -iE 'mpp|rkvdec|rkvenc|vpu' | tail -40"
try "encoder nodes in sysfs" 10 "ls -d /sys/class/mpp* /sys/kernel/debug/mpp* /sys/kernel/debug/rkvenc* 2>/dev/null"
if have ffmpeg; then
  try "ffmpeg's rockchip encoders" 30 "ffmpeg -hide_banner -encoders 2>/dev/null | grep -iE 'rkmpp|v4l2m2m'"
else
  missing ffmpeg ffmpeg "ffmpeg's rockchip encoders"
fi
if have gst-inspect-1.0; then
  try "gstreamer's rockchip elements" 30 "gst-inspect-1.0 2>/dev/null | grep -i mpp | head -20"
else
  record ""
  record "gstreamer is not installed (its mpp plugin is another way in, but ffmpeg"
  record "is the one the appliance would use)."
fi
if have mpi_enc_test; then
  try "the rockchip test encoder binary" 15 "mpi_enc_test -h 2>&1 | head -20"
else
  record "mpi_enc_test is not installed — it ships with Rockchip's mpp sources,"
  record "not with apt."
fi

ENC_TOOL=""
if have ffmpeg && ffmpeg -hide_banner -encoders 2>/dev/null | grep -q 'h264_rkmpp'; then
  ENC_TOOL="ffmpeg"
elif have gst-launch-1.0 && gst-inspect-1.0 mpph264enc >/dev/null 2>&1; then
  ENC_TOOL="gstreamer"
fi

ELOG="$OUT_DIR/encode.log"
record ""
record "--- encoder result ---"
if [ -z "$ENC_TOOL" ]; then
  record "No hardware H.264 encoder found. On this board that usually means the"
  record "vendor kernel's media processor pieces are missing — the dmesg and module"
  record "output above says whether the driver is there at all."
  skip "no hardware encoder to test"
elif [ ! -s "$CAPFILE" ]; then
  record "Found $ENC_TOOL, but there are no captured frames to feed it, so the"
  record "encode was not run. The capture result in section 4 is the blocker."
  skip "hardware encode — nothing was captured to encode"
elif [ "$ENC_TOOL" = "ffmpeg" ]; then
  record "Encoding with ffmpeg -c:v h264_rkmpp, reading the captured NV12."
  record "-benchmark makes ffmpeg report its own CPU time, which is the number"
  record "that matters: a hardware encoder should leave the CPU nearly idle."
  _cmd="ffmpeg -hide_banner -benchmark -y -stream_loop -1 -f rawvideo -pix_fmt nv12 -s 1920x1080 -r 60 -i $CAPFILE -c:v h264_rkmpp -b:v 8M -t $ENCODE_SECONDS -f null - > $ELOG 2>&1; _rc=\$?; echo \"ffmpeg exit: \$_rc\"; grep -o 'fps=[ ]*[0-9.]*' $ELOG | tail -1; grep 'bench:' $ELOG | tail -1; exit \$_rc"
  if try "encode ${ENCODE_SECONDS}s of 1080p60 H.264 in hardware" $(( ENCODE_SECONDS + 60 )) "$_cmd"; then
    record ""
    record "fps above is ffmpeg's own counter over a sustained run; 'bench:' is the"
    record "CPU time it took. Compare utime+stime against rtime: they should be a"
    record "small fraction of it on a hardware encoder."
  else
    record ""
    record "The sustained encode failed. A single pass over the captured frames is"
    record "tried next — it gives a peak rate rather than a sustained one."
    _cmd="ffmpeg -hide_banner -benchmark -y -f rawvideo -pix_fmt nv12 -s 1920x1080 -r 60 -i $CAPFILE -c:v h264_rkmpp -b:v 8M -f null - > $ELOG 2>&1; _rc=\$?; echo \"ffmpeg exit: \$_rc\"; grep -o 'fps=[ ]*[0-9.]*' $ELOG | tail -1; grep 'bench:' $ELOG | tail -1; exit \$_rc"
    try "encode the captured frames once, no looping" 120 "$_cmd"
  fi
else
  record "Encoding with gstreamer mpph264enc."
  try "encode ${ENCODE_SECONDS}s of 1080p60 H.264 with gstreamer" $(( ENCODE_SECONDS + 60 )) \
    "gst-launch-1.0 -q filesrc location=$CAPFILE ! rawvideoparse format=nv12 width=1920 height=1080 ! mpph264enc ! fakesink 2>&1 | tail -5"
fi

# ── 7. Temperature under load ────────────────────────────────────────────────

thermal_line() {
  _z=""
  for _tz in /sys/class/thermal/thermal_zone*; do
    [ -r "$_tz/temp" ] || continue
    _z="$_z$(basename "$_tz")=$(cat "$_tz/temp" 2>/dev/null) "
  done
  _fr=""
  for _f in /sys/devices/system/cpu/cpu*/cpufreq/scaling_cur_freq; do
    [ -r "$_f" ] || continue
    _fr=$(cat "$_f" 2>/dev/null)
    break
  done
  printf '%s cpu=%s' "${_z:-no thermal zones }" "${_fr:-?}"
}

section "7. Temperature under load"

if [ "$DO_THERMAL" -eq 0 ]; then
  record ""
  record "--- temperature ---"
  record "Skipped: --no-thermal was given. This is the section that answers whether"
  record "the board can encode for as long as an event lasts without throttling, so"
  record "it is worth running when there is time for it."
  skip "temperature under load — skipped by --no-thermal"
else
  record ""
  record "--- the thermal zones this board has ---"
  for _tz in /sys/class/thermal/thermal_zone*; do
    [ -r "$_tz/type" ] || continue
    record "  $(basename "$_tz"): $(cat "$_tz/type" 2>/dev/null)"
  done
  record ""
  record "--- temperature every 30s for ${THERMAL_MINUTES} minutes ---"

  _load=0
  if [ "$ENC_TOOL" = "ffmpeg" ] && [ -s "$CAPFILE" ]; then
    (
      _until=$(( $(date +%s) + THERMAL_MINUTES * 60 ))
      while [ "$(date +%s)" -lt "$_until" ]; do
        ffmpeg -hide_banner -y -stream_loop -1 -f rawvideo -pix_fmt nv12 -s 1920x1080 \
          -r 60 -i "$CAPFILE" -c:v h264_rkmpp -b:v 8M -t 5 -f null - >/dev/null 2>&1
      done
    ) &
    _load=$!
    note "encoding in the background for the whole ${THERMAL_MINUTES} minutes"
  else
    warn "nothing to encode, so this watches temperature on an idle board"
    record "  (no encoder available to run: this is an idle board, not a loaded one)"
  fi

  _i=0
  _n=$(( THERMAL_MINUTES * 2 ))
  while [ "$_i" -lt "$_n" ]; do
    _i=$(( _i + 1 ))
    record "  t+$(printf '%4d' $(( _i * 30 )))s  $(thermal_line)"
    sleep 30
  done

  if [ "$_load" -ne 0 ]; then
    # The subshell stops immediately; an ffmpeg inside it finishes the 5-second
    # chunk it is on and then exits, so nothing is left encoding afterwards.
    kill "$_load" 2>/dev/null
    wait "$_load" 2>/dev/null
  fi
  pass "temperature watched for ${THERMAL_MINUTES} minutes"
fi

# ── 8. What was found ────────────────────────────────────────────────────────

section "8. What was found"

if [ "$KEEP_RAW" -eq 0 ]; then
  rm -f "$CAPFILE" "$BGRFILE" "$UHD4K"
  record ""
  record "(The captured frames were deleted: they are large, and only their size and"
  record " checksum matter. --keep-raw would have kept them.)"
fi

_p=0; _f=0; _s=0; _e=0
while IFS='|' read -r _st _lbl; do
  case "$_st" in
    PASS)  _p=$(( _p + 1 )) ;;
    FAIL)  _f=$(( _f + 1 )) ;;
    SKIP)  _s=$(( _s + 1 )) ;;
    EMPTY) _e=$(( _e + 1 )) ;;
  esac
done < "$RESULTS"

{
  printf '\n--- summary ---\n'
  while IFS='|' read -r _st _lbl; do
    printf '  %-5s %s\n' "$_st" "$_lbl"
  done < "$RESULTS"
  printf '\n  %s passed, %s failed, %s skipped, %s said nothing\n' "$_p" "$_f" "$_s" "$_e"
  printf '\n  How to read this\n'
  printf '    PASS   the probe ran and printed something to look at\n'
  printf '    FAIL   the probe ran and came back with an error, quoted above\n'
  printf '    EMPTY  the probe ran and told us nothing — no such device, no such\n'
  printf '           file, or a tool that is not installed. Not an error, but not\n'
  printf '           an answer either.\n'
  printf '    SKIP   the script chose not to run it, usually because a tool or the\n'
  printf '           device it needs is absent.\n'
  printf '\n  A report that is mostly SKIP and EMPTY is a normal report from a board\n'
  printf '  without the vendor kernel. What matters is which sections are EMPTY.\n'
} >> "$REPORT"

rm -f "$RESULTS" "$TMPOUT"

TARBALL="${OUT_DIR}.tar.gz"
rm -f "$TARBALL"
_send="$OUT_DIR (tar is not installed here, so send the directory)"
if have tar && tar -czf "$TARBALL" -C "$(dirname "$OUT_DIR")" "$(basename "$OUT_DIR")" 2>/dev/null; then
  _send="$TARBALL"
fi
{
  printf '\n--- sending this back ---\n  %s\n' "$_send"
} >> "$REPORT"

say "Done — $_p passed, $_f failed, $_s skipped, $_e said nothing"
printf '    Report:    %s\n' "$REPORT"
printf '    Send back: %s\n\n' "$_send"

# ── Sending it back ──────────────────────────────────────────────────────────
#
# Nobody has an account on either end and nothing of ours is running anywhere,
# so the tarball goes to a public file drop and the script prints the link for
# whoever ran it to send back. Not automatic — it still needs that one paste —
# but a short line is a great deal easier to get out of a lab than a tarball.
#
# Several services, tried in order, because which of these answer from inside
# mainland China is not something that can be established from outside it. The
# first one that returns a usable link wins; the report records which, so the
# next board can start with the one that worked.
#
# What this means for the report: the link is unguessable but it is not
# private. Anyone who has it can read the report — board model, kernel, the
# device list, and any serial numbers those commands happened to print. That is
# the trade for needing no account, and it is why the upload can be turned off.
upload_to_paste() {
  _file="$1"
  _name=$(basename "$_file")
  _url=""
  _via=""

  # Each entry is "name|host-that-must-appear-in-the-link". The host is not
  # decoration: the first version of this scraped any URL out of the reply, and
  # when curl failed a TLS handshake it matched the address inside curl's own
  # error text and reported success with a link to curl's documentation. A
  # wrong link is worse than no link, because nobody finds out until the report
  # never arrives.
  # litterbox first because its files expire. An anonymous catbox upload cannot
  # be deleted afterwards — there is no handle to delete it with — so every
  # report would sit publicly forever: board model, kernel, device list, and
  # whatever serial numbers the probes happened to print, from a vendor
  # relationship nobody asked to have indexed. 72h is its maximum and is long
  # enough for a report somebody is waiting for; catbox stays as the fallback,
  # because a permanent link still beats no report at all.
  # The host to match is the one the link comes back on, which is not always
  # the one uploaded to: litterbox takes the file at litterbox.catbox.moe and
  # hands back a link on litter.catbox.moe. Matching the registered domain
  # rather than the exact subdomain tolerates that — and still rejects the case
  # this check exists for, a URL scraped out of curl's own error text.
  for _entry in \
      "litterbox|catbox.moe" \
      "catbox.moe|catbox.moe" \
      "file.io|file.io"
  do
    _svc="${_entry%%|*}"
    _host="${_entry##*|}"
    printf '    trying %s ... ' "$_svc"
    _raw=""; _rc=0
    case "$_svc" in
      catbox.moe)
        _raw=$(curl --silent --show-error --connect-timeout 20 --max-time 600 \
                    -A "multisite-probe/$PROBE_VERSION" \
                    -F "reqtype=fileupload" -F "fileToUpload=@${_file}" \
                    "https://catbox.moe/user/api.php" 2>&1); _rc=$? ;;
      litterbox)
        # Same service as catbox, same API, but the file expires.
        _raw=$(curl --silent --show-error --connect-timeout 20 --max-time 600 \
                    -A "multisite-probe/$PROBE_VERSION" \
                    -F "reqtype=fileupload" -F "time=72h" \
                    -F "fileToUpload=@${_file}" \
                    "https://litterbox.catbox.moe/resources/internals/api.php" \
                    2>&1); _rc=$? ;;
      file.io)
        _raw=$(curl --silent --show-error --connect-timeout 20 --max-time 600 \
                    -A "multisite-probe/$PROBE_VERSION" \
                    -F "file=@${_file}" "https://file.io" 2>&1); _rc=$? ;;
    esac

    if [ "$_rc" -ne 0 ]; then
      # curl itself failed — no route, DNS, TLS. Its output is an error
      # message, not a reply, and must not be searched for a link.
      printf 'no (curl %s: %s)\n' "$_rc" \
             "$(printf '%s' "$_raw" | tr -d '\r' | head -1 | cut -c1-60)"
      continue
    fi

    # Match the link rather than the sentence around it — each of these words
    # it differently — but only a link at the host that service actually
    # serves from, and never a management or delete handle.
    _cand=$(printf '%s\n' "$_raw" \
            | grep -Eo 'https://[A-Za-z0-9._~:/?#@!$&()*+,;=%-]+' \
            | grep -F "$_host" \
            | grep -v -i -E 'manage|delete|admin|/rm/' \
            | head -1)
    case "$_cand" in
      https://*)
        printf 'ok\n'; _url="$_cand"; _via="$_svc"; break ;;
      *)
        # It answered, but not with a link we can use — an error page, a quota
        # notice, or a format change. Show the first line of what it did say.
        printf 'no (%s)\n' \
               "$(printf '%s' "$_raw" | tr -d '\r' | head -1 | cut -c1-60)" ;;
    esac
  done

  PASTE_URL="$_url"
  PASTE_VIA="$_via"
  [ -n "$_url" ]
}

# Two ways out, in order of preference:
#
#   --upload-url given   PUT straight to a pre-signed URL. Nothing for anyone
#                        to forward, and the report is not public. Needs the
#                        URL to have been generated and passed in.
#   otherwise            upload to a public file drop and print the link for
#                        whoever ran it to send back. Needs no account and no
#                        argument, which is why it is the default: the command
#                        the vendor has takes no options.
#
# This runs last and on purpose cannot fail the run. The tarball is already
# written and its path already printed, so every path through here — including
# both of them failing — ends with the operator able to send it by hand.
if [ -z "$UPLOAD_URL" ] && [ "$DO_UPLOAD" = "1" ]; then
  if [ ! -f "$TARBALL" ]; then
    printf '    Upload:    skipped — no tarball to send (is tar installed?)\n\n'
  elif ! have curl; then
    printf '    Upload:    skipped — curl is not installed on this board\n\n'
  else
    _bytes=$(wc -c < "$TARBALL" 2>/dev/null || echo 0)
    _mb=$(( _bytes / 1048576 ))
    say "Uploading the report (${_mb} MB) so it can be sent back as a link"
    if upload_to_paste "$TARBALL"; then
      # Printed like this because it is the one thing that has to survive being
      # read off a screen in somebody else's lab and pasted into a chat window.
      printf '\n'
      printf '    ============================================================\n'
      printf '      SEND THIS LINK BACK:\n\n'
      printf '        %s\n\n' "$PASTE_URL"
      printf '      (uploaded via %s; the file is also at %s)\n' "$PASTE_VIA" "$_send"
      printf '    ============================================================\n\n'
      {
        printf '\n--- sent back ---\n'
        printf '  uploaded %s bytes via %s\n' "$_bytes" "$PASTE_VIA"
        printf '  %s\n' "$PASTE_URL"
      } >> "$REPORT"
    else
      printf '\n    Upload:    FAILED — none of the file drops answered.\n'
      printf '               Nothing is wrong with the run. Send this file:\n'
      printf '               %s\n\n' "$_send"
      {
        printf '\n--- sent back ---\n'
        printf '  upload FAILED: no file drop answered (blocked, or no route out)\n'
        printf '  the tarball is still here and can be sent by hand\n'
      } >> "$REPORT"
    fi
  fi
elif [ -n "$UPLOAD_URL" ]; then
  if [ ! -f "$TARBALL" ]; then
    printf '    Upload:    skipped — no tarball to send (is tar installed?)\n\n'
  elif ! have curl; then
    printf '    Upload:    skipped — curl is not installed on this board\n\n'
  else
    # Show where it is going, without the signature. The query string IS the
    # credential, and this output is likely to be pasted into an email.
    _dest="${UPLOAD_URL%%\?*}"
    _bytes=$(wc -c < "$TARBALL" 2>/dev/null || echo 0)
    _mb=$(( _bytes / 1048576 ))
    say "Sending the report back (${_mb} MB) — $_dest"

    # Generous but bounded. A link out of a Chinese lab can be slow and can
    # stall outright, and a probe that hangs here at the end of a ten-minute
    # run is worse than one that gives up and says so. --retry covers the
    # transient half of that; --max-time covers the rest.
    _ulog="$OUT_DIR/upload.log"
    if curl --fail --silent --show-error \
            --connect-timeout 30 --max-time 900 \
            --retry 4 --retry-delay 10 --retry-connrefused \
            -T "$TARBALL" "$UPLOAD_URL" >"$_ulog" 2>&1; then
      printf '    Upload:    sent (%s MB)\n\n' "$_mb"
      {
        printf '\n--- sent back ---\n'
        printf '  uploaded %s bytes to %s\n' "$_bytes" "$_dest"
      } >> "$REPORT"
    else
      _err=$(head -c 400 "$_ulog" 2>/dev/null)
      printf '    Upload:    FAILED — %s\n' "${_err:-no output from curl}"
      printf '               The run itself is fine. Send this file by hand:\n'
      printf '               %s\n\n' "$_send"
      {
        printf '\n--- sent back ---\n'
        printf '  upload to %s FAILED: %s\n' "$_dest" "${_err:-no output}"
        printf '  (expired URL, no route out, or a proxy in the way — the\n'
        printf '   tarball is still here and can be sent by hand)\n'
      } >> "$REPORT"
    fi
    rm -f "$_ulog"
  fi
fi
