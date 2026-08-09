# TVStreamer v90 — DVB-S2 shared hub via dvbsrc + безопасный GstBus

Изменения:

- Shared DVB frontend теперь в первую очередь использует `dvbsrc` для получения полного MPEG-TS транспондера.
- Для `dvbsrc` задаётся `pids=8192`, чтобы отдавался полный transport stream, а не выбранные программные pad'ы.
- Per-service relay по SID продолжает выделять нужный канал через `tsdemux program-number`.
- Убран аварийный бесконечный переход в direct DVB fallback: после повторного no-input recovery останавливается с понятной диагностикой, чтобы не входить в crash-loop.
- Исправлена защита `monitorBus`: перед `gst_bus_timed_pop()` проверяется, что bus ещё валиден.
- Перед перезапуском pipeline старый GstBus переводится в flushing, чтобы поток мониторинга не читал освобождённый bus.

Не добавлено:

- newcamd/ECM/CW transport;
- программный descrambler;
- кэш ключей условного доступа.
