# TVStreamer v84

## CA Provider: корректный режим Authorized pre-decoded TS

- Убран предупреждающий диалог при успешном запуске через `Authorized pre-decoded TS`: это штатный поддерживаемый транспорт входного MPEG-TS.
- Если спутниковому каналу назначен CA Provider типа `external`, `cam-service` или `custom`, запуск теперь не продолжается с ложным предупреждением. API Start возвращает понятную ошибку и предлагает выбрать `Authorized pre-decoded TS` и настроить endpoint.
- Пустой endpoint у `Authorized pre-decoded TS` блокирует запуск с явной ошибкой.
- Новый CA Provider в интерфейсе по умолчанию создаётся с backend `Authorized pre-decoded TS`, а не `external`.
- В списке выбора CA Provider отображается, является ли provider поддерживаемым pre-decoded TS transport.
- `/api/state` публикует `stream_transport_supported` и более точный `backend_status`.

## Что не входит в v84

TVStreamer не принимает, не хранит и не экспортирует ECM/CW и не реализует программный descrambler. Для CA Provider используется уже расшифрованный MPEG-TS от авторизованного внешнего CAM/CA backend.
