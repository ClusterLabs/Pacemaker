/*
 * Copyright 2024 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU Lesser General Public License
 * version 2.1 or later (LGPLv2.1+) WITHOUT ANY WARRANTY.
 */

#include <crm_internal.h>

/*!
 * \internal
 * \brief Ensure a health strategy value is allowed
 *
 * \param[in] value  Configured health strategy
 *
 * \return true if \p value is an allowed health strategy value, otherwise false
 */
bool
pcmk__validate_health_strategy(const char *value)
{
    return pcmk__strcase_any_of(value,
                                PCMK_VALUE_NONE,
                                PCMK_VALUE_CUSTOM,
                                PCMK_VALUE_ONLY_GREEN,
                                PCMK_VALUE_PROGRESSIVE,
                                PCMK_VALUE_MIGRATE_ON_RED,
                                NULL);
}

/*!
 * \internal
 * \brief Parse node health strategy from a user-provided string
 *
 * \param[in] value  User-provided configuration value for node-health-strategy
 *
 * \return Node health strategy corresponding to \p value
 */
enum pcmk__health_strategy
pcmk__parse_health_strategy(const char *value)
{
    if (pcmk__str_eq(value, PCMK_VALUE_NONE,
                     pcmk__str_null_matches|pcmk__str_casei)) {
        return pcmk__health_strategy_none;
    }
    if (pcmk__str_eq(value, PCMK_VALUE_MIGRATE_ON_RED, pcmk__str_casei)) {
        return pcmk__health_strategy_no_red;
    }
    if (pcmk__str_eq(value, PCMK_VALUE_ONLY_GREEN, pcmk__str_casei)) {
        return pcmk__health_strategy_only_green;
    }
    if (pcmk__str_eq(value, PCMK_VALUE_PROGRESSIVE, pcmk__str_casei)) {
        return pcmk__health_strategy_progressive;
    }
    if (pcmk__str_eq(value, PCMK_VALUE_CUSTOM, pcmk__str_casei)) {
        return pcmk__health_strategy_custom;
    } else {
        pcmk__config_err("Using default of \"" PCMK_VALUE_NONE "\" for "
                         PCMK_OPT_NODE_HEALTH_STRATEGY
                         " because '%s' is not a valid value",
                         value);
        return pcmk__health_strategy_none;
    }
}
