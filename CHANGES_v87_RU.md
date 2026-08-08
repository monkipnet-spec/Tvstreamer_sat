# TVStreamer v87 — Phoenix load reduction without key cache

Изменения v87 направлены на снижение нагрузки на Phoenix/reader и повышение устойчивости DVB/TS без работы с ключами conditional access.

## Что изменено

- Добавлен throttling старта для каналов, назначенных на одну CA Card/Provider: новые старты через одну карту разнесены минимум на 1.5 секунды.
- Добавлено автоматическое восстановление внутреннего satellite service relay для спутниковых каналов без резервного входа.
- При `no input signal` у FTA/DVB канала TVStreamer теперь пробует перезапустить связку shared DVB frontend/service relay, а не просто оставляет канал в ошибке.
- Добавлен триггер восстановления при большом числе входных MPEG-TS continuity errors.
- Восстановление relay защищено anti-storm интервалом: не чаще одного раза в 12 секунд на канал.
- Статус FTA no-input теперь явно показывает, что CA не участвует и была попытка recovery.

## Что не добавлено

- ECM/CW transport не добавлен.
- Software descrambler не добавлен.
- Кэш CW/ECM/CA-ключей не добавлен.

## Проверка

После запуска проблемного FTA канала смотрите журнал:

```bash
journalctl -u TVStreamer5 -n 250 --no-pager | grep -E 'Restarting shared satellite input|Satellite service relay|Shared DVB|continuity|no input|ERROR|error|SID='
```

