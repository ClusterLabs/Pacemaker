/*
 * Copyright 2021-2024 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU Lesser General Public License
 * version 2.1 or later (LGPLv2.1+) WITHOUT ANY WARRANTY.
 */

#ifndef PCMK__CRM_CIB_UTIL_COMPAT__H
#  define PCMK__CRM_CIB_UTIL_COMPAT__H

#include <libxml/tree.h>        // xmlNode

#include <crm/cib/cib_types.h>  // cib_t

#include <crm/common/xml.h>
#ifdef __cplusplus
extern "C" {
#endif

/**
 * \file
 * \brief Deprecated Pacemaker configuration utilities
 * \ingroup cib
 * \deprecated Do not include this header directly. The utilities in this
 *             header, and the header itself, will be removed in a future
 *             release.
 */

//! \deprecated Use pcmk_cib_xpath_for() instead
const char *get_object_path(const char *object_type);

#ifdef __cplusplus
}
#endif

#endif // PCMK__CRM_CIB_UTIL_COMPAT__H
