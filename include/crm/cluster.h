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
#ifndef CRM_COMMON_CLUSTER__H
#  define CRM_COMMON_CLUSTER__H

#  include <crm/common/xml.h>
#  include <crm/common/util.h>

#  if SUPPORT_HEARTBEAT
#    include <heartbeat/hb_api.h>
#    include <ocf/oc_event.h>
#  endif

extern gboolean crm_have_quorum;
extern GHashTable *crm_peer_cache;
extern GHashTable *crm_peer_id_cache;
extern unsigned long long crm_peer_seq;

#  ifndef CRM_SERVICE
#    define CRM_SERVICE PCMK_SERVICE_ID
#  endif

/* *INDENT-OFF* */
#define CRM_NODE_LOST      "lost"
#define CRM_NODE_MEMBER    "member"
#define CRM_NODE_ACTIVE    CRM_NODE_MEMBER
#define CRM_NODE_EVICTED   "evicted"

/* *INDENT-ON* */

typedef struct crm_peer_node_s {
    uint32_t id;   /* Only used by corosync derivatives */
    uint64_t born; /* Only used by heartbeat and the legacy plugin */
    uint64_t last_seen;

    int32_t votes; /* Only used by the legacy plugin */
    uint32_t processes;

    char *uname;
    char *uuid;
    char *state;
    char *expected;

    char *addr;   /* Only used by the legacy plugin */
    char *version;/* Unused */
} crm_node_t;

void crm_peer_init(void);
void crm_peer_destroy(void);
char *get_corosync_uuid(uint32_t id, const char *uname);
const char *get_node_uuid(uint32_t id, const char *uname);
int get_corosync_id(int id, const char *uuid);

typedef struct crm_cluster_s
{
        char *uuid;
        char *uname;
        uint32_t nodeid;

#if SUPPORT_HEARTBEAT
        ll_cluster_t *hb_conn;
        void (*hb_dispatch)(HA_Message *msg, void *private);
#endif

        gboolean (*cs_dispatch) (int kind, const char *from, const char *data);
        void (*destroy) (gpointer);

} crm_cluster_t;

gboolean crm_cluster_connect(crm_cluster_t *cluster);

enum crm_ais_msg_types;
gboolean send_cluster_message(crm_node_t *node, enum crm_ais_msg_types service,
                              xmlNode * data, gboolean ordered);

void destroy_crm_node(gpointer/* crm_node_t* */ data);

crm_node_t *crm_get_peer(unsigned int id, const char *uname);

guint crm_active_peers(void);
gboolean crm_is_peer_active(const crm_node_t * node);
guint reap_crm_member(uint32_t id);
int crm_terminate_member(int nodeid, const char *uname, void* unused);
int crm_terminate_member_no_mainloop(int nodeid, const char *uname, int *connection);
gboolean crm_get_cluster_name(char **cname);

#  if SUPPORT_HEARTBEAT
gboolean crm_is_heartbeat_peer_active(const crm_node_t * node);
#  endif

#  if SUPPORT_COROSYNC
extern int ais_fd_sync;
gboolean crm_is_corosync_peer_active(const crm_node_t * node);
gboolean send_ais_text(int class, const char *data, gboolean local,
                              crm_node_t *node, enum crm_ais_msg_types dest);
gboolean get_ais_nodeid(uint32_t * id, char **uname);
#  endif

void empty_uuid_cache(void);
const char *get_uuid(const char *uname);
const char *get_uname(const char *uuid);
void set_uuid(xmlNode * node, const char *attr, const char *uname);
void unget_uuid(const char *uname);

enum crm_status_type {
    crm_status_uname,
    crm_status_nstate,
    crm_status_processes,
};

enum crm_ais_msg_types text2msg_type(const char *text);
void crm_set_status_callback(void (*dispatch) (enum crm_status_type, crm_node_t *, const void *));

/* *INDENT-OFF* */
enum cluster_type_e 
{
    pcmk_cluster_unknown     = 0x0001,
    pcmk_cluster_invalid     = 0x0002,
    pcmk_cluster_heartbeat   = 0x0004,
    pcmk_cluster_classic_ais = 0x0010,
    pcmk_cluster_corosync    = 0x0020,
    pcmk_cluster_cman        = 0x0040,
};
/* *INDENT-ON* */

enum cluster_type_e get_cluster_type(void);
const char *name_for_cluster_type(enum cluster_type_e type);

gboolean is_corosync_cluster(void);
gboolean is_cman_cluster(void);
gboolean is_openais_cluster(void);
gboolean is_classic_ais_cluster(void);
gboolean is_heartbeat_cluster(void);

#endif
