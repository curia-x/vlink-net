# vlink-net architecture

vlink-net is a lightweight Linux virtual Ethernet framework over pluggable transports.
The first implementation contains:

- `vlink_core`: netdev integration, Ethernet skb TX/RX, frame encode/decode, MTU, stats, queueing.
- `vlink_loopback`: local test backend, creates vlink pairs.
- `vlink_uart_serdev`: UART backend implemented as a serdev client.

The intended layering is:

```text
Linux network stack
  -> vlink net_device
  -> vlink frame protocol
  -> transport ops
  -> loopback / UART / future SPI/I2C/SDIO backends
```

`vlink_uart_serdev` is intentionally thin. It does not parse Ethernet or own the netdev path.
It only maps serdev RX/TX callbacks to vlink transport callbacks.
