# TVStreamer v95 — shared DVB-S2 hub for FTA + single-service remux

## Исправлено

- FTA канал больше не открывает `/dev/dvb/adapterN/frontendM` вторым `dvbbasebin`.
- Один `dvbsrc` владеет frontend и публикует полный транспондер во внутреннюю multicast-группу на loopback.
- Основной pipeline FTA читает этот multicast напрямую и применяет single-service remux из v94 (`tsdemux program-number=SID -> parsers -> mpegtsmux`).
- Удалён промежуточный per-service loopback relay из FTA пути.
- Для FTA больше не запускается watchdog `Restarting shared satellite input`, предназначенный для старого service relay.
- Это устраняет `GstDvbSrc: Failed to start`, возникавший при повторном открытии уже занятого frontend.
- OpenSSL 3 остаётся постоянной зависимостью проекта: `OpenSSL::SSL` и `OpenSSL::Crypto`.

## Ожидаемый FTA путь

`dvbsrc -> UDP multicast full transponder (lo) -> udpsrc -> tsdemux SID -> parser(s) -> mpegtsmux -> output`

Для FTA в журнале должны появляться `FTA shared DVB hub selected` и `FTA shared-transponder input attached`. Строки `FTA direct DVB input selected` и `Satellite service relay started` для FTA больше не ожидаются.
