# libfprint fork for ELAN 04f3:0c1a

[![build](https://github.com/pustoslov/libfprint-0c1a/actions/workflows/build.yml/badge.svg?branch=elan-0c1a)](https://github.com/pustoslov/libfprint-0c1a/actions/workflows/build.yml)

Experimental fork of libfprint 1.94.7 adding support for the ELAN
`04f3:0c1a` fingerprint sensor (Xiaomi Mi Notebook Pro 15.6, TM1703).

**This is not upstream libfprint.** It has not been reviewed by the
libfprint maintainers. Keep a password fallback.

## Status

Working on one 04f3:0c1a unit under Ubuntu 24.04 with fprintd 1.94.x.
A small sample of 17 genuine presses scored 5..19 consistent SIFT pairs,
while 40 impostor presses from other fingers reached at most 4. The match
threshold is set to 9: in that sample 15 of 17 genuine presses passed.
Expect occasional false rejections.

Tested hardware: Xiaomi Mi Notebook Pro 15.6 (TM1703), sensor `04f3:0c1a`.
Other ELAN sensors are not supported by this branch.

## What changed

- Poll the pre-scan command (`0x40 0x3f`). Firmware 0x0142 answers it
  immediately with `0xaf` instead of blocking until a finger is placed.
- Wait for the finger to lift before calibrating, so it is not baked into
  the sensor baseline.
- Capture in press mode: four frames, one settled frame is matched.
- Replace NBIS minutiae matching with SIFT keypoints and a rigid-placement
  matcher derived from the `elanpress` driver (see NOTICE).
- Store only SIFT keypoints/descriptors in templates, never images.
- Reject low-contrast captures and ask for another press.
- Use a threshold of 9 consistent pairs for this sensor.

## Privacy

Templates in `/var/lib/fprint` contain SIFT features only (coordinates,
angles, scales, 128-byte descriptors). This fork does not store or log
fingerprint images. Upstream test fixtures under `examples/prints/`,
`tests/*/capture.png` and Ubuntu's prebuilt fake TOD test drivers under
`tests/tod-drivers/` are public test data shipped with the source, not
user data. The patched library itself is always built from source.

## Requirements

- Linux with systemd for the install script (tested on Ubuntu 24.04).
- fprintd 1.94.x, ABI-compatible with libfprint 1.94.
- Build dependencies, per distribution.

Ubuntu / Debian:

```sh
sudo apt install meson ninja-build pkg-config gcc \
  libglib2.0-dev libusb-1.0-0-dev libgudev-1.0-dev libgusb-dev \
  libjson-glib-dev libudev-dev libpixman-1-dev libssl-dev libcairo2-dev
```

Fedora:

```sh
sudo dnf install meson ninja-build gcc pkgconf-pkg-config \
  glib2-devel libusb1-devel libgudev-devel libgusb-devel \
  json-glib-devel systemd-devel pixman-devel openssl-devel cairo-devel
```

Arch:

```sh
sudo pacman -S meson ninja gcc pkgconf glib2 libusb libgudev libgusb \
  json-glib systemd pixman openssl cairo
```

## Build

```sh
meson setup build -Ddrivers=elan -Dintrospection=false -Ddoc=false \
  -Dinstalled-tests=false -Dtod=true -Dudev_hwdb=disabled
meson compile -C build
```

`-Dudev_hwdb=disabled` keeps the build independent of `udev.pc`, which on
Debian/Ubuntu is shipped by `systemd-dev`, not `libudev-dev`. It is not needed
for the `elan` driver: systemd 248 and later already ship the libfprint
autosuspend hwdb. Drop the option if you need libfprint to install its own
hwdb (systemd older than 248) or if you build the `elanspi` driver, which
requires udev rules.

Optional tests:

```sh
meson test -C build --print-errorlogs
```

## Install (systemd)

The install script does not replace distribution packages. It copies the
built libraries to `/usr/local/lib/libfprint-0c1a` and gives `fprintd` an
`LD_LIBRARY_PATH` drop-in.

```sh
sudo ./contrib/install-systemd.sh
```

Then re-enroll, because the templates are matcher-specific:

```sh
fprintd-delete "$USER"
fprintd-enroll -f right-index-finger "$USER"
fprintd-verify "$USER"
```

Uninstall:

```sh
sudo ./contrib/uninstall-systemd.sh
```

The script is expected to work on systemd distributions beyond Ubuntu, but
only Ubuntu 24.04 has been tested. It refuses to run when `fprintd.service`
is missing. SELinux systems run `restorecon` when available.

### Non-systemd or manual install

On systems without systemd, either run `fprintd` manually with
`LD_LIBRARY_PATH=/usr/local/lib/libfprint-0c1a`, or install over the
distribution library:

```sh
meson setup build --prefix=/usr -Ddrivers=elan -Dintrospection=false \
  -Ddoc=false -Dinstalled-tests=false -Dtod=true -Dudev_hwdb=disabled
meson compile -C build
sudo meson install -C build
```

Replacing the distribution library is what the `elanpress` fork documents;
package updates may overwrite it. The drop-in method above is reversible
and does not touch packaged files.

## Debug and threshold tuning

To see per-press metadata (no images), add to the fprintd drop-in:

```ini
[Service]
Environment=G_MESSAGES_DEBUG=all
```

Then:

```sh
journalctl -u fprintd -f
```

Relevant lines contain `press image`, `sift image quality`, `sift keypoints`
and `sift best pairs N/9`. The threshold is
`ELANPRESS_MATCH_MIN_PAIRS` in `libfprint/drivers/elanpress-match.h`.
Lower it only after collecting impostor scores from other fingers.

## License and attribution

LGPL-2.1-or-later. See `COPYING` and `NOTICE`.

The SIFT implementation and rigid-placement matcher in
`libfprint/drivers/elanpress-sift.[ch]` and `elanpress-match.[ch]` are
derived from the `elanpress` driver by Filip Spanne
(<https://github.com/filip-rs/libfprint>). The custom image matcher API is
inspired by the `libfprint-elan-0c28-patch` experimental package.

Upstream libfprint: <https://gitlab.freedesktop.org/libfprint/libfprint>
