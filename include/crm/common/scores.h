/*
 * Copyright 2004-2024 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU Lesser General Public License
 * version 2.1 or later (LGPLv2.1+) WITHOUT ANY WARRANTY.
 */

#ifndef PCMK__CRM_COMMON_SCORES__H
#define PCMK__CRM_COMMON_SCORES__H

#include <stdbool.h>        // bool

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \file
 * \brief Pacemaker APIs related to scores
 * \ingroup core
 */

//! Integer score to use to represent "infinity"
#define PCMK_SCORE_INFINITY 1000000

int pcmk_parse_score(const char *score_s, int *score, int default_score);
const char *pcmk_readable_score(int score);
bool pcmk_str_is_infinity(const char *s);
bool pcmk_str_is_minus_infinity(const char *s);

#ifdef __cplusplus
}
#endif

#if !defined(PCMK_ALLOW_DEPRECATED) || (PCMK_ALLOW_DEPRECATED == 1)
#include <crm/common/scores_compat.h>
#endif

#endif // PCMK__CRM_COMMON_SCORES__H
