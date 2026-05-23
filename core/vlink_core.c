// SPDX-License-Identifier: GPL-2.0
/*
 * vlink_core - lightweight virtual Ethernet over pluggable transports
 *
 * This is a first-pass skeleton intended for Linux 6.x kernels.  It keeps
 * the main Linux netdev/protocol logic in the core and lets UART/SPI/I2C/etc.
 * backends only provide byte/packet transport operations.
 */

#include <linux/crc32.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/if_ether.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#include "vlink.h"

#define VLINK_DRV_NAME		"vlink"
#define VLINK_VERSION		1
#define VLINK_MAGIC0		'V'
#define VLINK_MAGIC1		'L'
#define VLINK_MAX_TXQLEN	128
#define VLINK_WAKE_TXQLEN	64

#define VLINK_FRAME_DATA	1

struct vlink_hdr {
	u8 magic[2];
	u8 version;
	u8 type;
	__le16 hdr_len;
	__le16 payload_len;
	__le32 flags;
	__le32 crc32;
} __packed;

enum vlink_rx_state {
	VLINK_RX_MAGIC0,
	VLINK_RX_MAGIC1,
	VLINK_RX_HDR,
	VLINK_RX_PAYLOAD,
};

struct vlink_rx_parser {
	enum vlink_rx_state state;
	u8 hdr_buf[sizeof(struct vlink_hdr)];
	unsigned int hdr_pos;
	u8 *payload;
	unsigned int payload_pos;
	unsigned int payload_len;
};

struct vlink_priv_stats {
	u64 rx_bad_magic;
	u64 rx_bad_version;
	u64 rx_bad_len;
	u64 rx_crc_errors;
	u64 rx_alloc_fail;
	u64 tx_queue_full;
	u64 tx_short_write;
	u64 tx_send_errors;
};

struct vlink_frame {
	struct list_head list;
	unsigned int len;
	unsigned int off;
	u8 data[];
};

struct vlink_dev {
	struct net_device *ndev;
	struct device *parent;
	struct vlink_transport transport;

	struct mutex rx_lock;
	struct vlink_rx_parser rx;

	spinlock_t tx_lock;
	struct list_head txq;
	unsigned int txqlen;
	struct work_struct tx_work;
	bool stopping;

	spinlock_t stats_lock;
	struct rtnl_link_stats64 stats;
	struct vlink_priv_stats priv_stats;
};

static const char vlink_gstrings[][ETH_GSTRING_LEN] = {
	"rx_bad_magic",
	"rx_bad_version",
	"rx_bad_len",
	"rx_crc_errors",
	"rx_alloc_fail",
	"tx_queue_full",
	"tx_short_write",
	"tx_send_errors",
};

static void vlink_rx_reset(struct vlink_dev *vdev)
{
	kfree(vdev->rx.payload);
	vdev->rx.payload = NULL;
	vdev->rx.payload_pos = 0;
	vdev->rx.payload_len = 0;
	vdev->rx.hdr_pos = 0;
	vdev->rx.state = VLINK_RX_MAGIC0;
}

static void vlink_stat_rx_packet(struct vlink_dev *vdev, unsigned int len)
{
	unsigned long flags;

	spin_lock_irqsave(&vdev->stats_lock, flags);
	vdev->stats.rx_packets++;
	vdev->stats.rx_bytes += len;
	spin_unlock_irqrestore(&vdev->stats_lock, flags);
}

static void vlink_stat_tx_packet(struct vlink_dev *vdev, unsigned int len)
{
	unsigned long flags;

	spin_lock_irqsave(&vdev->stats_lock, flags);
	vdev->stats.tx_packets++;
	vdev->stats.tx_bytes += len;
	spin_unlock_irqrestore(&vdev->stats_lock, flags);
}

static void vlink_stat_rx_drop(struct vlink_dev *vdev)
{
	unsigned long flags;

	spin_lock_irqsave(&vdev->stats_lock, flags);
	vdev->stats.rx_dropped++;
	spin_unlock_irqrestore(&vdev->stats_lock, flags);
}

static void vlink_stat_tx_drop(struct vlink_dev *vdev)
{
	unsigned long flags;

	spin_lock_irqsave(&vdev->stats_lock, flags);
	vdev->stats.tx_dropped++;
	spin_unlock_irqrestore(&vdev->stats_lock, flags);
}

static void vlink_stat_rx_error(struct vlink_dev *vdev)
{
	unsigned long flags;

	spin_lock_irqsave(&vdev->stats_lock, flags);
	vdev->stats.rx_errors++;
	spin_unlock_irqrestore(&vdev->stats_lock, flags);
}

static void vlink_stat_tx_error(struct vlink_dev *vdev)
{
	unsigned long flags;

	spin_lock_irqsave(&vdev->stats_lock, flags);
	vdev->stats.tx_errors++;
	spin_unlock_irqrestore(&vdev->stats_lock, flags);
}

static void vlink_priv_inc(struct vlink_dev *vdev, u64 *field)
{
	unsigned long flags;

	spin_lock_irqsave(&vdev->stats_lock, flags);
	(*field)++;
	spin_unlock_irqrestore(&vdev->stats_lock, flags);
}

static u32 vlink_crc(const u8 *payload, unsigned int len)
{
	return crc32_le(~0, payload, len);
}

static int vlink_deliver_payload(struct vlink_dev *vdev, const u8 *payload,
					 unsigned int len)
{
	struct net_device *ndev = vdev->ndev;
	struct sk_buff *skb;

	if (unlikely(len < ETH_HLEN)) {
		vlink_priv_inc(vdev, &vdev->priv_stats.rx_bad_len);
		vlink_stat_rx_error(vdev);
		vlink_stat_rx_drop(vdev);
		return -EINVAL;
	}

	skb = netdev_alloc_skb_ip_align(ndev, len);
	if (!skb) {
		vlink_priv_inc(vdev, &vdev->priv_stats.rx_alloc_fail);
		vlink_stat_rx_drop(vdev);
		return -ENOMEM;
	}

	skb_put_data(skb, payload, len);
	skb->dev = ndev;
	skb->protocol = eth_type_trans(skb, ndev);

	vlink_stat_rx_packet(vdev, len);
	netif_rx(skb);
	return 0;
}

static void vlink_rx_byte(struct vlink_dev *vdev, u8 byte)
{
	struct vlink_hdr *hdr;
	u32 crc;

	switch (vdev->rx.state) {
	case VLINK_RX_MAGIC0:
		if (byte == VLINK_MAGIC0) {
			vdev->rx.hdr_buf[0] = byte;
			vdev->rx.hdr_pos = 1;
			vdev->rx.state = VLINK_RX_MAGIC1;
		} else {
			vlink_priv_inc(vdev, &vdev->priv_stats.rx_bad_magic);
		}
		break;
	case VLINK_RX_MAGIC1:
		if (byte == VLINK_MAGIC1) {
			vdev->rx.hdr_buf[1] = byte;
			vdev->rx.hdr_pos = 2;
			vdev->rx.state = VLINK_RX_HDR;
		} else if (byte == VLINK_MAGIC0) {
			vdev->rx.hdr_buf[0] = byte;
			vdev->rx.hdr_pos = 1;
		} else {
			vlink_priv_inc(vdev, &vdev->priv_stats.rx_bad_magic);
			vdev->rx.state = VLINK_RX_MAGIC0;
			vdev->rx.hdr_pos = 0;
		}
		break;
	case VLINK_RX_HDR:
		vdev->rx.hdr_buf[vdev->rx.hdr_pos++] = byte;
		if (vdev->rx.hdr_pos < sizeof(struct vlink_hdr))
			break;

		hdr = (struct vlink_hdr *)vdev->rx.hdr_buf;
		if (hdr->version != VLINK_VERSION || hdr->type != VLINK_FRAME_DATA) {
			vlink_priv_inc(vdev, &vdev->priv_stats.rx_bad_version);
			vlink_stat_rx_error(vdev);
			vlink_rx_reset(vdev);
			break;
		}

		if (le16_to_cpu(hdr->hdr_len) != sizeof(*hdr) ||
		    le16_to_cpu(hdr->payload_len) < ETH_HLEN ||
		    le16_to_cpu(hdr->payload_len) > vdev->transport.max_payload) {
			vlink_priv_inc(vdev, &vdev->priv_stats.rx_bad_len);
			vlink_stat_rx_error(vdev);
			vlink_rx_reset(vdev);
			break;
		}

		vdev->rx.payload_len = le16_to_cpu(hdr->payload_len);
		vdev->rx.payload_pos = 0;
		vdev->rx.payload = kmalloc(vdev->rx.payload_len, GFP_ATOMIC);
		if (!vdev->rx.payload) {
			vlink_priv_inc(vdev, &vdev->priv_stats.rx_alloc_fail);
			vlink_stat_rx_drop(vdev);
			vlink_rx_reset(vdev);
			break;
		}

		vdev->rx.state = VLINK_RX_PAYLOAD;
		break;
	case VLINK_RX_PAYLOAD:
		vdev->rx.payload[vdev->rx.payload_pos++] = byte;
		if (vdev->rx.payload_pos < vdev->rx.payload_len)
			break;

		hdr = (struct vlink_hdr *)vdev->rx.hdr_buf;
		crc = vlink_crc(vdev->rx.payload, vdev->rx.payload_len);
		if (crc != le32_to_cpu(hdr->crc32)) {
			vlink_priv_inc(vdev, &vdev->priv_stats.rx_crc_errors);
			vlink_stat_rx_error(vdev);
			vlink_stat_rx_drop(vdev);
		} else {
			vlink_deliver_payload(vdev, vdev->rx.payload,
					     vdev->rx.payload_len);
		}

		vlink_rx_reset(vdev);
		break;
	}
}

size_t vlink_rx_bytes(struct vlink_dev *vdev, const u8 *buf, size_t len)
{
	size_t i;

	if (!vdev || !buf)
		return 0;

	mutex_lock(&vdev->rx_lock);
	for (i = 0; i < len; i++)
		vlink_rx_byte(vdev, buf[i]);
	mutex_unlock(&vdev->rx_lock);

	return len;
}
EXPORT_SYMBOL_GPL(vlink_rx_bytes);

static struct vlink_frame *vlink_encode_skb(struct vlink_dev *vdev,
					    struct sk_buff *skb)
{
	struct vlink_frame *frame;
	struct vlink_hdr *hdr;
	unsigned int payload_len = skb->len;
	unsigned int len = sizeof(*hdr) + payload_len;

	frame = kmalloc(sizeof(*frame) + len, GFP_ATOMIC);
	if (!frame)
		return NULL;

	INIT_LIST_HEAD(&frame->list);
	frame->len = len;
	frame->off = 0;

	hdr = (struct vlink_hdr *)frame->data;
	hdr->magic[0] = VLINK_MAGIC0;
	hdr->magic[1] = VLINK_MAGIC1;
	hdr->version = VLINK_VERSION;
	hdr->type = VLINK_FRAME_DATA;
	hdr->hdr_len = cpu_to_le16(sizeof(*hdr));
	hdr->payload_len = cpu_to_le16(payload_len);
	hdr->flags = 0;
	hdr->crc32 = cpu_to_le32(vlink_crc(skb->data, payload_len));

	if (skb_copy_bits(skb, 0, frame->data + sizeof(*hdr), payload_len)) {
		kfree(frame);
		return NULL;
	}

	return frame;
}

static void vlink_tx_worker(struct work_struct *work)
{
	struct vlink_dev *vdev = container_of(work, struct vlink_dev, tx_work);
	struct vlink_frame *frame;
	unsigned long flags;
	ssize_t ret;

	for (;;) {
		spin_lock_irqsave(&vdev->tx_lock, flags);
		if (list_empty(&vdev->txq) || vdev->stopping) {
			spin_unlock_irqrestore(&vdev->tx_lock, flags);
			break;
		}
		frame = list_first_entry(&vdev->txq, struct vlink_frame, list);
		spin_unlock_irqrestore(&vdev->tx_lock, flags);

		ret = vdev->transport.ops->send(&vdev->transport,
						 frame->data + frame->off,
						 frame->len - frame->off);
		if (ret == 0 || ret == -EAGAIN)
			break;
		if (ret < 0) {
			vlink_priv_inc(vdev, &vdev->priv_stats.tx_send_errors);
			vlink_stat_tx_error(vdev);
			break;
		}

		frame->off += ret;
		if (frame->off < frame->len) {
			vlink_priv_inc(vdev, &vdev->priv_stats.tx_short_write);
			break;
		}

		spin_lock_irqsave(&vdev->tx_lock, flags);
		list_del(&frame->list);
		vdev->txqlen--;
		if (netif_queue_stopped(vdev->ndev) &&
		    vdev->txqlen <= VLINK_WAKE_TXQLEN)
			netif_wake_queue(vdev->ndev);
		spin_unlock_irqrestore(&vdev->tx_lock, flags);

		kfree(frame);
	}
}

void vlink_tx_wake(struct vlink_dev *vdev)
{
	if (vdev)
		schedule_work(&vdev->tx_work);
}
EXPORT_SYMBOL_GPL(vlink_tx_wake);

void vlink_carrier_on(struct vlink_dev *vdev)
{
	if (vdev)
		netif_carrier_on(vdev->ndev);
}
EXPORT_SYMBOL_GPL(vlink_carrier_on);

void vlink_carrier_off(struct vlink_dev *vdev)
{
	if (vdev)
		netif_carrier_off(vdev->ndev);
}
EXPORT_SYMBOL_GPL(vlink_carrier_off);

static netdev_tx_t vlink_start_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct vlink_dev *vdev = netdev_priv(ndev);
	struct vlink_frame *frame;
	unsigned long flags;

	if (unlikely(skb->len > vdev->transport.max_payload)) {
		vlink_stat_tx_drop(vdev);
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}

	frame = vlink_encode_skb(vdev, skb);
	if (!frame) {
		vlink_stat_tx_drop(vdev);
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}

	spin_lock_irqsave(&vdev->tx_lock, flags);
	if (vdev->txqlen >= VLINK_MAX_TXQLEN) {
		vlink_priv_inc(vdev, &vdev->priv_stats.tx_queue_full);
		netif_stop_queue(ndev);
		spin_unlock_irqrestore(&vdev->tx_lock, flags);
		kfree(frame);
		vlink_stat_tx_drop(vdev);
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}
	list_add_tail(&frame->list, &vdev->txq);
	vdev->txqlen++;
	if (vdev->txqlen >= VLINK_MAX_TXQLEN)
		netif_stop_queue(ndev);
	spin_unlock_irqrestore(&vdev->tx_lock, flags);

	vlink_stat_tx_packet(vdev, skb->len);
	dev_kfree_skb_any(skb);
	schedule_work(&vdev->tx_work);
	return NETDEV_TX_OK;
}

static int vlink_open(struct net_device *ndev)
{
	struct vlink_dev *vdev = netdev_priv(ndev);
	int ret = 0;

	vdev->stopping = false;
	if (vdev->transport.ops->open) {
		ret = vdev->transport.ops->open(&vdev->transport);
		if (ret)
			return ret;
	}

	/* 表示链路层 carrier 可用 */
	netif_carrier_on(ndev);

	/* 允许网络栈调用这个网卡的 ndo_start_xmit() 发送 skb */
	/* 与 netif_stop_queue(ndev) 对应 */
	netif_start_queue(ndev);
	return 0;
}

static int vlink_stop(struct net_device *ndev)
{
	struct vlink_dev *vdev = netdev_priv(ndev);
	struct vlink_frame *frame, *tmp;
	unsigned long flags;

	netif_stop_queue(ndev);
	netif_carrier_off(ndev);

	spin_lock_irqsave(&vdev->tx_lock, flags);
	vdev->stopping = true;
	spin_unlock_irqrestore(&vdev->tx_lock, flags);

	cancel_work_sync(&vdev->tx_work);

	if (vdev->transport.ops->close)
		vdev->transport.ops->close(&vdev->transport);

	spin_lock_irqsave(&vdev->tx_lock, flags);
	list_for_each_entry_safe(frame, tmp, &vdev->txq, list) {
		list_del(&frame->list);
		kfree(frame);
	}
	vdev->txqlen = 0;
	spin_unlock_irqrestore(&vdev->tx_lock, flags);

	mutex_lock(&vdev->rx_lock);
	vlink_rx_reset(vdev);
	mutex_unlock(&vdev->rx_lock);

	return 0;
}

static int vlink_change_mtu(struct net_device *ndev, int new_mtu)
{
	struct vlink_dev *vdev = netdev_priv(ndev);

	if (new_mtu < ndev->min_mtu || new_mtu > ndev->max_mtu)
		return -EINVAL;
	if (new_mtu + ETH_HLEN > vdev->transport.max_payload)
		return -EINVAL;

	WRITE_ONCE(ndev->mtu, new_mtu);
	return 0;
}

static void vlink_get_stats64(struct net_device *ndev,
				      struct rtnl_link_stats64 *stats)
{
	struct vlink_dev *vdev = netdev_priv(ndev);
	unsigned long flags;

	spin_lock_irqsave(&vdev->stats_lock, flags);
	memcpy(stats, &vdev->stats, sizeof(*stats));
	spin_unlock_irqrestore(&vdev->stats_lock, flags);
}

static const struct net_device_ops vlink_netdev_ops = {
	.ndo_open		= vlink_open,
	.ndo_stop		= vlink_stop,
	.ndo_start_xmit		= vlink_start_xmit,
	.ndo_change_mtu		= vlink_change_mtu,
	.ndo_validate_addr	= eth_validate_addr,
	.ndo_set_mac_address	= eth_mac_addr,
	.ndo_get_stats64	= vlink_get_stats64,
};

static void vlink_get_drvinfo(struct net_device *ndev,
			      struct ethtool_drvinfo *info)
{
	strscpy(info->driver, VLINK_DRV_NAME, sizeof(info->driver));
	strscpy(info->version, "0.1", sizeof(info->version));
}

static int vlink_get_sset_count(struct net_device *ndev, int sset)
{
	if (sset == ETH_SS_STATS)
		return ARRAY_SIZE(vlink_gstrings);
	return -EOPNOTSUPP;
}

static void vlink_get_strings(struct net_device *ndev, u32 sset, u8 *data)
{
	if (sset != ETH_SS_STATS)
		return;
	memcpy(data, vlink_gstrings, sizeof(vlink_gstrings));
}

static void vlink_get_ethtool_stats(struct net_device *ndev,
				    struct ethtool_stats *stats, u64 *data)
{
	struct vlink_dev *vdev = netdev_priv(ndev);
	unsigned long flags;

	spin_lock_irqsave(&vdev->stats_lock, flags);
	data[0] = vdev->priv_stats.rx_bad_magic;
	data[1] = vdev->priv_stats.rx_bad_version;
	data[2] = vdev->priv_stats.rx_bad_len;
	data[3] = vdev->priv_stats.rx_crc_errors;
	data[4] = vdev->priv_stats.rx_alloc_fail;
	data[5] = vdev->priv_stats.tx_queue_full;
	data[6] = vdev->priv_stats.tx_short_write;
	data[7] = vdev->priv_stats.tx_send_errors;
	spin_unlock_irqrestore(&vdev->stats_lock, flags);
}

static const struct ethtool_ops vlink_ethtool_ops = {
	.get_drvinfo		= vlink_get_drvinfo,
	.get_sset_count		= vlink_get_sset_count,
	.get_strings		= vlink_get_strings,
	.get_ethtool_stats	= vlink_get_ethtool_stats,
};

static void vlink_setup(struct net_device *ndev)
{
	ether_setup(ndev);
	ndev->netdev_ops = &vlink_netdev_ops;
	ndev->ethtool_ops = &vlink_ethtool_ops;
	/*
	 * 一般 IPv4 的最小可用报文要求：
	 * 20 字节 IPv4 header + 8 字节 transport header + 40 字节重组相关余量 = 68
	 */
	ndev->min_mtu = ETH_MIN_MTU;
	ndev->mtu = VLINK_DEFAULT_MTU;
}

struct vlink_dev *vlink_register_transport(struct device *parent,
					   const struct vlink_transport_ops *ops,
					   void *priv,
					   const struct vlink_config *cfg)
{
	struct net_device *ndev;
	struct vlink_dev *vdev;
	unsigned int max_payload;
	int ret;

	if (!ops || !ops->send)
		return ERR_PTR(-EINVAL);

	max_payload = cfg && cfg->max_payload ? cfg->max_payload :
			      VLINK_DEFAULT_MAX_PAYLOAD;
	if (max_payload < ETH_HLEN + 68)
		return ERR_PTR(-EINVAL);

	ndev = alloc_netdev(sizeof(*vdev),
			   cfg && cfg->ifname ? cfg->ifname : "vlink%d",
			   NET_NAME_UNKNOWN, vlink_setup);
	if (!ndev)
		return ERR_PTR(-ENOMEM);

	vdev = netdev_priv(ndev);
	vdev->ndev = ndev;
	vdev->parent = parent;
	vdev->transport.ops = ops;
	vdev->transport.priv = priv;
	vdev->transport.caps = ops->caps;
	vdev->transport.max_payload = max_payload;

	mutex_init(&vdev->rx_lock);
	vlink_rx_reset(vdev);
	spin_lock_init(&vdev->tx_lock);
	INIT_LIST_HEAD(&vdev->txq);
	INIT_WORK(&vdev->tx_work, vlink_tx_worker);
	spin_lock_init(&vdev->stats_lock);

	ndev->max_mtu = max_payload - ETH_HLEN;
	ndev->mtu = cfg && cfg->mtu ? cfg->mtu : min_t(unsigned int,
					VLINK_DEFAULT_MTU, ndev->max_mtu);

	if (cfg && cfg->has_dev_addr)
		eth_hw_addr_set(ndev, cfg->dev_addr);
	else
		eth_hw_addr_random(ndev);

	if (parent)
		SET_NETDEV_DEV(ndev, parent);

	netif_carrier_off(ndev);
	ret = register_netdev(ndev);
	if (ret) {
		free_netdev(ndev);
		return ERR_PTR(ret);
	}

	return vdev;
}
EXPORT_SYMBOL_GPL(vlink_register_transport);

void vlink_unregister_transport(struct vlink_dev *vdev)
{
	if (!vdev)
		return;

	unregister_netdev(vdev->ndev);
	cancel_work_sync(&vdev->tx_work);
	vlink_rx_reset(vdev);
	free_netdev(vdev->ndev);
}
EXPORT_SYMBOL_GPL(vlink_unregister_transport);

MODULE_DESCRIPTION("vlink core: virtual Ethernet over pluggable transports");
MODULE_AUTHOR("demo");
MODULE_LICENSE("GPL");
