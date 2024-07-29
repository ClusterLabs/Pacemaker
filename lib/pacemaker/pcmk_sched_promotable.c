/*
 * Copyright 2004-2024 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU General Public License version 2
 * or later (GPLv2+) WITHOUT ANY WARRANTY.
 */

#include <crm_internal.h>

#include <crm/common/xml.h>
#include <pacemaker-internal.h>

#include "libpacemaker_private.h"

/*!
 * \internal
 * \brief Add implicit promotion ordering for a promotable instance
 *
 * \param[in,out] clone  Clone resource
 * \param[in,out] child  Instance of \p clone being ordered
 * \param[in,out] last   Previous instance ordered (NULL if \p child is first)
 */
static void
order_instance_promotion(pcmk_resource_t *clone, pcmk_resource_t *child,
                         pcmk_resource_t *last)
{
    // "Promote clone" -> promote instance -> "clone promoted"
    pcmk__order_resource_actions(clone, PCMK_ACTION_PROMOTE,
                                 child, PCMK_ACTION_PROMOTE,
                                 pcmk__ar_ordered);
    pcmk__order_resource_actions(child, PCMK_ACTION_PROMOTE,
                                 clone, PCMK_ACTION_PROMOTED,
                                 pcmk__ar_ordered);

    // If clone is ordered, order this instance relative to last
    if ((last != NULL) && pe__clone_is_ordered(clone)) {
        pcmk__order_resource_actions(last, PCMK_ACTION_PROMOTE,
                                     child, PCMK_ACTION_PROMOTE,
                                     pcmk__ar_ordered);
    }
}

/*!
 * \internal
 * \brief Add implicit demotion ordering for a promotable instance
 *
 * \param[in,out] clone  Clone resource
 * \param[in,out] child  Instance of \p clone being ordered
 * \param[in]     last   Previous instance ordered (NULL if \p child is first)
 */
static void
order_instance_demotion(pcmk_resource_t *clone, pcmk_resource_t *child,
                        pcmk_resource_t *last)
{
    // "Demote clone" -> demote instance -> "clone demoted"
    pcmk__order_resource_actions(clone, PCMK_ACTION_DEMOTE, child,
                                 PCMK_ACTION_DEMOTE,
                                 pcmk__ar_then_implies_first_graphed);
    pcmk__order_resource_actions(child, PCMK_ACTION_DEMOTE,
                                 clone, PCMK_ACTION_DEMOTED,
                                 pcmk__ar_first_implies_then_graphed);

    // If clone is ordered, order this instance relative to last
    if ((last != NULL) && pe__clone_is_ordered(clone)) {
        pcmk__order_resource_actions(child, PCMK_ACTION_DEMOTE, last,
                                     PCMK_ACTION_DEMOTE, pcmk__ar_ordered);
    }
}

/*!
 * \internal
 * \brief Check whether an instance will be promoted or demoted
 *
 * \param[in]  rsc        Instance to check
 * \param[out] demoting   If \p rsc will be demoted, this will be set to true
 * \param[out] promoting  If \p rsc will be promoted, this will be set to true
 */
static void
check_for_role_change(const pcmk_resource_t *rsc, bool *demoting,
                      bool *promoting)
{
    const GList *iter = NULL;

    // If this is a cloned group, check group members recursively
    if (rsc->children != NULL) {
        for (iter = rsc->children; iter != NULL; iter = iter->next) {
            check_for_role_change((const pcmk_resource_t *) iter->data,
                                  demoting, promoting);
        }
        return;
    }

    for (iter = rsc->actions; iter != NULL; iter = iter->next) {
        const pcmk_action_t *action = (const pcmk_action_t *) iter->data;

        if (*promoting && *demoting) {
            return;

        } else if (pcmk_is_set(action->flags, pcmk_action_optional)) {
            continue;

        } else if (pcmk__str_eq(PCMK_ACTION_DEMOTE, action->task,
                                pcmk__str_none)) {
            *demoting = true;

        } else if (pcmk__str_eq(PCMK_ACTION_PROMOTE, action->task,
                                pcmk__str_none)) {
            *promoting = true;
        }
    }
}

/*!
 * \internal
 * \brief Add promoted-role location constraint scores to an instance's priority
 *
 * Adjust a promotable clone instance's promotion priority by the scores of any
 * location constraints in a list that are both limited to the promoted role and
 * for the node where the instance will be placed.
 *
 * \param[in,out] child                 Promotable clone instance
 * \param[in]     location_constraints  List of location constraints to apply
 * \param[in]     chosen                Node where \p child will be placed
 */
static void
apply_promoted_locations(pcmk_resource_t *child,
                         const GList *location_constraints,
                         const pcmk_node_t *chosen)
{
    for (const GList *iter = location_constraints; iter; iter = iter->next) {
        const pcmk__location_t *location = iter->data;
        const pcmk_node_t *constraint_node = NULL;

        if (location->role_filter == pcmk_role_promoted) {
            constraint_node = pe_find_node_id(location->nodes,
                                              chosen->details->id);
        }
        if (constraint_node != NULL) {
            int new_priority = pcmk__add_scores(child->priority,
                                                constraint_node->weight);

            pcmk__rsc_trace(child,
                            "Applying location %s to %s promotion priority on "
                            "%s: %s + %s = %s",
                            location->id, child->id,
                            pcmk__node_name(constraint_node),
                            pcmk_readable_score(child->priority),
                            pcmk_readable_score(constraint_node->weight),
                            pcmk_readable_score(new_priority));
            child->priority = new_priority;
        }
    }
}

/*!
 * \internal
 * \brief Get the node that an instance will be promoted on
 *
 * \param[in] rsc  Promotable clone instance to check
 *
 * \return Node that \p rsc will be promoted on, or NULL if none
 */
static pcmk_node_t *
node_to_be_promoted_on(const pcmk_resource_t *rsc)
{
    pcmk_node_t *node = NULL;
    pcmk_node_t *local_node = NULL;
    const pcmk_resource_t *parent = NULL;

    // If this is a cloned group, bail if any group member can't be promoted
    for (GList *iter = rsc->children; iter != NULL; iter = iter->next) {
        pcmk_resource_t *child = (pcmk_resource_t *) iter->data;

        if (node_to_be_promoted_on(child) == NULL) {
            pcmk__rsc_trace(rsc,
                            "%s can't be promoted because member %s can't",
                            rsc->id, child->id);
            return NULL;
        }
    }

    node = rsc->fns->location(rsc, NULL, FALSE);
    if (node == NULL) {
        pcmk__rsc_trace(rsc, "%s can't be promoted because it won't be active",
                        rsc->id);
        return NULL;

    } else if (!pcmk_is_set(rsc->flags, pcmk_rsc_managed)) {
        if (rsc->fns->state(rsc, TRUE) == pcmk_role_promoted) {
            crm_notice("Unmanaged instance %s will be left promoted on %s",
                       rsc->id, pcmk__node_name(node));
        } else {
            pcmk__rsc_trace(rsc, "%s can't be promoted because it is unmanaged",
                            rsc->id);
            return NULL;
        }

    } else if (rsc->priority < 0) {
        pcmk__rsc_trace(rsc,
                        "%s can't be promoted because its promotion priority "
                        "%d is negative",
                        rsc->id, rsc->priority);
        return NULL;

    } else if (!pcmk__node_available(node, false, true)) {
        pcmk__rsc_trace(rsc,
                        "%s can't be promoted because %s can't run resources",
                        rsc->id, pcmk__node_name(node));
        return NULL;
    }

    parent = pe__const_top_resource(rsc, false);
    local_node = g_hash_table_lookup(parent->allowed_nodes, node->details->id);

    if (local_node == NULL) {
        /* It should not be possible for the scheduler to have assigned the
         * instance to a node where its parent is not allowed, but it's good to
         * have a fail-safe.
         */
        if (pcmk_is_set(rsc->flags, pcmk_rsc_managed)) {
            pcmk__sched_err("%s can't be promoted because %s is not allowed "
                            "on %s (scheduler bug?)",
                            rsc->id, parent->id, pcmk__node_name(node));
        } // else the instance is unmanaged and already promoted
        return NULL;

    } else if ((local_node->count >= pe__clone_promoted_node_max(parent))
               && pcmk_is_set(rsc->flags, pcmk_rsc_managed)) {
        pcmk__rsc_trace(rsc,
                        "%s can't be promoted because %s has "
                        "maximum promoted instances already",
                        rsc->id, pcmk__node_name(node));
        return NULL;
    }

    return local_node;
}

/*!
 * \internal
 * \brief Compare two promotable clone instances by promotion priority
 *
 * \param[in] a  First instance to compare
 * \param[in] b  Second instance to compare
 *
 * \return A negative number if \p a has higher promotion priority,
 *         a positive number if \p b has higher promotion priority,
 *         or 0 if promotion priorities are equal
 */
static gint
cmp_promotable_instance(gconstpointer a, gconstpointer b)
{
    const pcmk_resource_t *rsc1 = (const pcmk_resource_t *) a;
    const pcmk_resource_t *rsc2 = (const pcmk_resource_t *) b;

    enum rsc_role_e role1 = pcmk_role_unknown;
    enum rsc_role_e role2 = pcmk_role_unknown;

    CRM_ASSERT((rsc1 != NULL) && (rsc2 != NULL));

    // Check sort index set by pcmk__set_instance_roles()
    if (rsc1->sort_index > rsc2->sort_index) {
        pcmk__rsc_trace(rsc1,
                        "%s has higher promotion priority than %s "
                        "(sort index %d > %d)",
                        rsc1->id, rsc2->id, rsc1->sort_index, rsc2->sort_index);
        return -1;
    } else if (rsc1->sort_index < rsc2->sort_index) {
        pcmk__rsc_trace(rsc1,
                        "%s has lower promotion priority than %s "
                        "(sort index %d < %d)",
                        rsc1->id, rsc2->id, rsc1->sort_index, rsc2->sort_index);
        return 1;
    }

    // If those are the same, prefer instance whose current role is higher
    role1 = rsc1->fns->state(rsc1, TRUE);
    role2 = rsc2->fns->state(rsc2, TRUE);
    if (role1 > role2) {
        pcmk__rsc_trace(rsc1,
                        "%s has higher promotion priority than %s "
                        "(higher current role)",
                        rsc1->id, rsc2->id);
        return -1;
    } else if (role1 < role2) {
        pcmk__rsc_trace(rsc1,
                        "%s has lower promotion priority than %s "
                        "(lower current role)",
                        rsc1->id, rsc2->id);
        return 1;
    }

    // Finally, do normal clone instance sorting
    return pcmk__cmp_instance(a, b);
}

/*!
 * \internal
 * \brief Add a promotable clone instance's sort index to its node's score
 *
 * Add a promotable clone instance's sort index (which sums its promotion
 * preferences and scores of relevant location constraints for the promoted
 * role) to the node score of the instance's assigned node.
 *
 * \param[in]     data       Promotable clone instance
 * \param[in,out] user_data  Clone parent of \p data
 */
static void
add_sort_index_to_node_score(gpointer data, gpointer user_data)
{
    const pcmk_resource_t *child = (const pcmk_resource_t *) data;
    pcmk_resource_t *clone = (pcmk_resource_t *) user_data;

    pcmk_node_t *node = NULL;
    const pcmk_node_t *chosen = NULL;

    if (child->sort_index < 0) {
        pcmk__rsc_trace(clone, "Not adding sort index of %s: negative",
                        child->id);
        return;
    }

    chosen = child->fns->location(child, NULL, FALSE);
    if (chosen == NULL) {
        pcmk__rsc_trace(clone, "Not adding sort index of %s: inactive",
                        child->id);
        return;
    }

    node = g_hash_table_lookup(clone->allowed_nodes, chosen->details->id);
    CRM_ASSERT(node != NULL);

    node->weight = pcmk__add_scores(child->sort_index, node->weight);
    pcmk__rsc_trace(clone,
                    "Added cumulative priority of %s (%s) to score on %s "
                    "(now %s)",
                    child->id, pcmk_readable_score(child->sort_index),
                    pcmk__node_name(node), pcmk_readable_score(node->weight));
}

/*!
 * \internal
 * \brief Apply colocation to primary's node scores if for promoted role
 *
 * \param[in,out] data       Colocation constraint to apply
 * \param[in,out] user_data  Promotable clone that is constraint's primary
 */
static void
apply_coloc_to_primary(gpointer data, gpointer user_data)
{
    pcmk__colocation_t *colocation = data;
    pcmk_resource_t *clone = user_data;
    pcmk_resource_t *dependent = colocation->dependent;
    const float factor = colocation->score / (float) PCMK_SCORE_INFINITY;
    const uint32_t flags = pcmk__coloc_select_active
                           |pcmk__coloc_select_nonnegative;

    if ((colocation->primary_role != pcmk_role_promoted)
         || !pcmk__colocation_has_influence(colocation, NULL)) {
        return;
    }

    pcmk__rsc_trace(clone, "Applying colocation %s (%s with promoted %s) @%s",
                    colocation->id, colocation->dependent->id,
                    colocation->primary->id,
                    pcmk_readable_score(colocation->score));
    dependent->cmds->add_colocated_node_scores(dependent, clone, clone->id,
                                               &clone->allowed_nodes,
                                               colocation, factor, flags);
}

/*!
 * \internal
 * \brief Set clone instance's sort index to its node's score
 *
 * \param[in,out] data       Promotable clone instance
 * \param[in]     user_data  Parent clone of \p data
 */
static void
set_sort_index_to_node_score(gpointer data, gpointer user_data)
{
    pcmk_resource_t *child = (pcmk_resource_t *) data;
    const pcmk_resource_t *clone = (const pcmk_resource_t *) user_data;

    pcmk_node_t *chosen = child->fns->location(child, NULL, FALSE);

    if (!pcmk_is_set(child->flags, pcmk_rsc_managed)
        && (child->next_role == pcmk_role_promoted)) {
        child->sort_index = PCMK_SCORE_INFINITY;
        pcmk__rsc_trace(clone,
                        "Final sort index for %s is INFINITY "
                        "(unmanaged promoted)",
                        child->id);

    } else if (chosen == NULL) {
        child->sort_index = -PCMK_SCORE_INFINITY;
        pcmk__rsc_trace(clone,
                        "Final promotion priority for %s is %s "
                        "(will not be active)",
                        child->id, pcmk_readable_score(-PCMK_SCORE_INFINITY));

    } else if (child->sort_index < 0) {
        pcmk__rsc_trace(clone,
                        "Final sort index for %s is %d (ignoring node score)",
                        child->id, child->sort_index);

    } else {
        const pcmk_node_t *node = g_hash_table_lookup(clone->allowed_nodes,
                                                      chosen->details->id);

        CRM_ASSERT(node != NULL);
        child->sort_index = node->weight;
        pcmk__rsc_trace(clone,
                        "Adding scores for %s: final sort index for %s is %d",
                        clone->id, child->id, child->sort_index);
    }
}

/*!
 * \internal
 * \brief Sort a promotable clone's instances by descending promotion priority
 *
 * \param[in,out] clone  Promotable clone to sort
 */
static void
sort_promotable_instances(pcmk_resource_t *clone)
{
    GList *colocations = NULL;

    if (pe__set_clone_flag(clone, pcmk__clone_promotion_constrained)
            == pcmk_rc_already) {
        return;
    }
    pcmk__set_rsc_flags(clone, pcmk_rsc_updating_nodes);

    for (GList *iter = clone->children; iter != NULL; iter = iter->next) {
        pcmk_resource_t *child = (pcmk_resource_t *) iter->data;

        pcmk__rsc_trace(clone,
                        "Adding scores for %s: initial sort index for %s is %d",
                        clone->id, child->id, child->sort_index);
    }
    pe__show_node_scores(true, clone, "Before", clone->allowed_nodes,
                         clone->cluster);

    g_list_foreach(clone->children, add_sort_index_to_node_score, clone);

    // "this with" colocations were already applied via set_instance_priority()
    colocations = pcmk__with_this_colocations(clone);
    g_list_foreach(colocations, apply_coloc_to_primary, clone);
    g_list_free(colocations);

    // Ban resource from all nodes if it needs a ticket but doesn't have it
    pcmk__require_promotion_tickets(clone);

    pe__show_node_scores(true, clone, "After", clone->allowed_nodes,
                         clone->cluster);

    // Reset sort indexes to final node scores
    g_list_foreach(clone->children, set_sort_index_to_node_score, clone);

    // Finally, sort instances in descending order of promotion priority
    clone->children = g_list_sort(clone->children, cmp_promotable_instance);
    pcmk__clear_rsc_flags(clone, pcmk_rsc_updating_nodes);
}

/*!
 * \internal
 * \brief Find the active instance (if any) of an anonymous clone on a node
 *
 * \param[in] clone  Anonymous clone to check
 * \param[in] id     Instance ID (without instance number) to check
 * \param[in] node   Node to check
 *
 * \return
 */
static pcmk_resource_t *
find_active_anon_instance(const pcmk_resource_t *clone, const char *id,
                          const pcmk_node_t *node)
{
    for (GList *iter = clone->children; iter; iter = iter->next) {
        pcmk_resource_t *child = iter->data;
        pcmk_resource_t *active = NULL;

        // Use ->find_rsc() in case this is a cloned group
        active = clone->fns->find_rsc(child, id, node,
                                      pcmk_rsc_match_clone_only
                                      |pcmk_rsc_match_current_node);
        if (active != NULL) {
            return active;
        }
    }
    return NULL;
}

/*
 * \brief Check whether an anonymous clone instance is known on a node
 *
 * \param[in] clone  Anonymous clone to check
 * \param[in] id     Instance ID (without instance number) to check
 * \param[in] node   Node to check
 *
 * \return true if \p id instance of \p clone is known on \p node,
 *         otherwise false
 */
static bool
anonymous_known_on(const pcmk_resource_t *clone, const char *id,
                   const pcmk_node_t *node)
{
    for (GList *iter = clone->children; iter; iter = iter->next) {
        pcmk_resource_t *child = iter->data;

        /* Use ->find_rsc() because this might be a cloned group, and knowing
         * that other members of the group are known here implies nothing.
         */
        child = clone->fns->find_rsc(child, id, NULL,
                                     pcmk_rsc_match_clone_only);
        CRM_LOG_ASSERT(child != NULL);
        if (child != NULL) {
            if (g_hash_table_lookup(child->known_on, node->details->id)) {
                return true;
            }
        }
    }
    return false;
}

/*!
 * \internal
 * \brief Check whether a node is allowed to run a resource
 *
 * \param[in] rsc   Resource to check
 * \param[in] node  Node to check
 *
 * \return true if \p node is allowed to run \p rsc, otherwise false
 */
static bool
is_allowed(const pcmk_resource_t *rsc, const pcmk_node_t *node)
{
    pcmk_node_t *allowed = g_hash_table_lookup(rsc->allowed_nodes,
                                               node->details->id);

    return (allowed != NULL) && (allowed->weight >= 0);
}

/*!
 * \brief Check whether a clone instance's promotion score should be considered
 *
 * \param[in] rsc   Promotable clone instance to check
 * \param[in] node  Node where score would be applied
 *
 * \return true if \p rsc's promotion score should be considered on \p node,
 *         otherwise false
 */
static bool
promotion_score_applies(const pcmk_resource_t *rsc, const pcmk_node_t *node)
{
    char *id = clone_strip(rsc->id);
    const pcmk_resource_t *parent = pe__const_top_resource(rsc, false);
    pcmk_resource_t *active = NULL;
    const char *reason = "allowed";

    // Some checks apply only to anonymous clone instances
    if (!pcmk_is_set(rsc->flags, pcmk_rsc_unique)) {

        // If instance is active on the node, its score definitely applies
        active = find_active_anon_instance(parent, id, node);
        if (active == rsc) {
            reason = "active";
            goto check_allowed;
        }

        /* If *no* instance is active on this node, this instance's score will
         * count if it has been probed on this node.
         */
        if ((active == NULL) && anonymous_known_on(parent, id, node)) {
            reason = "probed";
            goto check_allowed;
        }
    }

    /* If this clone's status is unknown on *all* nodes (e.g. cluster startup),
     * take all instances' scores into account, to make sure we use any
     * permanent promotion scores.
     */
    if ((rsc->running_on == NULL) && (g_hash_table_size(rsc->known_on) == 0)) {
        reason = "none probed";
        goto check_allowed;
    }

    /* Otherwise, we've probed and/or started the resource *somewhere*, so
     * consider promotion scores on nodes where we know the status.
     */
    if ((g_hash_table_lookup(rsc->known_on, node->details->id) != NULL)
        || (pe_find_node_id(rsc->running_on, node->details->id) != NULL)) {
        reason = "known";
    } else {
        pcmk__rsc_trace(rsc,
                        "Ignoring %s promotion score (for %s) on %s: "
                        "not probed",
                        rsc->id, id, pcmk__node_name(node));
        free(id);
        return false;
    }

check_allowed:
    if (is_allowed(rsc, node)) {
        pcmk__rsc_trace(rsc, "Counting %s promotion score (for %s) on %s: %s",
                        rsc->id, id, pcmk__node_name(node), reason);
        free(id);
        return true;
    }

    pcmk__rsc_trace(rsc,
                    "Ignoring %s promotion score (for %s) on %s: not allowed",
                    rsc->id, id, pcmk__node_name(node));
    free(id);
    return false;
}

/*!
 * \internal
 * \brief Get the value of a promotion score node attribute
 *
 * \param[in] rsc   Promotable clone instance to get promotion score for
 * \param[in] node  Node to get promotion score for
 * \param[in] name  Resource name to use in promotion score attribute name
 *
 * \return Value of promotion score node attribute for \p rsc on \p node
 */
static const char *
promotion_attr_value(const pcmk_resource_t *rsc, const pcmk_node_t *node,
                     const char *name)
{
    char *attr_name = NULL;
    const char *attr_value = NULL;
    const char *target = NULL;
    enum pcmk__rsc_node node_type = pcmk__rsc_node_assigned;

    if (pcmk_is_set(rsc->flags, pcmk_rsc_unassigned)) {
        // Not assigned yet
        node_type = pcmk__rsc_node_current;
    }
    target = g_hash_table_lookup(rsc->meta,
                                 PCMK_META_CONTAINER_ATTRIBUTE_TARGET);
    attr_name = pcmk_promotion_score_name(name);
    attr_value = pcmk__node_attr(node, attr_name, target, node_type);
    free(attr_name);
    return attr_value;
}

/*!
 * \internal
 * \brief Get the promotion score for a clone instance on a node
 *
 * \param[in]  rsc         Promotable clone instance to get score for
 * \param[in]  node        Node to get score for
 * \param[out] is_default  If non-NULL, will be set true if no score available
 *
 * \return Promotion score for \p rsc on \p node (or 0 if none)
 */
static int
promotion_score(const pcmk_resource_t *rsc, const pcmk_node_t *node,
                bool *is_default)
{
    char *name = NULL;
    const char *attr_value = NULL;

    if (is_default != NULL) {
        *is_default = true;
    }

    CRM_CHECK((rsc != NULL) && (node != NULL), return 0);

    /* If this is an instance of a cloned group, the promotion score is the sum
     * of all members' promotion scores.
     */
    if (rsc->children != NULL) {
        int score = 0;

        for (const GList *iter = rsc->children;
             iter != NULL; iter = iter->next) {

            const pcmk_resource_t *child = (const pcmk_resource_t *) iter->data;
            bool child_default = false;
            int child_score = promotion_score(child, node, &child_default);

            if (!child_default && (is_default != NULL)) {
                *is_default = false;
            }
            score += child_score;
        }
        return score;
    }

    if (!promotion_score_applies(rsc, node)) {
        return 0;
    }

    /* For the promotion score attribute name, use the name the resource is
     * known as in resource history, since that's what crm_attribute --promotion
     * would have used.
     */
    name = (rsc->clone_name == NULL)? rsc->id : rsc->clone_name;

    attr_value = promotion_attr_value(rsc, node, name);
    if (attr_value != NULL) {
        pcmk__rsc_trace(rsc, "Promotion score for %s on %s = %s",
                        name, pcmk__node_name(node),
                        pcmk__s(attr_value, "(unset)"));
    } else if (!pcmk_is_set(rsc->flags, pcmk_rsc_unique)) {
        /* If we don't have any resource history yet, we won't have clone_name.
         * In that case, for anonymous clones, try the resource name without
         * any instance number.
         */
        name = clone_strip(rsc->id);
        if (strcmp(rsc->id, name) != 0) {
            attr_value = promotion_attr_value(rsc, node, name);
            pcmk__rsc_trace(rsc, "Promotion score for %s on %s (for %s) = %s",
                            name, pcmk__node_name(node), rsc->id,
                            pcmk__s(attr_value, "(unset)"));
        }
        free(name);
    }

    if (attr_value == NULL) {
        return 0;
    }

    if (is_default != NULL) {
        *is_default = false;
    }
    return char2score(attr_value);
}

/*!
 * \internal
 * \brief Include promotion scores in instances' node scores and priorities
 *
 * \param[in,out] rsc  Promotable clone resource to update
 */
void
pcmk__add_promotion_scores(pcmk_resource_t *rsc)
{
    if (pe__set_clone_flag(rsc,
                           pcmk__clone_promotion_added) == pcmk_rc_already) {
        return;
    }

    for (GList *iter = rsc->children; iter != NULL; iter = iter->next) {
        pcmk_resource_t *child_rsc = (pcmk_resource_t *) iter->data;

        GHashTableIter iter;
        pcmk_node_t *node = NULL;
        int score, new_score;

        g_hash_table_iter_init(&iter, child_rsc->allowed_nodes);
        while (g_hash_table_iter_next(&iter, NULL, (void **) &node)) {
            if (!pcmk__node_available(node, false, false)) {
                /* This node will never be promoted, so don't apply the
                 * promotion score, as that may lead to clone shuffling.
                 */
                continue;
            }

            score = promotion_score(child_rsc, node, NULL);
            if (score > 0) {
                new_score = pcmk__add_scores(node->weight, score);
                if (new_score != node->weight) { // Could remain INFINITY
                    node->weight = new_score;
                    pcmk__rsc_trace(rsc,
                                    "Added %s promotion priority (%s) to score "
                                    "on %s (now %s)",
                                    child_rsc->id, pcmk_readable_score(score),
                                    pcmk__node_name(node),
                                    pcmk_readable_score(new_score));
                }
            }

            if (score > child_rsc->priority) {
                pcmk__rsc_trace(rsc,
                                "Updating %s priority to promotion score "
                                "(%d->%d)",
                                child_rsc->id, child_rsc->priority, score);
                child_rsc->priority = score;
            }
        }
    }
}

/*!
 * \internal
 * \brief If a resource's current role is started, change it to unpromoted
 *
 * \param[in,out] data       Resource to update
 * \param[in]     user_data  Ignored
 */
static void
set_current_role_unpromoted(void *data, void *user_data)
{
    pcmk_resource_t *rsc = (pcmk_resource_t *) data;

    if (rsc->role == pcmk_role_started) {
        // Promotable clones should use unpromoted role instead of started
        rsc->role = pcmk_role_unpromoted;
    }
    g_list_foreach(rsc->children, set_current_role_unpromoted, NULL);
}

/*!
 * \internal
 * \brief Set a resource's next role to unpromoted (or stopped if unassigned)
 *
 * \param[in,out] data       Resource to update
 * \param[in]     user_data  Ignored
 */
static void
set_next_role_unpromoted(void *data, void *user_data)
{
    pcmk_resource_t *rsc = (pcmk_resource_t *) data;
    GList *assigned = NULL;

    rsc->fns->location(rsc, &assigned, FALSE);
    if (assigned == NULL) {
        pe__set_next_role(rsc, pcmk_role_stopped, "stopped instance");
    } else {
        pe__set_next_role(rsc, pcmk_role_unpromoted, "unpromoted instance");
        g_list_free(assigned);
    }
    g_list_foreach(rsc->children, set_next_role_unpromoted, NULL);
}

/*!
 * \internal
 * \brief Set a resource's next role to promoted if not already set
 *
 * \param[in,out] data       Resource to update
 * \param[in]     user_data  Ignored
 */
static void
set_next_role_promoted(void *data, gpointer user_data)
{
    pcmk_resource_t *rsc = (pcmk_resource_t *) data;

    if (rsc->next_role == pcmk_role_unknown) {
        pe__set_next_role(rsc, pcmk_role_promoted, "promoted instance");
    }
    g_list_foreach(rsc->children, set_next_role_promoted, NULL);
}

/*!
 * \internal
 * \brief Show instance's promotion score on node where it will be active
 *
 * \param[in,out] instance  Promotable clone instance to show
 */
static void
show_promotion_score(pcmk_resource_t *instance)
{
    pcmk_node_t *chosen = instance->fns->location(instance, NULL, FALSE);

    if (pcmk_is_set(instance->cluster->flags, pcmk_sched_output_scores)
        && !pcmk__is_daemon && (instance->cluster->priv != NULL)) {

        pcmk__output_t *out = instance->cluster->priv;

        out->message(out, "promotion-score", instance, chosen,
                     pcmk_readable_score(instance->sort_index));

    } else if (chosen == NULL) {
        pcmk__rsc_debug(pe__const_top_resource(instance, false),
                        "%s promotion score (inactive): %s (priority=%d)",
                        instance->id, pcmk_readable_score(instance->sort_index), instance->priority);

    } else {
        pcmk__rsc_debug(pe__const_top_resource(instance, false),
                        "%s promotion score on %s: %s (priority=%d)",
                        instance->id, pcmk__node_name(chosen),
                        pcmk_readable_score(instance->sort_index),
                        instance->priority);
    }
}

/*!
 * \internal
 * \brief Set a clone instance's promotion priority
 *
 * \param[in,out] data       Promotable clone instance to update
 * \param[in]     user_data  Instance's parent clone
 */
static void
set_instance_priority(gpointer data, gpointer user_data)
{
    pcmk_resource_t *instance = (pcmk_resource_t *) data;
    const pcmk_resource_t *clone = (const pcmk_resource_t *) user_data;
    const pcmk_node_t *chosen = NULL;
    enum rsc_role_e next_role = pcmk_role_unknown;
    GList *list = NULL;

    pcmk__rsc_trace(clone, "Assigning priority for %s: %s", instance->id,
                    pcmk_role_text(instance->next_role));

    if (instance->fns->state(instance, TRUE) == pcmk_role_started) {
        set_current_role_unpromoted(instance, NULL);
    }

    // Only an instance that will be active can be promoted
    chosen = instance->fns->location(instance, &list, FALSE);
    if (pcmk__list_of_multiple(list)) {
        pcmk__config_err("Cannot promote non-colocated child %s",
                         instance->id);
    }
    g_list_free(list);
    if (chosen == NULL) {
        return;
    }

    next_role = instance->fns->state(instance, FALSE);
    switch (next_role) {
        case pcmk_role_started:
        case pcmk_role_unknown:
            // Set instance priority to its promotion score (or -1 if none)
            {
                bool is_default = false;

                instance->priority = promotion_score(instance, chosen,
                                                      &is_default);
                if (is_default) {
                    /* Default to -1 if no value is set. This allows instances
                     * eligible for promotion to be specified based solely on
                     * PCMK_XE_RSC_LOCATION constraints, but prevents any
                     * instance from being promoted if neither a constraint nor
                     * a promotion score is present.
                     */
                    instance->priority = -1;
                }
            }
            break;

        case pcmk_role_unpromoted:
        case pcmk_role_stopped:
            // Instance can't be promoted
            instance->priority = -PCMK_SCORE_INFINITY;
            break;

        case pcmk_role_promoted:
            // Nothing needed (re-creating actions after scheduling fencing)
            break;

        default:
            CRM_CHECK(FALSE, crm_err("Unknown resource role %d for %s",
                                     next_role, instance->id));
    }

    // Add relevant location constraint scores for promoted role
    apply_promoted_locations(instance, instance->rsc_location, chosen);
    apply_promoted_locations(instance, clone->rsc_location, chosen);

    // Consider instance's role-based colocations with other resources
    list = pcmk__this_with_colocations(instance);
    for (GList *iter = list; iter != NULL; iter = iter->next) {
        pcmk__colocation_t *cons = (pcmk__colocation_t *) iter->data;

        instance->cmds->apply_coloc_score(instance, cons->primary, cons, true);
    }
    g_list_free(list);

    instance->sort_index = instance->priority;
    if (next_role == pcmk_role_promoted) {
        instance->sort_index = PCMK_SCORE_INFINITY;
    }
    pcmk__rsc_trace(clone, "Assigning %s priority = %d",
                    instance->id, instance->priority);
}

/*!
 * \internal
 * \brief Set a promotable clone instance's role
 *
 * \param[in,out] data       Promotable clone instance to update
 * \param[in,out] user_data  Pointer to count of instances chosen for promotion
 */
static void
set_instance_role(gpointer data, gpointer user_data)
{
    pcmk_resource_t *instance = (pcmk_resource_t *) data;
    int *count = (int *) user_data;

    const pcmk_resource_t *clone = pe__const_top_resource(instance, false);
    pcmk_node_t *chosen = NULL;

    show_promotion_score(instance);

    if (instance->sort_index < 0) {
        pcmk__rsc_trace(clone, "Not supposed to promote instance %s",
                        instance->id);

    } else if ((*count < pe__clone_promoted_max(instance))
               || !pcmk_is_set(clone->flags, pcmk_rsc_managed)) {
        chosen = node_to_be_promoted_on(instance);
    }

    if (chosen == NULL) {
        set_next_role_unpromoted(instance, NULL);
        return;
    }

    if ((instance->role < pcmk_role_promoted)
        && !pcmk_is_set(instance->cluster->flags, pcmk_sched_quorate)
        && (instance->cluster->no_quorum_policy == pcmk_no_quorum_freeze)) {
        crm_notice("Clone instance %s cannot be promoted without quorum",
                   instance->id);
        set_next_role_unpromoted(instance, NULL);
        return;
    }

    chosen->count++;
    pcmk__rsc_info(clone, "Choosing %s (%s) on %s for promotion",
                   instance->id, pcmk_role_text(instance->role),
                   pcmk__node_name(chosen));
    set_next_role_promoted(instance, NULL);
    (*count)++;
}

/*!
 * \internal
 * \brief Set roles for all instances of a promotable clone
 *
 * \param[in,out] rsc  Promotable clone resource to update
 */
void
pcmk__set_instance_roles(pcmk_resource_t *rsc)
{
    int promoted = 0;
    GHashTableIter iter;
    pcmk_node_t *node = NULL;

    // Repurpose count to track the number of promoted instances assigned
    g_hash_table_iter_init(&iter, rsc->allowed_nodes);
    while (g_hash_table_iter_next(&iter, NULL, (void **)&node)) {
        node->count = 0;
    }

    // Set instances' promotion priorities and sort by highest priority first
    g_list_foreach(rsc->children, set_instance_priority, rsc);
    sort_promotable_instances(rsc);

    // Choose the first N eligible instances to be promoted
    g_list_foreach(rsc->children, set_instance_role, &promoted);
    pcmk__rsc_info(rsc, "%s: Promoted %d instances of a possible %d",
                   rsc->id, promoted, pe__clone_promoted_max(rsc));
}

/*!
 *
 * \internal
 * \brief Create actions for promotable clone instances
 *
 * \param[in,out] clone          Promotable clone to create actions for
 * \param[out]    any_promoting  Will be set true if any instance is promoting
 * \param[out]    any_demoting   Will be set true if any instance is demoting
 */
static void
create_promotable_instance_actions(pcmk_resource_t *clone,
                                   bool *any_promoting, bool *any_demoting)
{
    for (GList *iter = clone->children; iter != NULL; iter = iter->next) {
        pcmk_resource_t *instance = (pcmk_resource_t *) iter->data;

        instance->cmds->create_actions(instance);
        check_for_role_change(instance, any_demoting, any_promoting);
    }
}

/*!
 * \internal
 * \brief Reset each promotable instance's resource priority
 *
 * Reset the priority of each instance of a promotable clone to the clone's
 * priority (after promotion actions are scheduled, when instance priorities
 * were repurposed as promotion scores).
 *
 * \param[in,out] clone  Promotable clone to reset
 */
static void
reset_instance_priorities(pcmk_resource_t *clone)
{
    for (GList *iter = clone->children; iter != NULL; iter = iter->next) {
        pcmk_resource_t *instance = (pcmk_resource_t *) iter->data;

        instance->priority = clone->priority;
    }
}

/*!
 * \internal
 * \brief Create actions specific to promotable clones
 *
 * \param[in,out] clone  Promotable clone to create actions for
 */
void
pcmk__create_promotable_actions(pcmk_resource_t *clone)
{
    bool any_promoting = false;
    bool any_demoting = false;

    // Create actions for each clone instance individually
    create_promotable_instance_actions(clone, &any_promoting, &any_demoting);

    // Create pseudo-actions for clone as a whole
    pe__create_promotable_pseudo_ops(clone, any_promoting, any_demoting);

    // Undo our temporary repurposing of resource priority for instances
    reset_instance_priorities(clone);
}

/*!
 * \internal
 * \brief Create internal orderings for a promotable clone's instances
 *
 * \param[in,out] clone  Promotable clone instance to order
 */
void
pcmk__order_promotable_instances(pcmk_resource_t *clone)
{
    pcmk_resource_t *previous = NULL; // Needed for ordered clones

    pcmk__promotable_restart_ordering(clone);

    for (GList *iter = clone->children; iter != NULL; iter = iter->next) {
        pcmk_resource_t *instance = (pcmk_resource_t *) iter->data;

        // Demote before promote
        pcmk__order_resource_actions(instance, PCMK_ACTION_DEMOTE,
                                     instance, PCMK_ACTION_PROMOTE,
                                     pcmk__ar_ordered);

        order_instance_promotion(clone, instance, previous);
        order_instance_demotion(clone, instance, previous);
        previous = instance;
    }
}

/*!
 * \internal
 * \brief Update dependent's allowed nodes for colocation with promotable
 *
 * \param[in,out] dependent     Dependent resource to update
 * \param[in]     primary       Primary resource
 * \param[in]     primary_node  Node where an instance of the primary will be
 * \param[in]     colocation    Colocation constraint to apply
 */
static void
update_dependent_allowed_nodes(pcmk_resource_t *dependent,
                               const pcmk_resource_t *primary,
                               const pcmk_node_t *primary_node,
                               const pcmk__colocation_t *colocation)
{
    GHashTableIter iter;
    pcmk_node_t *node = NULL;
    const char *primary_value = NULL;
    const char *attr = colocation->node_attribute;

    if (colocation->score >= PCMK_SCORE_INFINITY) {
        return; // Colocation is mandatory, so allowed node scores don't matter
    }

    primary_value = pcmk__colocation_node_attr(primary_node, attr, primary);

    pcmk__rsc_trace(colocation->primary,
                    "Applying %s (%s with %s on %s by %s @%d) to %s",
                    colocation->id, colocation->dependent->id,
                    colocation->primary->id, pcmk__node_name(primary_node),
                    attr, colocation->score, dependent->id);

    g_hash_table_iter_init(&iter, dependent->allowed_nodes);
    while (g_hash_table_iter_next(&iter, NULL, (void **) &node)) {
        const char *dependent_value = pcmk__colocation_node_attr(node, attr,
                                                                 dependent);

        if (pcmk__str_eq(primary_value, dependent_value, pcmk__str_casei)) {
            node->weight = pcmk__add_scores(node->weight, colocation->score);
            pcmk__rsc_trace(colocation->primary,
                            "Added %s score (%s) to %s (now %s)",
                            colocation->id,
                            pcmk_readable_score(colocation->score),
                            pcmk__node_name(node),
                            pcmk_readable_score(node->weight));
        }
    }
}

/*!
 * \brief Update dependent for a colocation with a promotable clone
 *
 * \param[in]     primary     Primary resource in the colocation
 * \param[in,out] dependent   Dependent resource in the colocation
 * \param[in]     colocation  Colocation constraint to apply
 */
void
pcmk__update_dependent_with_promotable(const pcmk_resource_t *primary,
                                       pcmk_resource_t *dependent,
                                       const pcmk__colocation_t *colocation)
{
    GList *affected_nodes = NULL;

    /* Build a list of all nodes where an instance of the primary will be, and
     * (for optional colocations) update the dependent's allowed node scores for
     * each one.
     */
    for (GList *iter = primary->children; iter != NULL; iter = iter->next) {
        pcmk_resource_t *instance = (pcmk_resource_t *) iter->data;
        pcmk_node_t *node = instance->fns->location(instance, NULL, FALSE);

        if (node == NULL) {
            continue;
        }
        if (instance->fns->state(instance, FALSE) == colocation->primary_role) {
            update_dependent_allowed_nodes(dependent, primary, node,
                                           colocation);
            affected_nodes = g_list_prepend(affected_nodes, node);
        }
    }

    /* For mandatory colocations, add the primary's node score to the
     * dependent's node score for each affected node, and ban the dependent
     * from all other nodes.
     *
     * However, skip this for promoted-with-promoted colocations, otherwise
     * inactive dependent instances can't start (in the unpromoted role).
     */
    if ((colocation->score >= PCMK_SCORE_INFINITY)
        && ((colocation->dependent_role != pcmk_role_promoted)
            || (colocation->primary_role != pcmk_role_promoted))) {

        pcmk__rsc_trace(colocation->primary,
                        "Applying %s (mandatory %s with %s) to %s",
                        colocation->id, colocation->dependent->id,
                        colocation->primary->id, dependent->id);
        pcmk__colocation_intersect_nodes(dependent, primary, colocation,
                                         affected_nodes, true);
    }
    g_list_free(affected_nodes);
}

/*!
 * \internal
 * \brief Update dependent priority for colocation with promotable
 *
 * \param[in]     primary     Primary resource in the colocation
 * \param[in,out] dependent   Dependent resource in the colocation
 * \param[in]     colocation  Colocation constraint to apply
 *
 * \return The score added to the dependent's priority
 */
int
pcmk__update_promotable_dependent_priority(const pcmk_resource_t *primary,
                                           pcmk_resource_t *dependent,
                                           const pcmk__colocation_t *colocation)
{
    pcmk_resource_t *primary_instance = NULL;

    // Look for a primary instance where dependent will be
    primary_instance = pcmk__find_compatible_instance(dependent, primary,
                                                      colocation->primary_role,
                                                      false);

    if (primary_instance != NULL) {
        // Add primary instance's priority to dependent's
        int new_priority = pcmk__add_scores(dependent->priority,
                                            colocation->score);

        pcmk__rsc_trace(colocation->primary,
                        "Applying %s (%s with %s) to %s priority "
                        "(%s + %s = %s)",
                        colocation->id, colocation->dependent->id,
                        colocation->primary->id, dependent->id,
                        pcmk_readable_score(dependent->priority),
                        pcmk_readable_score(colocation->score),
                        pcmk_readable_score(new_priority));
        dependent->priority = new_priority;
        return colocation->score;
    }

    if (colocation->score >= PCMK_SCORE_INFINITY) {
        // Mandatory colocation, but primary won't be here
        pcmk__rsc_trace(colocation->primary,
                        "Applying %s (%s with %s) to %s: can't be promoted",
                        colocation->id, colocation->dependent->id,
                        colocation->primary->id, dependent->id);
        dependent->priority = -PCMK_SCORE_INFINITY;
        return -PCMK_SCORE_INFINITY;
    }
    return 0;
}
