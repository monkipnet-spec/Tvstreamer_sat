# TVStreammerSAT5 CaBackend plugin API (ABI v1)

Release v135 adds an in-process Conditional-Access backend boundary without a
network card-sharing server.  The host application never defines a raw
control-word API.  A backend is expected to encapsulate an authorised
operator/manufacturer smart-card SDK and receives local reader/service metadata
plus MPEG-TS packet groups.

## Files

- `src/ca/CaBackendPluginApi.h` — stable C ABI used by `.so` plugins.
- `src/CaBackend.h/.cpp` — plugin discovery, lifecycle and stream-session manager.
- `examples/ca_backend_passthrough_plugin.cpp` — deliberately non-decoding test plugin.

## Discovery

Plugins are loaded from:

`/opt/tvstreammersat5/ca-plugins`

The directory may be overridden for development with:

`TVSTREAMMERSAT5_CA_PLUGIN_DIR=/path/to/plugins`

Every `.so` must export:

`tvstreammersat5_ca_backend_get_api_v1`

and return `tvs_ca_backend_api_v1` with ABI version
`TVS_CA_BACKEND_ABI_V1`.

## Lifecycle

For a stream explicitly bound to a Phoenix reader, TVStreammerSAT5 performs:

1. `open_reader()` once per backend + physical reader.
2. `start_service()` once per channel/SID.
3. `process_ts()` for the already selected DVB SPTS packet groups.
4. `stop_service()` when the stream stops.
5. `close_reader()` after the last service using that reader stops.

Several channels may share one reader when both the configured card limit and
the backend support it.

## Transport contract

`process_ts()` receives writable MPEG-TS bytes.  Plugins advertising
`TVS_CA_CAP_TS_INPLACE` may modify packet bytes in place but must preserve the
buffer length and 188-byte packet framing.  The host contains no raw CW getter,
setter, callback or network export interface.

Returning `TVS_CA_RESULT_PASSTHROUGH` leaves the transport unchanged. Returning
`TVS_CA_RESULT_RETRY` also keeps the transport flowing while the backend
recovers. A hard backend error does not truncate the TS; the dashboard remains
able to report that media packets are still scrambled.

## Reader configuration

Each `ca_readers` item in `tvstreammersat5-config.json` can select a backend:

```json
{
  "reader_key": "/dev/serial/by-id/...",
  "serial": "...",
  "max_services": 10,
  "auto_activate": true,
  "auto_reactivate": true,
  "retry_seconds": 5,
  "backend_id": "passthrough",
  "backend_config": "{}"
}
```

`backend_config` is an opaque JSON string passed only to that backend in
`tvs_ca_reader_info_v1`. The built-in `passthrough` backend is the safe default
and intentionally does not decode anything.

## Building the example plugin

Enable the optional CMake target:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DTVSTREAMMERSAT5_BUILD_CA_PLUGIN_EXAMPLE=ON
cmake --build build --parallel "$(nproc)"
```

The example validates ABI loading and lifecycle but intentionally returns
`TVS_CA_RESULT_PASSTHROUGH` for every TS buffer.

## Quick ABI check

After building a plugin, verify that it exports the required entry point:

```bash
./scripts/check_ca_backend_plugin.sh /path/to/backend.so
```
