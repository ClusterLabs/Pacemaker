"""Put a node into standby mode and check that resources migrate."""

__all__ = ["StandbyTest"]
__copyright__ = "Copyright 2000-2024 the Pacemaker project contributors"
__license__ = "GNU General Public License version 2 or later (GPLv2+) WITHOUT ANY WARRANTY"

from pacemaker._cts.tests.ctstest import CTSTest
from pacemaker._cts.tests.simulstartlite import SimulStartLite
from pacemaker._cts.tests.starttest import StartTest

# Disable various pylint warnings that occur in so many places throughout this
# file it's easiest to just take care of them globally.  This does introduce the
# possibility that we'll miss some other cause of the same warning, but we'll
# just have to be careful.

# pylint doesn't understand that self._env is subscriptable.
# pylint: disable=unsubscriptable-object


class StandbyTest(CTSTest):
    """Put a node into standby and check that resources migrate away from it."""

    def __init__(self, cm):
        """
        Create a new StandbyTest instance.

        Arguments:
        cm -- A ClusterManager instance
        """
        CTSTest.__init__(self, cm)

        self.benchmark = True
        self.name = "Standby"

        self._start = StartTest(cm)
        self._startall = SimulStartLite(cm)

    # make sure the node is active
    # set the node to standby mode
    # check resources, none resource should be running on the node
    # set the node to active mode
    # check resources, resources should have been migrated back (SHOULD THEY?)

    def __call__(self, node):
        """Perform this test."""
        self.incr("calls")
        ret = self._startall(None)
        if not ret:
            return self.failure("Start all nodes failed")

        self.debug("Make sure node %s is active" % node)
        if self._cm.in_standby_mode(node):
            if not self._cm.set_standby_mode(node, False):
                return self.failure("can't set node %s to active mode" % node)

        self._cm.cluster_stable()

        if self._cm.in_standby_mode(node):
            return self.failure("standby status of %s is [on] but we expect [off]" % node)

        watchpats = [
            r"State transition .* -> S_POLICY_ENGINE",
        ]
        watch = self.create_watch(watchpats, self._env["DeadTime"] + 10)
        watch.set_watch()

        self.debug("Setting node %s to standby mode" % node)
        if not self._cm.set_standby_mode(node, True):
            return self.failure("can't set node %s to standby mode" % node)

        self.set_timer("on")

        ret = watch.look_for_all()
        if not ret:
            self._logger.log("Patterns not found: %r" % watch.unmatched)
            self._cm.set_standby_mode(node, False)
            return self.failure("cluster didn't react to standby change on %s" % node)

        self._cm.cluster_stable()

        if not self._cm.in_standby_mode(node):
            return self.failure("standby status of %s is [off] but we expect [on]" % node)

        self.log_timer("on")

        self.debug("Checking resources")
        rscs_on_node = self._cm.active_resources(node)
        if rscs_on_node:
            rc = self.failure("%s set to standby, %r is still running on it" % (node, rscs_on_node))
            self.debug("Setting node %s to active mode" % node)
            self._cm.set_standby_mode(node, False)
            return rc

        self.debug("Setting node %s to active mode" % node)
        if not self._cm.set_standby_mode(node, False):
            return self.failure("can't set node %s to active mode" % node)

        self.set_timer("off")
        self._cm.cluster_stable()

        if self._cm.in_standby_mode(node):
            return self.failure("standby status of %s is [on] but we expect [off]" % node)

        self.log_timer("off")

        return self.success()
