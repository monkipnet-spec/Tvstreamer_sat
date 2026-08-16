# OSCam-mini integration

OSCam-mini is a standalone third-party card-server module for TVStreammerSAT5. It is used locally as Phoenix/Smartmouse -> Newcamd and is managed from the TVStreammerSAT5 web interface at `/oscam-mini`.

## Source layout

The complete OSCam source tree is vendored in the main repository under:

```text
third_party/oscam-mini/
```

A normal server build does not clone or download OSCam. The upstream revision is recorded by `third_party/oscam-mini/UPSTREAM_COMMIT`; keep the upstream `COPYING` file with the source.

## Minimal feature set

`scripts/build_oscam_mini.sh` disables all OSCam features and enables only:

- `MODULE_NEWCAMD`
- `READER_IRDETO`
- `READER_VIACCESS`
- `CARDREADER_PHOENIX`

The helper is invoked explicitly with `/usr/bin/env bash`, so a ZIP unpacked on Windows cannot break the build merely by losing the executable bit.

## Automatic build

```bash
rm -rf build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

The normal `ALL` target creates:

```text
build/oscam-mini/oscam-mini
```

For diagnostics:

```bash
rm -rf build/oscam-mini
cmake --build build --target oscam-mini -j1
```

## Installation

```bash
sudo cmake --install build
sudo bash /opt/TVStreammerSAT5/oscam-mini/install_oscam_mini.sh
```

`cmake --install` now installs the systemd unit directly to:

```text
/etc/systemd/system/oscam-mini.service
```

The installer performs `systemctl daemon-reload`, stops/disables legacy OSCam so it cannot hold the same Phoenix reader, and enables/starts `oscam-mini.service`.

Runtime configuration exists only in:

```text
/opt/TVStreammerSAT5/oscam-mini/config/oscam.conf
/opt/TVStreammerSAT5/oscam-mini/config/oscam.server
/opt/TVStreammerSAT5/oscam-mini/config/oscam.user
```

Existing runtime configuration is never overwritten during installation. Templates are stored separately in `default-config`.

## Multiple Newcamd users and ports

The OSCam-mini page supports multiple Newcamd accounts. Each UI entry has:

- username and password;
- TCP port;
- CAID;
- Provider/Ident;
- reader groups;
- AU/EMM flag.

TVStreammerSAT5 automatically generates the OSCam Newcamd port list, for example:

```ini
[newcamd]
serverip = 127.0.0.1
port = 4004@0652:0406BE;4005@0652:0400DC
key = 0102030405060708091011121314
keepalive = 1
```

and creates one `[account]` section per user in `oscam.user`, including `caid`, `ident`, `group` and `allowedprotocols = newcamd`. The UI rejects duplicate ports and duplicate usernames.

Important OSCam detail: Newcamd listening ports are server endpoints, while OSCam accounts are not intrinsically bound to one TCP port. TVStreammerSAT5 treats each account/port pair as one configured endpoint and additionally restricts that account by CAID/Provider/group. Configure TVStreamer clients with the corresponding port and account shown in the UI.

Legacy single-account configurations are read automatically and shown as the first user entry; additional port definitions are also imported into the UI.

## Web UI

Open:

```text
http://SERVER:TVSTREAMER_PORT/oscam-mini
```

The module provides service controls, multiple Newcamd endpoints/accounts, Phoenix reader settings, validation, process status and journal output. Backend errors are shown verbatim in the page instead of only displaying a generic `Ошибка`.
