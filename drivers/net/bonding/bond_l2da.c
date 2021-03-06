/* Copyright (c) 2016, The Linux Foundation. All rights reserved.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/etherdevice.h>
#include <linux/if_bonding_genl.h>
#include "bonding.h"
#include "bond_l2da.h"

#define L2DA_OPTS_DEFAULT 0

struct l2da_bond_matrix_entry {
	struct hlist_node hnode;
	unsigned char da[ETH_ALEN];
	struct slave *slave;
};

#define BOND_L2DA_INFO(bond) ((bond)->l2da_info)

#define SLAVE_CAN_XMIT(slave) (IS_UP((slave)->dev) && \
				((slave)->link == BOND_LINK_UP) && \
				bond_is_active_slave(slave))

/**
 * _bond_l2da_slave_name - returns slave name
 * @slave: slave struct to work on
 *
 * Returns @slave network device name, or "null" if it can't be found.
 */
static inline const char *_bond_l2da_slave_name(struct slave *slave)
{
	if (slave && slave->dev)
		return netdev_name(slave->dev);
	return "null";
}

/**
 * _bond_l2da_hash_val - hash function for L2DA map hash table
 * @da: DA to be used as a hash key
 *
 * Returns hash value for @da
 */
static inline u32 _bond_l2da_hash_val(const unsigned char *da)
{
	return da[ETH_ALEN - 2];
}

/**
 * _bond_l2da_find_entry_unsafe - searches for DA:iface mapping within the map
 * @bond_info: L2DA bonding struct to work on
 * @da: DA to be used as a key
 *
 * Returns map entry for @da, or %NULL if it can't be found.
 *
 * The function must be called under the L2DA bonding struct lock.
 */
static struct l2da_bond_matrix_entry *
_bond_l2da_find_entry_unsafe(struct l2da_bond_info *bond_info,
			     const unsigned char *da)
{
	struct l2da_bond_matrix_entry *entry = NULL;
	u32 hash_val;
	BUG_ON(da == NULL);

	hash_val = _bond_l2da_hash_val(da);
	hash_for_each_possible(bond_info->da_matrix, entry, hnode, hash_val)
		if (ether_addr_equal(entry->da, da))
			return entry;
	return NULL;
}

/**
 * _bond_l2da_select_default_slave_unsafe - selects default slave
 * @bond: bonding struct to work on
 *
 * The function must be called under the L2DA bonding struct lock.
 */
static void
_bond_l2da_select_default_slave_unsafe(struct bonding *bond)
{
	struct l2da_bond_info *bond_info = &BOND_L2DA_INFO(bond);
	struct slave *slave;
	struct list_head *iter;

	/* Default slave is OK, so continue to use it */
	if (bond_info->default_slave &&
	    SLAVE_CAN_XMIT(bond_info->default_slave))
		return;

	/* Select new default slave */
	bond_for_each_slave(bond, slave, iter) {
		if (slave != bond_info->default_slave &&
		    SLAVE_CAN_XMIT(slave)) {
			pr_info("bond_l2da default slave set to %s\n",
				_bond_l2da_slave_name(slave));
			bond_info->default_slave = slave;
			break;
		}
	}
}

/**
 * _bond_l2da_remove_entries_unsafe - removes all iface mappings from the map
 * @bond_info: L2DA bonding struct to work on
 * @slave: slave whose mappings have to be removed
 *
 * The function must be called under the L2DA bonding struct lock.
 */
static void _bond_l2da_remove_entries_unsafe(struct l2da_bond_info *bond_info,
					     struct slave *slave)
{
	struct l2da_bond_matrix_entry *entry = NULL;
	struct hlist_node *tmp;
	int counter;
	hash_for_each_safe(bond_info->da_matrix, counter, tmp, entry, hnode) {
		/* NULL slave means "remove all" */
		if (!slave || entry->slave == slave) {
			hash_del(&entry->hnode);
			bond_notify_l2da(entry->da);
			kfree(entry);
		}
	}
}

/**
 * _bond_l2da_bridge_clone_and_xmit - clones received packet and sends it to
 *      specific slave
 * @bond: bond struct to work on
 * @slave: slave to send cloned skb to
 * @skb: received packet to be cloned and sent
 *
 * Returns: %true if forwarding succeeded, %false - otherwise
 */
static bool _bond_l2da_bridge_clone_and_xmit(struct bonding *bond,
					     struct slave *slave,
					     struct sk_buff *skb)
{
	struct sk_buff *skb2;

	if (!SLAVE_CAN_XMIT(slave))
		return false;

	skb2 = skb_clone(skb, GFP_ATOMIC);
	if (!skb2) {
		pr_err_ratelimited("%s: Error: _bond_l2da_bridge_clone_and_xmit(): skb_clone() failed\n",
				   bond->dev->name);
		return false;
	}

	skb2->protocol = htons(ETH_P_802_3);
	skb_forward_csum(skb2);
	skb_push(skb2, ETH_HLEN);
	/* bond_dev_queue_xmit always returns 0 */
	bond_dev_queue_xmit(bond, skb2, slave->dev);
	return true;
}


/**
 * _bond_l2da_bridge_flood_multicast - implements L2DA packet forwarding
 *      functionality for multicast packets. See %BOND_L2DA_OPT_FORWARD_RX for
 *      more info.
 * @bond: bond struct to work on
 * @slave: slave that received the packet
 * @skb: received multicast packet
 *
 * Returns: always %true for consistency with other _bond_l2da_bridge_flood...
 *      functions.
 */
static bool _bond_l2da_bridge_flood_multicast(struct bonding *bond,
					      struct slave *slave,
					      struct sk_buff *skb)
{
	struct list_head *iter;
	struct slave *s;

	rcu_read_lock_bh();
	bond_for_each_slave_rcu(bond, s, iter) {
		if (s == slave)
			continue;

		if (_bond_l2da_bridge_clone_and_xmit(bond, s, skb))
			pr_debug("bond_l2da: bridge: MC (SA=%pM DA=%pM) %s => %s\n",
				 eth_hdr(skb)->h_source,
				 eth_hdr(skb)->h_dest,
				 _bond_l2da_slave_name(slave),
				 _bond_l2da_slave_name(s));
	}
	rcu_read_unlock_bh();
	/* Multicast packets should also be delivered locally */
	return true;
}

/**
 * _bond_l2da_bridge_flood_unicast - implements L2DA packet forwarding
 *      functionality for unicast packets. See %BOND_L2DA_OPT_FORWARD_RX for
 *      more info.
 * @bond: bond struct to work on
 * @slave: slave that received the packet
 * @skb: received unicast packet
 *
 * Returns: %true if skb should be delivered locally, %false - otherwise
 */
static bool _bond_l2da_bridge_flood_unicast(struct bonding *bond,
					    struct slave *slave,
					    struct sk_buff *skb)
{
	struct l2da_bond_info *bond_info = &BOND_L2DA_INFO(bond);
	struct l2da_bond_matrix_entry *entry;
	struct ethhdr *eth_data;
	bool res = true;

	eth_data = eth_hdr(skb);

	read_lock_bh(&bond_info->lock);
	/* There is no flood actually. Packet is sent to only single slave (if
	 * found in map).
	 */
	entry = _bond_l2da_find_entry_unsafe(bond_info, eth_data->h_dest);
	if (entry && entry->slave && entry->slave != slave) {
		/* We should not forward packet if entry->slave == slave
		 * (i.e. back to the slave it arrived from), as this scenario is
		 * supposed to be handled by underlying slave driver.
		 */
		if (_bond_l2da_bridge_clone_and_xmit(bond, entry->slave, skb)) {
			pr_debug("bond_l2da: bridge: UC (SA=%pM DA=%pM) %s => %s\n",
				 eth_hdr(skb)->h_source,
				 eth_hdr(skb)->h_dest,
				 _bond_l2da_slave_name(slave),
				 _bond_l2da_slave_name(entry->slave));
			res = false;
		}
	}
	read_unlock_bh(&bond_info->lock);

	return res;
}

/**
 * _bond_l2da_bridge_flood - implements L2DA packet forwarding functionality
 * @bond: bond struct to work on
 * @slave: slave that received the packet
 * @skb: received packet
 *
 * Returns: %true if skb should be delivered locally, %false - otherwise
 */
static bool _bond_l2da_bridge_flood(struct bonding *bond,
				    struct slave *slave,
				    struct sk_buff *skb)
{
	struct ethhdr *eth_data;

	if (unlikely(skb->pkt_type == PACKET_LOOPBACK))
		return true;

	eth_data = eth_hdr(skb);

	return is_multicast_ether_addr(eth_data->h_dest) ?
			_bond_l2da_bridge_flood_multicast(bond, slave, skb) :
			_bond_l2da_bridge_flood_unicast(bond, slave, skb);
}

/**
 * bond_l2da_initialize - initializes a bond's L2DA context
 * @bond: bonding struct to work on
 */
int bond_l2da_initialize(struct bonding *bond)
{
	struct l2da_bond_info *bond_info = &BOND_L2DA_INFO(bond);

	memset(bond_info, 0, sizeof(*bond_info));
	hash_init(bond_info->da_matrix);
	rwlock_init(&bond_info->lock);
	bond_info->default_slave = NULL;
	atomic_set(&bond_info->opts, L2DA_OPTS_DEFAULT);
	pr_info("bond_l2da initialized\n");
	return 0;
}

/**
 * bond_l2da_deinitialize - deinitializes a bond's L2DA context
 * @bond: bonding struct to work on
 */
void bond_l2da_deinitialize(struct bonding *bond)
{
	struct l2da_bond_info *bond_info = &BOND_L2DA_INFO(bond);

	bond_l2da_purge(bond);
	BUG_ON(!hash_empty(bond_info->da_matrix));
	memset(bond_info, 0, sizeof(*bond_info)); /* for debugging purposes */
	pr_info("bond_l2da de-initialized\n");
}

/**
 * bond_l2da_bind_slave - bind slave to L2DA
 * @bond: bonding struct to work on
 * @slave: slave struct to work on
 *
 * Assigns default slave (if needed).
 */
int bond_l2da_bind_slave(struct bonding *bond, struct slave *slave)
{
	struct l2da_bond_info *bond_info = &BOND_L2DA_INFO(bond);
	write_lock_bh(&bond_info->lock);
	if (!bond_info->default_slave) {
		bond_info->default_slave = slave;
		pr_info("bond_l2da default slave initially set to %s\n",
			_bond_l2da_slave_name(slave));
	}
	_bond_l2da_select_default_slave_unsafe(bond);
	write_unlock_bh(&bond_info->lock);
	return 0;
}

/**
 * bond_l2da_unbind_slave - unbind slave from L2DA
 * @slave: slave struct to work on
 *
 * Removes all matrix entries for this slave, re-assigns default slave (if
 * needed).
 */
void bond_l2da_unbind_slave(struct bonding *bond, struct slave *slave)
{
	struct l2da_bond_info *bond_info = &BOND_L2DA_INFO(bond);

	write_lock_bh(&bond_info->lock);
	if (slave == bond_info->default_slave) {
		/* default slave has gone, so let's use some other slave as
		* a new default
		*/
		bond_info->default_slave = bond_first_slave(bond);
		pr_info("bond_l2da default slave set to %s\n",
			_bond_l2da_slave_name(bond_info->default_slave));
	}
	_bond_l2da_remove_entries_unsafe(bond_info, slave);
	_bond_l2da_select_default_slave_unsafe(bond);
	write_unlock_bh(&bond_info->lock);
}

/**
 * bond_l2da_get_tx_dev - Calculate egress interface for a given packet,
			  for a LAG that is configured in L2DA mode
 * @dst_mac: pointer to destination L2 address
 * @bond_dev: pointer to bond master device

 * Returns: Either valid slave device, or NULL otherwise
 */
struct net_device *bond_l2da_get_tx_dev(uint8_t *dest_mac,
					struct net_device *bond_dev)
{
	struct bonding *bond = netdev_priv(bond_dev);
	struct l2da_bond_info *bond_info = &BOND_L2DA_INFO(bond);
	struct l2da_bond_matrix_entry *entry;
	struct net_device *dest_dev = NULL;
	u32 opts = atomic_read(&bond_info->opts);

	if ((opts & BOND_L2DA_OPT_DUP_MC_TX) &&
	    (is_multicast_ether_addr(dest_mac)))
		return NULL;

	read_lock_bh(&bond_info->lock);
	entry = _bond_l2da_find_entry_unsafe(bond_info, dest_mac);
	if (entry && entry->slave && SLAVE_CAN_XMIT(entry->slave)) {
		/* if a slave configured for this DA and it's OK - use it */
		dest_dev = entry->slave->dev;
	} else if (bond_info->default_slave &&
		   SLAVE_CAN_XMIT(bond_info->default_slave)) {
		/* otherwise, if default slave is configured - use it */
		dest_dev = bond_info->default_slave->dev;
	}
	read_unlock_bh(&bond_info->lock);

	return dest_dev;
}

/**
 * bond_l2da_xmit - transmits skb in L2DA mode
 * @skb: skb to transmit
 * @dev: bonding net device
 */
int bond_l2da_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct bonding *bond = netdev_priv(dev);
	struct l2da_bond_info *bond_info = &BOND_L2DA_INFO(bond);
	struct ethhdr *eth_data;
	struct l2da_bond_matrix_entry *entry;
	int no_slave = 0;
	u32 opts = atomic_read(&bond_info->opts);

	skb_reset_mac_header(skb);
	eth_data = eth_hdr(skb);

	if ((opts & BOND_L2DA_OPT_DUP_MC_TX) &&
	    is_multicast_ether_addr(eth_data->h_dest))
		return bond_xmit_all_slaves(bond, skb);

	read_lock_bh(&bond_info->lock);
	entry = _bond_l2da_find_entry_unsafe(bond_info, eth_data->h_dest);
	if (entry && entry->slave && SLAVE_CAN_XMIT(entry->slave)) {
		/* if a slave configured for this DA and it's OK - use it */
		bond_dev_queue_xmit(bond, skb, entry->slave->dev);
	} else if (bond_info->default_slave &&
		   SLAVE_CAN_XMIT(bond_info->default_slave)) {
		/* otherwise, if default slave is configured - use it */
		bond_dev_queue_xmit(bond, skb, bond_info->default_slave->dev);
	} else {
		no_slave = 1;
	}
	read_unlock_bh(&bond_info->lock);

	if (no_slave) {
		/* no suitable interface, frame not sent */
		dev_kfree_skb_any(skb);
	}

	return NETDEV_TX_OK;
}

/**
 * bond_l2da_handle_rx_frame - handles RX packets on L2DA mode
 * @bond: bonding struct to work on
 * @slave: slave that received the %skb
 * @skb: received skb
 * Returns: %true for allowing this %skb to be delivered to local stack, %false
 *          for dropping it
 */
bool bond_l2da_handle_rx_frame(struct bonding *bond, struct slave *slave,
			       struct sk_buff *skb)
{
	struct l2da_bond_info *bond_info = &BOND_L2DA_INFO(bond);
	struct ethhdr *eth_data;
	struct l2da_bond_matrix_entry *entry;
	bool res = true;
	u32 opts = atomic_read(&bond_info->opts);

	eth_data = eth_hdr(skb);

	/* if DEDUP disabled, all RX apackets are allowed */
	/* if DEDUP enabled, EAPOLs are allowed from all the slaves */
	if ((opts & BOND_L2DA_OPT_DEDUP_RX) &&
	    eth_data->h_proto != cpu_to_be16(ETH_P_PAE)) {
		/* if DEDUP enabled, non-EAPOL packets are allowed:
		 * - if there's a slave configured for this SA - only from it
		 * - else if default slave configured - only from it
		 * - else - from any slave
		 */
		read_lock_bh(&bond_info->lock);
		entry = _bond_l2da_find_entry_unsafe(bond_info,
						     eth_data->h_source);
		if (entry && entry->slave)
			res = (entry->slave->dev == skb->dev);
		else if (bond_info->default_slave)
			res = (bond_info->default_slave->dev == skb->dev);
		read_unlock_bh(&bond_info->lock);
	}

	/* DEDUP takes precedence over FORWARD, i.e. we only flood packets which
	 * are allowed for RX
	 */
	if (res && (opts & BOND_L2DA_OPT_FORWARD_RX))
		return _bond_l2da_bridge_flood(bond, slave, skb);

	return res;
}

/**
 * bond_l2da_set_default_slave - sets default slave
 * @bond: bonding struct to work on
 * @slave: slave struct to set default
 *
 * Returns: 0 on success, negative error code on failure
 */
int bond_l2da_set_default_slave(struct bonding *bond, struct slave *slave)
{
	struct l2da_bond_info *bond_info = &BOND_L2DA_INFO(bond);
	int res = -EINVAL;

	write_lock_bh(&bond_info->lock);
	if (SLAVE_CAN_XMIT(slave)) {
		bond_info->default_slave = slave;
		pr_info("bond_l2da default slave set to %s\n",
			_bond_l2da_slave_name(slave));
		res = 0;
	} else {
		_bond_l2da_select_default_slave_unsafe(bond);
	}
	write_unlock_bh(&bond_info->lock);
	return res;
}

/**
 * bond_l2da_get_default_slave_name - gets name of currently configured default
 * slave
 * @bond: bonding struct to work on
 * @buf: destination buffer
 * @size: destination buffer size
 */
int bond_l2da_get_default_slave_name(struct bonding *bond, char *buf, int size)
{
	struct l2da_bond_info *bond_info = &BOND_L2DA_INFO(bond);

	if (!buf || size < IFNAMSIZ)
		return -EINVAL;

	*buf = 0;

	read_lock_bh(&bond_info->lock);
	if (bond_info->default_slave) {
		strncpy(buf, netdev_name(bond_info->default_slave->dev),
			IFNAMSIZ);
		buf[IFNAMSIZ - 1] = 0;
	}
	read_unlock_bh(&bond_info->lock);
	return 0;
}

/**
 * bond_l2da_set_da_slave - adds DA:slave mapping
 * @bond: bonding struct to work on
 * @da: desired L2 destination address to map
 * @slave: slave to be used for sending packets to desired destination address
 */
int bond_l2da_set_da_slave(struct bonding *bond, const unsigned char *da,
			   struct slave *slave)
{
	struct l2da_bond_info *bond_info = &BOND_L2DA_INFO(bond);
	struct l2da_bond_matrix_entry *entry;
	struct slave *prev_slave = NULL;

	write_lock_bh(&bond_info->lock);
	entry = _bond_l2da_find_entry_unsafe(bond_info, da);
	if (entry) {
		prev_slave = entry->slave;
		entry->slave = slave;
	} else {
		entry = kmalloc(sizeof(*entry), GFP_ATOMIC);
		if (!entry) {
			pr_err("bond_l2da: pair node cannot be allocated for [%pM:%s]\n",
			       da, _bond_l2da_slave_name(slave));
			write_unlock_bh(&bond_info->lock);
			return -ENOMEM;
		}
		entry->slave = slave;
		ether_addr_copy(entry->da, da);
		hash_add(bond_info->da_matrix, &entry->hnode,
			 _bond_l2da_hash_val(da));
	}
	write_unlock_bh(&bond_info->lock);

	pr_info("bond_l2da: pair %s [%pM:%s]\n",
		prev_slave ? "changed" : "added",
		da, _bond_l2da_slave_name(slave));

	return 0;
}

/**
 * bond_l2da_set_opts - sets L2DA options
 * @bond: bonding struct to work on
 * @opts: options mask, see enum bond_genl_l2da_opts for details
 */
void bond_l2da_set_opts(struct bonding *bond, u32 opts)
{
	struct l2da_bond_info *bond_info = &BOND_L2DA_INFO(bond);
	u32 old_opts = atomic_xchg(&bond_info->opts, opts);

	if (old_opts != opts)
		pr_info("bond_l2da: opts changed 0x%08x => 0x%08x\n",
			old_opts, opts);
}

/**
 * bond_l2da_get_opts - gets L2DA options
 * @bond: bonding struct to work on
 *
 * Returns: L2DA options value, see enum bond_genl_l2da_opts for details
 */
u32 bond_l2da_get_opts(struct bonding *bond)
{
	struct l2da_bond_info *bond_info = &BOND_L2DA_INFO(bond);

	return atomic_read(&bond_info->opts);
}

/**
 * bond_l2da_del_da - removes DA mapping
 * @bond: bonding struct to work on
 * @da: L2 destination address whose mapping has to be removed
 */
int bond_l2da_del_da(struct bonding *bond, const unsigned char *da)
{
	struct l2da_bond_info *bond_info = &BOND_L2DA_INFO(bond);
	struct l2da_bond_matrix_entry *entry;

	write_lock_bh(&bond_info->lock);
	entry = _bond_l2da_find_entry_unsafe(bond_info, da);
	if (entry)
		hash_del(&entry->hnode);
	write_unlock_bh(&bond_info->lock);

	if (!entry) {
		pr_err("bond_l2da: pair node cannot be found for %pM\n", da);
		return -ENOENT;
	}

	pr_info("bond_l2da: pair deleted [%pM:%s]\n",
		da, _bond_l2da_slave_name(entry->slave));
	kfree(entry);
	return 0;
}

/**
 * bond_l2da_purge - removes all DA mappings
 * @bond: bonding struct to work on
 */
void bond_l2da_purge(struct bonding *bond)
{
	struct l2da_bond_info *bond_info = &BOND_L2DA_INFO(bond);
	write_lock_bh(&bond_info->lock);
	_bond_l2da_remove_entries_unsafe(bond_info, NULL);
	write_unlock_bh(&bond_info->lock);
}

/**
 * bond_l2da_handle_link_change - handle a slave's link status change indication
 * @bond: bonding struct to work on
 * @slave: slave struct whose link status changed
 *
 * Handle re-selection of default slave (if needed).
 */
void bond_l2da_handle_link_change(struct bonding *bond, struct slave *slave)
{
	struct l2da_bond_info *bond_info = &BOND_L2DA_INFO(bond);
	struct slave *prev_default_slave;

	write_lock_bh(&bond_info->lock);
	prev_default_slave = bond_info->default_slave;
	_bond_l2da_select_default_slave_unsafe(bond);

	spin_lock_bh(&bond_cb_lock);
	if (prev_default_slave && bond_cb && bond_cb->bond_cb_delete_by_slave)
		bond_cb->bond_cb_delete_by_slave(prev_default_slave->dev);
	spin_unlock_bh(&bond_cb_lock);
	write_unlock_bh(&bond_info->lock);
}

/**
 * bond_l2da_call_foreach - iterates over L2DA map
 * @bond: bonding struct to work on
 * @clb: callback function to be called for every mapping entry found
 * @ctx: user context to be passed to callback
 *
 * Callback function can return non-zero value to stop iteration.
 */
void bond_l2da_call_foreach(struct bonding *bond,
			    int (*clb)(const unsigned char *da,
				       struct slave *slave,
				       void *ctx),
			    void *ctx)
{
	struct l2da_bond_info *bond_info = &BOND_L2DA_INFO(bond);
	struct l2da_bond_matrix_entry *entry;
	int bkt;

	BUG_ON(!clb);

	read_lock_bh(&bond_info->lock);
	hash_for_each(bond_info->da_matrix, bkt, entry, hnode) {
		if (clb(entry->da, entry->slave, ctx))
			break;
	}
	read_unlock_bh(&bond_info->lock);
}
