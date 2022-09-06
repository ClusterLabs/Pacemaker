/*
 * Copyright 2004-2022 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU General Public License version 2
 * or later (GPLv2+) WITHOUT ANY WARRANTY.
 */

#include <crm_internal.h>

#include <stdio.h>
#include <stdbool.h>
#include <errno.h>
#include <glib.h>
#include <regex.h>
#include <sys/types.h>

#include <crm/crm.h>
#include <crm/common/ipc_internal.h>
#include <crm/common/mainloop.h>
#include <crm/msg_xml.h>

#include "pacemaker-attrd.h"

cib_t *the_cib = NULL;

static bool requesting_shutdown = false;
static bool shutting_down = false;
static GMainLoop *mloop = NULL;

/*!
 * \internal
 * \brief  Set requesting_shutdown state
 */
void
attrd_set_requesting_shutdown(void)
{
    requesting_shutdown = true;
}

/*!
 * \internal
 * \brief  Clear requesting_shutdown state
 */
void
attrd_clear_requesting_shutdown(void)
{
    requesting_shutdown = false;
}

/*!
 * \internal
 * \brief Check whether we're currently requesting shutdown
 *
 * \return true if requesting shutdown, false otherwise
 */
bool
attrd_requesting_shutdown(void)
{
    return requesting_shutdown;
}

/*!
 * \internal
 * \brief Check whether we're currently shutting down
 *
 * \return true if shutting down, false otherwise
 */
bool
attrd_shutting_down(void)
{
    return shutting_down;
}

/*!
 * \internal
 * \brief  Exit (using mainloop or not, as appropriate)
 *
 * \param[in] nsig  Ignored
 */
void
attrd_shutdown(int nsig)
{
    // Tell various functions not to do anthing
    shutting_down = true;

    // Don't respond to signals while shutting down
    mainloop_destroy_signal(SIGTERM);
    mainloop_destroy_signal(SIGCHLD);
    mainloop_destroy_signal(SIGPIPE);
    mainloop_destroy_signal(SIGUSR1);
    mainloop_destroy_signal(SIGUSR2);
    mainloop_destroy_signal(SIGTRAP);

    if ((mloop == NULL) || !g_main_loop_is_running(mloop)) {
        /* If there's no main loop active, just exit. This should be possible
         * only if we get SIGTERM in brief windows at start-up and shutdown.
         */
        crm_exit(CRM_EX_OK);
    } else {
        g_main_loop_quit(mloop);
        g_main_loop_unref(mloop);
    }
}

/*!
 * \internal
 * \brief Create a main loop for attrd
 */
void
attrd_init_mainloop(void)
{
    mloop = g_main_loop_new(NULL, FALSE);
}

/*!
 * \internal
 * \brief Run attrd main loop
 */
void
attrd_run_mainloop(void)
{
    g_main_loop_run(mloop);
}

void
attrd_cib_disconnect(void)
{
    CRM_CHECK(the_cib != NULL, return);
    the_cib->cmds->del_notify_callback(the_cib, T_CIB_REPLACE_NOTIFY, attrd_cib_replaced_cb);
    the_cib->cmds->del_notify_callback(the_cib, T_CIB_DIFF_NOTIFY, attrd_cib_updated_cb);
    cib__clean_up_connection(&the_cib);
}

void
attrd_cib_replaced_cb(const char *event, xmlNode * msg)
{
    int change_section = cib_change_section_nodes | cib_change_section_status | cib_change_section_alerts;

    if (attrd_requesting_shutdown() || attrd_shutting_down()) {
        return;
    }

    crm_element_value_int(msg, F_CIB_CHANGE_SECTION, &change_section);

    if (attrd_election_won()) {
        if (change_section & (cib_change_section_nodes | cib_change_section_status)) {
            crm_notice("Updating all attributes after %s event", event);
            attrd_write_attributes(true, false);
        }
    }

    if (change_section & cib_change_section_alerts) {
        // Check for changes in alerts
        mainloop_set_trigger(attrd_config_read);
    }
}

/* strlen("value") */
#define plus_plus_len (5)

/*!
 * \internal
 * \brief  Check whether an attribute value should be expanded
 *
 * \param[in] value  Attribute value to check
 *
 * \return true if value needs expansion, false otherwise
 */
bool
attrd_value_needs_expansion(const char *value)
{
    return ((strlen(value) >= (plus_plus_len + 2))
           && (value[plus_plus_len] == '+')
           && ((value[plus_plus_len + 1] == '+')
               || (value[plus_plus_len + 1] == '=')));
}

/*!
 * \internal
 * \brief Expand an increment expression into an integer
 *
 * \param[in] value      Attribute increment expression to expand
 * \param[in] old_value  Previous value of attribute
 *
 * \return Expanded value
 */
int
attrd_expand_value(const char *value, const char *old_value)
{
    int offset = 1;
    int int_value = char2score(old_value);

    if (value[plus_plus_len + 1] != '+') {
        const char *offset_s = value + (plus_plus_len + 2);

        offset = char2score(offset_s);
    }
    int_value += offset;

    if (int_value > INFINITY) {
        int_value = INFINITY;
    }
    return int_value;
}

/*!
 * \internal
 * \brief Create regular expression matching failure-related attributes
 *
 * \param[out] regex  Where to store created regular expression
 * \param[in]  rsc    Name of resource to clear (or NULL for all)
 * \param[in]  op     Operation to clear if rsc is specified (or NULL for all)
 * \param[in]  interval_ms  Interval of operation to clear if op is specified
 *
 * \return pcmk_ok on success, -EINVAL if arguments are invalid
 *
 * \note The caller is responsible for freeing the result with regfree().
 */
int
attrd_failure_regex(regex_t *regex, const char *rsc, const char *op,
                    guint interval_ms)
{
    char *pattern = NULL;
    int rc;

    /* Create a pattern that matches desired attributes */

    if (rsc == NULL) {
        pattern = strdup(ATTRD_RE_CLEAR_ALL);
    } else if (op == NULL) {
        pattern = crm_strdup_printf(ATTRD_RE_CLEAR_ONE, rsc);
    } else {
        pattern = crm_strdup_printf(ATTRD_RE_CLEAR_OP, rsc, op, interval_ms);
    }

    /* Compile pattern into regular expression */
    crm_trace("Clearing attributes matching %s", pattern);
    rc = regcomp(regex, pattern, REG_EXTENDED|REG_NOSUB);
    free(pattern);

    return (rc == 0)? pcmk_ok : -EINVAL;
}

void
attrd_free_attribute_value(gpointer data)
{
    attribute_value_t *v = data;

    free(v->nodename);
    free(v->current);
    free(v->requested);
    free(v);
}

void
attrd_free_attribute(gpointer data)
{
    attribute_t *a = data;
    if(a) {
        free(a->id);
        free(a->set);
        free(a->uuid);
        free(a->user);

        mainloop_timer_del(a->timer);
        g_hash_table_destroy(a->values);

        free(a);
    }
}

void
attrd_update_minimum_protocol_ver(const char *value)
{
    int ver;

    pcmk__scan_min_int(value, &ver, 0);

    if (ver > 0 && (minimum_protocol_version == -1 || ver < minimum_protocol_version)) {
        minimum_protocol_version = ver;
        crm_trace("Set minimum attrd protocol version to %d",
                  minimum_protocol_version);
    }
}
