#!/usr/bin/env bash
#
# purge-digisyn.sh — take Digisynthetic's AES67 stack back off a player.
#
#   sudo bash scripts/player/purge-digisyn.sh
#
# or, on the box itself, with no checkout to hand:
#
#   curl -fsSL https://raw.githubusercontent.com/stageaudioworks/obs-multisite/main/scripts/player/purge-digisyn.sh | sudo bash
#
# Why this exists
# ---------------
# This is the only file in this repository that names Digisynthetic, and it only
# names it in order to remove it. Their virtual sound card was this project's
# first route onto the network. It has since been replaced by the open stack in
# scripts/player/merging-aes67.sh — Merging's ravenna-alsa-lkm kernel module and
# the GPL aes67-daemon — and the player carries no code that knows about their
# calendar, their device node or their daemon any more.
#
# What is left is the installation itself, sitting on a box that was set up
# before the change: a kernel module, a daemon and its licence, a systemd unit,
# a drop-in ordering the player after it, an entry that loads the module at
# boot, a DKMS registration, and one line of the player's configuration. This
# removes exactly those, and nothing else. It is the mirror image of the
# installer that wrote them, and every path below is one that installer wrote.
#
# What it does NOT touch
# ----------------------
# The open AES67 stack. MergingRavennaALSA, aes67-daemon, /etc/daemon.conf and
# aes67-daemon.service are left exactly as they are: on a box that has already
# moved over, they are the thing keeping the sound on the network. Nor does it
# unload snd or snd-pcm, which the HDMI output needs too.
#
# Safe to run again: every step looks first, reports what it found, and does
# nothing if the thing is already gone.
set -euo pipefail

# ── What the installer wrote, and where ──────────────────────────────────────
# Every path here is taken from the installer this script undoes, rather than
# guessed at, so a box set up by that installer is fully covered.
PREFIX_BIN="${PREFIX_BIN:-/usr/local/bin}"
DAEMON="$PREFIX_BIN/DigiAes67Proc"
DAEMON_CONF_DIR="/etc/DigiAes67Proc"
DAEMON_SERVICE="DigiAes67Proc"

# The .ko is Digisyn-vSndCard.ko, so the module loads by that name either way;
# the underscore form is what lsmod shows. Both are tried when unloading.
MODULE_NAME="Digisyn-vSndCard"
MODULE_NAME_LSMOD="Digisyn_vSndCard"
MODULE_LOAD_CONF="/etc/modules-load.d/digisyn-vsndcard.conf"

# DKMS name and source tree. The version is discovered from `dkms status` rather
# than written down, because the installer derived it from the vendor's package
# version and a vendor update would change it.
DKMS_NAME="digisyn-vsndcard"
DKMS_SRC_GLOB="/usr/src/digisyn-vsndcard-*"

# Where the installer unpacked and built, and where it kept its own report.
BUILD_DIR="${BUILD_DIR:-/var/tmp/digisyn-vsc}"
SCRATCH_GLOB="/var/tmp/multisite-aes67-*"

PLAYER_CONFIG="/etc/multisite-player/config.json"
PLAYER_UNIT="multisite-player"
PLAYER_DROPIN_DIR="/etc/systemd/system/$PLAYER_UNIT.service.d"
PLAYER_DROPIN="$PLAYER_DROPIN_DIR/aes67.conf"

# What the player goes back to. "default" follows the system and is what
# install.sh writes on a fresh box, so it is the honest answer for a player that
# was pointed at the vendor's card and nothing else.
PLAYER_FALLBACK_DEVICE="default"

DRY_RUN=0
CHECK_ONLY=0
KEEP_PLAYER_CONFIG=0
ASSUME_YES="${ASSUME_YES:-0}"

# ── Output ───────────────────────────────────────────────────────────────────
say()  { printf '\n\033[1;36m==>\033[0m \033[1m%s\033[0m\n' "$*"; }
note() { printf '    %s\n' "$*"; }
warn() { printf '\033[1;33m    %s\033[0m\n' "$*"; }
die()  { printf '\n\033[1;31mThat did not work:\033[0m %s\n\n' "$*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

usage() {
    cat <<'EOF'
usage: purge-digisyn.sh [options]

Removes Digisynthetic's AES67 virtual sound card from a player: its kernel
module, daemon, licence, systemd unit, the drop-in that ordered the player
after it, its DKMS registration and its boot-time load entry — and points the
player back at its ordinary output.

What it changes
  the DigiAes67Proc service and /etc/systemd/system/DigiAes67Proc.service,
  /usr/local/bin/DigiAes67Proc, /etc/DigiAes67Proc,
  /etc/modules-load.d/digisyn-vsndcard.conf, the DKMS entry and /usr/src copy,
  the .ko under /lib/modules/*/extra, the build scratch in /var/tmp,
  the player's systemd drop-in, and alsa_device in the player's config.

What it never touches
  the open AES67 stack — MergingRavennaALSA, aes67-daemon, /etc/daemon.conf —
  or snd/snd-pcm, which HDMI needs too.

Options
  --check              Report what is on the box and change nothing.
  --keep-player-config Leave alsa_device alone; remove the card, do not repoint
                       the player. Do this if it is already on another device.
  --dry-run            Print every change and make none of them.
  -y, --yes            Do not ask for confirmation.
  -h, --help           This text.
EOF
}

# ── Doing things, or saying what would have been done ────────────────────────
# Every mutating command goes through one of these, so --dry-run is a property
# of the script rather than something each step has to remember. `set -e` is on,
# which is the other reason: a command that would fail on a box already cleaned
# up never runs, and the run does not end on it.
run() {
    if [ "$DRY_RUN" -eq 1 ]; then
        note "would run:  $*"
        return 0
    fi
    "$@"
}

# remove <path> [tree] — pass the literal -r for a directory.
remove() {
    local path="$1" tree="${2:-}"
    if [ ! -e "$path" ] && [ ! -L "$path" ]; then
        note "absent:      $path"
        return 0
    fi
    if [ "$DRY_RUN" -eq 1 ]; then
        note "would remove $path"
        return 0
    fi
    if [ "$tree" = "-r" ]; then
        rm -rf -- "$path"
    else
        rm -f -- "$path"
    fi
    note "removed:     $path"
}

# Several paths at once, where an argument may be a pattern rather than a name.
# The patterns arrive quoted and are expanded here rather than at the call site,
# so nothing depends on the caller's word splitting — and a pattern that matches
# nothing is reported as absent instead of being handed to rm as its own literal
# name, which is what would otherwise try to delete a file called
# "/var/tmp/multisite-aes67-*".
remove_all() {
    local pattern path
    for pattern in "$@"; do
        # shellcheck disable=SC2086  # the expansion is the point: this is a glob
        for path in $pattern; do
            if [ -e "$path" ] || [ -L "$path" ]; then
                remove "$path" -r
            else
                note "absent:      $pattern"
            fi
        done
    done
}

# True if any of these patterns matches something on disk. compgen does the
# expansion itself, so a pattern can be passed quoted and still work.
any_exists() {
    local pattern
    for pattern in "$@"; do
        if compgen -G "$pattern" >/dev/null 2>&1; then return 0; fi
    done
    return 1
}

confirm() {
    [ "$ASSUME_YES" -eq 1 ] && return 0
    if [ ! -r /dev/tty ]; then
        warn "there is no terminal here to ask on, and -y was not given."
        die "run again with -y to accept this, or --check to look first"
    fi
    local answer=""
    printf '    %s [y/N]: ' "$1" > /dev/tty
    IFS= read -r answer < /dev/tty || answer=""
    case "$answer" in
        y|Y|yes|YES) return 0 ;;
        *) return 1 ;;
    esac
}

# ── Reading the box ──────────────────────────────────────────────────────────
# The card id the vendor's driver is publishing, read from the kernel rather
# than written down: the driver never names its own card, so ALSA derived the id
# from the driver's shortname and truncated it to its fifteen-character limit.
#
# This is read before the module is unloaded, because afterwards there is
# nothing left to read — and it is what decides whether the player's
# alsa_device is pointing at this card or at something else entirely.
digisyn_card_ids() {
    awk '
        /^[[:space:]]*[0-9]+[[:space:]]*\[/ {
            if (index($0, "Digisyn") > 0) {
                match($0, /\[[^]]*\]/)
                if (RSTART > 0) {
                    s = substr($0, RSTART + 1, RLENGTH - 2)
                    gsub(/[[:space:]]+$/, "", s)
                    if (s != "") print s
                }
            }
        }
    ' /proc/asound/cards 2>/dev/null
}

# What the player is currently asked to open. Empty when there is no config, no
# python3, or a file that will not parse — all three of which mean the same
# thing here: leave it alone rather than overwrite settings we cannot read.
player_device() {
    [ -f "$PLAYER_CONFIG" ] || return 0
    have python3 || return 0
    python3 - "$PLAYER_CONFIG" <<'PY' 2>/dev/null || true
import json, sys
try:
    with open(sys.argv[1]) as f:
        cfg = json.load(f)
except Exception:
    sys.exit(0)
print(cfg.get("alsa_device", ""))
PY
}

# The card id inside an alsa_device string: "plughw:CARD=RAVENNA" -> "RAVENNA".
device_card_id() {
    printf '%s' "$1" | sed -n 's/.*CARD=\([^,:]*\).*/\1/p'
}

while [ $# -gt 0 ]; do
    case "$1" in
        --check)              CHECK_ONLY=1; shift ;;
        --keep-player-config) KEEP_PLAYER_CONFIG=1; shift ;;
        --dry-run)            DRY_RUN=1; shift ;;
        -y|--yes)             ASSUME_YES=1; shift ;;
        -h|--help)            usage; exit 0 ;;
        *) die "unknown option: $1 (try --help)" ;;
    esac
done

# ── Preflight ────────────────────────────────────────────────────────────────
# Only for a run that is going to change something. --help and --check are read
# only, and --dry-run changes nothing, so all three are allowed on a laptop —
# which is exactly where this wants to be read through before anybody drives to
# a church with it.
if [ "$CHECK_ONLY" -eq 0 ] && [ "$DRY_RUN" -eq 0 ]; then
    if [ "$(uname -s)" != "Linux" ]; then
        die "this is for the player, which runs Linux; $(uname -s) is not that"
    fi
    if [ "$(id -u)" -ne 0 ]; then
        die "run this with sudo — it removes files under /etc, /lib/modules and /usr/src"
    fi
fi

VENDOR_CARD_IDS="$(digisyn_card_ids 2>/dev/null || true)"
# ═════════════════════════════════════════════════════════════════════════════
# What is on the box now
# ═════════════════════════════════════════════════════════════════════════════
say "What is on this box before anything is changed"

# Every item the installer could have left, reported as found or not, so the
# whole picture is on the screen before anything is agreed to — and so the same
# list going quiet afterwards is the evidence that it worked.
# Every one of these answers "is it still here?", so the report at the start and
# the check at the end are asking the same questions rather than two similar
# ones that could disagree.
present_file() { [ -e "$1" ] || [ -L "$1" ]; }

report_item() {
    if present_file "$2"; then
        note "found:  $1"
    else
        note "none:   $1"
    fi
}

service_known() {
    have systemctl || return 1
    systemctl list-unit-files 2>/dev/null | grep -q "^$DAEMON_SERVICE\.service"
}
module_loaded()   { lsmod 2>/dev/null | grep -qE "^$MODULE_NAME_LSMOD\b"; }
card_present()    { [ -n "$(digisyn_card_ids 2>/dev/null)" ]; }
dkms_registered() {
    have dkms || return 1
    dkms status 2>/dev/null | grep -q "^$DKMS_NAME"
}

if service_known; then
    note "found:  the $DAEMON_SERVICE service"
else
    note "none:   the $DAEMON_SERVICE service"
fi
report_item "$DAEMON" "$DAEMON"
report_item "its settings and licence, $DAEMON_CONF_DIR" "$DAEMON_CONF_DIR"
report_item "its boot-time load entry, $MODULE_LOAD_CONF" "$MODULE_LOAD_CONF"
report_item "the player drop-in, $PLAYER_DROPIN" "$PLAYER_DROPIN"

if module_loaded; then
    note "found:  the $MODULE_NAME module is loaded"
else
    note "none:   the $MODULE_NAME module is not loaded"
fi
if card_present; then
    note "found:  an ALSA card for it: $(printf '%s' "$VENDOR_CARD_IDS" | tr '\n' ' ')"
else
    note "none:   no ALSA card for it in /proc/asound/cards"
fi
if [ -e "/dev/$MODULE_NAME_LSMOD" ]; then
    note "found:  the device node /dev/$MODULE_NAME_LSMOD"
else
    note "none:   the device node /dev/$MODULE_NAME_LSMOD"
fi
if dkms_registered; then
    note "found:  a DKMS entry — $(dkms status 2>/dev/null | grep "^$DKMS_NAME" | head -1)"
elif have dkms; then
    note "none:   no DKMS entry"
else
    note "none:   dkms is not installed here"
fi
if any_exists "$DKMS_SRC_GLOB"; then
    note "found:  the DKMS source tree in /usr/src"
fi
if any_exists "$BUILD_DIR" "$SCRATCH_GLOB"; then
    note "found:  build scratch in /var/tmp"
fi

_device="$(player_device)"
if [ -n "$_device" ]; then
    note "player: alsa_device is '$_device'"
else
    note "player: no alsa_device to read (no config, or no python3 to read it with)"
fi

# Named so the operator can see the script has noticed the other stack and is
# deliberately leaving it where it is.
if lsmod 2>/dev/null | grep -q '^MergingRavennaALSA'; then
    note "open:   MergingRavennaALSA is loaded — left alone"
fi
if have systemctl && systemctl is-active --quiet aes67-daemon 2>/dev/null; then
    note "open:   aes67-daemon is running — left alone"
fi

if [ "$CHECK_ONLY" -eq 1 ]; then
    say "Check only — nothing was changed"
    note "Run again without --check to remove whatever was found."
    exit 0
fi

echo
say "About to remove Digisynthetic's stack"
note "The open AES67 stack (MergingRavennaALSA, aes67-daemon, /etc/daemon.conf)"
note "is not touched."
if [ "$KEEP_PLAYER_CONFIG" -eq 0 ]; then
    note "If the player is pointed at the vendor's card, its alsa_device goes back"
    note "to '$PLAYER_FALLBACK_DEVICE'; every other setting in that file is kept."
fi
echo
if ! confirm "Remove it?"; then
    say "Nothing was changed"
    exit 0
fi

# ═════════════════════════════════════════════════════════════════════════════
# Taking it off
# ═════════════════════════════════════════════════════════════════════════════

say "Stopping the daemon"
if have systemctl; then
    if systemctl is-active --quiet "$DAEMON_SERVICE" 2>/dev/null; then
        run systemctl stop "$DAEMON_SERVICE"
        note "stopped:     $DAEMON_SERVICE"
    else
        note "not running: $DAEMON_SERVICE"
    fi
    if systemctl is-enabled --quiet "$DAEMON_SERVICE" 2>/dev/null; then
        run systemctl disable "$DAEMON_SERVICE" >/dev/null 2>&1 || \
            warn "could not disable it; removing its unit file below anyway"
        note "disabled:    $DAEMON_SERVICE"
    else
        note "not enabled: $DAEMON_SERVICE"
    fi
else
    note "no systemd here, so there is no service to stop"
fi

say "Removing the daemon's systemd files"
remove "/etc/systemd/system/$DAEMON_SERVICE.service"
remove "$PLAYER_DROPIN"

# The drop-in directory is the installer's too — it was created only to hold
# that one file. Left behind it makes systemd mention an empty drop-in directory
# on every reload, so it goes, but only if it is empty: anything else in there
# belongs to somebody else.
if [ -d "$PLAYER_DROPIN_DIR" ]; then
    if [ "$DRY_RUN" -eq 1 ]; then
        note "would remove $PLAYER_DROPIN_DIR if it is empty"
    elif rmdir "$PLAYER_DROPIN_DIR" 2>/dev/null; then
        note "removed:     $PLAYER_DROPIN_DIR (empty)"
    else
        note "kept:        $PLAYER_DROPIN_DIR (not empty — something else is in it)"
    fi
fi

if have systemctl; then
    run systemctl daemon-reload
    note "systemd reloaded"
fi

say "Unloading the kernel module"
if module_loaded; then
    if [ "$DRY_RUN" -eq 1 ]; then
        note "would unload $MODULE_NAME"
    # Both spellings are tried: the file is Digisyn-vSndCard.ko and lsmod prints
    # the underscore form, and modprobe accepts either.
    elif modprobe -r "$MODULE_NAME" 2>/dev/null || \
         modprobe -r "$MODULE_NAME_LSMOD" 2>/dev/null; then
        note "unloaded:    $MODULE_NAME"
    else
        warn "could not unload $MODULE_NAME — something still has its device open,"
        warn "which is usually the daemon. See what does:"
        warn "    sudo fuser -v /dev/$MODULE_NAME_LSMOD"
        warn "Its files are removed either way, and the module goes at the next reboot."
    fi
else
    note "not loaded:  $MODULE_NAME"
fi

say "Removing the daemon, its settings and its licence"
remove "$DAEMON"
remove "$DAEMON_CONF_DIR" -r
remove "$MODULE_LOAD_CONF"
remove "$BUILD_DIR" -r
remove_all "$SCRATCH_GLOB"

say "Removing the DKMS registration"
if have dkms; then
    _versions="$(dkms status 2>/dev/null | sed -n "s|^$DKMS_NAME/\([^,]*\).*|\1|p" | sort -u || true)"
    if [ -n "$_versions" ]; then
        for _v in $_versions; do
            if [ "$DRY_RUN" -eq 1 ]; then
                note "would remove the DKMS entry $DKMS_NAME/$_v"
            elif dkms remove -m "$DKMS_NAME" -v "$_v" --all >/dev/null 2>&1; then
                note "dkms:        removed $DKMS_NAME/$_v"
            else
                warn "dkms could not remove $DKMS_NAME/$_v — its source tree goes below"
            fi
        done
    else
        note "dkms:        nothing registered under $DKMS_NAME"
    fi
else
    note "dkms:        not installed here"
fi
remove_all "$DKMS_SRC_GLOB"

say "Removing the installed module"
_found_ko=0
for _ko in /lib/modules/*/extra/"$MODULE_NAME".ko; do
    [ -e "$_ko" ] || continue
    _found_ko=1
    remove "$_ko"

    # depmod has to be rerun for the kernel whose tree we just changed, and only
    # for that one — depmod -a with no argument rebuilds only the running one.
    _kern="${_ko#/lib/modules/}"
    _kern="${_kern%%/*}"
    if [ "$DRY_RUN" -eq 1 ]; then
        note "would run:  depmod -a $_kern"
    elif depmod -a "$_kern" >/dev/null 2>&1; then
        note "depmod:      $_kern"
    else
        warn "depmod -a $_kern failed — the module may still be listed by name"
    fi

    # The extra/ directory is the installer's too, if nothing else is in it.
    _extra="$(dirname "$_ko")"
    if [ "$DRY_RUN" -eq 0 ] && rmdir "$_extra" 2>/dev/null; then
        note "removed:     $_extra (empty)"
    fi
done
[ "$_found_ko" -eq 1 ] || note "no installed .ko found under /lib/modules/*/extra"

say "Pointing the player back at its own output"
if [ "$KEEP_PLAYER_CONFIG" -eq 1 ]; then
    note "--keep-player-config was given, so the player's settings are untouched."
elif [ ! -f "$PLAYER_CONFIG" ]; then
    note "there is no $PLAYER_CONFIG, so there is nothing to put back"
elif ! have python3; then
    warn "python3 is not here, so the player's settings were not edited. Editing"
    warn "them by hand is the only option, and the reason this script will not do"
    warn "it without a JSON parser is that the other settings have to survive:"
    warn "    set \"alsa_device\": \"$PLAYER_FALLBACK_DEVICE\" in $PLAYER_CONFIG"
else
    _device="$(player_device)"
    _points_at_vendor=0

    # Two ways of recognising the vendor's card, because by now it is very likely
    # gone from /proc/asound/cards and only the string in the file is left.
    case "$_device" in
        *Digisyn*|*digisyn*) _points_at_vendor=1 ;;
    esac
    if [ "$_points_at_vendor" -eq 0 ] && [ -n "$VENDOR_CARD_IDS" ]; then
        _card="$(device_card_id "$_device")"
        for _id in $VENDOR_CARD_IDS; do
            if [ "$_card" = "$_id" ]; then _points_at_vendor=1; fi
        done
    fi

    if [ -z "$_device" ]; then
        note "the player has no alsa_device set — left alone"
    elif [ "$_points_at_vendor" -eq 0 ]; then
        note "the player is on '$_device', which is not the vendor's card — left alone"
    elif [ "$DRY_RUN" -eq 1 ]; then
        note "would set alsa_device from '$_device' to '$PLAYER_FALLBACK_DEVICE'"
    else
        # The same read-one-key, write-it-back pattern the installer and the
        # interface use, so every other setting — room name, cache folder, tunnel
        # token — comes through untouched.
        cp -a "$PLAYER_CONFIG" "$PLAYER_CONFIG.bak"
        note "kept the old config as $PLAYER_CONFIG.bak"
        python3 - "$PLAYER_CONFIG" "$PLAYER_FALLBACK_DEVICE" <<'PY'
import json, sys
path, device = sys.argv[1], sys.argv[2]
with open(path) as f:
    cfg = json.load(f)
was = cfg.get("alsa_device", "(unset)")
cfg["alsa_device"] = device
with open(path, "w") as f:
    json.dump(cfg, f, indent=2)
print("alsa_device: %s -> %s" % (was, device))
PY
        note "alsa_device: '$_device' -> '$PLAYER_FALLBACK_DEVICE'"

        if have systemctl && systemctl is-active --quiet "$PLAYER_UNIT" 2>/dev/null; then
            run systemctl restart "$PLAYER_UNIT"
            note "player restarted"
        else
            note "the player is not running under systemd — start it when ready"
        fi
    fi
fi

# ═════════════════════════════════════════════════════════════════════════════
# The same list again, quiet this time
# ═════════════════════════════════════════════════════════════════════════════
if [ "$DRY_RUN" -eq 1 ]; then
    say "Dry run — nothing was changed, so everything above is still on the box"
    note "Run again without --dry-run to do it."
    exit 0
fi

say "Checking the result"
CLEAN=1

# Each of these answers "is it still here?". The label says what it is; the
# command is one of the small predicates above, so the same definitions decide
# this as decided the report at the start.
check_gone() {
    local label="$1"; shift
    if "$@"; then
        printf '  \033[1;31mSTILL HERE\033[0m  %s\n' "$label"
        CLEAN=0
    else
        printf '  \033[1;32mgone\033[0m        %s\n' "$label"
    fi
}
scratch_present() {
    any_exists "$BUILD_DIR" "$SCRATCH_GLOB" "$DKMS_SRC_GLOB"
}

check_gone "the $DAEMON_SERVICE service"  service_known
check_gone "$DAEMON"                      present_file "$DAEMON"
check_gone "$DAEMON_CONF_DIR"             present_file "$DAEMON_CONF_DIR"
check_gone "$MODULE_LOAD_CONF"            present_file "$MODULE_LOAD_CONF"
check_gone "$PLAYER_DROPIN"               present_file "$PLAYER_DROPIN"
check_gone "the $MODULE_NAME module"      module_loaded
check_gone "/dev/$MODULE_NAME_LSMOD"      present_file "/dev/$MODULE_NAME_LSMOD"
check_gone "an ALSA card for it"          card_present
check_gone "a DKMS entry"                 dkms_registered
check_gone "build scratch in /var/tmp"    scratch_present

echo
if [ "$CLEAN" -eq 1 ]; then
    say "Digisynthetic's stack is gone"
else
    say "Something is still here"
    note "The lines marked STILL HERE are above. A module that would not unload is"
    note "the usual one, and it is not a problem: its files are already gone, so it"
    note "goes at the next reboot. Anything else is worth a second look."
fi

echo
say "Left alone, on purpose"
if lsmod 2>/dev/null | grep -q '^MergingRavennaALSA'; then
    note "MergingRavennaALSA — the open AES67 stack's kernel module"
fi
if have systemctl && systemctl is-active --quiet aes67-daemon 2>/dev/null; then
    note "aes67-daemon — the open AES67 stack's daemon"
fi
note "snd and snd-pcm — the HDMI output needs them too"
echo
note "Getting the sound back on the network is scripts/player/merging-aes67.sh,"
note "documented in docs/SATELLITE.md."

