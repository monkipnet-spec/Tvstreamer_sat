# TVStreamer5 v82

- Добавлен безопасный CA Provider transport типа `authorized-ts`: TVStreamer принимает уже расшифрованный MPEG-TS от внешнего авторизованного CAM/CA backend.
- Endpoint поддерживает шаблоны `{service_id}`, `{stream_id}`, `{frequency_khz}`, `{frequency_mhz}`.
- Для provider transport увеличен входной GStreamer queue до 8 секунд.
- Добавлено автоматическое переподключение provider transport при GStreamer ERROR и при отсутствии входных данных.
- В памяти процесса хранится только состояние transport/session и последний успешный endpoint; CW/ECM/CA keys не принимаются, не сохраняются и не экспортируются.
- Ограничение `max_channels` из v80 сохранено.
- Равные плитки каналов/потоков из v81 сохранены.
