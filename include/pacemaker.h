/*
 * Copyright 2019-2021 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU Lesser General Public License
 * version 2.1 or later (LGPLv2.1+) WITHOUT ANY WARRANTY.
 */

#ifndef PCMK__PACEMAKER__H
#  define PCMK__PACEMAKER__H

#  include <glib.h>
#  include <libxml/tree.h>
#  include <crm/pengine/pe_types.h>

#  include <crm/stonith-ng.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \file
 * \brief High Level API
 * \ingroup pacemaker
 */


/*!
 * \brief Modify operation of running a cluster simulation.
 */
enum pcmk_sim_flags {
    pcmk_sim_none             = 0,
    pcmk_sim_all_actions      = 1 << 0,
    pcmk_sim_show_pending     = 1 << 1,
    pcmk_sim_process          = 1 << 2,
    pcmk_sim_show_scores      = 1 << 3,
    pcmk_sim_show_utilization = 1 << 4,
    pcmk_sim_simulate         = 1 << 5,
    pcmk_sim_sanitized        = 1 << 6,
    pcmk_sim_verbose          = 1 << 7,
};

/*!
 * \brief Synthetic cluster events that can be injected into the cluster
 *        for running simulations.
 */
typedef struct {
    /*! A list of node names (gchar *) to simulate bringing online */
    GList *node_up;
    /*! A list of node names (gchar *) to simulate bringing offline */
    GList *node_down;
    /*! A list of node names (gchar *) to simulate failing */
    GList *node_fail;
    /*! A list of operations (gchar *) to inject.  The format of these strings
     * is described in the "Operation Specification" section of crm_simulate
     * help output.
     */
    GList *op_inject;
    /*! A list of operations (gchar *) that should return a given error code
     * if they fail.  The format of these strings is described in the
     * "Operation Specification" section of crm_simulate help output.
     */
    GList *op_fail;
    /*! A list of tickets (gchar *) to simulate granting */
    GList *ticket_grant;
    /*! A list of tickets (gchar *) to simulate revoking */
    GList *ticket_revoke;
    /*! A list of tickets (gchar *) to simulate putting on standby */
    GList *ticket_standby;
    /*! A list of tickets (gchar *) to simulate activating */
    GList *ticket_activate;
    /*! Does the cluster have an active watchdog device? */
    char *watchdog;
    /*! Does the cluster have quorum? */
    char *quorum;
} pcmk_injections_t;

/*!
 * \brief Get controller status
 *
 * \param[in,out] xml                The destination for the result, as an XML tree.
 * \param[in]     dest_node          Destination node for request
 * \param[in]     message_timeout_ms Message timeout
 *
 * \return Standard Pacemaker return code
 */
int pcmk_controller_status(xmlNodePtr *xml, char *dest_node, unsigned int message_timeout_ms);

/*!
 * \brief Get designated controller
 *
 * \param[in,out] xml                The destination for the result, as an XML tree.
 * \param[in]     message_timeout_ms Message timeout
 *
 * \return Standard Pacemaker return code
 */
int pcmk_designated_controller(xmlNodePtr *xml, unsigned int message_timeout_ms);

/*!
 * \brief Free a :pcmk_injections_t structure
 *
 * \param[in,out] injections The structure to be freed
 */
void pcmk_free_injections(pcmk_injections_t *injections);

/*!
 * \brief Get pacemakerd status
 *
 * \param[in,out] xml                The destination for the result, as an XML tree.
 * \param[in]     ipc_name           IPC name for request
 * \param[in]     message_timeout_ms Message timeout
 *
 * \return Standard Pacemaker return code
 */
int pcmk_pacemakerd_status(xmlNodePtr *xml, char *ipc_name, unsigned int message_timeout_ms);

/*!
 * \brief Calculate and output resource operation digests
 *
 * \param[out] xml        Where to store XML with result
 * \param[in]  rsc        Resource to calculate digests for
 * \param[in]  node       Node whose operation history should be used
 * \param[in]  overrides  Hash table of configuration parameters to override
 * \param[in]  data_set   Cluster working set (with status)
 *
 * \return Standard Pacemaker return code
 */
int pcmk_resource_digests(xmlNodePtr *xml, pe_resource_t *rsc,
                          pe_node_t *node, GHashTable *overrides,
                          pe_working_set_t *data_set);

/**
 * \brief Simulate a cluster's response to events.
 *
 * This high-level function essentially implements crm_simulate(8).  It operates
 * on an input CIB file and various lists of events that can be simulated.  It
 * optionally writes out a variety of artifacts to show the results of the
 * simulation.  Output can be modified with various flags.
 *
 * \param[in,out] xml          The destination for the result, as an XML tree.
 * \param[in,out] data_set     Working set for the cluster.
 * \param[in]     events       A structure containing cluster events
 *                             (node up/down, tickets, injected operations)
 * \param[in]     flags        A bitfield of :pcmk_sim_flags to modify
 *                             operation of the simulation.
 * \param[in]     section_opts Which portions of the cluster status output
 *                             should be displayed?
 * \param[in]     use_date     The date to set the cluster's time to
 *                             (may be NULL).
 * \param[in]     input_file   The source CIB file, which may be overwritten by
 *                             this function (may be NULL).
 * \param[in]     graph_file   Where to write the XML-formatted transition graph
 *                             (may be NULL, in which case no file will be
 *                             written).
 * \param[in]     dot_file     Where to write the dot(1) formatted transition
 *                             graph (may be NULL, in which case no file will
 *                             be written).  See \p pcmk__write_sim_dotfile().
 *
 * \return Standard Pacemaker return code
 */
int pcmk_simulate(xmlNodePtr *xml, pe_working_set_t *data_set,
                  pcmk_injections_t *injections, unsigned int flags,
                  unsigned int section_opts, char *use_date, char *input_file,
                  char *graph_file, char *dot_file);

/*!
 * \brief Get nodes list
 *
 * \param[in,out] xml                The destination for the result, as an XML tree.
 * \param[in]     node_types         Node type(s) to return (default: all)
 *
 * \return Standard Pacemaker return code
 */
int pcmk_list_nodes(xmlNodePtr *xml, char *node_types);

#ifdef BUILD_PUBLIC_LIBPACEMAKER

/*!
 * \brief Ask the cluster to perform fencing
 *
 * \param[in] st        A connection to the fencer API
 * \param[in] target    The node that should be fenced
 * \param[in] action    The fencing action (on, off, reboot) to perform
 * \param[in] name      Who requested the fence action?
 * \param[in] timeout   How long to wait for the operation to complete (in ms)
 * \param[in] tolerance If a successful action for \p target happened within
 *                      this many ms, return 0 without performing the action
 *                      again
 * \param[in] delay     Apply this delay (in milliseconds) before initiating the
 *                      fencing action (a value of -1 applies no delay and also
 *                      disables any fencing delay from pcmk_delay_base and
 *                      pcmk_delay_max)
 * \param[out] reason   If not NULL, where to put descriptive failure reason
 *
 * \return Standard Pacemaker return code
 * \note If \p reason is not NULL, the caller is responsible for freeing its
 *       returned value.
 */
int pcmk_request_fencing(stonith_t *st, const char *target, const char *action,
                         const char *name, unsigned int timeout,
                         unsigned int tolerance, int delay, char **reason);

/*!
 * \brief List the fencing operations that have occurred for a specific node.
 *
 * \note If \p xml is not NULL, it will be freed first and the previous
 *       contents lost.
 *
 * \param[in,out] xml       The destination for the result, as an XML tree.
 * \param[in]     st        A connection to the STONITH API.
 * \param[in]     target    The node to get history for.
 * \param[in]     timeout   How long to wait for the operation to complete (in ms).
 * \param[in]     quiet     Suppress most output.
 * \param[in]     verbose   Include additional output.
 * \param[in]     broadcast Gather fencing history from all nodes.
 * \param[in]     cleanup   Clean up fencing history after listing.
 *
 * \return Standard Pacemaker return code
 */
int pcmk_fence_history(xmlNodePtr *xml, stonith_t *st, char *target,
                       unsigned int timeout, bool quiet, int verbose,
                       bool broadcast, bool cleanup);

/*!
 * \brief List all installed STONITH agents.
 *
 * \note If \p xml is not NULL, it will be freed first and the previous
 *       contents lost.
 *
 * \param[in,out] xml     The destination for the result, as an XML tree.
 * \param[in]     st      A connection to the STONITH API.
 * \param[in]     timeout How long to wait for the operation to complete (in ms).
 *
 * \return Standard Pacemaker return code
 */
int pcmk_fence_installed(xmlNodePtr *xml, stonith_t *st, unsigned int timeout);

/*!
 * \brief When was a device last fenced?
 *
 * \note If \p xml is not NULL, it will be freed first and the previous
 *       contents lost.
 *
 * \param[in,out] xml       The destination for the result, as an XML tree.
 * \param[in]     target    The node that was fenced.
 * \param[in]     as_nodeid
 *
 * \return Standard Pacemaker return code
 */
int pcmk_fence_last(xmlNodePtr *xml, const char *target, bool as_nodeid);

/*!
 * \brief List nodes that can be fenced.
 *
 * \note If \p xml is not NULL, it will be freed first and the previous
 *       contents lost.
 *
 * \param[in,out] xml        The destination for the result, as an XML tree
 * \param[in]     st         A connection to the STONITH API
 * \param[in]     device_id  Resource ID of fence device to check
 * \param[in]     timeout    How long to wait for the operation to complete (in ms)
 *
 * \return Standard Pacemaker return code
 */
int pcmk_fence_list_targets(xmlNodePtr *xml, stonith_t *st,
                            const char *device_id, unsigned int timeout);

/*!
 * \brief Get metadata for a resource.
 *
 * \note If \p xml is not NULL, it will be freed first and the previous
 *       contents lost.
 *
 * \param[in,out] xml     The destination for the result, as an XML tree.
 * \param[in]     st      A connection to the STONITH API.
 * \param[in]     agent   The fence agent to get metadata for.
 * \param[in]     timeout How long to wait for the operation to complete (in ms).
 *
 * \return Standard Pacemaker return code
 */
int pcmk_fence_metadata(xmlNodePtr *xml, stonith_t *st, char *agent,
                        unsigned int timeout);

/*!
 * \brief List registered fence devices.
 *
 * \note If \p xml is not NULL, it will be freed first and the previous
 *       contents lost.
 *
 * \param[in,out] xml     The destination for the result, as an XML tree.
 * \param[in]     st      A connection to the STONITH API.
 * \param[in]     target  If not NULL, only return devices that can fence
 *                        this node.
 * \param[in]     timeout How long to wait for the operation to complete (in ms).
 *
 * \return Standard Pacemaker return code
 */
int pcmk_fence_registered(xmlNodePtr *xml, stonith_t *st, char *target,
                          unsigned int timeout);

/*!
 * \brief Register a fencing level for a specific node, node regex, or attribute.
 *
 * \p target can take three different forms:
 *   - name=value, in which case \p target is an attribute.
 *   - @pattern, in which case \p target is a node regex.
 *   - Otherwise, \p target is a node name.
 *
 * \param[in] st          A connection to the STONITH API.
 * \param[in] target      The object to register a fencing level for.
 * \param[in] fence_level Index number of level to add.
 * \param[in] devices     Devices to use in level.
 *
 * \return Standard Pacemaker return code
 */
int pcmk_fence_register_level(stonith_t *st, char *target, int fence_level,
                              stonith_key_value_t *devices);

/*!
 * \brief Unregister a fencing level for a specific node, node regex, or attribute.
 *
 * \p target can take three different forms:
 *   - name=value, in which case \p target is an attribute.
 *   - @pattern, in which case \p target is a node regex.
 *   - Otherwise, \p target is a node name.
 *
 * \param[in] st          A connection to the STONITH API.
 * \param[in] target      The object to unregister a fencing level for.
 * \param[in] fence_level Index number of level to remove.
 *
 * \return Standard Pacemaker return code
 */
int pcmk_fence_unregister_level(stonith_t *st, char *target, int fence_level);

/*!
 * \brief Validate a STONITH device configuration.
 *
 * \note If \p xml is not NULL, it will be freed first and the previous
 *       contents lost.
 *
 * \param[in,out] xml     The destination for the result, as an XML tree.
 * \param[in]     st      A connection to the STONITH API.
 * \param[in]     agent   The agent to validate (for example, "fence_xvm").
 * \param[in]     id      STONITH device ID (may be NULL).
 * \param[in]     params  STONITH device configuration parameters.
 * \param[in]     timeout How long to wait for the operation to complete (in ms).
 *
 * \return Standard Pacemaker return code
 */
int pcmk_fence_validate(xmlNodePtr *xml, stonith_t *st, const char *agent,
                        const char *id, stonith_key_value_t *params,
                        unsigned int timeout);
#endif

#ifdef __cplusplus
}
#endif

#endif
