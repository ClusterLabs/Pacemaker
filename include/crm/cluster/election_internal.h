/*
 * Copyright 2009-2024 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU Lesser General Public License
 * version 2.1 or later (LGPLv2.1+) WITHOUT ANY WARRANTY.
 */

#ifndef PCMK__CRM_CLUSTER_ELECTION_INTERNAL__H
#define PCMK__CRM_CLUSTER_ELECTION_INTERNAL__H

#include <stdbool.h>        // bool

#include <glib.h>           // guint, GSourceFunc
#include <libxml/tree.h>    // xmlNode

#include <crm/common/ipc.h> // enum pcmk_ipc_server
#include <crm/cluster.h>    // pcmk_cluster_t

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \file
 * \brief Functions for conducting elections
 *
 * An election is useful for a daemon that runs on all nodes but needs any one
 * instance to perform a special role.
 *
 * Elections are closely tied to the cluster peer cache. Peers in the cache that
 * are active members are eligible to vote. Elections are named for logging
 * purposes, but only one election may exist at any time, so typically an
 * election would be created at daemon start-up and freed at shutdown.
 *
 * Pacemaker's election procedure has been heavily adapted from the
 * Invitation Algorithm variant of the Garcia-Molina Bully Algorithm:
 *
 *   https://en.wikipedia.org/wiki/Bully_algorithm
 *
 * Elections are conducted via cluster messages. There are two types of
 * messages: a "vote" is a declaration of the voting node's candidacy, and is
 * always broadcast; a "no-vote" is a concession by the responding node, and is
 * always a reply to the preferred node's vote. (These correspond to "invite"
 * and "accept" in the traditional algorithm.)
 *
 * A vote together with any no-vote replies to it is considered an election
 * round. Rounds are numbered with a simple counter unique to each node
 * (this would be the group number in the traditional algorithm). Concurrent
 * election rounds are possible.
 *
 * An election round is started when any node broadcasts a vote. When a node
 * receives another node's vote, it compares itself against the sending node
 * according to certain metrics, and either starts a new round (if it prefers
 * itself) or replies to the other node with a no-vote (if it prefers that
 * node).
 *
 * If a node receives no-votes from all other active nodes, it declares itself
 * the winner. The library API does not notify other nodes of this; callers
 * must implement that if desired.
 */

// Possible election results
enum election_result {
    election_start = 0,     // New election needed
    election_in_progress,   // Election started but not all peers have voted
    election_lost,          // Local node lost most recent election
    election_won,           // Local node won most recent election
    election_error,         // Election message or object invalid
};

void election_reset(pcmk_cluster_t *cluster);
void election_init(pcmk_cluster_t *cluster, void (*cb)(pcmk_cluster_t *));

void election_timeout_set_period(pcmk_cluster_t *cluster, guint period_ms);
void election_timeout_stop(pcmk_cluster_t *cluster);

void election_vote(pcmk_cluster_t *cluster);
bool election_check(pcmk_cluster_t *cluster);
void election_remove(pcmk_cluster_t *cluster, const char *uname);
enum election_result election_state(const pcmk_cluster_t *cluster);
enum election_result election_count_vote(pcmk_cluster_t *cluster,
                                         const xmlNode *message, bool can_win);
void election_clear_dampening(pcmk_cluster_t *cluster);

#ifdef __cplusplus
}
#endif

#endif // PCMK__CRM_CLUSTER_ELECTION_INTERNAL__H
