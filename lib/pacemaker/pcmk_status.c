/*
 * Copyright 2004-2022 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU General Public License version 2
 * or later (GPLv2+) WITHOUT ANY WARRANTY.
 */

#include <crm_internal.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <crm/cib/internal.h>
#include <crm/common/output.h>
#include <crm/common/results.h>
#include <crm/fencing/internal.h>
#include <crm/stonith-ng.h>
#include <pacemaker.h>
#include <pacemaker-internal.h>

static int
cib_connect(pcmk__output_t *out, cib_t *cib, xmlNode **current_cib)
{
    int rc = pcmk_rc_ok;

    CRM_CHECK(cib != NULL, return EINVAL);

    if (cib->state == cib_connected_query ||
        cib->state == cib_connected_command) {
        return rc;
    }

    crm_trace("Connecting to the CIB");

    rc = cib->cmds->signon(cib, crm_system_name, cib_query);
    rc = pcmk_legacy2rc(rc);

    if (rc != pcmk_rc_ok) {
        out->err(out, "Could not connect to the CIB: %s",
                 pcmk_rc_str(rc));
        return rc;
    }

    rc = cib->cmds->query(cib, NULL, current_cib,
                          cib_scope_local | cib_sync_call);
    rc = pcmk_legacy2rc(rc);

    return rc;
}

static stonith_t *
fencing_connect(void)
{
    stonith_t *st = stonith_api_new();
    int rc = pcmk_rc_ok;

    if (st == NULL) {
        return NULL;
    }

    rc = st->cmds->connect(st, crm_system_name, NULL);
    if (rc == pcmk_rc_ok) {
        return st;
    } else {
        stonith_api_delete(st);
        return NULL;
    }
}

static void
pacemakerd_event_cb(pcmk_ipc_api_t *pacemakerd_api,
                    enum pcmk_ipc_event event_type, crm_exit_t status,
                    void *event_data, void *user_data)
{
    pcmk_pacemakerd_api_reply_t *reply = event_data;
    enum pcmk_pacemakerd_state *state =
        (enum pcmk_pacemakerd_state *) user_data;

    /* we are just interested in the latest reply */
    *state = pcmk_pacemakerd_state_invalid;

    if (event_type != pcmk_ipc_event_reply || status != CRM_EX_OK) {
        return;
    }

    if (reply->reply_type == pcmk_pacemakerd_reply_ping &&
        reply->data.ping.last_good != (time_t) 0 &&
        reply->data.ping.status == pcmk_rc_ok) {
        *state = reply->data.ping.state;
    }
}

static int
pacemakerd_status(pcmk__output_t *out)
{
    int rc = pcmk_rc_ok;
    pcmk_ipc_api_t *pacemakerd_api = NULL;
    enum pcmk_pacemakerd_state state = pcmk_pacemakerd_state_invalid;

    rc = pcmk_new_ipc_api(&pacemakerd_api, pcmk_ipc_pacemakerd);
    if (pacemakerd_api == NULL) {
        out->err(out, "Could not connect to pacemakerd: %s",
                 pcmk_rc_str(rc));
        return rc;
    }

    pcmk_register_ipc_callback(pacemakerd_api, pacemakerd_event_cb, (void *) &state);

    rc = pcmk_connect_ipc(pacemakerd_api, pcmk_ipc_dispatch_sync);
    if (rc == EREMOTEIO) {
        return pcmk_rc_ok;
    } else if (rc != pcmk_rc_ok) {
        out->err(out, "Could not connect to pacemakerd: %s",
                 pcmk_rc_str(rc));
        pcmk_free_ipc_api(pacemakerd_api);
        return rc;
    }

    rc = pcmk_pacemakerd_api_ping(pacemakerd_api, crm_system_name);

    if (rc != pcmk_rc_ok) {
        /* Got some error from pcmk_pacemakerd_api_ping, so return it. */
    } else if (state == pcmk_pacemakerd_state_running) {
        rc = pcmk_rc_ok;
    } else if (state == pcmk_pacemakerd_state_shutting_down) {
        rc = ENOTCONN;
    } else {
        rc = EAGAIN;
    }

    pcmk_free_ipc_api(pacemakerd_api);
    return rc;
}

int
pcmk__output_cluster_status(pcmk__output_t *out, stonith_t *st, cib_t *cib,
                            xmlNode *current_cib, enum pcmk__fence_history fence_history,
                            uint32_t show, uint32_t show_opts, char *only_node,
                            char *only_rsc, const char *neg_location_prefix, bool simple_output)
{
    xmlNode *cib_copy = copy_xml(current_cib);
    stonith_history_t *stonith_history = NULL;
    int history_rc = 0;
    pe_working_set_t *data_set = NULL;
    GList *unames = NULL;
    GList *resources = NULL;

    int rc = pcmk_rc_ok;

    if (cli_config_update(&cib_copy, NULL, FALSE) == FALSE) {
        cib__clean_up_connection(&cib);
        free_xml(cib_copy);
        rc = pcmk_rc_schema_validation;
        out->err(out, "Upgrade failed: %s", pcmk_rc_str(rc));
        return rc;
    }

    /* get the stonith-history if there is evidence we need it */
    if (fence_history != pcmk__fence_history_none) {
        history_rc = pcmk__get_fencing_history(st, &stonith_history, fence_history);
    }

    data_set = pe_new_working_set();
    CRM_ASSERT(data_set != NULL);
    pe__set_working_set_flags(data_set, pe_flag_no_compat);

    data_set->input = cib_copy;
    data_set->priv = out;
    cluster_status(data_set);

    /* Unpack constraints if any section will need them
     * (tickets may be referenced in constraints but not granted yet,
     * and bans need negative location constraints) */
    if (pcmk_is_set(show, pcmk_section_bans) || pcmk_is_set(show, pcmk_section_tickets)) {
        pcmk__unpack_constraints(data_set);
    }

    unames = pe__build_node_name_list(data_set, only_node);
    resources = pe__build_rsc_list(data_set, only_rsc);

    /* Always print DC if NULL. */
    if (data_set->dc_node == NULL) {
        show |= pcmk_section_dc;
    }

    if (simple_output) {
        rc = pcmk__output_simple_status(out, data_set);
    } else {
        out->message(out, "cluster-status", data_set, pcmk_rc2exitc(history_rc),
                     stonith_history, fence_history, show, show_opts,
                     neg_location_prefix, unames, resources);
    }

    g_list_free_full(unames, free);
    g_list_free_full(resources, free);

    stonith_history_free(stonith_history);
    stonith_history = NULL;
    pe_free_working_set(data_set);
    return rc;
}

int
pcmk_status(xmlNodePtr *xml)
{
    cib_t *cib = NULL;
    pcmk__output_t *out = NULL;
    int rc = pcmk_rc_ok;

    uint32_t show_opts = pcmk_show_pending | pcmk_show_inactive_rscs | pcmk_show_timing;

    cib = cib_new();

    if (cib == NULL) {
        return pcmk_rc_cib_corrupt;
    }

    rc = pcmk__xml_output_new(&out, xml);
    if (rc != pcmk_rc_ok) {
        cib_delete(cib);
        return rc;
    }

    pcmk__register_lib_messages(out);
    pe__register_messages(out);
    stonith__register_messages(out);

    rc = pcmk__status(out, cib, pcmk__fence_history_full, pcmk_section_all,
                      show_opts, NULL, NULL, NULL, false);
    pcmk__xml_output_finish(out, xml);

    cib_delete(cib);
    return rc;
}

int
pcmk__status(pcmk__output_t *out, cib_t *cib, enum pcmk__fence_history fence_history,
             uint32_t show, uint32_t show_opts, char *only_node, char *only_rsc,
             const char *neg_location_prefix, bool simple_output)
{
    xmlNode *current_cib = NULL;
    int rc = pcmk_rc_ok;
    stonith_t *st = NULL;

    if (cib == NULL) {
        return ENOTCONN;
    }

    if (cib->variant == cib_native) {
        if (cib->state == cib_connected_query || cib->state == cib_connected_command) {
            rc = pcmk_rc_ok;
        } else {
            rc = pacemakerd_status(out);
        }
    }

    if (rc != pcmk_rc_ok) {
        return rc;
    }

    if (fence_history != pcmk__fence_history_none && cib->variant == cib_native) {
        st = fencing_connect();
    }

    rc = cib_connect(out, cib, &current_cib);
    if (rc != pcmk_rc_ok) {
        goto done;
    }

    rc = pcmk__output_cluster_status(out, st, cib, current_cib, fence_history, show, show_opts,
                                     only_node, only_rsc, neg_location_prefix, simple_output);

done:
    if (st != NULL) {
        if (st->state != stonith_disconnected) {
            st->cmds->remove_notification(st, NULL);
            st->cmds->disconnect(st);
        }

        stonith_api_delete(st);
    }

    if (current_cib != NULL) {
        free_xml(current_cib);
    }

    return rc;
}

/* This is an internal-only function that is planned to be deprecated and removed.
 * It should only ever be called from crm_mon.
 */
int
pcmk__output_simple_status(pcmk__output_t *out, pe_working_set_t *data_set)
{
    int nodes_online = 0;
    int nodes_standby = 0;
    int nodes_maintenance = 0;
    GString *offline_nodes = NULL;
    bool no_dc = false;
    bool offline = false;
    bool has_warnings = false;

    if (data_set->dc_node == NULL) {
        has_warnings = true;
        no_dc = true;
    }

    for (GList *iter = data_set->nodes; iter != NULL; iter = iter->next) {
        pe_node_t *node = (pe_node_t *) iter->data;

        if (node->details->standby && node->details->online) {
            nodes_standby++;
        } else if (node->details->maintenance && node->details->online) {
            nodes_maintenance++;
        } else if (node->details->online) {
            nodes_online++;
        } else {
            pcmk__add_word(&offline_nodes, 1024, "offline node:");
            pcmk__add_word(&offline_nodes, 0, pe__node_name(node));
            has_warnings = true;
            offline = true;
        }
    }

    if (has_warnings) {
        out->info(out, "CLUSTER WARN: %s%s%s",
                  no_dc ? "No DC" : "",
                  no_dc && offline ? ", " : "",
                  (offline? (const char *) offline_nodes->str : ""));

        if (offline_nodes != NULL) {
            g_string_free(offline_nodes, TRUE);
        }

    } else {
        char *nodes_standby_s = NULL;
        char *nodes_maint_s = NULL;

        if (nodes_standby > 0) {
            nodes_standby_s = crm_strdup_printf(", %d standby node%s", nodes_standby,
                                                pcmk__plural_s(nodes_standby));
        }

        if (nodes_maintenance > 0) {
            nodes_maint_s = crm_strdup_printf(", %d maintenance node%s",
                                              nodes_maintenance,
                                              pcmk__plural_s(nodes_maintenance));
        }

        out->info(out, "CLUSTER OK: %d node%s online%s%s, "
                       "%d resource instance%s configured",
                  nodes_online, pcmk__plural_s(nodes_online),
                  nodes_standby_s != NULL ? nodes_standby_s : "",
                  nodes_maint_s != NULL ? nodes_maint_s : "",
                  data_set->ninstances, pcmk__plural_s(data_set->ninstances));

        free(nodes_standby_s);
        free(nodes_maint_s);
    }

    if (has_warnings) {
        return pcmk_rc_error;
    } else {
        return pcmk_rc_ok;
    }
    /* coverity[leaked_storage] False positive */
}
