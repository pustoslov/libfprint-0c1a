#!/usr/bin/env bash
# Install this fork's built libfprint into /usr/local and make fprintd load it
# through a systemd drop-in. Does not replace distribution packages.
#
# Usage: sudo ./contrib/install-systemd.sh
set -euo pipefail

PROJ="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$PROJ/build}"
LIBDIR="${LIBDIR:-/usr/local/lib/libfprint-0c1a}"
DROPIN_DIR="${DROPIN_DIR:-/etc/systemd/system/fprintd.service.d}"
DROPIN="$DROPIN_DIR/override.conf"

die() { echo "error: $*" >&2; exit 1; }

if [ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ]; then
  echo "Usage: sudo $0"
  echo "Environment: BUILD_DIR, LIBDIR, DROPIN_DIR"
  exit 0
fi

[ "$(id -u)" -eq 0 ] || die "run as root: sudo $0"
command -v systemctl >/dev/null 2>&1 || die "systemd not found; use the manual install in README.md"
systemctl cat fprintd.service >/dev/null 2>&1 || die "fprintd.service not found; install fprintd first"

MAIN="$BUILD_DIR/libfprint/libfprint-2.so.2.0.0"
TOD="$BUILD_DIR/libfprint/tod/libfprint-2-tod.so.1"
[ -f "$MAIN" ] || die "missing $MAIN; build first (see README.md)"
[ -f "$TOD" ] || die "missing $TOD; build with -Dtod=true"

FPRINTD_BIN="$(systemctl show fprintd.service -p ExecStart --value 2>/dev/null \
  | sed -n 's/.*path=\([^ ;]*\).*/\1/p' | head -n1 || true)"
if [ -z "$FPRINTD_BIN" ] || [ ! -x "$FPRINTD_BIN" ]; then
  FPRINTD_BIN="$(command -v fprintd || true)"
fi

FPV=""
if command -v dpkg-query >/dev/null 2>&1; then
  FPV="$(dpkg-query -W -f='${Version}' fprintd 2>/dev/null || true)"
elif command -v rpm >/dev/null 2>&1; then
  FPV="$(rpm -q --qf '%{VERSION}-%{RELEASE}' fprintd 2>/dev/null || true)"
fi
if [ -n "$FPV" ]; then
  echo "Detected fprintd: $FPV (this fork was tested with 1.94.x)"
  case "$FPV" in
    1.94*) ;;
    *) echo "warning: fprintd version differs from the tested 1.94.x" ;;
  esac
fi

echo "Installing libraries to $LIBDIR"
install -d -m 0755 "$LIBDIR"
install -m 0755 "$MAIN" "$LIBDIR/"
install -m 0755 "$TOD" "$LIBDIR/"
ln -sfn libfprint-2.so.2.0.0 "$LIBDIR/libfprint-2.so.2"

# Optional: clean a build-tree RUNPATH from the installed copies.
if command -v patchelf >/dev/null 2>&1; then
  patchelf --set-rpath '$ORIGIN' "$LIBDIR/libfprint-2.so.2.0.0" || true
  patchelf --set-rpath '$ORIGIN' "$LIBDIR/libfprint-2-tod.so.1" || true
fi

if command -v selinuxenabled >/dev/null 2>&1 && selinuxenabled; then
  if command -v restorecon >/dev/null 2>&1; then
    restorecon -R "$LIBDIR" || true
  fi
fi

echo "Installing systemd drop-in $DROPIN"
install -d -m 0755 "$DROPIN_DIR"
cat > "$DROPIN" <<DROPIN_EOF
[Service]
Environment=LD_LIBRARY_PATH=$LIBDIR
DROPIN_EOF
chmod 0644 "$DROPIN"

# fprintd can be stuck in a driver operation; do not block on systemctl stop.
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
systemctl daemon-reload
systemctl start --no-block fprintd 2>/dev/null || true

echo
echo "Loaded fprintd library path:"
if [ -n "$FPRINTD_BIN" ] && [ -x "$FPRINTD_BIN" ] && command -v ldd >/dev/null 2>&1; then
  echo "  $FPRINTD_BIN"
  LD_LIBRARY_PATH="$LIBDIR" ldd "$FPRINTD_BIN" | grep -E 'libfprint|not found' || true
else
  echo "  (could not locate the fprintd binary; check: systemctl status fprintd)"
fi

cat <<'MSG'

Installed. Re-enroll your finger as your desktop user:
  fprintd-delete "$USER"
  fprintd-enroll -f right-index-finger "$USER"
  fprintd-verify "$USER"

Existing templates are matcher-specific and will not work with the stock
libfprint after uninstall.

Uninstall: sudo ./contrib/uninstall-systemd.sh
MSG
