# OSCam-mini integration

OSCam-mini is a standalone third-party module used only as a local Phoenix/Smartmouse -> Newcamd card server.
The TVStreammerSAT5 UI is exposed at `/oscam-mini`.

Vendored source must live in `third_party/oscam-mini`. Run `scripts/vendor_oscam_mini.ps1` once on Windows before committing if this directory contains only README.VENDOR. After vendoring, normal CMake builds do not access the network.

The build script configures only:
- MODULE_NEWCAMD
- READER_IRDETO
- READER_VIACCESS
- CARDREADER_PHOENIX

Runtime configuration lives only in `/opt/TVStreammerSAT5/oscam-mini/config`.
