# Test plan

## 1. Loopback backend

```bash
insmod vlink_core.ko
insmod vlink_loopback.ko pairs=1
ip link
ip link set vlinklp0 up
ip link set vlinklp1 up
ip addr add 10.0.0.1/24 dev vlinklp0
ip addr add 10.0.0.2/24 dev vlinklp1
ping -c 3 10.0.0.2
ethtool -S vlinklp0
ip -s link show vlinklp0
```

For stronger isolation, move each endpoint into a separate network namespace.

## 2. UART serdev backend under QEMU

Use two PL011 UARTs: one for console and one for vlink. Add a serdev child under the second UART node:

```dts
&uart1 {
    status = "okay";

    vlink_uart {
        compatible = "demo,vlink-uart";
        current-speed = <115200>;
        vlink,mtu = <512>;
        vlink,max-payload = <2048>;
        local-mac-address = [02 00 00 00 00 01];
    };
};
```

Connect the second UART of two QEMU guests with a chardev socket. The exact command depends on the
kernel/rootfs setup, but the model is:

```text
Guest A ttyAMA1 <-> QEMU unix socket <-> Guest B ttyAMA1
```

Then configure IP addresses on the vlink netdev in each guest and test ping/iperf.
