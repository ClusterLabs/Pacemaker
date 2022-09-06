/*
 * Copyright 2021 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU Lesser General Public License
 * version 2.1 or later (LGPLv2.1+) WITHOUT ANY WARRANTY.
 */

#include <crm_internal.h>

#include <crm/common/unittest_internal.h>

static void
is_probe_test(void **state)
{
    assert_false(pcmk_is_probe(NULL, 0));
    assert_false(pcmk_is_probe("", 0));
    assert_false(pcmk_is_probe("blahblah", 0));
    assert_false(pcmk_is_probe("monitor", 1));
    assert_true(pcmk_is_probe("monitor", 0));
}

PCMK__UNIT_TEST(NULL, NULL,
                cmocka_unit_test(is_probe_test))
