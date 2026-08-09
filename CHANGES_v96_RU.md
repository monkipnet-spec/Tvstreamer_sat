# TVStreamer v96 — shared DVB multicast interface fix

- Исправлен запуск FTA через shared DVB hub на Linux-системах, где loopback `lo` не имеет флага multicast.
- Внутренний DVB multicast больше не привязывается к `127.0.0.1` / `lo`.
- Shared `udpsink` выбирает реальный UP-интерфейс с поддержкой multicast; приоритет: `input_interface_address`, затем основной `interface_address`, затем первый доступный multicast-интерфейс.
- FTA `udpsrc` присоединяется к той же сети по реальному IPv4 адресу выбранного интерфейса.
- В лог добавлены `iface=<name>` и `localaddr=<IPv4>` для shared DVB hub и FTA input.
- Схема v95 сохранена: один DVB frontend на транспондер и single-service remux выбранного SID без повторного открытия DVB frontend.
