# TVStreamer v93 — прямой DVB-S/S2 путь для FTA

## Исправлено

- FTA спутниковые сервисы больше не проходят через промежуточный per-service relay
  `shared DVB -> tsdemux -> mpegtsmux -> udp://127.0.0.1`.
- Для FTA используется прямой источник `dvbbasebin` с `program-numbers=<SID>` и
  далее обычный output pipeline TVStreamer.
- Это устраняет ложный `input timeout`, который возникал после успешного
  обнаружения video/audio PID, когда промежуточный `mpegtsmux` не выдавал
  пакеты в loopback UDP.
- Добавлен лог `FTA direct DVB input selected`.
- После окончательного отключения recovery shared relay watchdog больше не
  продолжает печатать attempt=3,4,5...

## Ожидаемый лог FTA

```text
FTA direct DVB input selected: ... SID=230 ...
Satellite input: adapter=5 frontend=0 ... service_id=230
Pipeline for stream ... input_proto=satellite ...
```

Для FTA больше не должны появляться `Satellite service relay started` и
`Restarting shared satellite input`.
