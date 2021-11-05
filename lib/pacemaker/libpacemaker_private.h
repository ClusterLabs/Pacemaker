/*
 * Copyright 2021 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU Lesser General Public License
 * version 2.1 or later (LGPLv2.1+) WITHOUT ANY WARRANTY.
 */

#ifndef PCMK__LIBPACEMAKER_PRIVATE__H
#  define PCMK__LIBPACEMAKER_PRIVATE__H

/* This header is for the sole use of libpacemaker, so that functions can be
 * declared with G_GNUC_INTERNAL for efficiency.
 */

#include <crm/pengine/pe_types.h> // pe_action_t, pe_node_t, pe_working_set_t

G_GNUC_INTERNAL
bool pcmk__graph_has_loop(pe_action_t *init_action, pe_action_t *action,
                          pe_action_wrapper_t *input);

G_GNUC_INTERNAL
void pcmk__order_vs_fence(pe_action_t *stonith_op, pe_working_set_t *data_set);

G_GNUC_INTERNAL
void pcmk__order_vs_unfence(pe_resource_t *rsc, pe_node_t *node,
                            pe_action_t *action, enum pe_ordering order,
                            pe_working_set_t *data_set);

G_GNUC_INTERNAL
void pcmk__fence_guest(pe_node_t *node, pe_working_set_t *data_set);

G_GNUC_INTERNAL
bool pcmk__node_unfenced(pe_node_t *node);

G_GNUC_INTERNAL
bool pcmk__is_unfence_device(const pe_resource_t *rsc,
                             const pe_working_set_t *data_set);

G_GNUC_INTERNAL
pe_resource_t *pcmk__find_constraint_resource(GList *rsc_list, const char *id);

G_GNUC_INTERNAL
xmlNode *pcmk__expand_tags_in_sets(xmlNode *xml_obj,
                                   pe_working_set_t *data_set);

G_GNUC_INTERNAL
bool pcmk__valid_resource_or_tag(pe_working_set_t *data_set, const char *id,
                                 pe_resource_t **rsc, pe_tag_t **tag);

G_GNUC_INTERNAL
bool pcmk__tag_to_set(xmlNode *xml_obj, xmlNode **rsc_set, const char *attr,
                      bool convert_rsc, pe_working_set_t *data_set);

G_GNUC_INTERNAL
void pcmk__create_internal_constraints(pe_working_set_t *data_set);


// Location constraints

G_GNUC_INTERNAL
void pcmk__unpack_location(xmlNode *xml_obj, pe_working_set_t *data_set);

G_GNUC_INTERNAL
pe__location_t *pcmk__new_location(const char *id, pe_resource_t *rsc,
                                   int node_weight, const char *discover_mode,
                                   pe_node_t *foo_node,
                                   pe_working_set_t *data_set);

G_GNUC_INTERNAL
void pcmk__apply_locations(pe_working_set_t *data_set);

G_GNUC_INTERNAL
void pcmk__apply_location(pe__location_t *constraint, pe_resource_t *rsc);


// Colocation constraints

enum pcmk__coloc_affects {
    pcmk__coloc_affects_nothing = 0,
    pcmk__coloc_affects_location,
    pcmk__coloc_affects_role,
};

G_GNUC_INTERNAL
enum pcmk__coloc_affects pcmk__colocation_affects(pe_resource_t *dependent,
                                                  pe_resource_t *primary,
                                                  pcmk__colocation_t *constraint,
                                                  bool preview);

G_GNUC_INTERNAL
void pcmk__apply_coloc_to_weights(pe_resource_t *dependent,
                                  pe_resource_t *primary,
                                  pcmk__colocation_t *constraint);

G_GNUC_INTERNAL
void pcmk__apply_coloc_to_priority(pe_resource_t *dependent,
                                   pe_resource_t *primary,
                                   pcmk__colocation_t *constraint);

G_GNUC_INTERNAL
void pcmk__unpack_colocation(xmlNode *xml_obj, pe_working_set_t *data_set);

G_GNUC_INTERNAL
void pcmk__new_colocation(const char *id, const char *node_attr, int score,
                          pe_resource_t *dependent, pe_resource_t *primary,
                          const char *dependent_role, const char *primary_role,
                          bool influence, pe_working_set_t *data_set);

G_GNUC_INTERNAL
void pcmk__block_colocated_starts(pe_action_t *action,
                                  pe_working_set_t *data_set);

G_GNUC_INTERNAL
void pcmk__new_ordering(pe_resource_t *lh_rsc, char *lh_task,
                        pe_action_t *lh_action, pe_resource_t *rh_rsc,
                        char *rh_task, pe_action_t *rh_action,
                        enum pe_ordering type, pe_working_set_t *data_set);

G_GNUC_INTERNAL
void pcmk__unpack_ordering(xmlNode *xml_obj, pe_working_set_t *data_set);

G_GNUC_INTERNAL
void pcmk__disable_invalid_orderings(pe_working_set_t *data_set);

G_GNUC_INTERNAL
void pcmk__order_stops_before_shutdown(pe_node_t *node,
                                       pe_action_t *shutdown_op,
                                       pe_working_set_t *data_set);

G_GNUC_INTERNAL
void pcmk__apply_orderings(pe_working_set_t *data_set);

/*!
 * \internal
 * \brief Create a new ordering between two resource actions
 *
 * \param[in] lh_rsc    Resource for 'first' action
 * \param[in] rh_rsc    Resource for 'then' action
 * \param[in] lh_task   Action key for 'first' action
 * \param[in] rh_task   Action key for 'then' action
 * \param[in] flags     Bitmask of enum pe_ordering flags
 * \param[in] data_set  Cluster working set to add ordering to
 */
#define pcmk__order_resource_actions(lh_rsc, lh_task, rh_rsc, rh_task,      \
                                     flags, data_set)                       \
    pcmk__new_ordering((lh_rsc), pcmk__op_key((lh_rsc)->id, (lh_task), 0),  \
                       NULL,                                                \
                       (rh_rsc), pcmk__op_key((rh_rsc)->id, (rh_task), 0),  \
                       NULL, (flags), (data_set))

#define pcmk__order_starts(rsc1, rsc2, type, data_set)       \
    pcmk__order_resource_actions((rsc1), CRMD_ACTION_START,  \
                                 (rsc2), CRMD_ACTION_START, (type), (data_set))

#define pcmk__order_stops(rsc1, rsc2, type, data_set)        \
    pcmk__order_resource_actions((rsc1), CRMD_ACTION_STOP,   \
                                 (rsc2), CRMD_ACTION_STOP, (type), (data_set))

G_GNUC_INTERNAL
void pcmk__unpack_rsc_ticket(xmlNode *xml_obj, pe_working_set_t *data_set);

G_GNUC_INTERNAL
void pcmk__order_probes(pe_working_set_t *data_set);

G_GNUC_INTERNAL
bool pcmk__is_failed_remote_node(pe_node_t *node);

G_GNUC_INTERNAL
void pcmk__order_remote_connection_actions(pe_working_set_t *data_set);

G_GNUC_INTERNAL
bool pcmk__rsc_corresponds_to_guest(pe_resource_t *rsc, pe_node_t *node);

G_GNUC_INTERNAL
pe_node_t *pcmk__connection_host_for_action(pe_action_t *action);

G_GNUC_INTERNAL
void pcmk__substitute_remote_addr(pe_resource_t *rsc, GHashTable *params,
                                  pe_working_set_t *data_set);

G_GNUC_INTERNAL
void pcmk__add_bundle_meta_to_xml(xmlNode *args_xml, pe_action_t *action);


// Groups (pcmk_sched_group.c)

G_GNUC_INTERNAL
GList *pcmk__group_colocated_resources(pe_resource_t *rsc,
                                       pe_resource_t *orig_rsc,
                                       GList *colocated_rscs);


// Functions applying to more than one variant (pcmk_sched_resource.c)

G_GNUC_INTERNAL
GList *pcmk__colocated_resources(pe_resource_t *rsc, pe_resource_t *orig_rsc,
                                 GList *colocated_rscs);

#endif // PCMK__LIBPACEMAKER_PRIVATE__H
