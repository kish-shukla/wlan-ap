/* SPDX-License-Identifier: BSD-3-Clause */

#include "netifd.h"

#include <uci.h>
#include <libubox/blobmsg.h>

#include "uci.h"
#include "utils.h"

enum {
	NET_ATTR_TYPE,
	NET_ATTR_PROTO,
	NET_ATTR_IPADDR,
	NET_ATTR_NETMASK,
	NET_ATTR_GATEWAY,
	NET_ATTR_DNS,
	NET_ATTR_MTU,
	NET_ATTR_DISABLED,
	NET_ATTR_IP6ASSIGN,
	NET_ATTR_IP4TABLE,
	NET_ATTR_IP6TABLE,
	NET_ATTR_IFNAME,
	NET_ATTR_VID,
	NET_ATTR_VLAN_FILTERING,
	__NET_ATTR_MAX,
};

static const struct blobmsg_policy network_policy[__NET_ATTR_MAX] = {
	[NET_ATTR_TYPE] = { .name = "type", .type = BLOBMSG_TYPE_STRING },
	[NET_ATTR_PROTO] = { .name = "proto", .type = BLOBMSG_TYPE_STRING },
	[NET_ATTR_IPADDR] = { .name = "ipaddr", .type = BLOBMSG_TYPE_STRING },
	[NET_ATTR_NETMASK] = { .name = "netmask", .type = BLOBMSG_TYPE_STRING },
	[NET_ATTR_GATEWAY] = { .name = "gateway", .type = BLOBMSG_TYPE_STRING },
	[NET_ATTR_DNS] = { .name = "dns", .type = BLOBMSG_TYPE_STRING },
	[NET_ATTR_MTU] = { .name = "mtu", .type = BLOBMSG_TYPE_INT32 },
	[NET_ATTR_DISABLED] = { .name = "disabled", .type = BLOBMSG_TYPE_BOOL },
	[NET_ATTR_IP6ASSIGN] = { .name = "ip6assign", .type = BLOBMSG_TYPE_INT32 },
	[NET_ATTR_IP4TABLE] = { .name = "ip4table", .type = BLOBMSG_TYPE_INT32 },
	[NET_ATTR_IP6TABLE] = { .name = "ip6table", .type = BLOBMSG_TYPE_INT32 },
	[NET_ATTR_IFNAME] = { .name = "ifname", .type = BLOBMSG_TYPE_STRING },
	[NET_ATTR_VID] = { .name = "vid", .type = BLOBMSG_TYPE_INT32 },
	[NET_ATTR_VLAN_FILTERING] = { .name = "vlan_filtering", .type = BLOBMSG_TYPE_BOOL },
};

const struct uci_blob_param_list network_param = {
	.n_params = __NET_ATTR_MAX,
	.params = network_policy,
};

ovsdb_table_t table_Wifi_Inet_Config;
struct blob_buf b = { };
struct blob_buf del = { };
struct uci_context *uci;

static void wifi_inet_conf_load(struct uci_section *s)
{
	struct blob_attr *tb[__NET_ATTR_MAX] = { };
	struct schema_Wifi_Inet_Config conf;
	char *ifname = NULL;

	if (!strcmp(s->e.name, "loopback"))
		return;

	blob_buf_init(&b, 0);

	uci_to_blob(&b, s, &network_param);
	blobmsg_parse(network_policy, __NET_ATTR_MAX, tb, blob_data(b.head), blob_len(b.head));
	memset(&conf, 0, sizeof(conf));

	SCHEMA_SET_STR(conf.if_name, s->e.name);
	if (tb[NET_ATTR_TYPE])
		SCHEMA_SET_STR(conf.if_type, blobmsg_get_string(tb[NET_ATTR_TYPE]));
	else
		SCHEMA_SET_STR(conf.if_type, "eth");

	if (!tb[NET_ATTR_DISABLED] || !blobmsg_get_u8(tb[NET_ATTR_DISABLED])) {
		conf.enabled = true;
		conf.network = true;
	}

	if (tb[NET_ATTR_PROTO])
		SCHEMA_SET_STR(conf.ip_assign_scheme, blobmsg_get_string(tb[NET_ATTR_PROTO]));
	else
		SCHEMA_SET_STR(conf.ip_assign_scheme, "none");

	if (!strcmp(conf.ip_assign_scheme, "dhcpv6"))
		SCHEMA_SET_STR(conf.ip_assign_scheme, "dhcp");

	if (!strcmp(conf.ip_assign_scheme, "static")) {
		if (tb[NET_ATTR_IPADDR])
			SCHEMA_SET_STR(conf.inet_addr, blobmsg_get_string(tb[NET_ATTR_IPADDR]));
		if (tb[NET_ATTR_NETMASK])
			SCHEMA_SET_STR(conf.netmask, blobmsg_get_string(tb[NET_ATTR_NETMASK]));
		if (tb[NET_ATTR_GATEWAY])
			SCHEMA_SET_STR(conf.gateway, blobmsg_get_string(tb[NET_ATTR_GATEWAY]));
		if (tb[NET_ATTR_DNS]) {
			STRSCPY(conf.dns_keys[0], SCHEMA_CONSTS_INET_DNS_PRIMARY);
			STRSCPY(conf.dns[0], blobmsg_get_string(tb[NET_ATTR_DNS]));
			conf.dns_len = 1;
		}
	}

	if (tb[NET_ATTR_MTU])
		SCHEMA_SET_INT(conf.mtu, blobmsg_get_u32(tb[NET_ATTR_MTU]));

	if (tb[NET_ATTR_VID])
		SCHEMA_SET_INT(conf.vlan_id, blobmsg_get_u32(tb[NET_ATTR_VID]));

	if (tb[NET_ATTR_IFNAME])
		ifname = blobmsg_get_string(tb[NET_ATTR_IFNAME]);

	if (ifname && *ifname == '@')
		SCHEMA_SET_STR(conf.parent_ifname, &ifname[1]);
	else
		dhcp_get_config(&conf);
	firewall_get_config(&conf);

	if (!ovsdb_table_upsert(&table_Wifi_Inet_Config, &conf, false))
		LOG(ERR, "inet_config: failed to insert");
}

static int wifi_inet_conf_add(struct schema_Wifi_Inet_Config *iconf)
{
	const char *lease_time = SCHEMA_FIND_KEY(iconf->dhcpd, "lease_time");
	const char *dhcp_start = SCHEMA_FIND_KEY(iconf->dhcpd, "start");
	const char *dhcp_stop = SCHEMA_FIND_KEY(iconf->dhcpd, "stop");
	int len = strlen(iconf->if_name);
	int is_ipv6 = 0;

	if (len && iconf->if_name[len - 1] == '6')
		is_ipv6 = 1;

	blob_buf_init(&b, 0);
	blob_buf_init(&del, 0);

	if (iconf->enabled_exists && !iconf->enabled)
		blobmsg_add_bool(&b, "disabled", 1);
	else
		blobmsg_add_bool(&del, "disabled", 1);

	if (strcmp(iconf->if_type, "eth")) {
		blobmsg_add_string(&b, "type", iconf->if_type);
		blobmsg_add_bool(&b, "vlan_filtering", 1);
	} else
		blobmsg_add_bool(&del, "type", 1);

	if (iconf->ip_assign_scheme_exists) {
		if (is_ipv6 && !strcmp(iconf->ip_assign_scheme, "dhcp"))
			blobmsg_add_string(&b, "proto", "dhcpv6");
		else
			blobmsg_add_string(&b, "proto", iconf->ip_assign_scheme);
	}

	if (iconf->inet_addr_exists)
		blobmsg_add_string(&b, "ipaddr", iconf->inet_addr);

	if (iconf->netmask_exists)
		blobmsg_add_string(&b, "netmask", iconf->netmask);

	if (iconf->vlan_id_exists) {
		blobmsg_add_u32(&b, "ip4table", iconf->vlan_id);
		blobmsg_add_u32(&b, "vid", iconf->vlan_id);
	} else {
		blobmsg_add_bool(&del, "ip4table", 1);
		blobmsg_add_bool(&del, "vid", 1);
	}

	if (iconf->mtu_exists)
		blobmsg_add_u32(&b, "mtu", iconf->mtu);
	else
		blobmsg_add_bool(&del, "mtu", 1);
	blob_to_uci_section(uci, "network", iconf->if_name, "interface", b.head, &network_param, del.head);

	if (!iconf->parent_ifname_exists || iconf->vlan_id > 2) {
		dhcp_add(iconf->if_name, lease_time, dhcp_start, dhcp_stop);
		firewall_add_zone(iconf->if_name, iconf->NAT_exists && iconf->NAT);
	}

	uci_commit_all(uci);

	return 0;
}

static void wifi_inet_conf_del(struct schema_Wifi_Inet_Config *iconf)
{
	if (!strcmp(iconf->if_name, "wan") || !strcmp(iconf->if_name, "lan")) {
		blob_buf_init(&b, 0);
		blobmsg_add_bool(&b, "disabled", 1);
		blob_to_uci_section(uci, "network", iconf->if_name, "interface", b.head, &network_param, NULL);
		return;
	}

	dhcp_del(iconf->if_name);
	firewall_del_zone(iconf->if_name);

	uci_section_del(uci, "network", "network", iconf->if_name, "interface");
	uci_commit_all(uci);
}

static void callback_Wifi_Inet_Config(ovsdb_update_monitor_t *mon,
			       struct schema_Wifi_Inet_Config *old_rec,
			       struct schema_Wifi_Inet_Config *iconf)
{
	switch (mon->mon_type) {
	case OVSDB_UPDATE_NEW:
	case OVSDB_UPDATE_MODIFY:
		wifi_inet_conf_add(iconf);
		break;
	case OVSDB_UPDATE_DEL:
		wifi_inet_conf_del(old_rec);
		break;
	default:
		LOG(ERR, "Invalid Wifi_Inet_Config mon_type(%d)", mon->mon_type);
	}

	return;
}

void wifi_inet_config_init(void)
{
	struct uci_element *e = NULL;
	struct uci_package *network;

	LOGI("Initializing netifd / Wifi Inet Config");
	uci = uci_alloc_context();

	OVSDB_TABLE_INIT(Wifi_Inet_Config, if_name);

	uci_load(uci, "network", &network);
	uci_foreach_element(&network->sections, e) {
		struct uci_section *s = uci_to_section(e);

		if (!strcmp(s->type, "interface"))
			wifi_inet_conf_load(s);
	}

	OVSDB_TABLE_MONITOR(Wifi_Inet_Config, false);

	return;
}

