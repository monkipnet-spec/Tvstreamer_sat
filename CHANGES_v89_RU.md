# TVStreamer v89 — DVB-S2 direct fallback for services invisible through relay

Исправления для случая, когда DVB-S2 frontend получает LOCK, shared DVB frontend стартует,
но per-service relay по выбранному SID не выдаёт ни одного TS-пакета:

- При первом `input timeout` спутникового shared service relay поток больше не перезапускает
  тот же relay по кругу.
- Добавлен автоматический fallback на прямой DVB-S/S2 input для конкретного канала:
  `dvbbasebin` сам настраивает frontend и фильтрует `program-numbers=<SID>`.
- Это особенно важно для FTA-каналов, где CA вообще не участвует, но SID не появляется
  на выходе внутреннего `tsdemux -> mpegtsmux` relay.
- Убран crash-loop, который возникал при повторном разрушении/создании live relay pipeline.
- Добавлены диагностические строки `tsdemux pad detected: ...`, чтобы видеть, какие ES pad'ы
  реально появляются из выбранного SID.

Это не меняет CA/descrambling. Исправление касается только DVB-S/S2 получения сервиса.
