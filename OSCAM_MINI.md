# OSCam-mini integration

OSCam-mini is a standalone third-party card-server module for TVStreammerSAT5. It is used locally as Phoenix/Smartmouse -> Newcamd and is managed from the TVStreammerSAT5 web interface at `/oscam-mini`.

## Source layout

The OSCam sources are vendored in the main repository under:

```text
third_party/oscam-mini/
```

A normal server build must **not** clone or download OSCam. The complete vendored tree must already be present after `git pull`.

The upstream revision used for the vendor snapshot is recorded by `third_party/oscam-mini/UPSTREAM_COMMIT` when present. Keep the upstream `COPYING` license file with the vendored source.

## Minimal feature set

Before building, `scripts/build_oscam_mini.sh` disables all OSCam options and enables only:

- `MODULE_NEWCAMD`
- `READER_IRDETO`
- `READER_VIACCESS`
- `CARDREADER_PHOENIX`

The build is driven through the vendored OSCam `CMakeLists.txt`. The root `third_party/oscam-mini/Makefile` is retained as a compatibility/manual-build wrapper, but the TVStreammerSAT5 build does not depend on its executable permissions.

## Automatic project build

`oscam-mini` is an `ALL` CMake target. A normal build therefore builds both TVStreammerSAT5 and OSCam-mini:

```bash
rm -rf build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

For OSCam-only diagnostics use:

```bash
rm -rf build/oscam-mini
cmake --build build --target oscam-mini -j1
```

Expected output binary:

```text
build/oscam-mini/oscam-mini
```

CMake explicitly starts the helper with `/usr/bin/env bash`, so losing the executable bit while copying a ZIP through Windows no longer causes `Permission denied`.

## Installation

```bash
sudo cmake --install build
sudo /opt/TVStreammerSAT5/oscam-mini/install_oscam_mini.sh
```

Installed binary:

```text
/opt/TVStreammerSAT5/oscam-mini/oscam-mini
```

Runtime configuration is stored **only** inside the application directory:

```text
/opt/TVStreammerSAT5/oscam-mini/config/oscam.conf
/opt/TVStreammerSAT5/oscam-mini/config/oscam.server
/opt/TVStreammerSAT5/oscam-mini/config/oscam.user
```

`cmake --install` installs templates to `default-config`; `install_oscam_mini.sh` copies a template into `config` only when the corresponding runtime file does not already exist. Existing settings are preserved.

The installer stops the legacy `oscam.service`/`oscam` process before starting `oscam-mini.service`, preventing two processes from opening the same Phoenix serial reader.

## Web UI

Open:

```text
http://SERVER:TVSTREAMER_PORT/oscam-mini
```

The OSCam-mini module provides service status/start/stop/restart, Newcamd settings, Phoenix reader configuration and service log/status information.
