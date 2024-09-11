/*
 * Copyright 2020-2024 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU Lesser General Public License
 * version 2.1 or later (LGPLv2.1+) WITHOUT ANY WARRANTY.
 */

#ifndef PCMK__CLUSTER_CRMCLUSTER_PRIVATE__H
#define PCMK__CLUSTER_CRMCLUSTER_PRIVATE__H

/* This header is for the sole use of libcrmcluster, so that functions can be
 * declared with G_GNUC_INTERNAL for efficiency.
 */

#include <stdbool.h>                // bool
#include <stdint.h>                 // uint32_t, uint64_t

#include <glib.h>                   // G_GNUC_INTERNAL, gboolean
#include <libxml/tree.h>            // xmlNode

#if SUPPORT_COROSYNC
#include <corosync/cpg.h>           // cpg_handle_t
#endif  // SUPPORT_COROSYNC

#include <crm/cluster.h>            // pcmk_cluster_t
#include <crm/cluster/internal.h>   // pcmk__node_status_t

#ifdef __cplusplus
extern "C" {
#endif

G_GNUC_INTERNAL
void pcmk__cluster_set_quorum(bool quorate);

G_GNUC_INTERNAL
void election_fini(pcmk_cluster_t *cluster);

#if SUPPORT_COROSYNC

G_GNUC_INTERNAL
bool pcmk__corosync_is_active(void);

G_GNUC_INTERNAL
bool pcmk__corosync_has_nodelist(void);

G_GNUC_INTERNAL
char *pcmk__corosync_uuid(const pcmk__node_status_t *peer);

G_GNUC_INTERNAL
char *pcmk__corosync_name(uint64_t /*cmap_handle_t */ cmap_handle,
                          uint32_t nodeid);

G_GNUC_INTERNAL
int pcmk__corosync_connect(pcmk_cluster_t *cluster);

G_GNUC_INTERNAL
void pcmk__corosync_disconnect(pcmk_cluster_t *cluster);

G_GNUC_INTERNAL
bool pcmk__corosync_is_peer_active(const pcmk__node_status_t *node);

G_GNUC_INTERNAL
int pcmk__cpg_connect(pcmk_cluster_t *cluster);

G_GNUC_INTERNAL
void pcmk__cpg_disconnect(pcmk_cluster_t *cluster);

G_GNUC_INTERNAL
uint32_t pcmk__cpg_local_nodeid(cpg_handle_t handle);

G_GNUC_INTERNAL
bool pcmk__cpg_send_xml(const xmlNode *msg, const pcmk__node_status_t *node,
                        enum pcmk_ipc_server dest);

#endif  // SUPPORT_COROSYNC

#ifdef __cplusplus
}
#endif

#endif // PCMK__CLUSTER_CRMCLUSTER_PRIVATE__H
