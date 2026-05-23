// SPDX-License-Identifier: GPL-2.0
/* vlink UART serdev backend: thin serdev glue around vlink core. */

#include <linux/etherdevice.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/property.h>
#include <linux/serdev.h>
#include <linux/slab.h>

#include "vlink.h"

struct vlink_uart {
	struct serdev_device *serdev;
	struct vlink_dev *vdev;
	bool dying;
	unsigned int baudrate;
	bool flow_control;
};

static size_t vlink_uart_receive_buf(struct serdev_device *serdev,
				     const u8 *buf, size_t len)
{
	struct vlink_uart *vu = serdev_device_get_drvdata(serdev);

	if (!vu || !vu->vdev)
		return 0;

	return vlink_rx_bytes(vu->vdev, buf, len);
}

static void vlink_uart_write_wakeup(struct serdev_device *serdev)
{
	struct vlink_uart *vu = serdev_device_get_drvdata(serdev);

	if (vu && vu->vdev)
		vlink_tx_wake(vu->vdev);
}

static const struct serdev_device_ops vlink_uart_serdev_ops = {
	.receive_buf = vlink_uart_receive_buf,
	.write_wakeup = vlink_uart_write_wakeup,
};

static int vlink_uart_open(struct vlink_transport *t)
{
	/* serdev is opened during probe so that RX can be accepted early. */
	return 0;
}

static void vlink_uart_close(struct vlink_transport *t)
{
	/* closed in remove; netdev stop only stops the logical network queue. */
}

static ssize_t vlink_uart_send(struct vlink_transport *t,
			       const u8 *buf, size_t len)
{
	struct vlink_uart *vu = t->priv;
	int ret;

	if (READ_ONCE(vu->dying))
		return -ENODEV;

	ret = serdev_device_write_buf(vu->serdev, buf, len);
	if (ret < 0)
		return ret;
	return ret;
}

static const struct vlink_transport_ops vlink_uart_ops = {
	.name = "uart-serdev",
	.caps = VLINK_F_STREAM | VLINK_F_ORDERED | VLINK_F_ASYNC_RX,
	.open = vlink_uart_open,
	.close = vlink_uart_close,
	.send = vlink_uart_send,
};

static void vlink_uart_read_config(struct serdev_device *serdev,
				   struct vlink_uart *vu,
				   struct vlink_config *cfg)
{
	u32 val;
	u8 mac[ETH_ALEN];

	vu->baudrate = 115200;
	device_property_read_u32(&serdev->dev, "current-speed", &vu->baudrate);
	vu->flow_control = device_property_read_bool(&serdev->dev, "hw-flow-control");

	if (!device_property_read_u32(&serdev->dev, "vlink,mtu", &val))
		cfg->mtu = val;
	if (!device_property_read_u32(&serdev->dev, "vlink,max-payload", &val))
		cfg->max_payload = val;
	if (!device_property_read_u8_array(&serdev->dev, "local-mac-address",
					      mac, ETH_ALEN)) {
		ether_addr_copy(cfg->dev_addr, mac);
		cfg->has_dev_addr = true;
	}
}

static int vlink_uart_probe(struct serdev_device *serdev)
{
	struct vlink_uart *vu;
	struct vlink_config cfg = { };
	int ret;

	vu = devm_kzalloc(&serdev->dev, sizeof(*vu), GFP_KERNEL);
	if (!vu)
		return -ENOMEM;

	vu->serdev = serdev;
	serdev_device_set_drvdata(serdev, vu);
	serdev_device_set_client_ops(serdev, &vlink_uart_serdev_ops);
	vlink_uart_read_config(serdev, vu, &cfg);

	ret = serdev_device_open(serdev);
	if (ret)
		return ret;

	serdev_device_set_baudrate(serdev, vu->baudrate);
	serdev_device_set_flow_control(serdev, vu->flow_control);

	vu->vdev = vlink_register_transport(&serdev->dev, &vlink_uart_ops, vu, &cfg);
	if (IS_ERR(vu->vdev)) {
		ret = PTR_ERR(vu->vdev);
		serdev_device_close(serdev);
		return ret;
	}

	dev_info(&serdev->dev, "registered vlink UART backend baud=%u flow=%d\n",
		 vu->baudrate, vu->flow_control);
	return 0;
}

static void vlink_uart_remove(struct serdev_device *serdev)
{
	struct vlink_uart *vu = serdev_device_get_drvdata(serdev);

	if (!vu)
		return;

	WRITE_ONCE(vu->dying, true);
	serdev_device_close(serdev);
	vlink_unregister_transport(vu->vdev);
	vu->vdev = NULL;
}

static const struct of_device_id vlink_uart_of_match[] = {
	{ .compatible = "demo,vlink-uart" },
	{ }
};
MODULE_DEVICE_TABLE(of, vlink_uart_of_match);

static struct serdev_device_driver vlink_uart_driver = {
	.probe = vlink_uart_probe,
	.remove = vlink_uart_remove,
	.driver = {
		.name = "vlink-uart",
		.of_match_table = vlink_uart_of_match,
	},
};

module_serdev_device_driver(vlink_uart_driver);

MODULE_DESCRIPTION("vlink UART serdev transport");
MODULE_AUTHOR("demo");
MODULE_LICENSE("GPL");
MODULE_SOFTDEP("pre: vlink_core");
