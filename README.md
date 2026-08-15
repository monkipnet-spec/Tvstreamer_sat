# TVStreammerSAT5 — Release 29


### Selected-PMT PCR lock + wire-rate diagnostics (v143)

Release 29 fixes StableUdpOutput PCR selection for shared DVB inputs. During the five-second reservoir warmup the shared frontend can briefly expose PCR-bearing packets from other services before the selected-service PID filter is fully healed. Previous releases locked the periodic 20 ms PCR generator to the first PCR packet seen, which could select the wrong service (for example `pcr_pid=461` while SID 470 PMT declares PCR PID 471). StableUdpOutput now reads the selected program PMT, accepts its declared PCR PID as authoritative, and locks/strips PCR only on that PID. The exact five-second WISI startup reservoir, 7x188 UDP packetization and final all-PID continuity normalization are unchanged.

`UDP shaper stats` also reports `wire=`, `sent_datagrams=` and `send_errors=`. `real=` remains the useful non-NULL TS bitrate, while `wire=` is the actual UDP MPEG-TS payload bitrate accepted by `sendto()`. This makes receiver/network loss directly distinguishable from application-side continuity errors. `ts_missing=` continues to mean GstBuffer PTS/DTS metadata was absent; it is not a lost-TS-packet counter.


### Global stable-UDP continuity + DVB release barrier (v142)

Release 28 applies the final post-PCR continuity normalizer to every StableUdpOutput stream, with Remap ON or OFF and for both IP and DVB inputs. The stable sender is itself a new paced transport domain (five-second reservoir, periodic PCR insertion and 7x188 UDP packetization), so limiting final CC repair to Remap ON left the non-remapped DVB path exposed to the same analyzer errors. SDT Service/Provider regeneration remains gated by Remap ON; only continuity normalization is global. `UDP shaper stats` therefore reports `final_cc_verify_errors` for non-remapped streams as well.

The DVB frontend shutdown path now has an explicit release barrier. When the last consumer stops, the frontend key is marked `releasing` until GstDvbSrc has reached NULL and been unreffed. A new tile that immediately tunes the same physical frontend waits up to 2.5 seconds for that release instead of racing the old source and failing with `Device or resource busy`. If an already-running stream still owns the frontend on another transponder, the start is rejected before opening the device with a clear message: one physical frontend can share only the same transponder; another satellite/transponder needs the current consumer stopped or another physical frontend.


### Fast DVB release + unified post-remap normalization (v141)

Release 27 makes DVB adapter release start immediately when a tile is stopped. The main stream pipeline is put into NULL and the shared physical frontend is released before waiting for bus threads, external outputs or transcoders. The localhost service relay is torn down afterwards. When other channels still share the same frontend, the PID union is now shrunk live without the old READY→PLAYING cycle and its 2-second wait; the tuner therefore stays on-air for the remaining services without blocking tile shutdown. For the last consumer, the DVB release wait is bounded to 250 ms and the log reports `adapter_release_ms=...`.

Startup logging now begins with the product name, release/version and support email, for example `TVStreammerSAT5 Release 27 / v141 | support=monkipnet@gmail.com`. Version strings are centralized in `src/AppVersion.h` so the HTTP API/About dialog and startup log use the same version.

Generic IP remap already receives a fresh MPEG-TS continuity/PSI domain from `mpegtsmux`. DVB packet-level remap cannot safely be routed through that same demux/remux path for scrambled/private streams, so v141 applies the equivalent final normalization after packet reassembly and immediately before network send: upstream discontinuity flags are absorbed and cleared, CC is regenerated for every final PID after periodic-PCR insertion, an internal verifier checks the exact datagrams being sent, and SDT Service/Provider is regenerated from the configured remap metadata. `UDP shaper stats` now exposes `final_cc_verify_errors`, `final_cc_rewrites`, `final_cc_discontinuities_cleared` and `final_sdt_rewrites`. A non-zero analyzer CC count with `final_cc_verify_errors=0` means packets were lost after `sendto()` (network/receiver path), not inside the remapper.


### Remap transport/CBR/SDT final-output repair (v140)

Release 26 fixes three issues observed on the remapped DVB output. StableUdpOutput now preserves incomplete MPEG-TS packet tails across GstBuffer boundaries instead of discarding them, eliminating real packet loss that appeared as CC errors after remap. For Remap ON, a final continuity pass is also performed on the finished 7x188 datagram after periodic-PCR insertion and immediately before sendto(); the five-second WISI startup reservoir, 20 ms PCR cadence and pacing are unchanged.

DVB Remap now reapplies the configured output Service Name and Provider to the final SDT immediately before StableUdpOutput and logs the exact configured SID/service/provider values. The web editor CBR checkbox is now authoritative for UDP: checking it selects UDP CBR, unchecking selects UDP VBR, and Target bitrate is enabled only for CBR. Saving an active stream continues to hard-restart it through the existing config-save path so the new mode and PSI metadata take effect.

Expected diagnostics include `DVB final SDT remap: ...`, `UDP final TS continuity guard: remap=on stage=pre-send ...`, and `Unified UDP periodic-PCR reservoir TS shaper: mode=CBR target_bitrate=...` when CBR is enabled.

### Final DVB remap continuity guard (v139)

Release 25 added a pre-output continuity guard for packet-level DVB remap. v140 keeps it as an earlier validation stage and adds the definitive post-PCR/pre-send guard.

### DVB remap continuity-counter repair (v138)

Release 24 regenerated continuity counters after packet-level SID/PID/PSI rewriting.

### Build hotfix: stopStreamAsync declaration (v137)

Release 23 declared the existing `StreamManager::stopStreamAsync()` implementation in the public class interface.


### PhoenixSerialTransport lifecycle layer (v136)

Release 22 moves Phoenix/SmartMouse serial handling into `src/ca/PhoenixSerialTransport.{h,cpp}`. The transport owns exclusive-open/lock, DCD card detection, `termios2`/`BOTHER` custom baud profiles, RTS/DTR reset pulses, bounded ATR acquisition through `poll()`, restoration of the original serial state, reconnect and a one-shot probe helper. It intentionally exposes no generic APDU, control-word or descrambling API. `PhoenixManager` now uses this transport for reader probing instead of keeping a second low-level serial implementation in the UI/discovery layer.

The known FTDI readers retain the 6.00 MHz initial profile (`16129 baud`, Fi=372) with `9600` fallback. Unknown readers probe the ISO-default profile first. Unlike the earlier experimental snippets, the transport does not open the tty with `O_NONBLOCK`; read readiness is controlled with `poll()`, avoiding immediate `EAGAIN`/false ATR failures. The selected reader is still independent and is never automatically balanced to another card.

The v135 `CaBackend` ABI remains source/ABI-compatible. Official/operator backends continue to receive the stable reader path and can use their own authorised SDK. DVB shared-frontend/SPTS/remap and `StableUdpOutput.cpp` are unchanged.


### Pluggable in-process CaBackend API (v135)

Release 21 adds a complete in-process `CaBackend` plugin boundary for authorised local Conditional-Access integrations. The application discovers `.so` backends from `/opt/tvstreammersat5/ca-plugins` (or `TVSTREAMMERSAT5_CA_PLUGIN_DIR`), validates a versioned C ABI, opens a physical reader once, starts/stops per-SID service sessions, and sends the already selected DVB SPTS through an in-process MPEG-TS callback. There is no network CA server, no raw control-word getter/setter and no external key-export API.

Each Phoenix reader now has a persistent `backend_id`; the web Phoenix panel shows a Backend selector populated from the actually loaded plugins. The safe built-in default is `passthrough`, which deliberately leaves encrypted packets unchanged. A real decoding backend therefore has to be supplied as an authorised operator/manufacturer plugin implementing `src/ca/CaBackendPluginApi.h`. Per-reader opaque plugin settings are stored as `backend_config` in `ca_readers` and passed only to the selected plugin.

The new files are `src/CaBackend.h`, `src/CaBackend.cpp`, `src/ca/CaBackendPluginApi.h`, the ABI documentation `docs/CA_BACKEND_PLUGIN_API.md`, and a deliberately non-decoding example plugin in `examples/ca_backend_passthrough_plugin.cpp`. The optional CMake switch `TVSTREAMMERSAT5_BUILD_CA_PLUGIN_EXAMPLE=ON` builds that test `.so`. The DVB service relay attaches the backend transport hook after single-service selection and before the local UDP handoff, so an authorised backend can process the local SPTS without introducing a card-sharing service.

`StableUdpOutput.cpp` and the WISI five-second startup reservoir, 20 ms periodic PCR, PCR restamping and 7x188/1316-byte UDP packetisation are unchanged. The native DVB scanner/shared-frontend path remains intact.


### Manual CA binding + DVB EMM intake telemetry (v134)

Release 20 disables cross-card AUTO routing. Encrypted channels must be bound to a specific Phoenix reader because different physical cards can carry different operator/provider entitlements. The per-reader service limit, reader auto-activation and reader auto-reactivation settings remain available. Legacy streams configured with `conditional_access_reader=auto` are rejected with a clear message until a specific reader is selected.

Satellite PSI scanning now reports CA signalling separately: ECM PIDs from PMT CA descriptors and EMM PIDs from CAT CA descriptors. The service PID filter continues to include both sets, so operator EMM packets are received from the satellite together with the selected encrypted service. This is reception/telemetry only: the current native CardManager does not implement the card-system-specific command path that applies EMM entitlements to the smart card and it does not implement ECM/CW descrambling. Therefore `Pervy1` shown on a tile means the stream is assigned to that reader, not that the transport has been decrypted.


### Phoenix card lifecycle + per-card service limits (v133)

Release 19 adds persistent per-reader Conditional Access policy without exposing any network CA or key interface. Each Phoenix reader now has an operator-configurable service limit from 1 to 64 (default 10), automatic initial activation, automatic reactivation after reader/card/USB errors, a configurable retry interval, and a manual **Reactivate** action. Settings are stored in the main `tvstreammersat5-config.json` under `ca_readers` and are matched by stable `/dev/serial/by-id` path and USB serial.

For scrambled DVB services the **Auto** reader mode is now a real CardManager route rather than a UI shortcut to the first detected card. Auto mode selects a usable reader with a free slot and balances by the configured `used/max` ratio. For example, with `Pervy1 max=3` and `Voprosy_otvety max=10`, the first three automatic services can fill `Pervy1` and subsequent services spill to the second reader according to availability/load. Manual per-channel reader binding remains available when operator entitlements differ between cards. Existing streams already saved with an explicit reader stay explicit until changed to **Auto** in the stream editor.

The CardManager lifecycle monitor is deliberately conservative: it never opens or resets a reader that is owned by another process. While OSCam or another process has the device open the reader is reported as `EXTERNAL_OWNER`; once the port becomes free, auto-activation can perform the local ATR/card-presence probe. Successful probe state is `READY`; missing card, permission errors, disconnected USB and failed probes can be retried automatically according to the per-reader policy. This lifecycle/slot manager still does **not** implement a native ECM/CW descrambling backend and does not export key material.

The Add Channel Phoenix panel now shows the activation state, `used/max` slots, maximum channel count, Auto activation, Auto reactivation, retry seconds, and a per-reader Reactivate button. FTA services do not consume a card slot. The DVB/SPTS, packet remap, shared-frontend and five-second WISI reservoir paths are unchanged by this release.

### Dashboard density + outgoing-interface selector recovery (v132)

Release 18 removes the remaining unused vertical space in stream tiles: all tiles remain equal height, but are reduced to 252 px, the information grid is tightened, and the control buttons sit directly under `Bitrate Out`. The `Декодирование` label and decode badge use a smaller font so CA telemetry takes less space without losing the green/red/waiting state.

The outgoing-interface selector is restored for both the regular stream editor and the satellite add-channel dialog. `/api/state` now carries the current network-interface list in addition to `/api/interfaces`, and the browser preserves the last valid interface list across a transient state refresh. This fixes the race where the 2-second `/api/state` poll could overwrite `state.interfaces` after `/api/interfaces` had already loaded, leaving only the automatic output-interface option.

DVB shared-frontend handling, SPTS/PAT/SDT filtering, packet-level DVB remap, Phoenix/CardManager, decode telemetry and StableUdpOutput/WISI five-second reservoir are unchanged.

TVStreammerSAT5 is an IPTV stream router, monitor and transcoder with a built-in web control panel. **Current program version: Release 29 / v143.**


### Retry inactive/error streams cleanly (v129)

Release 15 fixes a runtime-state bug visible as `stream is already active: <id>` after a tile had already fallen to **OFFLINE** because of a GStreamer/DVB error. A terminal ERROR/EOS now marks both `active=false` and `running=false`. When **Старт** is pressed for an inactive stream that still has a stale runtime object, TVStreammerSAT5 performs a complete silent cleanup first (joins the old bus thread, drives the pipeline to NULL, releases the per-service DVB relay and its port, decrements the shared-DVB frontend consumer count, releases any CA slot, then creates a fresh pipeline). Truly active streams are still protected from duplicate starts. This cleanup does not emit a false “stopped manually” Telegram notification.

The shared DVB frontend introduced in v128 is preserved: restarting one failed service does not retune or stop other services using the same adapter/frontend/transponder. The five-second WISI startup reservoir and StableUdpOutput implementation are unchanged.

### Shared DVB frontend fan-out without remux (v128)

Release 14 restores shared DVB frontend operation for channels on the same physical `adapter/frontend` and the same transponder. One `dvbsrc` owns the tuner and forwards the full transport stream to an internal loopback multicast relay. Every channel gets a lightweight internal service relay which keeps only its saved PMT/PCR/video/audio/teletext/subtitle/CA PID set and rewrites PAT/SDT to the selected SID. Unlike the old shared implementation, the service relay does **not** use `tsdemux -> mpegtsmux`, so the v122+ byte-preserving FTA/WISI path remains intact.

Expected log sequence for two channels on adapter 5/frontend 0 is:

```text
Shared DVB frontend started: 5:0 ...
DVB service relay started: stream=... SID=... mode=PID-passthrough-no-remux
Shared DVB frontend reused: 5:0 consumers=2 ...
DVB service relay started: stream=... SID=... mode=PID-passthrough-no-remux
```

If the same frontend is already tuned to a different transponder, startup is rejected with a clear error instead of trying to retune it. Failed GStreamer pipelines are explicitly driven to `GST_STATE_NULL` before unref, preventing the `Trying to dispose element ... but it is in READY` warning and deterministicly releasing the frontend descriptor. The Adapter/Frontend selectors also show current in-process consumers as `SHARED N` with the active frequency/polarization.

The five-second WISI startup reservoir, StableUdpOutput PCR restamping/20 ms periodic PCR, and 7x188=1316 byte output packetization are unchanged.

### DVB adapter selector, true SPTS service list and strict decode telemetry (v127)

Release 13 changes the satellite **Adapter** and **Frontend** fields in **Добавить канал** to drop-down lists populated from `/api/dvb-adapters`. The selected `/dev/dvb/adapterN/frontendN` is stored in the normal `dvb://satellite?...` URI.

The single-program PSI filter now rewrites both **PAT and SDT** for the selected SID. PMT/PCR/PES/video/audio/teletext/subtitle packets remain passthrough, while VLC receives only one advertised service instead of the complete transponder service list.

```text
DVB SPTS PSI filter: SID=<sid> PMT_PID=<pid> PAT=single-program SDT=single-service media=passthrough
```

The tile decode indicator now evaluates only actual outgoing A/V PIDs discovered from PMT (with configured VPID/APID as startup fallback). **ДЕКОД: ОК** additionally requires a valid clear PES start; clear PSI, ECM or teletext packets alone cannot turn the indicator green. Scrambled A/V or clear-looking payload without a valid PES is shown as **ДЕКОД: НЕТ**.

All stream tiles use the same fixed layout height. FTA and non-DVB tiles reserve invisible meter/CA rows so all controls and rows align.

`StableUdpOutput.cpp` and the WISI-specific five-second startup reservoir, PCR restamping, 20 ms periodic PCR generation and 7x188/1316-byte UDP packetisation are unchanged.

### Phoenix card-presence / 6 MHz ATR fix (v126)

v126 fixes the false **«НЕТ КАРТЫ»** state that appeared after OSCam released an active FTDI Phoenix reader. v125 probed ATR only at 9600 baud, which matches the usual 3.57 MHz ISO-7816 startup rate, while the deployed `A104JCGD` and `AD023J2Q` readers are configured at 6.00 MHz. The probe now tries the corresponding initial rate (~16129 baud, Fi=372) using Linux arbitrary-baud `termios2`, with a 9600 fallback. It also honours the hardware carrier-detect line used by the deployment (`detect=cd`): if CD reports an inserted card but ATR decoding still fails, the UI reports **«КАРТА · ATR НЕ ПРОЧИТАН»** instead of falsely claiming that the reader is empty. No application APDU, ECM/CW handling or descrambling logic is added by this probe.

The DVB/SPTS path and `StableUdpOutput` WISI five-second startup reservoir are unchanged from v125/v124.

### Per-tile DVB decode indication (v126)

For DVB channels bound to a Phoenix reader, each stream tile now shows a live **ДЕКОД: ОК / ДЕКОД: НЕТ / ДЕКОД: … / ДЕКОД: OFF** indicator. The state is measured passively from the `transport_scrambling_control` bits of the selected service MPEG-TS immediately before the output branch. `ДЕКОД: ОК` is shown only after enough recent payload packets were observed and none were still marked scrambled; any recently scrambled payload packet changes the tile to `ДЕКОД: НЕТ`. This telemetry does not expose or store ECM/CW/key material and does not alter MPEG-TS packets.


### Internal Multi-Service CardManager (v125)

Release 11 adds a closed in-process Conditional-Access control plane for Phoenix/SmartMouse readers. Each encrypted DVB stream can reserve one service slot on its configured reader; a reader is limited to 10 simultaneous reservations by default. Reader identity is normalised to stable `/dev/serial/by-id/*` paths so `/dev/ttyUSB0` / `/dev/ttyUSB1` renumbering after reboot cannot swap card profiles. The current deployment profiles recognise FTDI serial `A104JCGD` as `Voprosy_otvety` (`CAID 0652`, provider `0406BE`) and `AD023J2Q` as `Pervy1` (`CAID 0652`, provider `0400DC`). Unknown readers remain usable as generic Phoenix entries.

`/api/ca-manager` reports reader hardware state, immutable serial, profile, slot usage and per-service telemetry. Active DVB tiles show the local CA reservation state. The manager contains no Newcamd/CCcam server, no network key API and no key export/logging path. v125 is the multi-service control plane and reservation layer; it does not claim a native card descrambling backend. A reader already opened by another process is reported as an external owner and is never reset by the CardManager inventory path.

The DVB SPTS path from v124 is unchanged. The WISI-specific five-second startup reservoir, 20 ms periodic PCR generation, PCR restamping and 7x188/1316-byte UDP packetisation are unchanged.

### DVB single-program PSI cleanup (v124)

Release 10 introduced PAT single-program filtering after `dvbsrc` service PID selection. It keeps the selected PMT/PCR/video/audio/teletext/subtitle packets byte-for-byte and rewrites PAT PID 0 so it advertises exactly one selected SID and its original PMT PID. Some VLC builds still used the untouched transponder SDT to display unrelated service names; Release 13/v127 completes the cleanup by rewriting SDT too. The five-second WISI startup reservoir, PCR restamping, 20 ms periodic PCR generation and 7x188 (1316-byte) UDP packetisation are unchanged.

Expected log line after the selected PMT has been identified:

```text
DVB SPTS PSI filter: SID=<sid> PMT_PID=<pid> PAT=single-program SDT=single-service media=passthrough
```

The application receives live streams, monitors their state and bitrate, can switch to a backup input, optionally transcodes video/audio with GStreamer, remaps MPEG-TS service metadata where supported, and publishes one or more output formats.

## DVB-S/S2 satellite channel scanner (v116+)

The web interface includes **Add channel / Добавить канал** for satellite tuners exposed by Linux as `/dev/dvb/adapterN/frontendN`. The dialog accepts satellite frequency in **MHz**, symbol rate in kSym/s, polarity, DVB-S/DVB-S2, modulation/FEC, DiSEqC input, LNB LOF values and optional DVB-S2 stream ID.

While the dialog is open, TVStreammerSAT5 shows frontend lock, signal and quality. **Scan channels / Сканировать каналы** tunes the selected transponder, reads PAT/PMT/SDT tables, lists discovered services with SID/provider information, and lets the operator select which services to save. Saving creates one normal stream tile per selected service. UDP output ports are allocated sequentially from the configured first port, and the dialog lets the operator choose the output network interface used for multicast.

Satellite streams are stored as `dvb://satellite?...` inputs. Each scan result now includes the PMT/PCR and every elementary PID of the selected service. Release 7 stores that PID list in the channel URI and configures `dvbsrc` to capture the complete service directly, avoiding the `tsparse program_%u` request-pad path that could forward only PSI/SI on some GStreamer builds. The normal WISI-compatible 5-second UDP reservoir remains unchanged.



### Phoenix reader/card inventory (v123)

Release 9 adds automatic Phoenix/SmartMouse-style USB serial reader discovery without changing the working DVB/FTA or WISI UDP pipeline. The inventory prefers stable `/dev/serial/by-id/*` paths and falls back to `/dev/ttyUSB*` / `/dev/ttyACM*`, then numbers the detected readers as **Phoenix 1**, **Phoenix 2**, and so on. FTDI, PL2303, CP210x, CH341 and common usbserial/smart-card identities are recognised as candidates.

When the **Add channel** window is opened (or **Refresh** is pressed), a free Phoenix candidate is probed for an ISO 7816 ATR without sending application APDUs. If an ATR is received, the UI shows **CARD** and a best-effort card-system/provider label when recognizable from ATR historical data; otherwise it explicitly shows that the provider could not be determined. If no ATR is returned, the reader is shown as **Phoenix N — NO CARD**. Busy readers are not reset/probed and are shown as **BUSY**; permission failures are shown separately.

The selected reader is stored in `conditional_access_reader` for scrambled DVB tiles. FTA tiles do not depend on this field. This release is an inventory/status and reader-assignment feature only: it does not add ECM/CW extraction, sharing, caching, or software descrambling. The existing five-second WISI startup reservoir, PCR restamping/20 ms periodic PCR and 7x188 UDP packetisation remain unchanged.

### Stable UDP DVB passthrough fix (v122)

Release 8 fixes a zero-output condition where the selected DVB service was healthy at the input (for example ~2.1 Mbit/s) but StableUdpOutput received only two 1316-byte chunks and then reported no PCR (`pcr_pid=8191`). The cause was a redundant output `tsparse` configured with timestamp generation and smoothing immediately before the WISI reservoir. StableUdpOutput already owns the output timeline and PCR restamping, so UDP passthrough now feeds the already-normalised MPEG-TS directly into the five-second reservoir. The WISI startup reservoir, 20 ms periodic PCR generation and 7x188 UDP packetisation are unchanged.

Expected v122 log line for a normal UDP passthrough branch:

```text
Stable UDP passthrough: direct MPEG-TS -> WISI reservoir timestamp_tsparse=off smoothing=off packetization=preserve-upstream
```

### DVB service PID capture + FTA/CA scan indication (v121)

Release 7 fixes the case where a locked DVB-S/S2 channel showed only about 50–100 kbit/s input bitrate. That rate is characteristic of PAT/PMT/SDT/NIT and other service information without the video/audio PES packets. The scanner now parses PMT tables, records PCR and all elementary PIDs, detects Conditional Access from SDT/PMT CA signalling, and marks each scan result as **FTA** or **КОД.**. New channels carry their exact `dvbsrc` PID filter; older v120 channels automatically resolve the PID list at startup and fall back to the full transponder only if PMT discovery fails.

### FTA DVB SPTS + tile signal meters (v120)

Release 6 removed the DVB `tsdemux -> elementary parsers -> mpegtsmux` stage from normal satellite passthrough. Release 7 keeps that principle but replaces `tsparse program_%u` service filtering with deterministic `dvbsrc` PID capture. UDP without explicit PID/SID remapping continues into the WISI-compatible StableUdpOutput sender.

DVB tiles now show two compact live bars at the top: **S** (frontend signal strength) and **Q** (frontend quality/CNR-derived percentage). The dashboard reads frontend statistics with Linux DVB ioctls only; it does not instantiate a second `dvbsrc` or retune the live frontend. VBR/CBR is displayed at the top-right next to the delete button.

If the five-second input watchdog expires while the frontend still reports LOCK, the status is now `DVB LOCK - no service data (SID ...)` instead of the misleading `no input signal`.

### Test-pattern SID / bitrate fix (v119)

Release 5 fixes the satellite-channel replacement/test path. When a DVB channel has a configured input SID (for example 230), the internal `test://bars` transport is now demuxed in AUTO mode instead of trying to find that live DVB SID inside the synthetic TS. The same AUTO rule is applied to DVB output remuxing because the DVB source chain has already selected the requested SID before producing a clean single-program TS. The synthetic test source queue is also exposed to the normal input bitrate probe, so both input and output bitrate become visible while the test pattern is running. The 5-second WISI startup reservoir and periodic-PCR shaper are unchanged.

### WISI-compatible UDP reservoir (v118)

UDP VBR/CBR uses the stable application-level MPEG-TS sender with **7 x 188 = 1316 byte datagrams** and an intentional **5 second startup reservoir**. The reservoir is retained for WISI equipment compatibility. The sender then maintains the adaptive reservoir and a periodic 20 ms PCR cadence.

Release 4 removes the previous startup deadlock condition: after the full 5 second reservoir is accumulated the sender needs only one PCR sample to lock the periodic PCR generator, instead of five. If no PCR is seen at all, a bounded 2 second PCR grace period expires and UDP starts with a warning rather than waiting forever. This fallback does not shorten or bypass the 5 second WISI reservoir.

Runtime requirements:

```bash
ls -l /dev/dvb/adapter*/frontend*
gst-inspect-1.0 dvbsrc
```

The service user must have read/write access to the DVB devices. If TVStreammerSAT5 runs inside a container, the `/dev/dvb` devices must be passed through to that container. Multiple selected services from one transponder share the same physical tuner frequency; retuning that frontend to another transponder affects streams using the same frontend.


## Main features

- Web control panel with English/Russian interface.
- Start, stop, edit and delete streams without editing JSON manually.
- Primary/backup input switching and automatic return to the primary source.
- Input/output bitrate graphs, CC-error monitoring and interface load monitoring.
- Multiple outputs per stream.
- MPEG-TS PID/service remap for compatible TS paths.
- Optional H.264/AAC or H.264/MP3 transcoding through a clean external `gst-launch-1.0` pipeline.
- YADIF deinterlacing, scaling and fixed 25 fps progressive output for the transcoder.
- HTTP TS and HLS delivery from the built-in HTTP server.
- SRT listener/caller support.
- UDP unicast/multicast VBR and CBR output.
- RTMP/YouTube push and RTSP push.
- Subscriber/IP filtering for HTTP TS, HLS and SRT listener sessions.
- Telegram notifications.
- VLC playlist generation.
- Docker build/run scripts for host-network deployments.

## Release 4 architecture

Normal streams and transcoded streams use separate protocol modules. The normal pipeline remains in-process; transcoding is performed by an external GStreamer process.

```text
Input protocol
   |
   +-- passthrough/remap path --------------------> selected output protocol
   |
   +-- optional GStreamer transcoder
          |
          +-- decode
          +-- YADIF deinterlace
          +-- scale / 25 fps progressive
          +-- x264 H.264
          +-- AAC or MP3 audio
          +-- protocol-specific output module
```

Protocol code is split under `src/protocols/`:

```text
src/protocols/inputs/          transcoder input URI modules
src/protocols/outputs/         transcoder output modules
src/protocols/stream/inputs/   normal stream input modules
src/protocols/stream/outputs/  normal stream output modules
```

The legacy in-process transcoder implementation remains in the source tree for compatibility/fallback work, but the active clean transcoder path is `GstTranscoderProcess` using `gst-launch-1.0`.

## Supported inputs

Typical input URLs:

```text
http://server/live.ts
https://server/live.ts
http://server/live/playlist.m3u8
hls://server/live/playlist.m3u8
udp://@:1234
udp://239.1.1.1:1234
rtp://239.1.1.1:5004
srt://192.168.1.10:9000
rtsp://user:password@192.168.1.10:554/stream1
rtsps://server/stream
rtmp://server/live/camera1
test://bars
```

A local backup file is also supported by the normal stream path. The input interface can be selected separately from the output interface for protocols where GStreamer/Linux provides an explicit bind/interface option.

## Output formats

The web UI currently exposes:

```text
udp-vbr   MPEG-TS over WISI-compatible StableUdpOutput, bitrate follows the selected service
udp-cbr   MPEG-TS over WISI-compatible StableUdpOutput with NULL padding to Target bitrate
rtp       MPEG-TS over RTP/UDP
srt       MPEG-TS over SRT listener or caller
http      MPEG-TS over HTTP
hls       HLS playlist + MPEG-TS segments
rtsp      RTSP push to an external RTSP server
rtmp      RTMP push
youtube   RTMP push to YouTube Live
```

UDP VBR and UDP CBR use the WISI-compatible StableUdpOutput path (`appsink` + application-level UDP sender). MPEG-TS is packetized as **7 × 188 = 1316 byte** UDP datagrams and intentionally accumulates a **5 second startup reservoir** before transmission. PCR is restamped and periodic PCR cadence is maintained by the sender. UDP VBR follows the selected service rate; UDP CBR pads transport slots to the configured target bitrate. The configured output interface/address is used by the sender for multicast routing/binding.

RTP MPEG-TS output is selectable in the web UI. It uses `rtpmp2tpay` over UDP; with MTU 1400 the payload fits 7 MPEG-TS packets (7 × 188 = 1316 bytes), which is suitable for IPTV headends.

HTTP and HLS player URLs use the application HTTP server:

```text
http://SERVER:9000/stream/STREAM_ID.ts
http://SERVER:9000/hls/STREAM_ID/playlist.m3u8
```

RTSP output is **push**, not an embedded RTSP server. `output_host` must point to a server that accepts RTSP publishing.

## Multiple outputs

A stream can have one primary output plus `additional_outputs`. Example:

```json
{
  "output_type": "udp-cbr",
  "output_host": "239.1.1.1",
  "output_port": 1234,
  "additional_outputs": [
    {
      "output_type": "srt",
      "output_mode": "listener",
      "output_host": "0.0.0.0",
      "output_port": 9001
    },
    {
      "output_type": "hls",
      "output_mode": "listener",
      "output_host": "192.168.1.20",
      "output_port": 9000
    }
  ]
}
```

## MPEG-TS remap

Enable `remap_enabled` and set the required values:

```json
{
  "remap_enabled": true,
  "video_pid": 258,
  "audio_pid": 257,
  "service_id": 2,
  "service_name": "Channel",
  "service_provider": "Provider"
}
```

For UDP/SRT/HTTP MPEG-TS paths the mux/remap code uses MPEG-TS request pads and service mapping where the selected pipeline supports it.

**Known Release 4 limitation:** exact configured elementary PIDs are not currently guaranteed after **transcoded HLS** segmentation. Verify the generated `.ts` segments with `ffprobe -show_programs -show_streams` when exact HLS PID values are required. Do not rely only on the `.m3u8` playlist to verify remap.

## HLS behavior

Transcoded HLS is generated under:

```text
/tmp/tvstreammersat5-hls/<stream-id>/
```

Release 4 uses a three-segment live playlist:

```text
playlist-length = 3
target-duration = 5 seconds
max-files = 5
```

The HLS directory for a stream is cleared when a new transcoded HLS pipeline starts so stale segments from a previous run are not mixed into a new playlist.

## Transcoding

Transcoding is optional. When disabled, TVStreammerSAT5 uses the normal passthrough/remap pipeline.

The current transcoder video path is approximately:

```text
uridecodebin
 -> raw video
 -> videoconvert
 -> deinterlace method=yadif
 -> videoscale method=lanczos (selected output resolution, preserve display aspect with borders when required)
 -> videorate
 -> 25 fps progressive I420, SAR/PAR 1:1 (square pixels)
 -> x264enc superfast / zerolatency with VUI enabled
 -> h264parse
```


The transcoder always normalizes the scaled video to **SAR/PAR 1:1** before H.264 encoding. The selected output width/height therefore use square pixels, and `x264enc` is instructed to emit VUI information. `videoscale add-borders=true` preserves the source display aspect ratio when the selected resolution has a different shape, adding borders instead of stretching the picture.

Audio is normalized to 48 kHz stereo before encoding. AAC is the stable/default transcoder path; MP3 is used when explicitly configured and an MP3 encoder is available. In the clean transcoder path, a UI/config value intended as audio `copy` is currently handled as AAC re-encode rather than bit-exact passthrough.

The external transcoder owns its own input socket. Therefore the transcoder input bitrate graph is an application-side estimate while this architecture is used; it is not a direct GStreamer pad-probe measurement of the external process input.

Check installed transcoder/protocol elements with:

```bash
./scripts/check_transcoder_plugins.sh
```

## Build on Ubuntu/Debian

Install only the current build/runtime dependencies:

```bash
./install_deps.sh
```

Build:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

Run from the directory that contains `tvstreammersat5-config.json`:

```bash
./build/TVStreammerSAT5
```

Default web UI:

```text
http://localhost:9000
```

### Packages installed by `install_deps.sh`

Build libraries:

```text
build-essential
cmake
pkg-config
libgstreamer1.0-dev
libgstreamer-plugins-base1.0-dev
libgstreamer-plugins-bad1.0-dev
libcurl4-openssl-dev
libjsoncpp-dev
libboost-system-dev
libboost-thread-dev
```

Runtime GStreamer packages:

```text
gstreamer1.0-tools
gstreamer1.0-plugins-base
gstreamer1.0-plugins-good
gstreamer1.0-plugins-bad
gstreamer1.0-plugins-ugly
gstreamer1.0-libav
gstreamer1.0-rtsp
ca-certificates
```

`gstreamer1.0-rtsp` is required for `rtspclientsink` used by the RTSP push output module. Development packages for Boost filesystem/program-options, OpenSSL, gst-rtsp-server, Git and wget are not required by the current CMake target and are no longer installed by the dependency script.

## Install as a systemd service

```bash
sudo mkdir -p /opt/tvstreammersat5
sudo install -m 755 build/TVStreammerSAT5 /opt/tvstreammersat5/TVStreammerSAT5
sudo cp tvstreammersat5-config.json /opt/tvstreammersat5/
```

Example `/etc/systemd/system/tvstreammersat5.service`:

```ini
[Unit]
Description=TVStreammerSAT5 Release 29
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
WorkingDirectory=/opt/tvstreammersat5
ExecStart=/opt/tvstreammersat5/TVStreammerSAT5
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
```

Enable it:

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now tvstreammersat5
sudo systemctl status tvstreammersat5 --no-pager --full
```

## Docker

TVStreammerSAT5 can be built and run entirely in Docker. This keeps compiler and
GStreamer development packages out of the host operating system. The runtime
container still uses the host network because IPTV multicast, RTP, SRT listener
mode and interface-specific bindings work most reliably with `--network host`.

### Install Docker Engine on Ubuntu

The recommended production installation uses Docker's official `apt`
repository. The commands below are suitable for supported Ubuntu releases such
as 22.04 LTS and 24.04 LTS.

Remove packages that can conflict with the official Docker Engine packages:

```bash
sudo apt remove -y docker.io docker-compose docker-compose-v2 docker-doc \
  docker-buildx podman-docker containerd runc || true
```

Add Docker's signing key and repository:

```bash
sudo apt update
sudo apt install -y ca-certificates curl

sudo install -m 0755 -d /etc/apt/keyrings
sudo curl -fsSL https://download.docker.com/linux/ubuntu/gpg \
  -o /etc/apt/keyrings/docker.asc
sudo chmod a+r /etc/apt/keyrings/docker.asc

sudo tee /etc/apt/sources.list.d/docker.sources >/dev/null <<EOF
Types: deb
URIs: https://download.docker.com/linux/ubuntu
Suites: $(. /etc/os-release && echo "${UBUNTU_CODENAME:-$VERSION_CODENAME}")
Components: stable
Architectures: $(dpkg --print-architecture)
Signed-By: /etc/apt/keyrings/docker.asc
EOF

sudo apt update
```

Install Docker Engine, Buildx and the Compose plugin:

```bash
sudo apt install -y \
  docker-ce \
  docker-ce-cli \
  containerd.io \
  docker-buildx-plugin \
  docker-compose-plugin
```

Enable Docker at boot and verify the daemon:

```bash
sudo systemctl enable --now docker
sudo systemctl status docker --no-pager --full
sudo docker run --rm hello-world
```

Docker commands require `sudo` by default. To allow the current user to run
Docker commands without `sudo`:

```bash
sudo usermod -aG docker "$USER"
newgrp docker
docker version
```

Membership in the `docker` group effectively grants root-level access to the
host. Keep using `sudo docker ...` instead if that is preferable for the server.

### Build the TVStreammerSAT5 Docker image

From the repository directory:

```bash
cd /home/monk/TVStreammerSAT5
chmod +x scripts/build_container.sh scripts/run_container.sh
./scripts/build_container.sh
```

The default image name is:

```text
tvstreammersat5:release2
```

Verify that the image exists:

```bash
docker image ls tvstreammersat5:release2
docker image inspect tvstreammersat5:release2 >/dev/null && echo "TVStreammerSAT5 image OK"
```

The build script can be launched from any working directory because it resolves
the repository path from the script location itself. To use another image name:

```bash
IMAGE_NAME=my-tvstreammersat5:release2 ./scripts/build_container.sh
```

A direct equivalent build command is:

```bash
docker build --pull -t tvstreammersat5:release2 .
```

### Prepare persistent TVStreammerSAT5 data

For a production container, keep configuration and application data outside the
container. Example:

```bash
sudo mkdir -p /srv/tvstreammersat5
sudo cp tvstreammersat5-config.json /srv/tvstreammersat5/tvstreammersat5-config.json
sudo chown -R "$USER":"$USER" /srv/tvstreammersat5
```

The `/srv/tvstreammersat5` directory can then persist:

```text
tvstreammersat5-config.json
tvstreammersat5-subscribers.json
backup-files/
```

### Test the container interactively

The supplied run script starts an interactive temporary container:

```bash
CONFIG_FILE=/srv/tvstreammersat5/tvstreammersat5-config.json \
  ./scripts/run_container.sh
```

Press `Ctrl+C` to stop it. Because this helper uses `--rm`, the temporary
container is automatically removed after exit; configuration and other data
remain on the host.

To increase GStreamer logging temporarily:

```bash
GST_DEBUG=2 \
CONFIG_FILE=/srv/tvstreammersat5/tvstreammersat5-config.json \
  ./scripts/run_container.sh
```

### Run TVStreammerSAT5 as a background Docker service

Create the persistent production container:

```bash
docker run -d \
  --name tvstreammersat5 \
  --restart unless-stopped \
  --init \
  --network host \
  -v /srv/tvstreammersat5:/data \
  -w /data \
  -e GST_DEBUG=1 \
  tvstreammersat5:release2
```

The web interface then uses the HTTP port configured by TVStreammerSAT5, normally:

```text
http://SERVER_IP:9000
```

`--network host` is intentional. With host networking, Docker does not need
`-p 9000:9000`, `-p` mappings for UDP multicast, or separate SRT port
forwarding. TVStreammerSAT5 binds directly to the host's interfaces and ports.

### Docker management from the console

The following commands manage the `tvstreammersat5` container directly from a Linux
console. If the current user is not in the `docker` group, prefix each
`docker ...` command with `sudo`. Commands that stop or restart the Docker daemon
affect **all** containers on the host, not only TVStreammerSAT5.

```bash
# Show Docker Engine status
systemctl status docker --no-pager --full

# Start / stop / restart the Docker daemon
sudo systemctl start docker
sudo systemctl stop docker
sudo systemctl restart docker

# Enable / disable Docker daemon autostart
sudo systemctl enable docker
sudo systemctl disable docker

# Show all running containers
docker ps

# Show running and stopped containers
docker ps -a

# Show only TVStreammerSAT5
docker ps -a --filter name=tvstreammersat5

# Start TVStreammerSAT5
docker start tvstreammersat5

# Stop TVStreammerSAT5 gracefully
docker stop -t 15 tvstreammersat5

# Restart TVStreammerSAT5
docker restart -t 15 tvstreammersat5

# Show container state, exit code and restart count
docker inspect tvstreammersat5 --format \
  'status={{.State.Status}} running={{.State.Running}} exit={{.State.ExitCode}} restart={{.RestartCount}}'

# Follow logs in real time
docker logs -f tvstreammersat5

# Show the last 200 log lines
docker logs --tail 200 tvstreammersat5

# Show logs from the last 10 minutes
docker logs --since 10m tvstreammersat5

# Show CPU, RAM and network usage
docker stats tvstreammersat5

# Open a shell inside the running container
docker exec -it tvstreammersat5 bash

# Show processes running in the TVStreammerSAT5 container
docker top tvstreammersat5

# Check GStreamer plugins inside the container
docker exec tvstreammersat5 gst-inspect-1.0 x264enc
docker exec tvstreammersat5 gst-inspect-1.0 srtsink
docker exec tvstreammersat5 gst-inspect-1.0 rtspclientsink

# Check the config visible inside the container
docker exec tvstreammersat5 ls -lah /data
docker exec tvstreammersat5 test -f /data/tvstreammersat5-config.json && echo "config OK"

# Inspect mounted host directories
docker inspect tvstreammersat5 --format '{{json .Mounts}}'

# Inspect container network mode
docker inspect tvstreammersat5 --format '{{.HostConfig.NetworkMode}}'

# Show the image used by the container
docker inspect tvstreammersat5 --format '{{.Config.Image}}'

# Show installed TVStreammerSAT5 images
docker image ls 'tvstreammersat5*'

# Show Docker disk usage
docker system df
```

### Update TVStreammerSAT5 in Docker

After pulling new source code, rebuild the image and recreate the container. A
running container does not automatically switch to a newly built image.

```bash
cd /home/monk/TVStreammerSAT5
git pull origin main

./scripts/build_container.sh

docker stop -t 15 tvstreammersat5
docker rm tvstreammersat5

docker run -d \
  --name tvstreammersat5 \
  --restart unless-stopped \
  --init \
  --network host \
  -v /srv/tvstreammersat5:/data \
  -w /data \
  -e GST_DEBUG=1 \
  tvstreammersat5:release2

# Verify the new container
docker ps --filter name=tvstreammersat5
docker logs --tail 100 tvstreammersat5
```

The bind-mounted `/srv/tvstreammersat5` directory is not removed when the container
is recreated, so the configuration, subscriber database and backup files remain
persistent.

### Change the configuration and restart

Edit the host copy of the configuration:

```bash
sudoedit /srv/tvstreammersat5/tvstreammersat5-config.json
```

Then restart the container:

```bash
docker restart -t 15 tvstreammersat5
docker logs --tail 100 tvstreammersat5
```

### Remove or recreate the container

Removing the container does not remove `/srv/tvstreammersat5` because that directory
is a host bind mount.

```bash
docker stop -t 15 tvstreammersat5
docker rm tvstreammersat5
```

Force-remove a stuck container only when a normal stop does not work:

```bash
docker rm -f tvstreammersat5
```

Remove an old TVStreammerSAT5 image after the container using it has been removed:

```bash
docker image rm tvstreammersat5:release2
```

Remove only unused Docker objects:

```bash
docker container prune
docker image prune
docker builder prune
```

Do not use `docker system prune -a --volumes` on a production server unless you
explicitly intend to delete all unused images, networks, build cache and unused
Docker volumes.

### Docker troubleshooting

If the container immediately exits:

```bash
docker ps -a --filter name=tvstreammersat5
docker inspect tvstreammersat5 --format 'exit={{.State.ExitCode}} error={{.State.Error}}'
docker logs --tail 300 tvstreammersat5
```

If port 9000 is already occupied:

```bash
sudo ss -lntp | grep ':9000'
```

If UDP multicast or SRT traffic is missing, first verify that host networking is
actually enabled and inspect the host interface directly:

```bash
docker inspect tvstreammersat5 --format '{{.HostConfig.NetworkMode}}'
ip -br addr
ip route
sudo tcpdump -ni eth0 udp
```

Replace `eth0` with the real IPTV interface. Since TVStreammerSAT5 uses host
networking, protocol diagnostics are performed on the host network namespace.

If the image needs to be rebuilt without Docker layer cache:

```bash
cd /home/monk/TVStreammerSAT5
docker build --pull --no-cache -t tvstreammersat5:release2 .
```

If Docker itself is unhealthy:

```bash
sudo systemctl status docker --no-pager --full
sudo journalctl -u docker -n 200 --no-pager
docker info
```

Docker's official Ubuntu installation documentation should be checked when
upgrading the host to a new Ubuntu release because supported distributions and
package names can change.

## Network tuning for high-bitrate UDP

Example Linux tuning:

```bash
sudo sysctl -w net.core.rmem_max=67108864
sudo sysctl -w net.core.wmem_max=134217728
sudo sysctl -w net.core.rmem_default=8388608
sudo sysctl -w net.core.wmem_default=8388608
sudo sysctl -w net.ipv4.udp_rmem_min=131072
sudo sysctl -w net.ipv4.udp_wmem_min=131072
sudo sysctl -w net.core.netdev_max_backlog=50000
```

For multicast, verify routes and the selected interface instead of assuming the default route is correct:

```bash
ip -br addr
ip route get 239.1.1.1
sudo tcpdump -ni eth0 udp port 1234
```

`bad udp cksum` shown by `tcpdump` on the transmitting host can be a TX checksum-offload artifact; receiver-side capture is the better validation point.

## Backup failover

Set `backup_input_uri` to enable source failover. When the current input stops producing data, TVStreammerSAT5 can switch to the backup source and later return to the primary source when it recovers.

Useful fields:

```text
backup_input_uri
backup_input_type
backup_file_loop
auto_start
```

The stream tile displays whether the primary or backup source is active.

## Subscriber access control

Subscriber settings are stored in:

```text
tvstreammersat5-subscribers.json
```

When filtering is enabled, HTTP TS, HLS and SRT listener access can be restricted by subscriber IP and stream ID. The web UI can add/remove subscribers, assign streams and reset active sessions.

## Telegram notifications

Configure:

```text
telegram_token
telegram_chat_id
```

Notifications report stream start/stop, source failure, backup switching, recovery, pipeline errors and EOS events.

## Main configuration fields

Example:

```json
{
  "id": "channel-1",
  "name": "Channel 1",
  "input_uri": "udp://239.1.1.1:1234",
  "backup_input_uri": "",
  "input_mode": "auto",
  "input_interface_address": "",
  "output_type": "udp-cbr",
  "output_mode": "listener",
  "output_host": "239.2.2.2",
  "output_port": 1234,
  "interface_address": "",
  "target_bitrate": 7000000,
  "remap_enabled": true,
  "video_pid": 258,
  "audio_pid": 257,
  "service_id": 2,
  "service_name": "Channel 1",
  "service_provider": "TVStreammerSAT5",
  "transcode_enabled": false,
  "transcode_resolution": "1280x720",
  "transcode_video_bitrate": 3500000,
  "transcode_audio_codec": "aac",
  "transcode_audio_bitrate": 192000,
  "additional_outputs": []
}
```

Global settings include `http_port`, `login`, `password`, Telegram settings and stream arrays. The web UI is the recommended configuration editor.

## Diagnostics

Service logs:

```bash
sudo journalctl -u tvstreammersat5 -n 200 --no-pager
```

Processes:

```bash
ps aux | grep -Ei 'TVStreammerSAT5|gst-launch' | grep -v grep
```

GStreamer capability check:

```bash
./scripts/check_transcoder_plugins.sh
```

Inspect MPEG-TS:

```bash
ffprobe -hide_banner -show_programs -show_streams INPUT
```

Inspect current HLS segment:

```bash
SEG=$(ls -1t /tmp/tvstreammersat5-hls/STREAM_ID/segment*.ts | head -1)
ffprobe -hide_banner -show_programs -show_streams "$SEG"
```

## Screenshots

Dashboard:

![TVStreammerSAT5 dashboard](docs/screenshots/dashboard.png)

Stream settings:

![TVStreammerSAT5 stream settings](docs/screenshots/stream-settings.png)

Network monitoring:

![TVStreammerSAT5 network interface load](docs/screenshots/network.png)

## Notes

- Release 4 no longer uses FFmpeg as the active transcoder engine.
- Non-transcoded behavior should be treated as the stable baseline when diagnosing protocol-specific transcoder issues.
- Exact HLS PID preservation after transcoding remains a known limitation and must be verified on generated segments.
- For live IPTV, transport stability depends on source quality, kernel socket buffers, routing, multicast interface selection and available CPU for x264 encoding.

### CBR pacing for all transcoded outputs

For live IPTV inputs the external GStreamer transcoder disables `uridecodebin use-buffering` so a live UDP/SRT source is not repeatedly paused by generic URI buffering.

All **MPEG-TS based outputs after transcoding** now use one CBR transport policy: `mpegtsmux bitrate=<effective_mux_bitrate>` inserts NULL packets to maintain the selected multiplex bitrate, then `tsparse` normalizes TS timing and an `identity datarate=<bytes/s> sync=true` pacer releases the transport stream against the pipeline clock. This applies to UDP (including the old `udp-vbr` selector while transcoding), SRT, HTTP TS, HLS, RTP and FIFO relay outputs.

The effective TS CBR bitrate is `max(target_bitrate, video_bitrate + audio_bitrate + 1.2 Mbit/s)`. The extra headroom is reserved for MPEG-TS/SI overhead and prevents the mux target from being lower than the encoded elementary streams. The effective value is printed in the journal as `ts-cbr-bitrate=<bits_per_second>`.

RTMP/YouTube and RTSP do not carry MPEG-TS, so TS NULL-packet stuffing is not applicable to those protocols. Their transcoded H.264 branch still uses x264 CBR/HRD (`nal-hrd=cbr`, fixed encoder bitrate, VBV and CFR), and the audio encoder uses the configured fixed bitrate.

Current TS output pacing profile:

```text
UDP/RTP: mpegtsmux CBR -> tsparse -> identity CBR pacer -> network sink
HTTP:    mpegtsmux CBR -> tsparse -> identity CBR pacer -> tcpserversink
HLS:     mpegtsmux CBR -> tsparse -> identity CBR pacer -> hlssink
SRT:     mpegtsmux CBR -> tsparse -> identity CBR pacer -> loopback relay/public SRT
FIFO:    mpegtsmux CBR -> tsparse -> identity CBR pacer -> filesink/FIFO
```

For an SRT Listener with transcoding, the public `srtsink` is still owned by TVStreammerSAT5 so subscriber monitoring continues to use the normal `caller-added`/`caller-removed` callbacks. The external transcoder only sends the already paced CBR MPEG-TS over `127.0.0.1:<relay>`. The in-process relay no longer rebuilds PCR timestamps a second time. Transcoded SRT also uses a 2500 ms SRT latency and disables `GstBaseSink max-bitrate`; the latter is important because a stale/default `target_bitrate` can otherwise throttle a 6+ Mbit/s transcoded stream to only about 2 Mbit/s and create periodic A/V stalls.

Recommended first receiver test for transcoded SRT is 2500-3000 ms latency.

### Проверка GStreamer внутри Docker

Release 4 инициализирует GStreamer до запуска HTTP-интерфейса. Это важно: страница настройки потоков проверяет наличие элементов транскодера сразу после запуска приложения. Если registry GStreamer еще не инициализирован, интерфейс ошибочно может показать все элементы как отсутствующие, даже если пакеты установлены в контейнере.

После пересборки контейнера можно проверить runtime напрямую:

```bash
docker exec tvstreammersat5 gst-inspect-1.0 uridecodebin
docker exec tvstreammersat5 gst-inspect-1.0 decodebin
docker exec tvstreammersat5 gst-inspect-1.0 x264enc
docker exec tvstreammersat5 gst-inspect-1.0 h264parse
docker exec tvstreammersat5 gst-inspect-1.0 aacparse
docker exec tvstreammersat5 gst-inspect-1.0 mpegtsmux
docker exec tvstreammersat5 gst-inspect-1.0 identity
docker exec tvstreammersat5 gst-inspect-1.0 udpsink
```

Для краткой проверки всех обязательных элементов из исходного дерева:

```bash
./scripts/check_transcoder_plugins.sh
```

Dockerfile также выполняет обязательную проверку этих элементов во время `docker build`. Если один из критических GStreamer-плагинов отсутствует, новый образ теперь не будет собран успешно.

### Web UI polling and external transcoder sockets

Release 4 updates live dashboard values without rebuilding every stream card on every poll. The `/api/state` and `/api/system-metrics` requests run as independent sequential polling loops: the next request is scheduled only after the previous request finishes. This avoids overlapping HTTP requests behind reverse proxies such as Nginx/HestiaCP and prevents visible dashboard flicker on busy servers.

A full stream-card render is now performed only when the stream configuration, stream order or UI language changes. Runtime values such as Online/Offline, backup state, active input, input/output bitrate and status are updated in place.

External `gst-launch-1.0` transcoders must not inherit TVStreammerSAT5 HTTP, metrics or listener sockets. TVStreammerSAT5 marks non-standard file descriptors `FD_CLOEXEC` before starting an external GStreamer process. Verify after starting a transcoded stream:

```bash
ss -lntp | grep -E ':9000|:9100'
```

The listening ports should belong to `TVStreammerSAT5` only; `gst-launch-1.0` should not appear as an owner of those sockets.

### SRT после транскодирования

В Release 4 внешний GStreamer-транскодер кодирует видео/звук и отдаёт готовый MPEG-TS во внутренний loopback UDP relay. Сам SRT Listener теперь снова принадлежит процессу `TVStreammerSAT5`, как и SRT без транскодинга. Поэтому события `caller-added` и `caller-removed` приходят напрямую из `srtsink`, а окно абонентов видит подключение сразу, без опроса `ss`.

Смысл полей SRT такой же, как у обычного SRT-потока:

- `Listener`: TVStreammerSAT5 слушает `srt://:<port>` на `0.0.0.0` либо на адресе выбранного выходного интерфейса. Поле `Адрес выхода` используется только как адрес, показываемый в ссылке клиенту, и не используется как bind-адрес.
- `Caller`: `Адрес выхода` является адресом удалённого SRT-сервера, а выбранный выходной интерфейс применяется как локальный bind-адрес.
- Внешний `gst-launch-1.0` для SRT Listener больше не содержит `srtsink`; он содержит `udpsink host=127.0.0.1 port=<relay>`. SRT-сеть, фильтрация абонентов и активные сессии обслуживаются внутри TVStreammerSAT5.
- Для совместимости с GStreamer 1.20.x на Ubuntu 22.04 внешний `gst-launch-1.0` не использует свойство `auto-reconnect` у `srtsink`, потому что в этой версии элемента такого свойства нет.
- Если `gst-launch-1.0` завершается сразу из-за ошибки bind/URI/плагина, запуск потока завершается ошибкой вместо ложного статуса `running`.

Проверка процесса после запуска SRT Listener с транскодингом:

```bash
ps -eo pid,ppid,args | grep gst-launch | grep -v grep
```

В команде внешнего транскодера должен быть виден внутренний relay:

```text
uridecodebin ... use-buffering=false
mpegtsmux ... bitrate=<effective_cbr_bitrate>
tsparse ... smoothing-latency=500000
identity ... datarate=<effective_cbr_bytes_per_second> sync=true
udpsink host=127.0.0.1 port=<relay>
```

А SRT-порт должен принадлежать процессу TVStreammerSAT5:

```bash
ss -lunp | grep '<SRT_PORT>'
journalctl -u tvstreammersat5 -n 100 --no-pager | grep -Ei 'srt|caller|relay|GStreamer transcoder|error|failed|exit'
```

Проверка приёмником GStreamer:

```bash
gst-launch-1.0 -v srtsrc uri="srt://SERVER_IP:SRT_PORT?mode=caller" ! tsparse ! fakesink sync=false
```

### Active sessions for transcoded SRT

SRT без транскодинга и SRT Listener после транскодинга используют одинаковый механизм активных абонентских сессий: TVStreammerSAT5 владеет `srtsink` и получает `caller-added`/`caller-removed` callbacks напрямую.

Когда приёмник подключён, его удалённый IP должен совпадать с основным или резервным IP абонента, а сам абонент должен быть привязан к этому потоку. После этого окно «Абоненты» показывает абонента как Online. Если приёмник подключается через NAT, указывайте тот IP, который видит сервер TVStreammerSAT5.


### Единый мониторинг SRT-подключений

SRT Listener без транскодинга и SRT Listener после транскодинга теперь создают `srtsink` через один и тот же код `StreamManager::createOutputSink()`. Поэтому для обоих режимов используются одинаковые GStreamer-события подключения:

- `caller-connecting` — проверка доступа абонента;
- `caller-added` — добавление активной SRT-сессии;
- `caller-removed` — удаление активной SRT-сессии;
- `caller-rejected` — журналирование отказа.

Отдельный опрос `ss` для транскодированного SRT удалён. Он больше не нужен и мог давать двойной или запаздывающий подсчёт сессий. Внешний `gst-launch-1.0` только передаёт MPEG-TS на `127.0.0.1:<relay>`, а сетевой SRT Listener и мониторинг клиентов находятся внутри TVStreammerSAT5.

Для проверки подключите SRT caller и смотрите журнал:

```bash
journalctl -u tvstreammersat5 -f | grep -Ei 'SRT connection monitoring|SRT caller|Transcoded SRT output relay'
```

При подключении ожидается `SRT caller added ... from <CLIENT_IP>`, а при отключении — `SRT caller removed ...`. Тот же IP используется для статуса Online в таблице абонентов.


### SRT transcoding session path verification

For an SRT Listener with transcoding enabled, the external `gst-launch-1.0` process must terminate at an internal loopback `udpsink`; the public `srtsink` is owned by TVStreammerSAT5 so that the normal `caller-added`/`caller-removed` subscriber monitoring callbacks are used. The startup description should therefore contain `srt-listener-relay@127.0.0.1:`. If the UI still shows `srt-listener@srt://...`, an older/partial SRT output module is deployed.

The external transcoder command is now logged before process startup, and early `gst-launch-1.0` stderr is appended to the persistent web startup error so an exit code such as 255 is accompanied by the actual GStreamer error text.

- Transcoded UDP is routed through the default StableUdpOutput stage via an internal FIFO.


## Release 17 / v131 — dashboard tile render hotfix

- Fixed a v130 browser-side regression where stream tiles disappeared from the dashboard because `render()` called an undefined `escapeHtmlValue()` helper while building the new compact status line.
- Added the missing HTML escaping helper and use it for the runtime status and stream title so text cannot break tile markup.
- This release changes only dashboard rendering/version metadata. DVB shared-frontends, SPTS filtering, packet-level DVB remap and the five-second WISI UDP reservoir/shaper are unchanged from v130.
