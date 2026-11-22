#!/usr/bin/env bash
# Remove the systemd drop-in and /usr/local libraries installed by
# install-systemd.sh. Distribution packages are not touched.
set -euo pipefail

LIBDIR="${LIBDIR:-/usr/local/lib/libfprint-0c1a}"
DROPIN_DIR="${DROPIN_DIR:-/etc/systemd/system/fprintd.service.d}"
DROPIN="$DROPIN_DIR/override.conf"

die() { echo "error: $*" >&2; exit 1; }

[ "$(id -u)" -eq 0 ] || die "run as root: sudo $0"
command -v systemctl >/dev/null 2>&1 || die "systemd not found"

systemctl stop --no-block fprintd 2>/dev/null || true
if command -v pkill >/dev/null 2>&1; then
  pkill -9 -x fprintd 2>/dev/null || true
  for ((i = 0; i < 30; i++)); do
    pgrep -x fprintd >/dev/null 2>&1 || break
    sleep 0.1
  done
  pkill -9 -x fprintd 2>/dev/null || true
fi
systemctl reset-failed fprintd 2>/dev/null || true

rm -f "$DROPIN"
rmdir "$DROPIN_DIR" 2>/dev/null || true
rm -rf "$LIBDIR"

systemctl daemon-reload
systemctl start --no-block fprintd 2>/dev/null || true

cat <<'MSG'
Removed the drop-in and the patched libraries. The stock distribution
libfprint is in use again.

Re-enroll, because templates created by this fork use SIFT while the
stock matcher uses NBIS:
  fprintd-delete "$USER"
  fprintd-enroll -f right-index-finger "$USER"
MSG
