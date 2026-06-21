#!/usr/bin/env bash
# Install the AudioPipe receiver as a launchd job that starts automatically and
# auto-restarts if it dies (spec §6.4 / §10).
#
#   ./receiver/install_launchd.sh            # per-user LaunchAgent (recommended)
#   ./receiver/install_launchd.sh --daemon   # system LaunchDaemon (boot, no login)
#
# Default is a LaunchAgent: it runs in your GUI login session, where CoreAudio /
# coreaudiod access is reliable. Pair it with auto-login (System Settings > Users
# & Groups > Automatically log in) to get "starts after reboot, no manual login".
#
# --daemon installs a system LaunchDaemon that starts at boot with no session.
# That satisfies §6.4's "boot, no login" literally, but a daemon has no user
# audio session, so opening the AudioPipe HAL device may fail. Prefer the agent
# unless you've confirmed audio works as a daemon on your macOS version.
set -euo pipefail
cd "$(dirname "$0")/.."          # repo root

MODE="agent"
[ "${1:-}" = "--daemon" ] && MODE="daemon"

LABEL="com.audiopipe.receiver"
TEMPLATE="receiver/com.audiopipe.receiver.plist"
SRC_BIN="build/receiver/audiopipe_receiver"
SRC_CFG="config/receiver.json"

if [ ! -x "$SRC_BIN" ]; then
    echo "Receiver not built. Run:  cmake --build build" >&2
    exit 1
fi

# Install the binary + config to stable locations so the job doesn't depend on
# the build tree.
INSTALL_BIN="/usr/local/bin/audiopipe_receiver"
INSTALL_CFG_DIR="/usr/local/etc/audiopipe"
INSTALL_CFG="$INSTALL_CFG_DIR/receiver.json"

SUDO=""
[ "$MODE" = "daemon" ] && SUDO="sudo"

# The binary + config live under /usr/local (a root-owned system path on stock
# macOS, incl. Apple Silicon), so installing them always needs sudo -- regardless
# of agent vs daemon. Only the agent's plist/logs below are user-level.
sudo mkdir -p "$(dirname "$INSTALL_BIN")" "$INSTALL_CFG_DIR"
sudo cp "$SRC_BIN" "$INSTALL_BIN"
# Don't clobber an existing customized config.
if [ ! -f "$INSTALL_CFG" ]; then
    sudo cp "$SRC_CFG" "$INSTALL_CFG"
fi

if [ "$MODE" = "agent" ]; then
    PLIST_DIR="$HOME/Library/LaunchAgents"
    STDOUT="$HOME/Library/Logs/audiopipe-receiver.out.log"
    STDERR="$HOME/Library/Logs/audiopipe-receiver.err.log"
    DOMAIN="gui/$(id -u)"
else
    PLIST_DIR="/Library/LaunchDaemons"
    STDOUT="/var/log/audiopipe-receiver.out.log"
    STDERR="/var/log/audiopipe-receiver.err.log"
    DOMAIN="system"
fi
PLIST="$PLIST_DIR/$LABEL.plist"

mkdir -p "$(dirname "$STDOUT")" 2>/dev/null || true
$SUDO mkdir -p "$PLIST_DIR"

# Fill the template.
sed -e "s#__BIN__#$INSTALL_BIN#g" \
    -e "s#__CONFIG__#$INSTALL_CFG#g" \
    -e "s#__WORKDIR__#$INSTALL_CFG_DIR#g" \
    -e "s#__STDOUT__#$STDOUT#g" \
    -e "s#__STDERR__#$STDERR#g" \
    "$TEMPLATE" | $SUDO tee "$PLIST" >/dev/null

if [ "$MODE" = "daemon" ]; then
    sudo chown root:wheel "$PLIST"
    sudo chmod 644 "$PLIST"
fi

# Reload (bootout then bootstrap so re-running picks up changes).
# bootout is ASYNC: if the job is already loaded (e.g. you're re-running this),
# firing bootstrap immediately races the still-in-progress teardown and fails with
# "Bootstrap failed: 5: Input/output error". So wait until the job actually leaves
# the domain before bootstrapping.
$SUDO launchctl bootout "$DOMAIN/$LABEL" 2>/dev/null || true
for _ in $(seq 1 50); do
    $SUDO launchctl print "$DOMAIN/$LABEL" >/dev/null 2>&1 || break
    sleep 0.1
done
$SUDO launchctl enable "$DOMAIN/$LABEL" 2>/dev/null || true   # in case a prior run left it disabled
$SUDO launchctl bootstrap "$DOMAIN" "$PLIST"

echo "Installed $MODE: $PLIST"
echo "  binary: $INSTALL_BIN"
echo "  config: $INSTALL_CFG   (listen_port + device must match the sender)"
echo "  logs:   $STDOUT / $STDERR  (plus the log_file in the config)"
echo "Status:   launchctl print $DOMAIN/$LABEL | grep -E 'state|pid'"
