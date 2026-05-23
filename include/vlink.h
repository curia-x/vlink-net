/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _VLINK_H
#define _VLINK_H

#include <linux/device.h>
#include <linux/etherdevice.h>
#include <linux/types.h>

struct vlink_dev;
struct vlink_transport;

#define VLINK_F_STREAM		BIT(0)
#define VLINK_F_PACKET		BIT(1)
#define VLINK_F_RELIABLE	BIT(2)
#define VLINK_F_ORDERED		BIT(3)
#define VLINK_F_ASYNC_RX	BIT(4)
#define VLINK_F_HAS_CARRIER	BIT(5)

#define VLINK_DEFAULT_MTU	512
#define VLINK_DEFAULT_MAX_PAYLOAD	2048

struct vlink_config {
	const char *ifname;
	unsigned int mtu;
	unsigned int max_payload;
	u8 dev_addr[ETH_ALEN];
	bool has_dev_addr;
};

struct vlink_transport_ops {
	const char *name;
	u32 caps;

	int (*open)(struct vlink_transport *t);
	void (*close)(struct vlink_transport *t);

	/*
	 * Send bytes to the transport.
	 * Return >0 bytes accepted, 0/-EAGAIN when temporarily unable to accept,
	 * or a negative errno on fatal failure.
	 */
	ssize_t (*send)(struct vlink_transport *t, const u8 *buf, size_t len);
};

struct vlink_transport {
	const struct vlink_transport_ops *ops;
	void *priv;
	u32 caps;
	unsigned int max_payload;
};

struct vlink_dev *vlink_register_transport(struct device *parent,
					   const struct vlink_transport_ops *ops,
					   void *priv,
					   const struct vlink_config *cfg);
void vlink_unregister_transport(struct vlink_dev *vdev);

size_t vlink_rx_bytes(struct vlink_dev *vdev, const u8 *buf, size_t len);
void vlink_tx_wake(struct vlink_dev *vdev);
void vlink_carrier_on(struct vlink_dev *vdev);
void vlink_carrier_off(struct vlink_dev *vdev);

#endif /* _VLINK_H */
