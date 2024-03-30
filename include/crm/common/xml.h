/*
 * Copyright 2004-2024 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU Lesser General Public License
 * version 2.1 or later (LGPLv2.1+) WITHOUT ANY WARRANTY.
 */

#ifndef PCMK__CRM_COMMON_XML__H
#  define PCMK__CRM_COMMON_XML__H


#  include <stdio.h>
#  include <sys/types.h>
#  include <unistd.h>

#  include <stdlib.h>
#  include <errno.h>
#  include <fcntl.h>

#  include <libxml/tree.h>
#  include <libxml/xpath.h>

#  include <crm/crm.h>
#  include <crm/common/nvpair.h>
#  include <crm/common/xml_io.h>
#  include <crm/common/xml_names.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \file
 * \brief Wrappers for and extensions to libxml2
 * \ingroup core
 */

typedef const xmlChar *pcmkXmlStr;

void fix_plus_plus_recursive(xmlNode * target);


/*
 * Searching & Modifying
 */
xmlNode *get_xpath_object(const char *xpath, xmlNode * xml_obj, int error_level);

char *calculate_on_disk_digest(xmlNode * local_cib);
char *calculate_operation_digest(xmlNode * local_cib, const char *version);
char *calculate_xml_versioned_digest(xmlNode * input, gboolean sort, gboolean do_filter,
                                     const char *version);

/* schema-related functions (from schemas.c) */
gboolean validate_xml(xmlNode * xml_blob, const char *validation, gboolean to_logs);
gboolean validate_xml_verbose(const xmlNode *xml_blob);

/*!
 * \brief Update CIB XML to most recent schema version
 *
 * "Update" means either actively employ XSLT-based transformation(s)
 * (if intermediate product to transform valid per its declared schema version,
 * transformation available, proceeded successfully with a result valid per
 * expectated newer schema version), or just try to bump the marked validating
 * schema until all gradually rising schema versions attested or the first
 * such attempt subsequently fails to validate.   Which of the two styles will
 * be used depends on \p transform parameter (positive/negative, respectively).
 *
 * \param[in,out] xml_blob   XML tree representing CIB, may be swapped with
 *                           an "updated" one
 * \param[out]    best       The highest configuration version (per its index
 *                           in the global schemas table) it was possible to
 *                           reach during the update steps while ensuring
 *                           the validity of the result; if no validation
 *                           success was observed against possibly multiple
 *                           schemas, the value is less or equal the result
 *                           of \c get_schema_version applied on the input
 *                           \p xml_blob value (unless that function maps it
 *                           to -1, then 0 would be used instead)
 * \param[in]     max        When \p transform is positive, this allows to
 *                           set upper boundary schema (per its index in the
 *                           global schemas table) beyond which it's forbidden
 *                           to update by the means of XSLT transformation
 * \param[in]     transform  Whether to employ XSLT-based transformation so
 *                           as to allow overcoming possible incompatibilities
 *                           between major schema versions (see above)
 * \param[in]     to_logs    If true, output notable progress info to
 *                           internal log streams; if false, to stderr
 *
 * \return \c pcmk_ok if no non-recoverable error encountered (up to
 *         caller to evaluate if the update satisfies the requirements
 *         per returned \p best value), negative value carrying the reason
 *         otherwise
 */
int update_validation(xmlNode **xml_blob, int *best, int max,
                      gboolean transform, gboolean to_logs);

int get_schema_version(const char *name);
const char *get_schema_name(int version);
const char *xml_latest_schema(void);
gboolean cli_config_update(xmlNode ** xml, int *best_version, gboolean to_logs);

xmlNode *sorted_xml(xmlNode * input, xmlNode * parent, gboolean recursive);
xmlXPathObjectPtr xpath_search(const xmlNode *xml_top, const char *path);
void crm_foreach_xpath_result(xmlNode *xml, const char *xpath,
                              void (*helper)(xmlNode*, void*), void *user_data);
xmlNode *expand_idref(xmlNode * input, xmlNode * top);

void freeXpathObject(xmlXPathObjectPtr xpathObj);
xmlNode *getXpathResult(xmlXPathObjectPtr xpathObj, int index);
void dedupXpathResults(xmlXPathObjectPtr xpathObj);

static inline int numXpathResults(xmlXPathObjectPtr xpathObj)
{
    if(xpathObj == NULL || xpathObj->nodesetval == NULL) {
        return 0;
    }
    return xpathObj->nodesetval->nodeNr;
}

bool xml_tracking_changes(xmlNode * xml);
bool xml_document_dirty(xmlNode *xml);
void xml_track_changes(xmlNode * xml, const char *user, xmlNode *acl_source, bool enforce_acls);
void xml_calculate_changes(xmlNode *old_xml, xmlNode *new_xml);
void xml_calculate_significant_changes(xmlNode *old_xml, xmlNode *new_xml);
void xml_accept_changes(xmlNode * xml);
bool xml_patch_versions(const xmlNode *patchset, int add[3], int del[3]);

xmlNode *xml_create_patchset(
    int format, xmlNode *source, xmlNode *target, bool *config, bool manage_version);
int xml_apply_patchset(xmlNode *xml, xmlNode *patchset, bool check_version);

void patchset_process_digest(xmlNode *patch, xmlNode *source, xmlNode *target, bool with_digest);

void crm_xml_sanitize_id(char *id);
void crm_xml_set_id(xmlNode *xml, const char *format, ...) G_GNUC_PRINTF(2, 3);

#if !defined(PCMK_ALLOW_DEPRECATED) || (PCMK_ALLOW_DEPRECATED == 1)
#include <crm/common/xml_compat.h>
#endif

#ifdef __cplusplus
}
#endif

#endif
