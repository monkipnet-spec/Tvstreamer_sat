# TVStreamer v85 — Dynamic Reader/Card Manager

## Что изменено

- Убрана модель `Authorized pre-decoded TS` из интерфейса CA Providers.
- Количество Phoenix/serial-reader устройств больше нигде не предполагается фиксированным.
- `System -> CA Providers / Cards` динамически читает все `/dev/serial/by-id/*` и показывает текущий `ttyUSB/ttyACM`, vendor, model и serial.
- Для каждого обнаруженного reader можно одной кнопкой создать отдельный `ca-card-N`; есть также кнопка добавления всех новых reader'ов.
- Provider хранит стабильный `reader_by_id`, поэтому смена `/dev/ttyUSB0` на `/dev/ttyUSB2` после reboot не меняет привязку.
- Reader, который временно исчез, отображается как `OFFLINE`; после возврата того же by-id снова становится `ONLINE`.
- Лимит одновременных каналов принадлежит каждой карте/provider отдельно. Глобальной константы `8` нет.
- Добавлены `capacity_mode=manual|auto` и индивидуальный `max_channels`/fallback до 1024.
- Режим `Auto` подготовлен для документированного card/provider capability interface. Пока такой интерфейс не сообщает лимит, используется индивидуальный fallback.
- Start проверяет: provider существует, включён, reader назначен, reader online и индивидуальный session limit не исчерпан.
- В мастере спутникового канала и редакторе выбирается `CA карта`, а не `/dev/ttyUSBx`.
- Плитка канала показывает имя карты/provider, ONLINE/OFFLINE и текущую загрузку `active/max`.
- Существующие provider ID можно переименовывать с автоматической миграцией ссылок каналов.

## Граница реализации

В v85 Reader/Card Manager не реализует ECM/CW extraction, software descrambler или CW cache. Он отвечает за hardware discovery, stable identity, hot-plug status и session accounting.
