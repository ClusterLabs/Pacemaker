/*
 * Copyright 2005-2019 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU General Public License version 2
 * or later (GPLv2+) WITHOUT ANY WARRANTY.
 */

#include <crm_internal.h>
#include <crm/crm.h>
#include <crm/common/iso8601.h>
#include <crm/common/util.h>  /* CRM_ASSERT */
#include <unistd.h>

char command = 0;

/* *INDENT-OFF* */
static struct crm_option long_options[] = {
    /* Top-level Options */
    {"help",    0, 0, '?', "\tThis text"},
    {"version", 0, 0, '$', "\tVersion information"  },
    {"verbose", 0, 0, 'V', "\tIncrease debug output"},

    {"-spacer-",    0, 0, '-', "\nCommands:"},
    {"now",      0, 0, 'n', "\tDisplay the current date/time"},
    {"date",     1, 0, 'd', "Parse an ISO 8601 date/time (e.g. 2005-01-20T00:30:00+01:00 or 2005-040)"},
    { "period",   1, 0, 'p',
      "Parse an ISO 8601 period (interval) with start time (e.g. 2005-040/2005-043)"
    },
    { "duration", 1, 0, 'D',
      "Parse an ISO 8601 duration with start time (e.g. 2005-040/P1M)"
    },
    { "expected", 1, 0, 'E',
      "Parse an ISO 8601 duration with start time (e.g. 2005-040/P1M)"
    },
    {"-spacer-",0, 0, '-', "\nOutput Modifiers:"},
    {"seconds", 0, 0, 's', "\tShow result as seconds since 0000-001T00:00:00Z"},
    {"epoch", 0, 0, 'S', "\tShow result as seconds since EPOCH (1970-001T00:00:00Z)"},
    {"local",   0, 0, 'L', "\tShow result as a 'local' date/time"},
    {"ordinal", 0, 0, 'O', "\tShow result as an 'ordinal' date/time"},
    {"week",    0, 0, 'W', "\tShow result as a 'calendar week' date/time",
                           pcmk_option_paragraph},

    {"-spacer-", 1, NULL, '-', "Environment:"},
    {"-spacer-", 1, NULL, '-', "-   TZ:"},
    {"-spacer-", 1, NULL, '-', "    time zone specification to be considered"
                               " unless expressly defined"
                               " (especially for -d, ...); see tzset(3)"},

    /* SEE ALSO pasted verbatim */
    {"-spacer-", 1, NULL, '-', "\n\nSee also:"},
    {"-spacer-", 1, NULL, '-', "* regarding date/time specified per ISO 8601 (for -d, ...):"},
    {"-spacer-", 1, NULL, '-', "  https://en.wikipedia.org/wiki/ISO_8601",
                               pcmk_option_paragraph},

    {0, 0, 0, 0}
};
/* *INDENT-ON* */

static void
log_time_period(int log_level, crm_time_period_t * dtp, int flags)
{
    char *start = crm_time_as_string(dtp->start, flags);
    char *end = crm_time_as_string(dtp->end, flags);
    CRM_ASSERT(start != NULL && end != NULL);

    if (log_level < LOG_CRIT) {
        printf("Period: %s to %s\n", start, end);
    } else {
        do_crm_log(log_level, "Period: %s to %s", start, end);
    }
    free(start);
    free(end);
}

int
main(int argc, char **argv)
{
    crm_exit_t exit_code = CRM_EX_OK;
    int argerr = 0;
    int flag;
    int index = 0;
    int print_options = 0;
    crm_time_t *duration = NULL;
    crm_time_t *date_time = NULL;
    crm_time_period_t *period = NULL;

    const char *period_s = NULL;
    const char *duration_s = NULL;
    const char *date_time_s = NULL;
    const char *expected_s = NULL;

    crm_log_cli_init("iso8601");
    crm_set_options(NULL, "command [output modifier] ", long_options,
                    "Display and parse ISO8601 dates and times");

    if (argc < 2) {
        argerr++;
    }

    while (1) {
        flag = crm_get_option(argc, argv, &index);
        if (flag == -1)
            break;

        switch (flag) {
            case 'V':
                crm_bump_log_level(argc, argv);
                break;
            case '?':
            case '$':
                crm_help(flag, CRM_EX_OK);
                break;
            case 'n':
                date_time_s = "now";
                break;
            case 'd':
                date_time_s = optarg;
                break;
            case 'p':
                period_s = optarg;
                break;
            case 'D':
                duration_s = optarg;
                break;
            case 'E':
                expected_s = optarg;
                break;
            case 'S':
                print_options |= crm_time_epoch;
                break;
            case 's':
                print_options |= crm_time_seconds;
                break;
            case 'W':
                print_options |= crm_time_weeks;
                break;
            case 'O':
                print_options |= crm_time_ordinal;
                break;
            case 'L':
                print_options |= crm_time_log_with_timezone;
                break;
                break;
        }
    }

    if (safe_str_eq("now", date_time_s)) {
        date_time = crm_time_new(NULL);

        if (date_time == NULL) {
            fprintf(stderr, "Internal error: couldn't determine 'now'!\n");
            crm_exit(CRM_EX_SOFTWARE);
        }
        crm_time_log(LOG_TRACE, "Current date/time", date_time,
                     crm_time_ordinal | crm_time_log_date | crm_time_log_timeofday);
        crm_time_log(-1, "Current date/time", date_time,
                     print_options | crm_time_log_date | crm_time_log_timeofday);

    } else if (date_time_s) {
        date_time = crm_time_new(date_time_s);

        if (date_time == NULL) {
            fprintf(stderr, "Invalid date/time specified: %s\n", optarg);
            crm_help('?', CRM_EX_USAGE);
        }
        crm_time_log(LOG_TRACE, "Date", date_time,
                     crm_time_ordinal | crm_time_log_date | crm_time_log_timeofday);
        crm_time_log(-1, "Date", date_time,
                     print_options | crm_time_log_date | crm_time_log_timeofday);
    }

    if (duration_s) {
        duration = crm_time_parse_duration(duration_s);

        if (duration == NULL) {
            fprintf(stderr, "Invalid duration specified: %s\n", duration_s);
            crm_help('?', CRM_EX_USAGE);
        }
        crm_time_log(LOG_TRACE, "Duration", duration, crm_time_log_duration);
        crm_time_log(-1, "Duration", duration, print_options | crm_time_log_duration);
    }

    if (period_s) {
        period = crm_time_parse_period(period_s);

        if (period == NULL) {
            fprintf(stderr, "Invalid interval specified: %s\n", optarg);
            crm_help('?', CRM_EX_USAGE);
        }
        log_time_period(LOG_TRACE, period,
                        print_options | crm_time_log_date | crm_time_log_timeofday);
        log_time_period(-1, period,
                        print_options | crm_time_log_date | crm_time_log_timeofday);
    }

    if (date_time && duration) {
        crm_time_t *later = crm_time_add(date_time, duration);

        crm_time_log(LOG_TRACE, "Duration ends at", later,
                     crm_time_ordinal | crm_time_log_date | crm_time_log_timeofday);
        crm_time_log(-1, "Duration ends at", later,
                     print_options | crm_time_log_date | crm_time_log_timeofday |
                     crm_time_log_with_timezone);
        if (expected_s) {
            char *dt_s = crm_time_as_string(later,
                                            print_options | crm_time_log_date |
                                            crm_time_log_timeofday);
            if (safe_str_neq(expected_s, dt_s)) {
                exit_code = CRM_EX_ERROR;
            }
            free(dt_s);
        }
        crm_time_free(later);

    } else if (date_time && expected_s) {
        char *dt_s = crm_time_as_string(date_time,
                                        print_options | crm_time_log_date | crm_time_log_timeofday);

        if (safe_str_neq(expected_s, dt_s)) {
            exit_code = CRM_EX_ERROR;
        }
        free(dt_s);
    }

    crm_time_free(date_time);
    crm_time_free(duration);
    if (period) {
        crm_time_free(period->start);
        crm_time_free(period->end);
        crm_time_free(period->diff);
        free(period);
    }

    crm_exit(exit_code);
}
