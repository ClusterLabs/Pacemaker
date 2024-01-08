/*
 * Copyright 2004-2024 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU Lesser General Public License
 * version 2.1 or later (LGPLv2.1+) WITHOUT ANY WARRANTY.
 */

#ifndef PCMK__CRM_MSG_XML__H
#  define PCMK__CRM_MSG_XML__H

#  include <crm/common/xml.h>

#if !defined(PCMK_ALLOW_DEPRECATED) || (PCMK_ALLOW_DEPRECATED == 1)
#include <crm/msg_xml_compat.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* This file defines constants for various XML syntax (mainly element and
 * attribute names).
 *
 * For consistency, new constants should start with "PCMK_", followed by:
 * * "XE" for XML element names
 * * "XA" for XML attribute names
 * * "OPT" for cluster option (property) names
 * * "META" for meta-attribute names
 * * "VALUE" for enumerated values for various options
 *
 * Old names that don't follow this policy should eventually be deprecated and
 * replaced with names that do.
 *
 * Symbols should be public if the user may specify them somewhere (especially
 * the CIB) or if they're part of a well-defined structure that a user may need
 * to parse. They should be internal if they're used only internally to
 * Pacemaker (such as daemon IPC/CPG message XML).
 *
 * Constants belong in the following locations:
 * * Public "XE" and "XA": msg_xml.h
 * * Internal "XE" and "XA": crm_internal.h
 * * Public "OPT", "META", and "VALUE": options.h
 * * Internal "OPT", "META", and "VALUE": options_internal.h
 *
 * For meta-attributes that can be specified as either XML attributes or nvpair
 * names, use "META" unless using both "XA" and "META" constants adds clarity.
 * An example is operation attributes, which can be specified either as
 * attributes of the PCMK_XE_OP element or as nvpairs in a meta-attribute set
 * beneath the PCMK_XE_OP element.
 */

/*
 * XML elements
 */

#define PCMK_XE_ACL_GROUP                   "acl_group"
#define PCMK_XE_ACL_PERMISSION              "acl_permission"
#define PCMK_XE_ACL_ROLE                    "acl_role"
#define PCMK_XE_ACL_TARGET                  "acl_target"
#define PCMK_XE_ACLS                        "acls"
#define PCMK_XE_ACTION                      "action"
#define PCMK_XE_ACTIONS                     "actions"
#define PCMK_XE_ALERT                       "alert"
#define PCMK_XE_ALERTS                      "alerts"
#define PCMK_XE_ATTRIBUTE                   "attribute"
#define PCMK_XE_BUNDLE                      "bundle"
#define PCMK_XE_CHANGE                      "change"
#define PCMK_XE_CHANGE_ATTR                 "change-attr"
#define PCMK_XE_CHANGE_LIST                 "change-list"
#define PCMK_XE_CHANGE_RESULT               "change-result"
#define PCMK_XE_CIB                         "cib"
#define PCMK_XE_CLONE                       "clone"
#define PCMK_XE_CLUSTER_PROPERTY_SET        "cluster_property_set"
#define PCMK_XE_CONFIGURATION               "configuration"
#define PCMK_XE_CONSTRAINTS                 "constraints"
#define PCMK_XE_CONTENT                     "content"
#define PCMK_XE_CRM_CONFIG                  "crm_config"
#define PCMK_XE_DATE_EXPRESSION             "date_expression"
#define PCMK_XE_DATE_SPEC                   "date_spec"
#define PCMK_XE_DIFF                        "diff"
#define PCMK_XE_DURATION                    "duration"
#define PCMK_XE_EXPRESSION                  "expression"
#define PCMK_XE_FEATURE                     "feature"
#define PCMK_XE_FEATURES                    "features"
#define PCMK_XE_FENCE_EVENT                 "fence_event"
#define PCMK_XE_FENCE_HISTORY               "fence_history"
#define PCMK_XE_FENCING_LEVEL               "fencing-level"
#define PCMK_XE_FENCING_TOPOLOGY            "fencing-topology"
#define PCMK_XE_GROUP                       "group"
#define PCMK_XE_INSTANCE_ATTRIBUTES         "instance_attributes"
#define PCMK_XE_LAST_FENCED                 "last-fenced"
#define PCMK_XE_LONGDESC                    "longdesc"
#define PCMK_XE_META_ATTRIBUTES             "meta_attributes"
#define PCMK_XE_NETWORK                     "network"
#define PCMK_XE_NODE                        "node"
#define PCMK_XE_NODES                       "nodes"
#define PCMK_XE_NVPAIR                      "nvpair"
#define PCMK_XE_OBJ_REF                     "obj_ref"
#define PCMK_XE_OP                          "op"
#define PCMK_XE_OP_DEFAULTS                 "op_defaults"
#define PCMK_XE_OPERATION                   "operation"
#define PCMK_XE_OP_EXPRESSION               "op_expression"
#define PCMK_XE_OPTION                      "option"
#define PCMK_XE_OUTPUT                      "output"
#define PCMK_XE_PACEMAKERD                  "pacemakerd"
#define PCMK_XE_PARAMETER                   "parameter"
#define PCMK_XE_PARAMETERS                  "parameters"
#define PCMK_XE_PORT_MAPPING                "port-mapping"
#define PCMK_XE_POSITION                    "position"
#define PCMK_XE_PRIMITIVE                   "primitive"
#define PCMK_XE_RECIPIENT                   "recipient"
#define PCMK_XE_REPLICA                     "replica"
#define PCMK_XE_RESOURCE                    "resource"
#define PCMK_XE_RESOURCE_AGENT              "resource-agent"
#define PCMK_XE_RESOURCE_REF                "resource_ref"
#define PCMK_XE_RESOURCE_SET                "resource_set"
#define PCMK_XE_RESOURCES                   "resources"
#define PCMK_XE_ROLE                        "role"
#define PCMK_XE_RSC_ACTION                  "rsc_action"
#define PCMK_XE_RSC_COLOCATION              "rsc_colocation"
#define PCMK_XE_RSC_DEFAULTS                "rsc_defaults"
#define PCMK_XE_RSC_EXPRESSION              "rsc_expression"
#define PCMK_XE_RSC_LOCATION                "rsc_location"
#define PCMK_XE_RSC_ORDER                   "rsc_order"
#define PCMK_XE_RSC_TICKET                  "rsc_ticket"
#define PCMK_XE_RULE                        "rule"
#define PCMK_XE_SELECT                      "select"
#define PCMK_XE_SELECT_ATTRIBUTES           "select_attributes"
#define PCMK_XE_SELECT_FENCING              "select_fencing"
#define PCMK_XE_SELECT_NODES                "select_nodes"
#define PCMK_XE_SELECT_RESOURCES            "select_resources"
#define PCMK_XE_SHORTDESC                   "shortdesc"
#define PCMK_XE_SOURCE                      "source"
#define PCMK_XE_STATUS                      "status"
#define PCMK_XE_STORAGE                     "storage"
#define PCMK_XE_STORAGE_MAPPING             "storage-mapping"
#define PCMK_XE_TAG                         "tag"
#define PCMK_XE_TAGS                        "tags"
#define PCMK_XE_TARGET                      "target"
#define PCMK_XE_TEMPLATE                    "template"
#define PCMK_XE_TICKET                      "ticket"
#define PCMK_XE_TICKETS                     "tickets"
#define PCMK_XE_UTILIZATION                 "utilization"
#define PCMK_XE_VERSION                     "version"


/*
 * XML attributes
 */

#define PCMK_XA_ACTION                      "action"
#define PCMK_XA_ACTIVE                      "active"
#define PCMK_XA_ADD_HOST                    "add-host"
#define PCMK_XA_ADMIN_EPOCH                 "admin_epoch"
#define PCMK_XA_ATTRIBUTE                   "attribute"
#define PCMK_XA_AUTHOR                      "author"
#define PCMK_XA_BLOCKED                     "blocked"
#define PCMK_XA_BOOLEAN_OP                  "boolean-op"
#define PCMK_XA_BUILD                       "build"
#define PCMK_XA_CIB_LAST_WRITTEN            "cib-last-written"
#define PCMK_XA_CLASS                       "class"
#define PCMK_XA_CLIENT                      "client"
#define PCMK_XA_COMPLETED                   "completed"
#define PCMK_XA_CONTROL_PORT                "control-port"
#define PCMK_XA_CRM_DEBUG_ORIGIN            "crm-debug-origin"
#define PCMK_XA_CRM_FEATURE_SET             "crm_feature_set"
#define PCMK_XA_CRM_TIMESTAMP               "crm-timestamp"
#define PCMK_XA_DAYS                        "days"
#define PCMK_XA_DC_UUID                     "dc-uuid"
#define PCMK_XA_DEFAULT                     "default"
#define PCMK_XA_DELEGATE                    "delegate"
#define PCMK_XA_DESCRIPTION                 "description"
#define PCMK_XA_DEST                        "dest"
#define PCMK_XA_DEVICES                     "devices"
#define PCMK_XA_DISABLED                    "disabled"
#define PCMK_XA_DURATION                    "duration"
#define PCMK_XA_END                         "end"
#define PCMK_XA_EPOCH                       "epoch"
#define PCMK_XA_EXEC_TIME                   "exec-time"
#define PCMK_XA_EXECUTION_DATE              "execution-date"
#define PCMK_XA_EXIT_REASON                 "exit-reason"
#define PCMK_XA_EXPECTED_UP                 "expected_up"
#define PCMK_XA_EXTENDED_STATUS             "extended-status"
#define PCMK_XA_FAILED                      "failed"
#define PCMK_XA_FAILURE_IGNORED             "failure_ignored"
#define PCMK_XA_FEATURE_SET                 "feature_set"
#define PCMK_XA_FEATURES                    "features"
#define PCMK_XA_FIRST                       "first"
#define PCMK_XA_FIRST_ACTION                "first-action"
#define PCMK_XA_FORMAT                      "format"
#define PCMK_XA_HAVE_QUORUM                 "have-quorum"
#define PCMK_XA_HEALTH                      "health"
#define PCMK_XA_HOST                        "host"
#define PCMK_XA_HOST_INTERFACE              "host-interface"
#define PCMK_XA_HOST_NETMASK                "host-netmask"
#define PCMK_XA_HOURS                       "hours"
#define PCMK_XA_ID                          "id"
#define PCMK_XA_ID_AS_RESOURCE              "id_as_resource"
#define PCMK_XA_ID_REF                      "id-ref"
#define PCMK_XA_IMAGE                       "image"
#define PCMK_XA_INDEX                       "index"
#define PCMK_XA_INFLUENCE                   "influence"
#define PCMK_XA_INTERNAL_PORT               "internal-port"
#define PCMK_XA_IP_RANGE_START              "ip-range-start"
#define PCMK_XA_IS_DC                       "is_dc"
#define PCMK_XA_KIND                        "kind"
#define PCMK_XA_LANG                        "lang"
#define PCMK_XA_LAST_GRANTED                "last-granted"
#define PCMK_XA_LAST_RC_CHANGE              "last-rc-change"
#define PCMK_XA_LOCKED_TO                   "locked_to"
#define PCMK_XA_LOSS_POLICY                 "loss-policy"
#define PCMK_XA_MAINTENANCE                 "maintenance"
#define PCMK_XA_MANAGED                     "managed"
#define PCMK_XA_MINUTES                     "minutes"
#define PCMK_XA_MIXED_VERSION               "mixed_version"
#define PCMK_XA_MONTHDAYS                   "monthdays"
#define PCMK_XA_MONTHS                      "months"
#define PCMK_XA_MULTI_STATE                 "multi_state"
#define PCMK_XA_NAME                        "name"
#define PCMK_XA_NETWORK                     "network"
#define PCMK_XA_NEXT_ROLE                   "next-role"
#define PCMK_XA_NO_QUORUM_PANIC             "no-quorum-panic"
#define PCMK_XA_NODE                        "node"
#define PCMK_XA_NODE_ATTRIBUTE              "node-attribute"
#define PCMK_XA_NODES_RUNNING_ON            "nodes_running_on"
#define PCMK_XA_NUM_UPDATES                 "num_updates"
#define PCMK_XA_NUMBER                      "number"
#define PCMK_XA_OBJECT_TYPE                 "object-type"
#define PCMK_XA_ONLINE                      "online"
#define PCMK_XA_OP                          "op"
#define PCMK_XA_OPERATION                   "operation"
#define PCMK_XA_OPTIONS                     "options"
#define PCMK_XA_ORIGIN                      "origin"
#define PCMK_XA_ORPHANED                    "orphaned"
#define PCMK_XA_PATH                        "path"
#define PCMK_XA_PENDING                     "pending"
#define PCMK_XA_PORT                        "port"
#define PCMK_XA_PRESENT                     "present"
#define PCMK_XA_PROGRAM                     "program"
#define PCMK_XA_PROMOTED_MAX                "promoted-max"
#define PCMK_XA_PROMOTED_ONLY               "promoted-only"
#define PCMK_XA_PROVIDER                    "provider"
#define PCMK_XA_QUEUE_TIME                  "queue-time"
#define PCMK_XA_RANGE                       "range"
#define PCMK_XA_REASON                      "reason"
#define PCMK_XA_REFERENCE                   "reference"
#define PCMK_XA_RELOADABLE                  "reloadable"
#define PCMK_XA_REMOTE_CLEAR_PORT           "remote-clear-port"
#define PCMK_XA_REMOTE_TLS_PORT             "remote-tls-port"
#define PCMK_XA_REPLICAS                    "replicas"
#define PCMK_XA_REPLICAS_PER_HOST           "replicas-per-host"
#define PCMK_XA_REQUEST                     "request"
#define PCMK_XA_REQUIRE_ALL                 "require-all"
#define PCMK_XA_RESOURCE                    "resource"
#define PCMK_XA_RESOURCE_AGENT              "resource_agent"
#define PCMK_XA_RESOURCE_DISCOVERY          "resource-discovery"
#define PCMK_XA_RESOURCES_RUNNING           "resources_running"
#define PCMK_XA_RESULT                      "result"
#define PCMK_XA_ROLE                        "role"
#define PCMK_XA_RSC                         "rsc"
#define PCMK_XA_RSC_PATTERN                 "rsc-pattern"
#define PCMK_XA_RSC_ROLE                    "rsc-role"
#define PCMK_XA_RUN_COMMAND                 "run-command"
#define PCMK_XA_RUNNING                     "running"
#define PCMK_XA_SCOPE                       "scope"
#define PCMK_XA_SCORE                       "score"
#define PCMK_XA_SCORE_ATTRIBUTE             "score-attribute"
#define PCMK_XA_SEQUENTIAL                  "sequential"
#define PCMK_XA_SECONDS                     "seconds"
#define PCMK_XA_SHUTDOWN                    "shutdown"
#define PCMK_XA_SOURCE                      "source"
#define PCMK_XA_SOURCE_DIR                  "source-dir"
#define PCMK_XA_SOURCE_DIR_ROOT             "source-dir-root"
#define PCMK_XA_STANDBY                     "standby"
#define PCMK_XA_STANDBY_ONFAIL              "standby_onfail"
#define PCMK_XA_START                       "start"
#define PCMK_XA_STATUS                      "status"
#define PCMK_XA_SYMMETRICAL                 "symmetrical"
#define PCMK_XA_TARGET                      "target"
#define PCMK_XA_TARGET_ATTRIBUTE            "target-attribute"
#define PCMK_XA_TARGET_DIR                  "target-dir"
#define PCMK_XA_TARGET_PATTERN              "target-pattern"
#define PCMK_XA_TARGET_ROLE                 "target_role"
#define PCMK_XA_TARGET_VALUE                "target-value"
#define PCMK_XA_TEMPLATE                    "template"
#define PCMK_XA_TICKET                      "ticket"
#define PCMK_XA_TIME                        "time"
#define PCMK_XA_THEN                        "then"
#define PCMK_XA_THEN_ACTION                 "then-action"
#define PCMK_XA_TYPE                        "type"
#define PCMK_XA_UNAME                       "uname"
#define PCMK_XA_UNCLEAN                     "unclean"
#define PCMK_XA_UNIQUE                      "unique"
#define PCMK_XA_UPDATE_CLIENT               "update-client"
#define PCMK_XA_UPDATE_ORIGIN               "update-origin"
#define PCMK_XA_UPDATE_USER                 "update-user"
#define PCMK_XA_USER                        "user"
#define PCMK_XA_VALIDATE_WITH               "validate-with"
#define PCMK_XA_VALUE                       "value"
#define PCMK_XA_VALUE_SOURCE                "value-source"
#define PCMK_XA_VERSION                     "version"
#define PCMK_XA_WEEKDAYS                    "weekdays"
#define PCMK_XA_WEEKS                       "weeks"
#define PCMK_XA_WEEKYEARS                   "weekyears"
#define PCMK_XA_WEIGHT                      "weight"
#define PCMK_XA_WHEN                        "when"
#define PCMK_XA_WITH_QUORUM                 "with_quorum"
#define PCMK_XA_WITH_RSC                    "with-rsc"
#define PCMK_XA_WITH_RSC_ROLE               "with-rsc-role"
#define PCMK_XA_XPATH                       "xpath"
#define PCMK_XA_YEARDAYS                    "yeardays"
#define PCMK_XA_YEARS                       "years"


#  define ID(x) crm_element_value(x, PCMK_XA_ID)

#ifdef __cplusplus
}
#endif

#endif
