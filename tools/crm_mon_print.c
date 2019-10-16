/*
 * Copyright 2019 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU General Public License version 2
 * or later (GPLv2+) WITHOUT ANY WARRANTY.
 */

#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <config.h>
#include <crm/cib/util.h>
#include <crm/common/curses_internal.h>
#include <crm/common/iso8601_internal.h>
#include <crm/common/xml.h>
#include <crm/msg_xml.h>
#include <crm/pengine/internal.h>
#include <crm/pengine/pe_types.h>
#include <crm/stonith-ng.h>
#include <crm/common/internal.h>
#include <crm/common/util.h>
#include <crm/fencing/internal.h>

#include "crm_mon.h"

static void print_resources_heading(pcmk__output_t *out, unsigned int mon_ops);
static void print_resources_closing(pcmk__output_t *out, unsigned int mon_ops);
static gboolean print_resources(pcmk__output_t *out, pe_working_set_t *data_set,
                                int print_opts, unsigned int mon_ops, gboolean brief_output,
                                gboolean print_summary);
static void print_rsc_history(pcmk__output_t *out, pe_working_set_t *data_set,
                              node_t *node, xmlNode *rsc_entry, unsigned int mon_ops,
                              GListPtr op_list);
static void print_node_history(pcmk__output_t *out, pe_working_set_t *data_set,
                               xmlNode *node_state, gboolean operations,
                               unsigned int mon_ops);
static gboolean add_extra_info(pcmk__output_t *out, node_t * node, GListPtr rsc_list,
                               const char *attrname, int *expected_score);
static void print_node_attribute(gpointer name, gpointer user_data);
static gboolean print_node_summary(pcmk__output_t *out, pe_working_set_t * data_set,
                                   gboolean operations, unsigned int mon_ops);
static gboolean print_cluster_tickets(pcmk__output_t *out, pe_working_set_t * data_set);
static gboolean print_neg_locations(pcmk__output_t *out, pe_working_set_t *data_set,
                                    unsigned int mon_ops, const char *prefix);
static gboolean print_node_attributes(pcmk__output_t *out, pe_working_set_t *data_set,
                                      unsigned int mon_ops);
static void print_cluster_times(pcmk__output_t *out, pe_working_set_t *data_set);
static void print_cluster_dc(pcmk__output_t *out, pe_working_set_t *data_set,
                             unsigned int mon_ops);
static void print_cluster_summary(pcmk__output_t *out, pe_working_set_t *data_set,
                                  unsigned int mon_ops, unsigned int show);
static gboolean print_failed_actions(pcmk__output_t *out, pe_working_set_t *data_set);
static gboolean print_failed_stonith_actions(pcmk__output_t *out, stonith_history_t *history,
                                             unsigned int mon_ops);
static gboolean print_stonith_pending(pcmk__output_t *out, stonith_history_t *history,
                                      unsigned int mon_ops);
static gboolean print_stonith_history(pcmk__output_t *out, stonith_history_t *history,
                                      unsigned int mon_ops);
static gboolean print_stonith_history_full(pcmk__output_t *out, stonith_history_t *history,
                                           unsigned int mon_ops);

/*!
 * \internal
 * \brief Print resources section heading appropriate to options
 *
 * \param[in] out     The output functions structure.
 * \param[in] mon_ops Bitmask of mon_op_options.
 */
static void
print_resources_heading(pcmk__output_t *out, unsigned int mon_ops)
{
    const char *heading;

    if (is_set(mon_ops, mon_op_group_by_node)) {

        /* Active resources have already been printed by node */
        heading = is_set(mon_ops, mon_op_inactive_resources) ? "Inactive Resources" : NULL;

    } else if (is_set(mon_ops, mon_op_inactive_resources)) {
        heading = "Full List of Resources";

    } else {
        heading = "Active Resources";
    }

    /* Print section heading */
    out->begin_list(out, NULL, NULL, "%s", heading);
}

/*!
 * \internal
 * \brief Print whatever resource section closing is appropriate
 *
 * \param[in] out     The output functions structure.
 * \param[in] mon_ops Bitmask of mon_op_options.
 */
static void
print_resources_closing(pcmk__output_t *out, unsigned int mon_ops)
{
    const char *heading;

    /* What type of resources we did or did not display */
    if (is_set(mon_ops, mon_op_group_by_node)) {
        heading = "inactive ";
    } else if (is_set(mon_ops, mon_op_inactive_resources)) {
        heading = "";
    } else {
        heading = "active ";
    }

    out->list_item(out, NULL, "No %sresources", heading);
}

/*!
 * \internal
 * \brief Print whatever resource section(s) are appropriate
 *
 * \param[in] out           The output functions structure.
 * \param[in] data_set      Cluster state to display.
 * \param[in] print_opts    Bitmask of pe_print_options.
 * \param[in] mon_ops       Bitmask of mon_op_options.
 * \param[in] brief_output  Whether to display full or brief output.
 * \param[in] print_summary Whether to display a failure summary.
 */
static gboolean
print_resources(pcmk__output_t *out, pe_working_set_t *data_set,
                int print_opts, unsigned int mon_ops, gboolean brief_output,
                gboolean print_summary)
{
    GListPtr rsc_iter;
    gboolean printed_resource = FALSE;

    /* If we already showed active resources by node, and
     * we're not showing inactive resources, we have nothing to do
     */
    if (is_set(mon_ops, mon_op_group_by_node) && is_not_set(mon_ops, mon_op_inactive_resources)) {
        return FALSE;
    }

    print_resources_heading(out, mon_ops);

    /* If we haven't already printed resources grouped by node,
     * and brief output was requested, print resource summary */
    if (brief_output && is_not_set(mon_ops, mon_op_group_by_node)) {
        pe__rscs_brief_output(out, data_set->resources, print_opts,
                              is_set(mon_ops, mon_op_inactive_resources));
    }

    /* For each resource, display it if appropriate */
    for (rsc_iter = data_set->resources; rsc_iter != NULL; rsc_iter = rsc_iter->next) {
        resource_t *rsc = (resource_t *) rsc_iter->data;

        /* Complex resources may have some sub-resources active and some inactive */
        gboolean is_active = rsc->fns->active(rsc, TRUE);
        gboolean partially_active = rsc->fns->active(rsc, FALSE);

        /* Skip inactive orphans (deleted but still in CIB) */
        if (is_set(rsc->flags, pe_rsc_orphan) && !is_active) {
            continue;

        /* Skip active resources if we already displayed them by node */
        } else if (is_set(mon_ops, mon_op_group_by_node)) {
            if (is_active) {
                continue;
            }

        /* Skip primitives already counted in a brief summary */
        } else if (brief_output && (rsc->variant == pe_native)) {
            continue;

        /* Skip resources that aren't at least partially active,
         * unless we're displaying inactive resources
         */
        } else if (!partially_active && is_not_set(mon_ops, mon_op_inactive_resources)) {
            continue;
        }

        /* Print this resource */
        if (printed_resource == FALSE) {
            printed_resource = TRUE;
        }
        out->message(out, crm_element_name(rsc->xml), print_opts, rsc);
    }

    if (print_summary && !printed_resource) {
        print_resources_closing(out, mon_ops);
    }

    out->end_list(out);

    return TRUE;
}

static int
failure_count(pe_working_set_t *data_set, node_t *node, resource_t *rsc, time_t *last_failure) {
    return rsc ? pe_get_failcount(node, rsc, last_failure, pe_fc_default,
                                  NULL, data_set)
               : 0;
}

static GListPtr
get_operation_list(xmlNode *rsc_entry) {
    GListPtr op_list = NULL;
    xmlNode *rsc_op = NULL;

    for (rsc_op = __xml_first_child_element(rsc_entry); rsc_op != NULL;
         rsc_op = __xml_next_element(rsc_op)) {
        if (crm_str_eq((const char *) rsc_op->name, XML_LRM_TAG_RSC_OP, TRUE)) {
            op_list = g_list_append(op_list, rsc_op);
        }
    }

    op_list = g_list_sort(op_list, sort_op_by_callid);
    return op_list;
}

/*!
 * \internal
 * \brief Print resource operation/failure history
 *
 * \param[in] out       The output functions structure.
 * \param[in] data_set  Cluster state to display.
 * \param[in] node      Node that ran this resource.
 * \param[in] rsc_entry Root of XML tree describing resource status.
 * \param[in] mon_ops   Bitmask of mon_op_options.
 * \param[in] op_list   A list of operations to print.
 */
static void
print_rsc_history(pcmk__output_t *out, pe_working_set_t *data_set, node_t *node,
                  xmlNode *rsc_entry, unsigned int mon_ops, GListPtr op_list)
{
    GListPtr gIter = NULL;
    gboolean printed = FALSE;
    const char *rsc_id = crm_element_value(rsc_entry, XML_ATTR_ID);
    resource_t *rsc = pe_find_resource(data_set->resources, rsc_id);

    /* Print each operation */
    for (gIter = op_list; gIter != NULL; gIter = gIter->next) {
        xmlNode *xml_op = (xmlNode *) gIter->data;
        const char *task = crm_element_value(xml_op, XML_LRM_ATTR_TASK);
        const char *interval_ms_s = crm_element_value(xml_op,
                                                      XML_LRM_ATTR_INTERVAL_MS);
        const char *op_rc = crm_element_value(xml_op, XML_LRM_ATTR_RC);
        int rc = crm_parse_int(op_rc, "0");

        /* Display 0-interval monitors as "probe" */
        if (safe_str_eq(task, CRMD_ACTION_STATUS)
            && ((interval_ms_s == NULL) || safe_str_eq(interval_ms_s, "0"))) {
            task = "probe";
        }

        /* Ignore notifies and some probes */
        if (safe_str_eq(task, CRMD_ACTION_NOTIFY) || (safe_str_eq(task, "probe") && (rc == 7))) {
            continue;
        }

        /* If this is the first printed operation, print heading for resource */
        if (printed == FALSE) {
            time_t last_failure = 0;
            int failcount = failure_count(data_set, node, rsc, &last_failure);

            out->message(out, "resource-history", rsc, rsc_id, TRUE, failcount, last_failure);
            printed = TRUE;
        }

        /* Print the operation */
        out->message(out, "op-history", xml_op, task, interval_ms_s,
                     rc, mon_ops);
    }

    /* Free the list we created (no need to free the individual items) */
    g_list_free(op_list);

    /* If we printed anything, close the resource */
    if (printed) {
        out->end_list(out);
    }
}

/*!
 * \internal
 * \brief Print node operation/failure history
 *
 * \param[in] out        The output functions structure.
 * \param[in] data_set   Cluster state to display.
 * \param[in] node_state Root of XML tree describing node status.
 * \param[in] operations Whether to print operations or just failcounts.
 * \param[in] mon_ops    Bitmask of mon_op_options.
 */
static void
print_node_history(pcmk__output_t *out, pe_working_set_t *data_set,
                   xmlNode *node_state, gboolean operations,
                   unsigned int mon_ops)
{
    node_t *node = pe_find_node_id(data_set->nodes, ID(node_state));
    xmlNode *lrm_rsc = NULL;
    xmlNode *rsc_entry = NULL;
    gboolean printed_header = FALSE;

    if (node && node->details && node->details->online) {
        lrm_rsc = find_xml_node(node_state, XML_CIB_TAG_LRM, FALSE);
        lrm_rsc = find_xml_node(lrm_rsc, XML_LRM_TAG_RESOURCES, FALSE);

        /* Print history of each of the node's resources */
        for (rsc_entry = __xml_first_child_element(lrm_rsc); rsc_entry != NULL;
             rsc_entry = __xml_next_element(rsc_entry)) {

            if (crm_str_eq((const char *)rsc_entry->name, XML_LRM_TAG_RESOURCE, TRUE)) {
                if (operations == FALSE) {
                    const char *rsc_id = crm_element_value(rsc_entry, XML_ATTR_ID);
                    resource_t *rsc = pe_find_resource(data_set->resources, rsc_id);
                    time_t last_failure = 0;
                    int failcount = failure_count(data_set, node, rsc, &last_failure);

                    if (failcount > 0) {
                        if (printed_header == FALSE) {
                            printed_header = TRUE;
                            out->message(out, "node", node, mon_ops, FALSE);
                        }

                        out->message(out, "resource-history", rsc, rsc_id, FALSE,
                                     failcount, last_failure);
                        out->end_list(out);
                    }
                } else {
                    GListPtr op_list = get_operation_list(rsc_entry);

                    if (printed_header == FALSE) {
                        printed_header = TRUE;
                        out->message(out, "node", node, mon_ops, FALSE);
                    }

                    if (g_list_length(op_list) > 0) {
                        print_rsc_history(out, data_set, node, rsc_entry, mon_ops, op_list);
                    }
                }
            }
        }

        if (printed_header) {
            out->end_list(out);
        }
    }
}

/*!
 * \internal
 * \brief Determine whether extended information about an attribute should be added.
 *
 * \param[in]  out            The output functions structure.
 * \param[in]  node           Node that ran this resource.
 * \param[in]  rsc_list       The list of resources for this node.
 * \param[in]  attrname       The attribute to find.
 * \param[out] expected_score The expected value for this attribute.
 *
 * \return TRUE if extended information should be printed, FALSE otherwise
 * \note Currently, extended information is only supported for ping/pingd
 *       resources, for which a message will be printed if connectivity is lost
 *       or degraded.
 */
static gboolean
add_extra_info(pcmk__output_t *out, node_t *node, GListPtr rsc_list,
               const char *attrname, int *expected_score)
{
    GListPtr gIter = NULL;

    for (gIter = rsc_list; gIter != NULL; gIter = gIter->next) {
        resource_t *rsc = (resource_t *) gIter->data;
        const char *type = g_hash_table_lookup(rsc->meta, "type");
        const char *name = NULL;

        if (rsc->children != NULL) {
            if (add_extra_info(out, node, rsc->children, attrname, expected_score)) {
                return TRUE;
            }
        }

        if (safe_str_neq(type, "ping") && safe_str_neq(type, "pingd")) {
            return FALSE;
        }

        name = g_hash_table_lookup(rsc->parameters, "name");

        if (name == NULL) {
            name = "pingd";
        }

        /* To identify the resource with the attribute name. */
        if (safe_str_eq(name, attrname)) {
            int host_list_num = 0;
            /* int value = crm_parse_int(attrvalue, "0"); */
            const char *hosts = g_hash_table_lookup(rsc->parameters, "host_list");
            const char *multiplier = g_hash_table_lookup(rsc->parameters, "multiplier");

            if (hosts) {
                char **host_list = g_strsplit(hosts, " ", 0);
                host_list_num = g_strv_length(host_list);
                g_strfreev(host_list);
            }

            /* pingd multiplier is the same as the default value. */
            *expected_score = host_list_num * crm_parse_int(multiplier, "1");

            return TRUE;
        }
    }
    return FALSE;
}

/* structure for passing multiple user data to g_list_foreach() */
struct mon_attr_data {
    pcmk__output_t *out;
    node_t *node;
};

static void
print_node_attribute(gpointer name, gpointer user_data)
{
    const char *value = NULL;
    int expected_score = 0;
    gboolean add_extra = FALSE;
    struct mon_attr_data *data = (struct mon_attr_data *) user_data;

    value = pe_node_attribute_raw(data->node, name);

    add_extra = add_extra_info(data->out, data->node, data->node->details->running_rsc,
                               name, &expected_score);

    /* Print attribute name and value */
    data->out->message(data->out, "node-attribute", name, value, add_extra,
                       expected_score);
}

/*!
 * \internal
 * \brief Print history for all nodes.
 *
 * \param[in] out        The output functions structure.
 * \param[in] data_set   Cluster state to display.
 * \param[in] operations Whether to print operations or just failcounts.
 * \param[in] mon_ops    Bitmask of mon_op_options.
 */
static gboolean
print_node_summary(pcmk__output_t *out, pe_working_set_t * data_set,
                   gboolean operations, unsigned int mon_ops)
{
    xmlNode *node_state = NULL;
    xmlNode *cib_status = get_object_root(XML_CIB_TAG_STATUS, data_set->input);

    if (xmlChildElementCount(cib_status) == 0) {
        return FALSE;
    }

    /* Print heading */
    if (operations) {
        out->begin_list(out, NULL, NULL, "Operations");
    } else {
        out->begin_list(out, NULL, NULL, "Migration Summary");
    }

    /* Print each node in the CIB status */
    for (node_state = __xml_first_child_element(cib_status); node_state != NULL;
         node_state = __xml_next_element(node_state)) {
        if (crm_str_eq((const char *)node_state->name, XML_CIB_TAG_STATE, TRUE)) {
            print_node_history(out, data_set, node_state, operations, mon_ops);
        }
    }

    /* Close section */
    out->end_list(out);
    return TRUE;
}

/*!
 * \internal
 * \brief Print all tickets.
 *
 * \param[in] out      The output functions structure.
 * \param[in] data_set Cluster state to display.
 */
static gboolean
print_cluster_tickets(pcmk__output_t *out, pe_working_set_t * data_set)
{
    GHashTableIter iter;
    gpointer key, value;

    if (g_hash_table_size(data_set->tickets) == 0) {
        return FALSE;
    }

    /* Print section heading */
    out->begin_list(out, NULL, NULL, "Tickets");

    /* Print each ticket */
    g_hash_table_iter_init(&iter, data_set->tickets);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        ticket_t *ticket = (ticket_t *) value;
        out->message(out, "ticket", ticket);
    }

    /* Close section */
    out->end_list(out);
    return TRUE;
}

/*!
 * \internal
 * \brief Print section for negative location constraints
 *
 * \param[in] out      The output functions structure.
 * \param[in] data_set Cluster state to display.
 * \param[in] mon_ops  Bitmask of mon_op_options.
 * \param[in] prefix   ID prefix to filter results by.
 */
static gboolean
print_neg_locations(pcmk__output_t *out, pe_working_set_t *data_set, unsigned int mon_ops,
                    const char *prefix)
{
    GListPtr gIter, gIter2;
    gboolean printed_header = FALSE;

    /* Print each ban */
    for (gIter = data_set->placement_constraints; gIter != NULL; gIter = gIter->next) {
        pe__location_t *location = gIter->data;
        if (!g_str_has_prefix(location->id, prefix))
            continue;
        for (gIter2 = location->node_list_rh; gIter2 != NULL; gIter2 = gIter2->next) {
            node_t *node = (node_t *) gIter2->data;

            if (node->weight < 0) {
                if (printed_header == FALSE) {
                    printed_header = TRUE;
                    out->begin_list(out, NULL, NULL, "Negative Location Constraints");
                }

                out->message(out, "ban", node, location, mon_ops);
            }
        }
    }

    if (printed_header) {
        out->end_list(out);
        return TRUE;
    } else {
        return FALSE;
    }
}

/*!
 * \internal
 * \brief Print node attributes section
 *
 * \param[in] out      The output functions structure.
 * \param[in] data_set Cluster state to display.
 * \param[in] mon_ops  Bitmask of mon_op_options.
 */
static gboolean
print_node_attributes(pcmk__output_t *out, pe_working_set_t *data_set, unsigned int mon_ops)
{
    GListPtr gIter = NULL;
    gboolean printed_header = FALSE;

    /* Unpack all resource parameters (it would be more efficient to do this
     * only when needed for the first time in add_extra_info())
     */
    for (gIter = data_set->resources; gIter != NULL; gIter = gIter->next) {
        crm_mon_get_parameters(gIter->data, data_set);
    }

    /* Display each node's attributes */
    for (gIter = data_set->nodes; gIter != NULL; gIter = gIter->next) {
        struct mon_attr_data data;

        data.out = out;
        data.node = (node_t *) gIter->data;

        if (data.node && data.node->details && data.node->details->online) {
            GList *attr_list = NULL;
            GHashTableIter iter;
            gpointer key, value;

            g_hash_table_iter_init(&iter, data.node->details->attrs);
            while (g_hash_table_iter_next (&iter, &key, &value)) {
                attr_list = append_attr_list(attr_list, key);
            }

            if (g_list_length(attr_list) == 0) {
                g_list_free(attr_list);
                continue;
            }

            if (printed_header == FALSE) {
                printed_header = TRUE;
                out->begin_list(out, NULL, NULL, "Node Attributes");
            }

            out->message(out, "node", data.node, mon_ops, FALSE);
            g_list_foreach(attr_list, print_node_attribute, &data);
            g_list_free(attr_list);
            out->end_list(out);
        }
    }

    /* Print section footer */
    if (printed_header) {
        out->end_list(out);
        return TRUE;
    } else {
        return FALSE;
    }
}

/*!
 * \internal
 * \brief Print times the display was last updated and CIB last changed
 *
 * \param[in] out      The output functions structure.
 * \param[in] data_set Cluster state to display.
 */
static void
print_cluster_times(pcmk__output_t *out, pe_working_set_t *data_set)
{
    const char *last_written = crm_element_value(data_set->input, XML_CIB_ATTR_WRITTEN);
    const char *user = crm_element_value(data_set->input, XML_ATTR_UPDATE_USER);
    const char *client = crm_element_value(data_set->input, XML_ATTR_UPDATE_CLIENT);
    const char *origin = crm_element_value(data_set->input, XML_ATTR_UPDATE_ORIG);

    out->message(out, "cluster-times", last_written, user, client, origin);
}

/*!
 * \internal
 * \brief Print current DC and its version
 *
 * \param[in] out      The output functions structure.
 * \param[in] data_set Cluster state to display.
 * \param[in] mon_ops  Bitmask of mon_op_options.
 */
static void
print_cluster_dc(pcmk__output_t *out, pe_working_set_t *data_set, unsigned int mon_ops)
{
    node_t *dc = data_set->dc_node;
    xmlNode *dc_version = get_xpath_object("//nvpair[@name='dc-version']",
                                           data_set->input, LOG_DEBUG);
    const char *dc_version_s = dc_version?
                               crm_element_value(dc_version, XML_NVPAIR_ATTR_VALUE)
                               : NULL;
    const char *quorum = crm_element_value(data_set->input, XML_ATTR_HAVE_QUORUM);
    char *dc_name = dc? get_node_display_name(dc, mon_ops) : NULL;

    out->message(out, "cluster-dc", dc, quorum, dc_version_s, dc_name);
    free(dc_name);
}

/*!
 * \internal
 * \brief Print a summary of cluster-wide information
 *
 * \param[in] out      The output functions structure.
 * \param[in] data_set Cluster state to display.
 * \param[in] mon_ops  Bitmask of mon_op_options.
 * \param[in] show     Bitmask of mon_show_options.
 */
static void
print_cluster_summary(pcmk__output_t *out, pe_working_set_t *data_set,
                      unsigned int mon_ops, unsigned int show)
{
    const char *stack_s = get_cluster_stack(data_set);
    gboolean header_printed = FALSE;

    if (show & mon_show_stack) {
        if (header_printed == FALSE) {
            out->begin_list(out, NULL, NULL, "Cluster Summary");
            header_printed = TRUE;
        }
        out->message(out, "cluster-stack", stack_s);
    }

    /* Always print DC if none, even if not requested */
    if ((data_set->dc_node == NULL) || (show & mon_show_dc)) {
        if (header_printed == FALSE) {
            out->begin_list(out, NULL, NULL, "Cluster Summary");
            header_printed = TRUE;
        }
        print_cluster_dc(out, data_set, mon_ops);
    }

    if (show & mon_show_times) {
        if (header_printed == FALSE) {
            out->begin_list(out, NULL, NULL, "Cluster Summary");
            header_printed = TRUE;
        }
        print_cluster_times(out, data_set);
    }

    if (is_set(data_set->flags, pe_flag_maintenance_mode)
        || data_set->disabled_resources
        || data_set->blocked_resources
        || is_set(show, mon_show_count)) {
        if (header_printed == FALSE) {
            out->begin_list(out, NULL, NULL, "Cluster Summary");
            header_printed = TRUE;
        }
        out->message(out, "cluster-counts", g_list_length(data_set->nodes),
                     count_resources(data_set, NULL), data_set->disabled_resources,
                     data_set->blocked_resources);
    }

    /* There is not a separate option for showing cluster options, so show with
     * stack for now; a separate option could be added if there is demand
     */
    if (show & mon_show_stack) {
        out->message(out, "cluster-options", data_set);
    }

    if (header_printed) {
        out->end_list(out);
    }
}

/*!
 * \internal
 * \brief Print a section for failed actions
 *
 * \param[in] out      The output functions structure.
 * \param[in] data_set Cluster state to display.
 */
static gboolean
print_failed_actions(pcmk__output_t *out, pe_working_set_t *data_set)
{
    xmlNode *xml_op = NULL;

    if (xmlChildElementCount(data_set->failed) == 0) {
        return FALSE;
    }

    /* Print section heading */
    out->begin_list(out, NULL, NULL, "Failed Resource Actions");

    /* Print each failed action */
    for (xml_op = __xml_first_child(data_set->failed); xml_op != NULL;
         xml_op = __xml_next(xml_op)) {
        out->message(out, "failed-action", xml_op);
        out->increment_list(out);
    }

    /* End section */
    out->end_list(out);
    return TRUE;
}

/*!
 * \internal
 * \brief Print a section for failed stonith actions
 *
 * \note This function should not be called for XML output.
 *
 * \param[in] out      The output functions structure.
 * \param[in] history  List of stonith actions.
 * \param[in] mon_ops  Bitmask of mon_op_options.
 */
static gboolean
print_failed_stonith_actions(pcmk__output_t *out, stonith_history_t *history, unsigned int mon_ops)
{
    stonith_history_t *hp;

    for (hp = history; hp; hp = hp->next) {
        if (hp->state == st_failed) {
            break;
        }
    }
    if (!hp) {
        return FALSE;
    }

    /* Print section heading */
    out->begin_list(out, NULL, NULL, "Failed Fencing Actions");

    /* Print each failed stonith action */
    for (hp = history; hp; hp = hp->next) {
        if (hp->state == st_failed) {
            out->message(out, "stonith-event", hp, mon_ops & mon_op_fence_full_history, history);
            out->increment_list(out);
        }
    }

    /* End section */
    out->end_list(out);

    return TRUE;
}

/*!
 * \internal
 * \brief Print pending stonith actions
 *
 * \note This function should not be called for XML output.
 *
 * \param[in] out      The output functions structure.
 * \param[in] history  List of stonith actions.
 * \param[in] mon_ops  Bitmask of mon_op_options.
 */
static gboolean
print_stonith_pending(pcmk__output_t *out, stonith_history_t *history, unsigned int mon_ops)
{
    /* xml-output always shows the full history
     * so we'll never have to show pending-actions
     * separately
     */
    if (history && (history->state != st_failed) &&
        (history->state != st_done)) {
        stonith_history_t *hp;

        /* Print section heading */
        out->begin_list(out, NULL, NULL, "Pending Fencing Actions");

        history = stonith__sort_history(history);
        for (hp = history; hp; hp = hp->next) {
            if ((hp->state == st_failed) || (hp->state == st_done)) {
                break;
            }
            out->message(out, "stonith-event", hp, mon_ops & mon_op_fence_full_history, NULL);
            out->increment_list(out);
        }

        /* End section */
        out->end_list(out);

        return TRUE;
    }

    return FALSE;
}

/*!
 * \internal
 * \brief Print fencing history, skipping all failed actions.
 *
 * \note This function should not be called for XML output.
 *
 * \param[in] out      The output functions structure.
 * \param[in] history  List of stonith actions.
 * \param[in] mon_ops  Bitmask of mon_op_options.
 */
static gboolean
print_stonith_history(pcmk__output_t *out, stonith_history_t *history, unsigned int mon_ops)
{
    stonith_history_t *hp;

    if (history == NULL) {
        return FALSE;
    }

    /* Print section heading */
    out->begin_list(out, NULL, NULL, "Fencing History");

    stonith__sort_history(history);
    for (hp = history; hp; hp = hp->next) {
        if (hp->state != st_failed) {
            out->message(out, "stonith-event", hp, mon_ops & mon_op_fence_full_history, NULL);
            out->increment_list(out);
        }
    }

    /* End section */
    out->end_list(out);
    return TRUE;
}

/*!
 * \internal
 * \brief Print fencing history, including failed actions.
 *
 * \note This function should be called for XML output.  It may also be
 *       interesting for other output formats.
 *
 * \param[in] out      The output functions structure.
 * \param[in] history  List of stonith actions.
 * \param[in] mon_ops  Bitmask of mon_op_options.
 */
static gboolean
print_stonith_history_full(pcmk__output_t *out, stonith_history_t *history, unsigned int mon_ops)
{
    stonith_history_t *hp;

    if (history == NULL) {
        return FALSE;
    }

    /* Print section heading */
    out->begin_list(out, NULL, NULL, "Fencing History");

    stonith__sort_history(history);
    for (hp = history; hp; hp = hp->next) {
        out->message(out, "stonith-event", hp, mon_ops & mon_op_fence_full_history, NULL);
        out->increment_list(out);
    }

    /* End section */
    out->end_list(out);
    return TRUE;
}

/*!
 * \internal
 * \brief Top-level printing function for text/curses output.
 *
 * \param[in] out             The output functions structure.
 * \param[in] output_format   Is this text or curses output?
 * \param[in] data_set        Cluster state to display.
 * \param[in] stonith_history List of stonith actions.
 * \param[in] mon_ops         Bitmask of mon_op_options.
 * \param[in] show            Bitmask of mon_show_options.
 * \param[in] prefix          ID prefix to filter results by.
 */
void
print_status(pcmk__output_t *out, mon_output_format_t output_format,
             pe_working_set_t *data_set, stonith_history_t *stonith_history,
             unsigned int mon_ops, unsigned int show, const char *prefix)
{
    GListPtr gIter = NULL;
    int print_opts = get_resource_display_options(mon_ops, output_format);
    gboolean printed = FALSE;

    /* space-separated lists of node names */
    char *online_nodes = NULL;
    char *online_remote_nodes = NULL;
    char *online_guest_nodes = NULL;
    char *offline_nodes = NULL;
    char *offline_remote_nodes = NULL;

    print_cluster_summary(out, data_set, mon_ops, show);

    if (is_set(show, mon_show_headers)) {
        out->info(out, "%s", "");
    }

    /* Gather node information (and print if in bad state or grouping by node) */
    out->begin_list(out, NULL, NULL, "Node List");
    for (gIter = data_set->nodes; gIter != NULL; gIter = gIter->next) {
        node_t *node = (node_t *) gIter->data;
        const char *node_mode = NULL;
        char *node_name = get_node_display_name(node, mon_ops);

        /* Get node mode */
        if (node->details->unclean) {
            if (node->details->online) {
                node_mode = "UNCLEAN (online)";

            } else if (node->details->pending) {
                node_mode = "UNCLEAN (pending)";

            } else {
                node_mode = "UNCLEAN (offline)";
            }

        } else if (node->details->pending) {
            node_mode = "pending";

        } else if (node->details->standby_onfail && node->details->online) {
            node_mode = "standby (on-fail)";

        } else if (node->details->standby) {
            if (node->details->online) {
                if (node->details->running_rsc) {
                    node_mode = "standby (with active resources)";
                } else {
                    node_mode = "standby";
                }
            } else {
                node_mode = "OFFLINE (standby)";
            }

        } else if (node->details->maintenance) {
            if (node->details->online) {
                node_mode = "maintenance";
            } else {
                node_mode = "OFFLINE (maintenance)";
            }

        } else if (node->details->online) {
            node_mode = "online";
            if (is_not_set(mon_ops, mon_op_group_by_node)) {
                if (pe__is_guest_node(node)) {
                    online_guest_nodes = add_list_element(online_guest_nodes, node_name);
                } else if (pe__is_remote_node(node)) {
                    online_remote_nodes = add_list_element(online_remote_nodes, node_name);
                } else {
                    online_nodes = add_list_element(online_nodes, node_name);
                }
                free(node_name);
                continue;
            }

        } else {
            node_mode = "OFFLINE";
            if (is_not_set(mon_ops, mon_op_group_by_node)) {
                if (pe__is_remote_node(node)) {
                    offline_remote_nodes = add_list_element(offline_remote_nodes, node_name);
                } else if (pe__is_guest_node(node)) {
                    /* ignore offline guest nodes */
                } else {
                    offline_nodes = add_list_element(offline_nodes, node_name);
                }
                free(node_name);
                continue;
            }
        }

        /* If we get here, node is in bad state, or we're grouping by node */
        out->message(out, "node", node, mon_ops, TRUE, node_mode);
    }

    /* If we're not grouping by node, summarize nodes by status */
    if (online_nodes) {
        out->list_item(out, "Online", "[%s ]", online_nodes);
        free(online_nodes);
    }
    if (offline_nodes) {
        out->list_item(out, "OFFLINE", "[%s ]", offline_nodes);
        free(offline_nodes);
    }
    if (online_remote_nodes) {
        out->list_item(out, "RemoteOnline", "[%s ]", online_remote_nodes);
        free(online_remote_nodes);
    }
    if (offline_remote_nodes) {
        out->list_item(out, "RemoteOFFLINE", "[%s ]", offline_remote_nodes);
        free(offline_remote_nodes);
    }
    if (online_guest_nodes) {
        out->list_item(out, "GuestOnline", "[%s ]", online_guest_nodes);
        free(online_guest_nodes);
    }

    out->end_list(out);
    out->info(out, "%s", "");

    /* Print resources section, if needed */
    printed = print_resources(out, data_set, print_opts, mon_ops,
                              is_set(mon_ops, mon_op_print_brief), TRUE);

    /* print Node Attributes section if requested */
    if (show & mon_show_attributes) {
        if (printed) {
            out->info(out, "%s", "");
        }

        printed = print_node_attributes(out, data_set, mon_ops);
    }

    /* If requested, print resource operations (which includes failcounts)
     * or just failcounts
     */
    if (show & (mon_show_operations | mon_show_failcounts)) {
        if (printed) {
            out->info(out, "%s", "");
        }

        printed = print_node_summary(out, data_set,
                                     ((show & mon_show_operations)? TRUE : FALSE), mon_ops);
    }

    /* If there were any failed actions, print them */
    if (xml_has_children(data_set->failed)) {
        if (printed) {
            out->info(out, "%s", "");
        }

        printed = print_failed_actions(out, data_set);
    }

    /* Print failed stonith actions */
    if (is_set(mon_ops, mon_op_fence_history)) {
        if (printed) {
            out->info(out, "%s", "");
        }

        printed = print_failed_stonith_actions(out, stonith_history, mon_ops);
    }

    /* Print tickets if requested */
    if (show & mon_show_tickets) {
        if (printed) {
            out->info(out, "%s", "");
        }

        printed = print_cluster_tickets(out, data_set);
    }

    /* Print negative location constraints if requested */
    if (show & mon_show_bans) {
        if (printed) {
            out->info(out, "%s", "");
        }

        printed = print_neg_locations(out, data_set, mon_ops, prefix);
    }

    /* Print stonith history */
    if (is_set(mon_ops, mon_op_fence_history)) {
        if (printed) {
            out->info(out, "%s", "");
        }

        if (show & mon_show_fence_history) {
            print_stonith_history(out, stonith_history, mon_ops);
        } else {
            print_stonith_pending(out, stonith_history, mon_ops);
        }
    }
}

/*!
 * \internal
 * \brief Top-level printing function for XML output.
 *
 * \param[in] out             The output functions structure.
 * \param[in] data_set        Cluster state to display.
 * \param[in] stonith_history List of stonith actions.
 * \param[in] mon_ops         Bitmask of mon_op_options.
 * \param[in] show            Bitmask of mon_show_options.
 * \param[in] prefix          ID prefix to filter results by.
 */
void
print_xml_status(pcmk__output_t *out, pe_working_set_t *data_set,
                 stonith_history_t *stonith_history, unsigned int mon_ops,
                 unsigned int show, const char *prefix)
{
    GListPtr gIter = NULL;
    int print_opts = get_resource_display_options(mon_ops, mon_output_xml);

    print_cluster_summary(out, data_set, mon_ops, show);

    /*** NODES ***/
    out->begin_list(out, NULL, NULL, "nodes");
    for (gIter = data_set->nodes; gIter != NULL; gIter = gIter->next) {
        node_t *node = (node_t *) gIter->data;
        out->message(out, "node", node, mon_ops, TRUE);
    }
    out->end_list(out);

    /* Print resources section, if needed */
    print_resources(out, data_set, print_opts, mon_ops, FALSE, FALSE);

    /* print Node Attributes section if requested */
    if (show & mon_show_attributes) {
        print_node_attributes(out, data_set, mon_ops);
    }

    /* If requested, print resource operations (which includes failcounts)
     * or just failcounts
     */
    if (show & (mon_show_operations | mon_show_failcounts)) {
        print_node_summary(out, data_set,
                           ((show & mon_show_operations)? TRUE : FALSE), mon_ops);
    }

    /* If there were any failed actions, print them */
    if (xml_has_children(data_set->failed)) {
        print_failed_actions(out, data_set);
    }

    /* Print stonith history */
    if (is_set(mon_ops, mon_op_fence_history)) {
        print_stonith_history_full(out, stonith_history, mon_ops);
    }

    /* Print tickets if requested */
    if (show & mon_show_tickets) {
        print_cluster_tickets(out, data_set);
    }

    /* Print negative location constraints if requested */
    if (show & mon_show_bans) {
        print_neg_locations(out, data_set, mon_ops, prefix);
    }
}

/*!
 * \internal
 * \brief Top-level printing function for HTML output.
 *
 * \param[in] out             The output functions structure.
 * \param[in] output_format   Is this HTML or CGI output?
 * \param[in] data_set        Cluster state to display.
 * \param[in] stonith_history List of stonith actions.
 * \param[in] mon_ops         Bitmask of mon_op_options.
 * \param[in] show            Bitmask of mon_show_options.
 * \param[in] prefix          ID prefix to filter results by.
 */
int
print_html_status(pcmk__output_t *out, mon_output_format_t output_format,
                  pe_working_set_t *data_set, stonith_history_t *stonith_history,
                  unsigned int mon_ops, unsigned int show, const char *prefix)
{
    GListPtr gIter = NULL;
    int print_opts = get_resource_display_options(mon_ops, output_format);

    print_cluster_summary(out, data_set, mon_ops, show);

    /*** NODE LIST ***/
    out->begin_list(out, NULL, NULL, "Node List");
    for (gIter = data_set->nodes; gIter != NULL; gIter = gIter->next) {
        node_t *node = (node_t *) gIter->data;
        out->message(out, "node", node, mon_ops, TRUE);
    }
    out->end_list(out);

    /* Print resources section, if needed */
    print_resources(out, data_set, print_opts, mon_ops,
                    is_set(mon_ops, mon_op_print_brief), TRUE);

    /* print Node Attributes section if requested */
    if (show & mon_show_attributes) {
        print_node_attributes(out, data_set, mon_ops);
    }

    /* If requested, print resource operations (which includes failcounts)
     * or just failcounts
     */
    if (show & (mon_show_operations | mon_show_failcounts)) {
        print_node_summary(out, data_set,
                           ((show & mon_show_operations)? TRUE : FALSE), mon_ops);
    }

    /* If there were any failed actions, print them */
    if (xml_has_children(data_set->failed)) {
        print_failed_actions(out, data_set);
    }

    /* Print failed stonith actions */
    if (is_set(mon_ops, mon_op_fence_history)) {
        print_failed_stonith_actions(out, stonith_history, mon_ops);
    }

    /* Print stonith history */
    if (is_set(mon_ops, mon_op_fence_history)) {
        if (show & mon_show_fence_history) {
            print_stonith_history(out, stonith_history, mon_ops);
        } else {
            print_stonith_pending(out, stonith_history, mon_ops);
        }
    }

    /* Print tickets if requested */
    if (show & mon_show_tickets) {
        print_cluster_tickets(out, data_set);
    }

    /* Print negative location constraints if requested */
    if (show & mon_show_bans) {
        print_neg_locations(out, data_set, mon_ops, prefix);
    }

    return 0;
}
