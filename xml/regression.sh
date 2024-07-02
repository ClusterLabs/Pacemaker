#!/bin/sh
#
# Copyright 2018-2024 the Pacemaker project contributors
#
# The version control history for this file may have further details.
#
# This source code is licensed under the GNU General Public License version 2
# or later (GPLv2+) WITHOUT ANY WARRANTY.

set -eu

if [ ! -d "test-2" ]; then
    echo "$0 must be run from the xml subdirectory of a source tree"
    exit 1
fi

DIFF="diff -u"
DIFF_PAGER="less -LRX"
RNG_VALIDATOR="xmllint --noout --relaxng"
XSLT_PROCESSOR="xsltproc --nonet"

# Default tests
tests="test2to3 test2to3enter test2to3leave test2to3roundtrip cts_scheduler"

#
# commons
#

emit_result() {
    _er_howmany=${1:?}  # how many errors (0/anything else incl. strings)
    _er_subject=${2:?}
    _er_prefix=${3-}

    if [ -n "$_er_prefix" ]; then
        _er_prefix="${_er_prefix}: "
    fi

    if [ "$_er_howmany" = "0" ]; then
        printf "%s%s finished OK\n" "${_er_prefix}" "${_er_subject}"
    else
        printf "%s%s encountered ${_er_howmany} errors\n" \
            "${_er_prefix}" "${_er_subject}"
    fi
}

emit_error() {
    _ee_msg=${1:?}
    printf "%s\n" "${_ee_msg}" >&2
}

# returns 1 + floor of base 2 logaritm for _lo0r_i in 1...255,
# or 0 for _lo0r_i = 0
log2_or_0_return() {
    _lo0r_i=${1:?}
    return $(((!(_lo0r_i >> 1) && _lo0r_i) * 1 \
                + (!(_lo0r_i >> 2) && _lo0r_i & (1 << 1)) * 2 \
                + (!(_lo0r_i >> 3) && _lo0r_i & (1 << 2)) * 3 \
                + (!(_lo0r_i >> 4) && _lo0r_i & (1 << 3)) * 4 \
                + (!(_lo0r_i >> 5) && _lo0r_i & (1 << 4)) * 5 \
                + (!(_lo0r_i >> 6) && _lo0r_i & (1 << 5)) * 6 \
                + (!(_lo0r_i >> 7) && _lo0r_i & (1 << 6)) * 7 \
                + !!(_lo0r_i >> 7) * 7 ))
}

# rough addition of two base 2 logarithms
log2_or_0_add() {
    _lo0a_op1=${1:?}
    _lo0a_op2=${2:?}

    if [ "$_lo0a_op1" -gt "$_lo0a_op2" ]; then
        return ${_lo0a_op1}
    elif [ "$_lo0a_op2" -gt "$_lo0a_op1" ]; then
        return ${_lo0a_op2}
    elif [ "$_lo0a_op1" -gt 0 ]; then
        return $((_lo0a_op1 + 1))
    else
        return ${_lo0a_op1}
    fi
}

#
# test phases
#

# -r ... whether to remove referential files as well
# stdin: input file per line
test_cleaner() {
    _tc_cleanref=0

    while [ $# -gt 0 ]; do
        case "$1" in
            -r) _tc_cleanref=1;;
        esac
        shift
    done

    while read _tc_origin; do
        _tc_origin=${_tc_origin%.*}
        rm -f "${_tc_origin}.up" "${_tc_origin}.up.err"
        rm -f "$(dirname "${_tc_origin}")/.$(basename "${_tc_origin}").up"

        if [ "$_tc_cleanref" -eq 1 ]; then
            rm -f "${_tc_origin}.ref" "${_tc_origin}.ref.err"
        fi
    done
}

test_explanation() {
    _tsc_template=

    while [ $# -gt 0 ]; do
        case "$1" in
            -o=*) _tsc_template="upgrade-${1#-o=}.xsl";;
        esac
        shift
    done

    ${XSLT_PROCESSOR} upgrade-detail.xsl "${_tsc_template}"
}

cleanup_module_error() {
    # Work around a libxml2 bug. At least as of libxslt-1.1.41 and
    # libxml2-2.10.4, if the stylesheet contains a user-defined top-level
    # element (that is, one with a namespace other than the XSL namespace),
    # libxslt tries to load the namespace URI as an XML module. If this fails,
    # libxml2 logs a "module error: failed to open ..." message.
    #
    # This appears to be fixed in libxml2 v2.13 with commit ecb4c9fb.
    sed "/module error/d" "$1" > "$1.new"
    mv -- "$1.new" "$1"
}

# stdout: filename of the transformed file
test_runner_upgrade() {
    _tru_template=${1:?}
    _tru_source=${2:?}  # filename
    _tru_mode=${3:?}  # extra modes wrt. "referential" outcome, see below

    _tru_ref="${_tru_source%.*}.ref"

    if [ "$((_tru_mode & (1 << 0)))" -ne 0 ] || [ -f "${_tru_ref}.err" ]; then
        _tru_ref_err="${_tru_ref}.err"
    else
        _tru_ref_err=/dev/null
    fi

    _tru_proc_rc=0
    _tru_diff_rc=0
    _tru_target="${_tru_source%.*}.up"
    _tru_target_err="${_tru_target}.err"

    if [ "$((_tru_mode & (1 << 2)))" -eq 0 ]; then
        ${XSLT_PROCESSOR} "${_tru_template}" "${_tru_source}"   \
            > "${_tru_target}" 2> "${_tru_target_err}" \
        || _tru_proc_rc=$?

        cleanup_module_error "$_tru_target_err"

        if [ "$_tru_proc_rc" -ne 0 ]; then
            echo "$_tru_target_err"
            return "$_tru_proc_rc"
        fi
    else
        # when -B (deblanked outcomes handling) requested, we:
        # - drop blanks from the source XML
        #   (effectively emulating pacemaker handling)
        # - re-drop blanks from the XSLT outcome,
        #   which is compared with referential outcome
        #   processed with even greedier custom deblanking
        #   (extraneous inter-element whitespace like blank
        #   lines will not get removed otherwise, see lower)
        xmllint --noblanks "${_tru_source}" \
            | ${XSLT_PROCESSOR} "${_tru_template}" -  \
            > "${_tru_target}" 2> "${_tru_target_err}" \
        || _tru_proc_rc=$?

        cleanup_module_error "$_tru_target_err"

        if [ "$_tru_proc_rc" -ne 0 ]; then
            echo "$_tru_target_err"
            return "$_tru_proc_rc"
        fi

        # reusing variable no longer needed
        _tru_template="$(dirname "${_tru_target}")"
        _tru_template="${_tru_template}/.$(basename "${_tru_target}")"
        mv "${_tru_target}" "${_tru_template}"
        ${XSLT_PROCESSOR} - "${_tru_template}" > "${_tru_target}" <<EOF
<xsl:stylesheet version="1.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
<xsl:output method="xml" encoding="UTF-8" omit-xml-declaration="yes"/>
<xsl:template match="@*|*|comment()|processing-instruction()">
  <xsl:copy>
    <xsl:apply-templates select="@*|node()"/>
  </xsl:copy>
</xsl:template>
<xsl:template match="text()">
  <xsl:value-of select="normalize-space(.)"/>
</xsl:template>
</xsl:stylesheet>
EOF
    fi

    # only respond with the flags except for "-B", i.e., when both:
    # - _tru_mode non-zero
    # - "-B" in _tru_mode is zero (hence non-zero when flipped with XOR)
    if [ "$((_tru_mode * ((_tru_mode ^ (1 << 2)) & (1 << 2))))" -ne 0 ]; then
        if [ "$((_tru_mode & (1 << 0)))" -ne 0 ]; then
            cp -a "${_tru_target}" "${_tru_ref}"
            cp -a "${_tru_target_err}" "${_tru_ref_err}"
        fi
        if [ "$((_tru_mode & (1 << 1)))" -ne 0 ]; then
            { ${DIFF} "${_tru_source}" "${_tru_ref}" \
              && printf '\n(files match)\n'; } | ${DIFF_PAGER} >&2
            if [ $? -ne 0 ]; then
                printf "\npager failure\n" >&2
                return 1
            fi
            printf '\nIs comparison OK? ' >&2
            if read _tru_answer </dev/tty; then
                case "${_tru_answer}" in
                    y|yes) ;;
                    *) echo "Answer not 'y' nor 'yes'" >&2; return 1;;
                esac
            else
                return 1
            fi
        fi

    elif [ -f "$_tru_ref" ] && [ -e "$_tru_ref_err" ]; then
        if [ "$((_tru_mode & (1 << 2)))" -eq 0 ]; then
            _output=$(cat "$_tru_ref")
        else
            _output=$($XSLT_PROCESSOR - "$_tru_ref" <<EOF
<xsl:stylesheet version="1.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
<xsl:output method="xml" encoding="UTF-8" omit-xml-declaration="yes"/>
<xsl:template match="@*|*|comment()|processing-instruction()">
  <xsl:copy>
    <xsl:apply-templates select="@*|node()"/>
  </xsl:copy>
</xsl:template>
<xsl:template match="text()">
  <xsl:value-of select="normalize-space(.)"/>
</xsl:template>
</xsl:stylesheet>
EOF
)
        fi

        echo "$_output" | $DIFF - "$_tru_target" >&2 || _tru_diff_rc=$?
        if [ "$_tru_diff_rc" -eq 0 ]; then
            $DIFF "$_tru_ref_err" "$_tru_target_err" >&2 || _tru_diff_rc=$?
        fi
        if [ "$_tru_diff_rc" -ne 0 ]; then
            emit_error "Outputs differ from referential ones"
            echo "/dev/null"
            return 1
        fi
    else
        emit_error "Referential file(s) missing: ${_tru_ref}"
        echo "/dev/null"
        return 1
    fi

    echo "${_tru_target}"
}

test_runner_validate() {
    _trv_schema=${1:?}
    _trv_target=${2:?}  # filename

    if ! ${RNG_VALIDATOR} "${_trv_schema}" "${_trv_target}" \
        2>/dev/null; then
        ${RNG_VALIDATOR} "${_trv_schema}" "${_trv_target}"
    fi
}

# -a= ... action modifier completing template name (e.g. 2.10-(enter|leave))
# -o= ... which conventional version to deem as the transform origin
# -t= ... which conventional version to deem as the transform target
# -B
# -D
# -G ... see usage
# stdin: input file per line
test_runner() {
    _tr_mode=0
    _tr_ret=0
    _tr_action=
    _tr_schema_o=
    _tr_schema_t=
    _tr_target=
    _tr_template=

    while [ $# -gt 0 ]; do
        case "$1" in
            -a=*) _tr_action="${1#-a=}";;
            -o=*) _tr_template="${1#-o=}"
                  _tr_schema_o="pacemaker-${1#-o=}.rng";;
            -t=*) _tr_schema_t="pacemaker-${1#-t=}.rng";;
            -G) _tr_mode=$((_tr_mode | (1 << 0)));;
            -D) _tr_mode=$((_tr_mode | (1 << 1)));;
            -B) _tr_mode=$((_tr_mode | (1 << 2)));;
        esac
        shift
    done
    _tr_template="upgrade-${_tr_action:-${_tr_template:?}}.xsl"

    if [ ! -f "${_tr_schema_o:?}" ] || [ ! -f "${_tr_schema_t:?}" ]; then
        emit_error "Origin and/or target schema missing, rerun make"
        return 1
    fi

    while read _tr_origin; do
        printf '%-60s' "${_tr_origin}... "

        # pre-validate
        if ! test_runner_validate "${_tr_schema_o}" "${_tr_origin}"; then
            _tr_ret=$((_tr_ret + 1)); echo "E:pre-validate"; continue
        fi

        # upgrade
        if ! _tr_target=$(test_runner_upgrade "${_tr_template}" \
                          "${_tr_origin}" "${_tr_mode}"); then
            _tr_ret=$((_tr_ret + 1));
            if [ -z "$_tr_target" ]; then
                break
            fi

            echo "E:upgrade"
            if [ -s "$_tr_target" ]; then
                echo ---
                cat "$_tr_target" || :
                echo ---
            fi
            continue
        fi

        # post-validate
        if ! test_runner_validate "${_tr_schema_t}" "${_tr_target}"; then
            _tr_ret=$((_tr_ret + 1)); echo "E:post-validate"; continue
        fi

        echo "OK"
    done

    log2_or_0_return ${_tr_ret}
}

#
# particular test variations
# -C
# -X
# stdin: granular test specification(s) if any
#

test2to3() {
    _t23_pattern=

    while read _t23_spec; do
        _t23_spec=${_t23_spec%.xml}
        _t23_spec=${_t23_spec%\*}
        _t23_pattern="${_t23_pattern} -name ${_t23_spec}*.xml -o"
    done

    if [ -n "$_t23_pattern" ]; then
        _t23_pattern="( ${_t23_pattern%-o} )"
    fi

    find test-2 -name test-2 -o -type d -prune \
            -o -name '*.xml' ${_t23_pattern} -print \
        | env LC_ALL=C sort \
        | { case " $* " in
                *\ -C\ *) test_cleaner;;
                *\ -X\ *) test_explanation -o=2.10;;
                *) test_runner -o=2.10 -t=3.0 "$@" || return $?;;
            esac; }
}

test2to3enter() {
    _t23e_pattern=

    while read _t23e_spec; do
        _t23e_spec=${_t23e_spec%.xml}
        _t23e_spec=${_t23e_spec%\*}
        _t23e_pattern="${_t23e_pattern} -name ${_t23e_spec}*.xml -o"
    done

    if [ -n "$_t23e_pattern" ]; then
        _t23e_pattern="( ${_t23e_pattern%-o} )"
    fi

    find test-2-enter -name test-2-enter -o -type d -prune \
            -o -name '*.xml' ${_t23e_pattern} -print \
        | env LC_ALL=C sort \
        | { case " $* " in
                *\ -C\ *) test_cleaner;;
                *\ -X\ *) emit_result "not implemented" "option -X";;
                *) test_runner -a=2.10-enter -o=2.10 -t=2.10 "$@" || return $?;;
            esac; }
}

test2to3leave() {
    _t23l_pattern=

    while read _t23l_spec; do
        _t23l_spec=${_t23l_spec%.xml}
        _t23l_spec=${_t23l_spec%\*}
        _t23l_pattern="${_t23l_pattern} -name ${_t23l_spec}*.xml -o"
    done

    if [ -n "$_t23l_pattern" ]; then
        _t23l_pattern="( ${_t23l_pattern%-o} )"
    fi

    find test-2-leave -name test-2-leave -o -type d -prune \
            -o -name '*.xml' ${_t23l_pattern} -print \
        | env LC_ALL=C sort \
        | { case " $* " in
                *\ -C\ *) test_cleaner;;
                *\ -X\ *) emit_result "not implemented" "option -X";;
                *) test_runner -a=2.10-leave -o=3.0 -t=3.0 "$@" || return $?;;
            esac; }
}

test2to3roundtrip() {
    _t23rt_pattern=

    while read _t23tr_spec; do
        _t23rt_spec=${_t23rt_spec%.xml}
        _t23rt_spec=${_t23rt_spec%\*}
        _t23rt_pattern="${_t23rt_pattern} -name ${_t23rt_spec}*.xml -o"
    done

    if [ -n "$_t23rt_pattern" ]; then
        _t23rt_pattern="( ${_t23rt_pattern%-o} )"
    fi

    find test-2-roundtrip -name test-2-roundtrip -o -type d -prune \
            -o -name '*.xml' ${_t23rt_pattern} -print \
        | env LC_ALL=C sort \
        | { case " $* " in
                *\ -C\ *) test_cleaner;;
                *\ -X\ *) emit_result "not implemented" "option -X";;
                *) test_runner -a=2.10-roundtrip -o=2.10 -t=3.0 "$@" \
                   || return $?;;
            esac; }
}

# -B
# -D
# -G ... see usage
cts_scheduler() {
    _tcp_mode=0
    _tcp_ret=0
    _tcp_validatewith=
    _tcp_schema_o=
    _tcp_schema_t=
    _tcp_template=

    find ../cts/scheduler/xml -name '*.xml' | env LC_ALL=C sort \
        | { case " $* " in
                *\ -C\ *) test_cleaner -r;;
                *\ -X\ *) emit_result "not implemented" "option -X";;
                *)
        while [ $# -gt 0 ]; do
            case "$1" in
                -G) _tcp_mode=$((_tcp_mode | (1 << 0)));;
                -D) _tcp_mode=$((_tcp_mode | (1 << 1)));;
                -B) _tcp_mode=$((_tcp_mode | (1 << 2)));;
            esac
            shift
        done
        while read _tcp_origin; do
            _tcp_validatewith=$(${XSLT_PROCESSOR} - "${_tcp_origin}" <<EOF
<xsl:stylesheet version="1.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
<xsl:output method="text" encoding="UTF-8"/>
<xsl:template match="/">
  <xsl:choose>
    <xsl:when test="starts-with(cib/@validate-with, 'pacemaker-')">
      <xsl:variable name="Version" select="substring-after(cib/@validate-with, 'pacemaker-')"/>
      <xsl:choose>
        <xsl:when test="contains(\$Version, '.')">
          <xsl:value-of select="substring-before(\$Version, '.')"/>
        </xsl:when>
        <xsl:otherwise>
          <xsl:value-of select="cib/@validate-with"/>
        </xsl:otherwise>
      </xsl:choose>
    </xsl:when>
    <xsl:otherwise>
     <xsl:value-of select="cib/@validate-with"/>
    </xsl:otherwise>
  </xsl:choose>
</xsl:template>
</xsl:stylesheet>
EOF
)
            _tcp_schema_t=${_tcp_validatewith}
            case "${_tcp_validatewith}" in
                1) _tcp_schema_o=1.3;;
                2) _tcp_schema_o=2.10;;
                # only for gradual refinement as upgrade-2.10.xsl under
                # active development, move to 3.x when schema v4 emerges
                3) _tcp_schema_o=2.10
                   _tcp_schema_t=2;;
                *) emit_error \
                    "need to skip ${_tcp_origin} (schema: ${_tcp_validatewith})"
                    continue;;
            esac
            _tcp_template="upgrade-${_tcp_schema_o}.xsl"
            _tcp_schema_t="pacemaker-$((_tcp_schema_t + 1)).0.rng"

            if [ "${_tcp_schema_o%%.*}" = "${_tcp_validatewith}" ]; then
                _tcp_schema_o="pacemaker-${_tcp_schema_o}.rng"
            else
                _tcp_schema_o="${_tcp_schema_t}"
            fi

            # pre-validate
            if [ "$_tcp_schema_o" != "$_tcp_schema_t" ] \
                && ! test_runner_validate "$_tcp_schema_o" "$_tcp_origin"; then

                _tcp_ret=$((_tcp_ret + 1))
                echo "E:pre-validate"
                continue
            fi

            # upgrade
            if [ "$((_tcp_mode & (1 << 0)))" -eq 0 ]; then
                ln -fs "$(pwd)/$_tcp_origin" "${_tcp_origin%.*}.ref"
            fi

            if ! _tcp_target=$(test_runner_upgrade "${_tcp_template}" \
                                "${_tcp_origin}" "${_tcp_mode}"); then
                _tcp_ret=$((_tcp_ret + 1));

                if [ -z "$_tcp_target" ]; then
                    break
                fi

                echo "E:upgrade"
                if [ -s "$_tcp_target" ]; then
                    echo ---
                    cat "$_tcp_target" || :
                    echo ---
                fi
                continue
            fi

            if [ "$((_tcp_mode & (1 << 0)))" -eq 0 ]; then
                rm -f "${_tcp_origin%.*}.ref"
            fi

            # post-validate
            if ! test_runner_validate "${_tcp_schema_t}" "${_tcp_target}"; then
                _tcp_ret=$((_tcp_ret + 1)); echo "E:post-validate"; continue
            fi

            if [ "$((_tcp_mode & (1 << 0)))" -ne 0 ]; then
                mv "$_tcp_target" "$_tcp_origin"
            fi

        done; log2_or_0_return ${_tcp_ret};;
        esac; }
}

#
# "framework"
#

# option-likes ... options to be passed down
# argument-likes ... drives a test selection
test_suite() {
    _ts_pass=
    _ts_select=
    _ts_select_full=
    _ts_test_specs=
    _ts_global_ret=0
    _ts_ret=0

    while [ $# -gt 0 ]; do
        case "$1" in
            -) printf '%s\n' 'waiting for tests specified at stdin...';
                while read _ts_spec; do
                    _ts_select="${_ts_spec}@$1"
                done;;
            -*) _ts_pass="${_ts_pass} $1";;
            *) _ts_select_full="${_ts_select_full}@$1"
               _ts_select="${_ts_select}@${1%%/*}";;
        esac
        shift
    done
    _ts_select="${_ts_select}@"
    _ts_select_full="${_ts_select_full}@"

    for _ts_test in ${tests}; do

        _ts_test_specs=
        while true; do
            case "${_ts_select}" in
            *@${_ts_test}@*)
            _ts_test_specs="${_ts_select%%@${_ts_test}@*}"\
"@${_ts_select#*@${_ts_test}@}"
            if [ "$_ts_test_specs" = "@" ]; then
                _ts_select=  # nothing left
            else
                _ts_select="${_ts_test_specs}"
            fi
            continue
            ;;
            @) case "${_ts_test}" in test*) break;; esac  # filter
            ;;
            esac
            if [ -n "$_ts_test_specs" ]; then
                break
            fi
            continue 2  # move on to matching with next local test
        done

        _ts_test_specs=
        while true; do
            case "${_ts_select_full}" in
            *@${_ts_test}/*)
                _ts_test_full="${_ts_test}/${_ts_select_full#*@${_ts_test}/}"
                _ts_test_full="${_ts_test_full%%@*}"
                _ts_select_full="${_ts_select_full%%@${_ts_test_full}@*}"\
"@${_ts_select_full#*@${_ts_test_full}@}"
                _ts_test_specs="${_ts_test_specs} ${_ts_test_full#*/}"
            ;;
            *)
            break
            ;;
            esac
        done

        for _ts_test_spec in ${_ts_test_specs}; do
            printf '%s\n' "${_ts_test_spec}"
        done | "${_ts_test}" ${_ts_pass} || _ts_ret=$?

        if [ "$_ts_ret" = 0 ]; then
            emit_result "$_ts_ret" "$_ts_test"
        else
            emit_result "at least 2^$((_ts_ret - 1))" "$_ts_test"
        fi

        log2_or_0_add ${_ts_global_ret} ${_ts_ret}
        _ts_global_ret=$?
    done
    if [ -n "${_ts_select#@}" ]; then
        emit_error "Non-existing test(s):$(echo "${_ts_select}" \
                                            | tr '@' ' ')"
        log2_or_0_add ${_ts_global_ret} 1 || _ts_global_ret=$?
    fi

    return ${_ts_global_ret}
}

# NOTE: big letters are dedicated for per-test-set behaviour,
#       small ones for generic/global behaviour
usage() {
    printf \
'%s\n%s\n  %s\n  %s\n  %s\n  %s\n  %s\n  %s\n  %s\n  %s\n' \
    "usage: $0 [-{B,C,D,G,X}]* \\" \
          "       [-|{${tests## }}*]" \
    "- when no suites (arguments) provided, \"test*\" ones get used" \
    "- with '-' suite specification the actual ones grabbed on stdin" \
    "- use '-B' to run validate-only check suppressing blanks first" \
    "- use '-C' to only cleanup ephemeral byproducts" \
    "- use '-D' to review originals vs. \"referential\" outcomes" \
    "- use '-G' to generate \"referential\" outcomes" \
    "- use '-X' to show explanatory details about the upgrade" \
    "- test specification can be granular, e.g. 'test2to3/022'"
}

main() {
    _main_pass=
    _main_bailout=0
    _main_ret=0

    while [ $# -gt 0 ]; do
        case "$1" in
            -h) usage; exit;;
            -C|-G|-X) _main_bailout=1;;
        esac
        _main_pass="${_main_pass} $1"
        shift
    done

    test_suite ${_main_pass} || _main_ret=$?

    if [ "$_main_bailout" -eq 0 ]; then
        test_suite -C $_main_pass >/dev/null || true
    fi

    if [ "$_main_ret" = 0 ]; then
        emit_result "$_main_ret" "Overall suite"
    else
        emit_result "at least 2^$((_main_ret - 1))" "Overall suite"
    fi

    return ${_main_ret}
}

main "$@"
