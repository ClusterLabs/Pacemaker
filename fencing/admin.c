/* 
 * Copyright (C) 2009 Andrew Beekhof <andrew@beekhof.net>
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <crm_internal.h>

#include <sys/param.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/utsname.h>

#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>

#include <crm/crm.h>
#include <crm/msg_xml.h>
#include <crm/common/ipc.h>
#include <crm/common/cluster.h>

#include <crm/stonith-ng.h>
#include <crm/cib.h>
#include <crm/pengine/status.h>

#include <crm/common/xml.h>
#include <crm/common/msg.h>

/* *INDENT-OFF* */
static struct crm_option long_options[] = {
    {"help",        0, 0, '?', "\tThis text"},
    {"version",     0, 0, '$', "\tVersion information"  },
    {"verbose",     0, 0, 'V', "\tIncrease debug output"},
    {"quiet",       0, 0, 'q', "\tPrint only essential output"},

    {"-spacer-",    0, 0, '-', "Commands:"},
    {"list",            1, 0, 'l', "List devices that can terminate the specified host"},
    {"list-registered", 0, 0, 'L', "List all registered devices"},
    {"list-installed",  0, 0, 'I', "List all installed devices"},

    {"-spacer-",    0, 0, '-', ""},
    {"metadata",    0, 0, 'M', "Check the device's metadata"},
    {"query",       1, 0, 'Q', "Check the device's status"},
    {"fence",       1, 0, 'F', "Fence the named host"},
    {"unfence",     1, 0, 'U', "Unfence the named host"},
    {"reboot",      1, 0, 'B', "Reboot the named host"},
    {"confirm",     1, 0, 'C', "Confirm the named host is now safely down"},
    {"history",     1, 0, 'H', "Retrieve last fencing operation"},

    {"-spacer-",    0, 0, '-', ""},
    {"register",    1, 0, 'R', "Register the named stonith device. Requires: --agent, optional: --option"},
    {"deregister",  1, 0, 'D', "De-register the named stonith device"},

    {"register-level",    1, 0, 'r', "Register a stonith level for the named host. Requires: --index, one or more --device entries"},
    {"deregister-level",  1, 0, 'd', "De-register a stonith level for the named host. Requires: --index"},
    
    {"-spacer-",    0, 0, '-', ""},
    {"-spacer-",    0, 0, '-', "Options and modifiers:"},
    {"agent",       1, 0, 'a', "The agent (eg. fence_xvm) to instantiate when calling with --register"},
    {"env-option",  1, 0, 'e'},
    {"option",      1, 0, 'o'},

    {"device",      1, 0, 'v', "A device to associate with a given host and stonith level"},
    {"index",       1, 0, 'i', "The stonith level (1-9)"},

    {"timeout",     1, 0, 't', "Operation timeout in seconds"},

    {"list-all",    0, 0, 'L', "legacy alias for --list-registered"},

    {0, 0, 0, 0}
};
/* *INDENT-ON* */

int st_opts = st_opt_sync_call;

int
main(int argc, char ** argv)
{
    int flag;
    int rc = 0;
    int quiet = 0;
    int verbose = 0;
    int argerr = 0;
    int timeout = 120;
    int option_index = 0;
    int fence_level = 0;

    char name[512];
    char value[512];
    const char *agent = NULL;
    const char *device = NULL;
    const char *target = NULL;
    
    char action = 0;
    stonith_t *st = NULL;
    stonith_key_value_t *params = NULL;
    stonith_key_value_t *devices = NULL;
    stonith_key_value_t *dIter = NULL;
    
    crm_log_init(NULL, LOG_INFO, TRUE, FALSE, argc, argv);
    crm_set_options(NULL, "mode [options]", long_options,
		    "Provides access to the stonith-ng API.\n"
                    "\nAllows the administrator to add/remove/list devices, check device and host status and fence hosts\n");

    while (1) {
	flag = crm_get_option(argc, argv, &option_index);
	if (flag == -1)
	    break;
		
	switch(flag) {
	    case 'V':
		verbose = 1;
		crm_bump_log_level();
		break;
	    case '$':
	    case '?':
		crm_help(flag, LSB_EXIT_OK);
		break;
	    case 'L':
	    case 'I':
		action = flag;
		break;
	    case 'q':
		quiet = 1;
		break;
	    case 'Q':
	    case 'R':
	    case 'D':
		action = flag;
		device = optarg;
		break;
	    case 'a':
		agent = optarg;
		break;
	    case 'l':
		target = optarg;
		action = 'L';
		break;
	    case 'M':
		action = flag;
		break;
            case 't':
                timeout = crm_atoi(optarg, NULL);
                break;
	    case 'B':
	    case 'F':
	    case 'U':
	    case 'C':
	    case 'H':
	    case 'r':
	    case 'd':
		target = optarg;
		action = flag;
		break;
            case 'i':
                fence_level = crm_atoi(optarg, NULL);
                break;
            case 'v':
                devices = stonith_key_value_add(devices, NULL, optarg);
                break;
	    case 'o':
		crm_info("Scanning: -o %s", optarg);
		rc = sscanf(optarg, "%[^=]=%[^=]", name, value);
		if(rc != 2) {
		    crm_err("Invalid option: -o %s", optarg);
		    ++argerr;
		} else {
		    crm_info("Got: '%s'='%s'", name, value);
		    stonith_key_value_add(params, name, value);
		}
		break;
	    case 'e':
		{
		    char *key = crm_concat("OCF_RESKEY", optarg, '_');
		    const char *env = getenv(key);
		    
		    if(env == NULL) {
			crm_err("Invalid option: -e %s", optarg);
			++argerr;
		    } else {
			crm_info("Got: '%s'='%s'", optarg, env);
                        stonith_key_value_add( params, optarg, env);
		    }
		}
		break;
	    default:
		++argerr;
		break;
	}
    }

    if (optind > argc) {
	++argerr;
    }
    
    if (argerr) {
	crm_help('?', LSB_EXIT_GENERIC);
    }

    crm_debug("Create");
    st = stonith_api_new();

    if(action != 'M' && action != 'I') {
	rc = st->cmds->connect(st, crm_system_name, NULL);
	crm_debug("Connect: %d", rc);

	if(rc < 0) {
	    goto done;
	}
    }
    
    switch(action)
    {
	case 'I':
	    rc = st->cmds->list(st, st_opt_sync_call, NULL, &devices, timeout);
	    for(dIter = devices; dIter; dIter = dIter->next ) {
		fprintf( stdout, " %s\n", dIter->value );
	    }
	    if(rc == 0) {
		fprintf(stderr, "No devices found\n");

	    } else if(rc > 0) {
		fprintf(stderr, "%d devices found\n", rc);
		rc = 0;
	    }
	    stonith_key_value_freeall(devices, 1, 1);
	    break;
	    
	case 'L':
	    rc = st->cmds->query(st, st_opts, target, &devices, timeout);
	    for(dIter = devices; dIter; dIter = dIter->next ) {
		fprintf( stdout, " %s\n", dIter->value );
	    }
	    if(rc == 0) {
		fprintf(stderr, "No devices found\n");

	    } else if(rc > 0) {
		fprintf(stderr, "%d devices found\n", rc);
		rc = 0;
	    }
	    stonith_key_value_freeall(devices, 1, 1);
	    break;
	case 'Q':
	    rc = st->cmds->call(st, st_opts, device, "monitor", NULL, timeout);
	    if(rc < 0) {
		rc = st->cmds->call(st, st_opts, device, "list", NULL, timeout);
	    }
	    break;
	case 'R':
	    rc = st->cmds->register_device(st, st_opts, device, "stonith-ng",
                agent, params);
	    break;
	case 'D':
	    rc = st->cmds->remove_device(st, st_opts, device);
	    break;
	case 'r':
	    rc = st->cmds->register_level(st, st_opts, target, fence_level, devices);
	    break;
	case 'd':
	    rc = st->cmds->remove_level(st, st_opts, target, fence_level);
	    break;
	case 'M':
	    if (agent == NULL) {
		printf("Please specify an agent to query using -a,--agent [value]\n");
		return -1;
		
	    } else {
	        char *buffer = NULL;
		st->cmds->metadata(st, st_opt_sync_call, agent, NULL, &buffer, timeout);
		printf("%s\n", buffer);
		crm_free(buffer);
	    }
	    break;
	    
	case 'C':
	    rc = st->cmds->confirm(st, st_opts, target);
	    break;
	case 'B':
	    rc = st->cmds->fence(st, st_opts, target, "reboot", timeout);
	    break;
	case 'F':
	    rc = st->cmds->fence(st, st_opts, target, "off", timeout);
	    break;
	case 'U':
	    rc = st->cmds->fence(st, st_opts, target, "on", timeout);
	    break;
	case 'H':
	    {
		stonith_history_t *history, *hp, *latest = NULL;
		rc = st->cmds->history(st, st_opts, target, &history, timeout);
		for(hp = history; hp; hp = hp->next) {
		    char *action_s = NULL;
		    time_t complete = hp->completed;

		    if(hp->state == st_done) {
			latest = hp;
		    }

		    if(quiet || !verbose) {
			continue;
		    } else if(hp->action == NULL) {
			action_s = crm_strdup("unknown");
		    } else if(hp->action[0] != 'r') {
			action_s = crm_concat("turn", hp->action, ' ');
		    } else {
			action_s = crm_strdup(hp->action);
		    }
			
		    if(hp->state == st_failed) {
			printf("%s failed to %s node %s on behalf of %s at %s\n",
			       hp->delegate?hp->delegate:"We", action_s, hp->target, hp->origin,
			       ctime(&complete));

		    } else if(hp->state == st_done && hp->delegate) {
			printf("%s was able to %s node %s on behalf of %s at %s\n",
			       hp->delegate, action_s, hp->target, hp->origin, ctime(&complete));

		    } else if(hp->state == st_done) {
			printf("We were able to %s node %s on behalf of %s at %s\n",
			       action_s, hp->target, hp->origin, ctime(&complete));
		    } else {
			printf("%s wishes to %s node %s - %d %d\n",
			       hp->origin, action_s, hp->target, hp->state, hp->completed);
		    }
		    
		    crm_free(action_s);
	        }

		if(latest) {
		    if(quiet) {
			printf("%d\n", latest->completed);
		    } else {
			char *action_s = NULL;
			time_t complete = latest->completed;
			if(latest->action == NULL) {
			    action_s = crm_strdup("unknown");
			} else if(latest->action[0] != 'r') {
			    action_s = crm_concat("turn", latest->action, ' ');
			} else {
			    action_s = crm_strdup(latest->action);
			}
			
			printf("%s was able to %s node %s on behalf of %s at %s\n",
			       latest->delegate?latest->delegate:"We", action_s, latest->target,
			       latest->origin, ctime(&complete));
			
			crm_free(action_s);
		    }
		}
	    }
	    break;
    }    

  done:
    if(rc < 0) {
	printf("Command failed: %s\n", stonith_error2string(rc));
    }
    
    stonith_key_value_freeall(params, 1, 1);
    st->cmds->disconnect(st);
    crm_debug("Disconnect: %d", rc);

    crm_debug("Destroy");
    stonith_api_delete(st);
    
    return rc;
}
