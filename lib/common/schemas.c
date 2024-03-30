/*
 * Copyright 2004-2024 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU Lesser General Public License
 * version 2.1 or later (LGPLv2.1+) WITHOUT ANY WARRANTY.
 */

#include <crm_internal.h>

#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <stdarg.h>

#include <libxml/relaxng.h>
#include <libxslt/xslt.h>
#include <libxslt/transform.h>
#include <libxslt/security.h>
#include <libxslt/xsltutils.h>

#include <crm/common/xml.h>
#include <crm/common/xml_internal.h>  /* PCMK__XML_LOG_BASE */

#include "crmcommon_private.h"

#define SCHEMA_ZERO { .v = { 0, 0 } }

#define schema_strdup_printf(prefix, version, suffix) \
    crm_strdup_printf(prefix "%u.%u" suffix, (version).v[0], (version).v[1])

typedef struct {
    xmlRelaxNGPtr rng;
    xmlRelaxNGValidCtxtPtr valid;
    xmlRelaxNGParserCtxtPtr parser;
} relaxng_ctx_cache_t;

static GList *known_schemas = NULL;
static bool silent_logging = FALSE;

static void G_GNUC_PRINTF(2, 3)
xml_log(int priority, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    if (silent_logging == FALSE) {
        /* XXX should not this enable dechunking as well? */
        PCMK__XML_LOG_BASE(priority, FALSE, 0, NULL, fmt, ap);
    }
    va_end(ap);
}

static int
xml_latest_schema_index(GList *schemas)
{
    // @COMPAT: pacemaker-next is deprecated since 2.1.5
    // FIXME: This function assumes at least three schemas have been added
    // before it has been called for the first time, which is only the case
    // if we are not unit testing.
#if defined(PCMK__UNIT_TESTING)
    return g_list_length(schemas) - 1; // index from 0
#else
    return g_list_length(schemas) - 3; // index from 0, ignore "pacemaker-next"/"none"
#endif
}

/* Return the index of the most recent X.0 schema. */
int
pcmk__find_x_0_schema_index(GList *schemas)
{
    /* We can't just use best to determine whether we've found the index
     * or not.  What if we have a very long list of schemas all in the
     * same major version series?  We'd return 0 for that, which means
     * we would still run this function every time.
     */
#if defined(PCMK__UNIT_TESTING)
    /* If we're unit testing, these can't be static because they'll stick
     * around from one test run to the next.  They need to be cleared out
     * every time.
     */
    bool found = false;
    int best = 0;
#else
    static bool found = false;
    static int best = 0;
#endif
    int i;
    GList *best_node = NULL;
    pcmk__schema_t *best_schema = NULL;

    if (found) {
        return best;
    }

    CRM_ASSERT(schemas != NULL);

    /* Get the most recent schema so we can look at its version number. */
    best = xml_latest_schema_index(schemas);
    best_node = g_list_nth(schemas, best);
    best_schema = best_node->data;

    /* If we are unit testing, we don't add the pacemaker-next/none schemas
     * to the list because we're not using the standard schema adding
     * functions.  Thus, a singleton list means we're done.
     *
     * On the other hand, if we are running as usually, we have those two
     * schemas added to the list.  A list of length three actually only has
     * one useful schema.  So we're still done.
     *
     * @COMPAT Change this when we stop adding those schemas.
     */
#if defined(PCMK__UNIT_TESTING)
    if (pcmk__list_of_1(schemas)) {
        goto done;
    }
#else
    if (g_list_length(schemas) == 3) {
        goto done;
    }
#endif

    /* Start comparing the list from the node before the best schema (there's
     * no point in comparing something to itself).  Then, 'i' is an index
     * starting at the best schema and will always point at the node after
     * 'iter'.  This makes it the value we want to return when we find what
     * we're looking for.
     */
    i = best;

    for (GList *iter = best_node->prev; iter != NULL; iter = iter->prev) {
        pcmk__schema_t *schema = iter->data;

        /* We've found a schema in an older major version series.  Return
         * the index of the first one in the same major version series as
         * the best schema.
         */
        if (schema->version.v[0] < best_schema->version.v[0]) {
            best = i;
            goto done;

        /* We're out of list to examine.  This probably means there was only
         * one major version series, so return index 0.
         */
        } else if (iter->prev == NULL) {
            best = 0;
            goto done;
        }

        i--;
    }

done:
    found = true;
    return best;
}

const char *
xml_latest_schema(void)
{
    return get_schema_name(xml_latest_schema_index(known_schemas));
}

static inline bool
version_from_filename(const char *filename, pcmk__schema_version_t *version)
{
    if (pcmk__ends_with(filename, ".rng")) {
        return sscanf(filename, "pacemaker-%hhu.%hhu.rng", &(version->v[0]), &(version->v[1])) == 2;
    } else {
        return sscanf(filename, "pacemaker-%hhu.%hhu", &(version->v[0]), &(version->v[1])) == 2;
    }
}

static int
schema_filter(const struct dirent *a)
{
    int rc = 0;
    pcmk__schema_version_t version = SCHEMA_ZERO;

    if (strstr(a->d_name, "pacemaker-") != a->d_name) {
        /* crm_trace("%s - wrong prefix", a->d_name); */

    } else if (!pcmk__ends_with_ext(a->d_name, ".rng")) {
        /* crm_trace("%s - wrong suffix", a->d_name); */

    } else if (!version_from_filename(a->d_name, &version)) {
        /* crm_trace("%s - wrong format", a->d_name); */

    } else {
        /* crm_debug("%s - candidate", a->d_name); */
        rc = 1;
    }

    return rc;
}

static int
schema_cmp(pcmk__schema_version_t a_version, pcmk__schema_version_t b_version)
{
    for (int i = 0; i < 2; ++i) {
        if (a_version.v[i] < b_version.v[i]) {
            return -1;
        } else if (a_version.v[i] > b_version.v[i]) {
            return 1;
        }
    }
    return 0;
}

static int
schema_cmp_directory(const struct dirent **a, const struct dirent **b)
{
    pcmk__schema_version_t a_version = SCHEMA_ZERO;
    pcmk__schema_version_t b_version = SCHEMA_ZERO;

    if (!version_from_filename(a[0]->d_name, &a_version)
        || !version_from_filename(b[0]->d_name, &b_version)) {
        // Shouldn't be possible, but makes static analysis happy
        return 0;
    }

    return schema_cmp(a_version, b_version);
}

/*!
 * \internal
 * \brief Add given schema + auxiliary data to internal bookkeeping.
 *
 * \note When providing \p version, should not be called directly but
 *       through \c add_schema_by_version.
 */
static void
add_schema(enum pcmk__schema_validator validator, const pcmk__schema_version_t *version,
           const char *name, const char *transform,
           const char *transform_enter, bool transform_onleave)
{
    pcmk__schema_t *schema = NULL;
    int last = g_list_length(known_schemas);

    schema = pcmk__assert_alloc(1, sizeof(pcmk__schema_t));

    schema->validator = validator;
    schema->version.v[0] = version->v[0];
    schema->version.v[1] = version->v[1];
    schema->transform_onleave = transform_onleave;

    if (version->v[0] || version->v[1]) {
        schema->name = schema_strdup_printf("pacemaker-", *version, "");
    } else {
        schema->name = pcmk__str_copy(name);
    }

    if (transform) {
        schema->transform = pcmk__str_copy(transform);
    }

    if (transform_enter) {
        schema->transform_enter = pcmk__str_copy(transform_enter);
    }

    known_schemas = g_list_append(known_schemas, schema);

    if (schema->transform != NULL) {
        crm_debug("Added supported schema %d: %s (upgrades with %s.xsl)",
                  last, schema->name, schema->transform);

    } else {
        crm_debug("Added supported schema %d: %s", last, schema->name);
    }
}

/*!
 * \internal
 * \brief Add version-specified schema + auxiliary data to internal bookkeeping.
 * \return Standard Pacemaker return value (the only possible values are
 * \c ENOENT when no upgrade schema is associated, or \c pcmk_rc_ok otherwise.
 *
 * \note There's no reliance on the particular order of schemas entering here.
 *
 * \par A bit of theory
 * We track 3 XSLT stylesheets that differ per usage:
 * - "upgrade":
 *   . sparsely spread over the sequence of all available schemas,
 *     as they are only relevant when major version of the schema
 *     is getting bumped -- in that case, it MUST be set
 *   . name convention:  upgrade-X.Y.xsl
 * - "upgrade-enter":
 *   . may only accompany "upgrade" occurrence, but doesn't need to
 *     be present anytime such one is, i.e., it MAY not be set when
 *     "upgrade" is
 *   . name convention:  upgrade-X.Y-enter.xsl,
 *     when not present: upgrade-enter.xsl
 * - "upgrade-leave":
 *   . like "upgrade-enter", but SHOULD be present whenever
 *     "upgrade-enter" is (and vice versa, but that's only
 *     to prevent confusion based on observing the files,
 *     it would get ignored regardless)
 *   . name convention:  (see "upgrade-enter")
 */
static int
add_schema_by_version(const pcmk__schema_version_t *version, bool transform_expected)
{
    bool transform_onleave = FALSE;
    int rc = pcmk_rc_ok;
    struct stat s;
    char *xslt = NULL,
         *transform_upgrade = NULL,
         *transform_enter = NULL;

    /* prologue for further transform_expected handling */
    if (transform_expected) {
        /* check if there's suitable "upgrade" stylesheet */
        transform_upgrade = schema_strdup_printf("upgrade-", *version, );
        xslt = pcmk__xml_artefact_path(pcmk__xml_artefact_ns_legacy_xslt,
                                       transform_upgrade);
    }

    if (!transform_expected) {
        /* jump directly to the end */

    } else if (stat(xslt, &s) == 0) {
        /* perhaps there's also a targeted "upgrade-enter" stylesheet */
        transform_enter = schema_strdup_printf("upgrade-", *version, "-enter");
        free(xslt);
        xslt = pcmk__xml_artefact_path(pcmk__xml_artefact_ns_legacy_xslt,
                                       transform_enter);
        if (stat(xslt, &s) != 0) {
            /* or initially, at least a generic one */
            crm_debug("Upgrade-enter transform %s.xsl not found", xslt);
            free(xslt);
            free(transform_enter);
            transform_enter = strdup("upgrade-enter");
            xslt = pcmk__xml_artefact_path(pcmk__xml_artefact_ns_legacy_xslt,
                                           transform_enter);
            if (stat(xslt, &s) != 0) {
                crm_debug("Upgrade-enter transform %s.xsl not found, either", xslt);
                free(xslt);
                xslt = NULL;
            }
        }
        /* xslt contains full path to "upgrade-enter" stylesheet */
        if (xslt != NULL) {
            /* then there should be "upgrade-leave" counterpart (enter->leave) */
            memcpy(strrchr(xslt, '-') + 1, "leave", sizeof("leave") - 1);
            transform_onleave = (stat(xslt, &s) == 0);
            free(xslt);
        } else {
            free(transform_enter);
            transform_enter = NULL;
        }

    } else {
        crm_err("Upgrade transform %s not found", xslt);
        free(xslt);
        free(transform_upgrade);
        transform_upgrade = NULL;
        rc = ENOENT;
    }

    add_schema(pcmk__schema_validator_rng, version, NULL,
               transform_upgrade, transform_enter, transform_onleave);

    free(transform_upgrade);
    free(transform_enter);

    return rc;
}

static void
wrap_libxslt(bool finalize)
{
    static xsltSecurityPrefsPtr secprefs;
    int ret = 0;

    /* security framework preferences */
    if (!finalize) {
        CRM_ASSERT(secprefs == NULL);
        secprefs = xsltNewSecurityPrefs();
        ret = xsltSetSecurityPrefs(secprefs, XSLT_SECPREF_WRITE_FILE,
                                   xsltSecurityForbid)
              | xsltSetSecurityPrefs(secprefs, XSLT_SECPREF_CREATE_DIRECTORY,
                                     xsltSecurityForbid)
              | xsltSetSecurityPrefs(secprefs, XSLT_SECPREF_READ_NETWORK,
                                     xsltSecurityForbid)
              | xsltSetSecurityPrefs(secprefs, XSLT_SECPREF_WRITE_NETWORK,
                                     xsltSecurityForbid);
        if (ret != 0) {
            return;
        }
    } else {
        xsltFreeSecurityPrefs(secprefs);
        secprefs = NULL;
    }

    /* cleanup only */
    if (finalize) {
        xsltCleanupGlobals();
    }
}

void
pcmk__load_schemas_from_dir(const char *dir)
{
    int lpc, max;
    struct dirent **namelist = NULL;

    max = scandir(dir, &namelist, schema_filter, schema_cmp_directory);
    if (max < 0) {
        crm_warn("Could not load schemas from %s: %s", dir, strerror(errno));
        return;
    }

    for (lpc = 0; lpc < max; lpc++) {
        bool transform_expected = false;
        pcmk__schema_version_t version = SCHEMA_ZERO;

        if (!version_from_filename(namelist[lpc]->d_name, &version)) {
            // Shouldn't be possible, but makes static analysis happy
            crm_warn("Skipping schema '%s': could not parse version",
                     namelist[lpc]->d_name);
            continue;
        }
        if ((lpc + 1) < max) {
            pcmk__schema_version_t next_version = SCHEMA_ZERO;

            if (version_from_filename(namelist[lpc+1]->d_name, &next_version)
                    && (version.v[0] < next_version.v[0])) {
                transform_expected = true;
            }
        }

        if (add_schema_by_version(&version, transform_expected) != pcmk_rc_ok) {
            break;
        }
    }

    for (lpc = 0; lpc < max; lpc++) {
        free(namelist[lpc]);
    }

    free(namelist);
}

static gint
schema_sort_GCompareFunc(gconstpointer a, gconstpointer b)
{
    const pcmk__schema_t *schema_a = a;
    const pcmk__schema_t *schema_b = b;

    if (pcmk__str_eq(schema_a->name, "pacemaker-next", pcmk__str_none)) {
        if (pcmk__str_eq(schema_b->name, "none", pcmk__str_none)) {
            return -1;
        } else {
            return 1;
        }
    } else if (pcmk__str_eq(schema_a->name, "none", pcmk__str_none)) {
        return 1;
    } else if (pcmk__str_eq(schema_b->name, "pacemaker-next", pcmk__str_none)) {
        return -1;
    } else {
        return schema_cmp(schema_a->version, schema_b->version);
    }
}

/*!
 * \internal
 * \brief Sort the list of known schemas such that all pacemaker-X.Y are in
 *        version order, then pacemaker-next, then none
 *
 * This function should be called whenever additional schemas are loaded using
 * pcmk__load_schemas_from_dir(), after the initial sets in crm_schema_init().
 */
void
pcmk__sort_schemas(void)
{
    known_schemas = g_list_sort(known_schemas, schema_sort_GCompareFunc);
}

/*!
 * \internal
 * \brief Load pacemaker schemas into cache
 *
 * \note This currently also serves as an entry point for the
 *       generic initialization of the libxslt library.
 */
void
crm_schema_init(void)
{
    const char *remote_schema_dir = pcmk__remote_schema_dir();
    char *base = pcmk__xml_artefact_root(pcmk__xml_artefact_ns_legacy_rng);
    const pcmk__schema_version_t zero = SCHEMA_ZERO;

    wrap_libxslt(false);

    pcmk__load_schemas_from_dir(base);
    pcmk__load_schemas_from_dir(remote_schema_dir);

    // @COMPAT: Deprecated since 2.1.5
    add_schema(pcmk__schema_validator_rng, &zero, "pacemaker-next",
               NULL, NULL, FALSE);

    add_schema(pcmk__schema_validator_none, &zero, PCMK_VALUE_NONE,
               NULL, NULL, FALSE);

    /* This shouldn't be strictly necessary, but we'll do it here just in case
     * there's anything in PCMK__REMOTE_SCHEMA_DIR that messes up the order.
     */
    pcmk__sort_schemas();
}

static gboolean
validate_with_relaxng(xmlDocPtr doc, xmlRelaxNGValidityErrorFunc error_handler, void *error_handler_context, const char *relaxng_file,
                      relaxng_ctx_cache_t **cached_ctx)
{
    int rc = 0;
    gboolean valid = TRUE;
    relaxng_ctx_cache_t *ctx = NULL;

    CRM_CHECK(doc != NULL, return FALSE);
    CRM_CHECK(relaxng_file != NULL, return FALSE);

    if (cached_ctx && *cached_ctx) {
        ctx = *cached_ctx;

    } else {
        crm_debug("Creating RNG parser context");
        ctx = pcmk__assert_alloc(1, sizeof(relaxng_ctx_cache_t));

        ctx->parser = xmlRelaxNGNewParserCtxt(relaxng_file);
        CRM_CHECK(ctx->parser != NULL, goto cleanup);

        if (error_handler) {
            xmlRelaxNGSetParserErrors(ctx->parser,
                                      (xmlRelaxNGValidityErrorFunc) error_handler,
                                      (xmlRelaxNGValidityWarningFunc) error_handler,
                                      error_handler_context);
        } else {
            xmlRelaxNGSetParserErrors(ctx->parser,
                                      (xmlRelaxNGValidityErrorFunc) fprintf,
                                      (xmlRelaxNGValidityWarningFunc) fprintf,
                                      stderr);
        }

        ctx->rng = xmlRelaxNGParse(ctx->parser);
        CRM_CHECK(ctx->rng != NULL,
                  crm_err("Could not find/parse %s", relaxng_file);
                  goto cleanup);

        ctx->valid = xmlRelaxNGNewValidCtxt(ctx->rng);
        CRM_CHECK(ctx->valid != NULL, goto cleanup);

        if (error_handler) {
            xmlRelaxNGSetValidErrors(ctx->valid,
                                     (xmlRelaxNGValidityErrorFunc) error_handler,
                                     (xmlRelaxNGValidityWarningFunc) error_handler,
                                     error_handler_context);
        } else {
            xmlRelaxNGSetValidErrors(ctx->valid,
                                     (xmlRelaxNGValidityErrorFunc) fprintf,
                                     (xmlRelaxNGValidityWarningFunc) fprintf,
                                     stderr);
        }
    }

    rc = xmlRelaxNGValidateDoc(ctx->valid, doc);
    if (rc > 0) {
        valid = FALSE;

    } else if (rc < 0) {
        crm_err("Internal libxml error during validation");
    }

  cleanup:

    if (cached_ctx) {
        *cached_ctx = ctx;

    } else {
        if (ctx->parser != NULL) {
            xmlRelaxNGFreeParserCtxt(ctx->parser);
        }
        if (ctx->valid != NULL) {
            xmlRelaxNGFreeValidCtxt(ctx->valid);
        }
        if (ctx->rng != NULL) {
            xmlRelaxNGFree(ctx->rng);
        }
        free(ctx);
    }

    return valid;
}

static void
free_schema(gpointer data)
{
    pcmk__schema_t *schema = data;
    relaxng_ctx_cache_t *ctx = NULL;

    switch (schema->validator) {
        case pcmk__schema_validator_none: // not cached
            break;

        case pcmk__schema_validator_rng: // cached
            ctx = (relaxng_ctx_cache_t *) schema->cache;
            if (ctx == NULL) {
                break;
            }

            if (ctx->parser != NULL) {
                xmlRelaxNGFreeParserCtxt(ctx->parser);
            }

            if (ctx->valid != NULL) {
                xmlRelaxNGFreeValidCtxt(ctx->valid);
            }

            if (ctx->rng != NULL) {
                xmlRelaxNGFree(ctx->rng);
            }

            free(ctx);
            schema->cache = NULL;
            break;
    }

    free(schema->name);
    free(schema->transform);
    free(schema->transform_enter);
}

/*!
 * \internal
 * \brief Clean up global memory associated with XML schemas
 */
void
pcmk__schema_cleanup(void)
{
    if (known_schemas != NULL) {
        g_list_free_full(known_schemas, free_schema);
        known_schemas = NULL;
    }

    wrap_libxslt(true);
}

static gboolean
validate_with(xmlNode *xml, pcmk__schema_t *schema, xmlRelaxNGValidityErrorFunc error_handler, void* error_handler_context)
{
    gboolean valid = FALSE;
    char *file = NULL;
    relaxng_ctx_cache_t **cache = NULL;

    if (schema == NULL) {
        return FALSE;
    }

    if (schema->validator == pcmk__schema_validator_none) {
        return TRUE;
    }

    if (pcmk__str_eq(schema->name, "pacemaker-next", pcmk__str_none)) {
        crm_warn("The pacemaker-next schema is deprecated and will be removed "
                 "in a future release.");
    }

    file = pcmk__xml_artefact_path(pcmk__xml_artefact_ns_legacy_rng,
                                   schema->name);

    crm_trace("Validating with %s (type=%d)",
              pcmk__s(file, "missing schema"), schema->validator);
    switch (schema->validator) {
        case pcmk__schema_validator_rng:
            cache = (relaxng_ctx_cache_t **) &(schema->cache);
            valid = validate_with_relaxng(xml->doc, error_handler, error_handler_context, file, cache);
            break;
        default:
            crm_err("Unknown validator type: %d", schema->validator);
            break;
    }

    free(file);
    return valid;
}

static bool
validate_with_silent(xmlNode *xml, pcmk__schema_t *schema)
{
    bool rc, sl_backup = silent_logging;
    silent_logging = TRUE;
    rc = validate_with(xml, schema, (xmlRelaxNGValidityErrorFunc) xml_log, GUINT_TO_POINTER(LOG_ERR));
    silent_logging = sl_backup;
    return rc;
}

static void
dump_file(const char *filename)
{

    FILE *fp = NULL;
    int ch, line = 0;

    CRM_CHECK(filename != NULL, return);

    fp = fopen(filename, "r");
    if (fp == NULL) {
        crm_perror(LOG_ERR, "Could not open %s for reading", filename);
        return;
    }

    fprintf(stderr, "%4d ", ++line);
    do {
        ch = getc(fp);
        if (ch == EOF) {
            putc('\n', stderr);
            break;
        } else if (ch == '\n') {
            fprintf(stderr, "\n%4d ", ++line);
        } else {
            putc(ch, stderr);
        }
    } while (1);

    fclose(fp);
}

gboolean
validate_xml_verbose(const xmlNode *xml_blob)
{
    int fd = 0;
    xmlDoc *doc = NULL;
    xmlNode *xml = NULL;
    gboolean rc = FALSE;
    char *filename = NULL;

    filename = crm_strdup_printf("%s/cib-invalid.XXXXXX", pcmk__get_tmpdir());

    umask(S_IWGRP | S_IWOTH | S_IROTH);
    fd = mkstemp(filename);
    pcmk__xml_write_fd(xml_blob, filename, fd, false, NULL);

    dump_file(filename);

    doc = xmlReadFile(filename, NULL, 0);
    xml = xmlDocGetRootElement(doc);
    rc = validate_xml(xml, NULL, FALSE);
    pcmk__xml_free(xml);

    unlink(filename);
    free(filename);

    return rc;
}

gboolean
validate_xml(xmlNode *xml_blob, const char *validation, gboolean to_logs)
{
    return pcmk__validate_xml(xml_blob, validation, to_logs ? (xmlRelaxNGValidityErrorFunc) xml_log : NULL, GUINT_TO_POINTER(LOG_ERR));
}

gboolean
pcmk__validate_xml(xmlNode *xml_blob, const char *validation, xmlRelaxNGValidityErrorFunc error_handler, void* error_handler_context)
{
    int version = 0;

    CRM_CHECK((xml_blob != NULL) && (xml_blob->doc != NULL), return FALSE);

    if (validation == NULL) {
        validation = crm_element_value(xml_blob, PCMK_XA_VALIDATE_WITH);
    }

    if (validation == NULL) {
        bool valid = FALSE;

        for (GList *iter = known_schemas; iter != NULL; iter = iter->next) {
            pcmk__schema_t *schema = iter->data;

            if (validate_with(xml_blob, schema, NULL, NULL)) {
                valid = TRUE;
                crm_xml_add(xml_blob, PCMK_XA_VALIDATE_WITH, schema->name);
                crm_info("XML validated against %s", schema->name);
            }
        }

        return valid;
    }

    version = get_schema_version(validation);
    if (strcmp(validation, PCMK_VALUE_NONE) == 0) {
        return TRUE;
    } else if (version < g_list_length(known_schemas)) {
        pcmk__schema_t *schema = g_list_nth_data(known_schemas, version);
        return validate_with(xml_blob, schema, error_handler,
                             error_handler_context);
    }

    crm_err("Unknown validator: %s", validation);
    return FALSE;
}

/* With this arrangement, an attempt to identify the message severity
   as explicitly signalled directly from XSLT is performed in rather
   a smart way (no reliance on formatting string + arguments being
   always specified as ["%s", purposeful_string], as it can also be
   ["%s: %s", some_prefix, purposeful_string] etc. so every argument
   pertaining %s specifier is investigated), and if such a mark found,
   the respective level is determined and, when the messages are to go
   to the native logs, the mark itself gets dropped
   (by the means of string shift).

   NOTE: whether the native logging is the right sink is decided per
         the ctx parameter -- NULL denotes this case, otherwise it
         carries a pointer to the numeric expression of the desired
         target logging level (messages with higher level will be
         suppressed)

   NOTE: on some architectures, this string shift may not have any
         effect, but that's an acceptable tradeoff

   The logging level for not explicitly designated messages
   (suspicious, likely internal errors or some runaways) is
   LOG_WARNING.
 */
static void G_GNUC_PRINTF(2, 3)
cib_upgrade_err(void *ctx, const char *fmt, ...)
{
    va_list ap, aq;
    char *arg_cur;

    bool found = FALSE;
    const char *fmt_iter = fmt;
    uint8_t msg_log_level = LOG_WARNING;  /* default for runaway messages */
    const unsigned * log_level = (const unsigned *) ctx;
    enum {
        escan_seennothing,
        escan_seenpercent,
    } scan_state = escan_seennothing;

    va_start(ap, fmt);
    va_copy(aq, ap);

    while (!found && *fmt_iter != '\0') {
        /* while casing schema borrowed from libqb:qb_vsnprintf_serialize */
        switch (*fmt_iter++) {
        case '%':
            if (scan_state == escan_seennothing) {
                scan_state = escan_seenpercent;
            } else if (scan_state == escan_seenpercent) {
                scan_state = escan_seennothing;
            }
            break;
        case 's':
            if (scan_state == escan_seenpercent) {
                scan_state = escan_seennothing;
                arg_cur = va_arg(aq, char *);
                if (arg_cur != NULL) {
                    switch (arg_cur[0]) {
                    case 'W':
                        if (!strncmp(arg_cur, "WARNING: ",
                                     sizeof("WARNING: ") - 1)) {
                            msg_log_level = LOG_WARNING;
                        }
                        if (ctx == NULL) {
                            memmove(arg_cur, arg_cur + sizeof("WARNING: ") - 1,
                                    strlen(arg_cur + sizeof("WARNING: ") - 1) + 1);
                        }
                        found = TRUE;
                        break;
                    case 'I':
                        if (!strncmp(arg_cur, "INFO: ",
                                     sizeof("INFO: ") - 1)) {
                            msg_log_level = LOG_INFO;
                        }
                        if (ctx == NULL) {
                            memmove(arg_cur, arg_cur + sizeof("INFO: ") - 1,
                                    strlen(arg_cur + sizeof("INFO: ") - 1) + 1);
                        }
                        found = TRUE;
                        break;
                    case 'D':
                        if (!strncmp(arg_cur, "DEBUG: ",
                                     sizeof("DEBUG: ") - 1)) {
                            msg_log_level = LOG_DEBUG;
                        }
                        if (ctx == NULL) {
                            memmove(arg_cur, arg_cur + sizeof("DEBUG: ") - 1,
                                    strlen(arg_cur + sizeof("DEBUG: ") - 1) + 1);
                        }
                        found = TRUE;
                        break;
                    }
                }
            }
            break;
        case '#': case '-': case ' ': case '+': case '\'': case 'I': case '.':
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
        case '*':
            break;
        case 'l':
        case 'z':
        case 't':
        case 'j':
        case 'd': case 'i':
        case 'o':
        case 'u':
        case 'x': case 'X':
        case 'e': case 'E':
        case 'f': case 'F':
        case 'g': case 'G':
        case 'a': case 'A':
        case 'c':
        case 'p':
            if (scan_state == escan_seenpercent) {
                (void) va_arg(aq, void *);  /* skip forward */
                scan_state = escan_seennothing;
            }
            break;
        default:
            scan_state = escan_seennothing;
            break;
        }
    }

    if (log_level != NULL) {
        /* intention of the following offset is:
           cibadmin -V -> start showing INFO labelled messages */
        if (*log_level + 4 >= msg_log_level) {
            vfprintf(stderr, fmt, ap);
        }
    } else {
        PCMK__XML_LOG_BASE(msg_log_level, TRUE, 0, "CIB upgrade: ", fmt, ap);
    }

    va_end(aq);
    va_end(ap);
}

static xmlNode *
apply_transformation(xmlNode *xml, const char *transform, gboolean to_logs)
{
    char *xform = NULL;
    xmlNode *out = NULL;
    xmlDocPtr res = NULL;
    xsltStylesheet *xslt = NULL;

    xform = pcmk__xml_artefact_path(pcmk__xml_artefact_ns_legacy_xslt,
                                    transform);

    /* for capturing, e.g., what's emitted via <xsl:message> */
    if (to_logs) {
        xsltSetGenericErrorFunc(NULL, cib_upgrade_err);
    } else {
        xsltSetGenericErrorFunc(&crm_log_level, cib_upgrade_err);
    }

    xslt = xsltParseStylesheetFile((pcmkXmlStr) xform);
    CRM_CHECK(xslt != NULL, goto cleanup);

    res = xsltApplyStylesheet(xslt, xml->doc, NULL);
    CRM_CHECK(res != NULL, goto cleanup);

    xsltSetGenericErrorFunc(NULL, NULL);  /* restore default one */

    out = xmlDocGetRootElement(res);

  cleanup:
    if (xslt) {
        xsltFreeStylesheet(xslt);
    }

    free(xform);

    return out;
}

/*!
 * \internal
 * \brief Possibly full enter->upgrade->leave trip per internal bookkeeping.
 *
 * \note Only emits warnings about enter/leave phases in case of issues.
 */
static xmlNode *
apply_upgrade(xmlNode *xml, const pcmk__schema_t *schema, gboolean to_logs)
{
    bool transform_onleave = schema->transform_onleave;
    char *transform_leave;
    xmlNode *upgrade = NULL,
            *final = NULL;

    if (schema->transform_enter) {
        crm_debug("Upgrading %s-style configuration, pre-upgrade phase with %s.xsl",
                  schema->name, schema->transform_enter);
        upgrade = apply_transformation(xml, schema->transform_enter, to_logs);
        if (upgrade == NULL) {
            crm_warn("Upgrade-enter transformation %s.xsl failed",
                     schema->transform_enter);
            transform_onleave = FALSE;
        }
    }
    if (upgrade == NULL) {
        upgrade = xml;
    }

    crm_debug("Upgrading %s-style configuration, main phase with %s.xsl",
              schema->name, schema->transform);
    final = apply_transformation(upgrade, schema->transform, to_logs);
    if (upgrade != xml) {
        pcmk__xml_free(upgrade);
        upgrade = NULL;
    }

    if (final != NULL && transform_onleave) {
        upgrade = final;
        /* following condition ensured in add_schema_by_version */
        CRM_ASSERT(schema->transform_enter != NULL);
        transform_leave = strdup(schema->transform_enter);
        /* enter -> leave */
        memcpy(strrchr(transform_leave, '-') + 1, "leave", sizeof("leave") - 1);
        crm_debug("Upgrading %s-style configuration, post-upgrade phase with %s.xsl",
                  schema->name, transform_leave);
        final = apply_transformation(upgrade, transform_leave, to_logs);
        if (final == NULL) {
            crm_warn("Upgrade-leave transformation %s.xsl failed", transform_leave);
            final = upgrade;
        } else {
            pcmk__xml_free(upgrade);
        }
        free(transform_leave);
    }

    return final;
}

const char *
get_schema_name(int version)
{
    pcmk__schema_t *schema = g_list_nth_data(known_schemas, version);

    return (schema != NULL)? schema->name : "unknown";
}

int
get_schema_version(const char *name)
{
    int lpc = 0;

    if (name == NULL) {
        name = PCMK_VALUE_NONE;
    }

    for (GList *iter = known_schemas; iter != NULL; iter = iter->next) {
        pcmk__schema_t *schema = iter->data;

        if (pcmk__str_eq(name, schema->name, pcmk__str_casei)) {
            return lpc;
        }

        lpc++;
    }

    return -1;
}

/* set which validation to use */
int
update_validation(xmlNode **xml_blob, int *best, int max, gboolean transform,
                  gboolean to_logs)
{
    xmlNode *xml = NULL;
    char *value = NULL;
    int max_stable_schemas = xml_latest_schema_index(known_schemas);
    int lpc = 0, match = -1, rc = pcmk_ok;
    int next = -1;  /* -1 denotes "inactive" value */
    xmlRelaxNGValidityErrorFunc error_handler = 
        to_logs ? (xmlRelaxNGValidityErrorFunc) xml_log : NULL;

    CRM_CHECK(best != NULL, return -EINVAL);
    *best = 0;

    CRM_CHECK((xml_blob != NULL) && (*xml_blob != NULL)
              && ((*xml_blob)->doc != NULL),
              return -EINVAL);

    xml = *xml_blob;
    value = crm_element_value_copy(xml, PCMK_XA_VALIDATE_WITH);

    if (value != NULL) {
        match = get_schema_version(value);

        lpc = match;
        if (lpc >= 0 && transform == FALSE) {
            *best = lpc++;

        } else if (lpc < 0) {
            crm_debug("Unknown validation schema");
            lpc = 0;
        }
    }

    if (match >= max_stable_schemas) {
        /* nothing to do */
        free(value);
        *best = match;
        return pcmk_ok;
    }

    while (lpc <= max_stable_schemas) {
        /* FIXME: This will cause us to walk the known_schemas list every time
         * this loop iterates, which is not ideal.  However, for now it's a lot
         * easier than trying to get all the loop indices we're using here
         * sorted out and working correctly.
         */
        pcmk__schema_t *schema = g_list_nth_data(known_schemas, lpc);

        crm_debug("Testing '%s' validation (%d of %d)",
                  pcmk__s(schema->name, "<unset>"), lpc, max_stable_schemas);

        if (validate_with(xml, schema, error_handler, GUINT_TO_POINTER(LOG_ERR)) == FALSE) {
            if (next != -1) {
                crm_info("Configuration not valid for schema: %s",
                         schema->name);
                next = -1;
            } else {
                crm_trace("%s validation failed", pcmk__s(schema->name, "<unset>"));
            }
            if (*best) {
                /* we've satisfied the validation, no need to check further */
                break;
            }
            rc = -pcmk_err_schema_validation;

        } else {
            if (next != -1) {
                crm_debug("Configuration valid for schema: %s", schema->name);
                next = -1;
            }
            rc = pcmk_ok;
        }

        if (rc == pcmk_ok) {
            *best = lpc;
        }

        if (rc == pcmk_ok && transform) {
            xmlNode *upgrade = NULL;
            pcmk__schema_t *next_schema = NULL;
            next = lpc+1;

            if (next > max_stable_schemas) {
                /* There is no next version */
                crm_trace("Stopping at %s", schema->name);
                break;
            }

            if (max > 0 && (lpc == max || next > max)) {
                crm_trace("Upgrade limit reached at %s (lpc=%d, next=%d, max=%d)",
                          schema->name, lpc, next, max);
                break;
            }

            next_schema = g_list_nth_data(known_schemas, next);
            CRM_ASSERT(next_schema != NULL);

            if (schema->transform == NULL
                       /* possibly avoid transforming when readily valid
                          (in general more restricted when crossing the major
                          version boundary, as X.0 "transitional" version is
                          expected to be more strict than it's successors that
                          may re-allow constructs from previous major line) */
                       || validate_with_silent(xml, next_schema)) {
                crm_debug("%s-style configuration is also valid for %s",
                           schema->name, next_schema->name);

                lpc = next;

            } else {
                crm_debug("Upgrading %s-style configuration to %s with %s.xsl",
                           schema->name, next_schema->name, schema->transform);

                upgrade = apply_upgrade(xml, schema, to_logs);
                if (upgrade == NULL) {
                    crm_err("Transformation %s.xsl failed", schema->transform);
                    rc = -pcmk_err_transform_failed;

                } else if (validate_with(upgrade, next_schema, error_handler,
                                         GUINT_TO_POINTER(LOG_ERR))) {
                    crm_info("Transformation %s.xsl successful", schema->transform);
                    lpc = next;
                    *best = next;
                    pcmk__xml_free(xml);
                    xml = upgrade;
                    rc = pcmk_ok;

                } else {
                    crm_err("Transformation %s.xsl did not produce a valid configuration",
                            schema->transform);
                    crm_log_xml_info(upgrade, "transform:bad");
                    pcmk__xml_free(upgrade);
                    rc = -pcmk_err_schema_validation;
                }
                next = -1;
            }
        }

        if (transform == FALSE || rc != pcmk_ok) {
            /* we need some progress! */
            lpc++;
        }
    }

    if (*best > match && *best) {
        pcmk__schema_t *best_schema = g_list_nth_data(known_schemas, *best);

        crm_info("%s the configuration from %s to %s",
                   transform?"Transformed":"Upgraded", pcmk__s(value, "<none>"),
                   best_schema->name);
        crm_xml_add(xml, PCMK_XA_VALIDATE_WITH, best_schema->name);
    }

    *xml_blob = xml;
    free(value);
    return rc;
}

gboolean
cli_config_update(xmlNode **xml, int *best_version, gboolean to_logs)
{
    gboolean rc = TRUE;
    const char *value = crm_element_value(*xml, PCMK_XA_VALIDATE_WITH);
    char *const orig_value = strdup(value == NULL ? "(none)" : value);

    int version = get_schema_version(value);
    int orig_version = version;
    int min_version = pcmk__find_x_0_schema_index(known_schemas);

    if (version < min_version) {
        // Current configuration schema is not acceptable, try to update
        xmlNode *converted = NULL;

        converted = pcmk__xml_copy(NULL, *xml);
        update_validation(&converted, &version, 0, TRUE, to_logs);

        value = crm_element_value(converted, PCMK_XA_VALIDATE_WITH);
        if (version < min_version) {
            // Updated configuration schema is still not acceptable

            if (version < orig_version || orig_version == -1) {
                // We couldn't validate any schema at all
                if (to_logs) {
                    pcmk__config_err("Cannot upgrade configuration (claiming "
                                     "schema %s) to at least %s because it "
                                     "does not validate with any schema from "
                                     "%s to %s",
                                     orig_value,
                                     get_schema_name(min_version),
                                     get_schema_name(orig_version),
                                     xml_latest_schema());
                } else {
                    fprintf(stderr, "Cannot upgrade configuration (claiming "
                                    "schema %s) to at least %s because it "
                                    "does not validate with any schema from "
                                    "%s to %s\n",
                                    orig_value,
                                    get_schema_name(min_version),
                                    get_schema_name(orig_version),
                                    xml_latest_schema());
                }
            } else {
                // We updated configuration successfully, but still too low
                if (to_logs) {
                    pcmk__config_err("Cannot upgrade configuration (claiming "
                                     "schema %s) to at least %s because it "
                                     "would not upgrade past %s",
                                     orig_value,
                                     get_schema_name(min_version),
                                     pcmk__s(value, "unspecified version"));
                } else {
                    fprintf(stderr, "Cannot upgrade configuration (claiming "
                                    "schema %s) to at least %s because it "
                                    "would not upgrade past %s\n",
                                    orig_value,
                                    get_schema_name(min_version),
                                    pcmk__s(value, "unspecified version"));
                }
            }

            pcmk__xml_free(converted);
            converted = NULL;
            rc = FALSE;

        } else {
            // Updated configuration schema is acceptable
            pcmk__xml_free(*xml);
            *xml = converted;

            if (version < xml_latest_schema_index(known_schemas)) {
                if (to_logs) {
                    pcmk__config_warn("Configuration with schema %s was "
                                      "internally upgraded to acceptable (but "
                                      "not most recent) %s",
                                      orig_value, get_schema_name(version));
                }
            } else {
                if (to_logs) {
                    crm_info("Configuration with schema %s was internally "
                             "upgraded to latest version %s",
                             orig_value, get_schema_name(version));
                }
            }
        }

    } else if (version >= get_schema_version(PCMK_VALUE_NONE)) {
        // Schema validation is disabled
        if (to_logs) {
            pcmk__config_warn("Schema validation of configuration is disabled "
                              "(enabling is encouraged and prevents common "
                              "misconfigurations)");

        } else {
            fprintf(stderr, "Schema validation of configuration is disabled "
                            "(enabling is encouraged and prevents common "
                            "misconfigurations)\n");
        }
    }

    if (best_version) {
        *best_version = version;
    }

    free(orig_value);
    return rc;
}

/*!
 * \internal
 * \brief Return a list of all schema files and any associated XSLT files
 *        later than the given one
 * \brief Return a list of all schema versions later than the given one
 *
 * \param[in] schema The schema to compare against (for example,
 *                   "pacemaker-3.1.rng" or "pacemaker-3.1")
 *
 * \note The caller is responsible for freeing both the returned list and
 *       the elements of the list
 */
GList *
pcmk__schema_files_later_than(const char *name)
{
    GList *lst = NULL;
    pcmk__schema_version_t ver;

    if (!version_from_filename(name, &ver)) {
        return lst;
    }

    for (GList *iter = g_list_nth(known_schemas, xml_latest_schema_index(known_schemas));
         iter != NULL; iter = iter->prev) {
        pcmk__schema_t *schema = iter->data;
        char *s = NULL;

        if (schema_cmp(ver, schema->version) != -1) {
            continue;
        }

        s = crm_strdup_printf("%s.rng", schema->name);
        lst = g_list_prepend(lst, s);

        if (schema->transform != NULL) {
            char *xform = crm_strdup_printf("%s.xsl", schema->transform);
            lst = g_list_prepend(lst, xform);
        }

        if (schema->transform_enter != NULL) {
            char *enter = crm_strdup_printf("%s.xsl", schema->transform_enter);

            lst = g_list_prepend(lst, enter);

            if (schema->transform_onleave) {
                int last_dash = strrchr(enter, '-') - enter;
                char *leave = crm_strdup_printf("%.*s-leave.xsl", last_dash, enter);

                lst = g_list_prepend(lst, leave);
            }
        }
    }

    return lst;
}

static void
append_href(xmlNode *xml, void *user_data)
{
    GList **list = user_data;
    char *href = crm_element_value_copy(xml, "href");

    if (href == NULL) {
        return;
    }
    *list = g_list_prepend(*list, href);
}

static void
external_refs_in_schema(GList **list, const char *contents)
{
    /* local-name()= is needed to ignore the xmlns= setting at the top of
     * the XML file.  Otherwise, the xpath query will always return nothing.
     */
    const char *search = "//*[local-name()='externalRef'] | //*[local-name()='include']";
    xmlNode *xml = pcmk__xml_parse(contents);

    crm_foreach_xpath_result(xml, search, append_href, list);
    pcmk__xml_free(xml);
}

static int
read_file_contents(const char *file, char **contents)
{
    int rc = pcmk_rc_ok;
    char *path = NULL;

    if (pcmk__ends_with(file, ".rng")) {
        path = pcmk__xml_artefact_path(pcmk__xml_artefact_ns_legacy_rng, file);
    } else {
        path = pcmk__xml_artefact_path(pcmk__xml_artefact_ns_legacy_xslt, file);
    }

    rc = pcmk__file_contents(path, contents);

    free(path);
    return rc;
}

static void
add_schema_file_to_xml(xmlNode *parent, const char *file, GList **already_included)
{
    char *contents = NULL;
    char *path = NULL;
    xmlNode *file_node = NULL;
    GList *includes = NULL;
    int rc = pcmk_rc_ok;

    /* If we already included this file, don't do so again. */
    if (g_list_find_custom(*already_included, file, (GCompareFunc) strcmp) != NULL) {
        return;
    }

    /* Ensure whatever file we were given has a suffix we know about.  If not,
     * just assume it's an RNG file.
     */
    if (!pcmk__ends_with(file, ".rng") && !pcmk__ends_with(file, ".xsl")) {
        path = crm_strdup_printf("%s.rng", file);
    } else {
        path = pcmk__str_copy(file);
    }

    rc = read_file_contents(path, &contents);
    if (rc != pcmk_rc_ok || contents == NULL) {
        crm_warn("Could not read schema file %s: %s", file, pcmk_rc_str(rc));
        free(path);
        return;
    }

    /* Create a new <file path="..."> node with the contents of the file
     * as a CDATA block underneath it.
     */
    file_node = pcmk__xe_create(parent, PCMK_XA_FILE);
    crm_xml_add(file_node, PCMK_XA_PATH, path);
    *already_included = g_list_prepend(*already_included, path);

    xmlAddChild(file_node, xmlNewCDataBlock(parent->doc, (pcmkXmlStr) contents,
                                            strlen(contents)));

    /* Scan the file for any <externalRef> or <include> nodes and build up
     * a list of the files they reference.
     */
    external_refs_in_schema(&includes, contents);

    /* For each referenced file, recurse to add it (and potentially anything it
     * references, ...) to the XML.
     */
    for (GList *iter = includes; iter != NULL; iter = iter->next) {
        add_schema_file_to_xml(parent, iter->data, already_included);
    }

    free(contents);
    g_list_free_full(includes, free);
}

/*!
 * \internal
 * \brief Add an XML schema file and all the files it references as children
 *        of a given XML node
 *
 * \param[in,out] parent            The parent XML node
 * \param[in] name                  The schema version to compare against
 *                                  (for example, "pacemaker-3.1" or "pacemaker-3.1.rng")
 * \param[in,out] already_included  A list of names that have already been added
 *                                  to the parent node.
 *
 * \note The caller is responsible for freeing both the returned list and
 *       the elements of the list
 */
void
pcmk__build_schema_xml_node(xmlNode *parent, const char *name, GList **already_included)
{
    /* First, create an unattached node to add all the schema files to as children. */
    xmlNode *schema_node = pcmk__xe_create(NULL, PCMK__XA_SCHEMA);

    crm_xml_add(schema_node, PCMK_XA_VERSION, name);
    add_schema_file_to_xml(schema_node, name, already_included);

    /* Then, if we actually added any children, attach the node to parent.  If
     * we did not add any children (for instance, name was invalid), this prevents
     * us from returning a document with additional empty children.
     */
    if (schema_node->children != NULL) {
        xmlAddChild(parent, schema_node);
    } else {
        pcmk__xml_free(schema_node);
    }
}

/*!
 * \internal
 * \brief Return the directory containing any extra schema files that a
 *        Pacemaker Remote node fetched from the cluster
 */
const char *
pcmk__remote_schema_dir(void)
{
    const char *dir = pcmk__env_option(PCMK__ENV_REMOTE_SCHEMA_DIRECTORY);

    if (pcmk__str_empty(dir)) {
        return PCMK__REMOTE_SCHEMA_DIR;
    }

    return dir;
}

void
pcmk__log_known_schemas(void)
{
    int lpc = 0;

    for (GList *iter = known_schemas; iter != NULL; iter = iter->next) {
        pcmk__schema_t *schema = iter->data;

        if (schema->transform != NULL) {
            crm_debug("known_schemas[%d] => %s (upgrades with %s.xsl)",
                      lpc, schema->name, schema->transform);

        } else {
            crm_debug("known_schemas[%d] => %s", lpc, schema->name);
        }

        lpc++;
    }
}
