#!/usr/bin/env bash
# Stop and remove the AudioPipe receiver launchd job.
#
#   ./receiver/uninstall_launchd.sh            # remove the LaunchAgent
#   ./receiver/uninstall_launchd.sh --daemon   # remove the system LaunchDaemon
#
# Pass --purge to also delete the installed binary and config.
set -euo pipefail

MODE="agent"
PURGE=0
for a in "$@"; do
    [ "$a" = "--daemon" ] && MODE="daemon"
    [ "$a" = "--purge" ]  && PURGE=1
done

LABEL="com.audiopipe.receiver"
if [ "$MODE" = "agent" ]; then
    PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
    DOMAIN="gui/$(id -u)"
    SUDO=""
else
    PLIST="/Library/LaunchDaemons/$LABEL.plist"
    DOMAIN="system"
    SUDO="sudo"
fi

$SUDO launchctl bootout "$DOMAIN/$LABEL" 2>/dev/null || true
$SUDO rm -f "$PLIST"
echo "Removed $MODE job: $PLIST"

if [ "$PURGE" -eq 1 ]; then
    $SUDO rm -f /usr/local/bin/audiopipe_receiver
    $SUDO rm -rf /usr/local/etc/audiopipe
    echo "Purged /usr/local/bin/audiopipe_receiver and /usr/local/etc/audiopipe"
fi
