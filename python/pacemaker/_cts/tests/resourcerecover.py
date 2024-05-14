"""Fail a random resource and verify its fail count increases."""

__copyright__ = "Copyright 2000-2024 the Pacemaker project contributors"
__license__ = "GNU General Public License version 2 or later (GPLv2+) WITHOUT ANY WARRANTY"

from pacemaker._cts.audits import AuditResource
from pacemaker._cts.tests.ctstest import CTSTest
from pacemaker._cts.tests.simulstartlite import SimulStartLite
from pacemaker._cts.tests.starttest import StartTest
from pacemaker._cts.timer import Timer

# Disable various pylint warnings that occur in so many places throughout this
# file it's easiest to just take care of them globally.  This does introduce the
# possibility that we'll miss some other cause of the same warning, but we'll
# just have to be careful.

# pylint doesn't understand that self._rsh is callable.
# pylint: disable=not-callable


class ResourceRecover(CTSTest):
    """Fail a random resource."""

    def __init__(self, cm):
        """
        Create a new ResourceRecover instance.

        Arguments:
        cm -- A ClusterManager instance
        """
        CTSTest.__init__(self, cm)

        self.benchmark = True
        self.name = "ResourceRecover"

        self._action = "asyncmon"
        self._interval = 0
        self._rid = None
        self._rid_alt = None
        self._start = StartTest(cm)
        self._startall = SimulStartLite(cm)

    def __call__(self, node):
        """Perform this test."""
        self.incr("calls")

        if not self._startall(None):
            return self.failure("Setup failed")

        # List all resources active on the node (skip test if none)
        resourcelist = self._cm.active_resources(node)
        if not resourcelist:
            self._logger.log("No active resources on %s" % node)
            return self.skipped()

        # Choose one resource at random
        rsc = self._choose_resource(node, resourcelist)
        if rsc is None:
            return self.failure("Could not get details of resource '%s'" % self._rid)

        if rsc.id == rsc.clone_id:
            self.debug("Failing %s" % rsc.id)
        else:
            self.debug("Failing %s (also known as %s)" % (rsc.id, rsc.clone_id))

        # Log patterns to watch for (failure, plus restart if managed)
        pats = [
            self.templates["Pat:CloneOpFail"] % (self._action, rsc.id, rsc.clone_id)
        ]

        if rsc.managed:
            pats.append(self.templates["Pat:RscOpOK"] % ("stop", self._rid))

            if rsc.unique:
                pats.append(self.templates["Pat:RscOpOK"] % ("start", self._rid))
            else:
                # Anonymous clones may get restarted with a different clone number
                pats.append(self.templates["Pat:RscOpOK"] % ("start", ".*"))

        # Fail resource. (Ideally, we'd fail it twice, to ensure the fail count
        # is incrementing properly, but it might restart on a different node.
        # We'd have to temporarily ban it from all other nodes and ensure the
        # migration-threshold hasn't been reached.)
        if self._fail_resource(rsc, node, pats) is None:
            # self.failure() already called
            return None

        return self.success()

    def _choose_resource(self, node, resourcelist):
        """Choose a random resource to target."""
        self._rid = self._env.random_gen.choice(resourcelist)
        self._rid_alt = self._rid
        (_, lines) = self._rsh(node, "crm_resource -c", verbose=1)

        for line in lines:
            if line.startswith("Resource: "):
                rsc = AuditResource(self._cm, line)

                if rsc.id == self._rid:
                    # Handle anonymous clones that get renamed
                    self._rid = rsc.clone_id
                    return rsc

        return None

    def _get_failcount(self, node):
        """Check the fail count of targeted resource on given node."""
        cmd = "crm_failcount --quiet --query --resource %s --operation %s --interval %d --node %s"
        (rc, lines) = self._rsh(node, cmd % (self._rid, self._action, self._interval, node),
                                verbose=1)

        if rc != 0 or len(lines) != 1:
            lines = [line.strip() for line in lines]
            self._logger.log("crm_failcount on %s failed (%d): %s" % (node, rc, " // ".join(lines)))
            return -1

        try:
            failcount = int(lines[0])
        except (IndexError, ValueError):
            self._logger.log("crm_failcount output on %s unparseable: %s" % (node, " ".join(lines)))
            return -1

        return failcount

    def _fail_resource(self, rsc, node, pats):
        """Fail the targeted resource, and verify as expected."""
        orig_failcount = self._get_failcount(node)

        watch = self.create_watch(pats, 60)
        watch.set_watch()

        self._rsh(node, "crm_resource -V -F -r %s -H %s &>/dev/null" % (self._rid, node))

        with Timer(self._logger, self.name, "recover"):
            watch.look_for_all()

        self._cm.cluster_stable()
        recovered = self._cm.resource_location(self._rid)

        if watch.unmatched:
            return self.failure("Patterns not found: %r" % watch.unmatched)

        if rsc.unique and len(recovered) > 1:
            return self.failure("%s is now active on more than one node: %r" % (self._rid, recovered))

        if recovered:
            self.debug("%s is running on: %r" % (self._rid, recovered))

        elif rsc.managed:
            return self.failure("%s was not recovered and is inactive" % self._rid)

        new_failcount = self._get_failcount(node)
        if new_failcount != orig_failcount + 1:
            return self.failure("%s fail count is %d not %d"
                                % (self._rid, new_failcount, orig_failcount + 1))

        # Anything but None is success
        return 0

    @property
    def errors_to_ignore(self):
        """Return a list of errors which should be ignored."""
        return [
            r"Updating failcount for %s" % self._rid,
            r"schedulerd.*: Recover\s+(%s|%s)\s+\(.*\)" % (self._rid, self._rid_alt),
            r"Unknown operation: fail",
            self.templates["Pat:RscOpOK"] % (self._action, self._rid),
            r"(ERROR|error).*: Action %s_%s_%d .* initiated outside of a transition" % (self._rid, self._action, self._interval)
        ]
