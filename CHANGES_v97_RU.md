# TVStreamer v97 — FTA remux → HTTP output fix

- DVB-S2/shared multicast path from v96 is unchanged.
- `multifdsink` HTTP output now uses `sync=false`; a network fan-out sink must not wait for renderer-style clock synchronisation of MPEG-TS timestamps.
- Dynamic `tsdemux -> queue -> parser -> mpegtsmux` branches are fully linked before the new elements are synchronized with the already PLAYING parent pipeline.
- Added one-shot diagnostics for the first VIDEO buffer, AUDIO buffer, MPEG-TS buffer after `mpegtsmux`, and buffer arriving at the output sink.
- This targets the confirmed symptom: HTTP returns `200 OK` but sends 0 bytes while DVB-S2 and `tsdemux` already detect H.264/audio.
