/*
 * Copyright 2004-2019 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU Lesser General Public License
 * version 2.1 or later (LGPLv2.1+) WITHOUT ANY WARRANTY.
 */

#include <crm_internal.h>

#include <stdio.h>
#include <sys/types.h>
#include <pwd.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>
#if HAVE_LIBXSLT
#  include <libxslt/transform.h>
#  include <libxslt/variables.h>
#  include <libxslt/xsltutils.h>
#endif

#include <crm/compatibility.h>  /* PCMK_COMPAT_ACL_2_* */
#include <crm/crm.h>
#include <crm/msg_xml.h>
#include <crm/common/xml.h>
#include <crm/common/xml_internal.h>
#include "crmcommon_private.h"

#define MAX_XPATH_LEN	4096

typedef struct xml_acl_s {
        enum xml_private_flags mode;
        char *xpath;
} xml_acl_t;

static void
__xml_acl_free(void *data)
{
    if (data) {
        xml_acl_t *acl = data;

        free(acl->xpath);
        free(acl);
    }
}

void
pcmk__free_acls(GList *acls)
{
    g_list_free_full(acls, __xml_acl_free);
}

static GList *
__xml_acl_create(xmlNode *xml, GList *acls, enum xml_private_flags mode)
{
    xml_acl_t *acl = NULL;

    const char *tag = crm_element_value(xml, XML_ACL_ATTR_TAG);
    const char *ref = crm_element_value(xml, XML_ACL_ATTR_REF);
    const char *xpath = crm_element_value(xml, XML_ACL_ATTR_XPATH);
    const char *attr = crm_element_value(xml, XML_ACL_ATTR_ATTRIBUTE);

    if (tag == NULL) {
        // @COMPAT rolling upgrades <=1.1.11
        tag = crm_element_value(xml, XML_ACL_ATTR_TAGv1);
    }
    if (ref == NULL) {
        // @COMPAT rolling upgrades <=1.1.11
        ref = crm_element_value(xml, XML_ACL_ATTR_REFv1);
    }

    if ((tag == NULL) && (ref == NULL) && (xpath == NULL)) {
        // Schema should prevent this, but to be safe ...
        crm_trace("Ignoring ACL <%s> element without selection criteria",
                  crm_element_name(xml));
        return NULL;
    }

    acl = calloc(1, sizeof (xml_acl_t));
    CRM_ASSERT(acl != NULL);

    acl->mode = mode;
    if (xpath) {
        acl->xpath = strdup(xpath);
        CRM_ASSERT(acl->xpath != NULL);
        crm_trace("Unpacked ACL <%s> element using xpath: %s",
                  crm_element_name(xml), acl->xpath);

    } else {
        int offset = 0;
        char buffer[MAX_XPATH_LEN];

        if (tag) {
            offset += snprintf(buffer + offset, MAX_XPATH_LEN - offset,
                               "//%s", tag);
        } else {
            offset += snprintf(buffer + offset, MAX_XPATH_LEN - offset,
                               "//*");
        }

        if (ref || attr) {
            offset += snprintf(buffer + offset, MAX_XPATH_LEN - offset,
                               "[");
        }

        if (ref) {
            offset += snprintf(buffer + offset, MAX_XPATH_LEN - offset,
                               "@id='%s'", ref);
        }

        // NOTE: schema currently does not allow this
        if (ref && attr) {
            offset += snprintf(buffer + offset, MAX_XPATH_LEN - offset,
                               " and ");
        }

        if (attr) {
            offset += snprintf(buffer + offset, MAX_XPATH_LEN - offset,
                               "@%s", attr);
        }

        if (ref || attr) {
            offset += snprintf(buffer + offset, MAX_XPATH_LEN - offset,
                               "]");
        }

        CRM_LOG_ASSERT(offset > 0);
        acl->xpath = strdup(buffer);
        CRM_ASSERT(acl->xpath != NULL);

        crm_trace("Unpacked ACL <%s> element as xpath: %s",
                  crm_element_name(xml), acl->xpath);
    }

    return g_list_append(acls, acl);
}

/*!
 * \internal
 * \brief Unpack a user, group, or role subtree of the ACLs section
 *
 * \param[in]     acl_top    XML of entire ACLs section
 * \param[in]     acl_entry  XML of ACL element being unpacked
 * \param[in,out] acls       List of ACLs unpacked so far
 *
 * \return New head of (possibly modified) acls
 */
static GList *
__xml_acl_parse_entry(xmlNode *acl_top, xmlNode *acl_entry, GList *acls)
{
    xmlNode *child = NULL;

    for (child = __xml_first_child_element(acl_entry); child;
         child = __xml_next_element(child)) {
        const char *tag = crm_element_name(child);
        const char *kind = crm_element_value(child, XML_ACL_ATTR_KIND);

        if (strcmp(XML_ACL_TAG_PERMISSION, tag) == 0){
            CRM_ASSERT(kind != NULL);
            crm_trace("Unpacking ACL <%s> element of kind '%s'", tag, kind);
            tag = kind;
        } else {
            crm_trace("Unpacking ACL <%s> element", tag);
        }

        if (strcmp(XML_ACL_TAG_ROLE_REF, tag) == 0
                   || strcmp(XML_ACL_TAG_ROLE_REFv1, tag) == 0) {
            const char *ref_role = crm_element_value(child, XML_ATTR_ID);

            if (ref_role) {
                xmlNode *role = NULL;

                for (role = __xml_first_child_element(acl_top); role;
                     role = __xml_next_element(role)) {
                    if (!strcmp(XML_ACL_TAG_ROLE, (const char *) role->name)) {
                        const char *role_id = crm_element_value(role,
                                                                XML_ATTR_ID);

                        if (role_id && strcmp(ref_role, role_id) == 0) {
                            crm_trace("Unpacking referenced role '%s' in ACL <%s> element",
                                      role_id, crm_element_name(acl_entry));
                            acls = __xml_acl_parse_entry(acl_top, role, acls);
                            break;
                        }
                    }
                }
            }

        } else if (strcmp(XML_ACL_TAG_READ, tag) == 0) {
            acls = __xml_acl_create(child, acls, xpf_acl_read);

        } else if (strcmp(XML_ACL_TAG_WRITE, tag) == 0) {
            acls = __xml_acl_create(child, acls, xpf_acl_write);

        } else if (strcmp(XML_ACL_TAG_DENY, tag) == 0) {
            acls = __xml_acl_create(child, acls, xpf_acl_deny);

        } else {
            crm_warn("Ignoring unknown ACL %s '%s'",
                     (kind? "kind" : "element"), tag);
        }
    }

    return acls;
}

/*
    <acls>
      <acl_target id="l33t-haxor"><role id="auto-l33t-haxor"/></acl_target>
      <acl_role id="auto-l33t-haxor">
        <acl_permission id="crook-nothing" kind="deny" xpath="/cib"/>
      </acl_role>
      <acl_target id="niceguy">
        <role id="observer"/>
      </acl_target>
      <acl_role id="observer">
        <acl_permission id="observer-read-1" kind="read" xpath="/cib"/>
        <acl_permission id="observer-write-1" kind="write" xpath="//nvpair[@name='stonith-enabled']"/>
        <acl_permission id="observer-write-2" kind="write" xpath="//nvpair[@name='target-role']"/>
      </acl_role>
      <acl_target id="badidea"><role id="auto-badidea"/></acl_target>
      <acl_role id="auto-badidea">
        <acl_permission id="badidea-resources" kind="read" xpath="//meta_attributes"/>
        <acl_permission id="badidea-resources-2" kind="deny" reference="dummy-meta_attributes"/>
      </acl_role>
    </acls>
*/

static const char *
__xml_acl_to_text(enum xml_private_flags flags)
{
    if (is_set(flags, xpf_acl_deny)) {
        return "deny";

    } else if (is_set(flags, xpf_acl_write) || is_set(flags, xpf_acl_create)) {
        return "read/write";

    } else if (is_set(flags, xpf_acl_read)) {
        return "read";
    }
    return "none";
}

void
pcmk__apply_acl(xmlNode *xml)
{
    GListPtr aIter = NULL;
    xmlXPathObjectPtr xpathObj = NULL;
    xml_doc_private_t *docpriv = xml->doc->_private;
    xml_node_private_t *nodepriv;

    if (xml_acl_enabled(xml) == FALSE) {
        crm_trace("Skipping ACLs for user '%s' because not enabled for this XML",
                  docpriv->user);
        return;
    }

    for (aIter = docpriv->acls; aIter != NULL; aIter = aIter->next) {
        int max = 0, lpc = 0;
        xml_acl_t *acl = aIter->data;

        xpathObj = xpath_search(xml, acl->xpath);
        max = numXpathResults(xpathObj);

        for (lpc = 0; lpc < max; lpc++) {
            xmlNode *match = getXpathResult(xpathObj, lpc);
            char *path = xml_get_path(match);

            nodepriv = match->_private;
            crm_trace("Applying %s ACL to %s matched by %s",
                      __xml_acl_to_text(acl->mode), path, acl->xpath);

#ifdef SUSE_ACL_COMPAT
            if (is_not_set(nodepriv->flags, acl->mode)
                && (is_set(nodepriv->flags, xpf_acl_read)
                    || is_set(nodepriv->flags, xpf_acl_write)
                    || is_set(nodepriv->flags, xpf_acl_deny))) {
                crm_config_warn("Configuration element %s is matched by "
                                "multiple ACL rules, only the first applies "
                                "('%s' wins over '%s')",
                                path, __xml_acl_to_text(nodepriv->flags),
                                __xml_acl_to_text(acl->mode));
                free(path);
                continue;
            }
#endif
            nodepriv->flags |= acl->mode;
            free(path);
        }
        crm_trace("Applied %s ACL %s (%d match%s)",
                  __xml_acl_to_text(acl->mode), acl->xpath, max,
                  ((max == 1)? "" : "es"));
        freeXpathObject(xpathObj);
    }

    nodepriv = xml->_private;
    if (is_not_set(nodepriv->flags, xpf_acl_read)
        && is_not_set(nodepriv->flags, xpf_acl_write)) {

        nodepriv->flags |= xpf_acl_deny;
        docpriv = xml->doc->_private;
        crm_info("Applied default deny ACL for user '%s' to <%s>",
                 docpriv->user, crm_element_name(xml));
    }

}

/*!
 * \internal
 * \brief Unpack ACLs for a given user
 *
 * \param[in]     source  XML with ACL definitions
 * \param[in,out] target  XML that ACLs will be applied to
 * \param[in]     user    Username whose ACLs need to be unpacked
 */
void
pcmk__unpack_acl(xmlNode *source, xmlNode *target, const char *user)
{
#if ENABLE_ACL
    xml_doc_private_t *docpriv = NULL;

    if ((target == NULL) || (target->doc == NULL)
        || (target->doc->_private == NULL)) {
        return;
    }

    docpriv = target->doc->_private;
    if (pcmk_acl_required(user) == FALSE) {
        crm_trace("Not unpacking ACLs because not required for user '%s'",
                  user);

    } else if (docpriv->acls == NULL) {
        xmlNode *acls = get_xpath_object("//" XML_CIB_TAG_ACLS,
                                         source, LOG_TRACE);

        free(docpriv->user);
        docpriv->user = strdup(user);

        if (acls) {
            xmlNode *child = NULL;

            for (child = __xml_first_child_element(acls); child;
                 child = __xml_next_element(child)) {
                const char *tag = crm_element_name(child);

                if (!strcmp(tag, XML_ACL_TAG_USER)
                    || !strcmp(tag, XML_ACL_TAG_USERv1)) {
                    const char *id = crm_element_value(child, XML_ATTR_ID);

                    if (id && strcmp(id, user) == 0) {
                        crm_debug("Unpacking ACLs for user '%s'", id);
                        docpriv->acls = __xml_acl_parse_entry(acls, child,
                                                              docpriv->acls);
                    }
                }
            }
        }
    }
#endif
}

static inline bool
__xml_acl_mode_test(enum xml_private_flags allowed,
                    enum xml_private_flags requested)
{
    if (is_set(allowed, xpf_acl_deny)) {
        return FALSE;

    } else if (is_set(allowed, requested)) {
        return TRUE;

    } else if (is_set(requested, xpf_acl_read)
               && is_set(allowed, xpf_acl_write)) {
        return TRUE;

    } else if (is_set(requested, xpf_acl_create)
               && is_set(allowed, xpf_acl_write)) {
        return TRUE;

    } else if (is_set(requested, xpf_acl_create)
               && is_set(allowed, xpf_created)) {
        return TRUE;
    }
    return FALSE;
}

/* rc = TRUE if orig_cib has been filtered
 * That means '*result' rather than 'xml' should be exploited afterwards
 */
static bool
__xml_purge_attributes(xmlNode *xml)
{
    xmlNode *child = NULL;
    xmlAttr *xIter = NULL;
    bool readable_children = FALSE;
    xml_node_private_t *nodepriv = xml->_private;

    if (__xml_acl_mode_test(nodepriv->flags, xpf_acl_read)) {
        crm_trace("%s[@id=%s] is readable", crm_element_name(xml), ID(xml));
        return TRUE;
    }

    xIter = xml->properties;
    while (xIter != NULL) {
        xmlAttr *tmp = xIter;
        const char *prop_name = (const char *)xIter->name;

        xIter = xIter->next;
        if (strcmp(prop_name, XML_ATTR_ID) == 0) {
            continue;
        }

        xmlUnsetProp(xml, tmp->name);
    }

    child = __xml_first_child(xml);
    while ( child != NULL ) {
        xmlNode *tmp = child;

        child = __xml_next(child);
        readable_children |= __xml_purge_attributes(tmp);
    }

    if (readable_children == FALSE) {
        free_xml(xml); /* Nothing readable under here, purge completely */
    }
    return readable_children;
}

/*!
 * \internal
 * \brief Copy ACL-allowed portions of specified XML
 *
 * \param[in]  user        Username whose ACLs should be used
 * \param[in]  acl_source  XML containing ACLs
 * \param[in]  xml         XML to be copied
 * \param[out] result      Copy of XML portions readable via ACLs
 *
 * \return TRUE if xml exists and ACLs are required for user, otherwise FALSE
 */
bool
xml_acl_filtered_copy(const char *user, xmlNode *acl_source, xmlNode *xml,
                      xmlNode **result)
{
    GListPtr aIter = NULL;
    xmlNode *target = NULL;
    xml_node_private_t *nodepriv = NULL;
    xml_doc_private_t *docpriv = NULL;

    *result = NULL;
    if (xml == NULL || pcmk_acl_required(user) == FALSE) {
        crm_trace("Not filtering XML because ACLs not required for user '%s'",
                  user);
        return FALSE;
    }

    crm_trace("Filtering XML copy using user '%s' ACLs", user);
    target = copy_xml(xml);
    if (target == NULL) {
        return TRUE;
    }

    pcmk__unpack_acl(acl_source, target, user);
    pcmk__set_xml_flag(target, xpf_acl_enabled);
    pcmk__apply_acl(target);

    docpriv = target->doc->_private;
    for(aIter = docpriv->acls; aIter != NULL && target; aIter = aIter->next) {
        int max = 0;
        xml_acl_t *acl = aIter->data;

        if (acl->mode != xpf_acl_deny) {
            /* Nothing to do */

        } else if (acl->xpath) {
            int lpc = 0;
            xmlXPathObjectPtr xpathObj = xpath_search(target, acl->xpath);

            max = numXpathResults(xpathObj);
            for(lpc = 0; lpc < max; lpc++) {
                xmlNode *match = getXpathResult(xpathObj, lpc);

                if (__xml_purge_attributes(match) == FALSE && match == target) {
                    crm_trace("ACLs deny user '%s' access to entire XML document",
                              user);
                    freeXpathObject(xpathObj);
                    return TRUE;
                }
            }
            crm_trace("ACLs deny user '%s' access to %s (%d match%s)",
                      user, acl->xpath, max, ((max == 1)? "" : "es"));
            freeXpathObject(xpathObj);
        }
    }

    nodepriv = target->_private;
    if (is_set(nodepriv->flags, xpf_acl_deny)
        && (__xml_purge_attributes(target) == FALSE)) {
        crm_trace("ACLs deny user '%s' access to entire XML document", user);
        return TRUE;
    }

    if (docpriv->acls) {
        g_list_free_full(docpriv->acls, __xml_acl_free);
        docpriv->acls = NULL;

    } else {
        crm_trace("User '%s' without ACLs denied access to entire XML document",
                  user);
        free_xml(target);
        target = NULL;
    }

    if (target) {
        *result = target;
    }

    return TRUE;
}

/*!
 * \internal
 * \brief Check whether creation of an XML element is implicitly allowed
 *
 * Check whether XML is a "scaffolding" element whose creation is implicitly
 * allowed regardless of ACLs (that is, it is not in the ACL section and has
 * no attributes other than "id").
 *
 * \param[in] xml  XML element to check
 *
 * \return TRUE if XML element is implicitly allowed, FALSE otherwise
 */
static bool
implicitly_allowed(xmlNode *xml)
{
    char *path = NULL;

    for (xmlAttr *prop = xml->properties; prop != NULL; prop = prop->next) {
        if (strcmp((const char *) prop->name, XML_ATTR_ID) != 0) {
            return FALSE;
        }
    }

    path = xml_get_path(xml);
    if (strstr(path, "/" XML_CIB_TAG_ACLS "/") != NULL) {
        free(path);
        return FALSE;
    }
    free(path);

    return TRUE;
}

#define display_id(xml) (ID(xml)? ID(xml) : "<unset>")

/*!
 * \internal
 * \brief Drop XML nodes created in violation of ACLs
 *
 * Given an XML element, free all of its descendent nodes created in violation
 * of ACLs, with the exception of allowing "scaffolding" elements (i.e. those
 * that aren't in the ACL section and don't have any attributes other than
 * "id").
 *
 * \param[in,out] xml        XML to check
 * \param[in]     check_top  Whether to apply checks to argument itself
 *                           (if TRUE, xml might get freed)
 */
void
pcmk__apply_creation_acl(xmlNode *xml, bool check_top)
{
    xml_node_private_t *nodepriv = xml->_private;

    if (is_set(nodepriv->flags, xpf_created)) {
        if (implicitly_allowed(xml)) {
            crm_trace("Creation of <%s> scaffolding with id=\"%s\""
                      " is implicitly allowed",
                      crm_element_name(xml), display_id(xml));

        } else if (pcmk__check_acl(xml, NULL, xpf_acl_write)) {
            crm_trace("ACLs allow creation of <%s> with id=\"%s\"",
                      crm_element_name(xml), display_id(xml));

        } else if (check_top) {
            crm_trace("ACLs disallow creation of <%s> with id=\"%s\"",
                      crm_element_name(xml), display_id(xml));
            pcmk_free_xml_subtree(xml);
            return;

        } else {
            crm_notice("ACLs would disallow creation of %s<%s> with id=\"%s\" ",
                       ((xml == xmlDocGetRootElement(xml->doc))? "root element " : ""),
                       crm_element_name(xml), display_id(xml));
        }
    }

    for (xmlNode *cIter = __xml_first_child(xml); cIter != NULL; ) {
        xmlNode *child = cIter;
        cIter = __xml_next(cIter); /* In case it is free'd */
        pcmk__apply_creation_acl(child, TRUE);
    }
}

bool
xml_acl_denied(xmlNode *xml)
{
    if (xml && xml->doc && xml->doc->_private){
        xml_doc_private_t *docpriv = xml->doc->_private;

        return is_set(docpriv->flags, xpf_acl_denied);
    }
    return FALSE;
}

void
xml_acl_disable(xmlNode *xml)
{
    if (xml_acl_enabled(xml)) {
        xml_doc_private_t *docpriv = xml->doc->_private;

        /* Catch anything that was created but shouldn't have been */
        pcmk__apply_acl(xml);
        pcmk__apply_creation_acl(xml, FALSE);
        clear_bit(docpriv->flags, xpf_acl_enabled);
    }
}

bool
xml_acl_enabled(xmlNode *xml)
{
    if (xml && xml->doc && xml->doc->_private) {
        xml_doc_private_t *docpriv = xml->doc->_private;

        return is_set(docpriv->flags, xpf_acl_enabled);
    }
    return FALSE;
}

bool
pcmk__check_acl(xmlNode *xml, const char *name, enum xml_private_flags mode)
{
    CRM_ASSERT(xml);
    CRM_ASSERT(xml->doc);
    CRM_ASSERT(xml->doc->_private);

#if ENABLE_ACL
    if (pcmk__tracking_xml_changes(xml, FALSE) && xml_acl_enabled(xml)) {
        int offset = 0;
        xmlNode *parent = xml;
        char buffer[MAX_XPATH_LEN];
        xml_doc_private_t *docpriv = xml->doc->_private;

        offset = pcmk__element_xpath(NULL, xml, buffer, offset,
                                     sizeof(buffer));
        if (name) {
            offset += snprintf(buffer + offset, MAX_XPATH_LEN - offset,
                               "[@%s]", name);
        }
        CRM_LOG_ASSERT(offset > 0);

        if (docpriv->acls == NULL) {
            crm_trace("User '%s' without ACLs denied %s access to %s",
                      docpriv->user, __xml_acl_to_text(mode), buffer);
            pcmk__set_xml_flag(xml, xpf_acl_denied);
            return FALSE;
        }

        /* Walk the tree upwards looking for xml_acl_* flags
         * - Creating an attribute requires write permissions for the node
         * - Creating a child requires write permissions for the parent
         */

        if (name) {
            xmlAttr *attr = xmlHasProp(xml, (pcmkXmlStr) name);

            if (attr && mode == xpf_acl_create) {
                mode = xpf_acl_write;
            }
        }

        while (parent && parent->_private) {
            xml_node_private_t *nodepriv = parent->_private;
            if (__xml_acl_mode_test(nodepriv->flags, mode)) {
                return TRUE;

            } else if (is_set(nodepriv->flags, xpf_acl_deny)) {
                crm_trace("Parent ACL denies user '%s' %s access to %s",
                          docpriv->user, __xml_acl_to_text(mode), buffer);
                pcmk__set_xml_flag(xml, xpf_acl_denied);
                return FALSE;
            }
            parent = parent->parent;
        }

        crm_trace("Default ACL denies user '%s' %s access to %s",
                  docpriv->user, __xml_acl_to_text(mode), buffer);
        pcmk__set_xml_flag(xml, xpf_acl_denied);
        return FALSE;
    }
#endif

    return TRUE;
}

bool
pcmk_acl_required(const char *user)
{
#if ENABLE_ACL
    if ((user == NULL) || (*user == '\0')) {
        crm_trace("ACLs not required because no user set");
        return FALSE;

    } else if (!strcmp(user, CRM_DAEMON_USER) || !strcmp(user, "root")) {
        crm_trace("ACLs not required for privileged user %s", user);
        return FALSE;
    }
    crm_trace("ACLs required for %s", user);
    return TRUE;
#else
    crm_trace("ACLs not required because not supported by this build");
    return FALSE;
#endif
}

#if ENABLE_ACL
char *
uid2username(uid_t uid)
{
    struct passwd *pwent = getpwuid(uid);

    if (pwent == NULL) {
        crm_perror(LOG_INFO, "Cannot get user details for user ID %d", uid);
        return NULL;
    }
    return strdup(pwent->pw_name);
}

const char *
crm_acl_get_set_user(xmlNode *request, const char *field, const char *peer_user)
{
    static const char *effective_user = NULL;
    const char *requested_user = NULL;
    const char *user = NULL;

    if (effective_user == NULL) {
        effective_user = uid2username(geteuid());
        if (effective_user == NULL) {
            effective_user = strdup("#unprivileged");
            CRM_CHECK(effective_user != NULL, return NULL);
            crm_err("Unable to determine effective user, assuming unprivileged for ACLs");
        }
    }

    requested_user = crm_element_value(request, XML_ACL_TAG_USER);
    if (requested_user == NULL) {
        /* @COMPAT rolling upgrades <=1.1.11
         *
         * field is checked for backward compatibility with older versions that
         * did not use XML_ACL_TAG_USER.
         */
        requested_user = crm_element_value(request, field);
    }

    if (is_privileged(effective_user) == FALSE) {
        /* We're not running as a privileged user, set or overwrite any existing
         * value for $XML_ACL_TAG_USER
         */
        user = effective_user;

    } else if (peer_user == NULL && requested_user == NULL) {
        /* No user known or requested, use 'effective_user' and make sure one is
         * set for the request
         */
        user = effective_user;

    } else if (peer_user == NULL) {
        /* No user known, trusting 'requested_user' */
        user = requested_user;

    } else if (is_privileged(peer_user) == FALSE) {
        /* The peer is not a privileged user, set or overwrite any existing
         * value for $XML_ACL_TAG_USER
         */
        user = peer_user;

    } else if (requested_user == NULL) {
        /* Even if we're privileged, make sure there is always a value set */
        user = peer_user;

    } else {
        /* Legal delegation to 'requested_user' */
        user = requested_user;
    }

    // This requires pointer comparison, not string comparison
    if (user != crm_element_value(request, XML_ACL_TAG_USER)) {
        crm_xml_add(request, XML_ACL_TAG_USER, user);
    }

    if (field != NULL && user != crm_element_value(request, field)) {
        crm_xml_add(request, field, user);
    }

    return requested_user;
}

#define ACL_NS_PREFIX "http://clusterlabs.org/ns/pacemaker/access/"
#define ACL_NS_Q_PREFIX  "pcmk-access-"
#define ACL_NS_Q_WRITABLE (const xmlChar *) ACL_NS_Q_PREFIX   "writable"
#define ACL_NS_Q_READABLE (const xmlChar *) ACL_NS_Q_PREFIX   "readable"
#define ACL_NS_Q_DENIED   (const xmlChar *) ACL_NS_Q_PREFIX   "denied"

static const xmlChar *NS_WRITABLE = (xmlChar *) ACL_NS_PREFIX "writable";
static const xmlChar *NS_READABLE = (xmlChar *) ACL_NS_PREFIX "readable";
static const xmlChar *NS_DENIED =   (xmlChar *) ACL_NS_PREFIX "denied";

static int
pcmk__eval_acl_as_namespaces_2(xmlNode *xml_modify)
{

    static xmlNs *ns_recycle_writable = NULL,
                 *ns_recycle_readable = NULL,
                 *ns_recycle_denied = NULL;
    static const xmlDoc *prev_doc = NULL;

    xmlNode *i_node = NULL;
    const xmlChar *ns;
    int ret = 0;

    if (prev_doc == NULL || prev_doc != xml_modify->doc) {
        prev_doc = xml_modify->doc;
        ns_recycle_writable = ns_recycle_readable = ns_recycle_denied = NULL;
    }

    for (i_node = xml_modify; i_node != NULL; i_node = i_node->next) {
        switch (i_node->type) {
        case XML_ELEMENT_NODE:
            pcmk__set_xml_flag(i_node, xpf_tracking);
            ns = !pcmk__check_acl(i_node, NULL, xpf_acl_read)
                 ? NS_DENIED
                 : !pcmk__check_acl(i_node, NULL, xpf_acl_write)
                   ? NS_READABLE
                   : NS_WRITABLE;
            if (ns == NS_WRITABLE) {
                if (ns_recycle_writable == NULL) {
                    ns_recycle_writable = xmlNewNs(xmlDocGetRootElement(i_node->doc),
                                                   NS_WRITABLE, ACL_NS_Q_WRITABLE);
                    ret |= PCMK_ACL_VERDICT_WRITABLE;
                }
                xmlSetNs(i_node, ns_recycle_writable);
            } else if (ns == NS_READABLE) {
                if (ns_recycle_readable == NULL) {
                    ns_recycle_readable = xmlNewNs(xmlDocGetRootElement(i_node->doc),
                                                   NS_READABLE, ACL_NS_Q_READABLE);
                    ret |= PCMK_ACL_VERDICT_READABLE;
                }
                xmlSetNs(i_node, ns_recycle_readable);
            } else if (ns == NS_DENIED) {
                if (ns_recycle_denied == NULL) {
                    ns_recycle_denied = xmlNewNs(xmlDocGetRootElement(i_node->doc),
                                                 NS_DENIED, ACL_NS_Q_DENIED);
                    ret |= PCMK_ACL_VERDICT_DENIED;
                };
                xmlSetNs(i_node, ns_recycle_denied);
            }
            /* XXX recursion can be turned into plain iteration to save stack */
            if (i_node->properties != NULL) {
                /* this is not entirely clear, but relies on the very same
                   class-hierarchy emulation that libxml2 has firmly baked in
                   its API/ABI */
                ret |= pcmk__eval_acl_as_namespaces_2((xmlNodePtr) i_node->properties);
            }
            if (i_node->children != NULL) {
                ret |= pcmk__eval_acl_as_namespaces_2(i_node->children);
            }
            break;
        case XML_ATTRIBUTE_NODE:
            /* we can utilize that parent has already been assigned the ns */
            ns = !pcmk__check_acl(i_node->parent,
                                 (const char *) i_node->name,
                                 xpf_acl_read)
                 ? NS_DENIED
                 : !pcmk__check_acl(i_node,
                                   (const char *) i_node->name,
                                   xpf_acl_write)
                   ? NS_READABLE
                   : NS_WRITABLE;
            if (ns == NS_WRITABLE) {
                if (ns_recycle_writable == NULL) {
                    ns_recycle_writable = xmlNewNs(xmlDocGetRootElement(i_node->doc),
                                                   NS_WRITABLE, ACL_NS_Q_WRITABLE);
                    ret |= PCMK_ACL_VERDICT_WRITABLE;
                }
                xmlSetNs(i_node, ns_recycle_writable);
            } else if (ns == NS_READABLE) {
                if (ns_recycle_readable == NULL) {
                    ns_recycle_readable = xmlNewNs(xmlDocGetRootElement(i_node->doc),
                                                   NS_READABLE, ACL_NS_Q_READABLE);
                    ret |= PCMK_ACL_VERDICT_READABLE;
                }
                xmlSetNs(i_node, ns_recycle_readable);
            } else if (ns == NS_DENIED) {
                if (ns_recycle_denied == NULL) {
                    ns_recycle_denied = xmlNewNs(xmlDocGetRootElement(i_node->doc),
                                                 NS_DENIED, ACL_NS_Q_DENIED);
                    ret |= PCMK_ACL_VERDICT_DENIED;
                }
                xmlSetNs(i_node, ns_recycle_denied);
            }
            break;
        default:
            break;
        }
    }

    return ret;
}

int
pcmk_acl_evaled_as_namespaces(enum pcmk_acl_cred_type cred_type,
                              const char *cred, xmlDoc *cib_doc,
                              xmlDoc **acl_evaled_doc)
{
    /* XXX requires prior crm_xml_init */

    int ret, version;
    xmlNode *target, *comment;
    char comment_buf[256] = "access as evaluated for user ";
    const char *validation;

    CRM_CHECK(cred != NULL, return -1);
    CRM_CHECK(cib_doc != NULL, return -1);
    CRM_CHECK(acl_evaled_doc != NULL, return -1);
    /* XXX no proper support for groups yet */
    CRM_CHECK(cred_type == PCMK_ACL_CRED_USER, return -2);

    if (!pcmk_acl_required(cred)) {
        /* nothing to evaluate */
        return 0;
    }

    /* XXX see the comment for this function, pacemaker-4.0 may need
           updating respectively in the future */
    validation = crm_element_value(xmlDocGetRootElement(cib_doc),
                                   XML_ATTR_VALIDATION);
    version = get_schema_version(validation);
    if (get_schema_version(PCMK_COMPAT_ACL_2_MIN_INCL) > version
            || (-1 != get_schema_version(PCMK_COMPAT_ACL_2_MAX_EXCL)
                    && get_schema_version(PCMK_COMPAT_ACL_2_MAX_EXCL)
                        <= version)) {
        return -3;
    }

    target = copy_xml(xmlDocGetRootElement(cib_doc));
    if (target == NULL) {
        return -1;
    }

    pcmk__unpack_acl(target, target, cred);
    pcmk__set_xml_flag(target, xpf_acl_enabled);
    pcmk__apply_acl(target);
    ret = pcmk__eval_acl_as_namespaces_2(target);  /* XXX may need "switch" */

    if (ret > 0) {
        /* avoid trivial accidental XML injection */
        if (strpbrk(cred, "<>&") == NULL) {
            snprintf(comment_buf + strlen(comment_buf),
                     sizeof(comment_buf) - strlen(comment_buf), "%s", cred);
            comment = xmlNewDocComment(target->doc, (pcmkXmlStr) comment_buf);
            if (comment == NULL) {
                xmlFreeNode(target);
                return -1;
            }
            xmlAddPrevSibling(xmlDocGetRootElement(target->doc), comment);
        }
        *acl_evaled_doc = target->doc;
    } else {
        xmlFreeNode(target);
    }
    return ret;
}

/* this is used to dynamically adapt to user-modified stylesheet */
static const char **
parse_params(xmlDoc *doc, const char **fallback)
{
    xmlXPathContext *xpath_ctxt;
    xmlXPathObject *xpath_obj;
    const char **ret = NULL;
    size_t ret_cnt = 0, ret_iter = 0;

    if (doc == NULL) {
        return fallback;
    }

    xpath_ctxt = xmlXPathNewContext(doc);
    CRM_ASSERT(xpath_ctxt != NULL);

    if (xmlXPathRegisterNs(xpath_ctxt, (pcmkXmlStr) "xsl",
                           (pcmkXmlStr) "http://www.w3.org/1999/XSL/Transform") != 0) {
        return fallback;
    }

    while (*fallback != NULL) {
        char xpath_query[1024];
        const char *key = *fallback++;
        const char *value = *fallback++;
        CRM_ASSERT(value != NULL);

        if (ret_iter + 1 >= ret_cnt) {
            ret_cnt = ret_cnt ? ret_cnt : 1;
            ret_cnt *= 2;
            ret_cnt += 1;
            ret = realloc(ret, ret_cnt * sizeof(*ret));
            CRM_ASSERT(ret != NULL);
        }

        key = strdup(key);
        CRM_ASSERT(key != NULL);
        ret[ret_iter++] = key;

        snprintf(xpath_query, sizeof(xpath_query),
                 "substring("
                   "/xsl:stylesheet/xsl:param[@name = '%s']/xsl:value-of/@select,"
                   "2,"
                   "string-length(/xsl:stylesheet/xsl:param[@name = '%s']/xsl:value-of/@select) - 2"
                 ")",
                 key, key);
        xpath_obj = xmlXPathEvalExpression((pcmkXmlStr) xpath_query, xpath_ctxt);
        if (xpath_obj != NULL && xpath_obj->type == XPATH_STRING
                && *xpath_obj->stringval != '\0') {
            /* XXX convert first! */
            char *origval = strdup((const char *) xpath_obj->stringval);
            size_t reminder = strlen(origval) + 1;
            xmlXPathFreeObject(xpath_obj);
            value = origval;
            /* reconcile "\x1b" (3 chars) -> '\x1b' (single char) */
            while ((origval = strstr(origval, "\\x1b")) != NULL) {
                origval[0] = '\x1b';
                memmove(origval + 1, origval + (sizeof("\\x1b") - 1),
                        (reminder -= (sizeof("\\x1b") - 1)));
            }
        } else {
            value = strdup(value);
        }
        CRM_ASSERT(value != NULL);
        ret[ret_iter++] = value;
    }
    ret[ret_iter] = NULL;

    return ret;
}

int
pcmk__acl_evaled_render(xmlDoc *annotated_doc, enum pcmk__acl_render_how how,
                        xmlChar **doc_txt_ptr, int *doc_txt_len)
{
#if HAVE_LIBXSLT
    xmlDoc *xslt_doc;
    xsltStylesheet *xslt;
    xsltTransformContext *xslt_ctxt;
    xmlDoc *res;
    char *sfile;
    static const char *params_ns_simple[] = {
        "accessrendercfg:c-writable",           ACL_NS_Q_PREFIX "writable:",
        "accessrendercfg:c-readable",           ACL_NS_Q_PREFIX "readable:",
        "accessrendercfg:c-denied",             ACL_NS_Q_PREFIX "denied:",
        "accessrendercfg:c-reset",              "",
        "accessrender:extra-spacing",           "no",
        "accessrender:self-reproducing-prefix", ACL_NS_Q_PREFIX,
        NULL
    }, *params_useansi[] = {
        /* start with hard-coded defaults, then adapt per the template ones */
        "accessrendercfg:c-writable",           "\x1b[32m",
        "accessrendercfg:c-readable",           "\x1b[34m",
        "accessrendercfg:c-denied",             "\x1b[31m",
        "accessrendercfg:c-reset",              "\x1b[0m",
        "accessrender:extra-spacing",           "no",
        "accessrender:self-reproducing-prefix", ACL_NS_Q_PREFIX,
        NULL
    }, *params_noansi[] = {
        "accessrendercfg:c-writable",           "vvv---[ WRITABLE ]---vvv",
        "accessrendercfg:c-readable",           "vvv---[ READABLE ]---vvv",
        "accessrendercfg:c-denied",             "vvv---[ ~DENIED~ ]---vvv",
        "accessrendercfg:c-reset",              "",
        "accessrender:extra-spacing",           "yes",
        "accessrender:self-reproducing-prefix", "",
        NULL
    };
    const char **params;
    int ret;
    xmlParserCtxtPtr parser_ctxt;

    /* unfortunately, the input (coming from CIB originally) was parsed with
       blanks ignored, and since the output is a conversion of XML to text
       format (we would be covered otherwise thanks to implicit
       pretty-printing), we need to dump the tree to string output first,
       only to subsequently reparse it -- this time with blanks honoured */
    xmlChar *annotated_dump;
    int dump_size;
    xmlDocDumpFormatMemory(annotated_doc, &annotated_dump, &dump_size, 1);
    res = xmlReadDoc(annotated_dump, "on-the-fly-access-render", NULL,
                     XML_PARSE_NONET);
    CRM_ASSERT(res != NULL);
    xmlFree(annotated_dump);
    xmlFreeDoc(annotated_doc);
    annotated_doc = res;

    sfile = pcmk__xml_artefact_path(pcmk__xml_artefact_ns_base_xslt,
                                    "access-render-2");
    parser_ctxt = xmlNewParserCtxt();

    CRM_ASSERT(sfile != NULL);
    CRM_ASSERT(parser_ctxt != NULL);

    xslt_doc = xmlCtxtReadFile(parser_ctxt, sfile, NULL, XML_PARSE_NONET);

    xslt = xsltParseStylesheetDoc(xslt_doc);  /* acquires xslt_doc! */
    if (xslt == NULL) {
        crm_crit("Problem in parsing %s", sfile);
        return -1;
    }
    free(sfile);
    sfile = NULL;
    xmlFreeParserCtxt(parser_ctxt);

    xslt_ctxt = xsltNewTransformContext(xslt, annotated_doc);
    CRM_ASSERT(xslt_ctxt != NULL);

    params = (how == pcmk__acl_render_ns_simple)
             ? params_ns_simple
             : (how == pcmk__acl_render_text)
             ? params_noansi
             : parse_params(xslt_doc, params_useansi);

    xsltQuoteUserParams(xslt_ctxt, params);

    res = xsltApplyStylesheetUser(xslt, annotated_doc, NULL,
                                  NULL, NULL, xslt_ctxt);

    xmlFreeDoc(annotated_doc);
    annotated_doc = NULL;
    xsltFreeTransformContext(xslt_ctxt);
    xslt_ctxt = NULL;

    if (how == pcmk__acl_render_color && params != params_useansi) {
        char **param_i = (char **) params;
        do {
            free(*param_i);
        } while (*param_i++ != NULL);
        free(params);
    }

    if (res == NULL) {
        ret = -1;
    } else {
        ret = xsltSaveResultToString(doc_txt_ptr, doc_txt_len, res, xslt);
        xmlFreeDoc(res);
    }
    xsltFreeStylesheet(xslt);
    return ret;
#else
    return -1;
#endif
}

#endif
