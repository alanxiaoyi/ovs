/* Copyright (c) 2008, 2009, 2010 Nicira Networks
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <config.h>
#include "bridge.h"
#include <assert.h>
#include <errno.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <inttypes.h>
#include <net/if.h>
#include <openflow/openflow.h>
#include <signal.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include "bitmap.h"
#include "coverage.h"
#include "dirs.h"
#include "dpif.h"
#include "dynamic-string.h"
#include "flow.h"
#include "hash.h"
#include "list.h"
#include "mac-learning.h"
#include "netdev.h"
#include "odp-util.h"
#include "ofp-print.h"
#include "ofpbuf.h"
#include "ofproto/netflow.h"
#include "ofproto/ofproto.h"
#include "packets.h"
#include "poll-loop.h"
#include "port-array.h"
#include "proc-net-compat.h"
#include "process.h"
#include "sha1.h"
#include "shash.h"
#include "socket-util.h"
#include "stream-ssl.h"
#include "svec.h"
#include "timeval.h"
#include "util.h"
#include "unixctl.h"
#include "vconn.h"
#include "vswitchd/vswitch-idl.h"
#include "xenserver.h"
#include "xtoxll.h"

#define THIS_MODULE VLM_bridge
#include "vlog.h"

struct dst {
    uint16_t vlan;
    uint16_t dp_ifidx;
};

struct iface {
    /* These members are always valid. */
    struct port *port;          /* Containing port. */
    size_t port_ifidx;          /* Index within containing port. */
    char *name;                 /* Host network device name. */
    tag_type tag;               /* Tag associated with this interface. */
    long long delay_expires;    /* Time after which 'enabled' may change. */

    /* These members are valid only after bridge_reconfigure() causes them to
     * be initialized.*/
    int dp_ifidx;               /* Index within kernel datapath. */
    struct netdev *netdev;      /* Network device. */
    bool enabled;               /* May be chosen for flows? */

    /* This member is only valid *during* bridge_reconfigure(). */
    const struct ovsrec_interface *cfg;
};

#define BOND_MASK 0xff
struct bond_entry {
    int iface_idx;              /* Index of assigned iface, or -1 if none. */
    uint64_t tx_bytes;          /* Count of bytes recently transmitted. */
    tag_type iface_tag;         /* Tag associated with iface_idx. */
};

#define MAX_MIRRORS 32
typedef uint32_t mirror_mask_t;
#define MIRROR_MASK_C(X) UINT32_C(X)
BUILD_ASSERT_DECL(sizeof(mirror_mask_t) * CHAR_BIT >= MAX_MIRRORS);
struct mirror {
    struct bridge *bridge;
    size_t idx;
    char *name;

    /* Selection criteria. */
    struct shash src_ports;     /* Name is port name; data is always NULL. */
    struct shash dst_ports;     /* Name is port name; data is always NULL. */
    int *vlans;
    size_t n_vlans;

    /* Output. */
    struct port *out_port;
    int out_vlan;
};

#define FLOOD_PORT ((struct port *) 1) /* The 'flood' output port. */
struct port {
    struct bridge *bridge;
    size_t port_idx;
    int vlan;                   /* -1=trunk port, else a 12-bit VLAN ID. */
    unsigned long *trunks;      /* Bitmap of trunked VLANs, if 'vlan' == -1. */
    char *name;

    /* An ordinary bridge port has 1 interface.
     * A bridge port for bonding has at least 2 interfaces. */
    struct iface **ifaces;
    size_t n_ifaces, allocated_ifaces;

    /* Bonding info. */
    struct bond_entry *bond_hash; /* An array of (BOND_MASK + 1) elements. */
    int active_iface;           /* Ifidx on which bcasts accepted, or -1. */
    tag_type active_iface_tag;  /* Tag for bcast flows. */
    tag_type no_ifaces_tag;     /* Tag for flows when all ifaces disabled. */
    int updelay, downdelay;     /* Delay before iface goes up/down, in ms. */
    bool bond_compat_is_stale;  /* Need to call port_update_bond_compat()? */
    bool bond_fake_iface;       /* Fake a bond interface for legacy compat? */

    /* Port mirroring info. */
    mirror_mask_t src_mirrors;  /* Mirrors triggered when packet received. */
    mirror_mask_t dst_mirrors;  /* Mirrors triggered when packet sent. */
    bool is_mirror_output_port; /* Does port mirroring send frames here? */

    /* This member is only valid *during* bridge_reconfigure(). */
    const struct ovsrec_port *cfg;
};

#define DP_MAX_PORTS 255
struct bridge {
    struct list node;           /* Node in global list of bridges. */
    char *name;                 /* User-specified arbitrary name. */
    struct mac_learning *ml;    /* MAC learning table. */
    bool sent_config_request;   /* Successfully sent config request? */
    uint8_t default_ea[ETH_ADDR_LEN]; /* Default MAC. */

    /* Support for remote controllers. */
    char *controller;           /* NULL if there is no remote controller;
                                 * "discover" to do controller discovery;
                                 * otherwise a vconn name. */

    /* OpenFlow switch processing. */
    struct ofproto *ofproto;    /* OpenFlow switch. */

    /* Kernel datapath information. */
    struct dpif *dpif;          /* Datapath. */
    struct port_array ifaces;   /* Indexed by kernel datapath port number. */

    /* Bridge ports. */
    struct port **ports;
    size_t n_ports, allocated_ports;

    /* Bonding. */
    bool has_bonded_ports;
    long long int bond_next_rebalance;

    /* Flow tracking. */
    bool flush;

    /* Flow statistics gathering. */
    time_t next_stats_request;

    /* Port mirroring. */
    struct mirror *mirrors[MAX_MIRRORS];

    /* This member is only valid *during* bridge_reconfigure(). */
    const struct ovsrec_bridge *cfg;
};

/* List of all bridges. */
static struct list all_bridges = LIST_INITIALIZER(&all_bridges);

/* Maximum number of datapaths. */
enum { DP_MAX = 256 };

static struct bridge *bridge_create(const char *name);
static void bridge_destroy(struct bridge *);
static struct bridge *bridge_lookup(const char *name);
static unixctl_cb_func bridge_unixctl_dump_flows;
static int bridge_run_one(struct bridge *);
static void bridge_reconfigure_one(const struct ovsrec_open_vswitch *,
                                   struct bridge *);
static void bridge_reconfigure_controller(const struct ovsrec_open_vswitch *,
                                          struct bridge *);
static void bridge_get_all_ifaces(const struct bridge *, struct shash *ifaces);
static void bridge_fetch_dp_ifaces(struct bridge *);
static void bridge_flush(struct bridge *);
static void bridge_pick_local_hw_addr(struct bridge *,
                                      uint8_t ea[ETH_ADDR_LEN],
                                      struct iface **hw_addr_iface);
static uint64_t bridge_pick_datapath_id(struct bridge *,
                                        const uint8_t bridge_ea[ETH_ADDR_LEN],
                                        struct iface *hw_addr_iface);
static struct iface *bridge_get_local_iface(struct bridge *);
static uint64_t dpid_from_hash(const void *, size_t nbytes);

static unixctl_cb_func bridge_unixctl_fdb_show;

static void bond_init(void);
static void bond_run(struct bridge *);
static void bond_wait(struct bridge *);
static void bond_rebalance_port(struct port *);
static void bond_send_learning_packets(struct port *);
static void bond_enable_slave(struct iface *iface, bool enable);

static struct port *port_create(struct bridge *, const char *name);
static void port_reconfigure(struct port *, const struct ovsrec_port *);
static void port_destroy(struct port *);
static struct port *port_lookup(const struct bridge *, const char *name);
static struct iface *port_lookup_iface(const struct port *, const char *name);
static struct port *port_from_dp_ifidx(const struct bridge *,
                                       uint16_t dp_ifidx);
static void port_update_bond_compat(struct port *);
static void port_update_vlan_compat(struct port *);
static void port_update_bonding(struct port *);

static struct mirror *mirror_create(struct bridge *, const char *name);
static void mirror_destroy(struct mirror *);
static void mirror_reconfigure(struct bridge *);
static void mirror_reconfigure_one(struct mirror *, struct ovsrec_mirror *);
static bool vlan_is_mirrored(const struct mirror *, int vlan);

static struct iface *iface_create(struct port *port, 
                                  const struct ovsrec_interface *if_cfg);
static void iface_destroy(struct iface *);
static struct iface *iface_lookup(const struct bridge *, const char *name);
static struct iface *iface_from_dp_ifidx(const struct bridge *,
                                         uint16_t dp_ifidx);
static bool iface_is_internal(const struct bridge *, const char *name);
static void iface_set_mac(struct iface *);

/* Hooks into ofproto processing. */
static struct ofhooks bridge_ofhooks;

/* Public functions. */

/* Adds the name of each interface used by a bridge, including local and
 * internal ports, to 'svec'. */
void
bridge_get_ifaces(struct svec *svec) 
{
    struct bridge *br, *next;
    size_t i, j;

    LIST_FOR_EACH_SAFE (br, next, struct bridge, node, &all_bridges) {
        for (i = 0; i < br->n_ports; i++) {
            struct port *port = br->ports[i];

            for (j = 0; j < port->n_ifaces; j++) {
                struct iface *iface = port->ifaces[j];
                if (iface->dp_ifidx < 0) {
                    VLOG_ERR("%s interface not in datapath %s, ignoring",
                             iface->name, dpif_name(br->dpif));
                } else {
                    if (iface->dp_ifidx != ODPP_LOCAL) {
                        svec_add(svec, iface->name);
                    }
                }
            }
        }
    }
}

void
bridge_init(const struct ovsrec_open_vswitch *cfg)
{
    struct svec bridge_names;
    struct svec dpif_names;
    size_t i;

    unixctl_command_register("fdb/show", bridge_unixctl_fdb_show, NULL);

    svec_init(&bridge_names);
    for (i = 0; i < cfg->n_bridges; i++) {
        svec_add(&bridge_names, cfg->bridges[i]->name);
    }
    svec_sort(&bridge_names);

    svec_init(&dpif_names);
    dp_enumerate(&dpif_names);
    for (i = 0; i < dpif_names.n; i++) {
        const char *dpif_name = dpif_names.names[i];
        struct dpif *dpif;
        int retval;

        retval = dpif_open(dpif_name, &dpif);
        if (!retval) {
            struct svec all_names;
            size_t j;

            svec_init(&all_names);
            dpif_get_all_names(dpif, &all_names);
            for (j = 0; j < all_names.n; j++) {
                if (svec_contains(&bridge_names, all_names.names[j])) {
                    goto found;
                }
            }
            dpif_delete(dpif);
        found:
            svec_destroy(&all_names);
            dpif_close(dpif);
        }
    }
    svec_destroy(&dpif_names);

    unixctl_command_register("bridge/dump-flows", bridge_unixctl_dump_flows,
                             NULL);

    bond_init();
    bridge_reconfigure(cfg);
}

#ifdef HAVE_OPENSSL
static bool
config_string_change(const char *value, char **valuep)
{
    if (value && (!*valuep || strcmp(value, *valuep))) {
        free(*valuep);
        *valuep = xstrdup(value);
        return true;
    } else {
        return false;
    }
}

static void
bridge_configure_ssl(const struct ovsrec_ssl *ssl)
{
    /* XXX SSL should be configurable on a per-bridge basis.
     * XXX should be possible to de-configure SSL. */
    static char *private_key_file;
    static char *certificate_file;
    static char *cacert_file;
    struct stat s;

    if (!ssl) {
        /* XXX We can't un-set SSL settings. */
        return;
    }

    if (config_string_change(ssl->private_key, &private_key_file)) {
        stream_ssl_set_private_key_file(private_key_file);
    }

    if (config_string_change(ssl->certificate, &certificate_file)) {
        stream_ssl_set_certificate_file(certificate_file);
    }

    /* We assume that even if the filename hasn't changed, if the CA cert 
     * file has been removed, that we want to move back into
     * boot-strapping mode.  This opens a small security hole, because
     * the old certificate will still be trusted until vSwitch is
     * restarted.  We may want to address this in vconn's SSL library. */
    if (config_string_change(ssl->ca_cert, &cacert_file)
        || (cacert_file && stat(cacert_file, &s) && errno == ENOENT)) {
        stream_ssl_set_ca_cert_file(cacert_file, ssl->bootstrap_ca_cert);
    }
}
#endif

/* Attempt to create the network device 'iface_name' through the netdev
 * library. */
static int
set_up_iface(const struct ovsrec_interface *iface_cfg, struct iface *iface,
             bool create)
{
    struct shash_node *node;
    struct shash options;
    int error = 0;
    size_t i;

    shash_init(&options);
    for (i = 0; i < iface_cfg->n_options; i++) {
        shash_add(&options, iface_cfg->key_options[i],
                  xstrdup(iface_cfg->value_options[i]));
    }

    if (create) {
        struct netdev_options netdev_options;

        memset(&netdev_options, 0, sizeof netdev_options);
        netdev_options.name = iface_cfg->name;
        netdev_options.type = iface_cfg->type;
        netdev_options.args = &options;
        netdev_options.ethertype = NETDEV_ETH_TYPE_NONE;
        netdev_options.may_create = true;
        if (iface_is_internal(iface->port->bridge, iface_cfg->name)) {
            netdev_options.may_open = true;
        }

        error = netdev_open(&netdev_options, &iface->netdev);

        if (iface->netdev) {
            netdev_get_carrier(iface->netdev, &iface->enabled);
        }
    } else if (iface->netdev) {
        const char *netdev_type = netdev_get_type(iface->netdev);
        const char *iface_type = iface_cfg->type && strlen(iface_cfg->type)
                                  ? iface_cfg->type : NULL;

        if (!iface_type || !strcmp(netdev_type, iface_type)) {
            error = netdev_reconfigure(iface->netdev, &options);
        } else {
            VLOG_WARN("%s: attempting change device type from %s to %s",
                      iface_cfg->name, netdev_type, iface_type);
            error = EINVAL;
        }
    }

    SHASH_FOR_EACH (node, &options) {
        free(node->data);
    }
    shash_destroy(&options);

    return error;
}

static int
reconfigure_iface(const struct ovsrec_interface *iface_cfg, struct iface *iface)
{
    return set_up_iface(iface_cfg, iface, false);
}

static bool
check_iface_netdev(struct bridge *br UNUSED, struct iface *iface,
                   void *aux UNUSED)
{
    if (!iface->netdev) {
        int error = set_up_iface(iface->cfg, iface, true);
        if (error) {
            VLOG_WARN("could not open netdev on %s, dropping: %s", iface->name,
                                                               strerror(error));
            return false;
        }
    }

    return true;
}

static bool
check_iface_dp_ifidx(struct bridge *br, struct iface *iface, void *aux UNUSED)
{
    if (iface->dp_ifidx >= 0) {
        VLOG_DBG("%s has interface %s on port %d",
                 dpif_name(br->dpif),
                 iface->name, iface->dp_ifidx);
        return true;
    } else {
        VLOG_ERR("%s interface not in %s, dropping",
                 iface->name, dpif_name(br->dpif));
        return false;
    }
}

static bool
set_iface_properties(struct bridge *br UNUSED, struct iface *iface,
                   void *aux UNUSED)
{
    /* Set policing attributes. */
    netdev_set_policing(iface->netdev,
                        iface->cfg->ingress_policing_rate,
                        iface->cfg->ingress_policing_burst);

    /* Set MAC address of internal interfaces other than the local
     * interface. */
    if (iface->dp_ifidx != ODPP_LOCAL
        && iface_is_internal(br, iface->name)) {
        iface_set_mac(iface);
    }

    return true;
}

/* Calls 'cb' for each interfaces in 'br', passing along the 'aux' argument.
 * Deletes from 'br' all the interfaces for which 'cb' returns false, and then
 * deletes from 'br' any ports that no longer have any interfaces. */
static void
iterate_and_prune_ifaces(struct bridge *br,
                         bool (*cb)(struct bridge *, struct iface *,
                                    void *aux),
                         void *aux)
{
    size_t i, j;

    for (i = 0; i < br->n_ports; ) {
        struct port *port = br->ports[i];
        for (j = 0; j < port->n_ifaces; ) {
            struct iface *iface = port->ifaces[j];
            if (cb(br, iface, aux)) {
                j++;
            } else {
                iface_destroy(iface);
            }
        }

        if (port->n_ifaces) {
            i++;
        } else  {
            VLOG_ERR("%s port has no interfaces, dropping", port->name);
            port_destroy(port);
        }
    }
}

void
bridge_reconfigure(const struct ovsrec_open_vswitch *ovs_cfg)
{
    struct ovsdb_idl_txn *txn;
    struct shash old_br, new_br;
    struct shash_node *node;
    struct bridge *br, *next;
    size_t i;

    COVERAGE_INC(bridge_reconfigure);

    txn = ovsdb_idl_txn_create(ovs_cfg->header_.table->idl);

    /* Collect old and new bridges. */
    shash_init(&old_br);
    shash_init(&new_br);
    LIST_FOR_EACH (br, struct bridge, node, &all_bridges) {
        shash_add(&old_br, br->name, br);
    }
    for (i = 0; i < ovs_cfg->n_bridges; i++) {
        const struct ovsrec_bridge *br_cfg = ovs_cfg->bridges[i];
        if (!shash_add_once(&new_br, br_cfg->name, br_cfg)) {
            VLOG_WARN("more than one bridge named %s", br_cfg->name);
        }
    }

    /* Get rid of deleted bridges and add new bridges. */
    LIST_FOR_EACH_SAFE (br, next, struct bridge, node, &all_bridges) {
        struct ovsrec_bridge *br_cfg = shash_find_data(&new_br, br->name);
        if (br_cfg) {
            br->cfg = br_cfg;
        } else {
            bridge_destroy(br);
        }
    }
    SHASH_FOR_EACH (node, &new_br) {
        const char *br_name = node->name;
        const struct ovsrec_bridge *br_cfg = node->data;
        if (!shash_find_data(&old_br, br_name)) {
            br = bridge_create(br_name);
            if (br) {
                br->cfg = br_cfg;
            }
        }
    }
    shash_destroy(&old_br);
    shash_destroy(&new_br);

#ifdef HAVE_OPENSSL
    /* Configure SSL. */
    bridge_configure_ssl(ovs_cfg->ssl);
#endif

    /* Reconfigure all bridges. */
    LIST_FOR_EACH (br, struct bridge, node, &all_bridges) {
        bridge_reconfigure_one(ovs_cfg, br);
    }

    /* Add and delete ports on all datapaths.
     *
     * The kernel will reject any attempt to add a given port to a datapath if
     * that port already belongs to a different datapath, so we must do all
     * port deletions before any port additions. */
    LIST_FOR_EACH (br, struct bridge, node, &all_bridges) {
        struct odp_port *dpif_ports;
        size_t n_dpif_ports;
        struct shash want_ifaces;

        dpif_port_list(br->dpif, &dpif_ports, &n_dpif_ports);
        bridge_get_all_ifaces(br, &want_ifaces);
        for (i = 0; i < n_dpif_ports; i++) {
            const struct odp_port *p = &dpif_ports[i];
            if (!shash_find(&want_ifaces, p->devname)
                && strcmp(p->devname, br->name)) {
                int retval = dpif_port_del(br->dpif, p->port);
                if (retval) {
                    VLOG_ERR("failed to remove %s interface from %s: %s",
                             p->devname, dpif_name(br->dpif),
                             strerror(retval));
                }
            }
        }
        shash_destroy(&want_ifaces);
        free(dpif_ports);
    }
    LIST_FOR_EACH (br, struct bridge, node, &all_bridges) {
        struct odp_port *dpif_ports;
        size_t n_dpif_ports;
        struct shash cur_ifaces, want_ifaces;
        struct shash_node *node;

        /* Get the set of interfaces currently in this datapath. */
        dpif_port_list(br->dpif, &dpif_ports, &n_dpif_ports);
        shash_init(&cur_ifaces);
        for (i = 0; i < n_dpif_ports; i++) {
            const char *name = dpif_ports[i].devname;
            if (!shash_find(&cur_ifaces, name)) {
                shash_add(&cur_ifaces, name, NULL);
            }
        }
        free(dpif_ports);

        /* Get the set of interfaces we want on this datapath. */
        bridge_get_all_ifaces(br, &want_ifaces);

        SHASH_FOR_EACH (node, &want_ifaces) {
            const char *if_name = node->name;
            struct iface *iface = node->data;

            if (shash_find(&cur_ifaces, if_name)) {
                /* Already exists, just reconfigure it. */
                if (iface) {
                    reconfigure_iface(iface->cfg, iface);
                }
            } else {
                /* Need to add to datapath. */
                bool internal;
                int error;

                /* Add to datapath. */
                internal = iface_is_internal(br, if_name);
                error = dpif_port_add(br->dpif, if_name,
                                      internal ? ODP_PORT_INTERNAL : 0, NULL);
                if (error == EFBIG) {
                    VLOG_ERR("ran out of valid port numbers on %s",
                             dpif_name(br->dpif));
                    break;
                } else if (error) {
                    VLOG_ERR("failed to add %s interface to %s: %s",
                             if_name, dpif_name(br->dpif), strerror(error));
                }
            }
        }
        shash_destroy(&cur_ifaces);
        shash_destroy(&want_ifaces);
    }
    LIST_FOR_EACH (br, struct bridge, node, &all_bridges) {
        uint8_t ea[8];
        uint64_t dpid;
        struct iface *local_iface;
        struct iface *hw_addr_iface;
        char *dpid_string;

        bridge_fetch_dp_ifaces(br);

        iterate_and_prune_ifaces(br, check_iface_netdev, NULL);
        iterate_and_prune_ifaces(br, check_iface_dp_ifidx, NULL);

        /* Pick local port hardware address, datapath ID. */
        bridge_pick_local_hw_addr(br, ea, &hw_addr_iface);
        local_iface = bridge_get_local_iface(br);
        if (local_iface) {
            int error = netdev_set_etheraddr(local_iface->netdev, ea);
            if (error) {
                static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 5);
                VLOG_ERR_RL(&rl, "bridge %s: failed to set bridge "
                            "Ethernet address: %s",
                            br->name, strerror(error));
            }
        }

        dpid = bridge_pick_datapath_id(br, ea, hw_addr_iface);
        ofproto_set_datapath_id(br->ofproto, dpid);

        dpid_string = xasprintf("%012"PRIx64, dpid);
        ovsrec_bridge_set_datapath_id(br->cfg, dpid_string);
        free(dpid_string);

        /* Set NetFlow configuration on this bridge. */
        if (br->cfg->netflow) {
            struct ovsrec_netflow *nf_cfg = br->cfg->netflow;
            struct netflow_options opts;

            memset(&opts, 0, sizeof opts);

            dpif_get_netflow_ids(br->dpif, &opts.engine_type, &opts.engine_id);
            if (nf_cfg->engine_type) {
                opts.engine_type = nf_cfg->engine_type;
            }
            if (nf_cfg->engine_id) {
                opts.engine_id = nf_cfg->engine_id;
            }

            opts.active_timeout = nf_cfg->active_timeout;
            if (!opts.active_timeout) {
                opts.active_timeout = -1;
            } else if (opts.active_timeout < 0) {
                VLOG_WARN("bridge %s: active timeout interval set to negative "
                          "value, using default instead (%d seconds)", br->name,
                          NF_ACTIVE_TIMEOUT_DEFAULT);
                opts.active_timeout = -1;
            }

            opts.add_id_to_iface = nf_cfg->add_id_to_interface;
            if (opts.add_id_to_iface) {
                if (opts.engine_id > 0x7f) {
                    VLOG_WARN("bridge %s: netflow port mangling may conflict "
                              "with another vswitch, choose an engine id less "
                              "than 128", br->name);
                }
                if (br->n_ports > 508) {
                    VLOG_WARN("bridge %s: netflow port mangling will conflict "
                              "with another port when more than 508 ports are "
                              "used", br->name);
                }
            }

            opts.collectors.n = nf_cfg->n_targets;
            opts.collectors.names = nf_cfg->targets;
            if (ofproto_set_netflow(br->ofproto, &opts)) {
                VLOG_ERR("bridge %s: problem setting netflow collectors", 
                         br->name);
            }
        } else {
            ofproto_set_netflow(br->ofproto, NULL);
        }

        /* Update the controller and related settings.  It would be more
         * straightforward to call this from bridge_reconfigure_one(), but we
         * can't do it there for two reasons.  First, and most importantly, at
         * that point we don't know the dp_ifidx of any interfaces that have
         * been added to the bridge (because we haven't actually added them to
         * the datapath).  Second, at that point we haven't set the datapath ID
         * yet; when a controller is configured, resetting the datapath ID will
         * immediately disconnect from the controller, so it's better to set
         * the datapath ID before the controller. */
        bridge_reconfigure_controller(ovs_cfg, br);
    }
    LIST_FOR_EACH (br, struct bridge, node, &all_bridges) {
        for (i = 0; i < br->n_ports; i++) {
            struct port *port = br->ports[i];

            port_update_vlan_compat(port);
            port_update_bonding(port);
        }
    }
    LIST_FOR_EACH (br, struct bridge, node, &all_bridges) {
        iterate_and_prune_ifaces(br, set_iface_properties, NULL);
    }

    ovsrec_open_vswitch_set_cur_cfg(ovs_cfg, ovs_cfg->next_cfg);

    ovsdb_idl_txn_commit(txn);
    ovsdb_idl_txn_destroy(txn); /* XXX */
}

static const char *
bridge_get_other_config(const struct ovsrec_bridge *br_cfg, const char *key)
{
    size_t i;

    for (i = 0; i < br_cfg->n_other_config; i++) {
        if (!strcmp(br_cfg->key_other_config[i], key)) {
            return br_cfg->value_other_config[i];
        }
    }
    return NULL;
}

static void
bridge_pick_local_hw_addr(struct bridge *br, uint8_t ea[ETH_ADDR_LEN],
                          struct iface **hw_addr_iface)
{
    const char *hwaddr;
    size_t i, j;
    int error;

    *hw_addr_iface = NULL;

    /* Did the user request a particular MAC? */
    hwaddr = bridge_get_other_config(br->cfg, "hwaddr");
    if (hwaddr && eth_addr_from_string(hwaddr, ea)) {
        if (eth_addr_is_multicast(ea)) {
            VLOG_ERR("bridge %s: cannot set MAC address to multicast "
                     "address "ETH_ADDR_FMT, br->name, ETH_ADDR_ARGS(ea));
        } else if (eth_addr_is_zero(ea)) {
            VLOG_ERR("bridge %s: cannot set MAC address to zero", br->name);
        } else {
            return;
        }
    }

    /* Otherwise choose the minimum non-local MAC address among all of the
     * interfaces. */
    memset(ea, 0xff, sizeof ea);
    for (i = 0; i < br->n_ports; i++) {
        struct port *port = br->ports[i];
        uint8_t iface_ea[ETH_ADDR_LEN];
        struct iface *iface;

        /* Mirror output ports don't participate. */
        if (port->is_mirror_output_port) {
            continue;
        }

        /* Choose the MAC address to represent the port. */
        if (port->cfg->mac && eth_addr_from_string(port->cfg->mac, iface_ea)) {
            /* Find the interface with this Ethernet address (if any) so that
             * we can provide the correct devname to the caller. */
            iface = NULL;
            for (j = 0; j < port->n_ifaces; j++) {
                struct iface *candidate = port->ifaces[j];
                uint8_t candidate_ea[ETH_ADDR_LEN];
                if (!netdev_get_etheraddr(candidate->netdev, candidate_ea)
                    && eth_addr_equals(iface_ea, candidate_ea)) {
                    iface = candidate;
                }
            }
        } else {
            /* Choose the interface whose MAC address will represent the port.
             * The Linux kernel bonding code always chooses the MAC address of
             * the first slave added to a bond, and the Fedora networking
             * scripts always add slaves to a bond in alphabetical order, so
             * for compatibility we choose the interface with the name that is
             * first in alphabetical order. */
            iface = port->ifaces[0];
            for (j = 1; j < port->n_ifaces; j++) {
                struct iface *candidate = port->ifaces[j];
                if (strcmp(candidate->name, iface->name) < 0) {
                    iface = candidate;
                }
            }

            /* The local port doesn't count (since we're trying to choose its
             * MAC address anyway). */
            if (iface->dp_ifidx == ODPP_LOCAL) {
                continue;
            }

            /* Grab MAC. */
            error = netdev_get_etheraddr(iface->netdev, iface_ea);
            if (error) {
                static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 5);
                VLOG_ERR_RL(&rl, "failed to obtain Ethernet address of %s: %s",
                            iface->name, strerror(error));
                continue;
            }
        }

        /* Compare against our current choice. */
        if (!eth_addr_is_multicast(iface_ea) &&
            !eth_addr_is_local(iface_ea) &&
            !eth_addr_is_reserved(iface_ea) &&
            !eth_addr_is_zero(iface_ea) &&
            memcmp(iface_ea, ea, ETH_ADDR_LEN) < 0)
        {
            memcpy(ea, iface_ea, ETH_ADDR_LEN);
            *hw_addr_iface = iface;
        }
    }
    if (eth_addr_is_multicast(ea)) {
        memcpy(ea, br->default_ea, ETH_ADDR_LEN);
        *hw_addr_iface = NULL;
        VLOG_WARN("bridge %s: using default bridge Ethernet "
                  "address "ETH_ADDR_FMT, br->name, ETH_ADDR_ARGS(ea));
    } else {
        VLOG_DBG("bridge %s: using bridge Ethernet address "ETH_ADDR_FMT,
                 br->name, ETH_ADDR_ARGS(ea));
    }
}

/* Choose and returns the datapath ID for bridge 'br' given that the bridge
 * Ethernet address is 'bridge_ea'.  If 'bridge_ea' is the Ethernet address of
 * an interface on 'br', then that interface must be passed in as
 * 'hw_addr_iface'; if 'bridge_ea' was derived some other way, then
 * 'hw_addr_iface' must be passed in as a null pointer. */
static uint64_t
bridge_pick_datapath_id(struct bridge *br,
                        const uint8_t bridge_ea[ETH_ADDR_LEN],
                        struct iface *hw_addr_iface)
{
    /*
     * The procedure for choosing a bridge MAC address will, in the most
     * ordinary case, also choose a unique MAC that we can use as a datapath
     * ID.  In some special cases, though, multiple bridges will end up with
     * the same MAC address.  This is OK for the bridges, but it will confuse
     * the OpenFlow controller, because each datapath needs a unique datapath
     * ID.
     *
     * Datapath IDs must be unique.  It is also very desirable that they be
     * stable from one run to the next, so that policy set on a datapath
     * "sticks".
     */
    const char *datapath_id;
    uint64_t dpid;

    datapath_id = bridge_get_other_config(br->cfg, "datapath-id");
    if (datapath_id && dpid_from_string(datapath_id, &dpid)) {
        return dpid;
    }

    if (hw_addr_iface) {
        int vlan;
        if (!netdev_get_vlan_vid(hw_addr_iface->netdev, &vlan)) {
            /*
             * A bridge whose MAC address is taken from a VLAN network device
             * (that is, a network device created with vconfig(8) or similar
             * tool) will have the same MAC address as a bridge on the VLAN
             * device's physical network device.
             *
             * Handle this case by hashing the physical network device MAC
             * along with the VLAN identifier.
             */
            uint8_t buf[ETH_ADDR_LEN + 2];
            memcpy(buf, bridge_ea, ETH_ADDR_LEN);
            buf[ETH_ADDR_LEN] = vlan >> 8;
            buf[ETH_ADDR_LEN + 1] = vlan;
            return dpid_from_hash(buf, sizeof buf);
        } else {
            /*
             * Assume that this bridge's MAC address is unique, since it
             * doesn't fit any of the cases we handle specially.
             */
        }
    } else {
        /*
         * A purely internal bridge, that is, one that has no non-virtual
         * network devices on it at all, is more difficult because it has no
         * natural unique identifier at all.
         *
         * When the host is a XenServer, we handle this case by hashing the
         * host's UUID with the name of the bridge.  Names of bridges are
         * persistent across XenServer reboots, although they can be reused if
         * an internal network is destroyed and then a new one is later
         * created, so this is fairly effective.
         *
         * When the host is not a XenServer, we punt by using a random MAC
         * address on each run.
         */
        const char *host_uuid = xenserver_get_host_uuid();
        if (host_uuid) {
            char *combined = xasprintf("%s,%s", host_uuid, br->name);
            dpid = dpid_from_hash(combined, strlen(combined));
            free(combined);
            return dpid;
        }
    }

    return eth_addr_to_uint64(bridge_ea);
}

static uint64_t
dpid_from_hash(const void *data, size_t n)
{
    uint8_t hash[SHA1_DIGEST_SIZE];

    BUILD_ASSERT_DECL(sizeof hash >= ETH_ADDR_LEN);
    sha1_bytes(data, n, hash);
    eth_addr_mark_random(hash);
    return eth_addr_to_uint64(hash);
}

int
bridge_run(void)
{
    struct bridge *br, *next;
    int retval;

    retval = 0;
    LIST_FOR_EACH_SAFE (br, next, struct bridge, node, &all_bridges) {
        int error = bridge_run_one(br);
        if (error) {
            static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 5);
            VLOG_ERR_RL(&rl, "bridge %s: datapath was destroyed externally, "
                        "forcing reconfiguration", br->name);
            if (!retval) {
                retval = error;
            }
        }
    }
    return retval;
}

void
bridge_wait(void)
{
    struct bridge *br;

    LIST_FOR_EACH (br, struct bridge, node, &all_bridges) {
        ofproto_wait(br->ofproto);
        if (br->controller) {
            continue;
        }

        mac_learning_wait(br->ml);
        bond_wait(br);
    }
}

/* Forces 'br' to revalidate all of its flows.  This is appropriate when 'br''s
 * configuration changes.  */
static void
bridge_flush(struct bridge *br)
{
    COVERAGE_INC(bridge_flush);
    br->flush = true;
    mac_learning_flush(br->ml);
}

/* Returns the 'br' interface for the ODPP_LOCAL port, or null if 'br' has no
 * such interface. */
static struct iface *
bridge_get_local_iface(struct bridge *br)
{
    size_t i, j;

    for (i = 0; i < br->n_ports; i++) {
        struct port *port = br->ports[i];
        for (j = 0; j < port->n_ifaces; j++) {
            struct iface *iface = port->ifaces[j];
            if (iface->dp_ifidx == ODPP_LOCAL) {
                return iface;
            }
        }
    }

    return NULL;
}

/* Bridge unixctl user interface functions. */
static void
bridge_unixctl_fdb_show(struct unixctl_conn *conn,
                        const char *args, void *aux UNUSED)
{
    struct ds ds = DS_EMPTY_INITIALIZER;
    const struct bridge *br;
    const struct mac_entry *e;

    br = bridge_lookup(args);
    if (!br) {
        unixctl_command_reply(conn, 501, "no such bridge");
        return;
    }

    ds_put_cstr(&ds, " port  VLAN  MAC                Age\n");
    LIST_FOR_EACH (e, struct mac_entry, lru_node, &br->ml->lrus) {
        if (e->port < 0 || e->port >= br->n_ports) {
            continue;
        }
        ds_put_format(&ds, "%5d  %4d  "ETH_ADDR_FMT"  %3d\n",
                      br->ports[e->port]->ifaces[0]->dp_ifidx,
                      e->vlan, ETH_ADDR_ARGS(e->mac), mac_entry_age(e));
    }
    unixctl_command_reply(conn, 200, ds_cstr(&ds));
    ds_destroy(&ds);
}

/* Bridge reconfiguration functions. */
static struct bridge *
bridge_create(const char *name)
{
    struct bridge *br;
    int error;

    assert(!bridge_lookup(name));
    br = xzalloc(sizeof *br);

    error = dpif_create_and_open(name, &br->dpif);
    if (error) {
        free(br);
        return NULL;
    }
    dpif_flow_flush(br->dpif);

    error = ofproto_create(name, &bridge_ofhooks, br, &br->ofproto);
    if (error) {
        VLOG_ERR("failed to create switch %s: %s", name, strerror(error));
        dpif_delete(br->dpif);
        dpif_close(br->dpif);
        free(br);
        return NULL;
    }

    br->name = xstrdup(name);
    br->ml = mac_learning_create();
    br->sent_config_request = false;
    eth_addr_random(br->default_ea);

    port_array_init(&br->ifaces);

    br->flush = false;
    br->bond_next_rebalance = time_msec() + 10000;

    list_push_back(&all_bridges, &br->node);

    VLOG_INFO("created bridge %s on %s", br->name, dpif_name(br->dpif));

    return br;
}

static void
bridge_destroy(struct bridge *br)
{
    if (br) {
        int error;

        while (br->n_ports > 0) {
            port_destroy(br->ports[br->n_ports - 1]);
        }
        list_remove(&br->node);
        error = dpif_delete(br->dpif);
        if (error && error != ENOENT) {
            VLOG_ERR("failed to delete %s: %s",
                     dpif_name(br->dpif), strerror(error));
        }
        dpif_close(br->dpif);
        ofproto_destroy(br->ofproto);
        free(br->controller);
        mac_learning_destroy(br->ml);
        port_array_destroy(&br->ifaces);
        free(br->ports);
        free(br->name);
        free(br);
    }
}

static struct bridge *
bridge_lookup(const char *name)
{
    struct bridge *br;

    LIST_FOR_EACH (br, struct bridge, node, &all_bridges) {
        if (!strcmp(br->name, name)) {
            return br;
        }
    }
    return NULL;
}

bool
bridge_exists(const char *name)
{
    return bridge_lookup(name) ? true : false;
}

uint64_t
bridge_get_datapathid(const char *name)
{
    struct bridge *br = bridge_lookup(name);
    return br ? ofproto_get_datapath_id(br->ofproto) : 0;
}

/* Handle requests for a listing of all flows known by the OpenFlow
 * stack, including those normally hidden. */
static void
bridge_unixctl_dump_flows(struct unixctl_conn *conn,
                          const char *args, void *aux UNUSED)
{
    struct bridge *br;
    struct ds results;
    
    br = bridge_lookup(args);
    if (!br) {
        unixctl_command_reply(conn, 501, "Unknown bridge");
        return;
    }

    ds_init(&results);
    ofproto_get_all_flows(br->ofproto, &results);

    unixctl_command_reply(conn, 200, ds_cstr(&results));
    ds_destroy(&results);
}

static int
bridge_run_one(struct bridge *br)
{
    int error;

    error = ofproto_run1(br->ofproto);
    if (error) {
        return error;
    }

    mac_learning_run(br->ml, ofproto_get_revalidate_set(br->ofproto));
    bond_run(br);

    error = ofproto_run2(br->ofproto, br->flush);
    br->flush = false;

    return error;
}

static const struct ovsrec_controller *
bridge_get_controller(const struct ovsrec_open_vswitch *ovs_cfg,
                      const struct bridge *br)
{
    const struct ovsrec_controller *controller;

    controller = (br->cfg->controller ? br->cfg->controller
                  : ovs_cfg->controller ? ovs_cfg->controller
                  : NULL);

    if (controller && !strcmp(controller->target, "none")) {
        return NULL;
    }

    return controller;
}

static bool
check_duplicate_ifaces(struct bridge *br, struct iface *iface, void *ifaces_)
{
    struct svec *ifaces = ifaces_;
    if (!svec_contains(ifaces, iface->name)) {
        svec_add(ifaces, iface->name);
        svec_sort(ifaces);
        return true;
    } else {
        VLOG_ERR("bridge %s: %s interface is on multiple ports, "
                 "removing from %s",
                 br->name, iface->name, iface->port->name);
        return false;
    }
}

static void
bridge_reconfigure_one(const struct ovsrec_open_vswitch *ovs_cfg,
                       struct bridge *br)
{
    struct shash old_ports, new_ports;
    struct svec ifaces;
    struct svec listeners, old_listeners;
    struct svec snoops, old_snoops;
    struct shash_node *node;
    uint64_t mgmt_id;
    size_t i;

    /* Collect old ports. */
    shash_init(&old_ports);
    for (i = 0; i < br->n_ports; i++) {
        shash_add(&old_ports, br->ports[i]->name, br->ports[i]);
    }

    /* Collect new ports. */
    shash_init(&new_ports);
    for (i = 0; i < br->cfg->n_ports; i++) {
        const char *name = br->cfg->ports[i]->name;
        if (!shash_add_once(&new_ports, name, br->cfg->ports[i])) {
            VLOG_WARN("bridge %s: %s specified twice as bridge port",
                      br->name, name);
        }
    }

    /* If we have a controller, then we need a local port.  Complain if the
     * user didn't specify one.
     *
     * XXX perhaps we should synthesize a port ourselves in this case. */
    if (bridge_get_controller(ovs_cfg, br)) {
        char local_name[IF_NAMESIZE];
        int error;

        error = dpif_port_get_name(br->dpif, ODPP_LOCAL,
                                   local_name, sizeof local_name);
        if (!error && !shash_find(&new_ports, local_name)) {
            VLOG_WARN("bridge %s: controller specified but no local port "
                      "(port named %s) defined",
                      br->name, local_name);
        }
    }

    dpid_from_string(ovs_cfg->management_id, &mgmt_id);
    ofproto_set_mgmt_id(br->ofproto, mgmt_id);

    /* Get rid of deleted ports and add new ports. */
    SHASH_FOR_EACH (node, &old_ports) {
        if (!shash_find(&new_ports, node->name)) {
            port_destroy(node->data);
        }
    }
    SHASH_FOR_EACH (node, &new_ports) {
        struct port *port = shash_find_data(&old_ports, node->name);
        if (!port) {
            port = port_create(br, node->name);
        }
        port_reconfigure(port, node->data);
    }
    shash_destroy(&old_ports);
    shash_destroy(&new_ports);

    /* Check and delete duplicate interfaces. */
    svec_init(&ifaces);
    iterate_and_prune_ifaces(br, check_duplicate_ifaces, &ifaces);
    svec_destroy(&ifaces);

    /* Delete all flows if we're switching from connected to standalone or vice
     * versa.  (XXX Should we delete all flows if we are switching from one
     * controller to another?) */

#if 0
    /* Configure OpenFlow management listeners. */
    svec_init(&listeners);
    cfg_get_all_strings(&listeners, "bridge.%s.openflow.listeners", br->name);
    if (!listeners.n) {
        svec_add_nocopy(&listeners, xasprintf("punix:%s/%s.mgmt",
                                              ovs_rundir, br->name));
    } else if (listeners.n == 1 && !strcmp(listeners.names[0], "none")) {
        svec_clear(&listeners);
    }
    svec_sort_unique(&listeners);

    svec_init(&old_listeners);
    ofproto_get_listeners(br->ofproto, &old_listeners);
    svec_sort_unique(&old_listeners);

    if (!svec_equal(&listeners, &old_listeners)) {
        ofproto_set_listeners(br->ofproto, &listeners);
    }
    svec_destroy(&listeners);
    svec_destroy(&old_listeners);

    /* Configure OpenFlow controller connection snooping. */
    svec_init(&snoops);
    cfg_get_all_strings(&snoops, "bridge.%s.openflow.snoops", br->name);
    if (!snoops.n) {
        svec_add_nocopy(&snoops, xasprintf("punix:%s/%s.snoop",
                                           ovs_rundir, br->name));
    } else if (snoops.n == 1 && !strcmp(snoops.names[0], "none")) {
        svec_clear(&snoops);
    }
    svec_sort_unique(&snoops);

    svec_init(&old_snoops);
    ofproto_get_snoops(br->ofproto, &old_snoops);
    svec_sort_unique(&old_snoops);

    if (!svec_equal(&snoops, &old_snoops)) {
        ofproto_set_snoops(br->ofproto, &snoops);
    }
    svec_destroy(&snoops);
    svec_destroy(&old_snoops);
#else
    /* Default listener. */
    svec_init(&listeners);
    svec_add_nocopy(&listeners, xasprintf("punix:%s/%s.mgmt",
                                          ovs_rundir, br->name));
    svec_init(&old_listeners);
    ofproto_get_listeners(br->ofproto, &old_listeners);
    if (!svec_equal(&listeners, &old_listeners)) {
        ofproto_set_listeners(br->ofproto, &listeners);
    }
    svec_destroy(&listeners);
    svec_destroy(&old_listeners);

    /* Default snoop. */
    svec_init(&snoops);
    svec_add_nocopy(&snoops, xasprintf("punix:%s/%s.snoop",
                                       ovs_rundir, br->name));
    svec_init(&old_snoops);
    ofproto_get_snoops(br->ofproto, &old_snoops);
    if (!svec_equal(&snoops, &old_snoops)) {
        ofproto_set_snoops(br->ofproto, &snoops);
    }
    svec_destroy(&snoops);
    svec_destroy(&old_snoops);
#endif

    mirror_reconfigure(br);
}

static void
bridge_reconfigure_controller(const struct ovsrec_open_vswitch *ovs_cfg,
                              struct bridge *br)
{
    char *pfx = xasprintf("bridge.%s.controller", br->name);
    const struct ovsrec_controller *c;

    c = bridge_get_controller(ovs_cfg, br);
    if ((br->controller != NULL) != (c != NULL)) {
        ofproto_flush_flows(br->ofproto);
    }
    free(br->controller);
    br->controller = c ? xstrdup(c->target) : NULL;

    if (c) {
        int max_backoff, probe;
        int rate_limit, burst_limit;

        if (!strcmp(c->target, "discover")) {
            ofproto_set_discovery(br->ofproto, true,
                                  c->discover_accept_regex,
                                  c->discover_update_resolv_conf);
        } else {
            struct iface *local_iface;
            struct in_addr ip;
            bool in_band;

            in_band = (!c->connection_mode
                       || !strcmp(c->connection_mode, "out-of-band"));
            ofproto_set_discovery(br->ofproto, false, NULL, NULL);
            ofproto_set_in_band(br->ofproto, in_band);

            local_iface = bridge_get_local_iface(br);
            if (local_iface && c->local_ip && inet_aton(c->local_ip, &ip)) {
                struct netdev *netdev = local_iface->netdev;
                struct in_addr ip, mask, gateway;

                if (!c->local_netmask || !inet_aton(c->local_netmask, &mask)) {
                    mask.s_addr = 0;
                }
                if (!c->local_gateway
                    || !inet_aton(c->local_gateway, &gateway)) {
                    gateway.s_addr = 0;
                }

                netdev_turn_flags_on(netdev, NETDEV_UP, true);
                if (!mask.s_addr) {
                    mask.s_addr = guess_netmask(ip.s_addr);
                }
                if (!netdev_set_in4(netdev, ip, mask)) {
                    VLOG_INFO("bridge %s: configured IP address "IP_FMT", "
                              "netmask "IP_FMT,
                              br->name, IP_ARGS(&ip.s_addr),
                              IP_ARGS(&mask.s_addr));
                }

                if (gateway.s_addr) {
                    if (!netdev_add_router(netdev, gateway)) {
                        VLOG_INFO("bridge %s: configured gateway "IP_FMT,
                                  br->name, IP_ARGS(&gateway.s_addr));
                    }
                }
            }
        }

        ofproto_set_failure(br->ofproto,
                            (!c->fail_mode
                             || !strcmp(c->fail_mode, "standalone")
                             || !strcmp(c->fail_mode, "open")));

        probe = c->inactivity_probe ? *c->inactivity_probe / 1000 : 5;
        ofproto_set_probe_interval(br->ofproto, probe);

        max_backoff = c->max_backoff ? *c->max_backoff / 1000 : 8;
        ofproto_set_max_backoff(br->ofproto, max_backoff);

        rate_limit = c->controller_rate_limit ? *c->controller_rate_limit : 0;
        burst_limit = c->controller_burst_limit ? *c->controller_burst_limit : 0;
        ofproto_set_rate_limit(br->ofproto, rate_limit, burst_limit);
    } else {
        union ofp_action action;
        flow_t flow;

        /* Set up a flow that matches every packet and directs them to
         * OFPP_NORMAL (which goes to us). */
        memset(&action, 0, sizeof action);
        action.type = htons(OFPAT_OUTPUT);
        action.output.len = htons(sizeof action);
        action.output.port = htons(OFPP_NORMAL);
        memset(&flow, 0, sizeof flow);
        ofproto_add_flow(br->ofproto, &flow, OFPFW_ALL, 0,
                         &action, 1, 0);

        ofproto_set_in_band(br->ofproto, false);
        ofproto_set_max_backoff(br->ofproto, 1);
        ofproto_set_probe_interval(br->ofproto, 5);
        ofproto_set_failure(br->ofproto, false);
    }
    free(pfx);

    ofproto_set_controller(br->ofproto, br->controller);
}

static void
bridge_get_all_ifaces(const struct bridge *br, struct shash *ifaces)
{
    size_t i, j;

    shash_init(ifaces);
    for (i = 0; i < br->n_ports; i++) {
        struct port *port = br->ports[i];
        for (j = 0; j < port->n_ifaces; j++) {
            struct iface *iface = port->ifaces[j];
            shash_add_once(ifaces, iface->name, iface);
        }
        if (port->n_ifaces > 1 && port->cfg->bond_fake_iface) {
            shash_add_once(ifaces, port->name, NULL);
        }
    }
}

/* For robustness, in case the administrator moves around datapath ports behind
 * our back, we re-check all the datapath port numbers here.
 *
 * This function will set the 'dp_ifidx' members of interfaces that have
 * disappeared to -1, so only call this function from a context where those
 * 'struct iface's will be removed from the bridge.  Otherwise, the -1
 * 'dp_ifidx'es will cause trouble later when we try to send them to the
 * datapath, which doesn't support UINT16_MAX+1 ports. */
static void
bridge_fetch_dp_ifaces(struct bridge *br)
{
    struct odp_port *dpif_ports;
    size_t n_dpif_ports;
    size_t i, j;

    /* Reset all interface numbers. */
    for (i = 0; i < br->n_ports; i++) {
        struct port *port = br->ports[i];
        for (j = 0; j < port->n_ifaces; j++) {
            struct iface *iface = port->ifaces[j];
            iface->dp_ifidx = -1;
        }
    }
    port_array_clear(&br->ifaces);

    dpif_port_list(br->dpif, &dpif_ports, &n_dpif_ports);
    for (i = 0; i < n_dpif_ports; i++) {
        struct odp_port *p = &dpif_ports[i];
        struct iface *iface = iface_lookup(br, p->devname);
        if (iface) {
            if (iface->dp_ifidx >= 0) {
                VLOG_WARN("%s reported interface %s twice",
                          dpif_name(br->dpif), p->devname);
            } else if (iface_from_dp_ifidx(br, p->port)) {
                VLOG_WARN("%s reported interface %"PRIu16" twice",
                          dpif_name(br->dpif), p->port);
            } else {
                port_array_set(&br->ifaces, p->port, iface);
                iface->dp_ifidx = p->port;
            }

            if (iface->cfg) {
                int64_t ofport = (iface->dp_ifidx >= 0
                                  ? odp_port_to_ofp_port(iface->dp_ifidx)
                                  : -1);
                ovsrec_interface_set_ofport(iface->cfg, &ofport, 1);
            }
        }
    }
    free(dpif_ports);
}

/* Bridge packet processing functions. */

static int
bond_hash(const uint8_t mac[ETH_ADDR_LEN])
{
    return hash_bytes(mac, ETH_ADDR_LEN, 0) & BOND_MASK;
}

static struct bond_entry *
lookup_bond_entry(const struct port *port, const uint8_t mac[ETH_ADDR_LEN])
{
    return &port->bond_hash[bond_hash(mac)];
}

static int
bond_choose_iface(const struct port *port)
{
    static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 20);
    size_t i, best_down_slave = -1;
    long long next_delay_expiration = LLONG_MAX;

    for (i = 0; i < port->n_ifaces; i++) {
        struct iface *iface = port->ifaces[i];

        if (iface->enabled) {
            return i;
        } else if (iface->delay_expires < next_delay_expiration) {
            best_down_slave = i;
            next_delay_expiration = iface->delay_expires;
        }
    }

    if (best_down_slave != -1) {
        struct iface *iface = port->ifaces[best_down_slave];

        VLOG_INFO_RL(&rl, "interface %s: skipping remaining %lli ms updelay "
                     "since no other interface is up", iface->name,
                     iface->delay_expires - time_msec());
        bond_enable_slave(iface, true);
    }

    return best_down_slave;
}

static bool
choose_output_iface(const struct port *port, const uint8_t *dl_src,
                    uint16_t *dp_ifidx, tag_type *tags)
{
    struct iface *iface;

    assert(port->n_ifaces);
    if (port->n_ifaces == 1) {
        iface = port->ifaces[0];
    } else {
        struct bond_entry *e = lookup_bond_entry(port, dl_src);
        if (e->iface_idx < 0 || e->iface_idx >= port->n_ifaces
            || !port->ifaces[e->iface_idx]->enabled) {
            /* XXX select interface properly.  The current interface selection
             * is only good for testing the rebalancing code. */
            e->iface_idx = bond_choose_iface(port);
            if (e->iface_idx < 0) {
                *tags |= port->no_ifaces_tag;
                return false;
            }
            e->iface_tag = tag_create_random();
            ((struct port *) port)->bond_compat_is_stale = true;
        }
        *tags |= e->iface_tag;
        iface = port->ifaces[e->iface_idx];
    }
    *dp_ifidx = iface->dp_ifidx;
    *tags |= iface->tag;        /* Currently only used for bonding. */
    return true;
}

static void
bond_link_status_update(struct iface *iface, bool carrier)
{
    static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 20);
    struct port *port = iface->port;

    if ((carrier == iface->enabled) == (iface->delay_expires == LLONG_MAX)) {
        /* Nothing to do. */
        return;
    }
    VLOG_INFO_RL(&rl, "interface %s: carrier %s",
                 iface->name, carrier ? "detected" : "dropped");
    if (carrier == iface->enabled) {
        iface->delay_expires = LLONG_MAX;
        VLOG_INFO_RL(&rl, "interface %s: will not be %s",
                     iface->name, carrier ? "disabled" : "enabled");
    } else if (carrier && port->active_iface < 0) {
        bond_enable_slave(iface, true);
        if (port->updelay) {
            VLOG_INFO_RL(&rl, "interface %s: skipping %d ms updelay since no "
                         "other interface is up", iface->name, port->updelay);
        }
    } else {
        int delay = carrier ? port->updelay : port->downdelay;
        iface->delay_expires = time_msec() + delay;
        if (delay) {
            VLOG_INFO_RL(&rl,
                         "interface %s: will be %s if it stays %s for %d ms",
                         iface->name,
                         carrier ? "enabled" : "disabled",
                         carrier ? "up" : "down",
                         delay);
        }
    }
}

static void
bond_choose_active_iface(struct port *port)
{
    static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 20);

    port->active_iface = bond_choose_iface(port);
    port->active_iface_tag = tag_create_random();
    if (port->active_iface >= 0) {
        VLOG_INFO_RL(&rl, "port %s: active interface is now %s",
                     port->name, port->ifaces[port->active_iface]->name);
    } else {
        VLOG_WARN_RL(&rl, "port %s: all ports disabled, no active interface",
                     port->name);
    }
}

static void
bond_enable_slave(struct iface *iface, bool enable)
{
    struct port *port = iface->port;
    struct bridge *br = port->bridge;

    /* This acts as a recursion check.  If the act of disabling a slave
     * causes a different slave to be enabled, the flag will allow us to
     * skip redundant work when we reenter this function.  It must be
     * cleared on exit to keep things safe with multiple bonds. */
    static bool moving_active_iface = false;

    iface->delay_expires = LLONG_MAX;
    if (enable == iface->enabled) {
        return;
    }

    iface->enabled = enable;
    if (!iface->enabled) {
        VLOG_WARN("interface %s: disabled", iface->name);
        ofproto_revalidate(br->ofproto, iface->tag);
        if (iface->port_ifidx == port->active_iface) {
            ofproto_revalidate(br->ofproto,
                               port->active_iface_tag);

            /* Disabling a slave can lead to another slave being immediately
             * enabled if there will be no active slaves but one is waiting
             * on an updelay.  In this case we do not need to run most of the
             * code for the newly enabled slave since there was no period
             * without an active slave and it is redundant with the disabling
             * path. */
            moving_active_iface = true;
            bond_choose_active_iface(port);
        }
        bond_send_learning_packets(port);
    } else {
        VLOG_WARN("interface %s: enabled", iface->name);
        if (port->active_iface < 0 && !moving_active_iface) {
            ofproto_revalidate(br->ofproto, port->no_ifaces_tag);
            bond_choose_active_iface(port);
            bond_send_learning_packets(port);
        }
        iface->tag = tag_create_random();
    }

    moving_active_iface = false;
    port->bond_compat_is_stale = true;
}

static void
bond_run(struct bridge *br)
{
    size_t i, j;

    for (i = 0; i < br->n_ports; i++) {
        struct port *port = br->ports[i];

        if (port->n_ifaces >= 2) {
            for (j = 0; j < port->n_ifaces; j++) {
                struct iface *iface = port->ifaces[j];
                if (time_msec() >= iface->delay_expires) {
                    bond_enable_slave(iface, !iface->enabled);
                }
            }
        }

        if (port->bond_compat_is_stale) {
            port->bond_compat_is_stale = false;
            port_update_bond_compat(port);
        }
    }
}

static void
bond_wait(struct bridge *br)
{
    size_t i, j;

    for (i = 0; i < br->n_ports; i++) {
        struct port *port = br->ports[i];
        if (port->n_ifaces < 2) {
            continue;
        }
        for (j = 0; j < port->n_ifaces; j++) {
            struct iface *iface = port->ifaces[j];
            if (iface->delay_expires != LLONG_MAX) {
                poll_timer_wait(iface->delay_expires - time_msec());
            }
        }
    }
}

static bool
set_dst(struct dst *p, const flow_t *flow,
        const struct port *in_port, const struct port *out_port,
        tag_type *tags)
{
    p->vlan = (out_port->vlan >= 0 ? OFP_VLAN_NONE
              : in_port->vlan >= 0 ? in_port->vlan
              : ntohs(flow->dl_vlan));
    return choose_output_iface(out_port, flow->dl_src, &p->dp_ifidx, tags);
}

static void
swap_dst(struct dst *p, struct dst *q)
{
    struct dst tmp = *p;
    *p = *q;
    *q = tmp;
}

/* Moves all the dsts with vlan == 'vlan' to the front of the 'n_dsts' in
 * 'dsts'.  (This may help performance by reducing the number of VLAN changes
 * that we push to the datapath.  We could in fact fully sort the array by
 * vlan, but in most cases there are at most two different vlan tags so that's
 * possibly overkill.) */
static void
partition_dsts(struct dst *dsts, size_t n_dsts, int vlan)
{
    struct dst *first = dsts;
    struct dst *last = dsts + n_dsts;

    while (first != last) {
        /* Invariants:
         *      - All dsts < first have vlan == 'vlan'.
         *      - All dsts >= last have vlan != 'vlan'.
         *      - first < last. */
        while (first->vlan == vlan) {
            if (++first == last) {
                return;
            }
        }

        /* Same invariants, plus one additional:
         *      - first->vlan != vlan.
         */
        while (last[-1].vlan != vlan) {
            if (--last == first) {
                return;
            }
        }

        /* Same invariants, plus one additional:
         *      - last[-1].vlan == vlan.*/
        swap_dst(first++, --last);
    }
}

static int
mirror_mask_ffs(mirror_mask_t mask)
{
    BUILD_ASSERT_DECL(sizeof(unsigned int) >= sizeof(mask));
    return ffs(mask);
}

static bool
dst_is_duplicate(const struct dst *dsts, size_t n_dsts,
                 const struct dst *test)
{
    size_t i;
    for (i = 0; i < n_dsts; i++) {
        if (dsts[i].vlan == test->vlan && dsts[i].dp_ifidx == test->dp_ifidx) {
            return true;
        }
    }
    return false;
}

static bool
port_trunks_vlan(const struct port *port, uint16_t vlan)
{
    return port->vlan < 0 && bitmap_is_set(port->trunks, vlan);
}

static bool
port_includes_vlan(const struct port *port, uint16_t vlan)
{
    return vlan == port->vlan || port_trunks_vlan(port, vlan);
}

static size_t
compose_dsts(const struct bridge *br, const flow_t *flow, uint16_t vlan,
             const struct port *in_port, const struct port *out_port,
             struct dst dsts[], tag_type *tags, uint16_t *nf_output_iface)
{
    mirror_mask_t mirrors = in_port->src_mirrors;
    struct dst *dst = dsts;
    size_t i;

    if (out_port == FLOOD_PORT) {
        /* XXX use ODP_FLOOD if no vlans or bonding. */
        /* XXX even better, define each VLAN as a datapath port group */
        for (i = 0; i < br->n_ports; i++) {
            struct port *port = br->ports[i];
            if (port != in_port && port_includes_vlan(port, vlan)
                && !port->is_mirror_output_port
                && set_dst(dst, flow, in_port, port, tags)) {
                mirrors |= port->dst_mirrors;
                dst++;
            }
        }
        *nf_output_iface = NF_OUT_FLOOD;
    } else if (out_port && set_dst(dst, flow, in_port, out_port, tags)) {
        *nf_output_iface = dst->dp_ifidx;
        mirrors |= out_port->dst_mirrors;
        dst++;
    }

    while (mirrors) {
        struct mirror *m = br->mirrors[mirror_mask_ffs(mirrors) - 1];
        if (!m->n_vlans || vlan_is_mirrored(m, vlan)) {
            if (m->out_port) {
                if (set_dst(dst, flow, in_port, m->out_port, tags)
                    && !dst_is_duplicate(dsts, dst - dsts, dst)) {
                    dst++;
                }
            } else {
                for (i = 0; i < br->n_ports; i++) {
                    struct port *port = br->ports[i];
                    if (port_includes_vlan(port, m->out_vlan)
                        && set_dst(dst, flow, in_port, port, tags))
                    {
                        int flow_vlan;

                        if (port->vlan < 0) {
                            dst->vlan = m->out_vlan;
                        }
                        if (dst_is_duplicate(dsts, dst - dsts, dst)) {
                            continue;
                        }

                        /* Use the vlan tag on the original flow instead of
                         * the one passed in the vlan parameter.  This ensures
                         * that we compare the vlan from before any implicit
                         * tagging tags place. This is necessary because
                         * dst->vlan is the final vlan, after removing implicit
                         * tags. */
                        flow_vlan = ntohs(flow->dl_vlan);
                        if (flow_vlan == 0) {
                            flow_vlan = OFP_VLAN_NONE;
                        }
                        if (port == in_port && dst->vlan == flow_vlan) {
                            /* Don't send out input port on same VLAN. */
                            continue;
                        }
                        dst++;
                    }
                }
            }
        }
        mirrors &= mirrors - 1;
    }

    partition_dsts(dsts, dst - dsts, ntohs(flow->dl_vlan));
    return dst - dsts;
}

static void UNUSED
print_dsts(const struct dst *dsts, size_t n)
{
    for (; n--; dsts++) {
        printf(">p%"PRIu16, dsts->dp_ifidx);
        if (dsts->vlan != OFP_VLAN_NONE) {
            printf("v%"PRIu16, dsts->vlan);
        }
    }
}

static void
compose_actions(struct bridge *br, const flow_t *flow, uint16_t vlan,
                const struct port *in_port, const struct port *out_port,
                tag_type *tags, struct odp_actions *actions,
                uint16_t *nf_output_iface)
{
    struct dst dsts[DP_MAX_PORTS * (MAX_MIRRORS + 1)];
    size_t n_dsts;
    const struct dst *p;
    uint16_t cur_vlan;

    n_dsts = compose_dsts(br, flow, vlan, in_port, out_port, dsts, tags,
                          nf_output_iface);

    cur_vlan = ntohs(flow->dl_vlan);
    for (p = dsts; p < &dsts[n_dsts]; p++) {
        union odp_action *a;
        if (p->vlan != cur_vlan) {
            if (p->vlan == OFP_VLAN_NONE) {
                odp_actions_add(actions, ODPAT_STRIP_VLAN);
            } else {
                a = odp_actions_add(actions, ODPAT_SET_VLAN_VID);
                a->vlan_vid.vlan_vid = htons(p->vlan);
            }
            cur_vlan = p->vlan;
        }
        a = odp_actions_add(actions, ODPAT_OUTPUT);
        a->output.port = p->dp_ifidx;
    }
}

/* Returns the effective vlan of a packet, taking into account both the
 * 802.1Q header and implicitly tagged ports.  A value of 0 indicates that
 * the packet is untagged and -1 indicates it has an invalid header and
 * should be dropped. */
static int flow_get_vlan(struct bridge *br, const flow_t *flow,
                         struct port *in_port, bool have_packet)
{
    /* Note that dl_vlan of 0 and of OFP_VLAN_NONE both mean that the packet
     * belongs to VLAN 0, so we should treat both cases identically.  (In the
     * former case, the packet has an 802.1Q header that specifies VLAN 0,
     * presumably to allow a priority to be specified.  In the latter case, the
     * packet does not have any 802.1Q header.) */
    int vlan = ntohs(flow->dl_vlan);
    if (vlan == OFP_VLAN_NONE) {
        vlan = 0;
    }
    if (in_port->vlan >= 0) {
        if (vlan) {
            /* XXX support double tagging? */
            if (have_packet) {
                static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 5);
                VLOG_WARN_RL(&rl, "bridge %s: dropping VLAN %"PRIu16" tagged "
                             "packet received on port %s configured with "
                             "implicit VLAN %"PRIu16,
                             br->name, ntohs(flow->dl_vlan),
                             in_port->name, in_port->vlan);
            }
            return -1;
        }
        vlan = in_port->vlan;
    } else {
        if (!port_includes_vlan(in_port, vlan)) {
            if (have_packet) {
                static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 5);
                VLOG_WARN_RL(&rl, "bridge %s: dropping VLAN %d tagged "
                             "packet received on port %s not configured for "
                             "trunking VLAN %d",
                             br->name, vlan, in_port->name, vlan);
            }
            return -1;
        }
    }

    return vlan;
}

static void
update_learning_table(struct bridge *br, const flow_t *flow, int vlan,
                      struct port *in_port)
{
    tag_type rev_tag = mac_learning_learn(br->ml, flow->dl_src,
                                          vlan, in_port->port_idx);
    if (rev_tag) {
        /* The log messages here could actually be useful in debugging,
         * so keep the rate limit relatively high. */
        static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(30,
                                                                300);
        VLOG_DBG_RL(&rl, "bridge %s: learned that "ETH_ADDR_FMT" is "
                    "on port %s in VLAN %d",
                    br->name, ETH_ADDR_ARGS(flow->dl_src),
                    in_port->name, vlan);
        ofproto_revalidate(br->ofproto, rev_tag);
    }
}

static bool
is_bcast_arp_reply(const flow_t *flow)
{
    return (flow->dl_type == htons(ETH_TYPE_ARP)
            && flow->nw_proto == ARP_OP_REPLY
            && eth_addr_is_broadcast(flow->dl_dst));
}

/* If the composed actions may be applied to any packet in the given 'flow',
 * returns true.  Otherwise, the actions should only be applied to 'packet', or
 * not at all, if 'packet' was NULL. */
static bool
process_flow(struct bridge *br, const flow_t *flow,
             const struct ofpbuf *packet, struct odp_actions *actions,
             tag_type *tags, uint16_t *nf_output_iface)
{
    struct iface *in_iface;
    struct port *in_port;
    struct port *out_port = NULL; /* By default, drop the packet/flow. */
    int vlan;
    int out_port_idx;

    /* Find the interface and port structure for the received packet. */
    in_iface = iface_from_dp_ifidx(br, flow->in_port);
    if (!in_iface) {
        /* No interface?  Something fishy... */
        if (packet != NULL) {
            /* Odd.  A few possible reasons here:
             *
             * - We deleted an interface but there are still a few packets
             *   queued up from it.
             *
             * - Someone externally added an interface (e.g. with "ovs-dpctl
             *   add-if") that we don't know about.
             *
             * - Packet arrived on the local port but the local port is not
             *   one of our bridge ports.
             */
            static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 5);

            VLOG_WARN_RL(&rl, "bridge %s: received packet on unknown "
                         "interface %"PRIu16, br->name, flow->in_port); 
        }

        /* Return without adding any actions, to drop packets on this flow. */
        return true;
    }
    in_port = in_iface->port;
    vlan = flow_get_vlan(br, flow, in_port, !!packet);
    if (vlan < 0) {
        goto done;
    }

    /* Drop frames for reserved multicast addresses. */
    if (eth_addr_is_reserved(flow->dl_dst)) {
        goto done;
    }

    /* Drop frames on ports reserved for mirroring. */
    if (in_port->is_mirror_output_port) {
        static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 5);
        VLOG_WARN_RL(&rl, "bridge %s: dropping packet received on port %s, "
                     "which is reserved exclusively for mirroring",
                     br->name, in_port->name);
        goto done;
    }

    /* Packets received on bonds need special attention to avoid duplicates. */
    if (in_port->n_ifaces > 1) {
        int src_idx;

        if (eth_addr_is_multicast(flow->dl_dst)) {
            *tags |= in_port->active_iface_tag;
            if (in_port->active_iface != in_iface->port_ifidx) {
                /* Drop all multicast packets on inactive slaves. */
                goto done;
            }
        }

        /* Drop all packets for which we have learned a different input
         * port, because we probably sent the packet on one slave and got
         * it back on the other.  Broadcast ARP replies are an exception
         * to this rule: the host has moved to another switch. */
        src_idx = mac_learning_lookup(br->ml, flow->dl_src, vlan);
        if (src_idx != -1 && src_idx != in_port->port_idx &&
            !is_bcast_arp_reply(flow)) {
                goto done;
        }
    }

    /* MAC learning. */
    out_port = FLOOD_PORT;
    /* Learn source MAC (but don't try to learn from revalidation). */
    if (packet) {
        update_learning_table(br, flow, vlan, in_port);
    }

    /* Determine output port. */
    out_port_idx = mac_learning_lookup_tag(br->ml, flow->dl_dst, vlan,
                                           tags);
    if (out_port_idx >= 0 && out_port_idx < br->n_ports) {
        out_port = br->ports[out_port_idx];
    } else if (!packet && !eth_addr_is_multicast(flow->dl_dst)) {
        /* If we are revalidating but don't have a learning entry then
         * eject the flow.  Installing a flow that floods packets opens
         * up a window of time where we could learn from a packet reflected
         * on a bond and blackhole packets before the learning table is
         * updated to reflect the correct port. */
        return false;
    }

    /* Don't send packets out their input ports. */
    if (in_port == out_port) {
        out_port = NULL;
    }

done:
    compose_actions(br, flow, vlan, in_port, out_port, tags, actions,
                    nf_output_iface);

    return true;
}

/* Careful: 'opp' is in host byte order and opp->port_no is an OFP port
 * number. */
static void
bridge_port_changed_ofhook_cb(enum ofp_port_reason reason,
                              const struct ofp_phy_port *opp,
                              void *br_)
{
    struct bridge *br = br_;
    struct iface *iface;
    struct port *port;

    iface = iface_from_dp_ifidx(br, ofp_port_to_odp_port(opp->port_no));
    if (!iface) {
        return;
    }
    port = iface->port;

    if (reason == OFPPR_DELETE) {
        VLOG_WARN("bridge %s: interface %s deleted unexpectedly",
                  br->name, iface->name);
        iface_destroy(iface);
        if (!port->n_ifaces) {
            VLOG_WARN("bridge %s: port %s has no interfaces, dropping",
                      br->name, port->name);
            port_destroy(port);
        }

        bridge_flush(br);
    } else {
        if (port->n_ifaces > 1) {
            bool up = !(opp->state & OFPPS_LINK_DOWN);
            bond_link_status_update(iface, up);
            port_update_bond_compat(port);
        }
    }
}

static bool
bridge_normal_ofhook_cb(const flow_t *flow, const struct ofpbuf *packet,
                        struct odp_actions *actions, tag_type *tags,
                        uint16_t *nf_output_iface, void *br_)
{
    struct bridge *br = br_;

    COVERAGE_INC(bridge_process_flow);
    return process_flow(br, flow, packet, actions, tags, nf_output_iface);
}

static void
bridge_account_flow_ofhook_cb(const flow_t *flow,
                              const union odp_action *actions,
                              size_t n_actions, unsigned long long int n_bytes,
                              void *br_)
{
    struct bridge *br = br_;
    struct port *in_port;
    const union odp_action *a;

    /* Feed information from the active flows back into the learning table
     * to ensure that table is always in sync with what is actually flowing
     * through the datapath. */
    in_port = port_from_dp_ifidx(br, flow->in_port);
    if (in_port) {
        int vlan = flow_get_vlan(br, flow, in_port, false);
         if (vlan >= 0) {
            update_learning_table(br, flow, vlan, in_port);
        }
    }

    if (!br->has_bonded_ports) {
        return;
    }

    for (a = actions; a < &actions[n_actions]; a++) {
        if (a->type == ODPAT_OUTPUT) {
            struct port *out_port = port_from_dp_ifidx(br, a->output.port);
            if (out_port && out_port->n_ifaces >= 2) {
                struct bond_entry *e = lookup_bond_entry(out_port,
                                                         flow->dl_src);
                e->tx_bytes += n_bytes;
            }
        }
    }
}

static void
bridge_account_checkpoint_ofhook_cb(void *br_)
{
    struct bridge *br = br_;
    size_t i;

    if (!br->has_bonded_ports) {
        return;
    }

    /* The current ofproto implementation calls this callback at least once a
     * second, so this timer implementation is sufficient. */
    if (time_msec() < br->bond_next_rebalance) {
        return;
    }
    br->bond_next_rebalance = time_msec() + 10000;

    for (i = 0; i < br->n_ports; i++) {
        struct port *port = br->ports[i];
        if (port->n_ifaces > 1) {
            bond_rebalance_port(port);
        }
    }
}

static struct ofhooks bridge_ofhooks = {
    bridge_port_changed_ofhook_cb,
    bridge_normal_ofhook_cb,
    bridge_account_flow_ofhook_cb,
    bridge_account_checkpoint_ofhook_cb,
};

/* Bonding functions. */

/* Statistics for a single interface on a bonded port, used for load-based
 * bond rebalancing.  */
struct slave_balance {
    struct iface *iface;        /* The interface. */
    uint64_t tx_bytes;          /* Sum of hashes[*]->tx_bytes. */

    /* All the "bond_entry"s that are assigned to this interface, in order of
     * increasing tx_bytes. */
    struct bond_entry **hashes;
    size_t n_hashes;
};

/* Sorts pointers to pointers to bond_entries in ascending order by the
 * interface to which they are assigned, and within a single interface in
 * ascending order of bytes transmitted. */
static int
compare_bond_entries(const void *a_, const void *b_)
{
    const struct bond_entry *const *ap = a_;
    const struct bond_entry *const *bp = b_;
    const struct bond_entry *a = *ap;
    const struct bond_entry *b = *bp;
    if (a->iface_idx != b->iface_idx) {
        return a->iface_idx > b->iface_idx ? 1 : -1;
    } else if (a->tx_bytes != b->tx_bytes) {
        return a->tx_bytes > b->tx_bytes ? 1 : -1;
    } else {
        return 0;
    }
}

/* Sorts slave_balances so that enabled ports come first, and otherwise in
 * *descending* order by number of bytes transmitted. */
static int
compare_slave_balance(const void *a_, const void *b_)
{
    const struct slave_balance *a = a_;
    const struct slave_balance *b = b_;
    if (a->iface->enabled != b->iface->enabled) {
        return a->iface->enabled ? -1 : 1;
    } else if (a->tx_bytes != b->tx_bytes) {
        return a->tx_bytes > b->tx_bytes ? -1 : 1;
    } else {
        return 0;
    }
}

static void
swap_bals(struct slave_balance *a, struct slave_balance *b)
{
    struct slave_balance tmp = *a;
    *a = *b;
    *b = tmp;
}

/* Restores the 'n_bals' slave_balance structures in 'bals' to sorted order
 * given that 'p' (and only 'p') might be in the wrong location.
 *
 * This function invalidates 'p', since it might now be in a different memory
 * location. */
static void
resort_bals(struct slave_balance *p,
            struct slave_balance bals[], size_t n_bals)
{
    if (n_bals > 1) {
        for (; p > bals && p->tx_bytes > p[-1].tx_bytes; p--) {
            swap_bals(p, p - 1);
        }
        for (; p < &bals[n_bals - 1] && p->tx_bytes < p[1].tx_bytes; p++) {
            swap_bals(p, p + 1);
        }
    }
}

static void
log_bals(const struct slave_balance *bals, size_t n_bals, struct port *port)
{
    if (VLOG_IS_DBG_ENABLED()) {
        struct ds ds = DS_EMPTY_INITIALIZER;
        const struct slave_balance *b;

        for (b = bals; b < bals + n_bals; b++) {
            size_t i;

            if (b > bals) {
                ds_put_char(&ds, ',');
            }
            ds_put_format(&ds, " %s %"PRIu64"kB",
                          b->iface->name, b->tx_bytes / 1024);

            if (!b->iface->enabled) {
                ds_put_cstr(&ds, " (disabled)");
            }
            if (b->n_hashes > 0) {
                ds_put_cstr(&ds, " (");
                for (i = 0; i < b->n_hashes; i++) {
                    const struct bond_entry *e = b->hashes[i];
                    if (i > 0) {
                        ds_put_cstr(&ds, " + ");
                    }
                    ds_put_format(&ds, "h%td: %"PRIu64"kB",
                                  e - port->bond_hash, e->tx_bytes / 1024);
                }
                ds_put_cstr(&ds, ")");
            }
        }
        VLOG_DBG("bond %s:%s", port->name, ds_cstr(&ds));
        ds_destroy(&ds);
    }
}

/* Shifts 'hash' from 'from' to 'to' within 'port'. */
static void
bond_shift_load(struct slave_balance *from, struct slave_balance *to,
                int hash_idx)
{
    struct bond_entry *hash = from->hashes[hash_idx];
    struct port *port = from->iface->port;
    uint64_t delta = hash->tx_bytes;

    VLOG_INFO("bond %s: shift %"PRIu64"kB of load (with hash %td) "
              "from %s to %s (now carrying %"PRIu64"kB and "
              "%"PRIu64"kB load, respectively)",
              port->name, delta / 1024, hash - port->bond_hash,
              from->iface->name, to->iface->name,
              (from->tx_bytes - delta) / 1024,
              (to->tx_bytes + delta) / 1024);

    /* Delete element from from->hashes.
     *
     * We don't bother to add the element to to->hashes because not only would
     * it require more work, the only purpose it would be to allow that hash to
     * be migrated to another slave in this rebalancing run, and there is no
     * point in doing that.  */
    if (hash_idx == 0) {
        from->hashes++;
    } else {
        memmove(from->hashes + hash_idx, from->hashes + hash_idx + 1,
                (from->n_hashes - (hash_idx + 1)) * sizeof *from->hashes);
    }
    from->n_hashes--;

    /* Shift load away from 'from' to 'to'. */
    from->tx_bytes -= delta;
    to->tx_bytes += delta;

    /* Arrange for flows to be revalidated. */
    ofproto_revalidate(port->bridge->ofproto, hash->iface_tag);
    hash->iface_idx = to->iface->port_ifidx;
    hash->iface_tag = tag_create_random();
}

static void
bond_rebalance_port(struct port *port)
{
    struct slave_balance bals[DP_MAX_PORTS];
    size_t n_bals;
    struct bond_entry *hashes[BOND_MASK + 1];
    struct slave_balance *b, *from, *to;
    struct bond_entry *e;
    size_t i;

    /* Sets up 'bals' to describe each of the port's interfaces, sorted in
     * descending order of tx_bytes, so that bals[0] represents the most
     * heavily loaded slave and bals[n_bals - 1] represents the least heavily
     * loaded slave.
     *
     * The code is a bit tricky: to avoid dynamically allocating a 'hashes'
     * array for each slave_balance structure, we sort our local array of
     * hashes in order by slave, so that all of the hashes for a given slave
     * become contiguous in memory, and then we point each 'hashes' members of
     * a slave_balance structure to the start of a contiguous group. */
    n_bals = port->n_ifaces;
    for (b = bals; b < &bals[n_bals]; b++) {
        b->iface = port->ifaces[b - bals];
        b->tx_bytes = 0;
        b->hashes = NULL;
        b->n_hashes = 0;
    }
    for (i = 0; i <= BOND_MASK; i++) {
        hashes[i] = &port->bond_hash[i];
    }
    qsort(hashes, BOND_MASK + 1, sizeof *hashes, compare_bond_entries);
    for (i = 0; i <= BOND_MASK; i++) {
        e = hashes[i];
        if (e->iface_idx >= 0 && e->iface_idx < port->n_ifaces) {
            b = &bals[e->iface_idx];
            b->tx_bytes += e->tx_bytes;
            if (!b->hashes) {
                b->hashes = &hashes[i];
            }
            b->n_hashes++;
        }
    }
    qsort(bals, n_bals, sizeof *bals, compare_slave_balance);
    log_bals(bals, n_bals, port);

    /* Discard slaves that aren't enabled (which were sorted to the back of the
     * array earlier). */
    while (!bals[n_bals - 1].iface->enabled) {
        n_bals--;
        if (!n_bals) {
            return;
        }
    }

    /* Shift load from the most-loaded slaves to the least-loaded slaves. */
    to = &bals[n_bals - 1];
    for (from = bals; from < to; ) {
        uint64_t overload = from->tx_bytes - to->tx_bytes;
        if (overload < to->tx_bytes >> 5 || overload < 100000) {
            /* The extra load on 'from' (and all less-loaded slaves), compared
             * to that of 'to' (the least-loaded slave), is less than ~3%, or
             * it is less than ~1Mbps.  No point in rebalancing. */
            break;
        } else if (from->n_hashes == 1) {
            /* 'from' only carries a single MAC hash, so we can't shift any
             * load away from it, even though we want to. */
            from++;
        } else {
            /* 'from' is carrying significantly more load than 'to', and that
             * load is split across at least two different hashes.  Pick a hash
             * to migrate to 'to' (the least-loaded slave), given that doing so
             * must decrease the ratio of the load on the two slaves by at
             * least 0.1.
             *
             * The sort order we use means that we prefer to shift away the
             * smallest hashes instead of the biggest ones.  There is little
             * reason behind this decision; we could use the opposite sort
             * order to shift away big hashes ahead of small ones. */
            size_t i;
            bool order_swapped;

            for (i = 0; i < from->n_hashes; i++) {
                double old_ratio, new_ratio;
                uint64_t delta = from->hashes[i]->tx_bytes;

                if (delta == 0 || from->tx_bytes - delta == 0) {
                    /* Pointless move. */
                    continue;
                }

                order_swapped = from->tx_bytes - delta < to->tx_bytes + delta;

                if (to->tx_bytes == 0) {
                    /* Nothing on the new slave, move it. */
                    break;
                }

                old_ratio = (double)from->tx_bytes / to->tx_bytes;
                new_ratio = (double)(from->tx_bytes - delta) /
                            (to->tx_bytes + delta);

                if (new_ratio == 0) {
                    /* Should already be covered but check to prevent division
                     * by zero. */
                    continue;
                }

                if (new_ratio < 1) {
                    new_ratio = 1 / new_ratio;
                }

                if (old_ratio - new_ratio > 0.1) {
                    /* Would decrease the ratio, move it. */
                    break;
                }
            }
            if (i < from->n_hashes) {
                bond_shift_load(from, to, i);
                port->bond_compat_is_stale = true;

                /* If the result of the migration changed the relative order of
                 * 'from' and 'to' swap them back to maintain invariants. */
                if (order_swapped) {
                    swap_bals(from, to);
                }

                /* Re-sort 'bals'.  Note that this may make 'from' and 'to'
                 * point to different slave_balance structures.  It is only
                 * valid to do these two operations in a row at all because we
                 * know that 'from' will not move past 'to' and vice versa. */
                resort_bals(from, bals, n_bals);
                resort_bals(to, bals, n_bals);
            } else {
                from++;
            }
        }
    }

    /* Implement exponentially weighted moving average.  A weight of 1/2 causes
     * historical data to decay to <1% in 7 rebalancing runs.  */
    for (e = &port->bond_hash[0]; e <= &port->bond_hash[BOND_MASK]; e++) {
        e->tx_bytes /= 2;
    }
}

static void
bond_send_learning_packets(struct port *port)
{
    struct bridge *br = port->bridge;
    struct mac_entry *e;
    struct ofpbuf packet;
    int error, n_packets, n_errors;

    if (!port->n_ifaces || port->active_iface < 0) {
        return;
    }

    ofpbuf_init(&packet, 128);
    error = n_packets = n_errors = 0;
    LIST_FOR_EACH (e, struct mac_entry, lru_node, &br->ml->lrus) {
        union ofp_action actions[2], *a;
        uint16_t dp_ifidx;
        tag_type tags = 0;
        flow_t flow;
        int retval;

        if (e->port == port->port_idx
            || !choose_output_iface(port, e->mac, &dp_ifidx, &tags)) {
            continue;
        }

        /* Compose actions. */
        memset(actions, 0, sizeof actions);
        a = actions;
        if (e->vlan) {
            a->vlan_vid.type = htons(OFPAT_SET_VLAN_VID);
            a->vlan_vid.len = htons(sizeof *a);
            a->vlan_vid.vlan_vid = htons(e->vlan);
            a++;
        }
        a->output.type = htons(OFPAT_OUTPUT);
        a->output.len = htons(sizeof *a);
        a->output.port = htons(odp_port_to_ofp_port(dp_ifidx));
        a++;

        /* Send packet. */
        n_packets++;
        compose_benign_packet(&packet, "Open vSwitch Bond Failover", 0xf177,
                              e->mac);
        flow_extract(&packet, ODPP_NONE, &flow);
        retval = ofproto_send_packet(br->ofproto, &flow, actions, a - actions,
                                     &packet);
        if (retval) {
            error = retval;
            n_errors++;
        }
    }
    ofpbuf_uninit(&packet);

    if (n_errors) {
        static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 5);
        VLOG_WARN_RL(&rl, "bond %s: %d errors sending %d gratuitous learning "
                     "packets, last error was: %s",
                     port->name, n_errors, n_packets, strerror(error));
    } else {
        VLOG_DBG("bond %s: sent %d gratuitous learning packets",
                 port->name, n_packets);
    }
}

/* Bonding unixctl user interface functions. */

static void
bond_unixctl_list(struct unixctl_conn *conn,
                  const char *args UNUSED, void *aux UNUSED)
{
    struct ds ds = DS_EMPTY_INITIALIZER;
    const struct bridge *br;

    ds_put_cstr(&ds, "bridge\tbond\tslaves\n");

    LIST_FOR_EACH (br, struct bridge, node, &all_bridges) {
        size_t i;

        for (i = 0; i < br->n_ports; i++) {
            const struct port *port = br->ports[i];
            if (port->n_ifaces > 1) {
                size_t j;

                ds_put_format(&ds, "%s\t%s\t", br->name, port->name);
                for (j = 0; j < port->n_ifaces; j++) {
                    const struct iface *iface = port->ifaces[j];
                    if (j) {
                        ds_put_cstr(&ds, ", ");
                    }
                    ds_put_cstr(&ds, iface->name);
                }
                ds_put_char(&ds, '\n');
            }
        }
    }
    unixctl_command_reply(conn, 200, ds_cstr(&ds));
    ds_destroy(&ds);
}

static struct port *
bond_find(const char *name)
{
    const struct bridge *br;

    LIST_FOR_EACH (br, struct bridge, node, &all_bridges) {
        size_t i;

        for (i = 0; i < br->n_ports; i++) {
            struct port *port = br->ports[i];
            if (!strcmp(port->name, name) && port->n_ifaces > 1) {
                return port;
            }
        }
    }
    return NULL;
}

static void
bond_unixctl_show(struct unixctl_conn *conn,
                  const char *args, void *aux UNUSED)
{
    struct ds ds = DS_EMPTY_INITIALIZER;
    const struct port *port;
    size_t j;

    port = bond_find(args);
    if (!port) {
        unixctl_command_reply(conn, 501, "no such bond");
        return;
    }

    ds_put_format(&ds, "updelay: %d ms\n", port->updelay);
    ds_put_format(&ds, "downdelay: %d ms\n", port->downdelay);
    ds_put_format(&ds, "next rebalance: %lld ms\n",
                  port->bridge->bond_next_rebalance - time_msec());
    for (j = 0; j < port->n_ifaces; j++) {
        const struct iface *iface = port->ifaces[j];
        struct bond_entry *be;

        /* Basic info. */
        ds_put_format(&ds, "slave %s: %s\n",
                      iface->name, iface->enabled ? "enabled" : "disabled");
        if (j == port->active_iface) {
            ds_put_cstr(&ds, "\tactive slave\n");
        }
        if (iface->delay_expires != LLONG_MAX) {
            ds_put_format(&ds, "\t%s expires in %lld ms\n",
                          iface->enabled ? "downdelay" : "updelay",
                          iface->delay_expires - time_msec());
        }

        /* Hashes. */
        for (be = port->bond_hash; be <= &port->bond_hash[BOND_MASK]; be++) {
            int hash = be - port->bond_hash;
            struct mac_entry *me;

            if (be->iface_idx != j) {
                continue;
            }

            ds_put_format(&ds, "\thash %d: %"PRIu64" kB load\n",
                          hash, be->tx_bytes / 1024);

            /* MACs. */
            LIST_FOR_EACH (me, struct mac_entry, lru_node,
                           &port->bridge->ml->lrus) {
                uint16_t dp_ifidx;
                tag_type tags = 0;
                if (bond_hash(me->mac) == hash
                    && me->port != port->port_idx
                    && choose_output_iface(port, me->mac, &dp_ifidx, &tags)
                    && dp_ifidx == iface->dp_ifidx)
                {
                    ds_put_format(&ds, "\t\t"ETH_ADDR_FMT"\n",
                                  ETH_ADDR_ARGS(me->mac));
                }
            }
        }
    }
    unixctl_command_reply(conn, 200, ds_cstr(&ds));
    ds_destroy(&ds);
}

static void
bond_unixctl_migrate(struct unixctl_conn *conn, const char *args_,
                     void *aux UNUSED)
{
    char *args = (char *) args_;
    char *save_ptr = NULL;
    char *bond_s, *hash_s, *slave_s;
    uint8_t mac[ETH_ADDR_LEN];
    struct port *port;
    struct iface *iface;
    struct bond_entry *entry;
    int hash;

    bond_s = strtok_r(args, " ", &save_ptr);
    hash_s = strtok_r(NULL, " ", &save_ptr);
    slave_s = strtok_r(NULL, " ", &save_ptr);
    if (!slave_s) {
        unixctl_command_reply(conn, 501,
                              "usage: bond/migrate BOND HASH SLAVE");
        return;
    }

    port = bond_find(bond_s);
    if (!port) {
        unixctl_command_reply(conn, 501, "no such bond");
        return;
    }

    if (sscanf(hash_s, ETH_ADDR_SCAN_FMT, ETH_ADDR_SCAN_ARGS(mac))
        == ETH_ADDR_SCAN_COUNT) {
        hash = bond_hash(mac);
    } else if (strspn(hash_s, "0123456789") == strlen(hash_s)) {
        hash = atoi(hash_s) & BOND_MASK;
    } else {
        unixctl_command_reply(conn, 501, "bad hash");
        return;
    }

    iface = port_lookup_iface(port, slave_s);
    if (!iface) {
        unixctl_command_reply(conn, 501, "no such slave");
        return;
    }

    if (!iface->enabled) {
        unixctl_command_reply(conn, 501, "cannot migrate to disabled slave");
        return;
    }

    entry = &port->bond_hash[hash];
    ofproto_revalidate(port->bridge->ofproto, entry->iface_tag);
    entry->iface_idx = iface->port_ifidx;
    entry->iface_tag = tag_create_random();
    port->bond_compat_is_stale = true;
    unixctl_command_reply(conn, 200, "migrated");
}

static void
bond_unixctl_set_active_slave(struct unixctl_conn *conn, const char *args_,
                              void *aux UNUSED)
{
    char *args = (char *) args_;
    char *save_ptr = NULL;
    char *bond_s, *slave_s;
    struct port *port;
    struct iface *iface;

    bond_s = strtok_r(args, " ", &save_ptr);
    slave_s = strtok_r(NULL, " ", &save_ptr);
    if (!slave_s) {
        unixctl_command_reply(conn, 501,
                              "usage: bond/set-active-slave BOND SLAVE");
        return;
    }

    port = bond_find(bond_s);
    if (!port) {
        unixctl_command_reply(conn, 501, "no such bond");
        return;
    }

    iface = port_lookup_iface(port, slave_s);
    if (!iface) {
        unixctl_command_reply(conn, 501, "no such slave");
        return;
    }

    if (!iface->enabled) {
        unixctl_command_reply(conn, 501, "cannot make disabled slave active");
        return;
    }

    if (port->active_iface != iface->port_ifidx) {
        ofproto_revalidate(port->bridge->ofproto, port->active_iface_tag);
        port->active_iface = iface->port_ifidx;
        port->active_iface_tag = tag_create_random();
        VLOG_INFO("port %s: active interface is now %s",
                  port->name, iface->name);
        bond_send_learning_packets(port);
        unixctl_command_reply(conn, 200, "done");
    } else {
        unixctl_command_reply(conn, 200, "no change");
    }
}

static void
enable_slave(struct unixctl_conn *conn, const char *args_, bool enable)
{
    char *args = (char *) args_;
    char *save_ptr = NULL;
    char *bond_s, *slave_s;
    struct port *port;
    struct iface *iface;

    bond_s = strtok_r(args, " ", &save_ptr);
    slave_s = strtok_r(NULL, " ", &save_ptr);
    if (!slave_s) {
        unixctl_command_reply(conn, 501,
                              "usage: bond/enable/disable-slave BOND SLAVE");
        return;
    }

    port = bond_find(bond_s);
    if (!port) {
        unixctl_command_reply(conn, 501, "no such bond");
        return;
    }

    iface = port_lookup_iface(port, slave_s);
    if (!iface) {
        unixctl_command_reply(conn, 501, "no such slave");
        return;
    }

    bond_enable_slave(iface, enable);
    unixctl_command_reply(conn, 501, enable ? "enabled" : "disabled");
}

static void
bond_unixctl_enable_slave(struct unixctl_conn *conn, const char *args,
                          void *aux UNUSED)
{
    enable_slave(conn, args, true);
}

static void
bond_unixctl_disable_slave(struct unixctl_conn *conn, const char *args,
                           void *aux UNUSED)
{
    enable_slave(conn, args, false);
}

static void
bond_unixctl_hash(struct unixctl_conn *conn, const char *args,
                  void *aux UNUSED)
{
	uint8_t mac[ETH_ADDR_LEN];
	uint8_t hash;
	char *hash_cstr;

	if (sscanf(args, ETH_ADDR_SCAN_FMT, ETH_ADDR_SCAN_ARGS(mac))
	    == ETH_ADDR_SCAN_COUNT) {
		hash = bond_hash(mac);

		hash_cstr = xasprintf("%u", hash);
		unixctl_command_reply(conn, 200, hash_cstr);
		free(hash_cstr);
	} else {
		unixctl_command_reply(conn, 501, "invalid mac");
	}
}

static void
bond_init(void)
{
    unixctl_command_register("bond/list", bond_unixctl_list, NULL);
    unixctl_command_register("bond/show", bond_unixctl_show, NULL);
    unixctl_command_register("bond/migrate", bond_unixctl_migrate, NULL);
    unixctl_command_register("bond/set-active-slave",
                             bond_unixctl_set_active_slave, NULL);
    unixctl_command_register("bond/enable-slave", bond_unixctl_enable_slave,
                             NULL);
    unixctl_command_register("bond/disable-slave", bond_unixctl_disable_slave,
                             NULL);
    unixctl_command_register("bond/hash", bond_unixctl_hash, NULL);
}

/* Port functions. */

static struct port *
port_create(struct bridge *br, const char *name)
{
    struct port *port;

    port = xzalloc(sizeof *port);
    port->bridge = br;
    port->port_idx = br->n_ports;
    port->vlan = -1;
    port->trunks = NULL;
    port->name = xstrdup(name);
    port->active_iface = -1;

    if (br->n_ports >= br->allocated_ports) {
        br->ports = x2nrealloc(br->ports, &br->allocated_ports,
                               sizeof *br->ports);
    }
    br->ports[br->n_ports++] = port;

    VLOG_INFO("created port %s on bridge %s", port->name, br->name);
    bridge_flush(br);

    return port;
}

static void
port_reconfigure(struct port *port, const struct ovsrec_port *cfg)
{
    struct shash old_ifaces, new_ifaces;
    struct shash_node *node;
    unsigned long *trunks;
    int vlan;
    size_t i;

    port->cfg = cfg;

    /* Collect old and new interfaces. */
    shash_init(&old_ifaces);
    shash_init(&new_ifaces);
    for (i = 0; i < port->n_ifaces; i++) {
        shash_add(&old_ifaces, port->ifaces[i]->name, port->ifaces[i]);
    }
    for (i = 0; i < cfg->n_interfaces; i++) {
        const char *name = cfg->interfaces[i]->name;
        if (!shash_add_once(&new_ifaces, name, cfg->interfaces[i])) {
            VLOG_WARN("port %s: %s specified twice as port interface",
                      port->name, name);
        }
    }
    port->updelay = cfg->bond_updelay;
    if (port->updelay < 0) {
        port->updelay = 0;
    }
    port->updelay = cfg->bond_downdelay;
    if (port->downdelay < 0) {
        port->downdelay = 0;
    }

    /* Get rid of deleted interfaces and add new interfaces. */
    SHASH_FOR_EACH (node, &old_ifaces) {
        if (!shash_find(&new_ifaces, node->name)) {
            iface_destroy(node->data);
        }
    }
    SHASH_FOR_EACH (node, &new_ifaces) {
        const struct ovsrec_interface *if_cfg = node->data;
        struct iface *iface;

        iface = shash_find_data(&old_ifaces, if_cfg->name);
        if (!iface) {
            iface = iface_create(port, if_cfg);
        } else {
            iface->cfg = if_cfg;
        }
    }

    /* Get VLAN tag. */
    vlan = -1;
    if (cfg->tag) {
        if (port->n_ifaces < 2) {
            vlan = *cfg->tag;
            if (vlan >= 0 && vlan <= 4095) {
                VLOG_DBG("port %s: assigning VLAN tag %d", port->name, vlan);
            } else {
                vlan = -1;
            }
        } else {
            /* It's possible that bonded, VLAN-tagged ports make sense.  Maybe
             * they even work as-is.  But they have not been tested. */
            VLOG_WARN("port %s: VLAN tags not supported on bonded ports",
                      port->name);
        }
    }
    if (port->vlan != vlan) {
        port->vlan = vlan;
        bridge_flush(port->bridge);
    }

    /* Get trunked VLANs. */
    trunks = NULL;
    if (vlan < 0) {
        size_t n_errors;
        size_t i;

        trunks = bitmap_allocate(4096);
        n_errors = 0;
        for (i = 0; i < cfg->n_trunks; i++) {
            int trunk = cfg->trunks[i];
            if (trunk >= 0) {
                bitmap_set1(trunks, trunk);
            } else {
                n_errors++;
            }
        }
        if (n_errors) {
            VLOG_ERR("port %s: invalid values for %zu trunk VLANs",
                     port->name, cfg->n_trunks);
        }
        if (n_errors == cfg->n_trunks) {
            if (n_errors) {
                VLOG_ERR("port %s: no valid trunks, trunking all VLANs",
                         port->name);
            }
            bitmap_set_multiple(trunks, 0, 4096, 1);
        }
    } else {
        if (cfg->n_trunks) {
            VLOG_ERR("port %s: ignoring trunks in favor of implicit vlan",
                     port->name);
        }
    }
    if (trunks == NULL
        ? port->trunks != NULL
        : port->trunks == NULL || !bitmap_equal(trunks, port->trunks, 4096)) {
        bridge_flush(port->bridge);
    }
    bitmap_free(port->trunks);
    port->trunks = trunks;

    shash_destroy(&old_ifaces);
    shash_destroy(&new_ifaces);
}

static void
port_destroy(struct port *port)
{
    if (port) {
        struct bridge *br = port->bridge;
        struct port *del;
        int i;

        proc_net_compat_update_vlan(port->name, NULL, 0);
        proc_net_compat_update_bond(port->name, NULL);

        for (i = 0; i < MAX_MIRRORS; i++) {
            struct mirror *m = br->mirrors[i];
            if (m && m->out_port == port) {
                mirror_destroy(m);
            }
        }

        while (port->n_ifaces > 0) {
            iface_destroy(port->ifaces[port->n_ifaces - 1]);
        }

        del = br->ports[port->port_idx] = br->ports[--br->n_ports];
        del->port_idx = port->port_idx;

        free(port->ifaces);
        bitmap_free(port->trunks);
        free(port->name);
        free(port);
        bridge_flush(br);
    }
}

static struct port *
port_from_dp_ifidx(const struct bridge *br, uint16_t dp_ifidx)
{
    struct iface *iface = iface_from_dp_ifidx(br, dp_ifidx);
    return iface ? iface->port : NULL;
}

static struct port *
port_lookup(const struct bridge *br, const char *name)
{
    size_t i;

    for (i = 0; i < br->n_ports; i++) {
        struct port *port = br->ports[i];
        if (!strcmp(port->name, name)) {
            return port;
        }
    }
    return NULL;
}

static struct iface *
port_lookup_iface(const struct port *port, const char *name)
{
    size_t j;

    for (j = 0; j < port->n_ifaces; j++) {
        struct iface *iface = port->ifaces[j];
        if (!strcmp(iface->name, name)) {
            return iface;
        }
    }
    return NULL;
}

static void
port_update_bonding(struct port *port)
{
    if (port->n_ifaces < 2) {
        /* Not a bonded port. */
        if (port->bond_hash) {
            free(port->bond_hash);
            port->bond_hash = NULL;
            port->bond_compat_is_stale = true;
            port->bond_fake_iface = false;
        }
    } else {
        if (!port->bond_hash) {
            size_t i;

            port->bond_hash = xcalloc(BOND_MASK + 1, sizeof *port->bond_hash);
            for (i = 0; i <= BOND_MASK; i++) {
                struct bond_entry *e = &port->bond_hash[i];
                e->iface_idx = -1;
                e->tx_bytes = 0;
            }
            port->no_ifaces_tag = tag_create_random();
            bond_choose_active_iface(port);
        }
        port->bond_compat_is_stale = true;
        port->bond_fake_iface = port->cfg->bond_fake_iface;
    }
}

static void
port_update_bond_compat(struct port *port)
{
    struct compat_bond_hash compat_hashes[BOND_MASK + 1];
    struct compat_bond bond;
    size_t i;

    if (port->n_ifaces < 2) {
        proc_net_compat_update_bond(port->name, NULL);
        return;
    }

    bond.up = false;
    bond.updelay = port->updelay;
    bond.downdelay = port->downdelay;

    bond.n_hashes = 0;
    bond.hashes = compat_hashes;
    if (port->bond_hash) {
        const struct bond_entry *e;
        for (e = port->bond_hash; e <= &port->bond_hash[BOND_MASK]; e++) {
            if (e->iface_idx >= 0 && e->iface_idx < port->n_ifaces) {
                struct compat_bond_hash *cbh = &bond.hashes[bond.n_hashes++];
                cbh->hash = e - port->bond_hash;
                cbh->netdev_name = port->ifaces[e->iface_idx]->name;
            }
        }
    }

    bond.n_slaves = port->n_ifaces;
    bond.slaves = xmalloc(port->n_ifaces * sizeof *bond.slaves);
    for (i = 0; i < port->n_ifaces; i++) {
        struct iface *iface = port->ifaces[i];
        struct compat_bond_slave *slave = &bond.slaves[i];
        slave->name = iface->name;

        /* We need to make the same determination as the Linux bonding
         * code to determine whether a slave should be consider "up".
         * The Linux function bond_miimon_inspect() supports four 
         * BOND_LINK_* states:
         *      
         *    - BOND_LINK_UP: carrier detected, updelay has passed.
         *    - BOND_LINK_FAIL: carrier lost, downdelay in progress.
         *    - BOND_LINK_DOWN: carrier lost, downdelay has passed.
         *    - BOND_LINK_BACK: carrier detected, updelay in progress.
         *
         * The function bond_info_show_slave() only considers BOND_LINK_UP 
         * to be "up" and anything else to be "down".
         */
        slave->up = iface->enabled && iface->delay_expires == LLONG_MAX;
        if (slave->up) {
            bond.up = true;
        }
        netdev_get_etheraddr(iface->netdev, slave->mac);
    }

    if (port->bond_fake_iface) {
        struct netdev *bond_netdev;

        if (!netdev_open_default(port->name, &bond_netdev)) {
            if (bond.up) {
                netdev_turn_flags_on(bond_netdev, NETDEV_UP, true);
            } else {
                netdev_turn_flags_off(bond_netdev, NETDEV_UP, true);
            }
            netdev_close(bond_netdev);
        }
    }

    proc_net_compat_update_bond(port->name, &bond);
    free(bond.slaves);
}

static void
port_update_vlan_compat(struct port *port)
{
    struct bridge *br = port->bridge;
    char *vlandev_name = NULL;

    if (port->vlan > 0) {
        /* Figure out the name that the VLAN device should actually have, if it
         * existed.  This takes some work because the VLAN device would not
         * have port->name in its name; rather, it would have the trunk port's
         * name, and 'port' would be attached to a bridge that also had the
         * VLAN device one of its ports.  So we need to find a trunk port that
         * includes port->vlan.
         *
         * There might be more than one candidate.  This doesn't happen on
         * XenServer, so if it happens we just pick the first choice in
         * alphabetical order instead of creating multiple VLAN devices. */
        size_t i;
        for (i = 0; i < br->n_ports; i++) {
            struct port *p = br->ports[i];
            if (port_trunks_vlan(p, port->vlan)
                && p->n_ifaces
                && (!vlandev_name || strcmp(p->name, vlandev_name) <= 0))
            {
                uint8_t ea[ETH_ADDR_LEN];
                netdev_get_etheraddr(p->ifaces[0]->netdev, ea);
                if (!eth_addr_is_multicast(ea) &&
                    !eth_addr_is_reserved(ea) &&
                    !eth_addr_is_zero(ea)) {
                    vlandev_name = p->name;
                }
            }
        }
    }
    proc_net_compat_update_vlan(port->name, vlandev_name, port->vlan);
}

/* Interface functions. */

static struct iface *
iface_create(struct port *port, const struct ovsrec_interface *if_cfg)
{
    struct iface *iface;
    char *name = if_cfg->name;
    int error;

    iface = xzalloc(sizeof *iface);
    iface->port = port;
    iface->port_ifidx = port->n_ifaces;
    iface->name = xstrdup(name);
    iface->dp_ifidx = -1;
    iface->tag = tag_create_random();
    iface->delay_expires = LLONG_MAX;
    iface->netdev = NULL;
    iface->cfg = if_cfg;

    if (port->n_ifaces >= port->allocated_ifaces) {
        port->ifaces = x2nrealloc(port->ifaces, &port->allocated_ifaces,
                                  sizeof *port->ifaces);
    }
    port->ifaces[port->n_ifaces++] = iface;
    if (port->n_ifaces > 1) {
        port->bridge->has_bonded_ports = true;
    }

    /* Attempt to create the network interface in case it
     * doesn't exist yet. */
    if (!iface_is_internal(port->bridge, iface->name)) {
        error = set_up_iface(if_cfg, iface, true);
        if (error) {
            VLOG_WARN("could not create iface %s: %s", iface->name,
                    strerror(error));
        }
    }

    VLOG_DBG("attached network device %s to port %s", iface->name, port->name);

    bridge_flush(port->bridge);

    return iface;
}

static void
iface_destroy(struct iface *iface)
{
    if (iface) {
        struct port *port = iface->port;
        struct bridge *br = port->bridge;
        bool del_active = port->active_iface == iface->port_ifidx;
        struct iface *del;

        if (iface->dp_ifidx >= 0) {
            port_array_set(&br->ifaces, iface->dp_ifidx, NULL);
        }

        del = port->ifaces[iface->port_ifidx] = port->ifaces[--port->n_ifaces];
        del->port_ifidx = iface->port_ifidx;

        netdev_close(iface->netdev);

        if (del_active) {
            ofproto_revalidate(port->bridge->ofproto, port->active_iface_tag);
            bond_choose_active_iface(port);
            bond_send_learning_packets(port);
        }

        free(iface->name);
        free(iface);

        bridge_flush(port->bridge);
    }
}

static struct iface *
iface_lookup(const struct bridge *br, const char *name)
{
    size_t i, j;

    for (i = 0; i < br->n_ports; i++) {
        struct port *port = br->ports[i];
        for (j = 0; j < port->n_ifaces; j++) {
            struct iface *iface = port->ifaces[j];
            if (!strcmp(iface->name, name)) {
                return iface;
            }
        }
    }
    return NULL;
}

static struct iface *
iface_from_dp_ifidx(const struct bridge *br, uint16_t dp_ifidx)
{
    return port_array_get(&br->ifaces, dp_ifidx);
}

/* Returns true if 'iface' is the name of an "internal" interface on bridge
 * 'br', that is, an interface that is entirely simulated within the datapath.
 * The local port (ODPP_LOCAL) is always an internal interface.  Other local
 * interfaces are created by setting "iface.<iface>.internal = true".
 *
 * In addition, we have a kluge-y feature that creates an internal port with
 * the name of a bonded port if "bonding.<bondname>.fake-iface = true" is set.
 * This feature needs to go away in the long term.  Until then, this is one
 * reason why this function takes a name instead of a struct iface: the fake
 * interfaces created this way do not have a struct iface. */
static bool
iface_is_internal(const struct bridge *br, const char *if_name)
{
    /* XXX wastes time */
    struct iface *iface;
    struct port *port;

    if (!strcmp(if_name, br->name)) {
        return true;
    }

    iface = iface_lookup(br, if_name);
    if (iface && !strcmp(iface->cfg->type, "internal")) {
        return true;
    }

    port = port_lookup(br, if_name);
    if (port && port->n_ifaces > 1 && port->cfg->bond_fake_iface) {
        return true;
    }
    return false;
}

/* Set Ethernet address of 'iface', if one is specified in the configuration
 * file. */
static void
iface_set_mac(struct iface *iface)
{
    uint8_t ea[ETH_ADDR_LEN];

    if (iface->cfg->mac && eth_addr_from_string(iface->cfg->mac, ea)) {
        if (eth_addr_is_multicast(ea)) {
            VLOG_ERR("interface %s: cannot set MAC to multicast address",
                     iface->name);
        } else if (iface->dp_ifidx == ODPP_LOCAL) {
            VLOG_ERR("ignoring iface.%s.mac; use bridge.%s.mac instead",
                     iface->name, iface->name);
        } else {
            int error = netdev_set_etheraddr(iface->netdev, ea);
            if (error) {
                VLOG_ERR("interface %s: setting MAC failed (%s)",
                         iface->name, strerror(error));
            }
        }
    }
}

/* Port mirroring. */

static void
mirror_reconfigure(struct bridge *br)
{
    struct shash old_mirrors, new_mirrors;
    struct shash_node *node;
    unsigned long *rspan_vlans;
    int i;

    /* Collect old mirrors. */
    shash_init(&old_mirrors);
    for (i = 0; i < MAX_MIRRORS; i++) {
        if (br->mirrors[i]) {
            shash_add(&old_mirrors, br->mirrors[i]->name, br->mirrors[i]);
        }
    }

    /* Collect new mirrors. */
    shash_init(&new_mirrors);
    for (i = 0; i < br->cfg->n_mirrors; i++) {
        struct ovsrec_mirror *cfg = br->cfg->mirrors[i];
        if (!shash_add_once(&new_mirrors, cfg->name, cfg)) {
            VLOG_WARN("bridge %s: %s specified twice as mirror",
                      br->name, cfg->name);
        }
    }

    /* Get rid of deleted mirrors and add new mirrors. */
    SHASH_FOR_EACH (node, &old_mirrors) {
        if (!shash_find(&new_mirrors, node->name)) {
            mirror_destroy(node->data);
        }
    }
    SHASH_FOR_EACH (node, &new_mirrors) {
        struct mirror *mirror = shash_find_data(&old_mirrors, node->name);
        if (!mirror) {
            mirror = mirror_create(br, node->name);
            if (!mirror) {
                break;
            }
        }
        mirror_reconfigure_one(mirror, node->data);
    }
    shash_destroy(&old_mirrors);
    shash_destroy(&new_mirrors);

    /* Update port reserved status. */
    for (i = 0; i < br->n_ports; i++) {
        br->ports[i]->is_mirror_output_port = false;
    }
    for (i = 0; i < MAX_MIRRORS; i++) {
        struct mirror *m = br->mirrors[i];
        if (m && m->out_port) {
            m->out_port->is_mirror_output_port = true;
        }
    }

    /* Update flooded vlans (for RSPAN). */
    rspan_vlans = NULL;
    if (br->cfg->n_flood_vlans) {
        rspan_vlans = bitmap_allocate(4096);

        for (i = 0; i < br->cfg->n_flood_vlans; i++) {
            int64_t vlan = br->cfg->flood_vlans[i];
            if (vlan >= 0 && vlan < 4096) {
                bitmap_set1(rspan_vlans, vlan);
                VLOG_INFO("bridge %s: disabling learning on vlan %"PRId64,
                          br->name, vlan);
            } else {
                VLOG_ERR("bridge %s: invalid value %"PRId64 "for flood VLAN",
                         br->name, vlan);
            }
        }
    }
    if (mac_learning_set_flood_vlans(br->ml, rspan_vlans)) {
        bridge_flush(br);
    }
}

static struct mirror *
mirror_create(struct bridge *br, const char *name)
{
    struct mirror *m;
    size_t i;

    for (i = 0; ; i++) {
        if (i >= MAX_MIRRORS) {
            VLOG_WARN("bridge %s: maximum of %d port mirrors reached, "
                      "cannot create %s", br->name, MAX_MIRRORS, name);
            return NULL;
        }
        if (!br->mirrors[i]) {
            break;
        }
    }

    VLOG_INFO("created port mirror %s on bridge %s", name, br->name);
    bridge_flush(br);

    br->mirrors[i] = m = xzalloc(sizeof *m);
    m->bridge = br;
    m->idx = i;
    m->name = xstrdup(name);
    shash_init(&m->src_ports);
    shash_init(&m->dst_ports);
    m->vlans = NULL;
    m->n_vlans = 0;
    m->out_vlan = -1;
    m->out_port = NULL;

    return m;
}

static void
mirror_destroy(struct mirror *m)
{
    if (m) {
        struct bridge *br = m->bridge;
        size_t i;

        for (i = 0; i < br->n_ports; i++) {
            br->ports[i]->src_mirrors &= ~(MIRROR_MASK_C(1) << m->idx);
            br->ports[i]->dst_mirrors &= ~(MIRROR_MASK_C(1) << m->idx);
        }

        shash_destroy(&m->src_ports);
        shash_destroy(&m->dst_ports);
        free(m->vlans);

        m->bridge->mirrors[m->idx] = NULL;
        free(m);

        bridge_flush(br);
    }
}

static void
mirror_collect_ports(struct mirror *m, struct ovsrec_port **ports, int n_ports,
                     struct shash *names)
{
    size_t i;

    for (i = 0; i < n_ports; i++) {
        const char *name = ports[i]->name;
        if (port_lookup(m->bridge, name)) {
            shash_add_once(names, name, NULL);
        } else {
            VLOG_WARN("bridge %s: mirror %s cannot match on nonexistent "
                      "port %s", m->bridge->name, m->name, name);
        }
    }
}

static size_t
mirror_collect_vlans(struct mirror *m, const struct ovsrec_mirror *cfg,
                     int **vlans)
{
    size_t n_vlans;
    size_t i;

    *vlans = xmalloc(sizeof *vlans * cfg->n_select_vlan);
    n_vlans = 0;
    for (i = 0; i < cfg->n_select_vlan; i++) {
        int64_t vlan = cfg->select_vlan[i];
        if (vlan < 0 || vlan > 4095) {
            VLOG_WARN("bridge %s: mirror %s selects invalid VLAN %"PRId64,
                      m->bridge->name, m->name, vlan);
        } else {
            (*vlans)[n_vlans++] = vlan;
        }
    }
    return n_vlans;
}

static bool
vlan_is_mirrored(const struct mirror *m, int vlan)
{
    size_t i;

    for (i = 0; i < m->n_vlans; i++) {
        if (m->vlans[i] == vlan) {
            return true;
        }
    }
    return false;
}

static bool
port_trunks_any_mirrored_vlan(const struct mirror *m, const struct port *p)
{
    size_t i;

    for (i = 0; i < m->n_vlans; i++) {
        if (port_trunks_vlan(p, m->vlans[i])) {
            return true;
        }
    }
    return false;
}

static void
mirror_reconfigure_one(struct mirror *m, struct ovsrec_mirror *cfg)
{
    struct shash src_ports, dst_ports;
    mirror_mask_t mirror_bit;
    struct port *out_port;
    int out_vlan;
    size_t n_vlans;
    int *vlans;
    size_t i;
    bool mirror_all_ports;
    bool any_ports_specified;
    bool any_vlans_specified;

    /* Get output port. */
    if (cfg->output_port) {
        out_port = port_lookup(m->bridge, cfg->output_port->name);
        if (!out_port) {
            VLOG_ERR("bridge %s: mirror %s outputs to port not on bridge",
                     m->bridge->name, m->name);
            mirror_destroy(m);
            return;
        }
        out_vlan = -1;

        if (cfg->output_vlan) {
            VLOG_ERR("bridge %s: mirror %s specifies both output port and "
                     "output vlan; ignoring output vlan",
                     m->bridge->name, m->name);
        }
    } else if (cfg->output_vlan) {
        out_port = NULL;
        out_vlan = *cfg->output_vlan;
    } else {
        VLOG_ERR("bridge %s: mirror %s does not specify output; ignoring",
                 m->bridge->name, m->name);
        mirror_destroy(m);
        return;
    }

    /* Get all the ports, and drop duplicates and ports that don't exist. */
    shash_init(&src_ports);
    shash_init(&dst_ports);
    mirror_collect_ports(m, cfg->select_src_port, cfg->n_select_src_port,
                         &src_ports);
    mirror_collect_ports(m, cfg->select_dst_port, cfg->n_select_dst_port,
                         &dst_ports);
    any_ports_specified = cfg->n_select_dst_port || cfg->n_select_dst_port;
    if (any_ports_specified
        && shash_is_empty(&src_ports) && shash_is_empty(&dst_ports)) {
        VLOG_ERR("bridge %s: disabling mirror %s since none of the specified "
                 "selection ports exists", m->bridge->name, m->name);
        mirror_destroy(m);
        goto exit;
    }

    /* Get all the vlans, and drop duplicate and invalid vlans. */
    n_vlans = mirror_collect_vlans(m, cfg, &vlans);
    any_vlans_specified = cfg->n_select_vlan > 0;
    if (any_vlans_specified && !n_vlans) {
        VLOG_ERR("bridge %s: disabling mirror %s since none of the specified "
                 "VLANs exists", m->bridge->name, m->name);
        mirror_destroy(m);
        goto exit;
    }

    /* Update mirror data. */
    if (!shash_equal_keys(&m->src_ports, &src_ports)
        || !shash_equal_keys(&m->dst_ports, &dst_ports)
        || m->n_vlans != n_vlans
        || memcmp(m->vlans, vlans, sizeof *vlans * n_vlans)
        || m->out_port != out_port
        || m->out_vlan != out_vlan) {
        bridge_flush(m->bridge);
    }
    shash_swap(&m->src_ports, &src_ports);
    shash_swap(&m->dst_ports, &dst_ports);
    free(m->vlans);
    m->vlans = vlans;
    m->n_vlans = n_vlans;
    m->out_port = out_port;
    m->out_vlan = out_vlan;

    /* If no selection criteria have been given, mirror for all ports. */
    mirror_all_ports = !any_ports_specified && !any_vlans_specified;

    /* Update ports. */
    mirror_bit = MIRROR_MASK_C(1) << m->idx;
    for (i = 0; i < m->bridge->n_ports; i++) {
        struct port *port = m->bridge->ports[i];

        if (mirror_all_ports
            || shash_find(&m->src_ports, port->name)
            || (m->n_vlans
                && (!port->vlan
                    ? port_trunks_any_mirrored_vlan(m, port)
                    : vlan_is_mirrored(m, port->vlan)))) {
            port->src_mirrors |= mirror_bit;
        } else {
            port->src_mirrors &= ~mirror_bit;
        }

        if (mirror_all_ports || shash_find(&m->dst_ports, port->name)) {
            port->dst_mirrors |= mirror_bit;
        } else {
            port->dst_mirrors &= ~mirror_bit;
        }
    }

    /* Clean up. */
exit:
    shash_destroy(&src_ports);
    shash_destroy(&dst_ports);
}
