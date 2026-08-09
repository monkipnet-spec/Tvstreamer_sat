# TVStreamer v88 — CA Card/Provider как отдельный модуль

## Что изменено

- Логика CA Card/Provider вынесена из `HttpServer.cpp` в отдельный модуль:
  - `src/CaProviderManager.h`
  - `src/CaProviderManager.cpp`
- Модуль отвечает за:
  - обнаружение serial/Phoenix reader'ов через `/dev/serial/by-id/*`;
  - разрешение стабильного `by-id` пути в текущий `/dev/ttyUSB*` или `/dev/ttyACM*`;
  - чтение `udev` metadata: vendor, model, serial, USB path;
  - поиск reader'а по стабильному `by-id`;
  - поиск CA Card/Provider по логическому ID;
  - расчёт индивидуального лимита каналов конкретной карты;
  - статусы `NO_READER`, `OFFLINE`, `READER_ONLINE`;
  - текстовое состояние менеджера для UI/API.
- `HttpServer.cpp` больше не содержит низкоуровневую логику обнаружения reader'ов.
- `/api/serial-readers` и `/api/state` используют новый модуль.
- Проверка Start для зашифрованного спутникового сервиса использует новый модуль для проверки reader online и лимита конкретной карты.

## Что не изменено

- Формат конфигурации `ca_providers[]` сохранён.
- Старые `ca-card-*` и уже назначенные каналы сохраняются.
- Частоты в MHz, FTA bypass, равные плитки и recovery v87 сохранены.
- ECM/CW transport, software descrambler и key-cache не добавлены.

## Зачем это сделано

CA Card/Provider теперь является отдельной частью архитектуры, а не набором функций внутри HTTP-интерфейса. Следующие изменения по reader/card management можно будет делать в одном модуле, не смешивая их с UI, API и StreamManager.
