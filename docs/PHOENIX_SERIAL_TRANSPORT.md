# PhoenixSerialTransport (v136)

`src/ca/PhoenixSerialTransport.{h,cpp}` is the low-level Phoenix/SmartMouse reader lifecycle layer.

It provides:

- exclusive local tty ownership and restoration of the original port state;
- DCD/CD card-presence detection;
- Linux `termios2` + `BOTHER` custom baud profiles;
- RTS/DTR reset pulses;
- bounded ATR collection driven by `poll()` (no `O_NONBLOCK` race);
- reconnect and one-shot probe helpers;
- 6 MHz / Fi=372 profile (16129 baud) for the known FTDI readers, with 9600 fallback.

The transport deliberately does **not** expose generic APDU exchange, control-word access or MPEG-TS descrambling. Protocol-specific authorised logic belongs in a separate CaBackend plugin/SDK.
