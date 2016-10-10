ifelse(HAVE_INITSCRIPT, 1,
# If pacemaker is started via init script, the script may attempt to detect the
# cluster layer in use. This option forces it to recognize a particular type,
# in case its detection is inaccurate. Currently, the only value that is
# significant is "cman", which will cause the init script to start and stop
# important ancillary services so that services such as fenced and cman can
# reliably shut down. Any other value is ignored. The default is unset.
# PCMK_STACK=cman

)dnl
#==#==# Variables that control logging

# Enable debug logging globally or per-subsystem.
# Multiple subsystems may be listed separated by commas,
# e.g. PCMK_debug=crmd,pengine
# PCMK_debug=yes|no|crmd|pengine|cib|stonith-ng|attrd|pacemakerd

# Send detailed log messages to the specified file. Compared to messages logged
# via syslog, messages in this file may have extended information, and will
# include messages of "info" severity (and, if debug and/or trace logging
# has been enabled, those as well). This log is of more use to developers and
# advanced system administrators, and when reporting problems. By default,
# Pacemaker will use the value of logfile in corosync.conf, if found.
# PCMK_logfile=/var/log/pacemaker.log

# Enable logging via syslog, using the specified syslog facility. Messages sent
# here are of value to all Pacemaker users. This can be disabled using "none",
# but that is not recommended. The default is "daemon".
# PCMK_logfacility=none|daemon|user|local0|local1|local2|local3|local4|local5|local6|local7

# Unless syslog logging is disabled using PCMK_logfacility=none, messages of
# the specified severity and higher will be sent to syslog. The default value
# of "notice" is appropriate for most installations; "info" is highly verbose
# and "debug" is almost certain to send you blind (which is why there is a
# separate detail log specified by PCMK_logfile).
# PCMK_logpriority=emerg|alert|crit|error|warning|notice|info|debug

# Log all messages from a comma-separated list of functions.
# PCMK_trace_functions=function1,function2,function3

# Log all messages from a comma-separated list of files (no path).
# Wildcards are supported, e.g. PCMK_trace_files=prefix*.c
# PCMK_trace_files=file.c,other.h

# Log all messages matching comma-separated list of formats.
# PCMK_trace_formats="Sent delete %d"

# Log all messages from a comma-separated list of tags.
# PCMK_trace_tags=tag1,tag2

# Dump the blackbox whenever the message at function and line is emitted,
# e.g. PCMK_trace_blackbox=te_graph_trigger:223,unpack_clone:81
# PCMK_trace_blackbox=fn:line,fn2:line2,...

# Enable blackbox logging globally or per-subsystem. The blackbox contains a
# rolling buffer of all logs (including info, debug, and trace) and is written
# after a crash or assertion failure, and/or when SIGTRAP is received. The
# blackbox recorder can also be enabled for Pacemaker daemons at runtime by
# sending SIGUSR1 (or SIGTRAP), and disabled by sending SIGUSR2. Multiple
# subsystems may be listed separated by commas, e.g. PCMK_blackbox=crmd,pengine
# PCMK_blackbox=yes|no|crmd|pengine|cib|stonith-ng|attrd|pacemakerd

#==#==# Advanced use only

# If the cluster uses an older version of corosync (prior to 2.0), set this to
# "true", which will use a node's uname as its UUID. The default, "false", is
# appropriate for newer versions of corosync, and will use a node's corosync ID
# as its UUID. It is ignored by clusters that do not use corosync.
# PCMK_uname_is_uuid=false

# Specify an alternate location for RNG schemas and XSL transforms.
# (This is of use only to developers.)
# PCMK_schema_directory=/some/path

# Pacemaker consists of a master process with multiple subsidiary daemons. If
# one of the daemons crashes, the master process will normally attempt to
# restart it. If this is set to "true", the master process will instead panic
# the host (see PCMK_panic_action). The default is unset.
# PCMK_fail_fast=no

# Pacemaker will panic its host under certain conditions. If this is set to
# "crash", Pacemaker will trigger a kernel crash (which is useful if you want a
# kernel dump to investigate). For any other value, Pacemaker will trigger a
# host reboot. The default is unset.
# PCMK_panic_action=crash

#==#==# Pacemaker Remote
# Use the contents of this file as the authorization key to use with Pacemaker
# Remote connections. This file must be readable by Pacemaker daemons (that is,
# it must allow read permissions to either the hacluster user or the haclient
# group), and its contents must be identical on all nodes. The default is
# "/etc/pacemaker/authkey".
# PCMK_authkey_location=/etc/pacemaker/authkey

# Use this TCP port number when connecting to a Pacemaker Remote node. This
# value must be the same on all nodes. The default is "3121".
# PCMK_remote_port=3121

#==#==# IPC

# Force use of a particular class of IPC connection.
# PCMK_ipc_type=shared-mem|socket|posix|sysv

# Specify an IPC buffer size in bytes. This is useful when connecting to really
# big clusters that exceed the default 128KB buffer.
# PCMK_ipc_buffer=131072

#==#==# Profiling and memory leak testing (mainly useful to developers)

# Affect the behavior of glib's memory allocator. Setting to "always-malloc"
# when running under valgrind will help valgrind track malloc/free better;
# setting to "debug-blocks" when not running under valgrind will perform
# (somewhat expensive) memory checks.
# G_SLICE=always-malloc

# Uncommenting this will make malloc() initialize newly allocated memory
# and free() wipe it (to help catch uninitialized-memory/use-after-free).
# MALLOC_PERTURB_=221

# Uncommenting this will make malloc() and friends print to stderr and abort
# for some (inexpensive) memory checks.
# MALLOC_CHECK_=3

# Set to yes/no or cib,crmd etc. to run some or all daemons under valgrind.
# PCMK_valgrind_enabled=yes
# PCMK_valgrind_enabled=cib,crmd

# Set to yes/no or cib,crmd etc. to run some or all daemons under valgrind with
# the callgrind tool enabled.
# PCMK_callgrind_enabled=yes
# PCMK_callgrind_enabled=cib,crmd

# Set the options to pass to valgrind, when valgrind is enabled.
# VALGRIND_OPTS="--leak-check=full --trace-children=no --num-callers=25 --log-file=/var/lib/pacemaker/valgrind-%p --suppressions=/usr/share/pacemaker/tests/valgrind-pcmk.suppressions --gen-suppressions=all"
