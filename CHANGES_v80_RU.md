# TVStreamer v80 — логический CA Provider

## Что изменено

- Удалена привязка физического `conditional_access_reader` к отдельному каналу.
- Канал теперь хранит только `ca_provider_id`.
- В глобальном конфиге добавлен массив `ca_providers`.
- В меню **System → CA Providers** можно создавать и редактировать логические провайдеры.
- Для CA Provider доступны:
  - `id`;
  - название;
  - тип внешнего backend;
  - endpoint;
  - `max_channels` (по умолчанию 8);
  - enabled/disabled.
- В `/api/state` для каждого provider выводятся:
  - `assigned_channels`;
  - `active_channels`;
  - `available_channels`;
  - `capacity_ok`;
  - `backend_connected` / `backend_status`.
- Лимит проверяется по одновременно активным спутниковым каналам. Канал, работающий с резервного входа, слот CA Provider не занимает.
- При попытке запустить канал сверх `max_channels` кнопка Start получает явную ошибку.
- В мастере спутниковых каналов вместо `/dev/ttyUSB*`, `/dev/ttyACM*` и DVB CA выбирается только логический CA Provider.
- В редактировании уже созданного спутникового канала CA Provider можно изменить.
- На плитке спутникового канала отображается назначенный provider и его текущая загрузка.
- Ввод спутниковой частоты в MHz из v79 сохранён.

## Важно

Эта версия реализует модель CA Provider, распределение каналов и контроль лимита, но не реализует встроенный обмен ECM/CW, чтение Phoenix-карты или descrambling. Поле `endpoint` предназначено для дальнейшей интеграции с внешним авторизованным CA/CAM backend.

Старое поле `conditional_access_reader` больше не используется. После обновления нужно создать CA Provider в **System → CA Providers** и назначить его нужным спутниковым каналам.

## Пример конфигурации

```json
{
  "ca_providers": [
    {
      "id": "ca-main",
      "name": "Main CA Provider",
      "backend_type": "external",
      "endpoint": "",
      "max_channels": 8,
      "enabled": true
    }
  ],
  "streams": [
    {
      "id": "channel-1",
      "ca_provider_id": "ca-main"
    }
  ]
}
```
