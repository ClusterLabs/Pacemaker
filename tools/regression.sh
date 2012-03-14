#!/bin/bash

: ${shadow=tools-regression}
test_home=`dirname $0`
num_errors=0
num_passed=0
GREP_OPTIONS=

function assert() {
    rc=$1; shift
    target=$1; shift
    app=$1; shift
    msg=$1; shift
    exit_code=$1; shift

    cibadmin -Q

    if [ $rc -ne $target ]; then
	num_errors=`expr $num_errors + 1`
	printf "* Failed (rc=%.3d): %-14s - %s\n" $rc $app "$msg"
	if [ ! -z $exit_code ]; then
	    echo "Aborting tests"
	    exit $exit_code
	fi
	exit 1
    else
	printf "* Passed: %-14s - %s\n" $app "$msg"
	num_passed=`expr $num_passed + 1`
    fi
}

function usage() {
    echo "Usage: ./regression.sh [-s(ave)] [-x] [-v(erbose)]"
    exit $1
}

done=0
do_save=0
VALGRIND_CMD=
while test "$done" = "0"; do
    case "$1" in
	-V|--verbose) verbose=1; shift;;
	-v|--valgrind) 
	    export G_SLICE=always-malloc
	    VALGRIND_CMD="valgrind -q --show-reachable=no --leak-check=full --trace-children=no --time-stamp=yes --num-callers=20 --suppressions=$test_home/cli.supp"
	    shift;;
	-x) set -x; shift;;
	-s) do_save=1; shift;;
	-p) PATH="$2:$PATH"; export PATH; shift 1;;
	-?) usage 0;;
	-*) echo "unknown option: $1"; usage 1;;
	*) done=1;;
    esac
done

if [ "x$VALGRIND_CMD" = "x" -a -x $test_home/crm_simulate ]; then
    echo "Using local binaries from: $test_home"
    PATH="$test_home:$PATH"
fi

function test_tools() {
    export CIB_shadow_dir=$test_home
    $VALGRIND_CMD crm_shadow --batch --force --create-empty $shadow
    export CIB_shadow=$shadow
    $VALGRIND_CMD cibadmin -Q
    
    $VALGRIND_CMD cibadmin -E 
    assert $? 1 cibadmin "Require --force for CIB erasure"
    
    $VALGRIND_CMD cibadmin -E --force
    assert $? 0 cibadmin "Allow CIB erasure with --force"
    
    $VALGRIND_CMD cibadmin -Q > /tmp/$$.existing.xml
    assert $? 0 cibadmin "Query CIB"

    $VALGRIND_CMD crm_attribute -n cluster-delay -v 60s
    assert $? 0 crm_attribute "Set cluster option"

    $VALGRIND_CMD cibadmin -Q -o crm_config | grep cib-bootstrap-options-cluster-delay 
    assert $? 0 cibadmin "Query new cluster option"

    $VALGRIND_CMD cibadmin -Q -o crm_config > /tmp/$$.opt.xml
    assert $? 0 cibadmin "Query cluster options"
    
    $VALGRIND_CMD cibadmin -D -o crm_config --xml-text '<nvpair id="cib-bootstrap-options-cluster-delay"/>'
    assert $? 0 cibadmin "Delete nvpair"
    
    $VALGRIND_CMD cibadmin -C -o crm_config --xml-file /tmp/$$.opt.xml 
    assert $? 21 cibadmin "Create operaton should fail with: -21, The object already exists"
    
    $VALGRIND_CMD cibadmin -M -o crm_config --xml-file /tmp/$$.opt.xml
    assert $? 0 cibadmin "Modify cluster options section"
    
    $VALGRIND_CMD cibadmin -Q -o crm_config | grep cib-bootstrap-options-cluster-delay 
    assert $? 0 cibadmin "Query updated cluster option"
    
    $VALGRIND_CMD crm_attribute -n cluster-delay -v 40s -s duplicate 
    assert $? 0 crm_attribute "Set duplicate cluster option"
    
    $VALGRIND_CMD crm_attribute -n cluster-delay -v 30s 
    assert $? 216 crm_attribute "Setting multiply defined cluster option should fail with -216, Could not set cluster option"
    
    $VALGRIND_CMD crm_attribute -n cluster-delay -v 30s -s duplicate
    assert $? 0 crm_attribute "Set cluster option with -s"
    
    $VALGRIND_CMD crm_attribute -n cluster-delay -D -i cib-bootstrap-options-cluster-delay
    assert $? 0 crm_attribute "Delete cluster option with -i"
    
    $VALGRIND_CMD cibadmin -C -o nodes --xml-text '<node id="clusterNode-UUID" uname="clusterNode-UNAME" type="member">'
    assert $? 0 cibadmin "Create node entry"
    
    $VALGRIND_CMD cibadmin -C -o status --xml-text '<node_state id="clusterNode-UUID" uname="clusterNode-UNAME"/>'
    assert $? 0 cibadmin "Create node status entry"
        
    $VALGRIND_CMD crm_attribute -n ram -v 1024M -U clusterNode-UNAME -t nodes
    assert $? 0 crm_attribute "Create node attribute"
    
    $VALGRIND_CMD cibadmin -Q -o nodes | grep clusterNode-UUID-ram 
    assert $? 0 cibadmin "Query new node attribute"
    
    $VALGRIND_CMD cibadmin -Q | cibadmin -5 -p 2>&1 > /dev/null
    assert $? 0 cibadmin "Digest calculation"
    
    # This update will fail because it has version numbers
    $VALGRIND_CMD cibadmin -R --xml-file /tmp/$$.existing.xml
    assert $? 45 cibadmin "Replace operation should fail with: -45, Update was older than existing configuration"

    crm_standby -N clusterNode-UNAME -G
    assert $? 0 crm_standby "Default standby value"

    crm_standby -N clusterNode-UNAME -v true
    assert $? 0 crm_standby "Set standby status"

    crm_standby -N clusterNode-UNAME -G
    assert $? 0 crm_standby "Query standby value"
    
    crm_standby -N clusterNode-UNAME -D
    assert $? 0 crm_standby "Delete standby value"
    
    $VALGRIND_CMD cibadmin -C -o resources --xml-text '<primitive id="dummy" class="ocf" provider="pacemaker" type="Dummy"/>'
    assert $? 0 cibadmin "Create a resource"

    $VALGRIND_CMD crm_resource -r dummy --meta -p is-managed -v false
    assert $? 0 crm_resource "Create a resource meta attribute"

    $VALGRIND_CMD crm_resource -r dummy --meta -g is-managed
    assert $? 0 crm_resource "Query a resource meta attribute"

    $VALGRIND_CMD crm_resource -r dummy --meta -d is-managed
    assert $? 0 crm_resource "Remove a resource meta attribute"

    $VALGRIND_CMD crm_resource -r dummy -p delay -v 10s
    assert $? 0 crm_resource "Create a resource attribute"

    $VALGRIND_CMD crm_resource -L
    assert $? 0 crm_resource "List the configured resources"

    crm_failcount -r dummy -v 10 -N clusterNode-UNAME
    assert $? 0 crm_resource "Set a resource's fail-count"

    $VALGRIND_CMD crm_resource -r dummy -M
    assert $? 244 crm_resource "Require a destination when migrating a resource that is stopped"

    $VALGRIND_CMD crm_resource -r dummy -M -N i.dont.exist
    assert $? 234 crm_resource "Don't support migration to non-existant locations"

    $VALGRIND_CMD crm_resource -r dummy -M -N clusterNode-UNAME
    assert $? 0 crm_resource "Migrate a resource"

    $VALGRIND_CMD crm_resource -r dummy -U
    assert $? 0 crm_resource "Un-migrate a resource"

    crm_ticket -t ticketA -G
    assert $? 0 crm_ticket "Default granted-ticket value"

    crm_ticket -t ticketA -v false --force
    assert $? 0 crm_ticket "Set granted-ticket value"

    crm_ticket -t ticketA -G
    assert $? 0 crm_ticket "Query granted-ticket value"
    
    crm_ticket -t ticketA -D --force
    assert $? 0 crm_ticket "Delete granted-ticket value"
 }

test_tools 2>&1 | sed s/cib-last-written.*\>/\>/ > $test_home/regression.out
rc=$?

if [ $do_save = 1 ]; then
    cp $test_home/regression.out $test_home/regression.exp
fi

grep -e "^*" $test_home/regression.out
diff -u $test_home/regression.exp $test_home/regression.out 
diff_rc=$?

if [ $rc != 0 ]; then
    echo Tests failed
    exit 1

elif [ $diff_rc != 0 ]; then
    echo Tests passed but diff failed
    exit 2

else
    echo Tests passed
    exit 0
fi
