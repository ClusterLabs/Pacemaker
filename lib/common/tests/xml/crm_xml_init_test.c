/*
 * Copyright 2023-2024 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU General Public License version 2
 * or later (GPLv2+) WITHOUT ANY WARRANTY.
 */

#include <crm_internal.h>

#include <crm/common/xml.h>
#include <crm/common/unittest_internal.h>
#include <crm/common/xml_internal.h>

#include "crmcommon_private.h"

static void
buffer_scheme_test(void **state) {
    assert_int_equal(XML_BUFFER_ALLOC_DOUBLEIT, xmlGetBufferAllocationScheme());
}

/* These functions also serve as unit tests of the static new_private_data
 * function.  We can't test free_private_data because libxml will call that as
 * part of freeing everything else.  By the time we'd get back into a unit test
 * where we could check that private members are NULL, the structure containing
 * the private data would have been freed.
 *
 * This could probably be tested with a lot of function mocking, but that
 * doesn't seem worth it.
 */

static void
create_element_node(void **state) {
    xml_doc_private_t *docpriv = NULL;
    xml_node_private_t *priv = NULL;
    xmlDoc *doc = pcmk__xml_new_doc();
    xmlNodePtr node = xmlNewDocNode(doc, NULL, (pcmkXmlStr) "test", NULL);

    /* Adding a node to the document marks it as dirty */
    docpriv = doc->_private;
    assert_true(pcmk_all_flags_set(docpriv->flags, pcmk__xf_dirty));

    /* Double check things */
    assert_non_null(node);
    assert_int_equal(node->type, XML_ELEMENT_NODE);

    /* Check that the private data is initialized correctly */
    priv = node->_private;
    assert_non_null(priv);
    assert_int_equal(priv->check, PCMK__XML_NODE_PRIVATE_MAGIC);
    assert_true(pcmk_all_flags_set(priv->flags, pcmk__xf_dirty|pcmk__xf_created));

    /* Clean up */
    pcmk__xml_free_doc(doc);
}

static void
create_attr_node(void **state) {
    xml_doc_private_t *docpriv = NULL;
    xml_node_private_t *priv = NULL;
    xmlDoc *doc = pcmk__xml_new_doc();
    xmlNodePtr node = xmlNewDocNode(doc, NULL, (pcmkXmlStr) "test", NULL);
    xmlAttrPtr attr = xmlNewProp(node, (pcmkXmlStr) PCMK_XA_NAME,
                                 (pcmkXmlStr) "dummy-value");

    /* Adding a node to the document marks it as dirty */
    docpriv = doc->_private;
    assert_true(pcmk_all_flags_set(docpriv->flags, pcmk__xf_dirty));

    /* Double check things */
    assert_non_null(attr);
    assert_int_equal(attr->type, XML_ATTRIBUTE_NODE);

    /* Check that the private data is initialized correctly */
    priv = attr->_private;
    assert_non_null(priv);
    assert_int_equal(priv->check, PCMK__XML_NODE_PRIVATE_MAGIC);
    assert_true(pcmk_all_flags_set(priv->flags, pcmk__xf_dirty|pcmk__xf_created));

    /* Clean up */
    pcmk__xml_free_doc(doc);
}

static void
create_comment_node(void **state) {
    xml_doc_private_t *docpriv = NULL;
    xml_node_private_t *priv = NULL;
    xmlDoc *doc = pcmk__xml_new_doc();
    xmlNodePtr node = xmlNewDocComment(doc, (pcmkXmlStr) "blahblah");

    /* Adding a node to the document marks it as dirty */
    docpriv = doc->_private;
    assert_true(pcmk_all_flags_set(docpriv->flags, pcmk__xf_dirty));

    /* Double check things */
    assert_non_null(node);
    assert_int_equal(node->type, XML_COMMENT_NODE);

    /* Check that the private data is initialized correctly */
    priv = node->_private;
    assert_non_null(priv);
    assert_int_equal(priv->check, PCMK__XML_NODE_PRIVATE_MAGIC);
    assert_true(pcmk_all_flags_set(priv->flags, pcmk__xf_dirty|pcmk__xf_created));

    /* Clean up */
    pcmk__xml_free_doc(doc);
}

static void
create_text_node(void **state) {
    xml_doc_private_t *docpriv = NULL;
    xml_node_private_t *priv = NULL;
    xmlDoc *doc = pcmk__xml_new_doc();
    xmlNodePtr node = xmlNewDocText(doc, (pcmkXmlStr) "blahblah");

    /* Adding a node to the document marks it as dirty */
    docpriv = doc->_private;
    assert_true(pcmk_all_flags_set(docpriv->flags, pcmk__xf_dirty));

    /* Double check things */
    assert_non_null(node);
    assert_int_equal(node->type, XML_TEXT_NODE);

    /* Check that no private data was created */
    priv = node->_private;
    assert_null(priv);

    /* Clean up */
    pcmk__xml_free_doc(doc);
}

static void
create_dtd_node(void **state) {
    xml_doc_private_t *docpriv = NULL;
    xml_node_private_t *priv = NULL;
    xmlDoc *doc = pcmk__xml_new_doc();
    xmlDtdPtr dtd = xmlNewDtd(doc, (pcmkXmlStr) PCMK_XA_NAME,
                              (pcmkXmlStr) "externalId",
                              (pcmkXmlStr) "systemId");

    /* Adding a node to the document marks it as dirty */
    docpriv = doc->_private;
    assert_true(pcmk_all_flags_set(docpriv->flags, pcmk__xf_dirty));

    /* Double check things */
    assert_non_null(dtd);
    assert_int_equal(dtd->type, XML_DTD_NODE);

    /* Check that no private data was created */
    priv = dtd->_private;
    assert_null(priv);

    /* Clean up */
    // If you call xmlFreeDtd() before pcmk__xml_free_doc(), you get a segfault
    pcmk__xml_free_doc(doc);
}

static void
create_cdata_node(void **state) {
    xml_doc_private_t *docpriv = NULL;
    xml_node_private_t *priv = NULL;
    xmlDoc *doc = pcmk__xml_new_doc();
    xmlNodePtr node = xmlNewCDataBlock(doc, (pcmkXmlStr) "blahblah", 8);

    /* Adding a node to the document marks it as dirty */
    docpriv = doc->_private;
    assert_true(pcmk_all_flags_set(docpriv->flags, pcmk__xf_dirty));

    /* Double check things */
    assert_non_null(node);
    assert_int_equal(node->type, XML_CDATA_SECTION_NODE);

    /* Check that no private data was created */
    priv = node->_private;
    assert_null(priv);

    /* Clean up */
    pcmk__xml_free_doc(doc);
}

// The group setup/teardown functions call crm_xml_init()/crm_xml_cleanup()
PCMK__UNIT_TEST(pcmk__xml_test_setup_group, pcmk__xml_test_teardown_group,
                cmocka_unit_test(buffer_scheme_test),
                cmocka_unit_test(create_element_node),
                cmocka_unit_test(create_attr_node),
                cmocka_unit_test(create_comment_node),
                cmocka_unit_test(create_text_node),
                cmocka_unit_test(create_dtd_node),
                cmocka_unit_test(create_cdata_node));
