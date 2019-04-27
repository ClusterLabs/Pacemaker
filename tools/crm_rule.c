/*
 * Copyright 2019 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU General Public License version 2
 * or later (GPLv2+) WITHOUT ANY WARRANTY.
 */

#include <crm_internal.h>

#include <crm/cib.h>
#include <crm/common/iso8601.h>
#include <crm/msg_xml.h>
#include <crm/pengine/rules_internal.h>
#include <crm/pengine/status.h>
#include <pacemaker-internal.h>

#include <sys/stat.h>

enum crm_rule_mode {
    crm_rule_mode_none,
    crm_rule_mode_check
} rule_mode = crm_rule_mode_none;

static int crm_rule_check(pe_working_set_t *data_set, const char *rule_id, crm_time_t *effective_date);

static struct crm_option long_options[] = {
    /* Top-level Options */
    {"help",       no_argument,       NULL, '?', "\tThis text"},
    {"version",    no_argument,       NULL, '$', "\tVersion information"},
    {"verbose",    no_argument,       NULL, 'V', "\tIncrease debug output"},

    {"-spacer-",   required_argument, NULL, '-', "\nModes (mutually exclusive):"},
    {"check",      no_argument,       NULL, 'c', "\tCheck if a rule is in effect, now or at given date/time"},

    {"-spacer-",   required_argument, NULL, '-', "\nAdditional options:"},
    {"date",       required_argument, NULL, 'd', "Date/time (ISO 8601) specification as a mode's input"},
    {"rule",       required_argument, NULL, 'r', "The ID of the rule to check" },

    {"-spacer-",   no_argument,       NULL, '-', "\nData:"},
    {"xml-text",   required_argument, NULL, 'X', "Use argument for XML (or stdin if '-')"},

    {"-spacer-",   required_argument, NULL, '-', "\n\nThis tool is currently experimental.",
     pcmk_option_paragraph},
    {"-spacer-",   required_argument, NULL, '-', "The interface, behavior, and output may "
                                                 "change with any version of pacemaker.",
     pcmk_option_paragraph},
    {"-spacer-",   required_argument, NULL, '-', "Important: avoid making conclusion based on"
                                                 " this tool when it is not of the same version as"
                                                 " an overall pacemaker deployment."
                                                 "\nThe exact date/time parsing behaviour may get altered"
                                                 " over time, so only the above requirement guarantees"
                                                 " reliable outcomes related to particular deployment"
                                                 " (i.e. without a generic validity for older/newer"
                                                 " versions of pacemaker, possibly -- depending on the"
                                                 " permanency of the arising decisions -- mandating"
                                                 " any such to be re-evaluated anew upon an update).",
     pcmk_option_paragraph},

    {0, 0, 0, 0}
};

static int
crm_rule_check(pe_working_set_t *data_set, const char *rule_id, crm_time_t *effective_date)
{
    xmlNode *cib_constraints = NULL;
    xmlNode *match = NULL;
    xmlXPathObjectPtr xpathObj = NULL;
    pe_eval_date_result_t result;
    char *xpath = NULL;
    int rc = pcmk_ok;
    int max = 0;

    /* Rules are under the constraints node in the XML, so first find that. */
    cib_constraints = get_object_root(XML_CIB_TAG_CONSTRAINTS, data_set->input);

    /* Get all rules matching the given ID, which also have only a single date_expression
     * child whose operation is not 'date_spec'.  This is fairly limited, but it's hard
     * to check expressions more complicated than that.
     */
    xpath = crm_strdup_printf("//rule[@id='%s']/date_expression[@operation!='date_spec']", rule_id);
    xpathObj = xpath_search(cib_constraints, xpath);

    max = numXpathResults(xpathObj);
    if (max == 0) {
        CMD_ERR("No rule found with ID=%s containing a date_expression", rule_id);
        rc = -ENXIO;
        goto bail;
    } else if (max > 1) {
        CMD_ERR("More than one date_expression in %s is not supported", rule_id);
        rc = -EOPNOTSUPP;
        goto bail;
    }

    match = getXpathResult(xpathObj, 0);

    /* We should have ensured both of these pass with the xpath query above, but
     * double checking can't hurt.
     */
    CRM_ASSERT(match != NULL);
    CRM_ASSERT(find_expression_type(match) == time_expr);

    result = pe_eval_date_expression(match, effective_date);

    if (result == pe_date_within_range) {
        printf("Rule %s is still in effect\n", rule_id);
        rc = 0;
    } else if (result == pe_date_after_range) {
        printf("Rule %s is expired\n", rule_id);
        rc = 1;
    } else if (result == pe_date_before_range) {
        printf("Rule %s has not yet taken effect\n", rule_id);
        rc = 2;
    } else {
        printf("Could not determine whether rule %s is expired\n", rule_id);
        rc = 3;
    }

bail:
    free(xpath);
    freeXpathObject(xpathObj);
    return rc;
}

int
main(int argc, char **argv)
{
    cib_t *cib_conn = NULL;
    pe_working_set_t *data_set = NULL;

    int flag = 0;
    int option_index = 0;

    char *rule_id = NULL;
    crm_time_t *rule_date = NULL;

    xmlNode *input = NULL;
    char *input_xml = NULL;

    int rc = pcmk_ok;
    crm_exit_t exit_code = CRM_EX_OK;

    crm_log_cli_init("crm_rule");
    crm_set_options(NULL, "[options]", long_options,
                    "Tool for querying the state of rules");

    while (flag >= 0) {
        flag = crm_get_option(argc, argv, &option_index);
        switch (flag) {
            case -1:
                break;

            case 'V':
                crm_bump_log_level(argc, argv);
                break;

            case '$':
            case '?':
                crm_help(flag, CRM_EX_OK);
                break;

            case 'c':
                rule_mode = crm_rule_mode_check;
                break;

            case 'd':
                rule_date = crm_time_new(optarg);
                if (rule_date == NULL) {
                    exit_code = CRM_EX_DATAERR;
                    goto bail;
                }

                break;

            case 'X':
                input_xml = optarg;
                break;

            case 'r':
                rule_id = strdup(optarg);
                break;

            default:
                crm_help(flag, CRM_EX_OK);
                break;
        }
    }

    /* Check command line arguments before opening a connection to
     * the CIB manager or doing anything else important.
     */
    if (rule_mode == crm_rule_mode_check) {
        if (rule_id == NULL) {
            CMD_ERR("--check requires use of --rule=\n");
            crm_help(flag, CRM_EX_USAGE);
        }
    }

    /* Set up some defaults. */
    if (rule_date == NULL) {
        rule_date = crm_time_new(NULL);
    }

    /* Where does the XML come from?  If one of various command line options were
     * given, use those.  Otherwise, connect to the CIB and use that.
     */
    if (safe_str_eq(input_xml, "-")) {
        input = stdin2xml();

        if (input == NULL) {
            fprintf(stderr, "Couldn't parse input from STDIN\n");
            exit_code = CRM_EX_DATAERR;
            goto bail;
        }
    } else if (input_xml != NULL) {
        input = string2xml(input_xml);

        if (input == NULL) {
            fprintf(stderr, "Couldn't parse input string: %s\n", input_xml);
            exit_code = CRM_EX_DATAERR;
            goto bail;
        }
    } else {
        /* Establish a connection to the CIB manager */
        cib_conn = cib_new();
        rc = cib_conn->cmds->signon(cib_conn, crm_system_name, cib_command);
        if (rc != pcmk_ok) {
            CMD_ERR("Error connecting to the CIB manager: %s", pcmk_strerror(rc));
            exit_code = crm_errno2exit(rc);
            goto bail;
        }
    }

    /* Populate working set from CIB query */
    if (input == NULL) {
        rc = cib_conn->cmds->query(cib_conn, NULL, &input, cib_scope_local | cib_sync_call);
        if (rc != pcmk_ok) {
            exit_code = crm_errno2exit(rc);
            goto bail;
        }
    }

    /* Populate the working set instance */
    data_set = pe_new_working_set();
    if (data_set == NULL) {
        exit_code = crm_errno2exit(ENOMEM);
        goto bail;
    }

    data_set->input = input;
    data_set->now = rule_date;

    /* Unpack everything. */
    cluster_status(data_set);

    /* Now do whichever operation mode was asked for.  There's only one at the
     * moment so this looks a little silly, but I expect there will be more
     * modes in the future.
     */
    switch(rule_mode) {
        case crm_rule_mode_check:
            rc = crm_rule_check(data_set, rule_id, rule_date);

            if (rc < 0) {
                CMD_ERR("Error checking rule: %s", pcmk_strerror(rc));
                exit_code = crm_errno2exit(rc);
            } else if (rc == 1) {
                exit_code = CRM_EX_EXPIRED;
            } else if (rc == 2) {
                exit_code = CRM_EX_NOT_YET_IN_EFFECT;
            } else if (rc == 3) {
                exit_code = CRM_EX_INDETERMINATE;
            } else {
                exit_code = rc;
            }

            break;

        default:
            break;
    }

bail:
    if (cib_conn != NULL) {
        cib_conn->cmds->signoff(cib_conn);
        cib_delete(cib_conn);
    }

    pe_free_working_set(data_set);
    crm_exit(exit_code);
}
