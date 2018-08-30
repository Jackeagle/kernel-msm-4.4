// SPDX-License-Identifier: GPL-2.0

/* Copyright (c) 2014-2018, The Linux Foundation. All rights reserved.
 * Copyright (C) 2018 Linaro Ltd.
 */

/* WWAN Transport Network Driver. */

#define pr_fmt(fmt)    "ipa-wan %s:%d " fmt, __func__, __LINE__

#include <linux/completion.h>
#include <linux/errno.h>
#include <linux/if_arp.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/of_device.h>
#include <linux/string.h>
#include <linux/skbuff.h>
#include <linux/version.h>
#include <linux/workqueue.h>
#include <net/pkt_sched.h>
#include "net_map.h"
#include "msm_rmnet.h"
#include "rmnet_config.h"
#include "ipa_qmi.h"
#include "ipa_i.h"

#define WWAN_METADATA_SHFT 24
#define WWAN_METADATA_MASK 0xFF000000
#define WWAN_DATA_LEN 2000
#define IPA_RM_INACTIVITY_TIMER 100 /* IPA_RM */
#define HEADROOM_FOR_QMAP   8 /* for mux header */
#define TAILROOM	    0 /* for padding by mux layer */
#define MAX_NUM_OF_MUX_CHANNEL	10 /* max mux channels */
#define UL_FILTER_RULE_HANDLE_START 69
#define DEFAULT_OUTSTANDING_HIGH 128
#define DEFAULT_OUTSTANDING_HIGH_CTL (DEFAULT_OUTSTANDING_HIGH + 32)
#define DEFAULT_OUTSTANDING_LOW 64

#define IPA_WWAN_DEV_NAME "rmnet_ipa%d"
#define IPA_UPSTEAM_WLAN_IFACE_NAME "wlan0"

#define IPA_WWAN_RX_SOFTIRQ_THRESH 16

#define INVALID_MUX_ID 0xFF
#define IPA_QUOTA_REACH_ALERT_MAX_SIZE 64
#define IPA_QUOTA_REACH_IF_NAME_MAX_SIZE 64
#define IPA_UEVENT_NUM_EVNP 4 /* number of event pointers */
#define NAPI_WEIGHT 60
#define DRIVER_NAME "wwan_ioctl"

#define IPA_APPS_WWAN_CONS_RING_COUNT	128
#define IPA_APPS_WWAN_PROD_RING_COUNT	256

static int ipa_rmnet_poll(struct napi_struct *napi, int budget);

static void ipa_wake_tx_queue(struct work_struct *work);
static DECLARE_WORK(ipa_tx_wakequeue_work, ipa_wake_tx_queue);

/** struct ipa_wwan_private - WWAN private data
 * @net: network interface struct implemented by this driver
 * @stats: iface statistics
 * @outstanding_pkts: number of packets sent to IPA without TX complete ACKed
 * @outstanding_high: number of outstanding packets allowed
 * @outstanding_low: number of outstanding packets which shall cause
 * @ch_id: channel id
 * @lock: spinlock for mutual exclusion
 * @device_active: true if device is active
 *
 * WWAN private - holds all relevant info about WWAN driver
 */
struct ipa_wwan_private {
	struct net_device_stats stats;
	atomic_t outstanding_pkts;
	int outstanding_high_ctl;
	int outstanding_high;
	int outstanding_low;
	bool device_active;
	struct napi_struct napi;
};

struct ipa_rmnet_mux_val {
	u32  mux_id;
};

struct rmnet_ipa_context {
	struct net_device *dev;
	struct ipa_sys_connect_params apps_to_ipa_ep_cfg;
	struct ipa_sys_connect_params ipa_to_apps_ep_cfg;
	struct ipa_rmnet_mux_val mux_channel[MAX_NUM_OF_MUX_CHANNEL];
	int num_q6_rules;
	int old_num_q6_rules;
	int rmnet_index;	/* updates protected by add_mux_channel_lock */
	bool egress_set;
	bool a7_ul_flt_set;
	u32 apps_to_ipa_hdl;
	u32 ipa_to_apps_hdl;
	struct mutex pipe_handle_guard;		/* pipe setup/teardown */
	struct mutex add_mux_channel_lock;	/* updates to mux_channel[] */
};

static bool initialized;	/* Avoid duplicate initialization */

static struct rmnet_ipa_context rmnet_ipa_ctx_struct;
static struct rmnet_ipa_context *rmnet_ipa_ctx = &rmnet_ipa_ctx_struct;

static int ipa_find_mux_channel_index(u32 mux_id)
{
	int i;

	for (i = 0; i < MAX_NUM_OF_MUX_CHANNEL; i++) {
		if (mux_id == rmnet_ipa_ctx->mux_channel[i].mux_id)
			return i;
	}
	return MAX_NUM_OF_MUX_CHANNEL;
}

/** wwan_open() - Opens the wwan network interface. Opens logical
 * channel on A2 MUX driver and starts the network stack queue
 *
 * @dev: network device
 *
 * Return codes:
 * 0: success
 */
static int ipa_wwan_open(struct net_device *dev)
{
	struct ipa_wwan_private *wwan_ptr = netdev_priv(dev);

	ipa_debug("[%s] wwan_open()\n", dev->name);
	wwan_ptr->device_active = true;
	napi_enable(&wwan_ptr->napi);
	netif_start_queue(dev);

	return 0;
}

/** ipa_wwan_stop() - Stops the wwan network interface. Closes
 * logical channel on A2 MUX driver and stops the network stack
 * queue
 *
 * @dev: network device
 *
 * Return codes:
 * 0: success
 */
static int ipa_wwan_stop(struct net_device *dev)
{
	struct ipa_wwan_private *wwan_ptr = netdev_priv(dev);

	ipa_debug("%s: stop %s\n", __func__, dev->name);
	wwan_ptr->device_active = false;
	netif_stop_queue(dev);

	return 0;
}

/** ipa_wwan_xmit() - Transmits an skb.
 *
 * @skb: skb to be transmitted
 * @dev: network device
 *
 * Return codes:
 * 0: success
 * NETDEV_TX_BUSY: Error while transmitting the skb. Try again
 * later
 * -EFAULT: Error while transmitting the skb
 */
static int ipa_wwan_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct ipa_wwan_private *wwan_ptr = netdev_priv(dev);
	unsigned int skb_len;
	int outstanding;

	if (skb->protocol != htons(ETH_P_MAP)) {
		dev_kfree_skb_any(skb);
		dev->stats.tx_dropped++;
		return NETDEV_TX_OK;
	}

	/* Control packets are sent even if queue is stopped.  We
	 * always honor the data and control high-water marks.
	 */
	outstanding = atomic_read(&wwan_ptr->outstanding_pkts);
	if (!RMNET_MAP_GET_CD_BIT(skb)) {	/* Data packet? */
		if (netif_queue_stopped(dev))
			return NETDEV_TX_BUSY;
		if (outstanding >= wwan_ptr->outstanding_high)
			return NETDEV_TX_BUSY;
	} else if (outstanding >= wwan_ptr->outstanding_high_ctl) {
		return NETDEV_TX_BUSY;
	}

	/* both data packets and commands will be routed to
	 * IPA_CLIENT_Q6_WAN_CONS based on status configuration.
	 */
	skb_len = skb->len;
	if (ipa_tx_dp(IPA_CLIENT_APPS_WAN_PROD, skb))
		return NETDEV_TX_BUSY;

	atomic_inc(&wwan_ptr->outstanding_pkts);
	dev->stats.tx_packets++;
	dev->stats.tx_bytes += skb_len;

	return NETDEV_TX_OK;
}

/** apps_ipa_tx_complete_notify() - Rx notify
 *
 * @priv: driver context
 * @evt: event type
 * @data: data provided with event
 *
 * Check that the packet is the one we sent and release it
 * This function will be called in defered context in IPA wq.
 */
static void apps_ipa_tx_complete_notify(void *priv, enum ipa_dp_evt_type evt,
					unsigned long data)
{
	struct sk_buff *skb = (struct sk_buff *)data;
	struct net_device *dev = (struct net_device *)priv;
	struct ipa_wwan_private *wwan_ptr;

	if (dev != rmnet_ipa_ctx->dev) {
		ipa_debug("Received pre-SSR packet completion\n");
		dev_kfree_skb_any(skb);
		return;
	}

	if (evt != IPA_WRITE_DONE) {
		ipa_err("unsupported evt on Tx callback, Drop the packet\n");
		dev_kfree_skb_any(skb);
		dev->stats.tx_dropped++;
		return;
	}

	wwan_ptr = netdev_priv(dev);
	atomic_dec(&wwan_ptr->outstanding_pkts);
	__netif_tx_lock_bh(netdev_get_tx_queue(dev, 0));
	if (netif_queue_stopped(dev) &&
	    atomic_read(&wwan_ptr->outstanding_pkts) <
				wwan_ptr->outstanding_low) {
		ipa_debug_low("Outstanding low (%d) - waking up queue\n",
			      wwan_ptr->outstanding_low);
		netif_wake_queue(dev);
	}

	__netif_tx_unlock_bh(netdev_get_tx_queue(dev, 0));
	dev_kfree_skb_any(skb);
}

/** apps_ipa_packet_receive_notify() - Rx notify
 *
 * @priv: driver context
 * @evt: event type
 * @data: data provided with event
 *
 * IPA will pass a packet to the Linux network stack with skb->data
 */
static void apps_ipa_packet_receive_notify(void *priv, enum ipa_dp_evt_type evt,
					   unsigned long data)
{
	struct net_device *dev = priv;
	struct ipa_wwan_private *wwan_ptr = netdev_priv(dev);

	if (evt == IPA_RECEIVE) {
		struct sk_buff *skb = (struct sk_buff *)data;
		int result;
		unsigned int packet_len = skb->len;

		ipa_debug("Rx packet was received\n");
		skb->dev = rmnet_ipa_ctx->dev;
		skb->protocol = htons(ETH_P_MAP);

		result = netif_receive_skb(skb);
		if (result) {
			pr_err_ratelimited("fail on netif_receive_skb\n");
			dev->stats.rx_dropped++;
		}
		dev->stats.rx_packets++;
		dev->stats.rx_bytes += packet_len;
	} else if (evt == IPA_CLIENT_START_POLL) {
		napi_schedule(&wwan_ptr->napi);
	} else if (evt == IPA_CLIENT_COMP_NAPI) {
		napi_complete(&wwan_ptr->napi);
	} else {
		ipa_err("Invalid evt %d received in wan_ipa_receive\n", evt);
	}
}

static int handle_ingress_format(struct net_device *dev,
				  struct rmnet_ioctl_extended_s *in)
{
	int ret;
	struct ipa_sys_connect_params *ipa_wan_ep_cfg;

	ipa_debug("Get RMNET_IOCTL_SET_INGRESS_DATA_FORMAT\n");
	ipa_wan_ep_cfg = &rmnet_ipa_ctx->ipa_to_apps_ep_cfg;
	if (in->u.data & RMNET_IOCTL_INGRESS_FORMAT_CHECKSUM)
		ipa_wan_ep_cfg->ipa_ep_cfg.cfg.cs_offload_en =
		   IPA_ENABLE_CS_OFFLOAD_DL;

	if (in->u.data & RMNET_IOCTL_INGRESS_FORMAT_AGG_DATA) {
		ipa_debug("get AGG size %d count %d\n",
			  in->u.ingress_format.agg_size,
			  in->u.ingress_format.agg_count);

		ret = ipa_disable_apps_wan_cons_deaggr(
			  in->u.ingress_format.agg_size,
			  in->u.ingress_format.agg_count);
		if (ret)
			return ret;

		ipa_wan_ep_cfg->ipa_ep_cfg.aggr.aggr_byte_limit =
			in->u.ingress_format.agg_size;
		ipa_wan_ep_cfg->ipa_ep_cfg.aggr.aggr_pkt_limit =
			in->u.ingress_format.agg_count;
	}

	ipa_wan_ep_cfg->ipa_ep_cfg.hdr.hdr_len =
					sizeof(struct rmnet_map_header_s);
	ipa_wan_ep_cfg->ipa_ep_cfg.hdr.hdr_ofst_metadata_valid = 1;
	ipa_wan_ep_cfg->ipa_ep_cfg.hdr.hdr_ofst_metadata = 1;
	ipa_wan_ep_cfg->ipa_ep_cfg.hdr.hdr_ofst_pkt_size_valid = 1;
	ipa_wan_ep_cfg->ipa_ep_cfg.hdr.hdr_ofst_pkt_size = 2;

	ipa_wan_ep_cfg->ipa_ep_cfg.hdr_ext.hdr_total_len_or_pad_valid = true;
	ipa_wan_ep_cfg->ipa_ep_cfg.hdr_ext.hdr_total_len_or_pad = IPA_HDR_PAD;
	ipa_wan_ep_cfg->ipa_ep_cfg.hdr_ext.hdr_payload_len_inc_padding = true;
	ipa_wan_ep_cfg->ipa_ep_cfg.metadata_mask.metadata_mask = 0xFF000000;

	ipa_wan_ep_cfg->client = IPA_CLIENT_APPS_WAN_CONS;
	ipa_wan_ep_cfg->notify = apps_ipa_packet_receive_notify;
	ipa_wan_ep_cfg->priv = dev;

	ipa_wan_ep_cfg->napi_enabled = true;
	ipa_wan_ep_cfg->fifo_count = IPA_APPS_WWAN_CONS_RING_COUNT;

	mutex_lock(&rmnet_ipa_ctx->pipe_handle_guard);

	ret = ipa_setup_sys_pipe(ipa_wan_ep_cfg);
	if (ret < 0) {
		ipa_err("failed to configure ingress\n");
		mutex_unlock(&rmnet_ipa_ctx->pipe_handle_guard);

		return ret;
	}
	rmnet_ipa_ctx->ipa_to_apps_hdl = ret;

	mutex_unlock(&rmnet_ipa_ctx->pipe_handle_guard);

	return 0;
}

/** handle_egress_format() - Egress data format configuration
 *
 * Setup IPA egress system pipe and Configure:
 *	header handling, checksum, de-aggregation and fifo size
 *
 * @dev: network device
 * @e: egress configuration
 */
static int handle_egress_format(struct net_device *dev,
				 struct rmnet_ioctl_extended_s *e)
{
	int rc;
	struct ipa_sys_connect_params *ipa_wan_ep_cfg;

	ipa_debug("get RMNET_IOCTL_SET_EGRESS_DATA_FORMAT\n");
	ipa_wan_ep_cfg = &rmnet_ipa_ctx->apps_to_ipa_ep_cfg;
	ipa_wan_ep_cfg->ipa_ep_cfg.hdr.hdr_len =
					sizeof(struct rmnet_map_header_s);
	if (e->u.data & RMNET_IOCTL_EGRESS_FORMAT_CHECKSUM) {
		ipa_wan_ep_cfg->ipa_ep_cfg.hdr.hdr_len += sizeof(u32);
		ipa_wan_ep_cfg->ipa_ep_cfg.cfg.cs_offload_en =
			IPA_ENABLE_CS_OFFLOAD_UL;
		ipa_wan_ep_cfg->ipa_ep_cfg.cfg.cs_metadata_hdr_offset = 1;
	}

	if (e->u.data & RMNET_IOCTL_EGRESS_FORMAT_AGGREGATION) {
		ipa_err("WAN UL Aggregation enabled\n");

		ipa_wan_ep_cfg->ipa_ep_cfg.aggr.aggr_en = IPA_ENABLE_DEAGGR;
		ipa_wan_ep_cfg->ipa_ep_cfg.aggr.aggr = IPA_QCMAP;

		ipa_wan_ep_cfg->ipa_ep_cfg.hdr.hdr_ofst_pkt_size = 2;

		ipa_wan_ep_cfg->ipa_ep_cfg.hdr_ext.hdr_total_len_or_pad_valid =
			true;
		ipa_wan_ep_cfg->ipa_ep_cfg.hdr_ext.hdr_total_len_or_pad =
			IPA_HDR_PAD;
		ipa_wan_ep_cfg->ipa_ep_cfg.hdr_ext.hdr_pad_to_alignment =
			2;
		ipa_wan_ep_cfg->ipa_ep_cfg.hdr_ext.hdr_payload_len_inc_padding =
			true;
	} else {
		ipa_debug("WAN UL Aggregation disabled\n");
		ipa_wan_ep_cfg->ipa_ep_cfg.aggr.aggr_en = IPA_BYPASS_AGGR;
	}

	ipa_wan_ep_cfg->ipa_ep_cfg.hdr.hdr_ofst_metadata_valid = 1;
	/* modem want offset at 0! */
	ipa_wan_ep_cfg->ipa_ep_cfg.hdr.hdr_ofst_metadata = 0;

	ipa_wan_ep_cfg->ipa_ep_cfg.mode.dst = IPA_CLIENT_APPS_WAN_PROD;
	ipa_wan_ep_cfg->ipa_ep_cfg.mode.mode = IPA_BASIC;

	ipa_wan_ep_cfg->client = IPA_CLIENT_APPS_WAN_PROD;
	ipa_wan_ep_cfg->notify = apps_ipa_tx_complete_notify;
	ipa_wan_ep_cfg->fifo_count = IPA_APPS_WWAN_PROD_RING_COUNT;
	ipa_wan_ep_cfg->priv = dev;

	mutex_lock(&rmnet_ipa_ctx->pipe_handle_guard);

	rc = ipa_setup_sys_pipe(ipa_wan_ep_cfg);
	if (rc < 0) {
		ipa_err("failed to config egress endpoint\n");
		mutex_unlock(&rmnet_ipa_ctx->pipe_handle_guard);

		return rc;
	}
	rmnet_ipa_ctx->apps_to_ipa_hdl = rc;

	mutex_unlock(&rmnet_ipa_ctx->pipe_handle_guard);

	if (rmnet_ipa_ctx->num_q6_rules != 0) {
		rmnet_ipa_ctx->a7_ul_flt_set = true;
	} else {
		/* wait Q6 UL filter rules*/
		ipa_debug("no UL-rules\n");
	}
	rmnet_ipa_ctx->egress_set = true;

	return 0;
}

static int ipa_wwan_add_mux_channel(struct rmnet_ioctl_extended_s *edata)
{
	struct ipa_rmnet_mux_val *mux_channel;
	u32 mux_id = edata->u.rmnet_mux_val.mux_id;
	int mux_index;
	int rmnet_index;

	mux_index = ipa_find_mux_channel_index(mux_id);
	if (mux_index < MAX_NUM_OF_MUX_CHANNEL)
		return 0;

	mutex_lock(&rmnet_ipa_ctx->add_mux_channel_lock);

	if (rmnet_ipa_ctx->rmnet_index >= MAX_NUM_OF_MUX_CHANNEL) {
		ipa_err("Exceed mux_channel limit(%d)\n", rmnet_index);
		mutex_unlock(&rmnet_ipa_ctx->add_mux_channel_lock);
		return -EFAULT;
	}
	rmnet_index = rmnet_ipa_ctx->rmnet_index++;

	/* cache the mux name and id */
	mux_channel = rmnet_ipa_ctx->mux_channel;
	mux_channel[rmnet_index].mux_id = mux_id;

	mutex_unlock(&rmnet_ipa_ctx->add_mux_channel_lock);

	return 0;
}

static int ipa_wwan_ioctl_extended(struct net_device *dev, void __user *data)
{
	struct rmnet_ioctl_extended_s edata = { };
	size_t size = sizeof(edata);

	if (copy_from_user(&edata, data, size))
		return -EFAULT;

	ipa_debug("RMNET_IOCTL_EXTENDED 0x%08x\n", edata.extended_ioctl);

	switch (edata.extended_ioctl) {
	case RMNET_IOCTL_GET_SUPPORTED_FEATURES:	/* Get features */
		edata.u.data = RMNET_IOCTL_FEAT_NOTIFY_MUX_CHANNEL;
		edata.u.data |= RMNET_IOCTL_FEAT_SET_EGRESS_DATA_FORMAT;
		edata.u.data |= RMNET_IOCTL_FEAT_SET_INGRESS_DATA_FORMAT;

		return copy_to_user(data, &edata, size) ? -EFAULT : 0;

	case RMNET_IOCTL_SET_MRU:			/* Set MRU */
		return 0;

	case RMNET_IOCTL_GET_MRU:			/* Get MRU */
		edata.u.data = 1000;

		return copy_to_user(data, &edata, size) ? -EFAULT : 0;

	case RMNET_IOCTL_GET_SG_SUPPORT:		/* Get SG support */
		edata.u.data = 1;	/* Scatter/gather is always supported */

		return copy_to_user(data, &edata, size) ? -EFAULT : 0;

	case RMNET_IOCTL_GET_EPID:			/* Get endpoint ID */
		edata.u.data = 1;

		return copy_to_user(data, &edata, size) ? -EFAULT : 0;

	case RMNET_IOCTL_GET_EP_PAIR:			/* Get endpoint pair */
		edata.u.ipa_ep_pair.consumer_pipe_num =
				ipa_get_ep_mapping(IPA_CLIENT_APPS_WAN_PROD);
		edata.u.ipa_ep_pair.producer_pipe_num =
				ipa_get_ep_mapping(IPA_CLIENT_APPS_WAN_CONS);

		return copy_to_user(data, &edata, size) ? -EFAULT : 0;

	case RMNET_IOCTL_GET_DRIVER_NAME:		/* Get driver name */
		memcpy(&edata.u.if_name, rmnet_ipa_ctx->dev->name, IFNAMSIZ);

		return copy_to_user(data, &edata, size) ? -EFAULT : 0;

	case RMNET_IOCTL_ADD_MUX_CHANNEL:		/* Add MUX ID */
		return ipa_wwan_add_mux_channel(&edata);

	case RMNET_IOCTL_SET_EGRESS_DATA_FORMAT:	/* Egress data format */
		return handle_egress_format(dev, &edata) ? -EFAULT : 0;

	case RMNET_IOCTL_SET_INGRESS_DATA_FORMAT:	/* Ingress format */
		return handle_ingress_format(dev, &edata) ? -EFAULT : 0;

	case RMNET_IOCTL_GET_AGGREGATION_COUNT:		/* Get agg count */
	case RMNET_IOCTL_SET_AGGREGATION_COUNT:		/* Set agg count */
	case RMNET_IOCTL_GET_AGGREGATION_SIZE:		/* Get agg size */
	case RMNET_IOCTL_SET_AGGREGATION_SIZE:		/* Set agg size */
	case RMNET_IOCTL_FLOW_CONTROL:			/* Do flow control */
	case RMNET_IOCTL_GET_DFLT_CONTROL_CHANNEL:	/* For legacy use */
	case RMNET_IOCTL_GET_HWSW_MAP:			/* Get HW/SW map */
	case RMNET_IOCTL_SET_RX_HEADROOM:		/* Set RX Headroom */
		return 0;

	default:
		ipa_err("[%s] unsupported extended cmd[%d]",
			dev->name, edata.extended_ioctl);
		return -EINVAL;
	}
}

/** ipa_wwan_ioctl() - I/O control for wwan network driver.
 *
 * @dev: network device
 * @ifr: ignored
 * @cmd: cmd to be excecuded. can be one of the following:
 * IPA_WWAN_IOCTL_OPEN - Open the network interface
 * IPA_WWAN_IOCTL_CLOSE - Close the network interface
 *
 * Return codes:
 * 0: success
 * NETDEV_TX_BUSY: Error while transmitting the skb. Try again
 * later
 * -EFAULT: Error while transmitting the skb
 */
static int ipa_wwan_ioctl(struct net_device *dev, struct ifreq *ifr, int cmd)
{
	struct rmnet_ioctl_data_s ioctl_data = { };
	void __user *data = ifr->ifr_ifru.ifru_data;
	size_t size = sizeof(ioctl_data);

	ipa_debug("rmnet_ipa got ioctl number 0x%08x", cmd);
	switch (cmd) {
	/* These features are implied; alternatives are not supported */
	case RMNET_IOCTL_SET_LLP_IP:		/* RAW IP protocol */
	case RMNET_IOCTL_SET_QOS_DISABLE:	/* QoS header disabled */
		return 0;

	/* These features are not supported; use alternatives */
	case RMNET_IOCTL_SET_LLP_ETHERNET:	/* Ethernet protocol */
	case RMNET_IOCTL_SET_QOS_ENABLE:	/* QoS header enabled */
	case RMNET_IOCTL_GET_OPMODE:		/* Get operation mode */
	case RMNET_IOCTL_FLOW_ENABLE:		/* Flow enable */
	case RMNET_IOCTL_FLOW_DISABLE:		/* Flow disable */
	case RMNET_IOCTL_FLOW_SET_HNDL:		/* Set flow handle */
		return -ENOTSUPP;

	case RMNET_IOCTL_GET_LLP:		/* Get link protocol */
		ioctl_data.u.operation_mode = RMNET_MODE_LLP_IP;

		return copy_to_user(data, &ioctl_data, size) ? -EFAULT : 0;

	case RMNET_IOCTL_GET_QOS:		/* Get QoS header state */
		ioctl_data.u.operation_mode = RMNET_MODE_NONE;

		return copy_to_user(data, &ioctl_data, size) ? -EFAULT : 0;

	case RMNET_IOCTL_OPEN:			/* Open transport port */
	case RMNET_IOCTL_CLOSE:			/* Close transport port */
		return 0;

	case RMNET_IOCTL_EXTENDED:		/* Extended IOCTLs */
		return ipa_wwan_ioctl_extended(dev, data);

	default:
		ipa_err("[%s] unsupported cmd[%d]", dev->name, cmd);

		return -EINVAL;
	}
}

static const struct net_device_ops ipa_wwan_ops_ip = {
	.ndo_open	= ipa_wwan_open,
	.ndo_stop	= ipa_wwan_stop,
	.ndo_start_xmit	= ipa_wwan_xmit,
	.ndo_do_ioctl	= ipa_wwan_ioctl,
};

/** wwan_setup() - Setups the wwan network driver.
 *
 * @dev: network device
 *
 * Return codes:
 * None
 */
static void ipa_wwan_setup(struct net_device *dev)
{
	dev->netdev_ops = &ipa_wwan_ops_ip;
	ether_setup(dev);
	dev->header_ops = NULL;	 /* No header (override ether_setup() value) */
	dev->type = ARPHRD_RAWIP;
	dev->hard_header_len = 0;
	dev->max_mtu = WWAN_DATA_LEN;
	dev->mtu = dev->max_mtu;
	dev->addr_len = 0;
	dev->flags &= ~(IFF_BROADCAST | IFF_MULTICAST);
	dev->needed_headroom = HEADROOM_FOR_QMAP;
	dev->needed_tailroom = TAILROOM;
	dev->watchdog_timeo = msecs_to_jiffies(10 * MSEC_PER_SEC);
}

static void ipa_wake_tx_queue(struct work_struct *work)
{
	if (rmnet_ipa_ctx->dev) {
		struct netdev_queue *queue;

		queue = netdev_get_tx_queue(rmnet_ipa_ctx->dev, 0);
		__netif_tx_lock_bh(queue);
		netif_wake_queue(rmnet_ipa_ctx->dev);
		__netif_tx_unlock_bh(queue);
	}
}

/** ipa_wwan_probe() - Initialized the module and registers as a
 * network interface to the network stack
 *
 * Note: In case IPA driver hasn't initialized already, the probe function
 * will return immediately after registering a callback to be invoked when
 * IPA driver initialization is complete.
 *
 * Return codes:
 * 0: success
 * -ENOMEM: No memory available
 * -EFAULT: Internal error
 */
static int ipa_wwan_probe(struct platform_device *pdev)
{
	int ret;
	struct net_device *dev;
	struct ipa_wwan_private *wwan_ptr;


	mutex_init(&rmnet_ipa_ctx->pipe_handle_guard);
	mutex_init(&rmnet_ipa_ctx->add_mux_channel_lock);

	/* Mark client handles bad until we initialize them */
	rmnet_ipa_ctx->apps_to_ipa_hdl = IPA_CLNT_HDL_BAD;
	rmnet_ipa_ctx->ipa_to_apps_hdl = IPA_CLNT_HDL_BAD;

	ret = ipa_init_q6_smem();
	if (ret) {
		ipa_err("ipa_init_q6_smem failed!\n");
		goto err_clear_ctx;
	}

	/* start A7 QMI service/client */
	ipa_qmi_init();

	/* initialize wan-driver netdev */
	dev = alloc_netdev(sizeof(struct ipa_wwan_private),
			   IPA_WWAN_DEV_NAME,
			   NET_NAME_UNKNOWN,
			   ipa_wwan_setup);
	if (!dev) {
		ipa_err("no memory for netdev\n");
		ret = -ENOMEM;
		goto err_clear_ctx;
	}
	rmnet_ipa_ctx->dev = dev;
	wwan_ptr = netdev_priv(dev);
	ipa_debug("wwan_ptr (private) = %p", wwan_ptr);
	wwan_ptr->outstanding_high_ctl = DEFAULT_OUTSTANDING_HIGH_CTL;
	wwan_ptr->outstanding_high = DEFAULT_OUTSTANDING_HIGH;
	wwan_ptr->outstanding_low = DEFAULT_OUTSTANDING_LOW;
	atomic_set(&wwan_ptr->outstanding_pkts, 0);

	/* Enable SG support in netdevice. */
	dev->hw_features |= NETIF_F_SG;

	netif_napi_add(dev, &wwan_ptr->napi, ipa_rmnet_poll, NAPI_WEIGHT);
	ret = register_netdev(dev);
	if (ret) {
		ipa_err("unable to register ipa_netdev %d rc=%d\n", 0, ret);
		goto err_napi_del;
	}

	ipa_debug("IPA-WWAN devices (%s) initialization ok :>>>>\n", dev->name);
	/* offline charging mode */
	ipa_proxy_clk_unvote();

	/* Till the system is suspended, we keep the clock open */
	ipa_client_add();

	initialized = true;

	return 0;

err_napi_del:
	netif_napi_del(&wwan_ptr->napi);
	free_netdev(dev);
err_clear_ctx:
	memset(&rmnet_ipa_ctx_struct, 0, sizeof(rmnet_ipa_ctx_struct));

	return ret;
}

static int ipa_wwan_remove(struct platform_device *pdev)
{
	struct ipa_wwan_private *wwan_ptr = netdev_priv(rmnet_ipa_ctx->dev);

	ipa_info("rmnet_ipa started deinitialization\n");
	mutex_lock(&rmnet_ipa_ctx->pipe_handle_guard);
	if (rmnet_ipa_ctx->ipa_to_apps_hdl != IPA_CLNT_HDL_BAD) {
		ipa_teardown_sys_pipe(rmnet_ipa_ctx->ipa_to_apps_hdl);
		rmnet_ipa_ctx->ipa_to_apps_hdl = IPA_CLNT_HDL_BAD;
	}

	if (rmnet_ipa_ctx->apps_to_ipa_hdl != IPA_CLNT_HDL_BAD) {
		ipa_teardown_sys_pipe(rmnet_ipa_ctx->apps_to_ipa_hdl);
		rmnet_ipa_ctx->apps_to_ipa_hdl = IPA_CLNT_HDL_BAD;
	}

	netif_napi_del(&wwan_ptr->napi);
	mutex_unlock(&rmnet_ipa_ctx->pipe_handle_guard);
	unregister_netdev(rmnet_ipa_ctx->dev);

	cancel_work_sync(&ipa_tx_wakequeue_work);
	if (rmnet_ipa_ctx->dev)
		free_netdev(rmnet_ipa_ctx->dev);
	rmnet_ipa_ctx->dev = NULL;

	mutex_destroy(&rmnet_ipa_ctx->add_mux_channel_lock);
	mutex_destroy(&rmnet_ipa_ctx->pipe_handle_guard);

	initialized = false;
	ipa_info("rmnet_ipa completed deinitialization\n");

	return 0;
}

/** rmnet_ipa_ap_suspend() - suspend callback for runtime_pm
 * @dev: pointer to device
 *
 * This callback will be invoked by the runtime_pm framework when an AP suspend
 * operation is invoked, usually by pressing a suspend button.
 *
 * Returns -EAGAIN to runtime_pm framework in case there are pending packets
 * in the Tx queue. This will postpone the suspend operation until all the
 * pending packets will be transmitted.
 *
 * In case there are no packets to send, releases the WWAN0_PROD entity.
 * As an outcome, the number of IPA active clients should be decremented
 * until IPA clocks can be gated.
 */
static int rmnet_ipa_ap_suspend(struct device *dev)
{
	struct net_device *netdev = rmnet_ipa_ctx->dev;
	struct ipa_wwan_private *wwan_ptr;
	int ret;

	ipa_debug("Enter...\n");
	if (!netdev) {
		ipa_err("netdev is NULL.\n");
		ret = 0;
		goto bail;
	}

	netif_tx_lock_bh(netdev);
	wwan_ptr = netdev_priv(netdev);
	if (!wwan_ptr) {
		ipa_err("wwan_ptr is NULL.\n");
		ret = 0;
		goto unlock_and_bail;
	}

	/* Do not allow A7 to suspend in case there are outstanding packets */
	if (atomic_read(&wwan_ptr->outstanding_pkts) != 0) {
		ipa_debug("Outstanding packets, postponing AP suspend.\n");
		ret = -EAGAIN;
		goto unlock_and_bail;
	}

	/* Make sure that there is no Tx operation ongoing */
	netif_stop_queue(netdev);

	ret = 0;
	ipa_client_remove();
	ipa_debug("IPA clocks disabled\n");

unlock_and_bail:
	netif_tx_unlock_bh(netdev);
bail:
	ipa_debug("Exit with %d\n", ret);

	return ret;
}

/** rmnet_ipa_ap_resume() - resume callback for runtime_pm
 * @dev: pointer to device
 *
 * This callback will be invoked by the runtime_pm framework when an AP resume
 * operation is invoked.
 *
 * Enables the network interface queue and returns success to the
 * runtime_pm framework.
 */
static int rmnet_ipa_ap_resume(struct device *dev)
{
	struct net_device *netdev = rmnet_ipa_ctx->dev;

	ipa_client_add();
	ipa_debug("IPA clocks enabled\n");
	if (netdev)
		netif_wake_queue(netdev);
	ipa_debug("Exit\n");

	return 0;
}

static const struct of_device_id rmnet_ipa_dt_match[] = {
	{.compatible = "qcom,rmnet-ipa"},
	{},
};
MODULE_DEVICE_TABLE(of, rmnet_ipa_dt_match);

static const struct dev_pm_ops rmnet_ipa_pm_ops = {
	.suspend_noirq = rmnet_ipa_ap_suspend,
	.resume_noirq = rmnet_ipa_ap_resume,
};

static struct platform_driver rmnet_ipa_driver = {
	.driver = {
		.name = "rmnet_ipa",
		.owner = THIS_MODULE,
		.pm = &rmnet_ipa_pm_ops,
		.of_match_table = rmnet_ipa_dt_match,
	},
	.probe = ipa_wwan_probe,
	.remove = ipa_wwan_remove,
};

/** ipa_q6_handshake_complete() - Perform operations once Q6 is up
 * @ssr_bootup - Indicates whether this is a cold boot-up or post-SSR.
 *
 * This function is invoked once the handshake between the IPA AP driver
 * and IPA Q6 driver is complete. At this point, it is possible to perform
 * operations which can't be performed until IPA Q6 driver is up.
 *
 */
void ipa_q6_handshake_complete(bool ssr_bootup)
{
	/* It is required to recover the network stats after SSR recovery */
	if (ssr_bootup) {
		/* In case the uC is required to be loaded by the Modem,
		 * the proxy vote will be removed only when uC loading is
		 * complete and indication is received by the AP. After SSR,
		 * uC is already loaded. Therefore, proxy vote can be removed
		 * once Modem init is complete.
		 */
		ipa_proxy_clk_unvote();
	}
}

int ipa_wwan_init(void)
{
	if (initialized)
		return 0;

	return platform_driver_register(&rmnet_ipa_driver);
}

void ipa_wwan_cleanup(void)
{
	platform_driver_unregister(&rmnet_ipa_driver);
	memset(&rmnet_ipa_ctx_struct, 0, sizeof(rmnet_ipa_ctx_struct));
}

static int ipa_rmnet_poll(struct napi_struct *napi, int budget)
{
	int rcvd_pkts;

	rcvd_pkts = ipa_rx_poll(rmnet_ipa_ctx->ipa_to_apps_hdl, budget);
	ipa_debug_low("rcvd packets: %d\n", rcvd_pkts);

	return rcvd_pkts;
}

MODULE_DESCRIPTION("WWAN Network Interface");
MODULE_LICENSE("GPL v2");
