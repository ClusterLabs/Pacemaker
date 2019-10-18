/*
 * Copyright 2013-2018 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU Lesser General Public License
 * version 2.1 or later (LGPLv2.1+) WITHOUT ANY WARRANTY.
 */

#ifndef CRM_COMMON_IPCS__H
#  define CRM_COMMON_IPCS__H

#ifdef __cplusplus
extern "C" {
#endif

#  include <stdbool.h>
#  include <qb/qbipcs.h>
#  ifdef HAVE_GNUTLS_GNUTLS_H
#    undef KEYFILE
#    include <gnutls/gnutls.h>
#  endif

#  include <crm/common/ipc.h>
#  include <crm/common/mainloop.h>

typedef struct crm_client_s crm_client_t;

enum client_type {
    CRM_CLIENT_IPC = 1,
    CRM_CLIENT_TCP = 2,
#  ifdef HAVE_GNUTLS_GNUTLS_H
    CRM_CLIENT_TLS = 3,
#  endif
};

struct crm_remote_s {
    /* Shared */
    char *buffer;
    size_t buffer_size;
    size_t buffer_offset;
    int auth_timeout;
    int tcp_socket;
    mainloop_io_t *source;

    /* CIB-only */
    bool authenticated;
    char *token;

    /* TLS only */
#  ifdef HAVE_GNUTLS_GNUTLS_H
    gnutls_session_t *tls_session;
    bool tls_handshake_complete;
#  endif
};

enum crm_client_flags
{
    crm_client_flag_ipc_proxied    = 0x00001, /* ipc_proxy code only */
    crm_client_flag_ipc_privileged = 0x00002, /* root or cluster user */
};

struct crm_client_s {
    uint pid;

    /* this pair valid for CRM_CLIENT_IPC kind only (authentic AF_UNIX data) */
    uid_t uid;
    gid_t gid;

    char *id;
    char *name;
    char *user;

    /* Provided for server use (not used by library) */
    /* @TODO merge options, flags, and kind (reserving lower bits for server) */
    long long options;

    int request_id;
    uint32_t flags;
    void *userdata;

    int event_timer;
    GQueue *event_queue;

    /* Depending on the value of kind, only some of the following
     * will be populated/valid
     */
    enum client_type kind;

    qb_ipcs_connection_t *ipcs; /* IPC */

    struct crm_remote_s *remote;        /* TCP/TLS */

    unsigned int queue_backlog; /* IPC queue length after last flush */
    unsigned int queue_max;     /* Evict client whose queue grows this big */
};

extern GHashTable *client_connections;

void crm_client_init(void);
void crm_client_cleanup(void);

crm_client_t *crm_client_get(qb_ipcs_connection_t * c);
crm_client_t *crm_client_get_by_id(const char *id);
const char *crm_client_name(crm_client_t * c);
const char *crm_client_type_text(enum client_type client_type);

crm_client_t *crm_client_alloc(void *key);
crm_client_t *crm_client_new(qb_ipcs_connection_t * c, uid_t uid, gid_t gid);
void crm_client_destroy(crm_client_t * c);
void crm_client_disconnect_all(qb_ipcs_service_t *s);
bool crm_set_client_queue_max(crm_client_t *client, const char *qmax);

void crm_ipcs_send_ack(crm_client_t * c, uint32_t request, uint32_t flags,
                       const char *tag, const char *function, int line);

/* when max_send_size is 0, default ipc buffer size is used */
ssize_t crm_ipc_prepare(uint32_t request, xmlNode * message, struct iovec ** result, uint32_t max_send_size);
ssize_t crm_ipcs_send(crm_client_t * c, uint32_t request, xmlNode * message, enum crm_ipc_flags flags);
ssize_t crm_ipcs_sendv(crm_client_t * c, struct iovec *iov, enum crm_ipc_flags flags);
xmlNode *crm_ipcs_recv(crm_client_t * c, void *data, size_t size, uint32_t * id, uint32_t * flags);

int crm_ipcs_client_pid(qb_ipcs_connection_t * c);

#ifdef __cplusplus
}
#endif

#endif
