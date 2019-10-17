/*
 * Copyright 2017-2019 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */
#ifndef CRM_COMMON_XML_INTERNAL__H
#  define CRM_COMMON_XML_INTERNAL__H

/*
 * Internal-only wrappers for and extensions to libxml2 (libxslt)
 */

#  include <stdlib.h>
#  include <stdio.h>
#  include <string.h>

#  include <crm/crm.h>  /* transitively imports qblog.h */

/*!
 * \internal
 * \brief Dummy marker to indicate the "calls itself directly".
 *
 * \note Having such a marker consistently used for what's inherently
 *       a tree algoritm (and commonly, in-depth XML processing is of
 *       this kind) is indispensable for a general reasoning in the
 *       context of the tree (is this function meant to only be applied
 *       to root/leave/complement of those two? etc.).
 *
 * \note Currently only applied in \c{acl.c}.
 */
#define PCMK__XML_DIRECTLY_RECURSIVE


/*!
 * \brief Base for directing lib{xml2,xslt} log into standard libqb backend
 *
 * This macro implements the core of what can be needed for directing
 * libxml2 or libxslt error messaging into standard, preconfigured
 * libqb-backed log stream.
 *
 * It's a bit unfortunate that libxml2 (and more sparsely, also libxslt)
 * emits a single message by chunks (location is emitted separatedly from
 * the message itself), so we have to take the effort to combine these
 * chunks back to single message.  Whether to do this or not is driven
 * with \p dechunk toggle.
 *
 * The form of a macro was chosen for implicit deriving of __FILE__, etc.
 * and also because static dechunking buffer should be differentiated per
 * library (here we assume different functions referring to this macro
 * will not ever be using both at once), preferably also per-library
 * context of use to avoid clashes altogether.
 *
 * Note that we cannot use qb_logt, because callsite data have to be known
 * at the moment of compilation, which it is not always the case -- xml_log
 * (and unfortunately there's no clear explanation of the fail to compile).
 *
 * Also note that there's no explicit guard against said libraries producing
 * never-newline-terminated chunks (which would just keep consuming memory),
 * as it's quite improbable.  Termination of the program in between the
 * same-message chunks will raise a flag with valgrind and the likes, though.
 *
 * And lastly, regarding how dechunking combines with other non-message
 * parameters -- for \p priority, most important running specification
 * wins (possibly elevated to LOG_ERR in case of nonconformance with the
 * newline-termination "protocol"), \p dechunk is expected to always be
 * on once it was at the start, and the rest (\p postemit and \p prefix)
 * are picked directly from the last chunk entry finalizing the message
 * (also reasonable to always have it the same with all related entries).
 *
 * \param[in] priority Syslog priority for the message to be logged
 * \param[in] dechunk  Whether to dechunk new-line terminated message
 * \param[in] postemit Code to be executed once message is sent out
 * \param[in] prefix   How to prefix the message or NULL for raw passing
 * \param[in] fmt      Format string as with printf-like functions
 * \param[in] ap       Variable argument list to supplement \p fmt format string
 */
#define CRM_XML_LOG_BASE(priority, dechunk, postemit, prefix, fmt, ap)          \
do {                                                                            \
    if (!(dechunk) && (prefix) == NULL) {  /* quick pass */                     \
        qb_log_from_external_source_va(__FUNCTION__, __FILE__, (fmt),           \
                                       (priority), __LINE__, 0, (ap));          \
        (void) (postemit);                                                      \
    } else {                                                                    \
        int CXLB_len = 0;                                                       \
        char *CXLB_buf = NULL;                                                  \
        static int CXLB_buffer_len = 0;                                         \
        static char *CXLB_buffer = NULL;                                        \
        static uint8_t CXLB_priority = 0;                                       \
                                                                                \
        CXLB_len = vasprintf(&CXLB_buf, (fmt), (ap));                           \
                                                                                \
        if (CXLB_len <= 0 || CXLB_buf[CXLB_len - 1] == '\n' || !(dechunk)) {    \
            if (CXLB_len < 0) {                                                 \
                CXLB_buf = (char *) "LOG CORRUPTION HAZARD"; /*we don't modify*/\
                CXLB_priority = QB_MIN(CXLB_priority, LOG_ERR);                 \
            } else if (CXLB_len > 0 /* && (dechunk) */                          \
                       && CXLB_buf[CXLB_len - 1] == '\n') {                     \
                CXLB_buf[CXLB_len - 1] = '\0';                                  \
            }                                                                   \
            if (CXLB_buffer) {                                                  \
                qb_log_from_external_source(__FUNCTION__, __FILE__, "%s%s%s",   \
                                            CXLB_priority, __LINE__, 0,         \
                                            (prefix) != NULL ? (prefix) : "",   \
                                            CXLB_buffer, CXLB_buf);             \
                free(CXLB_buffer);                                              \
            } else {                                                            \
                qb_log_from_external_source(__FUNCTION__, __FILE__, "%s%s",     \
                                            (priority), __LINE__, 0,            \
                                            (prefix) != NULL ? (prefix) : "",   \
                                            CXLB_buf);                          \
            }                                                                   \
            if (CXLB_len < 0) {                                                 \
                CXLB_buf = NULL;  /* restore temporary override */              \
            }                                                                   \
            CXLB_buffer = NULL;                                                 \
            CXLB_buffer_len = 0;                                                \
            (void) (postemit);                                                  \
                                                                                \
        } else if (CXLB_buffer == NULL) {                                       \
            CXLB_buffer_len = CXLB_len;                                         \
            CXLB_buffer = CXLB_buf;                                             \
            CXLB_buf = NULL;                                                    \
            CXLB_priority = (priority);  /* remember as a running severest */   \
                                                                                \
        } else {                                                                \
            CXLB_buffer = realloc(CXLB_buffer, 1 + CXLB_buffer_len + CXLB_len); \
            memcpy(CXLB_buffer + CXLB_buffer_len, CXLB_buf, CXLB_len);          \
            CXLB_buffer_len += CXLB_len;                                        \
            CXLB_buffer[CXLB_buffer_len] = '\0';                                \
            CXLB_priority = QB_MIN(CXLB_priority, (priority));  /* severest? */ \
        }                                                                       \
        free(CXLB_buf);                                                         \
    }                                                                           \
} while (0)

/*!
 * \internal
 * \brief Serialize XML (using libxml) into provided descriptor
 *
 * \param[in] fd  File descriptor to (piece-wise) write to
 * \param[in] cur XML subtree to proceed
 */
void pcmk__xml_serialize_fd_formatted(int fd, xmlNode *cur);

enum pcmk__xml_artefact_ns {
    pcmk__xml_artefact_ns_legacy_rng = 1,
    pcmk__xml_artefact_ns_legacy_xslt,
    pcmk__xml_artefact_ns_base_rng,
    pcmk__xml_artefact_ns_base_xslt,
};

/*!
 * \internal
 * \brief Get the root directory to scan XML artefacts of given kind for
 *
 * \param[in] ns governs the hierarchy nesting against the inherent root dir
 *
 * \return root directory to scan XML artefacts of given kind for
 */
char *
pcmk__xml_artefact_root(enum pcmk__xml_artefact_ns ns);

/*!
 * \internal
 * \brief Get the fully unwrapped path to particular XML artifact (RNG/XSLT)
 *
 * \param[in] ns       denotes path forming details (parent dir, suffix)
 * \param[in] filespec symbolic file specification to be combined with
 *                     #artefact_ns to form the final path
 * \return unwrapped path to particular XML artifact (RNG/XSLT)
 */
char *pcmk__xml_artefact_path(enum pcmk__xml_artefact_ns ns,
                              const char *filespec);

enum pcmk__acl_render_how {
    pcmk__acl_render_ns_simple = 1,
    pcmk__acl_render_text,
    pcmk__acl_render_color,
};

/*!
 * \internal
 * \brief Serialize-render already pcmk_acl_evaled_as_namespaces annotated XML
 *
 * This function is vitally coupled with externalized material:
 * - acls-2-render.xsl
 * - acls-render-cfg.xsl (referred to from the former in an abstracted way)
 *
 * In fact, it's just a wrapper for a graceful conducting of such
 * transformation, in particular, it cares about converting values of some
 * configuration parameters directly in said stylesheet(s) since the desired
 * ANSI colors at the output are not expressible directly (alternative approach
 * to this preprocessing: eventual postprocessing, which is less handy here).
 *
 * \param[in] annotated_doc pcmk_acl_evaled_as_namespaces annotated XML
 * \param[in] how           render kind, see #pcmk__acl_render_how enumeration
 * \param[out] doc_txt_ptr  where to put the final outcome string
 * \param[out] doc_txt_len  length of the output string \p doc_txt_ptr
 * \return 0 or -1, see \c xsltSaveResultToString
 *
 * \note Currently, the function did not receive enough of testing regarding
 *       leak of resources, hence it is not recommended for anything other
 *       than short-lived processes at this time.
 */
int pcmk__acl_evaled_render(xmlDoc *annotated_doc, enum pcmk__acl_render_how,
                            xmlChar **doc_txt_ptr, int *doc_txt_len);

/*!
 * \internal
 * \brief Disposal of "selected credentials" context
 *
 * \param[in] selected_creds  Context of ACLs unpacking to be free'd
 *
 * \see pcmk__selected_creds_init
 */
void pcmk__selected_creds_free(char **selected_creds);

#endif
