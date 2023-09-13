/*
 * Copyright 2013-2023 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU General Public License version 2
 * or later (GPLv2+) WITHOUT ANY WARRANTY.
 */

#include <crm_internal.h>

#include <errno.h>
#include <inttypes.h>   // PRIu32
#include <stdbool.h>
#include <stdlib.h>
#include <glib.h>

#include <crm/msg_xml.h>
#include <crm/common/logging.h>
#include <crm/common/results.h>
#include <crm/common/strings_internal.h>
#include <crm/common/xml.h>

#include "pacemaker-attrd.h"

static int last_cib_op_done = 0;

static void
attrd_cib_destroy_cb(gpointer user_data)
{
    cib_t *cib = user_data;

    cib->cmds->signoff(cib);

    if (attrd_shutting_down(false)) {
        crm_info("Disconnected from the CIB manager");

    } else {
        // @TODO This should trigger a reconnect, not a shutdown
        crm_crit("Lost connection to the CIB manager, shutting down");
        attrd_exit_status = CRM_EX_DISCONNECT;
        attrd_shutdown(0);
    }
}

static void
attrd_cib_updated_cb(const char *event, xmlNode *msg)
{
    const xmlNode *patchset = NULL;
    const char *client_name = NULL;

    if (attrd_shutting_down(true)) {
        return;
    }

    if (cib__get_notify_patchset(msg, &patchset) != pcmk_rc_ok) {
        return;
    }

    if (cib__element_in_patchset(patchset, XML_CIB_TAG_ALERTS)) {
        mainloop_set_trigger(attrd_config_read);
    }

    if (!attrd_election_won()) {
        // Don't write attributes if we're not the writer
        return;
    }

    client_name = crm_element_value(msg, F_CIB_CLIENTNAME);
    if (cib__client_name_is_safe(client_name)) {
        // The CIB is still accurate
        return;
    }

    if (cib__element_in_patchset(patchset, XML_CIB_TAG_NODES)
        || cib__element_in_patchset(patchset, XML_CIB_TAG_STATUS)) {

        /* An unsafe client modified the nodes or status section. Write
         * transient attributes to ensure they're up-to-date in the CIB.
         */
        if (client_name == NULL) {
            client_name = crm_element_value(msg, F_CIB_CLIENTID);
        }
        crm_notice("Updating all attributes after %s event triggered by %s",
                   event, pcmk__s(client_name, "(unidentified client)"));

        attrd_write_attributes(attrd_write_all);
    }
}

int
attrd_cib_connect(int max_retry)
{
    static int attempts = 0;

    int rc = -ENOTCONN;

    the_cib = cib_new();
    if (the_cib == NULL) {
        return -ENOTCONN;
    }

    do {
        if (attempts > 0) {
            sleep(attempts);
        }
        attempts++;
        crm_debug("Connection attempt %d to the CIB manager", attempts);
        rc = the_cib->cmds->signon(the_cib, T_ATTRD, cib_command);

    } while ((rc != pcmk_ok) && (attempts < max_retry));

    if (rc != pcmk_ok) {
        crm_err("Connection to the CIB manager failed: %s " CRM_XS " rc=%d",
                pcmk_strerror(rc), rc);
        goto cleanup;
    }

    crm_debug("Connected to the CIB manager after %d attempts", attempts);

    rc = the_cib->cmds->set_connection_dnotify(the_cib, attrd_cib_destroy_cb);
    if (rc != pcmk_ok) {
        crm_err("Could not set disconnection callback");
        goto cleanup;
    }

    rc = the_cib->cmds->add_notify_callback(the_cib, T_CIB_DIFF_NOTIFY,
                                            attrd_cib_updated_cb);
    if (rc != pcmk_ok) {
        crm_err("Could not set CIB notification callback");
        goto cleanup;
    }

    return pcmk_ok;

cleanup:
    cib__clean_up_connection(&the_cib);
    return -ENOTCONN;
}

void
attrd_cib_disconnect(void)
{
    CRM_CHECK(the_cib != NULL, return);
    the_cib->cmds->del_notify_callback(the_cib, T_CIB_DIFF_NOTIFY,
                                       attrd_cib_updated_cb);
    cib__clean_up_connection(&the_cib);
}

static void
attrd_erase_cb(xmlNode *msg, int call_id, int rc, xmlNode *output,
               void *user_data)
{
    do_crm_log_unlikely(((rc != pcmk_ok)? LOG_NOTICE : LOG_DEBUG),
                        "Cleared transient attributes: %s "
                        CRM_XS " xpath=%s rc=%d",
                        pcmk_strerror(rc), (char *) user_data, rc);
}

#define XPATH_TRANSIENT "//node_state[@uname='%s']/" XML_TAG_TRANSIENT_NODEATTRS

/*!
 * \internal
 * \brief Wipe all transient attributes for this node from the CIB
 *
 * Clear any previous transient node attributes from the CIB. This is
 * normally done by the DC's controller when this node leaves the cluster, but
 * this handles the case where the node restarted so quickly that the
 * cluster layer didn't notice.
 *
 * \todo If pacemaker-attrd respawns after crashing (see PCMK_respawned),
 *       ideally we'd skip this and sync our attributes from the writer.
 *       However, currently we reject any values for us that the writer has, in
 *       attrd_peer_update().
 */
static void
attrd_erase_attrs(void)
{
    int call_id = 0;
    char *xpath = crm_strdup_printf(XPATH_TRANSIENT, attrd_cluster->uname);

    crm_info("Clearing transient attributes from CIB " CRM_XS " xpath=%s",
             xpath);

    call_id = the_cib->cmds->remove(the_cib, xpath, NULL, cib_xpath);
    the_cib->cmds->register_callback_full(the_cib, call_id, 120, FALSE, xpath,
                                          "attrd_erase_cb", attrd_erase_cb,
                                          free);
}

/*!
 * \internal
 * \brief Prepare the CIB after cluster is connected
 */
void
attrd_cib_init(void)
{
    // We have no attribute values in memory, wipe the CIB to match
    attrd_erase_attrs();

    // Set a trigger for reading the CIB (for the alerts section)
    attrd_config_read = mainloop_add_trigger(G_PRIORITY_HIGH, attrd_read_options, NULL);

    // Always read the CIB at start-up
    mainloop_set_trigger(attrd_config_read);
}

static gboolean
attribute_timer_cb(gpointer data)
{
    attribute_t *a = data;
    crm_trace("Dampen interval expired for %s", a->id);
    attrd_write_or_elect_attribute(a);
    return FALSE;
}

static void
attrd_cib_callback(xmlNode *msg, int call_id, int rc, xmlNode *output, void *user_data)
{
    int level = LOG_ERR;
    GHashTableIter iter;
    const char *peer = NULL;
    attribute_value_t *v = NULL;

    char *name = user_data;
    attribute_t *a = g_hash_table_lookup(attributes, name);

    if(a == NULL) {
        crm_info("Attribute %s no longer exists", name);
        return;
    }

    a->update = 0;
    if (rc == pcmk_ok && call_id < 0) {
        rc = call_id;
    }

    switch (rc) {
        case pcmk_ok:
            level = LOG_INFO;
            last_cib_op_done = call_id;
            attrd_send_attribute_alerts_all(a);

            if (a->timer && !a->timeout_ms) {
                // Remove temporary dampening for failed writes
                mainloop_timer_del(a->timer);
                a->timer = NULL;
            }
            break;

        case -pcmk_err_diff_failed:    /* When an attr changes while the CIB is syncing */
        case -ETIME:           /* When an attr changes while there is a DC election */
        case -ENXIO:           /* When an attr changes while the CIB is syncing a
                                *   newer config from a node that just came up
                                */
            level = LOG_WARNING;
            break;
    }

    do_crm_log(level, "CIB update %d result for %s: %s " CRM_XS " rc=%d",
               call_id, a->id, pcmk_strerror(rc), rc);

    g_hash_table_iter_init(&iter, a->values);
    while (g_hash_table_iter_next(&iter, (gpointer *) & peer, (gpointer *) & v)) {
        do_crm_log(level, "* %s[%s]=%s", a->id, peer, v->requested);
        free(v->requested);
        v->requested = NULL;
        if (rc != pcmk_ok) {
            a->changed = true; /* Attempt write out again */
        }
    }

    if (a->changed && attrd_election_won()) {
        if (rc == pcmk_ok) {
            /* We deferred a write of a new update because this update was in
             * progress. Write out the new value without additional delay.
             */
            attrd_write_attribute(a, false);

        /* We're re-attempting a write because the original failed; delay
         * the next attempt so we don't potentially flood the CIB manager
         * and logs with a zillion attempts per second.
         *
         * @TODO We could elect a new writer instead. However, we'd have to
         * somehow downgrade our vote, and we'd still need something like this
         * if all peers similarly fail to write this attribute (which may
         * indicate a corrupted attribute entry rather than a CIB issue).
         */
        } else if (a->timer) {
            // Attribute has a dampening value, so use that as delay
            if (!mainloop_timer_running(a->timer)) {
                crm_trace("Delayed re-attempted write for %s by %s",
                          name, pcmk__readable_interval(a->timeout_ms));
                mainloop_timer_start(a->timer);
            }
        } else {
            /* Set a temporary dampening of 2 seconds (timer will continue
             * to exist until the attribute's dampening gets set or the
             * write succeeds).
             */
            a->timer = attrd_add_timer(a->id, 2000, a);
            mainloop_timer_start(a->timer);
        }
    }
}

#define XPATH_ATTR "/" XML_CIB_TAG_STATUS                           \
                   "/" XML_CIB_TAG_STATE "[@" XML_ATTR_ID "='%s']"  \
                   "/" XML_TAG_TRANSIENT_NODEATTRS                  \
                       "[@" XML_ATTR_ID "='%s']"                    \
                   "/%s[@" XML_ATTR_ID "='%s']"                     \
                   "/" XML_CIB_TAG_NVPAIR                           \
                       "[@" XML_ATTR_ID "='%s' "                    \
                       "and @" XML_NVPAIR_ATTR_NAME "='%s']"

/*!
 * \internal
 * \brief Add an attribute update request to the current CIB transaction
 *
 * \param[in] attr      Attribute to update
 * \param[in] set_type  Type of attribute set block (\c XML_TAG_ATTR_SETS or
 *                      \c XML_TAG_UTILIZATION)
 * \param[in] nodeid    ID of node for which to update attribute value
 * \param[in] value     New value for attribute
 *
 * \return Legacy Pacemaker return code
 */
static int
add_attr_update(const attribute_t *attr, const char *set_type,
                const char *nodeid, const char *value)
{
    char *set_id = NULL;
    char *attr_id = NULL;
    int rc = pcmk_ok;

    if (attr->set_id != NULL) {
        pcmk__str_update(&set_id, attr->set_id);
    } else {
        set_id = crm_strdup_printf("%s-%s", XML_CIB_TAG_STATUS, nodeid);
    }
    crm_xml_sanitize_id(set_id);

    if (attr->uuid != NULL) {
        pcmk__str_update(&attr_id, attr->uuid);
    } else {
        attr_id = crm_strdup_printf("%s-%s", set_id, attr->id);
    }
    crm_xml_sanitize_id(attr_id);

    if (value != NULL) {
        xmlNode *update = create_xml_node(NULL, XML_CIB_TAG_STATE);
        xmlNode *child = update;

        rc = -ENOMEM;   // For any failed create_xml_node() call
        if (child == NULL) {
            goto done;
        }
        crm_xml_add(child, XML_ATTR_ID, nodeid);

        child = create_xml_node(child, XML_TAG_TRANSIENT_NODEATTRS);
        if (child == NULL) {
            goto done;
        }
        crm_xml_add(child, XML_ATTR_ID, nodeid);

        child = create_xml_node(child, set_type);
        if (child == NULL) {
            goto done;
        }
        crm_xml_add(child, XML_ATTR_ID, set_id);

        child = create_xml_node(child, XML_CIB_TAG_NVPAIR);
        if (child == NULL) {
            goto done;
        }
        crm_xml_add(child, XML_ATTR_ID, attr_id);
        crm_xml_add(child, XML_NVPAIR_ATTR_NAME, attr->id);
        crm_xml_add(child, XML_NVPAIR_ATTR_VALUE, value);

        rc = cib_internal_op(the_cib, PCMK__CIB_REQUEST_MODIFY, NULL,
                             XML_CIB_TAG_STATUS, update, NULL,
                             cib_can_create|cib_transaction, attr->user);
        free_xml(update);

    } else {
        char *xpath = crm_strdup_printf(XPATH_ATTR,
                                        nodeid, nodeid, set_type, set_id,
                                        attr_id, attr->id);

        rc = cib_internal_op(the_cib, PCMK__CIB_REQUEST_DELETE, NULL, xpath,
                             NULL, NULL, cib_xpath|cib_transaction, attr->user);
        free(xpath);
    }

done:
    free(set_id);
    free(attr_id);
    return rc;
}

mainloop_timer_t *
attrd_add_timer(const char *id, int timeout_ms, attribute_t *attr)
{
   return mainloop_timer_add(id, timeout_ms, FALSE, attribute_timer_cb, attr);
}

void
attrd_write_attribute(attribute_t *a, bool ignore_delay)
{
    int private_updates = 0, cib_updates = 0;
    const char *set_type = NULL;
    attribute_value_t *v = NULL;
    GHashTableIter iter;
    int rc = pcmk_ok;

    if (a == NULL) {
        return;
    }

    /* If this attribute will be written to the CIB ... */
    if (!stand_alone && !a->is_private) {

        /* Defer the write if now's not a good time */
        CRM_CHECK(the_cib != NULL, return);
        if (a->update && (a->update < last_cib_op_done)) {
            crm_info("Write out of '%s' continuing: update %d considered lost", a->id, a->update);
            a->update = 0; // Don't log this message again

        } else if (a->update) {
            crm_info("Write out of '%s' delayed: update %d in progress", a->id, a->update);
            return;

        } else if (mainloop_timer_running(a->timer)) {
            if (ignore_delay) {
                /* 'refresh' forces a write of the current value of all attributes
                 * Cancel any existing timers, we're writing it NOW
                 */
                mainloop_timer_stop(a->timer);
                crm_debug("Write out of '%s': timer is running but ignore delay", a->id);
            } else {
                crm_info("Write out of '%s' delayed: timer is running", a->id);
                return;
            }
        }

        // Initiate a transaction for all the peer value updates
        rc = the_cib->cmds->init_transaction(the_cib);
        if (rc != pcmk_ok) {
            crm_err("Failed to write %s (id %s, set %s): Could not initiate "
                    "CIB transaction",
                    a->id, pcmk__s(a->uuid, "n/a"), pcmk__s(a->set_id, "n/a"));
            return;
        }

        if (pcmk__str_eq(a->set_type, XML_TAG_ATTR_SETS,
                         pcmk__str_null_matches)) {
            set_type = XML_TAG_ATTR_SETS;

        } else if (pcmk__str_eq(a->set_type, XML_TAG_UTILIZATION,
                                pcmk__str_none)) {
            set_type = XML_TAG_UTILIZATION;

        } else {
            crm_err("Failed to write %s (id %s, set %s): Unknown set type %s",
                    a->id, pcmk__s(a->uuid, "n/a"), pcmk__s(a->set_id, "n/a"),
                    a->set_type);
            goto done;
        }
    }

    /* Attribute will be written shortly, so clear changed flag */
    a->changed = false;

    /* We will check all peers' uuids shortly, so initialize this to false */
    a->unknown_peer_uuids = false;

    /* Attribute will be written shortly, so clear forced write flag */
    a->force_write = FALSE;

    /* Iterate over each peer value of this attribute */
    g_hash_table_iter_init(&iter, a->values);
    while (g_hash_table_iter_next(&iter, NULL, (gpointer *) & v)) {
        crm_node_t *peer = crm_get_peer_full(v->nodeid, v->nodename, CRM_GET_PEER_ANY);

        /* If the value's peer info does not correspond to a peer, ignore it */
        if (peer == NULL) {
            crm_notice("Cannot update %s[%s]=%s because peer not known",
                       a->id, v->nodename, v->current);
            continue;
        }

        /* If we're just learning the peer's node id, remember it */
        if (peer->id && (v->nodeid == 0)) {
            crm_trace("Learned ID %u for node %s", peer->id, v->nodename);
            v->nodeid = peer->id;
        }

        /* If this is a private attribute, no update needs to be sent */
        if (stand_alone || a->is_private) {
            private_updates++;
            continue;
        }

        /* If the peer is found, but its uuid is unknown, defer write */
        if (peer->uuid == NULL) {
            a->unknown_peer_uuids = true;
            crm_notice("Cannot update %s[%s]=%s because peer UUID not known "
                       "(will retry if learned)",
                       a->id, v->nodename, v->current);
            continue;
        }

        // Update this value as part of the CIB transaction we're building
        rc = add_attr_update(a, set_type, peer->uuid, v->current);
        if (rc != pcmk_ok) {
            crm_err("Failed to update %s[%s]=%s (peer known as %s, UUID %s, "
                    "ID %" PRIu32 "/%" PRIu32 "): %s",
                    a->id, v->nodename, v->current, peer->uname, peer->uuid,
                    peer->id, v->nodeid, pcmk_strerror(rc));
            continue;
        }

        crm_debug("Updating %s[%s]=%s (peer known as %s, UUID %s, ID "
                  "%" PRIu32 "/%" PRIu32 ")",
                  a->id, v->nodename, v->current,
                  peer->uname, peer->uuid, peer->id, v->nodeid);
        cib_updates++;

        // Save the attribute value for use when sending alerts
        attrd_record_alert_attribute_value(v);

        pcmk__str_update(&(v->requested), v->current);
    }

    if (private_updates) {
        crm_info("Processed %d private change%s for %s, id=%s, set=%s",
                 private_updates, pcmk__plural_s(private_updates),
                 a->id, pcmk__s(a->uuid, "n/a"), pcmk__s(a->set_id, "n/a"));
    }
    if (cib_updates > 0) {
        char *id = NULL;

        // Call cib_internal_op() directly to pass a->user
        a->update = cib_internal_op(the_cib, PCMK__CIB_REQUEST_COMMIT_TRANSACT,
                                    NULL, NULL, the_cib->transaction, NULL,
                                    cib_none, a->user);

        crm_info("Sent CIB request %d with %d change%s for %s (id %s, set %s)",
                 a->update, cib_updates, pcmk__plural_s(cib_updates),
                 a->id, pcmk__s(a->uuid, "n/a"), pcmk__s(a->set_id, "n/a"));

        pcmk__str_update(&id, a->id);
        the_cib->cmds->register_callback_full(the_cib, a->update,
                                              CIB_OP_TIMEOUT_S, FALSE, id,
                                              "attrd_cib_callback",
                                              attrd_cib_callback, free);
    }

done:
    the_cib->cmds->end_transaction(the_cib, false, cib_none);
}

/*!
 * \internal
 * \brief Write out attributes
 *
 * \param[in] options  Group of enum attrd_write_options
 */
void
attrd_write_attributes(uint32_t options)
{
    GHashTableIter iter;
    attribute_t *a = NULL;

    crm_debug("Writing out %s attributes",
              pcmk_is_set(options, attrd_write_all)? "all" : "changed");
    g_hash_table_iter_init(&iter, attributes);
    while (g_hash_table_iter_next(&iter, NULL, (gpointer *) & a)) {
        if (pcmk_is_set(options, attrd_write_skip_shutdown)
            && pcmk__str_eq(a->id, XML_CIB_ATTR_SHUTDOWN, pcmk__str_none)) {
            continue;
        }

        if (!pcmk_is_set(options, attrd_write_all) && a->unknown_peer_uuids) {
            // Try writing this attribute again, in case peer ID was learned
            a->changed = true;
        } else if (a->force_write) {
            /* If the force_write flag is set, write the attribute. */
            a->changed = true;
        }

        if (pcmk_is_set(options, attrd_write_all) || a->changed) {
            bool ignore_delay = pcmk_is_set(options, attrd_write_no_delay);

            if (a->force_write) {
                // Always ignore delay when forced write flag is set
                ignore_delay = true;
            }
            attrd_write_attribute(a, ignore_delay);
        } else {
            crm_trace("Skipping unchanged attribute %s", a->id);
        }
    }
}

void
attrd_write_or_elect_attribute(attribute_t *a)
{
    if (attrd_election_won()) {
        attrd_write_attribute(a, false);
    } else {
        attrd_start_election_if_needed();
    }
}
