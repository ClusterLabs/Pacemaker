/* 
 * Copyright (C) 2004 Andrew Beekhof <andrew@beekhof.net>
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <crm_internal.h>

#include <sys/param.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <crm/crm.h>
#include <crm/cib.h>
#include <crm/msg_xml.h>
#include <crm/common/xml.h>

#include <crmd.h>
#include <crmd_fsa.h>
#include <crmd_messages.h>
#include <crmd_callbacks.h>
#include <crmd_lrm.h>

#include <lrm/lrm_api.h>
#include <lrm/raexec.h>

#define START_DELAY_THRESHOLD 5 * 60 * 1000

typedef struct resource_history_s {
    char *id;
    lrm_rsc_t rsc;
    lrm_op_t *last;
    lrm_op_t *failed;
    GList *recurring_op_list;
} rsc_history_t;

struct recurring_op_s {
    char *rsc_id;
    char *op_key;
    int call_id;
    int interval;
    gboolean remove;
    gboolean cancelled;
};

struct pending_deletion_op_s {
    char *rsc;
    ha_msg_input_t *input;
};

struct delete_event_s {
    int rc;
    const char *rsc;
};

GHashTable *resource_history = NULL;
GHashTable *pending_ops = NULL;
GHashTable *deletion_ops = NULL;
GCHSource *lrm_source = NULL;

int num_lrm_register_fails = 0;
int max_lrm_register_fails = 30;

gboolean populate_history_cache(void);
gboolean process_lrm_event(lrm_op_t * op);
gboolean is_rsc_active(const char *rsc_id);
gboolean build_active_RAs(xmlNode * rsc_list);
static gboolean stop_recurring_actions(gpointer key, gpointer value, gpointer user_data);
static int delete_rsc_status(const char *rsc_id, int call_options, const char *user_name);

lrm_op_t *construct_op(xmlNode * rsc_op, const char *rsc_id, const char *operation);
void do_lrm_rsc_op(lrm_rsc_t * rsc, const char *operation, xmlNode * msg, xmlNode * request);

void send_direct_ack(const char *to_host, const char *to_sys,
                     lrm_rsc_t * rsc, lrm_op_t * op, const char *rsc_id);

void
lrm_connection_destroy(gpointer user_data)
{
    if (is_set(fsa_input_register, R_LRM_CONNECTED)) {
        crm_crit("LRM Connection failed");
        register_fsa_input(C_FSA_INTERNAL, I_ERROR, NULL);
        clear_bit_inplace(fsa_input_register, R_LRM_CONNECTED);

    } else {
        crm_info("LRM Connection disconnected");
    }

    lrm_source = NULL;
}

static void
free_deletion_op(gpointer value)
{
    struct pending_deletion_op_s *op = value;

    crm_free(op->rsc);
    delete_ha_msg_input(op->input);
    crm_free(op);
}

static void
free_recurring_op(gpointer value)
{
    struct recurring_op_s *op = (struct recurring_op_s *)value;

    crm_free(op->rsc_id);
    crm_free(op->op_key);
    crm_free(op);
}

static char *
make_stop_id(const char *rsc, int call_id)
{
    char *op_id = NULL;

    crm_malloc0(op_id, strlen(rsc) + 34);
    if (op_id != NULL) {
        snprintf(op_id, strlen(rsc) + 34, "%s:%d", rsc, call_id);
    }
    return op_id;
}

static void
dup_attr(gpointer key, gpointer value, gpointer user_data)
{
    g_hash_table_replace(user_data, crm_strdup(key), crm_strdup(value));
}

static void
history_cache_destroy(gpointer data)
{
    rsc_history_t *entry = data;

    crm_free(entry->rsc.type);
    crm_free(entry->rsc.class);
    crm_free(entry->rsc.provider);

    free_lrm_op(entry->failed);
    free_lrm_op(entry->last);
    crm_free(entry->id);
    crm_free(entry);
}

static void
update_history_cache(lrm_rsc_t * rsc, lrm_op_t * op)
{
    int target_rc = 0;
    rsc_history_t *entry = NULL;

#ifdef HAVE_LRM_OP_T_RSC_DELETED
    if (op->rsc_deleted) {
        crm_debug("Purged history for '%s' after %s", op->rsc_id, op->op_type);
        delete_rsc_status(op->rsc_id, cib_quorum_override, NULL);
        return;
    }
#endif

    if (safe_str_eq(op->op_type, RSC_NOTIFY)) {
        return;
    }

    crm_debug("Appending %s op to history for '%s'", op->op_type, op->rsc_id);

    entry = g_hash_table_lookup(resource_history, op->rsc_id);
    if (entry == NULL && rsc) {
        crm_malloc0(entry, sizeof(rsc_history_t));
        entry->id = crm_strdup(op->rsc_id);
        g_hash_table_insert(resource_history, entry->id, entry);

        entry->rsc.id = entry->id;
        entry->rsc.type = crm_strdup(rsc->type);
        entry->rsc.class = crm_strdup(rsc->class);
        if (rsc->provider) {
            entry->rsc.provider = crm_strdup(rsc->provider);
        } else {
            entry->rsc.provider = NULL;
        }

    } else if (entry == NULL) {
        crm_info("Resource %s no longer exists, not updating cache", op->rsc_id);
        return;
    }

    target_rc = rsc_op_expected_rc(op);
    if (op->op_status == LRM_OP_CANCELLED) {
        crm_trace("Skipping %s_%s_%d rc=%d, status=%d", op->rsc_id, op->op_type, op->interval,
                  op->rc, op->op_status);

    } else if (did_rsc_op_fail(op, target_rc)) {
        /* We must store failed monitors here
         * - otherwise the block below will cause them to be forgetten them when a stop happens
         */
        if (entry->failed) {
            free_lrm_op(entry->failed);
        }
        entry->failed = copy_lrm_op(op);

    } else if (op->interval == 0) {
        if (entry->last) {
            free_lrm_op(entry->last);
        }
        entry->last = copy_lrm_op(op);
    }

    if (op->interval > 0) {
        crm_trace("Adding recurring op: %s_%s_%d", op->rsc_id, op->op_type, op->interval);
        entry->recurring_op_list = g_list_prepend(entry->recurring_op_list, copy_lrm_op(op));

    } else if (entry->recurring_op_list && safe_str_eq(op->op_type, RSC_STATUS) == FALSE) {
        GList *gIter = entry->recurring_op_list;

        crm_trace("Dropping %d recurring ops because of: %s_%s_%d",
                  g_list_length(gIter), op->rsc_id, op->op_type, op->interval);
        for (; gIter != NULL; gIter = gIter->next) {
            free_lrm_op(gIter->data);
        }
        g_list_free(entry->recurring_op_list);
        entry->recurring_op_list = NULL;
    }
}

/*	 A_LRM_CONNECT	*/
void
do_lrm_control(long long action,
               enum crmd_fsa_cause cause,
               enum crmd_fsa_state cur_state,
               enum crmd_fsa_input current_input, fsa_data_t * msg_data)
{
    if (fsa_lrm_conn == NULL) {
        register_fsa_error(C_FSA_INTERNAL, I_ERROR, NULL);
        return;
    }

    if (action & A_LRM_DISCONNECT) {
        if (verify_stopped(cur_state, LOG_INFO) == FALSE) {
            crmd_fsa_stall(NULL);
            return;
        }

        if (is_set(fsa_input_register, R_LRM_CONNECTED)) {
            clear_bit_inplace(fsa_input_register, R_LRM_CONNECTED);
            fsa_lrm_conn->lrm_ops->signoff(fsa_lrm_conn);
            crm_info("Disconnected from the LRM");
        }

        /* TODO: Clean up the hashtable */
    }

    if (action & A_LRM_CONNECT) {
        int ret = HA_OK;

        deletion_ops = g_hash_table_new_full(crm_str_hash, g_str_equal,
                                             g_hash_destroy_str, free_deletion_op);

        pending_ops = g_hash_table_new_full(crm_str_hash, g_str_equal,
                                            g_hash_destroy_str, free_recurring_op);

        resource_history = g_hash_table_new_full(crm_str_hash, g_str_equal,
                                                 NULL, history_cache_destroy);

        if (ret == HA_OK) {
            crm_debug("Connecting to the LRM");
            ret = fsa_lrm_conn->lrm_ops->signon(fsa_lrm_conn, CRM_SYSTEM_CRMD);
        }

        if (ret != HA_OK) {
            if (++num_lrm_register_fails < max_lrm_register_fails) {
                crm_warn("Failed to sign on to the LRM %d"
                         " (%d max) times", num_lrm_register_fails, max_lrm_register_fails);

                crm_timer_start(wait_timer);
                crmd_fsa_stall(NULL);
                return;
            }
        }

        if (ret == HA_OK) {
            crm_debug_4("LRM: set_lrm_callback...");
            ret = fsa_lrm_conn->lrm_ops->set_lrm_callback(fsa_lrm_conn, lrm_op_callback);
            if (ret != HA_OK) {
                crm_err("Failed to set LRM callbacks");
            }
        }

        if (ret != HA_OK) {
            crm_err("Failed to sign on to the LRM %d" " (max) times", num_lrm_register_fails);
            register_fsa_error(C_FSA_INTERNAL, I_ERROR, NULL);
            return;
        }

        populate_history_cache();

        /* TODO: create a destroy handler that causes
         * some recovery to happen
         */
        lrm_source = G_main_add_IPC_Channel(G_PRIORITY_LOW,
                                            fsa_lrm_conn->lrm_ops->ipcchan(fsa_lrm_conn),
                                            FALSE, lrm_dispatch, fsa_lrm_conn,
                                            lrm_connection_destroy);

        set_bit_inplace(fsa_input_register, R_LRM_CONNECTED);
        crm_debug("LRM connection established");
    }

    if (action & ~(A_LRM_CONNECT | A_LRM_DISCONNECT)) {
        crm_err("Unexpected action %s in %s", fsa_action2string(action), __FUNCTION__);
    }
}

static void
ghash_print_pending(gpointer key, gpointer value, gpointer user_data)
{
    const char *stop_id = key;
    int *log_level = user_data;
    struct recurring_op_s *pending = value;

    do_crm_log(*log_level, "Pending action: %s (%s)", stop_id, pending->op_key);
}

static void
ghash_print_pending_for_rsc(gpointer key, gpointer value, gpointer user_data)
{
    const char *stop_id = key;
    char *rsc = user_data;
    struct recurring_op_s *pending = value;

    if (safe_str_eq(rsc, pending->rsc_id)) {
        do_crm_log(LOG_NOTICE, "%sction %s (%s) incomplete at shutdown",
                   pending->interval == 0 ? "A" : "Recurring a", stop_id, pending->op_key);
    }
}

static void
ghash_count_pending(gpointer key, gpointer value, gpointer user_data)
{
    int *counter = user_data;
    struct recurring_op_s *pending = value;

    if (pending->interval > 0) {
        /* Ignore recurring actions in the shutdown calculations */
        return;
    }

    (*counter)++;
}

gboolean
verify_stopped(enum crmd_fsa_state cur_state, int log_level)
{
    int counter = 0;
    gboolean rc = TRUE;

    GHashTableIter gIter;
    rsc_history_t *entry = NULL;

    crm_debug("Checking for active resources before exit");

    if (cur_state == S_TERMINATE) {
        log_level = LOG_ERR;
    }

    if (pending_ops) {
        if (is_set(fsa_input_register, R_LRM_CONNECTED)) {
            /* Only log/complain about non-recurring actions */
            g_hash_table_foreach_remove(pending_ops, stop_recurring_actions, NULL);
        }
        g_hash_table_foreach(pending_ops, ghash_count_pending, &counter);
    }

    if (counter > 0) {
        rc = FALSE;
        do_crm_log(log_level,
                   "%d pending LRM operations at shutdown%s",
                   counter, cur_state == S_TERMINATE ? "" : "... waiting");

        if (cur_state == S_TERMINATE || !is_set(fsa_input_register, R_SENT_RSC_STOP)) {
            g_hash_table_foreach(pending_ops, ghash_print_pending, &log_level);
        }
        goto bail;
    }

    if (resource_history == NULL) {
        goto bail;
    }

    g_hash_table_iter_init(&gIter, resource_history);
    while (g_hash_table_iter_next(&gIter, NULL, (void **)&entry)) {
        if (is_rsc_active(entry->id) == FALSE) {
            continue;
        }

        crm_err("Resource %s was active at shutdown."
                "  You may ignore this error if it is unmanaged.", entry->id);

        g_hash_table_foreach(pending_ops, ghash_print_pending_for_rsc, entry->id);
    }

  bail:
    set_bit_inplace(fsa_input_register, R_SENT_RSC_STOP);

    if (cur_state == S_TERMINATE) {
        rc = TRUE;
    }

    return rc;
}

static char *
get_rsc_metadata(const char *type, const char *class, const char *provider)
{
    char *metadata = NULL;

    CRM_CHECK(type != NULL, return NULL);
    CRM_CHECK(class != NULL, return NULL);
    if (provider == NULL) {
        provider = "heartbeat";
    }

    crm_debug_2("Retreiving metadata for %s::%s:%s", type, class, provider);
    metadata = fsa_lrm_conn->lrm_ops->get_rsc_type_metadata(fsa_lrm_conn, class, type, provider);

    if (metadata) {
        /* copy the metadata because the LRM likes using
         *   g_alloc instead of cl_malloc
         */
        char *m_copy = crm_strdup(metadata);

        g_free(metadata);
        metadata = m_copy;

    } else {
        crm_warn("No metadata found for %s::%s:%s", type, class, provider);
    }

    return metadata;
}

typedef struct reload_data_s {
    char *key;
    char *metadata;
    time_t last_query;
    gboolean can_reload;
    GListPtr restart_list;
} reload_data_t;

static void
g_hash_destroy_reload(gpointer data)
{
    reload_data_t *reload = data;

    crm_free(reload->key);
    crm_free(reload->metadata);
    slist_basic_destroy(reload->restart_list);
    crm_free(reload);
}

GHashTable *reload_hash = NULL;
static GListPtr
get_rsc_restart_list(lrm_rsc_t * rsc, lrm_op_t * op)
{
    int len = 0;
    char *key = NULL;
    char *copy = NULL;
    const char *value = NULL;
    const char *provider = NULL;

    xmlNode *param = NULL;
    xmlNode *params = NULL;
    xmlNode *actions = NULL;
    xmlNode *metadata = NULL;

    time_t now = time(NULL);
    reload_data_t *reload = NULL;

    if (reload_hash == NULL) {
        reload_hash = g_hash_table_new_full(crm_str_hash, g_str_equal, NULL, g_hash_destroy_reload);
    }

    provider = rsc->provider;
    if (provider == NULL) {
        provider = "heartbeat";
    }

    len = strlen(rsc->type) + strlen(rsc->class) + strlen(provider) + 4;
    crm_malloc(key, len);
    snprintf(key, len, "%s::%s:%s", rsc->type, rsc->class, provider);

    reload = g_hash_table_lookup(reload_hash, key);

    if (reload && ((now - 9) > reload->last_query)
        && safe_str_eq(op->op_type, RSC_START)) {
        reload = NULL;          /* re-query */
    }

    if (reload == NULL) {
        xmlNode *action = NULL;

        crm_malloc0(reload, sizeof(reload_data_t));
        g_hash_table_replace(reload_hash, key, reload);

        reload->last_query = now;
        reload->key = key;
        key = NULL;
        reload->metadata = get_rsc_metadata(rsc->type, rsc->class, provider);

        metadata = string2xml(reload->metadata);
        if (metadata == NULL) {
            crm_err("Metadata for %s::%s:%s is not valid XML",
                    rsc->provider, rsc->class, rsc->type);
            goto cleanup;
        }

        actions = find_xml_node(metadata, "actions", TRUE);

        for (action = __xml_first_child(actions); action != NULL; action = __xml_next(action)) {
            if (crm_str_eq((const char *)action->name, "action", TRUE)) {
                value = crm_element_value(action, "name");
                if (safe_str_eq("reload", value)) {
                    reload->can_reload = TRUE;
                    break;
                }
            }
        }

        if (reload->can_reload == FALSE) {
            goto cleanup;
        }

        params = find_xml_node(metadata, "parameters", TRUE);
        for (param = __xml_first_child(params); param != NULL; param = __xml_next(param)) {
            if (crm_str_eq((const char *)param->name, "parameter", TRUE)) {
                value = crm_element_value(param, "unique");
                if (crm_is_true(value)) {
                    value = crm_element_value(param, "name");
                    if (value == NULL) {
                        crm_err("%s: NULL param", key);
                        continue;
                    }
                    crm_debug("Attr %s is not reloadable", value);
                    copy = crm_strdup(value);
                    CRM_CHECK(copy != NULL, continue);
                    reload->restart_list = g_list_append(reload->restart_list, copy);
                }
            }
        }
    }

  cleanup:
    crm_free(key);
    free_xml(metadata);
    return reload ? reload->restart_list : NULL;
}

static void
append_restart_list(lrm_rsc_t * rsc, lrm_op_t * op, xmlNode * update, const char *version)
{
    int len = 0;
    char *list = NULL;
    char *digest = NULL;
    const char *value = NULL;
    gboolean non_empty = FALSE;
    xmlNode *restart = NULL;
    GListPtr restart_list = NULL;
    GListPtr lpc = NULL;

    if (op->interval > 0) {
        /* monitors are not reloadable */
        return;

    } else if (op->params == NULL) {
        crm_debug("%s has no parameters", ID(update));
        return;

    } else if (rsc == NULL) {
        return;

    } else if (crm_str_eq(CRMD_ACTION_START, op->op_type, TRUE) == FALSE) {
        /* only starts are potentially reloadable */
        return;

    } else if (compare_version("1.0.8", version) > 0) {
        /* Caller version does not support reloads */
        return;
    }

    restart_list = get_rsc_restart_list(rsc, op);
    if (restart_list == NULL) {
        /* Resource does not support reloads */
        return;
    }

    restart = create_xml_node(NULL, XML_TAG_PARAMS);
    for (lpc = restart_list; lpc != NULL; lpc = lpc->next) {
        const char *param = (const char *)lpc->data;

        int start = len;

        CRM_CHECK(param != NULL, continue);
        value = g_hash_table_lookup(op->params, param);
        if (value != NULL) {
            non_empty = TRUE;
            crm_xml_add(restart, param, value);
        }
        len += strlen(param) + 2;
        crm_realloc(list, len + 1);
        sprintf(list + start, " %s ", param);
    }

    digest = calculate_operation_digest(restart, version);
    crm_xml_add(update, XML_LRM_ATTR_OP_RESTART, list);
    crm_xml_add(update, XML_LRM_ATTR_RESTART_DIGEST, digest);

#if 0
    crm_debug("%s: %s, %s", rsc->id, digest, list);
    if (non_empty) {
        crm_log_xml_debug(restart, "restart digest source");
    }
#endif

    free_xml(restart);
    crm_free(digest);
    crm_free(list);
}

static gboolean
build_operation_update(xmlNode * parent, lrm_rsc_t * rsc, lrm_op_t * op, const char *src)
{
    int target_rc = 0;
    xmlNode *xml_op = NULL;
    const char *caller_version = CRM_FEATURE_SET;

    if (op == NULL) {
        return FALSE;

    } else if (AM_I_DC) {

    } else if (fsa_our_dc_version != NULL) {
        caller_version = fsa_our_dc_version;
    } else if (op->params == NULL) {
        caller_version = fsa_our_dc_version;
    } else {
        /* there is a small risk in formerly mixed clusters that
         *   it will be sub-optimal.
         * however with our upgrade policy, the update we send
         *   should still be completely supported anyway
         */
        caller_version = g_hash_table_lookup(op->params, XML_ATTR_CRM_VERSION);
        crm_warn("Falling back to operation originator version: %s", caller_version);
    }

    target_rc = rsc_op_expected_rc(op);
    xml_op = create_operation_update(parent, op, caller_version, target_rc, src, LOG_DEBUG);

    if (xml_op) {
        append_restart_list(rsc, op, xml_op, caller_version);
    }
    return TRUE;
}

gboolean
is_rsc_active(const char *rsc_id)
{
    rsc_history_t *entry = NULL;

    crm_debug_3("Processing lrm_rsc_t entry %s", rsc_id);

    entry = g_hash_table_lookup(resource_history, rsc_id);
    if (entry == NULL || entry->last == NULL) {
        return FALSE;
    }

    if (entry->last->rc == EXECRA_OK && safe_str_eq(entry->last->op_type, CRMD_ACTION_STOP)) {
        return FALSE;

    } else if (entry->last->rc == EXECRA_OK
               && safe_str_eq(entry->last->op_type, CRMD_ACTION_MIGRATE)) {
        /* a stricter check is too complex...
         * leave that to the PE
         */
        return FALSE;

    } else if (entry->last->rc == EXECRA_NOT_RUNNING) {
        return FALSE;
    }

    return TRUE;
}

gboolean
build_active_RAs(xmlNode * rsc_list)
{
    GHashTableIter iter;
    rsc_history_t *entry = NULL;

    g_hash_table_iter_init(&iter, resource_history);
    while (g_hash_table_iter_next(&iter, NULL, (void **)&entry)) {

        GList *gIter = NULL;
        xmlNode *xml_rsc = create_xml_node(rsc_list, XML_LRM_TAG_RESOURCE);

        crm_xml_add(xml_rsc, XML_ATTR_ID, entry->id);
        crm_xml_add(xml_rsc, XML_ATTR_TYPE, entry->rsc.type);
        crm_xml_add(xml_rsc, XML_AGENT_ATTR_CLASS, entry->rsc.class);
        crm_xml_add(xml_rsc, XML_AGENT_ATTR_PROVIDER, entry->rsc.provider);

        build_operation_update(xml_rsc, &(entry->rsc), entry->last, __FUNCTION__);
        build_operation_update(xml_rsc, &(entry->rsc), entry->failed, __FUNCTION__);
        for (gIter = entry->recurring_op_list; gIter != NULL; gIter = gIter->next) {
            build_operation_update(xml_rsc, &(entry->rsc), gIter->data, __FUNCTION__);
        }
    }

    return FALSE;
}

gboolean
populate_history_cache(void)
{
    GListPtr gIter = NULL;
    GListPtr gIter2 = NULL;
    GList *op_list = NULL;
    GList *rsc_list = NULL;
    state_flag_t cur_state = 0;

    rsc_list = fsa_lrm_conn->lrm_ops->get_all_rscs(fsa_lrm_conn);
    for (gIter = rsc_list; gIter != NULL; gIter = gIter->next) {
        char *rid = (char *)gIter->data;

        int max_call_id = -1;
        lrm_rsc_t *rsc = fsa_lrm_conn->lrm_ops->get_rsc(fsa_lrm_conn, rid);

        if (rsc == NULL) {
            crm_err("NULL resource returned from the LRM: %s", rid);
            continue;
        }

        op_list = rsc->ops->get_cur_state(rsc, &cur_state);
        for (gIter2 = op_list; gIter2 != NULL; gIter2 = gIter2->next) {
            lrm_op_t *op = (lrm_op_t *) gIter2->data;

            if (max_call_id < op->call_id) {
                update_history_cache(rsc, op);

            } else if (max_call_id > op->call_id) {
                crm_err("Bad call_id in list=%d. Previous call_id=%d", op->call_id, max_call_id);

            } else {
                crm_warn("lrm->get_cur_state() returned"
                         " duplicate entries for call_id=%d", op->call_id);
            }

            max_call_id = op->call_id;
            lrm_free_op(op);
        }

        g_list_free(op_list);
        lrm_free_rsc(rsc);
        free(rid);
    }

    g_list_free(rsc_list);
    return TRUE;
}

xmlNode *
do_lrm_query(gboolean is_replace)
{
    gboolean shut_down = FALSE;
    xmlNode *xml_result = NULL;
    xmlNode *xml_state = NULL;
    xmlNode *xml_data = NULL;
    xmlNode *rsc_list = NULL;
    const char *exp_state = CRMD_STATE_ACTIVE;

    if (is_set(fsa_input_register, R_SHUTDOWN)) {
        exp_state = CRMD_STATE_INACTIVE;
        shut_down = TRUE;
    }

    xml_state = create_node_state(fsa_our_uname, ACTIVESTATUS, XML_BOOLEAN_TRUE,
                                  ONLINESTATUS, CRMD_JOINSTATE_MEMBER, exp_state,
                                  !shut_down, __FUNCTION__);

    xml_data = create_xml_node(xml_state, XML_CIB_TAG_LRM);
    crm_xml_add(xml_data, XML_ATTR_ID, fsa_our_uuid);
    rsc_list = create_xml_node(xml_data, XML_LRM_TAG_RESOURCES);

    /* Build a list of active (not always running) resources */
    build_active_RAs(rsc_list);

    xml_result = create_cib_fragment(xml_state, XML_CIB_TAG_STATUS);
    crm_log_xml_debug_3(xml_state, "Current state of the LRM");
    free_xml(xml_state);

    return xml_result;
}

static void
notify_deleted(ha_msg_input_t * input, const char *rsc_id, int rc)
{
    lrm_op_t *op = NULL;
    const char *from_sys = crm_element_value(input->msg, F_CRM_SYS_FROM);
    const char *from_host = crm_element_value(input->msg, F_CRM_HOST_FROM);

    crm_info("Notifying %s on %s that %s was%s deleted",
             from_sys, from_host, rsc_id, rc == HA_OK ? "" : " not");

    op = construct_op(input->xml, rsc_id, CRMD_ACTION_DELETE);
    CRM_ASSERT(op != NULL);

    if (rc == HA_OK) {
        op->op_status = LRM_OP_DONE;
        op->rc = EXECRA_OK;
    } else {
        op->op_status = LRM_OP_ERROR;
        op->rc = EXECRA_UNKNOWN_ERROR;
    }

    send_direct_ack(from_host, from_sys, NULL, op, rsc_id);
    free_lrm_op(op);

    if (safe_str_neq(from_sys, CRM_SYSTEM_TENGINE)) {
        /* this isn't expected - trigger a new transition */
        time_t now = time(NULL);
        char *now_s = crm_itoa(now);

        crm_debug("Triggering a refresh after %s deleted %s from the LRM", from_sys, rsc_id);

        update_attr(fsa_cib_conn, cib_none, XML_CIB_TAG_CRMCONFIG,
                    NULL, NULL, NULL, NULL, "last-lrm-refresh", now_s, FALSE);
        crm_free(now_s);
    }
}

static gboolean
lrm_remove_deleted_rsc(gpointer key, gpointer value, gpointer user_data)
{
    struct delete_event_s *event = user_data;
    struct pending_deletion_op_s *op = value;

    if (safe_str_eq(event->rsc, op->rsc)) {
        notify_deleted(op->input, event->rsc, event->rc);
        return TRUE;
    }
    return FALSE;
}

static gboolean
lrm_remove_deleted_op(gpointer key, gpointer value, gpointer user_data)
{
    const char *rsc = user_data;
    struct recurring_op_s *pending = value;

    if (safe_str_eq(rsc, pending->rsc_id)) {
        crm_info("Removing op %s:%d for deleted resource %s",
                 pending->op_key, pending->call_id, rsc);
        return TRUE;
    }
    return FALSE;
}

/*
 * Remove the rsc from the CIB
 *
 * Avoids refreshing the entire LRM section of this host
 */
#define rsc_template "//"XML_CIB_TAG_STATE"[@uname='%s']//"XML_LRM_TAG_RESOURCE"[@id='%s']"

static int
delete_rsc_status(const char *rsc_id, int call_options, const char *user_name)
{
    char *rsc_xpath = NULL;
    int max = 0;
    int rc = cib_ok;

    CRM_CHECK(rsc_id != NULL, return cib_id_check);

    max = strlen(rsc_template) + strlen(rsc_id) + strlen(fsa_our_uname) + 1;
    crm_malloc0(rsc_xpath, max);
    snprintf(rsc_xpath, max, rsc_template, fsa_our_uname, rsc_id);

    rc = fsa_cib_conn->cmds->delegated_variant_op(fsa_cib_conn, CIB_OP_DELETE, NULL, rsc_xpath,
                                                  NULL, NULL, call_options | cib_xpath, user_name);

    crm_free(rsc_xpath);
    return rc;
}

static void
delete_rsc_entry(ha_msg_input_t * input, const char *rsc_id, int rc, const char *user_name)
{
    struct delete_event_s event;

    CRM_CHECK(rsc_id != NULL, return);

    if (rc == HA_OK) {
        char *rsc_id_copy = crm_strdup(rsc_id);

        g_hash_table_remove(resource_history, rsc_id);
        crm_debug("sync: Sending delete op for %s", rsc_id);
        delete_rsc_status(rsc_id, cib_quorum_override, user_name);

        g_hash_table_foreach_remove(pending_ops, lrm_remove_deleted_op, rsc_id_copy);

        crm_free(rsc_id_copy);
    }

    if (input) {
        notify_deleted(input, rsc_id, rc);
    }

    event.rc = rc;
    event.rsc = rsc_id;
    g_hash_table_foreach_remove(deletion_ops, lrm_remove_deleted_rsc, &event);
}

/*
 * Remove the op from the CIB
 *
 * Avoids refreshing the entire LRM section of this host
 */

#define op_template "//"XML_CIB_TAG_STATE"[@uname='%s']//"XML_LRM_TAG_RESOURCE"[@id='%s']/"XML_LRM_TAG_RSC_OP"[@id='%s']"
#define op_call_template "//"XML_CIB_TAG_STATE"[@uname='%s']//"XML_LRM_TAG_RESOURCE"[@id='%s']/"XML_LRM_TAG_RSC_OP"[@id='%s' and @"XML_LRM_ATTR_CALLID"='%d']"

static void
delete_op_entry(lrm_op_t * op, const char *rsc_id, const char *key, int call_id)
{
    xmlNode *xml_top = NULL;

    if (op != NULL) {
        xml_top = create_xml_node(NULL, XML_LRM_TAG_RSC_OP);
        crm_xml_add_int(xml_top, XML_LRM_ATTR_CALLID, op->call_id);
        crm_xml_add(xml_top, XML_ATTR_TRANSITION_KEY, op->user_data);

        crm_debug("async: Sending delete op for %s_%s_%d (call=%d)",
                  op->rsc_id, op->op_type, op->interval, op->call_id);

        fsa_cib_conn->cmds->delete(fsa_cib_conn, XML_CIB_TAG_STATUS, xml_top, cib_quorum_override);

    } else if (rsc_id != NULL && key != NULL) {
        int max = 0;
        char *op_xpath = NULL;

        if (call_id > 0) {
            max =
                strlen(op_call_template) + strlen(rsc_id) + strlen(fsa_our_uname) + strlen(key) +
                10;
            crm_malloc0(op_xpath, max);
            snprintf(op_xpath, max, op_call_template, fsa_our_uname, rsc_id, key, call_id);

        } else {
            max = strlen(op_template) + strlen(rsc_id) + strlen(fsa_our_uname) + strlen(key) + 1;
            crm_malloc0(op_xpath, max);
            snprintf(op_xpath, max, op_template, fsa_our_uname, rsc_id, key);
        }

        crm_debug("sync: Sending delete op for %s (call=%d)", rsc_id, call_id);
        fsa_cib_conn->cmds->delete(fsa_cib_conn, op_xpath, NULL, cib_quorum_override | cib_xpath);

        crm_free(op_xpath);

    } else {
        crm_err("Not enough information to delete op entry: rsc=%p key=%p", rsc_id, key);
        return;
    }

    crm_log_xml_debug_2(xml_top, "op:cancel");
    free_xml(xml_top);
}

static gboolean
cancel_op(lrm_rsc_t * rsc, const char *key, int op, gboolean remove)
{
    int rc = HA_OK;
    struct recurring_op_s *pending = NULL;

    CRM_CHECK(op != 0, return FALSE);
    CRM_CHECK(rsc != NULL, return FALSE);
    if (key == NULL) {
        key = make_stop_id(rsc->id, op);
    }
    pending = g_hash_table_lookup(pending_ops, key);

    if (pending) {
        if (remove && pending->remove == FALSE) {
            pending->remove = TRUE;
            crm_debug("Scheduling %s for removal", key);
        }

        if (pending->cancelled) {
            crm_debug("Operation %s already cancelled", key);
            return TRUE;
        }

        pending->cancelled = TRUE;

    } else {
        crm_info("No pending op found for %s", key);
    }

    crm_debug("Cancelling op %d for %s (%s)", op, rsc->id, key);

    rc = rsc->ops->cancel_op(rsc, op);
    if (rc == HA_OK) {
        crm_debug("Op %d for %s (%s): cancelled", op, rsc->id, key);
#ifdef HAVE_LRM_OP_T_RSC_DELETED
    } else if (rc == HA_RSCBUSY) {
        crm_debug("Op %d for %s (%s): cancelation pending", op, rsc->id, key);
#endif
    } else {
        crm_debug("Op %d for %s (%s): Nothing to cancel", op, rsc->id, key);
        /* The caller needs to make sure the entry is
         * removed from the pending_ops list
         *
         * Usually by returning TRUE inside the worker function
         * supplied to g_hash_table_foreach_remove()
         *
         * Not removing the entry from pending_ops will block
         * the node from shutting down
         */
        return FALSE;
    }

    return TRUE;
}

struct cancel_data {
    gboolean done;
    gboolean remove;
    const char *key;
    lrm_rsc_t *rsc;
};

static gboolean
cancel_action_by_key(gpointer key, gpointer value, gpointer user_data)
{
    struct cancel_data *data = user_data;
    struct recurring_op_s *op = (struct recurring_op_s *)value;

    if (safe_str_eq(op->op_key, data->key)) {
        data->done = TRUE;
        if (cancel_op(data->rsc, key, op->call_id, data->remove) == FALSE) {
            return TRUE;
        }
    }
    return FALSE;
}

static gboolean
cancel_op_key(lrm_rsc_t * rsc, const char *key, gboolean remove)
{
    struct cancel_data data;

    CRM_CHECK(rsc != NULL, return FALSE);
    CRM_CHECK(key != NULL, return FALSE);

    data.key = key;
    data.rsc = rsc;
    data.done = FALSE;
    data.remove = remove;

    g_hash_table_foreach_remove(pending_ops, cancel_action_by_key, &data);
    return data.done;
}

static lrm_rsc_t *
get_lrm_resource(xmlNode * resource, xmlNode * op_msg, gboolean do_create)
{
    char rid[RID_LEN];
    lrm_rsc_t *rsc = NULL;
    const char *short_id = ID(resource);
    const char *long_id = crm_element_value(resource, XML_ATTR_ID_LONG);

    crm_debug_2("Retrieving %s from the LRM.", short_id);
    CRM_CHECK(short_id != NULL, return NULL);

    if (rsc == NULL) {
        /* check if its already there (short name) */
        strncpy(rid, short_id, RID_LEN);
        rid[RID_LEN - 1] = 0;
        rsc = fsa_lrm_conn->lrm_ops->get_rsc(fsa_lrm_conn, rid);
    }
    if (rsc == NULL && long_id != NULL) {
        /* try the long name instead */
        strncpy(rid, long_id, RID_LEN);
        rid[RID_LEN - 1] = 0;
        rsc = fsa_lrm_conn->lrm_ops->get_rsc(fsa_lrm_conn, rid);
    }

    if (rsc == NULL && do_create) {
        /* add it to the LRM */
        const char *type = crm_element_value(resource, XML_ATTR_TYPE);
        const char *class = crm_element_value(resource, XML_AGENT_ATTR_CLASS);
        const char *provider = crm_element_value(resource, XML_AGENT_ATTR_PROVIDER);
        GHashTable *params = xml2list(op_msg);

        CRM_CHECK(class != NULL, return NULL);
        CRM_CHECK(type != NULL, return NULL);

        crm_debug_2("Adding rsc %s before operation", short_id);
        strncpy(rid, short_id, RID_LEN);
        rid[RID_LEN - 1] = 0;

        if (g_hash_table_size(params) == 0) {
            crm_log_xml_warn(op_msg, "EmptyParams");
        }

        if (params != NULL) {
            g_hash_table_remove(params, CRM_META "_op_target_rc");
        }

        fsa_lrm_conn->lrm_ops->add_rsc(fsa_lrm_conn, rid, class, type, provider, params);

        rsc = fsa_lrm_conn->lrm_ops->get_rsc(fsa_lrm_conn, rid);
        g_hash_table_destroy(params);

        if (rsc == NULL) {
            fsa_data_t *msg_data = NULL;

            crm_err("Could not add resource %s to LRM", rid);
            register_fsa_error(C_FSA_INTERNAL, I_FAIL, NULL);
        }
    }
    return rsc;
}

static void
delete_resource(const char *id, lrm_rsc_t * rsc,
                const char *sys, const char *host, const char *user, ha_msg_input_t * request)
{
    int rc = HA_OK;

    crm_info("Removing resource %s for %s (%s) on %s", id, sys, user ? user : "internal", host);

    if (rsc) {
        rc = fsa_lrm_conn->lrm_ops->delete_rsc(fsa_lrm_conn, id);
    }

    if (rc == HA_OK) {
        crm_trace("Resource '%s' deleted", id);

#ifdef HAVE_LRM_OP_T_RSC_DELETED
    } else if (rc == HA_RSCBUSY) {
        crm_info("Deletion of resource '%s' pending", id);
        if (request) {
            struct pending_deletion_op_s *op = NULL;
            char *ref = crm_element_value_copy(request->msg, XML_ATTR_REFERENCE);

            crm_malloc0(op, sizeof(struct pending_deletion_op_s));
            op->rsc = crm_strdup(rsc->id);
            op->input = copy_ha_msg_input(request);
            g_hash_table_insert(deletion_ops, ref, op);
        }
        return;

#endif
    } else {
        crm_warn("Deletion of resource '%s' for %s (%s) on %s failed: %d",
                 id, sys, user ? user : "internal", host, rc);
    }

    delete_rsc_entry(request, id, rc, user);
}

/*	 A_LRM_INVOKE	*/
void
do_lrm_invoke(long long action,
              enum crmd_fsa_cause cause,
              enum crmd_fsa_state cur_state,
              enum crmd_fsa_input current_input, fsa_data_t * msg_data)
{
    gboolean done = FALSE;
    gboolean create_rsc = TRUE;
    const char *crm_op = NULL;
    const char *from_sys = NULL;
    const char *from_host = NULL;
    const char *operation = NULL;
    ha_msg_input_t *input = fsa_typed_data(fsa_dt_ha_msg);
    const char *user_name = NULL;

#if ENABLE_ACL
    user_name = crm_element_value(input->msg, F_CRM_USER);
    crm_debug_2("LRM command from user '%s'", user_name);
#endif

    crm_op = crm_element_value(input->msg, F_CRM_TASK);
    from_sys = crm_element_value(input->msg, F_CRM_SYS_FROM);
    if (safe_str_neq(from_sys, CRM_SYSTEM_TENGINE)) {
        from_host = crm_element_value(input->msg, F_CRM_HOST_FROM);
    }

    crm_debug_2("LRM command from: %s", from_sys);

    if (safe_str_eq(crm_op, CRM_OP_LRM_DELETE)) {
        operation = CRMD_ACTION_DELETE;

    } else if (safe_str_eq(operation, CRM_OP_LRM_REFRESH)) {
        crm_op = CRM_OP_LRM_REFRESH;

    } else if (safe_str_eq(crm_op, CRM_OP_LRM_FAIL)) {
#if HAVE_STRUCT_LRM_OPS_FAIL_RSC
        int rc = HA_OK;
        lrm_op_t *op = NULL;

        lrm_rsc_t *rsc = NULL;
        xmlNode *xml_rsc = find_xml_node(input->xml, XML_CIB_TAG_RESOURCE, TRUE);

        CRM_CHECK(xml_rsc != NULL, return);

        op = construct_op(input->xml, ID(xml_rsc), "fail");
        op->op_status = LRM_OP_ERROR;
        op->rc = EXECRA_UNKNOWN_ERROR;
        CRM_ASSERT(op != NULL);

#  if ENABLE_ACL
        if (user_name && is_privileged(user_name) == FALSE) {
            crm_err("%s does not have permission to fail %s", user_name, ID(xml_rsc));
            send_direct_ack(from_host, from_sys, NULL, op, ID(xml_rsc));
            free_lrm_op(op);
            return;
        }
#  endif

        rsc = get_lrm_resource(xml_rsc, input->xml, create_rsc);
        if (rsc) {
            crm_info("Failing resource %s...", rsc->id);

            rc = fsa_lrm_conn->lrm_ops->fail_rsc(fsa_lrm_conn, rsc->id, 1,
                                                 "do_lrm_invoke: Async failure");
            if (rc != HA_OK) {
                crm_err("Could not initiate an asynchronous failure for %s (%d)", rsc->id, rc);
            } else {
                op->op_status = LRM_OP_DONE;
                op->rc = EXECRA_OK;
            }

            lrm_free_rsc(rsc);

        } else {
            crm_info("Cannot find/create resource in order to fail it...");
            crm_log_xml_warn(input->msg, "bad input");
        }

        send_direct_ack(from_host, from_sys, NULL, op, ID(xml_rsc));
        free_lrm_op(op);
        return;
#else
        crm_info("Failing resource...");
        operation = "fail";
#endif

    } else if (input->xml != NULL) {
        operation = crm_element_value(input->xml, XML_LRM_ATTR_TASK);
    }

    if (safe_str_eq(crm_op, CRM_OP_LRM_REFRESH)) {
        enum cib_errors rc = cib_ok;
        xmlNode *fragment = do_lrm_query(TRUE);

        crm_info("Forcing a local LRM refresh");

        fsa_cib_update(XML_CIB_TAG_STATUS, fragment, cib_quorum_override, rc, user_name);
        free_xml(fragment);

    } else if (safe_str_eq(crm_op, CRM_OP_LRM_QUERY)) {
        xmlNode *data = do_lrm_query(FALSE);
        xmlNode *reply = create_reply(input->msg, data);

        if (relay_message(reply, TRUE) == FALSE) {
            crm_err("Unable to route reply");
            crm_log_xml(LOG_ERR, "reply", reply);
        }
        free_xml(reply);
        free_xml(data);

    } else if (safe_str_eq(operation, CRM_OP_PROBED)) {
        update_attrd(NULL, CRM_OP_PROBED, XML_BOOLEAN_TRUE, user_name);

    } else if (safe_str_eq(crm_op, CRM_OP_REPROBE)) {
        GHashTableIter gIter;
        rsc_history_t *entry = NULL;

        crm_notice("Forcing the status of all resources to be redetected");

        g_hash_table_iter_init(&gIter, resource_history);
        while (g_hash_table_iter_next(&gIter, NULL, (void **)&entry)) {
            lrm_rsc_t *rsc = fsa_lrm_conn->lrm_ops->get_rsc(fsa_lrm_conn, entry->id);

            delete_resource(entry->id, rsc, from_sys, from_host, user_name, NULL);
            lrm_free_rsc(rsc);
        }

        /* Now delete the copy in the CIB */
        erase_status_tag(fsa_our_uname, XML_CIB_TAG_LRM, cib_scope_local);

        /* And finally, _delete_ the value in attrd
         * Setting it to FALSE results in the PE sending us back here again
         */
        update_attrd(NULL, CRM_OP_PROBED, NULL, user_name);

    } else if (operation != NULL) {
        lrm_rsc_t *rsc = NULL;
        xmlNode *params = NULL;
        xmlNode *xml_rsc = find_xml_node(input->xml, XML_CIB_TAG_RESOURCE, TRUE);

        CRM_CHECK(xml_rsc != NULL, return);

        /* only the first 16 chars are used by the LRM */
        params = find_xml_node(input->xml, XML_TAG_ATTRS, TRUE);

        if (safe_str_eq(operation, CRMD_ACTION_DELETE)) {
            create_rsc = FALSE;
        }

        rsc = get_lrm_resource(xml_rsc, input->xml, create_rsc);

        if (rsc == NULL && create_rsc) {
            crm_err("Invalid resource definition");
            crm_log_xml_warn(input->msg, "bad input");

        } else if (rsc == NULL) {
            lrm_op_t *op = NULL;

            crm_notice("Not creating resource for a %s event: %s", operation, ID(input->xml));
            delete_rsc_entry(input, ID(xml_rsc), HA_OK, user_name);

            op = construct_op(input->xml, ID(xml_rsc), operation);
            op->op_status = LRM_OP_DONE;
            op->rc = EXECRA_OK;
            CRM_ASSERT(op != NULL);
            send_direct_ack(from_host, from_sys, NULL, op, ID(xml_rsc));
            free_lrm_op(op);

        } else if (safe_str_eq(operation, CRMD_ACTION_CANCEL)) {
            lrm_op_t *op = NULL;
            char *op_key = NULL;
            char *meta_key = NULL;
            int call = 0;
            const char *call_id = NULL;
            const char *op_task = NULL;
            const char *op_interval = NULL;

            CRM_CHECK(params != NULL, crm_log_xml_warn(input->xml, "Bad command");
                      return);

            meta_key = crm_meta_name(XML_LRM_ATTR_INTERVAL);
            op_interval = crm_element_value(params, meta_key);
            crm_free(meta_key);

            meta_key = crm_meta_name(XML_LRM_ATTR_TASK);
            op_task = crm_element_value(params, meta_key);
            crm_free(meta_key);

            meta_key = crm_meta_name(XML_LRM_ATTR_CALLID);
            call_id = crm_element_value(params, meta_key);
            crm_free(meta_key);

            CRM_CHECK(op_task != NULL, crm_log_xml_warn(input->xml, "Bad command");
                      return);
            CRM_CHECK(op_interval != NULL, crm_log_xml_warn(input->xml, "Bad command");
                      return);

            op = construct_op(input->xml, rsc->id, op_task);
            CRM_ASSERT(op != NULL);
            op_key = generate_op_key(rsc->id, op_task, crm_parse_int(op_interval, "0"));

            crm_debug("PE requested op %s (call=%s) be cancelled",
                      op_key, call_id ? call_id : "NA");
            call = crm_parse_int(call_id, "0");
            if (call == 0) {
                /* the normal case when the PE cancels a recurring op */
                done = cancel_op_key(rsc, op_key, TRUE);

            } else {
                /* the normal case when the PE cancels an orphan op */
                done = cancel_op(rsc, NULL, call, TRUE);
            }

            if (done == FALSE) {
                crm_debug("Nothing known about operation %d for %s", call, op_key);
                delete_op_entry(NULL, rsc->id, op_key, call);

                /* needed?? surely not otherwise the cancel_op_(_key) wouldn't
                 * have failed in the first place
                 */
                g_hash_table_remove(pending_ops, op_key);
            }

            op->rc = EXECRA_OK;
            op->op_status = LRM_OP_DONE;
            send_direct_ack(from_host, from_sys, rsc, op, rsc->id);

            crm_free(op_key);
            free_lrm_op(op);

        } else if (safe_str_eq(operation, CRMD_ACTION_DELETE)) {
            int cib_rc = cib_ok;

            CRM_ASSERT(rsc != NULL);

            cib_rc = delete_rsc_status(rsc->id, cib_dryrun | cib_sync_call, user_name);
            if (cib_rc != cib_ok) {
                lrm_op_t *op = NULL;

                crm_err
                    ("Attempt of deleting resource status '%s' from CIB for %s (user=%s) on %s failed: (rc=%d) %s",
                     rsc->id, from_sys, user_name ? user_name : "unknown", from_host, cib_rc,
                     cib_error2string(cib_rc));

                op = construct_op(input->xml, rsc->id, operation);
                op->op_status = LRM_OP_ERROR;

                if (cib_rc == cib_permission_denied) {
                    op->rc = EXECRA_INSUFFICIENT_PRIV;
                } else {
                    op->rc = EXECRA_UNKNOWN_ERROR;
                }
                send_direct_ack(from_host, from_sys, NULL, op, rsc->id);
                free_lrm_op(op);
                return;
            }

            delete_resource(rsc->id, rsc, from_sys, from_host, user_name, input);

        } else if (rsc != NULL) {
            do_lrm_rsc_op(rsc, operation, input->xml, input->msg);
        }

        lrm_free_rsc(rsc);

    } else {
        crm_err("Operation was neither a lrm_query, nor a rsc op.  %s", crm_str(crm_op));
        register_fsa_error(C_FSA_INTERNAL, I_ERROR, NULL);
    }
}

static void
copy_meta_keys(gpointer key, gpointer value, gpointer user_data)
{
    if (strstr(key, CRM_META "_") != NULL) {
        g_hash_table_insert(user_data, strdup((const char *)key), strdup((const char *)value));
    }
}

lrm_op_t *
construct_op(xmlNode * rsc_op, const char *rsc_id, const char *operation)
{
    lrm_op_t *op = NULL;
    const char *op_delay = NULL;
    const char *op_timeout = NULL;
    const char *op_interval = NULL;
    GHashTable *params = NULL;

    const char *transition = NULL;

    CRM_LOG_ASSERT(rsc_id != NULL);

    crm_malloc0(op, sizeof(lrm_op_t));
    op->op_type = crm_strdup(operation);
    op->op_status = LRM_OP_PENDING;
    op->rc = -1;
    op->rsc_id = crm_strdup(rsc_id);
    op->interval = 0;
    op->timeout = 0;
    op->start_delay = 0;
    op->copyparams = 0;
    op->app_name = crm_strdup(CRM_SYSTEM_CRMD);

    if (rsc_op == NULL) {
        CRM_LOG_ASSERT(safe_str_eq(CRMD_ACTION_STOP, operation));
        op->user_data = NULL;
        op->user_data_len = 0;
        /* the stop_all_resources() case
         * by definition there is no DC (or they'd be shutting
         *   us down).
         * So we should put our version here.
         */
        op->params = g_hash_table_new_full(crm_str_hash, g_str_equal,
                                           g_hash_destroy_str, g_hash_destroy_str);

        g_hash_table_insert(op->params,
                            crm_strdup(XML_ATTR_CRM_VERSION), crm_strdup(CRM_FEATURE_SET));

        crm_debug_2("Constructed %s op for %s", operation, rsc_id);
        return op;
    }

    params = xml2list(rsc_op);
    g_hash_table_remove(params, CRM_META "_op_target_rc");

    op_delay = crm_meta_value(params, XML_OP_ATTR_START_DELAY);
    op_timeout = crm_meta_value(params, XML_ATTR_TIMEOUT);
    op_interval = crm_meta_value(params, XML_LRM_ATTR_INTERVAL);

    op->interval = crm_parse_int(op_interval, "0");
    op->timeout = crm_parse_int(op_timeout, "0");
    op->start_delay = crm_parse_int(op_delay, "0");

    if (safe_str_neq(operation, RSC_STOP)) {
        op->params = params;

    } else {
        /* Create a blank parameter list so that we stop the resource
         * with the old attributes, not the new ones
         */
        const char *version = g_hash_table_lookup(params, XML_ATTR_CRM_VERSION);

        op->params = g_hash_table_new_full(crm_str_hash, g_str_equal,
                                           g_hash_destroy_str, g_hash_destroy_str);

        if (version) {
            g_hash_table_insert(op->params, crm_strdup(XML_ATTR_CRM_VERSION), crm_strdup(version));
        }

        g_hash_table_foreach(params, copy_meta_keys, op->params);
        g_hash_table_destroy(params);
        params = NULL;
    }

    /* sanity */
    if (op->interval < 0) {
        op->interval = 0;
    }
    if (op->timeout <= 0) {
        op->timeout = op->interval;
    }
    if (op->start_delay < 0) {
        op->start_delay = 0;
    }

    transition = crm_element_value(rsc_op, XML_ATTR_TRANSITION_KEY);
    CRM_CHECK(transition != NULL, return op);

    op->user_data = crm_strdup(transition);
    op->user_data_len = 1 + strlen(op->user_data);

    if (op->interval != 0) {
        if (safe_str_eq(operation, CRMD_ACTION_START)
            || safe_str_eq(operation, CRMD_ACTION_STOP)) {
            crm_err("Start and Stop actions cannot have an interval: %d", op->interval);
            op->interval = 0;
        }
    }

    /* reset the resource's parameters? */
    if (op->interval == 0) {
        if (safe_str_eq(CRMD_ACTION_START, operation)
            || safe_str_eq(CRMD_ACTION_STATUS, operation)) {
            op->copyparams = 1;
        }
    }

    crm_debug_2("Constructed %s op for %s: interval=%d", operation, rsc_id, op->interval);

    return op;
}

void
send_direct_ack(const char *to_host, const char *to_sys,
                lrm_rsc_t * rsc, lrm_op_t * op, const char *rsc_id)
{
    xmlNode *reply = NULL;
    xmlNode *update, *iter;
    xmlNode *fragment;

    CRM_CHECK(op != NULL, return);
    if (op->rsc_id == NULL) {
        CRM_LOG_ASSERT(rsc_id != NULL);
        op->rsc_id = crm_strdup(rsc_id);
    }
    if (to_sys == NULL) {
        to_sys = CRM_SYSTEM_TENGINE;
    }
    update = create_node_state(fsa_our_uname, NULL, NULL, NULL, NULL, NULL, FALSE, __FUNCTION__);

    iter = create_xml_node(update, XML_CIB_TAG_LRM);
    crm_xml_add(iter, XML_ATTR_ID, fsa_our_uuid);
    iter = create_xml_node(iter, XML_LRM_TAG_RESOURCES);
    iter = create_xml_node(iter, XML_LRM_TAG_RESOURCE);

    crm_xml_add(iter, XML_ATTR_ID, op->rsc_id);

    build_operation_update(iter, rsc, op, __FUNCTION__);
    fragment = create_cib_fragment(update, XML_CIB_TAG_STATUS);

    reply = create_request(CRM_OP_INVOKE_LRM, fragment, to_host, to_sys, CRM_SYSTEM_LRMD, NULL);

    crm_log_xml_debug_2(update, "ACK Update");

    crm_info("ACK'ing resource op %s_%s_%d from %s: %s",
             op->rsc_id, op->op_type, op->interval, op->user_data,
             crm_element_value(reply, XML_ATTR_REFERENCE));

    if (relay_message(reply, TRUE) == FALSE) {
        crm_log_xml(LOG_ERR, "Unable to route reply", reply);
    }

    free_xml(fragment);
    free_xml(update);
    free_xml(reply);
}

static gboolean
stop_recurring_action_by_rsc(gpointer key, gpointer value, gpointer user_data)
{
    lrm_rsc_t *rsc = user_data;
    struct recurring_op_s *op = (struct recurring_op_s *)value;

    if (op->interval != 0 && safe_str_eq(op->rsc_id, rsc->id)) {
        if (cancel_op(rsc, key, op->call_id, FALSE) == FALSE) {
            return TRUE;
        }
    }

    return FALSE;
}

static gboolean
stop_recurring_actions(gpointer key, gpointer value, gpointer user_data)
{
    gboolean remove = FALSE;
    struct recurring_op_s *op = (struct recurring_op_s *)value;

    if (op->interval != 0) {
        lrm_rsc_t *rsc = fsa_lrm_conn->lrm_ops->get_rsc(fsa_lrm_conn, op->rsc_id);

        if (rsc) {
            remove = cancel_op(rsc, key, op->call_id, FALSE);
            lrm_free_rsc(rsc);
        }
    }

    return remove;
}

void
do_lrm_rsc_op(lrm_rsc_t * rsc, const char *operation, xmlNode * msg, xmlNode * request)
{
    int call_id = 0;
    char *op_id = NULL;
    lrm_op_t *op = NULL;

    fsa_data_t *msg_data = NULL;
    const char *transition = NULL;

    CRM_CHECK(rsc != NULL, return);

    if (msg != NULL) {
        transition = crm_element_value(msg, XML_ATTR_TRANSITION_KEY);
        if (transition == NULL) {
            crm_log_xml_err(msg, "Missing transition number");
        }
    }

    op = construct_op(msg, rsc->id, operation);

    /* stop the monitor before stopping the resource */
    if (crm_str_eq(operation, CRMD_ACTION_STOP, TRUE)
        || crm_str_eq(operation, CRMD_ACTION_DEMOTE, TRUE)
        || crm_str_eq(operation, CRMD_ACTION_PROMOTE, TRUE)
        || crm_str_eq(operation, CRMD_ACTION_MIGRATE, TRUE)) {
        g_hash_table_foreach_remove(pending_ops, stop_recurring_action_by_rsc, rsc);
    }

    /* now do the op */
    crm_info("Performing key=%s op=%s_%s_%d )", transition, rsc->id, operation, op->interval);

    if (fsa_state != S_NOT_DC && fsa_state != S_POLICY_ENGINE && fsa_state != S_TRANSITION_ENGINE) {
        if (safe_str_neq(operation, "fail")
            && safe_str_neq(operation, CRMD_ACTION_STOP)) {
            crm_info("Discarding attempt to perform action %s on %s"
                     " in state %s", operation, rsc->id, fsa_state2string(fsa_state));
            op->rc = 99;
            op->op_status = LRM_OP_ERROR;
            send_direct_ack(NULL, NULL, rsc, op, rsc->id);
            free_lrm_op(op);
            crm_free(op_id);
            return;
        }
    }

    op_id = generate_op_key(rsc->id, op->op_type, op->interval);

    if (op->interval > 0) {
        /* cancel it so we can then restart it without conflict */
        cancel_op_key(rsc, op_id, FALSE);
        op->target_rc = CHANGED;

    } else {
        op->target_rc = EVERYTIME;
    }

    call_id = rsc->ops->perform_op(rsc, op);

    if (call_id <= 0) {
        crm_err("Operation %s on %s failed: %d", operation, rsc->id, call_id);
        register_fsa_error(C_FSA_INTERNAL, I_FAIL, NULL);

    } else {
        /* record all operations so we can wait
         * for them to complete during shutdown
         */
        char *call_id_s = make_stop_id(rsc->id, call_id);
        struct recurring_op_s *pending = NULL;

        crm_malloc0(pending, sizeof(struct recurring_op_s));
        crm_debug_2("Recording pending op: %d - %s %s", call_id, op_id, call_id_s);

        pending->call_id = call_id;
        pending->interval = op->interval;
        pending->op_key = crm_strdup(op_id);
        pending->rsc_id = crm_strdup(rsc->id);
        g_hash_table_replace(pending_ops, call_id_s, pending);

        if (op->interval > 0 && op->start_delay > START_DELAY_THRESHOLD) {
            char *uuid = NULL;
            int dummy = 0, target_rc = 0;

            crm_info("Faking confirmation of %s: execution postponed for over 5 minutes", op_id);

            decode_transition_key(op->user_data, &uuid, &dummy, &dummy, &target_rc);
            crm_free(uuid);

            op->rc = target_rc;
            op->op_status = LRM_OP_DONE;
            send_direct_ack(NULL, NULL, rsc, op, rsc->id);
        }
    }

    crm_free(op_id);
    free_lrm_op(op);
    return;
}

lrm_op_t *
copy_lrm_op(const lrm_op_t * op)
{
    lrm_op_t *op_copy = NULL;

    CRM_CHECK(op != NULL, return NULL);
    CRM_CHECK(op->rsc_id != NULL, return NULL);

    crm_malloc0(op_copy, sizeof(lrm_op_t));

    op_copy->op_type = crm_strdup(op->op_type);
    /* input fields */
    op_copy->params = g_hash_table_new_full(crm_str_hash, g_str_equal,
                                            g_hash_destroy_str, g_hash_destroy_str);

    if (op->params != NULL) {
        g_hash_table_foreach(op->params, dup_attr, op_copy->params);
    }
    op_copy->timeout = op->timeout;
    op_copy->interval = op->interval;
    op_copy->target_rc = op->target_rc;

    /* in the CRM, this is always a string */
    if (op->user_data != NULL) {
        op_copy->user_data = crm_strdup(op->user_data);
    }

    /* output fields */
    op_copy->op_status = op->op_status;
    op_copy->rc = op->rc;
    op_copy->call_id = op->call_id;
    op_copy->output = NULL;
    op_copy->rsc_id = crm_strdup(op->rsc_id);
    if (op->app_name != NULL) {
        op_copy->app_name = crm_strdup(op->app_name);
    }
    if (op->output != NULL) {
        op_copy->output = crm_strdup(op->output);
    }

    return op_copy;
}

lrm_rsc_t *
copy_lrm_rsc(const lrm_rsc_t * rsc)
{
    lrm_rsc_t *rsc_copy = NULL;

    if (rsc == NULL) {
        return NULL;
    }

    crm_malloc0(rsc_copy, sizeof(lrm_rsc_t));

    rsc_copy->id = crm_strdup(rsc->id);
    rsc_copy->type = crm_strdup(rsc->type);
    rsc_copy->class = NULL;
    rsc_copy->provider = NULL;

    if (rsc->class != NULL) {
        rsc_copy->class = crm_strdup(rsc->class);
    }
    if (rsc->provider != NULL) {
        rsc_copy->provider = crm_strdup(rsc->provider);
    }
/* 	GHashTable* 	params; */
    rsc_copy->params = NULL;
    rsc_copy->ops = NULL;

    return rsc_copy;
}

static void
cib_rsc_callback(xmlNode * msg, int call_id, int rc, xmlNode * output, void *user_data)
{
    switch (rc) {
        case cib_ok:
        case cib_diff_failed:
        case cib_diff_resync:
            crm_debug_2("Resource update %d complete: rc=%d", call_id, rc);
            break;
        default:
            crm_warn("Resource update %d failed: (rc=%d) %s", call_id, rc, cib_error2string(rc));
    }
}

static int
do_update_resource(lrm_rsc_t * rsc, lrm_op_t * op)
{
/*
  <status>
  <nodes_status id=uname>
  <lrm>
  <lrm_resources>
  <lrm_resource id=...>
  </...>
*/
    int rc = cib_ok;
    xmlNode *update, *iter = NULL;
    int call_opt = cib_quorum_override;

    CRM_CHECK(op != NULL, return 0);

    if (fsa_state == S_ELECTION || fsa_state == S_PENDING) {
        crm_info("Sending update to local CIB in state: %s", fsa_state2string(fsa_state));
        call_opt |= cib_scope_local;
    }

    iter = create_xml_node(iter, XML_CIB_TAG_STATUS);
    update = iter;
    iter = create_xml_node(iter, XML_CIB_TAG_STATE);

    set_uuid(iter, XML_ATTR_UUID, fsa_our_uname);
    crm_xml_add(iter, XML_ATTR_UNAME, fsa_our_uname);
    crm_xml_add(iter, XML_ATTR_ORIGIN, __FUNCTION__);

    iter = create_xml_node(iter, XML_CIB_TAG_LRM);
    crm_xml_add(iter, XML_ATTR_ID, fsa_our_uuid);

    iter = create_xml_node(iter, XML_LRM_TAG_RESOURCES);
    iter = create_xml_node(iter, XML_LRM_TAG_RESOURCE);
    crm_xml_add(iter, XML_ATTR_ID, op->rsc_id);

    build_operation_update(iter, rsc, op, __FUNCTION__);

    if (rsc) {
        crm_xml_add(iter, XML_ATTR_TYPE, rsc->type);
        crm_xml_add(iter, XML_AGENT_ATTR_CLASS, rsc->class);
        crm_xml_add(iter, XML_AGENT_ATTR_PROVIDER, rsc->provider);

        CRM_CHECK(rsc->type != NULL, crm_err("Resource %s has no value for type", op->rsc_id));
        CRM_CHECK(rsc->class != NULL, crm_err("Resource %s has no value for class", op->rsc_id));

    } else {
        crm_warn("Resource %s no longer exists in the lrmd", op->rsc_id);
        goto cleanup;
    }

    /* make it an asyncronous call and be done with it
     *
     * Best case:
     *   the resource state will be discovered during
     *   the next signup or election.
     *
     * Bad case:
     *   we are shutting down and there is no DC at the time,
     *   but then why were we shutting down then anyway?
     *   (probably because of an internal error)
     *
     * Worst case:
     *   we get shot for having resources "running" when the really weren't
     *
     * the alternative however means blocking here for too long, which
     * isnt acceptable
     */
    fsa_cib_update(XML_CIB_TAG_STATUS, update, call_opt, rc, NULL);

    /* the return code is a call number, not an error code */
    crm_debug_2("Sent resource state update message: %d", rc);
    fsa_cib_conn->cmds->register_callback(fsa_cib_conn, rc, 60, FALSE, NULL, "cib_rsc_callback",
                                          cib_rsc_callback);

  cleanup:
    free_xml(update);
    return rc;
}

void
do_lrm_event(long long action,
             enum crmd_fsa_cause cause,
             enum crmd_fsa_state cur_state, enum crmd_fsa_input cur_input, fsa_data_t * msg_data)
{
    CRM_CHECK(FALSE, return);
}

gboolean
process_lrm_event(lrm_op_t * op)
{
    char *op_id = NULL;
    char *op_key = NULL;

    int update_id = 0;
    int log_level = LOG_ERR;
    gboolean removed = FALSE;
    lrm_rsc_t *rsc = NULL;

    struct recurring_op_s *pending = NULL;

    CRM_CHECK(op != NULL, return FALSE);
    CRM_CHECK(op->rsc_id != NULL, return FALSE);

    op_key = generate_op_key(op->rsc_id, op->op_type, op->interval);

    switch (op->op_status) {
        case LRM_OP_ERROR:
        case LRM_OP_PENDING:
        case LRM_OP_NOTSUPPORTED:
            break;
        case LRM_OP_CANCELLED:
            log_level = LOG_INFO;
            break;
        case LRM_OP_DONE:
            log_level = LOG_INFO;
            break;
        case LRM_OP_TIMEOUT:
            log_level = LOG_DEBUG_3;
            crm_err("LRM operation %s (%d) %s (timeout=%dms)",
                    op_key, op->call_id, op_status2text(op->op_status), op->timeout);
            break;
        default:
            crm_err("Mapping unknown status (%d) to ERROR", op->op_status);
            op->op_status = LRM_OP_ERROR;
    }

    if (op->op_status == LRM_OP_ERROR
        && (op->rc == EXECRA_RUNNING_MASTER || op->rc == EXECRA_NOT_RUNNING)) {
        /* Leave it up to the TE/PE to decide if this is an error */
        op->op_status = LRM_OP_DONE;
        log_level = LOG_INFO;
    }

    op_id = make_stop_id(op->rsc_id, op->call_id);
    pending = g_hash_table_lookup(pending_ops, op_id);
    rsc = fsa_lrm_conn->lrm_ops->get_rsc(fsa_lrm_conn, op->rsc_id);

    if (op->op_status != LRM_OP_CANCELLED) {
        if (safe_str_eq(op->op_type, RSC_NOTIFY)) {
            /* Keep notify ops out of the CIB */
            send_direct_ack(NULL, NULL, NULL, op, op->rsc_id);
        } else {
            update_id = do_update_resource(rsc, op);
        }

        if (op->interval != 0) {
            goto out;
        }

    } else if (op->interval == 0) {
        /* This will occur when "crm resource cleanup" is called while actions are in-flight */
        crm_err("Op %s (call=%d): Cancelled", op_key, op->call_id);
        send_direct_ack(NULL, NULL, NULL, op, op->rsc_id);

    } else if (pending == NULL) {
        crm_err("Op %s (call=%d): No 'pending' entry", op_key, op->call_id);

    } else if (op->user_data == NULL) {
        crm_err("Op %s (call=%d): No user data", op_key, op->call_id);

    } else if (pending->remove) {
        delete_op_entry(op, op->rsc_id, op_key, op->call_id);

    } else {
        /* Before a stop is called, no need to direct ack */
        crm_debug_2("Op %s (call=%d): no delete event required", op_key, op->call_id);
    }

    if (g_hash_table_remove(pending_ops, op_id)) {
        removed = TRUE;
        crm_debug_2("Op %s (call=%d, stop-id=%s): Confirmed", op_key, op->call_id, op_id);
    }

  out:
    if (op->op_status == LRM_OP_DONE) {
        do_crm_log(log_level,
                   "LRM operation %s (call=%d, rc=%d, cib-update=%d, confirmed=%s) %s",
                   op_key, op->call_id, op->rc, update_id, removed ? "true" : "false",
                   execra_code2string(op->rc));
    } else {
        do_crm_log(log_level,
                   "LRM operation %s (call=%d, status=%d, cib-update=%d, confirmed=%s) %s",
                   op_key, op->call_id, op->op_status, update_id, removed ? "true" : "false",
                   op_status2text(op->op_status));
    }

    if (op->rc != 0 && op->output != NULL) {
        crm_info("Result: %s", op->output);
    } else if (op->output != NULL) {
        crm_debug("Result: %s", op->output);
    }
#ifdef HAVE_LRM_OP_T_RSC_DELETED
    if (op->rsc_deleted) {
        crm_info("Deletion of resource '%s' complete after %s", op->rsc_id, op_key);
        delete_rsc_entry(NULL, op->rsc_id, HA_OK, NULL);
    }
#endif

    /* If a shutdown was escalated while operations were pending, 
     * then the FSA will be stalled right now... allow it to continue
     */
    mainloop_set_trigger(fsa_source);
    update_history_cache(rsc, op);

    lrm_free_rsc(rsc);
    crm_free(op_key);
    crm_free(op_id);

    return TRUE;
}
