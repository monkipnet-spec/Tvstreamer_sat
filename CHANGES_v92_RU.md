# TVStreamer v92 — DVB-S2 FTA relay liveness fix

- Исправлен ложный `input timeout` для уже найденного и реально демультиплексируемого DVB-S/S2 сервиса.
- В `SatelliteServiceRelayState` добавлен независимый счётчик байтов на выходе service relay непосредственно перед loopback `udpsink`.
- Watchdog shared DVB-S2 теперь считает relay живым, если выбранный SID реально выдаёт MPEG-TS байты, даже если общий downstream input probe не обновился.
- Это предотвращает циклический restart исправного FTA канала каждые ~5 секунд после строк `tsdemux pad detected` / `remap linked ...`.
- CA Provider изменения v91 сохранены; данная правка относится только к DVB-S/S2 приёму/ретрансляции.
