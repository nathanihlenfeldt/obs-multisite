#!/usr/bin/env bash
#
# install.sh — turn a stock Raspberry Pi OS install into a campus player.
#
#   curl -fsSL https://raw.githubusercontent.com/StageAudioWorks/obs-multisite/main/scripts/player/install.sh | sudo bash
#
# It installs the build dependencies, builds the player, installs it as a
# service that starts on power-up, and leaves the box showing a screen with its
# own address on it so somebody can finish the job from a phone.
#
# Safe to run again: it updates an existing installation in place and keeps the
# settings and the segment cache.

set -euo pipefail

REPO_URL="${REPO_URL:-https://github.com/StageAudioWorks/obs-multisite.git}"
BRANCH="${BRANCH:-main}"
SRC_DIR="${SRC_DIR:-/opt/multisite-player/src}"
PREFIX="${PREFIX:-/usr/local}"
CONFIG_DIR="/etc/multisite-player"
CONFIG="$CONFIG_DIR/config.json"
STATE_DIR="/var/lib/multisite-player"
SERVICE="multisite-player"
JOBS="${JOBS:-$(nproc)}"

say()  { printf '\n\033[1;36m==>\033[0m \033[1m%s\033[0m\n' "$*"; }
note() { printf '    %s\n' "$*"; }
warn() { printf '\033[1;33m    %s\033[0m\n' "$*"; }
die()  { printf '\n\033[1;31mThat did not work:\033[0m %s\n\n' "$*" >&2; exit 1; }

[ "$(id -u)" -eq 0 ] || die "run this with sudo."

# ── What are we installing onto? ─────────────────────────────────────────────
MODEL="$(tr -d '\0' < /sys/firmware/devicetree/base/model 2>/dev/null || echo 'unknown machine')"
say "Installing the campus player on: $MODEL"

case "$(uname -m)" in
  aarch64|arm64|x86_64) ;;
  *) warn "This has only been tested on 64-bit ARM and x86. Carrying on anyway." ;;
esac

if [ -n "${DISPLAY:-}" ] || systemctl is-active --quiet lightdm 2>/dev/null \
   || systemctl is-active --quiet gdm3 2>/dev/null; then
  warn "A desktop is running on this box."
  warn "The player takes over the HDMI output directly and cannot share it"
  warn "with a desktop. Raspberry Pi OS Lite is the right image for a campus"
  warn "box. To carry on here, stop the desktop first:"
  warn "    sudo systemctl disable --now lightdm"
fi

# ── Dependencies ─────────────────────────────────────────────────────────────
say "Installing what it needs to build"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y --no-install-recommends \
  build-essential cmake pkg-config git ca-certificates \
  libcurl4-openssl-dev libssl-dev \
  libavformat-dev libavcodec-dev libavutil-dev libswresample-dev libswscale-dev \
  libdrm-dev libasound2-dev \
  >/dev/null
note "done"

# ── Source ───────────────────────────────────────────────────────────────────
if [ -d "$SRC_DIR/.git" ]; then
  say "Updating the source"
  git -C "$SRC_DIR" fetch --quiet origin "$BRANCH"
  git -C "$SRC_DIR" reset --quiet --hard "origin/$BRANCH"
else
  say "Fetching the source"
  mkdir -p "$(dirname "$SRC_DIR")"
  git clone --quiet --depth 1 --branch "$BRANCH" "$REPO_URL" "$SRC_DIR"
fi
note "$(git -C "$SRC_DIR" log -1 --format='%h %s')"

# ── Build ────────────────────────────────────────────────────────────────────
say "Building (this takes a few minutes on a Pi)"
cmake -S "$SRC_DIR" -B "$SRC_DIR/build" \
      -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_PLAYER=ON \
      -DCMAKE_INSTALL_PREFIX="$PREFIX" >/dev/null
cmake --build "$SRC_DIR/build" --target multisite-player -j "$JOBS" >/dev/null
cmake --install "$SRC_DIR/build" >/dev/null 2>&1 || {
  # Only the player is wanted here, not every optional target the project can
  # produce.
  install -m 0755 "$SRC_DIR/build/multisite-player" "$PREFIX/bin/multisite-player"
  mkdir -p "$PREFIX/share/multisite-player/web"
  cp -r "$SRC_DIR/src/appliance/web/." "$PREFIX/share/multisite-player/web/"
}
note "installed $($PREFIX/bin/multisite-player --version)"

# ── Where the cache goes ─────────────────────────────────────────────────────
# The cache writes roughly 3 GB an hour. On an SD card that is a wear-out
# problem, not a performance one, so a USB SSD is looked for and used.
say "Choosing where to keep the downloaded service"
CACHE_DIR="$STATE_DIR/cache"
SSD_MOUNT=""
while read -r src target fstype _; do
  case "$src" in
    /dev/sd*|/dev/nvme*)
      case "$target" in
        /|/boot*) ;;
        *) [ "$fstype" != "vfat" ] && SSD_MOUNT="$target" && break ;;
      esac ;;
  esac
done < /proc/self/mounts

if [ -n "$SSD_MOUNT" ]; then
  CACHE_DIR="$SSD_MOUNT/multisite-player/cache"
  note "using the drive mounted at $SSD_MOUNT"
else
  ROOT_SRC="$(findmnt -n -o SOURCE / 2>/dev/null || true)"
  case "$ROOT_SRC" in
    *mmcblk*)
      warn "No USB drive found, so the cache will go on the SD card."
      warn "That writes about 3 GB an hour and will wear the card out."
      warn "Plug in a USB SSD and change the cache folder in Settings." ;;
  esac
fi
mkdir -p "$CACHE_DIR" "$STATE_DIR"

# ── Settings ─────────────────────────────────────────────────────────────────
mkdir -p "$CONFIG_DIR"
chmod 0700 "$CONFIG_DIR"
if [ -f "$CONFIG" ]; then
  say "Keeping the settings already on this box"
else
  say "Writing a starting set of settings"
  cat > "$CONFIG" <<EOF
{
  "room_id": "main-auditorium",
  "cache_dir": "$CACHE_DIR",
  "web_port": 8080,
  "idle_mode": "splash",
  "auto_play": true,
  "audio_enabled": true,
  "alsa_device": "default",
  "buffer_minutes": 10
}
EOF
  chmod 0600 "$CONFIG"
  note "the storage details still need entering — from a browser, in a moment"
fi

# ── Service ──────────────────────────────────────────────────────────────────
say "Setting it to start on power-up"
install -m 0644 "$SRC_DIR/scripts/player/$SERVICE.service" \
        "/etc/systemd/system/$SERVICE.service"
systemctl daemon-reload
systemctl enable --quiet "$SERVICE"
systemctl restart "$SERVICE"

# The console cursor and kernel messages would otherwise blink over the
# picture the player is putting on the screen.
if [ -w /sys/class/graphics/fb0/blank ] 2>/dev/null; then
  printf 0 > /sys/class/graphics/fb0/blank || true
fi
if ! grep -q 'consoleblank=0' /boot/firmware/cmdline.txt 2>/dev/null; then
  warn "To stop the console blanking the screen, add consoleblank=0 to"
  warn "/boot/firmware/cmdline.txt and reboot."
fi

sleep 2
if ! systemctl is-active --quiet "$SERVICE"; then
  warn "The service did not stay running. What it said:"
  journalctl -u "$SERVICE" -n 25 --no-pager || true
  die "see above"
fi

# ── Where to go next ─────────────────────────────────────────────────────────
PORT="$(grep -o '"web_port"[[:space:]]*:[[:space:]]*[0-9]*' "$CONFIG" \
        | grep -o '[0-9]*$' || echo 8080)"
say "Done. Finish setting it up from a browser on this network:"
ip -4 -o addr show scope global 2>/dev/null \
  | awk -v p="$PORT" '{split($4,a,"/"); printf "        http://%s:%s   (%s)\n", a[1], p, $2}'
echo
note "The same address is on the screen attached to this box."
note "Enter the bucket details under Settings, then press Play."
echo
note "If something is wrong:  journalctl -u $SERVICE -f"
note "To update it later:     sudo bash $SRC_DIR/scripts/player/install.sh"
echo
