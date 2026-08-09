# TVStreamer v91 — modular CA Provider backends

## Что изменено

- CA Provider получил выбор backend'а:
  - `Phoenix / serial reader` — существующий локальный Reader/Card Manager.
  - `Newcamd Status (TCP only)` — отдельный сетевой health/status backend.
- Добавлены `src/NewcamdStatusBackend.h/.cpp`.
- Newcamd Status принимает только `host:port`, выполняет DNS/TCP connect с таймаутом и возвращает состояния `TCP_ONLINE`, `TCP_OFFLINE`, `DNS_ERROR`, `NOT_CONFIGURED`.
- Статус backend'а включён в `/api/state` (`backend_status`).
- Web UI сохраняет `backend_type` и `endpoint` вместо принудительного сброса этих полей в `reader`/пустую строку.
- Для сетевого backend'а добавлена форма endpoint и отображение TCP состояния.
- Network health-check выполняется один раз на provider при формировании state, чтобы не создавать лишних соединений.
- `newcamd-status` намеренно является только мониторинговым backend'ом. Если его выбрать для зашифрованного DVB-S/S2 канала, Start возвращает понятную ошибку вместо создания ложного рабочего состояния.

## Что намеренно не включено

- Newcamd authentication/key exchange.
- ECM/EMM transport.
- Получение или хранение CW.
- Software descrambling.
- Любой key cache.

## Совместимость

Старые CA Providers без `backend_type` продолжают загружаться как `reader`. Формат существующих stream/channel настроек не менялся.
