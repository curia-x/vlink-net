# SPDX-License-Identifier: GPL-2.0
TARGET_KDIR ?= /home/book/source_code/linux

all:
	make -C $(TARGET_KDIR) M=$(PWD) modules
	make -C $(TARGET_KDIR) M=$(PWD) compile_commands.json

obj-m += vlink_core.o
obj-m += vlink_loopback.o
obj-m += vlink_uart_serdev.o

vlink_core-y := core/vlink_core.o
vlink_loopback-y := transport/vlink_loopback.o
vlink_uart_serdev-y := transport/vlink_uart_serdev.o

ccflags-y += -I$(src)/include

clean:
	make -C $(TARGET_KDIR) M=$(PWD) clean
