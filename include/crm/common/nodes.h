/*
 * Copyright 2004-2024 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU Lesser General Public License
 * version 2.1 or later (LGPLv2.1+) WITHOUT ANY WARRANTY.
 */

#ifndef PCMK__CRM_COMMON_NODES__H
#define PCMK__CRM_COMMON_NODES__H

#include <stdbool.h>                    // bool
#include <glib.h>                       // gboolean, GList, GHashTable
#include <libxml/tree.h>                // xmlNode

#include <crm/common/scheduler_types.h> // pcmk_resource_t, pcmk_scheduler_t

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * \file
 * \brief Scheduler API for nodes
 * \ingroup core
 */

// Special node attributes

#define PCMK_NODE_ATTR_MAINTENANCE          "maintenance"
#define PCMK_NODE_ATTR_STANDBY              "standby"
#define PCMK_NODE_ATTR_TERMINATE            "terminate"


// @COMPAT Make this internal when we can break API backward compatibility
//!@{
//! \deprecated Do not use (public access will be removed in a future release)
enum node_type { // Possible node types
    pcmk_node_variant_cluster  = 1,     // Cluster layer node
    pcmk_node_variant_remote   = 2,     // Pacemaker Remote node

    node_ping   = 0,                    // deprecated
#if !defined(PCMK_ALLOW_DEPRECATED) || (PCMK_ALLOW_DEPRECATED == 1)
    node_member = pcmk_node_variant_cluster,
    node_remote = pcmk_node_variant_remote,
#endif
};
//!@}

// When to probe a resource on a node (as specified in location constraints)
// @COMPAT Make this internal when we can break API backward compatibility
//!@{
//! \deprecated Do not use (public access will be removed in a future release)
enum pe_discover_e {
    pcmk_probe_always       = 0,    // Always probe resource on node
    pcmk_probe_never        = 1,    // Never probe resource on node
    pcmk_probe_exclusive    = 2,    // Probe only on designated nodes

#if !defined(PCMK_ALLOW_DEPRECATED) || (PCMK_ALLOW_DEPRECATED == 1)
    pe_discover_always      = pcmk_probe_always,
    pe_discover_never       = pcmk_probe_never,
    pe_discover_exclusive   = pcmk_probe_exclusive,
#endif
};
//!@}

// Basic node information (all node objects for the same node share this)
// @COMPAT Make this internal when we can break API backward compatibility
//!@{
//! \deprecated Do not use (public access will be removed in a future release)
struct pe_node_shared_s {
    const char *id;             // Node ID at the cluster layer
    const char *uname;          // Node name in cluster
    enum node_type type;        // Node variant

    // @TODO Convert these into a flag group

    // NOTE: sbd (as of at least 1.5.2) uses this
    //! \deprecated Call pcmk_node_is_online() instead
    gboolean online;            // Whether online

    gboolean standby;           // Whether in standby mode
    gboolean standby_onfail;    // Whether in standby mode due to on-fail

    // NOTE: sbd (as of at least 1.5.2) uses this
    //! \deprecated Call pcmk_node_is_pending() instead
    gboolean pending;           // Whether controller membership is pending

    // NOTE: sbd (as of at least 1.5.2) uses this
    //! \deprecated Call !pcmk_node_is_clean() instead
    gboolean unclean;           // Whether node requires fencing

    gboolean unseen;            // Whether node has never joined cluster

    // NOTE: sbd (as of at least 1.5.2) uses this
    //! \deprecated Call pcmk_node_is_shutting_down() instead
    gboolean shutdown;          // Whether shutting down

    gboolean expected_up;       // Whether expected join state is member
    gboolean is_dc;             // Whether node is cluster's DC

    // NOTE: sbd (as of at least 1.5.2) uses this
    //! \deprecated Call pcmk_node_is_in_maintenance() instead
    gboolean maintenance;       // Whether in maintenance mode

    gboolean rsc_discovery_enabled; // Whether probes are allowed on node

    /*
     * Whether this is a guest node whose guest resource must be recovered or a
     * remote node that must be fenced
     */
    gboolean remote_requires_reset;

    /*
     * Whether this is a Pacemaker Remote node that was fenced since it was last
     * connected by the cluster
     */
    gboolean remote_was_fenced;

    /*
     * Whether this is a Pacemaker Remote node previously marked in its
     * node state as being in maintenance mode
     */
    gboolean remote_maintenance;

    gboolean unpacked;              // Whether node history has been unpacked

    /*
     * Number of resources active on this node (valid after CIB status section
     * has been unpacked, as long as pcmk_sched_no_counts was not set)
     */
    int num_resources;

    // Remote connection resource for node, if it is a Pacemaker Remote node
    pcmk_resource_t *remote_rsc;

    // NOTE: sbd (as of at least 1.5.2) uses this
    // \deprecated Call pcmk_foreach_active_resource() instead
    GList *running_rsc;             // List of resources active on node

    GList *allocated_rsc;           // List of resources assigned to node
    GHashTable *attrs;              // Node attributes
    GHashTable *utilization;        // Node utilization attributes
    GHashTable *digest_cache;       // Cache of calculated resource digests

    /*
     * Sum of priorities of all resources active on node and on any guest nodes
     * connected to this node, with +1 for promoted instances (used to compare
     * nodes for PCMK_OPT_PRIORITY_FENCING_DELAY)
     */
    int priority;

    pcmk_scheduler_t *data_set;     // Cluster that node is part of
};
//!@}

// Implementation of pcmk_node_t
// @COMPAT Make contents internal when we can break API backward compatibility
//!@{
//! \deprecated Do not use (public access will be removed in a future release)
struct pe_node_s {
    int weight;         // Node score for a given resource
    gboolean fixed;     // \deprecated Do not use
    int count;          // Counter reused by assignment and promotion code

    // NOTE: sbd (as of at least 1.5.2) uses this
    struct pe_node_shared_s *details;   // Basic node information

    // @COMPAT This should be enum pe_discover_e
    int rsc_discover_mode;              // Probe mode (enum pe_discover_e)
};
//!@}

bool pcmk_node_is_online(const pcmk_node_t *node);
bool pcmk_node_is_pending(const pcmk_node_t *node);
bool pcmk_node_is_clean(const pcmk_node_t *node);
bool pcmk_node_is_shutting_down(const pcmk_node_t *node);
bool pcmk_node_is_in_maintenance(const pcmk_node_t *node);

bool pcmk_foreach_active_resource(pcmk_node_t *node,
                                  bool (*fn)(pcmk_resource_t *, void *),
                                  void *user_data);

const char *pcmk_cib_node_shutdown(xmlNode *cib, const char *node);

/*!
 * \internal
 * \brief Return a string suitable for logging as a node name
 *
 * \param[in] node  Node to return a node name string for
 *
 * \return Node name if available, otherwise node ID if available,
 *         otherwise "unspecified node" if node is NULL or "unidentified node"
 *         if node has neither a name nor ID.
 */
static inline const char *
pcmk__node_name(const pcmk_node_t *node)
{
    if (node == NULL) {
        return "unspecified node";

    } else if (node->details->uname != NULL) {
        return node->details->uname;

    } else if (node->details->id != NULL) {
        return node->details->id;

    } else {
        return "unidentified node";
    }
}

/*!
 * \internal
 * \brief Check whether two node objects refer to the same node
 *
 * \param[in] node1  First node object to compare
 * \param[in] node2  Second node object to compare
 *
 * \return true if \p node1 and \p node2 refer to the same node
 */
static inline bool
pcmk__same_node(const pcmk_node_t *node1, const pcmk_node_t *node2)
{
    return (node1 != NULL) && (node2 != NULL)
           && (node1->details == node2->details);
}

#ifdef __cplusplus
}
#endif

#endif // PCMK__CRM_COMMON_NODES__H
