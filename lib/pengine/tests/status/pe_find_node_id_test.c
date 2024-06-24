/*
 * Copyright 2022-2024 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU General Public License version 2
 * or later (GPLv2+) WITHOUT ANY WARRANTY.
 */

#include <crm_internal.h>

#include <crm/common/unittest_internal.h>
#include <crm/pengine/internal.h>

static void
empty_list(void **state) {
    assert_null(pe_find_node_id(NULL, NULL));
    assert_null(pe_find_node_id(NULL, "id1"));
}

static void
non_null_list(void **state) {
    GList *nodes = NULL;

    pcmk_node_t *a = pcmk__assert_alloc(1, sizeof(pcmk_node_t));
    pcmk_node_t *b = pcmk__assert_alloc(1, sizeof(pcmk_node_t));

    a->priv = pcmk__assert_alloc(1, sizeof(struct pcmk__node_private));
    b->priv = pcmk__assert_alloc(1, sizeof(struct pcmk__node_private));

    a->priv->id = "id1";
    b->priv->id = "id2";

    nodes = g_list_append(nodes, a);
    nodes = g_list_append(nodes, b);

    assert_ptr_equal(a, pe_find_node_id(nodes, "id1"));
    assert_null(pe_find_node_id(nodes, "id10"));
    assert_null(pe_find_node_id(nodes, "nodeid1"));
    assert_ptr_equal(b, pe_find_node_id(nodes, "ID2"));
    assert_null(pe_find_node_id(nodes, "xyz"));

    free(a->priv);
    free(a);
    free(b->priv);
    free(b);
    g_list_free(nodes);
}

PCMK__UNIT_TEST(NULL, NULL,
                cmocka_unit_test(empty_list),
                cmocka_unit_test(non_null_list))
