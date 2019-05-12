#!/bin/sh
# Copyright 2003-2019 the Pacemaker project contributors
#
# The version control history for this file may have further details.
#
# SPDX-License-Identifier: GPL-2.0-or-later

# portably check whether configure is up to date first unless arguments passed
if ! { test $# -ne 0 && ${BUILD_MAKE:-make} -q -r -f - configure; }; then
	autoreconf -fisv || exit $?
fi <<EOF
configure: autogen.sh configure.ac
	: pass
EOF

# convenience: use "./autogen.sh roll [along] --with-cibsecrets" etc.;
# this is mostly to force-prefer dash on platforms where /bin/sh ~ slow bash,
# and with "along", to force-prefer gmake on platforms where make is not GNU
if test $# -gt 0 && test "x$1" = xroll; then
	shift
	mk=$(command -v ${BUILD_MAKE:-gmake} 2>/dev/null \
	     || command -v make 2>/dev/null) || mk=:
	test $# -gt 0 && test "x$1" = xalong && shift && tailc=$mk || tailc=:
	# shell must support LINENO variable, but that's POSIX now
	# https://github.com/koalaman/shellcheck/issues/644
	for sh in ${BUILD_SH} dash ash 'busybox ash' sh bash; do
		shargs=${sh#* }; test "x$sh" != "x$shargs" || shargs=
		sh=$(command -v ${sh%% *} 2>/dev/null)
		#export CONFIG_SHELL=$sh  # no gain + extra shell re-exec
		test -x "$sh" && exec $sh $shargs -c "./configure \
		  \"CONFIG_SHELL=$sh${shargs:+ $shargs}\" \
		  \"MAKE=$mk\" \
		  \"SHELL=$sh${shargs:+ $shargs}\" \
		  $* && $tailc"
	done
else
	echo 'Now run ./configure and make'
fi
