# Semi-random collection of tasks we'd like to get done

## Targeted for 1.2
- Need a way to indicate when unfencing operations need to be initiated from the host to be unfenced

## Targeted for 1.2.x

- Support
  http://cgit.freedesktop.org/systemd/systemd/commit/?id=96342de68d0d6de71a062d984dafd2a0905ed9fe
- Add corosync to ComponentFail cts test
- Support 'yesterday' and 'thursday' and '24-04' as dates in crm_report 
- Allow the N in 'give up after N failed fencing attempts' to be configurable 
- Check for uppercase letters in node names, warn if found
- Imply startup-failure-is-fatal from on-fail="restart" 
- Show an english version of the config with crm_resource --rules
- Convert cts/CIB.py into a supported Python API for the CIB
- Reduce the amount of stonith-ng logging
- Use dlopen for snmp in crm_mon
- Re-implement no-quorum filter for cib updates?
- More concise logging of print_graph() when a transition is cancelled or fails

    warning: print_graph: Synapse 26 is pending (priority: 0)
    warning: print_elem:     [Action 11]: Pending (id: all_stopped, type: pseduo, priority: 0)
    warning: print_elem:      * [Input 36]: Pending (id: rsc_pcmk-1_stop_0, loc: pcmk-1, priority: 0)
    warning: print_elem:      * [Input 39]: Pending (id: rsc_pcmk-2_stop_0, loc: pcmk-3, priority: 0)
    warning: print_elem:      * [Input 42]: Pending (id: rsc_pcmk-3_stop_0, loc: pcmk-3, priority: 0)
    warning: print_elem:      * [Input 47]: Pending (id: migrator_stop_0, loc: pcmk-1, priority: 0)
    warning: print_elem:      * [Input 50]: Pending (id: ping-1_stop_0, loc: pcmk-3, priority: 0)
    warning: print_elem:      * [Input 53]: Pending (id: ping-1_stop_0, loc: pcmk-1, priority: 0)


## Targeted for 1.4

- Support A colocated with (B || C || D)
- Implement a truely atomic version of attrd
- Support rolling average values in attrd
- Support heartbeat with the mcp
- Freeze/Thaw
- Create Pacemaker plugin for snmpd - http://www.net-snmp.org/
- Investigate using a DB as the back-end for the CIB
- Decide whether to fully support or drop failover domains

# Testing
- Convert BandwidthTest CTS test into a Scenario wrapper
- find_operations() is not covered by PE regression tests
- no_quorum_policy==suicide is not covered by PE regression tests
- parse_xml_duration() is not covered by PE regression tests
- phase_of_the_moon() is not covered by PE regression tests
- test_role_expression() is not covered by PE regression tests
- native_parameter() is not covered by PE regression tests
- clone_active() is not covered by PE regression tests
- convert_non_atomic_task() in native.c is not covered by PE regression tests
- group_rsc_colocation_lh() is not covered by PE regression tests
- Test on-fail=standby

# Documentation
- Clusters from Scratch: Mail
- Clusters from Scratch: MySQL
- Document reload in Pacemaker Explained
- Document advanced fencing logic in Pacemaker Explained
- Use ann:defaultValue="..." instead of <optional> in the schema more often
- Allow Clusters from Scratch to be built in two flavors - pcs and crm shell
