/*
 * Copyright 2004-2024 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU Lesser General Public License
 * version 2.1 or later (LGPLv2.1+) WITHOUT ANY WARRANTY.
 */

#ifndef PCMK__CRM_COMMON_SCHEDULER__H
#define PCMK__CRM_COMMON_SCHEDULER__H

#include <sys/types.h>                  // time_t
#include <libxml/tree.h>                // xmlNode
#include <glib.h>                       // guint, GList, GHashTable

#include <crm/common/iso8601.h>         // crm_time_t

#include <crm/common/actions.h>
#include <crm/common/nodes.h>
#include <crm/common/resources.h>
#include <crm/common/roles.h>
#include <crm/common/rules.h>
#include <crm/common/scheduler_types.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * \file
 * \brief Scheduler API
 * \ingroup core
 */

// NOTE: sbd (as of at least 1.5.2) uses this enum
//! Possible responses to loss of quorum
enum pe_quorum_policy {
    pcmk_no_quorum_freeze,  //<! Do not recover resources from outside partition
    pcmk_no_quorum_stop,    //<! Stop all resources in partition
    pcmk_no_quorum_ignore,  //<! Act as if partition still holds quorum
    pcmk_no_quorum_fence,   //<! Fence all nodes in partition
    pcmk_no_quorum_demote,  //<! Demote promotable resources and stop all others

#if !defined(PCMK_ALLOW_DEPRECATED) || (PCMK_ALLOW_DEPRECATED == 1)
    // NOTE: sbd (as of at least 1.5.2) uses this value
    //! \deprecated Use pcmk_no_quorum_freeze instead
    no_quorum_freeze    = pcmk_no_quorum_freeze,

    // NOTE: sbd (as of at least 1.5.2) uses this value
    //! \deprecated Use pcmk_no_quorum_stop instead
    no_quorum_stop      = pcmk_no_quorum_stop,

    // NOTE: sbd (as of at least 1.5.2) uses this value
    //! \deprecated Use pcmk_no_quorum_ignore instead
    no_quorum_ignore    = pcmk_no_quorum_ignore,

    //! \deprecated Use pcmk_no_quorum_fence instead
    no_quorum_suicide   = pcmk_no_quorum_fence,

    // NOTE: sbd (as of at least 1.5.2) uses this value
    //! \deprecated Use pcmk_no_quorum_demote instead
    no_quorum_demote    = pcmk_no_quorum_demote,
#endif
};

//! \internal Do not use
typedef struct pcmk__scheduler_private pcmk__scheduler_private_t;

/* Implementation of pcmk_scheduler_t
 *
 * @COMPAT Drop this struct once all members are moved to
 * pcmk__scheduler_private_t, and repoint pcmk_scheduler_t to that
 */
//!@{
//! \deprecated Do not use (public access will be removed in a future release)
struct pcmk__scheduler {
    // Be careful about when each piece of information is available and final

    // NOTE: sbd (as of at least 1.5.2) uses this
    //! \deprecated Set scheduler input with pcmk_set_scheduler_cib() instead
    xmlNode *input;                 // CIB XML

    crm_time_t *now;                // Current time for evaluation purposes
    char *dc_uuid;                  // Node ID of designated controller

    // NOTE: sbd (as of at least 1.5.2) uses this
    //! \deprecated Call pcmk_get_dc() instead
    pcmk_node_t *dc_node;           // Node object for DC

    const char *stonith_action;     // Default fencing action
    const char *placement_strategy; // Value of placement-strategy property

    // NOTE: sbd (as of at least 1.5.2) uses this
    // @COMPAT Change to uint64_t at a compatibility break
    //! \deprecated Call pcmk_has_quorum() to check quorum
    unsigned long long flags;       // Group of enum pcmk__scheduler_flags

    int stonith_timeout;            // Value of stonith-timeout property

    // NOTE: sbd (as of at least 1.5.2) uses this
    //! \deprecated Call pcmk_get_no_quorum_policy() to get no-quorum policy
    enum pe_quorum_policy no_quorum_policy; // Response to loss of quorum

    GHashTable *config_hash;        // Cluster properties

    // Ticket constraints unpacked from ticket state
    GHashTable *tickets;

    // Actions for which there can be only one (such as "fence node X")
    GHashTable *singletons;

    // NOTE: sbd (as of at least 1.5.2) uses this
    //! \deprecated Call pcmk_find_node() to find a node instead
    GList *nodes;                   // Nodes in cluster

    GList *resources;               // Resources in cluster
    GList *placement_constraints;   // Location constraints
    GList *ordering_constraints;    // Ordering constraints
    GList *colocation_constraints;  // Colocation constraints

    // Ticket constraints unpacked by libpacemaker
    GList *ticket_constraints;

    GList *actions;                 // Scheduled actions
    xmlNode *failed;                // History entries of failed actions
    xmlNode *op_defaults;           // Configured operation defaults
    xmlNode *rsc_defaults;          // Configured resource defaults
    int num_synapse;                // Number of transition graph synapses
    int max_valid_nodes;            // \deprecated Do not use
    int order_id;                   // ID to use for next created ordering
    int action_id;                  // ID to use for next created action
    xmlNode *graph;                 // Transition graph
    GHashTable *template_rsc_sets;  // Mappings of template ID to resource ID

    // @COMPAT Replace this with a fencer variable (only place it's used)
    const char *localhost;          // \deprecated Do not use

    GHashTable *tags;               // Configuration tags (ID -> pcmk__idref_t*)
    int blocked_resources;          // Number of blocked resources in cluster
    int disabled_resources;         // Number of disabled resources in cluster
    GList *param_check;             // History entries that need to be checked
    GList *stop_needed;             // Containers that need stop actions
    time_t recheck_by;              // Hint to controller when to reschedule
    int ninstances;                 // Total number of resource instances
    guint shutdown_lock;            // How long to lock resources (seconds)
    int priority_fencing_delay;     // Priority fencing delay

    pcmk__scheduler_private_t *priv;    // For Pacemaker use only

    guint node_pending_timeout;     // Pending join times out after this (ms)
};
//!@}

pcmk_node_t *pcmk_get_dc(const pcmk_scheduler_t *scheduler);
enum pe_quorum_policy pcmk_get_no_quorum_policy(const pcmk_scheduler_t
                                                *scheduler);

int pcmk_set_scheduler_cib(pcmk_scheduler_t *scheduler, xmlNode *cib);

bool pcmk_has_quorum(const pcmk_scheduler_t *scheduler);
pcmk_node_t *pcmk_find_node(const pcmk_scheduler_t *scheduler,
                            const char *node_name);

#ifdef __cplusplus
}
#endif

#endif // PCMK__CRM_COMMON_SCHEDULER__H
