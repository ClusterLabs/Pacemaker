/*
 * Copyright 2004-2019 Andrew Beekhof <andrew@beekhof.net>
 *
 * This source code is licensed under the GNU Lesser General Public License
 * version 2.1 or later (LGPLv2.1+) WITHOUT ANY WARRANTY.
 */

#ifndef CRM_COMMON_NVPAIR__H
#  define CRM_COMMON_NVPAIR__H

#  ifdef __cplusplus
extern "C" {
#  endif

/**
 * \file
 * \brief Functionality for manipulating name/value pairs
 * \ingroup core
 */

#  include <sys/time.h>     // struct timeval
#  include <glib.h>         // gpointer, gboolean, guint
#  include <libxml/tree.h>  // xmlNode
#  include <crm/crm.h>

/*
 *  nvpairs (list of nvpair objects) encapsulating datatype
 *  + basic public operations
 */

/* name that wouldn't need to be exposed if -fms-extensions CFLAG was a norm */
#define pcmk_nvpairs_s _GSList
struct pcmk_nvpairs_s;

/*!
 * \brief Opaque encapsulation of a list of name/value pairs
 */
typedef struct pcmk_nvpairs_s pcmk_nvpairs_t;

/*!
 * \brief Prepend a name/value pair to (a list of) nvpairs
 *
 * \param[in,out] nvpairs  List to modify
 * \param[in]     name     New entry's name
 * \param[in]     value    New entry's value
 *
 * \return New head (of list) of nvpairs
 * \note The caller is responsible for freeing the nvpairs object
 *       with \c pcmk_free_nvpairs().
 */
pcmk_nvpairs_t *pcmk_prepend_nvpair(pcmk_nvpairs_t *nvpairs, const char *name,
                                    const char *value);

/*!
 * \brief Free (a list of) nvpairs (name/value pairs)
 *
 * \param[in] nvpairs  Nvpairs to free
 */
void pcmk_free_nvpairs(pcmk_nvpairs_t *nvpairs);

/*!
 * \brief Sort (a list of) nvpairs (name/value pairs)
 *
 * \param[in,out] list  Nvpairs to sort
 *
 * \return New head (of list) of nvpairs.
 */
pcmk_nvpairs_t *pcmk_sort_nvpairs(pcmk_nvpairs_t *nvpairs);

/*!
 * \brief Create (a list of) nvpairs from an XML node's attributes
 *
 * \param[in]  XML to parse
 *
 * \return New (list of) nvpairs (name/value pairs)
 * \note The caller is responsible for freeing the nvpairs object
 *       with \c pcmk_free_nvpairs().
 */
pcmk_nvpairs_t *pcmk_xml_attrs2nvpairs(xmlNode *xml);

/*!
 * \brief Add XML attributes based on (a list of) nvpairs
 *
 * \param[in]     nvpairs  (List of) nvpairs (name/value pairs)
 * \param[in,out] xml      XML node to add attributes to
 */
void pcmk_nvpairs2xml_attrs(pcmk_nvpairs_t *nvpairs, xmlNode *xml);

/*
 *  other related functions
 */

xmlNode *crm_create_nvpair_xml(xmlNode *parent, const char *id,
                               const char *name, const char *value);
void hash2nvpair(gpointer key, gpointer value, gpointer user_data);
void hash2field(gpointer key, gpointer value, gpointer user_data);
void hash2metafield(gpointer key, gpointer value, gpointer user_data);
void hash2smartfield(gpointer key, gpointer value, gpointer user_data);
GHashTable *xml2list(xmlNode *parent);

const char *crm_xml_add(xmlNode *node, const char *name, const char *value);
const char *crm_xml_replace(xmlNode *node, const char *name, const char *value);
const char *crm_xml_add_int(xmlNode *node, const char *name, int value);
const char *crm_xml_add_ms(xmlNode *node, const char *name, guint ms);

const char *crm_element_value(const xmlNode *data, const char *name);
int crm_element_value_int(const xmlNode *data, const char *name, int *dest);
int crm_element_value_ms(const xmlNode *data, const char *name, guint *dest);
int crm_element_value_timeval(const xmlNode *data, const char *name_sec,
                              const char *name_usec, struct timeval *dest);
char *crm_element_value_copy(const xmlNode *data, const char *name);

/*!
 * \brief Copy an element from one XML object to another
 *
 * \param[in]     obj1     Source XML
 * \param[in,out] obj2     Destination XML
 * \param[in]     element  Name of element to copy
 *
 * \return Pointer to copied value (from source)
 */
static inline const char *
crm_copy_xml_element(xmlNode *obj1, xmlNode *obj2, const char *element)
{
    const char *value = crm_element_value(obj1, element);

    crm_xml_add(obj2, element, value);
    return value;
}

/*!
 * \brief Add a boolean attribute to an XML object
 *
 * Add an attribute with the value \c XML_BOOLEAN_TRUE or \c XML_BOOLEAN_FALSE
 * as appropriate to an XML object.
 *
 * \param[in,out] node   XML object to add attribute to
 * \param[in]     name   Name of attribute to add
 * \param[in]     value  Boolean whose value will be tested
 *
 * \return Pointer to newly created XML attribute's content, or \c NULL on error
 */
static inline const char *
crm_xml_add_boolean(xmlNode *node, const char *name, gboolean value)
{
    return crm_xml_add(node, name, (value? "true" : "false"));
}

#  ifdef __cplusplus
}
#  endif

#endif // CRM_COMMON_NVPAIR__H
