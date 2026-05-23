# vlink-net

A first-pass Linux virtual Ethernet framework over pluggable transports.

The UART backend is only a demo transport. The core idea is:

```text
net_device + Ethernet skb path + common vlink framing + transport ops
```

Current modules:

- `vlink_core.ko`: core netdev/protocol implementation.
- `vlink_loopback.ko`: local pair backend for basic testing.
- `vlink_uart_serdev.ko`: UART backend using serdev.

Target kernel: Linux 6.x. The serdev backend uses the current `size_t receive_buf(...)` API.

## Build

```bash
make -C /path/to/linux M=$PWD modules
```

## Load loopback test

```bash
insmod vlink_core.ko
insmod vlink_loopback.ko pairs=1
ip link set vlinklp0 up
ip link set vlinklp1 up
ip addr add 10.0.0.1/24 dev vlinklp0
ip addr add 10.0.0.2/24 dev vlinklp1
ping 10.0.0.2
ethtool -S vlinklp0
```

## UART backend

`vlink_uart_serdev` is a serdev client driver. The device tree node should be a child of the UART
controller node, not a separate platform node with a phandle to the UART.

See `docs/qemu-test.md`.

## Scope intentionally left out

- bonding / multipath / failover
- encryption / compression
- retransmission / TCP-like reliability
- offload / XDP / GRO / GSO
- real SPI/I2C/SDIO backends

These can be added later, but they are not needed for the first complete driver.
