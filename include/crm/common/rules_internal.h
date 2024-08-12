/*
 * Copyright 2004-2024 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU Lesser General Public License
 * version 2.1 or later (LGPLv2.1+) WITHOUT ANY WARRANTY.
 */

#ifndef PCMK__CRM_COMMON_RULES_INTERNAL__H
#define PCMK__CRM_COMMON_RULES_INTERNAL__H

#include <regex.h>                      // regmatch_t
#include <libxml/tree.h>                // xmlNode

#include <crm/common/rules.h>           // enum expression_type, etc.
#include <crm/common/iso8601.h>         // crm_time_t

#ifdef __cplusplus
extern "C" {
#endif

enum pcmk__combine {
    pcmk__combine_unknown,
    pcmk__combine_and,
    pcmk__combine_or,
};

enum expression_type pcmk__condition_type(const xmlNode *condition);
char *pcmk__replace_submatches(const char *string, const char *match,
                               const regmatch_t submatches[], int nmatches);
enum pcmk__combine pcmk__parse_combine(const char *combine);

int pcmk__evaluate_date_expression(const xmlNode *date_expression,
                                   const crm_time_t *now,
                                   crm_time_t *next_change);
int pcmk__evaluate_condition(xmlNode *expr, const pcmk_rule_input_t *rule_input,
                             crm_time_t *next_change);

#ifdef __cplusplus
}
#endif

#endif // PCMK__CRM_COMMON_RULES_INTERNAL__H
