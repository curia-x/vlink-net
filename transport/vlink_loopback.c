// SPDX-License-Identifier: GPL-2.0
/* vlink loopback transport: creates vlink pairs for local/QEMU testing. */

#include <linux/module.h>
#include <linux/slab.h>

#include <vlink.h>

struct vlink_loop_ep {
	struct vlink_dev *vdev;
	struct vlink_loop_ep *peer;
};

struct vlink_loop_pair {
	struct list_head list;
	struct vlink_loop_ep ep[2];
};

static LIST_HEAD(vlink_loop_pairs);
static int pairs = 1;
module_param(pairs, int, 0444);
MODULE_PARM_DESC(pairs, "number of loopback vlink pairs to create");

static ssize_t vlink_loop_send(struct vlink_transport *t,
			       const u8 *buf, size_t len)
{
	struct vlink_loop_ep *ep = t->priv;

	if (!ep->peer || !ep->peer->vdev)
		return -ENODEV;

	vlink_rx_bytes(ep->peer->vdev, buf, len);
	return len;
}

static const struct vlink_transport_ops vlink_loop_ops = {
	.name = "loopback",
	.caps = VLINK_F_PACKET | VLINK_F_RELIABLE | VLINK_F_ORDERED | VLINK_F_ASYNC_RX,
	.send = vlink_loop_send,
};

static int vlink_loop_create_pair(int id)
{
	struct vlink_loop_pair *pair;
	struct vlink_config cfg0 = { .ifname = "vlinklp%d" };
	struct vlink_config cfg1 = { .ifname = "vlinklp%d" };

	pair = kzalloc(sizeof(*pair), GFP_KERNEL);
	if (!pair)
		return -ENOMEM;

	pair->ep[0].peer = &pair->ep[1];
	pair->ep[1].peer = &pair->ep[0];

	pair->ep[0].vdev = vlink_register_transport(NULL, &vlink_loop_ops,
						       &pair->ep[0], &cfg0);
	if (IS_ERR(pair->ep[0].vdev)) {
		int ret = PTR_ERR(pair->ep[0].vdev);

		kfree(pair);
		return ret;
	}

	pair->ep[1].vdev = vlink_register_transport(NULL, &vlink_loop_ops,
						       &pair->ep[1], &cfg1);
	if (IS_ERR(pair->ep[1].vdev)) {
		int ret = PTR_ERR(pair->ep[1].vdev);

		vlink_unregister_transport(pair->ep[0].vdev);
		kfree(pair);
		return ret;
	}

	INIT_LIST_HEAD(&pair->list);
	list_add_tail(&pair->list, &vlink_loop_pairs);
	pr_info("vlink_loopback: created pair %d\n", id);
	return 0;
}

static int __init vlink_loop_init(void)
{
	int i, ret;

	if (pairs < 0)
		return -EINVAL;

	for (i = 0; i < pairs; i++) {
		ret = vlink_loop_create_pair(i);
		if (ret)
			return ret;
	}

	return 0;
}

static void __exit vlink_loop_exit(void)
{
	struct vlink_loop_pair *pair, *tmp;

	list_for_each_entry_safe(pair, tmp, &vlink_loop_pairs, list) {
		list_del(&pair->list);
		vlink_unregister_transport(pair->ep[0].vdev);
		vlink_unregister_transport(pair->ep[1].vdev);
		kfree(pair);
	}
}

module_init(vlink_loop_init);
module_exit(vlink_loop_exit);

MODULE_DESCRIPTION("vlink loopback transport");
MODULE_AUTHOR("demo");
MODULE_LICENSE("GPL");
MODULE_SOFTDEP("pre: vlink_core");
