/*
 * Copyright 2018-2023 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU Lesser General Public License
 * version 2.1 or later (LGPLv2.1+) WITHOUT ANY WARRANTY.
 */

#ifndef PE_STATUS_PRIVATE__H
#  define PE_STATUS_PRIVATE__H

/* This header is for the sole use of libpe_status, so that functions can be
 * declared with G_GNUC_INTERNAL for efficiency.
 */

#if defined(PCMK__UNIT_TESTING)
#undef G_GNUC_INTERNAL
#define G_GNUC_INTERNAL
#endif

#include <glib.h>                 // GSList, GList, GHashTable
#include <libxml/tree.h>          // xmlNode

#include <crm/pengine/status.h>   // pcmk_action_t, pcmk_resource_t, etc.

/*!
 * \internal
 * \deprecated This macro will be removed in a future release
 */
#  define status_print(fmt, args...)           \
   if(options & pe_print_html) {           \
       FILE *stream = print_data;      \
       fprintf(stream, fmt, ##args);       \
   } else if(options & pe_print_printf || options & pe_print_ncurses) {      \
       FILE *stream = print_data;      \
       fprintf(stream, fmt, ##args);       \
   } else if(options & pe_print_xml) {     \
       FILE *stream = print_data;      \
       fprintf(stream, fmt, ##args);       \
   } else if(options & pe_print_log) {     \
       int log_level = *(int*)print_data;  \
       do_crm_log(log_level, fmt, ##args); \
   }

typedef struct notify_data_s {
    GSList *keys;               // Environment variable name/value pairs

    const char *action;

    pcmk_action_t *pre;
    pcmk_action_t *post;
    pcmk_action_t *pre_done;
    pcmk_action_t *post_done;

    GList *active;            /* notify_entry_t*  */
    GList *inactive;          /* notify_entry_t*  */
    GList *start;             /* notify_entry_t*  */
    GList *stop;              /* notify_entry_t*  */
    GList *demote;            /* notify_entry_t*  */
    GList *promote;           /* notify_entry_t*  */
    GList *promoted;          /* notify_entry_t*  */
    GList *unpromoted;        /* notify_entry_t*  */
    GHashTable *allowed_nodes;
} notify_data_t;

G_GNUC_INTERNAL
pcmk_resource_t *pe__create_clone_child(pcmk_resource_t *rsc,
                                      pcmk_scheduler_t *scheduler);

G_GNUC_INTERNAL
void pe__create_action_notifications(pcmk_resource_t *rsc,
                                     notify_data_t *n_data);

G_GNUC_INTERNAL
void pe__free_action_notification_data(notify_data_t *n_data);

G_GNUC_INTERNAL
notify_data_t *pe__action_notif_pseudo_ops(pcmk_resource_t *rsc,
                                           const char *task,
                                           pcmk_action_t *action,
                                           pcmk_action_t *complete);

G_GNUC_INTERNAL
void pe__force_anon(const char *standard, pcmk_resource_t *rsc, const char *rid,
                    pcmk_scheduler_t *scheduler);

G_GNUC_INTERNAL
gint pe__cmp_rsc_priority(gconstpointer a, gconstpointer b);

G_GNUC_INTERNAL
gboolean pe__unpack_resource(xmlNode *xml_obj, pcmk_resource_t **rsc,
                             pcmk_resource_t *parent,
                             pcmk_scheduler_t *scheduler);

G_GNUC_INTERNAL
gboolean unpack_remote_nodes(xmlNode *xml_resources,
                             pcmk_scheduler_t *scheduler);

G_GNUC_INTERNAL
gboolean unpack_resources(const xmlNode *xml_resources,
                          pcmk_scheduler_t *scheduler);

G_GNUC_INTERNAL
void pcmk__unpack_fencing_topology(const xmlNode *xml_fencing_topology,
                                   pcmk_scheduler_t *scheduler);

G_GNUC_INTERNAL
gboolean unpack_config(xmlNode *config, pcmk_scheduler_t *scheduler);

G_GNUC_INTERNAL
gboolean unpack_nodes(xmlNode *xml_nodes, pcmk_scheduler_t *scheduler);

G_GNUC_INTERNAL
gboolean unpack_tags(xmlNode *xml_tags, pcmk_scheduler_t *scheduler);

G_GNUC_INTERNAL
gboolean unpack_status(xmlNode *status, pcmk_scheduler_t *scheduler);

G_GNUC_INTERNAL
pcmk__op_digest_t *pe__compare_fencing_digest(pcmk_resource_t *rsc,
                                              const char *agent,
                                              pcmk_node_t *node,
                                              pcmk_scheduler_t *scheduler);

G_GNUC_INTERNAL
void pe__unpack_node_health_scores(pcmk_scheduler_t *scheduler);

// Primitive resource methods

G_GNUC_INTERNAL
unsigned int pe__primitive_max_per_node(const pcmk_resource_t *rsc);

// Group resource methods

G_GNUC_INTERNAL
unsigned int pe__group_max_per_node(const pcmk_resource_t *rsc);

// Clone resource methods

G_GNUC_INTERNAL
unsigned int pe__clone_max_per_node(const pcmk_resource_t *rsc);

// Bundle resource methods

G_GNUC_INTERNAL
pcmk_node_t *pe__bundle_active_node(const pcmk_resource_t *rsc,
                                    unsigned int *count_all,
                                    unsigned int *count_clean);

G_GNUC_INTERNAL
unsigned int pe__bundle_max_per_node(const pcmk_resource_t *rsc);

#endif  // PE_STATUS_PRIVATE__H
