/*
 * Copyright 2021-2023 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU Lesser General Public License
 * version 2.1 or later (LGPLv2.1+) WITHOUT ANY WARRANTY.
 */
#ifndef PCMK__PCMKI_PCMKI_RESOURCE__H
#define PCMK__PCMKI_PCMKI_RESOURCE__H

#include <glib.h>

#include <crm/cib/cib_types.h>
#include <crm/common/scheduler.h>
#include <crm/common/output_internal.h>

int pcmk__resource_delete(cib_t *cib, uint32_t cib_opts, const char *rsc_id,
                          const char *rsc_type);

int pcmk__resource_digests(pcmk__output_t *out, pcmk_resource_t *rsc,
                           const pcmk_node_t *node, GHashTable *overrides);

#endif /* PCMK__PCMKI_PCMKI_RESOURCE__H */
