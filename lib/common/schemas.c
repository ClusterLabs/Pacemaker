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
static bool initialized = false;
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
xml_latest_schema_index(void)
{
    /* This function assumes that pcmk__schema_init() has been called
     * beforehand, so we have at least three schemas (one real schema, the
     * "pacemaker-next" schema, and the "none" schema).
     *
     * @COMPAT: pacemaker-next is deprecated since 2.1.5 and none since 2.1.8.
     * Update this when we drop those.
     */
    return g_list_length(known_schemas) - 3;
}

/*!
 * \internal
 * \brief Return the schema entry of the highest-versioned schema
 *
 * \return Schema entry of highest-versioned schema (or NULL on error)
 */
static GList *
get_highest_schema(void)
{
    /* The highest numerically versioned schema is the one before pacemaker-next
     *
     * @COMPAT pacemaker-next is deprecated since 2.1.5
     */
    GList *entry = pcmk__get_schema("pacemaker-next");

    CRM_ASSERT((entry != NULL) && (entry->prev != NULL));
    return entry->prev;
}

/*!
 * \internal
 * \brief Return the name of the highest-versioned schema
 *
 * \return Name of highest-versioned schema (or NULL on error)
 */
const char *
pcmk__highest_schema_name(void)
{
    GList *entry = get_highest_schema();

    return ((pcmk__schema_t *)(entry->data))->name;
}

/*!
 * \internal
 * \brief Find first entry of highest major schema version series
 *
 * \return Schema entry of first schema with highest major version
 */
GList *
pcmk__find_x_0_schema(void)
{
#if defined(PCMK__UNIT_TESTING)
    /* If we're unit testing, this can't be static because it'll stick
     * around from one test run to the next. It needs to be cleared out
     * every time.
     */
    GList *x_0_entry = NULL;
#else
    static GList *x_0_entry = NULL;
#endif

    pcmk__schema_t *highest_schema = NULL;

    if (x_0_entry != NULL) {
        return x_0_entry;
    }
    x_0_entry = get_highest_schema();
    highest_schema = x_0_entry->data;

    for (GList *iter = x_0_entry->prev; iter != NULL; iter = iter->prev) {
        pcmk__schema_t *schema = iter->data;

        /* We've found a schema in an older major version series.  Return
         * the index of the first one in the same major version series as
         * the highest schema.
         */
        if (schema->version.v[0] < highest_schema->version.v[0]) {
            x_0_entry = iter->next;
            break;
        }

        /* We're out of list to examine.  This probably means there was only
         * one major version series, so return the first schema entry.
         */
        if (iter->prev == NULL) {
            x_0_entry = known_schemas->data;
            break;
        }
    }
    return x_0_entry;
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
 */
static void
add_schema(enum pcmk__schema_validator validator,
           const pcmk__schema_version_t *version, const char *name,
           GList *transforms)
{
    pcmk__schema_t *schema = NULL;

    schema = pcmk__assert_alloc(1, sizeof(pcmk__schema_t));

    schema->validator = validator;
    schema->version.v[0] = version->v[0];
    schema->version.v[1] = version->v[1];
    schema->transforms = transforms;
    // schema->schema_index is set after all schemas are loaded and sorted

    if (version->v[0] || version->v[1]) {
        schema->name = schema_strdup_printf("pacemaker-", *version, "");
    } else {
        schema->name = pcmk__str_copy(name);
    }

    known_schemas = g_list_prepend(known_schemas, schema);
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

/*!
 * \internal
 * \brief Check whether a directory entry matches the upgrade XSLT pattern
 *
 * \param[in] entry  Directory entry whose filename to check
 *
 * \return 1 if the entry's filename is of the form
 *         <tt>upgrade-X.Y-ORDER.xsl</tt>, or 0 otherwise
 */
static int
transform_filter(const struct dirent *entry)
{
    return pcmk__str_eq(entry->d_name,
                        "upgrade-[[:digit:]]+.[[:digit:]]+-[[:digit:]]+.xsl",
                        pcmk__str_regex)? 1 : 0;
}

/*!
 * \internal
 * \brief Free a list of XSLT transform <tt>struct dirent</tt> objects
 *
 * \param[in,out] data  List to free
 */
static void
free_transform_list(void *data)
{
    g_list_free_full((GList *) data, free);
}

/*!
 * \internal
 * \brief Load names of upgrade XSLT stylesheets from a directory into a table
 *
 * Stylesheets must have names of the form "upgrade-X.Y-order.xsl", where:
 * * X is the schema major version
 * * Y is the schema minor version
 * * ORDER is the order in which the stylesheet occurs in the transform pipeline
 *
 * \param[in] dir  Directory containing XSLT stylesheets
 *
 * \return Table with schema version as key and \c GList of associated transform
 *         files (as <tt>struct dirent</tt>) as value
 */
static GHashTable *
load_transforms_from_dir(const char *dir)
{
    struct dirent **namelist = NULL;
    int num_matches = scandir(dir, &namelist, transform_filter, versionsort);
    GHashTable *transforms = pcmk__strkey_table(free, free_transform_list);

    for (int i = 0; i < num_matches; i++) {
        pcmk__schema_version_t version = SCHEMA_ZERO;
        int order = 0;  // Placeholder only

        if (sscanf(namelist[i]->d_name, "upgrade-%hhu.%hhu-%d.xsl",
                   &(version.v[0]), &(version.v[1]), &order) == 3) {

            char *version_s = crm_strdup_printf("%hhu.%hhu",
                                                version.v[0], version.v[1]);
            GList *list = g_hash_table_lookup(transforms, version_s);

            if (list == NULL) {
                /* Prepend is more efficient. However, there won't be many of
                 * these, and we want them to remain sorted by version. It's not
                 * worth reversing all the lists at the end.
                 *
                 * Avoid calling g_hash_table_insert() if the list already
                 * exists. Otherwise free_transform_list() gets called on it.
                 */
                list = g_list_append(list, namelist[i]);
                g_hash_table_insert(transforms, version_s, list);

            } else {
                list = g_list_append(list, namelist[i]);
                free(version_s);
            }

        } else {
            // Sanity only, should never happen thanks to transform_filter()
            free(namelist[i]);
        }
    }

    free(namelist);
    return transforms;
}

void
pcmk__load_schemas_from_dir(const char *dir)
{
    int lpc, max;
    struct dirent **namelist = NULL;
    GHashTable *transforms = NULL;

    max = scandir(dir, &namelist, schema_filter, schema_cmp_directory);
    if (max < 0) {
        crm_warn("Could not load schemas from %s: %s", dir, strerror(errno));
        return;
    }

    // Look for any upgrade transforms in the same directory
    transforms = load_transforms_from_dir(dir);

    for (lpc = 0; lpc < max; lpc++) {
        pcmk__schema_version_t version = SCHEMA_ZERO;

        if (version_from_filename(namelist[lpc]->d_name, &version)) {
            char *version_s = crm_strdup_printf("%hhu.%hhu",
                                                version.v[0], version.v[1]);
            char *orig_key = NULL;
            GList *transform_list = NULL;

            // The schema becomes the owner of transform_list
            g_hash_table_lookup_extended(transforms, version_s,
                                         (gpointer *) &orig_key,
                                         (gpointer *) &transform_list);
            g_hash_table_steal(transforms, version_s);

            add_schema(pcmk__schema_validator_rng, &version, NULL,
                       transform_list);

            free(version_s);
            free(orig_key);

        } else {
            // Shouldn't be possible, but makes static analysis happy
            crm_warn("Skipping schema '%s': could not parse version",
                     namelist[lpc]->d_name);
        }
    }

    for (lpc = 0; lpc < max; lpc++) {
        free(namelist[lpc]);
    }

    free(namelist);
    g_hash_table_destroy(transforms);
}

static gint
schema_sort_GCompareFunc(gconstpointer a, gconstpointer b)
{
    const pcmk__schema_t *schema_a = a;
    const pcmk__schema_t *schema_b = b;

    // @COMPAT pacemaker-next is deprecated since 2.1.5 and none since 2.1.8
    if (pcmk__str_eq(schema_a->name, "pacemaker-next", pcmk__str_none)) {
        if (pcmk__str_eq(schema_b->name, PCMK_VALUE_NONE, pcmk__str_none)) {
            return -1;
        } else {
            return 1;
        }
    } else if (pcmk__str_eq(schema_a->name, PCMK_VALUE_NONE, pcmk__str_none)) {
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
 * \c pcmk__load_schemas_from_dir(), after the initial sets in
 * \c pcmk__schema_init().
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
pcmk__schema_init(void)
{
    if (!initialized) {
        const char *remote_schema_dir = pcmk__remote_schema_dir();
        char *base = pcmk__xml_artefact_root(pcmk__xml_artefact_ns_legacy_rng);
        const pcmk__schema_version_t zero = SCHEMA_ZERO;
        int schema_index = 0;

        initialized = true;

        wrap_libxslt(false);

        pcmk__load_schemas_from_dir(base);
        pcmk__load_schemas_from_dir(remote_schema_dir);
        free(base);

        // @COMPAT: Deprecated since 2.1.5
        add_schema(pcmk__schema_validator_rng, &zero, "pacemaker-next", NULL);

        // @COMPAT Deprecated since 2.1.8
        add_schema(pcmk__schema_validator_none, &zero, PCMK_VALUE_NONE, NULL);

        /* add_schema() prepends items to the list, so in the simple case, this
         * just reverses the list. However if there were any remote schemas,
         * sorting is necessary.
         */
        pcmk__sort_schemas();

        // Now set the schema indexes and log the final result
        for (GList *iter = known_schemas; iter != NULL; iter = iter->next) {
            pcmk__schema_t *schema = iter->data;

            crm_debug("Loaded schema %d: %s", schema_index, schema->name);
            schema->schema_index = schema_index++;
        }
    }
}

static bool
validate_with_relaxng(xmlDocPtr doc, xmlRelaxNGValidityErrorFunc error_handler,
                      void *error_handler_context, const char *relaxng_file,
                      relaxng_ctx_cache_t **cached_ctx)
{
    int rc = 0;
    bool valid = true;
    relaxng_ctx_cache_t *ctx = NULL;

    CRM_CHECK(doc != NULL, return false);
    CRM_CHECK(relaxng_file != NULL, return false);

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
        valid = false;

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
    g_list_free_full(schema->transforms, free);
    free(schema);
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
    initialized = false;

    wrap_libxslt(true);
}

/*!
 * \internal
 * \brief Get schema list entry corresponding to a schema name
 *
 * \param[in] name  Name of schema to get
 *
 * \return Schema list entry corresponding to \p name, or NULL if unknown
 */
GList *
pcmk__get_schema(const char *name)
{
    // @COMPAT Not specifying a schema name is deprecated since 2.1.8
    if (name == NULL) {
        name = PCMK_VALUE_NONE;
    }
    for (GList *iter = known_schemas; iter != NULL; iter = iter->next) {
        pcmk__schema_t *schema = iter->data;

        if (pcmk__str_eq(name, schema->name, pcmk__str_casei)) {
            return iter;
        }
    }
    return NULL;
}

/*!
 * \internal
 * \brief Compare two schema version numbers given the schema names
 *
 * \param[in] schema1  Name of first schema to compare
 * \param[in] schema2  Name of second schema to compare
 *
 * \return Standard comparison result (negative integer if \p schema1 has the
 *         lower version number, positive integer if \p schema1 has the higher
 *         version number, of 0 if the version numbers are equal)
 */
int
pcmk__cmp_schemas_by_name(const char *schema1_name, const char *schema2_name)
{
    GList *entry1 = pcmk__get_schema(schema1_name);
    GList *entry2 = pcmk__get_schema(schema2_name);

    if (entry1 == NULL) {
        return (entry2 == NULL)? 0 : -1;

    } else if (entry2 == NULL) {
        return 1;

    } else {
        pcmk__schema_t *schema1 = entry1->data;
        pcmk__schema_t *schema2 = entry2->data;

        return schema1->schema_index - schema2->schema_index;
    }
}

static bool
validate_with(xmlNode *xml, pcmk__schema_t *schema,
              xmlRelaxNGValidityErrorFunc error_handler,
              void *error_handler_context)
{
    bool valid = false;
    char *file = NULL;
    relaxng_ctx_cache_t **cache = NULL;

    if (schema == NULL) {
        return false;
    }

    if (schema->validator == pcmk__schema_validator_none) {
        return true;
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

bool
pcmk__validate_xml(xmlNode *xml_blob, const char *validation,
                   xmlRelaxNGValidityErrorFunc error_handler,
                   void *error_handler_context)
{
    GList *entry = NULL;
    pcmk__schema_t *schema = NULL;

    CRM_CHECK((xml_blob != NULL) && (xml_blob->doc != NULL), return false);

    if (validation == NULL) {
        validation = crm_element_value(xml_blob, PCMK_XA_VALIDATE_WITH);
    }
    pcmk__warn_if_schema_deprecated(validation);

    // @COMPAT Not specifying a schema name is deprecated since 2.1.8
    if (validation == NULL) {
        bool valid = false;

        for (entry = known_schemas; entry != NULL; entry = entry->next) {
            schema = entry->data;
            if (validate_with(xml_blob, schema, NULL, NULL)) {
                valid = true;
                crm_xml_add(xml_blob, PCMK_XA_VALIDATE_WITH, schema->name);
                crm_info("XML validated against %s", schema->name);
            }
        }
        return valid;
    }

    entry = pcmk__get_schema(validation);
    if (entry == NULL) {
        pcmk__config_err("Cannot validate CIB with " PCMK_XA_VALIDATE_WITH
                         " set to an unknown schema such as '%s' (manually"
                         " edit to use a known schema)",
                         validation);
        return false;
    }

    schema = entry->data;
    return validate_with(xml_blob, schema, error_handler,
                         error_handler_context);
}

/*!
 * \internal
 * \brief Validate XML using its configured schema (and send errors to logs)
 *
 * \param[in] xml  XML to validate
 *
 * \return true if XML validates, otherwise false
 */
bool
pcmk__configured_schema_validates(xmlNode *xml)
{
    return pcmk__validate_xml(xml, NULL,
                              (xmlRelaxNGValidityErrorFunc) xml_log,
                              GUINT_TO_POINTER(LOG_ERR));
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

/*!
 * \internal
 * \brief Apply a single XSL transformation to given XML
 *
 * \param[in] xml        XML to transform
 * \param[in] transform  XSL name
 * \param[in] to_logs    If false, certain validation errors will be sent to
 *                       stderr rather than logged
 *
 * \return Transformed XML on success, otherwise NULL
 */
static xmlNode *
apply_transformation(const xmlNode *xml, const char *transform,
                     gboolean to_logs)
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
 * \brief Perform all transformations needed to upgrade XML to next schema
 *
 * \param[in] input_xml     XML to transform
 * \param[in] schema_index  Index of schema that successfully validates
 *                          \p original_xml
 * \param[in] to_logs       If false, certain validation errors will be sent to
 *                          stderr rather than logged
 *
 * \return XML result of schema transforms if successful, otherwise NULL
 */
static xmlNode *
apply_upgrade(const xmlNode *input_xml, int schema_index, gboolean to_logs)
{
    pcmk__schema_t *schema = g_list_nth_data(known_schemas, schema_index);
    pcmk__schema_t *upgraded_schema = g_list_nth_data(known_schemas,
                                                      schema_index + 1);

    xmlNode *old_xml = NULL;
    xmlNode *new_xml = NULL;
    xmlRelaxNGValidityErrorFunc error_handler = NULL;

    CRM_ASSERT((schema != NULL) && (upgraded_schema != NULL));

    if (to_logs) {
        error_handler = (xmlRelaxNGValidityErrorFunc) xml_log;
    }

    for (GList *iter = schema->transforms; iter != NULL; iter = iter->next) {
        const struct dirent *entry = iter->data;
        const char *transform = entry->d_name;

        crm_debug("Upgrading schema from %s to %s: applying XSL transform %s",
                  schema->name, upgraded_schema->name, transform);

        new_xml = apply_transformation(input_xml, transform, to_logs);
        pcmk__xml_free(old_xml);

        if (new_xml == NULL) {
            crm_err("XSL transform %s failed, aborting upgrade", transform);
            return NULL;
        }
        input_xml = new_xml;
        old_xml = new_xml;
    }

    // Ensure result validates with its new schema
    if (!validate_with(new_xml, upgraded_schema, error_handler,
                       GUINT_TO_POINTER(LOG_ERR))) {
        crm_err("Schema upgrade from %s to %s failed: "
                "XSL transform pipeline produced an invalid configuration",
                schema->name, upgraded_schema->name);
        crm_log_xml_debug(new_xml, "bad-transform-result");
        pcmk__xml_free(new_xml);
        return NULL;
    }

    crm_info("Schema upgrade from %s to %s succeeded",
             schema->name, upgraded_schema->name);
    return new_xml;
}

/*!
 * \internal
 * \brief Get the schema list entry corresponding to XML configuration
 *
 * \param[in] xml  CIB XML to check
 *
 * \return List entry of schema configured in \p xml
 */
static GList *
get_configured_schema(const xmlNode *xml)
{
    const char *schema_name = crm_element_value(xml, PCMK_XA_VALIDATE_WITH);

    pcmk__warn_if_schema_deprecated(schema_name);
    if (schema_name == NULL) {
        return NULL;
    }
    return pcmk__get_schema(schema_name);
}

/*!
 * \brief Update CIB XML to latest schema that validates it
 *
 * \param[in,out] xml              XML to update (may be freed and replaced
 *                                 after being transformed)
 * \param[in]     max_schema_name  If not NULL, do not update \p xml to any
 *                                 schema later than this one
 * \param[in]     transform        If false, do not update \p xml to any schema
 *                                 that requires an XSL transform
 * \param[in]     to_logs          If false, certain validation errors will be
 *                                 sent to stderr rather than logged
 *
 * \return Standard Pacemaker return code
 */
int
pcmk__update_schema(xmlNode **xml, const char *max_schema_name, bool transform,
                    bool to_logs)
{
    int max_stable_schemas = xml_latest_schema_index();
    int max_schema_index = 0;
    int rc = pcmk_rc_ok;
    GList *entry = NULL;
    pcmk__schema_t *best_schema = NULL;
    pcmk__schema_t *original_schema = NULL;
    xmlRelaxNGValidityErrorFunc error_handler = 
        to_logs ? (xmlRelaxNGValidityErrorFunc) xml_log : NULL;

    CRM_CHECK((xml != NULL) && (*xml != NULL) && ((*xml)->doc != NULL),
              return EINVAL);

    if (max_schema_name != NULL) {
        GList *max_entry = pcmk__get_schema(max_schema_name);

        if (max_entry != NULL) {
            pcmk__schema_t *max_schema = max_entry->data;

            max_schema_index = max_schema->schema_index;
        }
    }
    if ((max_schema_index < 1) || (max_schema_index > max_stable_schemas)) {
        max_schema_index = max_stable_schemas;
    }

    entry = get_configured_schema(*xml);
    if (entry == NULL) {
        // @COMPAT Not specifying a schema name is deprecated since 2.1.8
        entry = known_schemas;
    } else {
        original_schema = entry->data;
        if (original_schema->schema_index >= max_schema_index) {
            return pcmk_rc_ok;
        }
    }

    for (; entry != NULL; entry = entry->next) {
        pcmk__schema_t *current_schema = entry->data;
        xmlNode *upgrade = NULL;

        if (current_schema->schema_index > max_schema_index) {
            break;
        }

        if (!validate_with(*xml, current_schema, error_handler,
                           GUINT_TO_POINTER(LOG_ERR))) {
            crm_debug("Schema %s does not validate", current_schema->name);
            if (best_schema != NULL) {
                /* we've satisfied the validation, no need to check further */
                break;
            }
            rc = pcmk_rc_schema_validation;
            continue; // Try again with the next higher schema
        }

        crm_debug("Schema %s validates", current_schema->name);
        rc = pcmk_rc_ok;
        best_schema = current_schema;
        if (current_schema->schema_index == max_schema_index) {
            break; // No further transformations possible
        }

        if (!transform || (current_schema->transforms == NULL)
            || validate_with_silent(*xml, entry->next->data)) {
            /* The next schema either doesn't require a transform or validates
             * successfully even without the transform. Skip the transform and
             * try the next schema with the same XML.
             */
            continue;
        }

        upgrade = apply_upgrade(*xml, current_schema->schema_index, to_logs);
        if (upgrade == NULL) {
            /* The transform failed, so this schema can't be used. Later
             * schemas are unlikely to validate, but try anyway until we
             * run out of options.
             */
            rc = pcmk_rc_transform_failed;
        } else {
            best_schema = current_schema;
            pcmk__xml_free(*xml);
            *xml = upgrade;
        }
    }

    if (best_schema != NULL) {
        if ((original_schema == NULL)
            || (best_schema->schema_index > original_schema->schema_index)) {
            crm_info("%s the configuration schema to %s",
                     (transform? "Transformed" : "Upgraded"),
                     best_schema->name);
            crm_xml_add(*xml, PCMK_XA_VALIDATE_WITH, best_schema->name);
        }
    }
    return rc;
}

int
pcmk_update_configured_schema(xmlNode **xml)
{
    return pcmk__update_configured_schema(xml, true);
}

/*!
 * \brief Update XML from its configured schema to the latest major series
 *
 * \param[in,out] xml      XML to update
 * \param[in]     to_logs  If false, certain validation errors will be
 *                         sent to stderr rather than logged
 *
 * \return Standard Pacemaker return code
 */
int
pcmk_update_configured_schema(xmlNode **xml, bool to_logs) {
    pcmk__output_t *out = NULL;
    int rc = pcmk_rc_ok;

    rc = pcmk__xml_output_new(&out, xml);
    if (rc != pcmk_rc_ok) {
        return rc;
    }

    rc = pcmk__update_configured_schema(xml, out);
    return rc;
}

int
pcmk__update_configured_schema(xmlNode **xml, pcmk__output_t *out)
{
    int rc = pcmk_rc_ok;
    char *original_schema_name = NULL;

    // @COMPAT Not specifying a schema name is deprecated since 2.1.8
    const char *effective_original_name = "the first";

    int orig_version = -1;
    pcmk__schema_t *x_0_schema = pcmk__find_x_0_schema()->data;
    GList *entry = NULL;

    CRM_CHECK(xml != NULL, return EINVAL);

    original_schema_name = crm_element_value_copy(*xml, PCMK_XA_VALIDATE_WITH);
    pcmk__warn_if_schema_deprecated(original_schema_name);
    entry = pcmk__get_schema(original_schema_name);
    if (entry != NULL) {
        pcmk__schema_t *original_schema = entry->data;

        effective_original_name = original_schema->name;
        orig_version = original_schema->schema_index;
    }

    if (orig_version < x_0_schema->schema_index) {
        // Current configuration schema is not acceptable, try to update
        xmlNode *converted = NULL;
        const char *new_schema_name = NULL;
        pcmk__schema_t *schema = NULL;

        entry = NULL;
        converted = pcmk__xml_copy(NULL, *xml);
        if (pcmk__update_schema(&converted, NULL, true, out) == pcmk_rc_ok) {
            new_schema_name = crm_element_value(converted,
                                                PCMK_XA_VALIDATE_WITH);
            entry = pcmk__get_schema(new_schema_name);
        }
        schema = (entry == NULL)? NULL : entry->data;

        if ((schema == NULL)
            || (schema->schema_index < x_0_schema->schema_index)) {
            // Updated configuration schema is still not acceptable

            if ((orig_version == -1) || (schema == NULL)
                || (schema->schema_index < orig_version)) {
                // We couldn't validate any schema at all
                out->err(out, "Cannot upgrade configuration (claiming "
                              "%s schema) to at least %s because it "
                              "does not validate with any schema from "
                              "%s to the latest",
                              pcmk__s(original_schema_name, "no"),
                              x_0_schema->name, effective_original_name);
            } else {
                // We updated configuration successfully, but still too low
                out->err(out, "Cannot upgrade configuration (claiming "
                              "%s schema) to at least %s because it "
                              "would not upgrade past %s",
                              pcmk__s(original_schema_name, "no"),
                              x_0_schema->name,
                              pcmk__s(new_schema_name, "unspecified version"));
            }

            pcmk__xml_free(converted);
            converted = NULL;
            rc = pcmk_rc_transform_failed;

        } else {
            // Updated configuration schema is acceptable
            pcmk__xml_free(*xml);
            *xml = converted;

            if (schema->schema_index < xml_latest_schema_index()) {
                // NOTE: originally warn, not info
                out->info(out, "Configuration with %s schema was "
                               "internally upgraded to acceptable (but "
                               "not most recent) %s",
                               pcmk__s(original_schema_name, "no"),
                               schema->name);
            } else {
                out->info(out, "Configuration with %s schema was internally "
                               "upgraded to latest version %s",
                               pcmk__s(original_schema_name, "no"),
                               schema->name);
            }
        }

    } else {
        // @COMPAT the none schema is deprecated since 2.1.8
        pcmk__schema_t *none_schema = NULL;

        entry = pcmk__get_schema(PCMK_VALUE_NONE);
        CRM_ASSERT((entry != NULL) && (entry->data != NULL));

        none_schema = entry->data;
        if (orig_version >= none_schema->schema_index) {
            out->err(out, "Schema validation of configuration is "
                          "disabled (support for " PCMK_XA_VALIDATE_WITH
                          " set to \"" PCMK_VALUE_NONE "\" is deprecated"
                          " and will be removed in a future release)\n");
        }
    }

    free(original_schema_name);
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

    for (GList *iter = g_list_nth(known_schemas, xml_latest_schema_index());
         iter != NULL; iter = iter->prev) {
        pcmk__schema_t *schema = iter->data;

        if (schema_cmp(ver, schema->version) != -1) {
            continue;
        }

        for (GList *iter2 = g_list_last(schema->transforms); iter2 != NULL;
             iter2 = iter2->prev) {

            const struct dirent *entry = iter2->data;

            lst = g_list_prepend(lst, pcmk__str_copy(entry->d_name));
        }

        lst = g_list_prepend(lst, crm_strdup_printf("%s.rng", schema->name));
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
    xmlNode *schema_node = pcmk__xe_create(parent, PCMK__XA_SCHEMA);

    crm_xml_add(schema_node, PCMK_XA_VERSION, name);
    add_schema_file_to_xml(schema_node, name, already_included);

    if (schema_node->children == NULL) {
        // Not needed if empty. May happen if name was invalid, for example.
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

/*!
 * \internal
 * \brief Warn if a given validation schema is deprecated
 *
 * \param[in] Schema name to check
 */
void
pcmk__warn_if_schema_deprecated(const char *schema)
{
    if ((schema == NULL) ||
        pcmk__strcase_any_of(schema, "pacemaker-next", PCMK_VALUE_NONE, NULL)) {
        pcmk__config_warn("Support for " PCMK_XA_VALIDATE_WITH "='%s' is "
                          "deprecated and will be removed in a future release "
                          "without the possibility of upgrades (manually edit "
                          "to use a supported schema)", pcmk__s(schema, ""));
    }
}

// Deprecated functions kept only for backward API compatibility
// LCOV_EXCL_START

#include <crm/common/xml_compat.h>

gboolean
cli_config_update(xmlNode **xml, int *best_version, gboolean to_logs)
{
    int rc = pcmk__update_configured_schema(xml, to_logs);

    if (best_version != NULL) {
        const char *name = crm_element_value(*xml, PCMK_XA_VALIDATE_WITH);

        if (name == NULL) {
            *best_version = -1;
        } else {
            GList *entry = pcmk__get_schema(name);
            pcmk__schema_t *schema = (entry == NULL)? NULL : entry->data;

            *best_version = (schema == NULL)? -1 : schema->schema_index;
        }
    }
    return (rc == pcmk_rc_ok)? TRUE: FALSE;
}

// LCOV_EXCL_STOP
// End deprecated API
