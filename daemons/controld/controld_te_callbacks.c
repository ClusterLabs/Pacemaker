/*
 * Copyright 2004-2024 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU General Public License version 2
 * or later (GPLv2+) WITHOUT ANY WARRANTY.
 */

#include <crm_internal.h>

#include <sys/stat.h>

#include <crm/crm.h>
#include <crm/common/xml.h>
#include <crm/common/xml_internal.h>

#include <pacemaker-controld.h>

void te_update_confirm(const char *event, xmlNode * msg);

#define RSC_OP_PREFIX "//" PCMK__XE_DIFF_ADDED "//" PCMK_XE_CIB \
                      "//" PCMK__XE_LRM_RSC_OP "[@" PCMK_XA_ID "='"

// An explicit PCMK_OPT_SHUTDOWN_LOCK of 0 means the lock has been cleared
static bool
shutdown_lock_cleared(xmlNode *lrm_resource)
{
    time_t shutdown_lock = 0;

    return (crm_element_value_epoch(lrm_resource, PCMK_OPT_SHUTDOWN_LOCK,
                                    &shutdown_lock) == pcmk_ok)
           && (shutdown_lock == 0);
}

static void
te_update_diff_v1(const char *event, xmlNode *diff)
{
    int lpc, max;
    xmlXPathObject *xpathObj = NULL;
    GString *rsc_op_xpath = NULL;

    CRM_CHECK(diff != NULL, return);

    pcmk__output_set_log_level(controld_globals.logger_out, LOG_TRACE);
    controld_globals.logger_out->message(controld_globals.logger_out,
                                         "xml-patchset", diff);

    if (cib__config_changed_v1(NULL, NULL, &diff)) {
        abort_transition(PCMK_SCORE_INFINITY, pcmk__graph_restart,
                         "Non-status change", diff);
        goto bail;              /* configuration changed */
    }

    /* Tickets Attributes - Added/Updated */
    xpathObj =
        xpath_search(diff,
                     "//" PCMK__XA_CIB_UPDATE_RESULT
                     "//" PCMK__XE_DIFF_ADDED
                     "//" PCMK_XE_TICKETS);
    if (numXpathResults(xpathObj) > 0) {
        xmlNode *aborted = getXpathResult(xpathObj, 0);

        abort_transition(PCMK_SCORE_INFINITY, pcmk__graph_restart,
                         "Ticket attribute: update", aborted);
        goto bail;

    }
    freeXpathObject(xpathObj);

    /* Tickets Attributes - Removed */
    xpathObj =
        xpath_search(diff,
                     "//" PCMK__XA_CIB_UPDATE_RESULT
                     "//" PCMK__XE_DIFF_REMOVED
                     "//" PCMK_XE_TICKETS);
    if (numXpathResults(xpathObj) > 0) {
        xmlNode *aborted = getXpathResult(xpathObj, 0);

        abort_transition(PCMK_SCORE_INFINITY, pcmk__graph_restart,
                         "Ticket attribute: removal", aborted);
        goto bail;
    }
    freeXpathObject(xpathObj);

    /* Transient Attributes - Removed */
    xpathObj =
        xpath_search(diff,
                     "//" PCMK__XA_CIB_UPDATE_RESULT
                     "//" PCMK__XE_DIFF_REMOVED
                     "//" PCMK__XE_TRANSIENT_ATTRIBUTES);
    if (numXpathResults(xpathObj) > 0) {
        xmlNode *aborted = getXpathResult(xpathObj, 0);

        abort_transition(PCMK_SCORE_INFINITY, pcmk__graph_restart,
                         "Transient attribute: removal", aborted);
        goto bail;

    }
    freeXpathObject(xpathObj);

    // Check for PCMK__XE_LRM_RESOURCE entries
    xpathObj = xpath_search(diff,
                            "//" PCMK__XA_CIB_UPDATE_RESULT
                            "//" PCMK__XE_DIFF_ADDED
                            "//" PCMK__XE_LRM_RESOURCE);
    max = numXpathResults(xpathObj);

    /*
     * Updates by, or in response to, graph actions will never affect more than
     * one resource at a time, so such updates indicate an LRM refresh. In that
     * case, start a new transition rather than check each result individually,
     * which can result in _huge_ speedups in large clusters.
     *
     * Unfortunately, we can only do so when there are no pending actions.
     * Otherwise, we could mistakenly throw away those results here, and
     * the cluster will stall waiting for them and time out the operation.
     */
    if ((controld_globals.transition_graph->pending == 0) && (max > 1)) {
        crm_debug("Ignoring resource operation updates due to history refresh of %d resources",
                  max);
        crm_log_xml_trace(diff, "lrm-refresh");
        abort_transition(PCMK_SCORE_INFINITY, pcmk__graph_restart,
                         "History refresh", NULL);
        goto bail;
    }

    if (max == 1) {
        xmlNode *lrm_resource = getXpathResult(xpathObj, 0);

        if (shutdown_lock_cleared(lrm_resource)) {
            // @TODO would be more efficient to abort once after transition done
            abort_transition(PCMK_SCORE_INFINITY, pcmk__graph_restart,
                             "Shutdown lock cleared", lrm_resource);
            // Still process results, so we stop timers and update failcounts
        }
    }
    freeXpathObject(xpathObj);

    /* Process operation updates */
    xpathObj =
        xpath_search(diff,
                     "//" PCMK__XA_CIB_UPDATE_RESULT
                     "//" PCMK__XE_DIFF_ADDED
                     "//" PCMK__XE_LRM_RSC_OP);
    max = numXpathResults(xpathObj);
    if (max > 0) {
        int lpc = 0;

        for (lpc = 0; lpc < max; lpc++) {
            xmlNode *rsc_op = getXpathResult(xpathObj, lpc);
            const char *node = get_node_id(rsc_op);

            process_graph_event(rsc_op, node);
        }
    }
    freeXpathObject(xpathObj);

    /* Detect deleted (as opposed to replaced or added) actions - eg. crm_resource -C */
    xpathObj = xpath_search(diff,
                            "//" PCMK__XE_DIFF_REMOVED
                            "//" PCMK__XE_LRM_RSC_OP);
    max = numXpathResults(xpathObj);
    for (lpc = 0; lpc < max; lpc++) {
        const char *op_id = NULL;
        xmlXPathObject *op_match = NULL;
        xmlNode *match = getXpathResult(xpathObj, lpc);

        CRM_LOG_ASSERT(match != NULL);
        if(match == NULL) { continue; };

        op_id = pcmk__xe_id(match);

        if (rsc_op_xpath == NULL) {
            rsc_op_xpath = g_string_new(RSC_OP_PREFIX);
        } else {
            g_string_truncate(rsc_op_xpath, sizeof(RSC_OP_PREFIX) - 1);
        }
        pcmk__g_strcat(rsc_op_xpath, op_id, "']", NULL);

        op_match = xpath_search(diff, (const char *) rsc_op_xpath->str);
        if (numXpathResults(op_match) == 0) {
            /* Prevent false positives by matching cancelations too */
            const char *node = get_node_id(match);
            pcmk__graph_action_t *cancelled = get_cancel_action(op_id, node);

            if (cancelled == NULL) {
                crm_debug("No match for deleted action %s (%s on %s)",
                          (const char *) rsc_op_xpath->str, op_id, node);
                abort_transition(PCMK_SCORE_INFINITY, pcmk__graph_restart,
                                 "Resource op removal", match);
                freeXpathObject(op_match);
                goto bail;

            } else {
                crm_debug("Deleted " PCMK__XE_LRM_RSC_OP " %s on %s was for "
                          "graph event %d",
                          op_id, node, cancelled->id);
            }
        }

        freeXpathObject(op_match);
    }

  bail:
    freeXpathObject(xpathObj);
    if (rsc_op_xpath != NULL) {
        g_string_free(rsc_op_xpath, TRUE);
    }
}

static void
process_lrm_resource_diff(xmlNode *lrm_resource, const char *node)
{
    for (xmlNode *rsc_op = pcmk__xml_first_child(lrm_resource); rsc_op != NULL;
         rsc_op = pcmk__xml_next(rsc_op)) {
        process_graph_event(rsc_op, node);
    }
    if (shutdown_lock_cleared(lrm_resource)) {
        // @TODO would be more efficient to abort once after transition done
        abort_transition(PCMK_SCORE_INFINITY, pcmk__graph_restart,
                         "Shutdown lock cleared", lrm_resource);
    }
}

static void
process_resource_updates(const char *node, xmlNode *xml, xmlNode *change,
                         const char *op, const char *xpath)
{
    xmlNode *rsc = NULL;

    if (xml == NULL) {
        return;
    }

    if (pcmk__xe_is(xml, PCMK__XE_LRM)) {
        xml = pcmk__xe_match_name(xml, PCMK__XE_LRM_RESOURCES);
        CRM_CHECK(xml != NULL, return);
    }

    CRM_CHECK(pcmk__xe_is(xml, PCMK__XE_LRM_RESOURCES), return);

    /*
     * Updates by, or in response to, TE actions will never contain updates
     * for more than one resource at a time, so such updates indicate an
     * LRM refresh.
     *
     * In that case, start a new transition rather than check each result
     * individually, which can result in _huge_ speedups in large clusters.
     *
     * Unfortunately, we can only do so when there are no pending actions.
     * Otherwise, we could mistakenly throw away those results here, and
     * the cluster will stall waiting for them and time out the operation.
     */
    if ((controld_globals.transition_graph->pending == 0)
        && (xml->children != NULL) && (xml->children->next != NULL)) {

        crm_log_xml_trace(change, "lrm-refresh");
        abort_transition(PCMK_SCORE_INFINITY, pcmk__graph_restart,
                         "History refresh", NULL);
        return;
    }

    for (rsc = pcmk__xml_first_child(xml); rsc != NULL;
         rsc = pcmk__xml_next(rsc)) {
        crm_trace("Processing %s", pcmk__xe_id(rsc));
        process_lrm_resource_diff(rsc, node);
    }
}

static char *extract_node_uuid(const char *xpath) 
{
    char *mutable_path = strdup(xpath);
    char *node_uuid = NULL;
    char *search = NULL;
    char *match = NULL;

    match = strstr(mutable_path, PCMK__XE_NODE_STATE "[@" PCMK_XA_ID "=\'");
    if (match == NULL) {
        free(mutable_path);
        return NULL;
    }
    match += strlen(PCMK__XE_NODE_STATE "[@" PCMK_XA_ID "=\'");

    search = strchr(match, '\'');
    if (search == NULL) {
        free(mutable_path);
        return NULL;
    }
    search[0] = 0;

    node_uuid = strdup(match);
    free(mutable_path);
    return node_uuid;
}

static void
abort_unless_down(const char *xpath, const char *op, xmlNode *change,
                  const char *reason)
{
    char *node_uuid = NULL;
    pcmk__graph_action_t *down = NULL;

    if (!pcmk__str_eq(op, PCMK_VALUE_DELETE, pcmk__str_none)) {
        abort_transition(PCMK_SCORE_INFINITY, pcmk__graph_restart, reason,
                         change);
        return;
    }

    node_uuid = extract_node_uuid(xpath);
    if(node_uuid == NULL) {
        crm_err("Could not extract node ID from %s", xpath);
        abort_transition(PCMK_SCORE_INFINITY, pcmk__graph_restart, reason,
                         change);
        return;
    }

    down = match_down_event(node_uuid);
    if (down == NULL) {
        crm_trace("Not expecting %s to be down (%s)", node_uuid, xpath);
        abort_transition(PCMK_SCORE_INFINITY, pcmk__graph_restart, reason,
                         change);
    } else {
        crm_trace("Expecting changes to %s (%s)", node_uuid, xpath);
    }
    free(node_uuid);
}

static void
process_op_deletion(const char *xpath, xmlNode *change)
{
    char *mutable_key = strdup(xpath);
    char *key;
    char *node_uuid;

    // Extract the part of xpath between last pair of single quotes
    key = strrchr(mutable_key, '\'');
    if (key != NULL) {
        *key = '\0';
        key = strrchr(mutable_key, '\'');
    }
    if (key == NULL) {
        crm_warn("Ignoring malformed CIB update (resource deletion of %s)",
                 xpath);
        free(mutable_key);
        return;
    }
    ++key;

    node_uuid = extract_node_uuid(xpath);
    if (confirm_cancel_action(key, node_uuid) == FALSE) {
        abort_transition(PCMK_SCORE_INFINITY, pcmk__graph_restart,
                         "Resource operation removal", change);
    }
    free(mutable_key);
    free(node_uuid);
}

static void
process_delete_diff(const char *xpath, const char *op, xmlNode *change)
{
    if (strstr(xpath, "/" PCMK__XE_LRM_RSC_OP "[")) {
        process_op_deletion(xpath, change);

    } else if (strstr(xpath, "/" PCMK__XE_LRM "[")) {
        abort_unless_down(xpath, op, change, "Resource state removal");

    } else if (strstr(xpath, "/" PCMK__XE_NODE_STATE "[")) {
        abort_unless_down(xpath, op, change, "Node state removal");

    } else {
        crm_trace("Ignoring delete of %s", xpath);
    }
}

static void
process_node_state_diff(xmlNode *state, xmlNode *change, const char *op,
                        const char *xpath)
{
    xmlNode *lrm = pcmk__xe_match_name(state, PCMK__XE_LRM);

    process_resource_updates(pcmk__xe_id(state), lrm, change, op, xpath);
}

static void
process_status_diff(xmlNode *status, xmlNode *change, const char *op,
                    const char *xpath)
{
    for (xmlNode *state = pcmk__xml_first_child(status); state != NULL;
         state = pcmk__xml_next(state)) {
        process_node_state_diff(state, change, op, xpath);
    }
}

static void
process_cib_diff(xmlNode *cib, xmlNode *change, const char *op,
                 const char *xpath)
{
    xmlNode *status = pcmk__xe_match_name(cib, PCMK_XE_STATUS);
    xmlNode *config = pcmk__xe_match_name(cib, PCMK_XE_CONFIGURATION);

    if (status) {
        process_status_diff(status, change, op, xpath);
    }
    if (config) {
        abort_transition(PCMK_SCORE_INFINITY, pcmk__graph_restart,
                         "Non-status-only change", change);
    }
}

static void
te_update_diff_v2(xmlNode *diff)
{
    crm_log_xml_trace(diff, "Patch:Raw");

    for (xmlNode *change = pcmk__xml_first_child(diff); change != NULL;
         change = pcmk__xml_next(change)) {

        xmlNode *match = NULL;
        const char *name = NULL;
        const char *xpath = crm_element_value(change, PCMK_XA_PATH);

        // Possible ops: create, modify, delete, move
        const char *op = crm_element_value(change, PCMK_XA_OPERATION);

        // Ignore uninteresting updates
        if (op == NULL) {
            continue;

        } else if (xpath == NULL) {
            crm_trace("Ignoring %s change for version field", op);
            continue;

        } else if ((strcmp(op, PCMK_VALUE_MOVE) == 0)
                   && (strstr(xpath,
                              "/" PCMK_XE_CIB "/" PCMK_XE_CONFIGURATION
                              "/" PCMK_XE_RESOURCES) == NULL)) {
            /* We still need to consider moves within the resources section,
             * since they affect placement order.
             */
            crm_trace("Ignoring move change at %s", xpath);
            continue;
        }

        // Find the result of create/modify ops
        if (strcmp(op, PCMK_VALUE_CREATE) == 0) {
            match = change->children;

        } else if (strcmp(op, PCMK_VALUE_MODIFY) == 0) {
            match = pcmk__xe_match_name(change, PCMK_XE_CHANGE_RESULT);
            if(match) {
                match = match->children;
            }

        } else if (!pcmk__str_any_of(op,
                                     PCMK_VALUE_DELETE, PCMK_VALUE_MOVE,
                                     NULL)) {
            crm_warn("Ignoring malformed CIB update (%s operation on %s is unrecognized)",
                     op, xpath);
            continue;
        }

        if (match) {
            if (match->type == XML_COMMENT_NODE) {
                crm_trace("Ignoring %s operation for comment at %s", op, xpath);
                continue;
            }
            name = (const char *)match->name;
        }

        crm_trace("Handling %s operation for %s%s%s",
                  op, (xpath? xpath : "CIB"),
                  (name? " matched by " : ""), (name? name : ""));

        if (strstr(xpath, "/" PCMK_XE_CIB "/" PCMK_XE_CONFIGURATION)) {
            abort_transition(PCMK_SCORE_INFINITY, pcmk__graph_restart,
                             "Configuration change", change);
            break; // Won't be packaged with operation results we may be waiting for

        } else if (strstr(xpath, "/" PCMK_XE_TICKETS)
                   || pcmk__str_eq(name, PCMK_XE_TICKETS, pcmk__str_none)) {
            abort_transition(PCMK_SCORE_INFINITY, pcmk__graph_restart,
                             "Ticket attribute change", change);
            break; // Won't be packaged with operation results we may be waiting for

        } else if (strstr(xpath, "/" PCMK__XE_TRANSIENT_ATTRIBUTES "[")
                   || pcmk__str_eq(name, PCMK__XE_TRANSIENT_ATTRIBUTES,
                                   pcmk__str_none)) {
            abort_unless_down(xpath, op, change, "Transient attribute change");
            break; // Won't be packaged with operation results we may be waiting for

        } else if (strcmp(op, PCMK_VALUE_DELETE) == 0) {
            process_delete_diff(xpath, op, change);

        } else if (name == NULL) {
            crm_warn("Ignoring malformed CIB update (%s at %s has no result)",
                     op, xpath);

        } else if (strcmp(name, PCMK_XE_CIB) == 0) {
            process_cib_diff(match, change, op, xpath);

        } else if (strcmp(name, PCMK_XE_STATUS) == 0) {
            process_status_diff(match, change, op, xpath);

        } else if (strcmp(name, PCMK__XE_NODE_STATE) == 0) {
            process_node_state_diff(match, change, op, xpath);

        } else if (strcmp(name, PCMK__XE_LRM) == 0) {
            process_resource_updates(pcmk__xe_id(match), match, change, op,
                                     xpath);

        } else if (strcmp(name, PCMK__XE_LRM_RESOURCES) == 0) {
            char *local_node = pcmk__xpath_node_id(xpath, PCMK__XE_LRM);

            process_resource_updates(local_node, match, change, op, xpath);
            free(local_node);

        } else if (strcmp(name, PCMK__XE_LRM_RESOURCE) == 0) {
            char *local_node = pcmk__xpath_node_id(xpath, PCMK__XE_LRM);

            process_lrm_resource_diff(match, local_node);
            free(local_node);

        } else if (strcmp(name, PCMK__XE_LRM_RSC_OP) == 0) {
            char *local_node = pcmk__xpath_node_id(xpath, PCMK__XE_LRM);

            process_graph_event(match, local_node);
            free(local_node);

        } else {
            crm_warn("Ignoring malformed CIB update (%s at %s has unrecognized result %s)",
                     op, xpath, name);
        }
    }
}

void
te_update_diff(const char *event, xmlNode * msg)
{
    xmlNode *diff = NULL;
    const char *op = NULL;
    int rc = -EINVAL;
    int format = 1;
    int p_add[] = { 0, 0, 0 };
    int p_del[] = { 0, 0, 0 };

    CRM_CHECK(msg != NULL, return);
    crm_element_value_int(msg, PCMK__XA_CIB_RC, &rc);

    if (controld_globals.transition_graph == NULL) {
        crm_trace("No graph");
        return;

    } else if (rc < pcmk_ok) {
        crm_trace("Filter rc=%d (%s)", rc, pcmk_strerror(rc));
        return;

    } else if (controld_globals.transition_graph->complete
               && (controld_globals.fsa_state != S_IDLE)
               && (controld_globals.fsa_state != S_TRANSITION_ENGINE)
               && (controld_globals.fsa_state != S_POLICY_ENGINE)) {
        crm_trace("Filter state=%s (complete)",
                  fsa_state2string(controld_globals.fsa_state));
        return;
    }

    op = crm_element_value(msg, PCMK__XA_CIB_OP);
    diff = pcmk__message_get_xml(msg, PCMK__XA_CIB_UPDATE_RESULT);

    xml_patch_versions(diff, p_add, p_del);
    crm_debug("Processing (%s) diff: %d.%d.%d -> %d.%d.%d (%s)", op,
              p_del[0], p_del[1], p_del[2], p_add[0], p_add[1], p_add[2],
              fsa_state2string(controld_globals.fsa_state));

    crm_element_value_int(diff, PCMK_XA_FORMAT, &format);
    switch (format) {
        case 1:
            te_update_diff_v1(event, diff);
            break;
        case 2:
            te_update_diff_v2(diff);
            break;
        default:
            crm_warn("Ignoring malformed CIB update (unknown patch format %d)",
                     format);
    }
    controld_remove_all_outside_events();
}

void
process_te_message(xmlNode * msg, xmlNode * xml_data)
{
    const char *value = NULL;
    xmlXPathObject *xpathObj = NULL;
    int nmatches = 0;

    CRM_CHECK(msg != NULL, return);

    // Transition requests must specify transition engine as subsystem
    value = crm_element_value(msg, PCMK__XA_CRM_SYS_TO);
    if (pcmk__str_empty(value)
        || !pcmk__str_eq(value, CRM_SYSTEM_TENGINE, pcmk__str_none)) {
        crm_info("Received invalid transition request: subsystem '%s' not '"
                 CRM_SYSTEM_TENGINE "'", pcmk__s(value, ""));
        return;
    }

    // Only the lrm_invoke command is supported as a transition request
    value = crm_element_value(msg, PCMK__XA_CRM_TASK);
    if (!pcmk__str_eq(value, CRM_OP_INVOKE_LRM, pcmk__str_none)) {
        crm_info("Received invalid transition request: command '%s' not '"
                 CRM_OP_INVOKE_LRM "'", pcmk__s(value, ""));
        return;
    }

    // Transition requests must be marked as coming from the executor
    value = crm_element_value(msg, PCMK__XA_CRM_SYS_FROM);
    if (!pcmk__str_eq(value, CRM_SYSTEM_LRMD, pcmk__str_none)) {
        crm_info("Received invalid transition request: from '%s' not '"
                 CRM_SYSTEM_LRMD "'", pcmk__s(value, ""));
        return;
    }

    crm_debug("Processing transition request with ref='%s' origin='%s'",
              pcmk__s(crm_element_value(msg, PCMK_XA_REFERENCE), ""),
              pcmk__s(crm_element_value(msg, PCMK__XA_SRC), ""));

    xpathObj = xpath_search(xml_data, "//" PCMK__XE_LRM_RSC_OP);
    nmatches = numXpathResults(xpathObj);
    if (nmatches == 0) {
        crm_err("Received transition request with no results (bug?)");
    } else {
        for (int lpc = 0; lpc < nmatches; lpc++) {
            xmlNode *rsc_op = getXpathResult(xpathObj, lpc);
            const char *node = get_node_id(rsc_op);

            process_graph_event(rsc_op, node);
        }
    }
    freeXpathObject(xpathObj);
}

void
cib_action_updated(xmlNode * msg, int call_id, int rc, xmlNode * output, void *user_data)
{
    if (rc < pcmk_ok) {
        crm_err("Update %d FAILED: %s", call_id, pcmk_strerror(rc));
    }
}

/*!
 * \brief Handle a timeout in node-to-node communication
 *
 * \param[in,out] data  Pointer to graph action
 *
 * \return FALSE (indicating that source should be not be re-added)
 */
gboolean
action_timer_callback(gpointer data)
{
    pcmk__graph_action_t *action = (pcmk__graph_action_t *) data;
    const char *task = NULL;
    const char *on_node = NULL;
    const char *via_node = NULL;

    CRM_CHECK(data != NULL, return FALSE);

    stop_te_timer(action);

    task = crm_element_value(action->xml, PCMK_XA_OPERATION);
    on_node = crm_element_value(action->xml, PCMK__META_ON_NODE);
    via_node = crm_element_value(action->xml, PCMK__XA_ROUTER_NODE);

    if (controld_globals.transition_graph->complete) {
        crm_notice("Node %s did not send %s result (via %s) within %dms "
                   "(ignoring because transition not in progress)",
                   (on_node? on_node : ""), (task? task : "unknown action"),
                   (via_node? via_node : "controller"), action->timeout);
    } else {
        /* fail the action */

        crm_err("Node %s did not send %s result (via %s) within %dms "
                "(action timeout plus " PCMK_OPT_CLUSTER_DELAY ")",
                (on_node? on_node : ""), (task? task : "unknown action"),
                (via_node? via_node : "controller"),
                (action->timeout
                 + controld_globals.transition_graph->network_delay));
        pcmk__log_graph_action(LOG_ERR, action);

        pcmk__set_graph_action_flags(action, pcmk__graph_action_failed);

        te_action_confirmed(action, controld_globals.transition_graph);
        abort_transition(PCMK_SCORE_INFINITY, pcmk__graph_restart,
                         "Action lost", NULL);

        // Record timeout in the CIB if appropriate
        if ((action->type == pcmk__rsc_graph_action)
            && controld_action_is_recordable(task)) {
            controld_record_action_timeout(action);
        }
    }

    return FALSE;
}
