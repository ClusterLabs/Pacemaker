/*
 * Copyright (c) 2012 David Vossel <dvossel@redhat.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * 
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include <crm_internal.h>

#include <glib.h>
#include <unistd.h>

#include <crm/crm.h>
#include <crm/msg_xml.h>
#include <crm/services.h>
#include <crm/common/mainloop.h>
#include <crm/common/ipc.h>

#include <lrmd_private.h>

GMainLoop *mainloop = NULL;
qb_ipcs_service_t *ipcs = NULL;
stonith_t *stonith_api = NULL;


static void
stonith_connection_destroy_cb(stonith_t * st, stonith_event_t *e)
{
    stonith_api->state = stonith_disconnected;
    crm_err("LRMD lost STONITH connection");
    stonith_connection_failed();
}

stonith_t *
get_stonith_connection(void)
{
    if (stonith_api && stonith_api->state == stonith_disconnected) {
        stonith_api_delete(stonith_api);
        stonith_api = NULL;
    }

    if (!stonith_api) {
        int rc = 0;
        int tries = 10;

        stonith_api = stonith_api_new();
        do {
            rc = stonith_api->cmds->connect(stonith_api, "lrmd", NULL);
            if (rc == pcmk_ok) {
                stonith_api->cmds->register_notification(stonith_api,
                    T_STONITH_NOTIFY_DISCONNECT,
                    stonith_connection_destroy_cb);
                break;
            }
            sleep(1);
            tries--;
        } while (tries);

        if (rc) {
            crm_err("Unable to connect to stonith daemon to execute command. error: %s",
                    pcmk_strerror(rc));
            stonith_api_delete(stonith_api);
            stonith_api = NULL;
        }
    }
    return stonith_api;
}

static int32_t
lrmd_ipc_accept(qb_ipcs_connection_t * c, uid_t uid, gid_t gid)
{
    struct qb_ipcs_connection_stats stats = { 0, };

    qb_ipcs_connection_stats_get(c, &stats, 1);
    crm_info("Accepting client connection: %p pid=%d for uid=%d gid=%d",
             c, stats.client_pid, uid, gid);
    return 0;
}

static void
lrmd_ipc_created(qb_ipcs_connection_t * c)
{
    lrmd_client_t *new_client = NULL;

    new_client = calloc(1, sizeof(lrmd_client_t));
    new_client->channel = c;

    new_client->id = crm_generate_uuid();
    crm_trace("LRMD client connection established. %p id: %s", c, new_client->id);

    g_hash_table_insert(client_list, new_client->id, new_client);
    qb_ipcs_context_set(c, new_client);
}

static int32_t
lrmd_ipc_dispatch(qb_ipcs_connection_t * c, void *data, size_t size)
{
    uint32_t id = 0;
    uint32_t flags = 0;
    xmlNode *request = crm_ipcs_recv(c, data, size, &id, &flags);
    lrmd_client_t *client = (lrmd_client_t *) qb_ipcs_context_get(c);

    CRM_CHECK(client != NULL, crm_err("Invalid client");
              return FALSE);
    CRM_CHECK(client->id != NULL, crm_err("Invalid client: %p", client);
              return FALSE);

    CRM_CHECK(flags & crm_ipc_client_response, crm_err("Invalid client request: %p", client);
              return FALSE);
    
    if (!request) {
        return 0;
    }

    if (!client->name) {
        const char *value = crm_element_value(request, F_LRMD_CLIENTNAME);

        if (value == NULL) {
            client->name = crm_itoa(crm_ipcs_client_pid(c));
        } else {
            client->name = strdup(value);
        }
    }

    crm_xml_add(request, F_LRMD_CLIENTID, client->id);
    crm_xml_add(request, F_LRMD_CLIENTNAME, client->name);

    process_lrmd_message(client, id, request);

    free_xml(request);
    return 0;
}

static int32_t
lrmd_ipc_closed(qb_ipcs_connection_t * c)
{
    lrmd_client_t *client = (lrmd_client_t *) qb_ipcs_context_get(c);
    int found = 0;

    if (!client) {
        crm_err("No client for ipc");
        return 0;
    }

    if (client->id) {
        found = g_hash_table_remove(client_list, client->id);
    }

    if (!found) {
        crm_err("Asked to remove unknown client with id %d", client->id);
    }

    return 0;
}

static void
lrmd_ipc_destroy(qb_ipcs_connection_t * c)
{
    lrmd_client_t *client = (lrmd_client_t *) qb_ipcs_context_get(c);

    if (!client) {
        crm_err("No client for ipc");
        return;
    }

    crm_info("LRMD client disconnecting %p - name: %s id: %s", c, client->name, client->id);

    qb_ipcs_context_set(c, NULL);
    free(client->name);
    free(client->id);
    free(client);
}

static struct qb_ipcs_service_handlers lrmd_ipc_callbacks = {
    .connection_accept = lrmd_ipc_accept,
    .connection_created = lrmd_ipc_created,
    .msg_process = lrmd_ipc_dispatch,
    .connection_closed = lrmd_ipc_closed,
    .connection_destroyed = lrmd_ipc_destroy
};

void
lrmd_shutdown(int nsig)
{
    crm_info("Terminating with  %d clients", g_hash_table_size(client_list));
    if (ipcs) {
        mainloop_del_ipc_server(ipcs);
    }
    exit(0);
}

static int
try_server_create(void)
{
    int tries = 10;

    /*
     * This should complete on the first iteration. The only
     * known reason for why this would fail is if another lrmd process
     * already exists on the system.  To avoid this situation
     * we attempt to connect to the old lrmd process and shut it down
     * using the client library.
     */
    do {
        ipcs = mainloop_add_ipc_server(CRM_SYSTEM_LRMD, QB_IPC_SHM, &lrmd_ipc_callbacks);

        if (ipcs == NULL) {
            xmlNode *reply = NULL;
            xmlNode *hello = create_xml_node(NULL, "lrmd_command");
            crm_ipc_t *ipc = crm_ipc_new("lrmd", 0);

            if (ipc && crm_ipc_connect(ipc)) {
                crm_xml_add(hello, F_TYPE, T_LRMD);
                crm_xml_add(hello, F_LRMD_OPERATION, CRM_OP_QUIT);
                crm_xml_add(hello, F_LRMD_CLIENTNAME, "new_lrmd");

                crm_ipc_send(ipc, hello, 0, 0, &reply);

                crm_ipc_close(ipc);
                crm_ipc_destroy(ipc);
            }
            free_xml(reply);
            free_xml(hello);

            crm_err
                ("New IPC server could not be created because another lrmd process exists, sending shutdown command to old lrmd process.");
        }

        tries--;
    } while (!ipcs && tries);

    return ipcs ? 0 : -1;
}

/* *INDENT-OFF* */
static struct crm_option long_options[] = {
    /* Top-level Options */
    {"help",    0, 0, '?', "\tThis text"},
    {"version", 0, 0, '$', "\tVersion information"  },
    {"verbose", 0, 0, 'V', "\tIncrease debug output"},
    
    {"logfile", 1, 0, 'l', "\tSend logs to the additional named logfile"},
    {0, 0, 0, 0}
};
/* *INDENT-ON* */

int
main(int argc, char **argv)
{
    int rc = 0;
    int flag = 0;
    int index = 0;

    crm_log_init("lrmd", LOG_INFO, TRUE, FALSE, argc, argv, FALSE);
    crm_set_options(NULL, "[options]", long_options, "Daemon for controlling services confirming to different standards");

    while (1) {
        flag = crm_get_option(argc, argv, &index);
        if (flag == -1) {
            break;
        }

        switch (flag) {
            case 'l':
                crm_add_logfile(optarg);
                break;
            case 'V':
                set_crm_log_level(crm_log_level+1);
                break;
            case '?':
            case '$':
                crm_help(flag, EX_OK);
                break;
            default:
                crm_help('?', EX_USAGE);
                break;
        }
    }

    rsc_list = g_hash_table_new_full(crm_str_hash, g_str_equal, NULL, free_rsc);
    client_list = g_hash_table_new(crm_str_hash, g_str_equal);
    if (try_server_create()) {
        crm_err("Failed to allocate lrmd server.  shutting down");
        exit(-1);
    }

    mainloop_add_signal(SIGTERM, lrmd_shutdown);
    mainloop = g_main_new(FALSE);
    crm_info("Starting");
    g_main_run(mainloop);

    mainloop_del_ipc_server(ipcs);
    g_hash_table_destroy(client_list);
    g_hash_table_destroy(rsc_list);

    if (stonith_api) {
        stonith_api->cmds->disconnect(stonith_api);
        stonith_api_delete(stonith_api);
    }

    return rc;
}
