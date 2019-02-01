/*
 * Copyright 2004-2018 Andrew Beekhof <andrew@beekhof.net>
 *
 * This source code is licensed under the GNU Lesser General Public License
 * version 2.1 or later (LGPLv2.1+) WITHOUT ANY WARRANTY.
 */

#include <crm_internal.h>

#ifndef _GNU_SOURCE
#  define _GNU_SOURCE
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <bzlib.h>
#include <sys/types.h>

char *
crm_itoa_stack(int an_int, char *buffer, size_t len)
{
    if (buffer != NULL) {
        snprintf(buffer, len, "%d", an_int);
    }

    return buffer;
}

long long
crm_int_helper(const char *text, char **end_text)
{
    long long result = -1;
    char *local_end_text = NULL;
    int saved_errno = 0;

    errno = 0;

    if (text != NULL) {
#ifdef ANSI_ONLY
        if (end_text != NULL) {
            result = strtol(text, end_text, 10);
        } else {
            result = strtol(text, &local_end_text, 10);
        }
#else
        if (end_text != NULL) {
            result = strtoll(text, end_text, 10);
        } else {
            result = strtoll(text, &local_end_text, 10);
        }
#endif

        saved_errno = errno;
        if (errno == EINVAL) {
            crm_err("Conversion of %s failed", text);
            result = -1;

        } else if (errno == ERANGE) {
            crm_err("Conversion of %s was clipped: %lld", text, result);

        } else if (errno != 0) {
            crm_log_perror(LOG_ERR, "Conversion of %s failed", text);
        }

        if (local_end_text != NULL && local_end_text[0] != '\0') {
            crm_err("Characters left over after parsing '%s': '%s'", text, local_end_text);
        }

        errno = saved_errno;
    }
    return result;
}

/*!
 * \brief Parse a long long integer value from a string
 *
 * \param[in] text          The string to parse
 * \param[in] default_text  Default string to parse if text is NULL
 *
 * \return Parsed value on success, -1 (and set errno) on error
 */
long long
crm_parse_ll(const char *text, const char *default_text)
{
    if (text == NULL) {
        text = default_text;
        if (text == NULL) {
            crm_err("No default conversion value supplied");
            errno = EINVAL;
            return -1;
        }
    }
    return crm_int_helper(text, NULL);
}

/*!
 * \brief Parse an integer value from a string
 *
 * \param[in] text          The string to parse
 * \param[in] default_text  Default string to parse if text is NULL
 *
 * \return Parsed value on success, -1 (and set errno) on error
 */
int
crm_parse_int(const char *text, const char *default_text)
{
    long long result = crm_parse_ll(text, default_text);

    if ((result < INT_MIN) || (result > INT_MAX)) {
        errno = ERANGE;
        return -1;
    }
    return (int) result;
}

/*!
 * \internal
 * \brief Parse a milliseconds value (without units) from a string
 *
 * \param[in] text  String to parse
 *
 * \return Milliseconds on success, 0 otherwise (and errno will be set)
 */
guint
crm_parse_ms(const char *text)
{
    if (text) {
        long long ms = crm_int_helper(text, NULL);

        if ((ms < 0) || (ms > G_MAXUINT)) {
            errno = ERANGE;
        }
        return errno? 0 : (guint) ms;
    }
    return 0;
}

gboolean
safe_str_neq(const char *a, const char *b)
{
    if (a == b) {
        return FALSE;

    } else if (a == NULL || b == NULL) {
        return TRUE;

    } else if (strcasecmp(a, b) == 0) {
        return FALSE;
    }
    return TRUE;
}

gboolean
crm_is_true(const char *s)
{
    gboolean ret = FALSE;

    if (s != NULL) {
        crm_str_to_boolean(s, &ret);
    }
    return ret;
}

int
crm_str_to_boolean(const char *s, int *ret)
{
    if (s == NULL) {
        return -1;

    } else if (strcasecmp(s, "true") == 0
               || strcasecmp(s, "on") == 0
               || strcasecmp(s, "yes") == 0 || strcasecmp(s, "y") == 0 || strcasecmp(s, "1") == 0) {
        *ret = TRUE;
        return 1;

    } else if (strcasecmp(s, "false") == 0
               || strcasecmp(s, "off") == 0
               || strcasecmp(s, "no") == 0 || strcasecmp(s, "n") == 0 || strcasecmp(s, "0") == 0) {
        *ret = FALSE;
        return 1;
    }
    return -1;
}

char *
crm_strip_trailing_newline(char *str)
{
    int len;

    if (str == NULL) {
        return str;
    }

    for (len = strlen(str) - 1; len >= 0 && str[len] == '\n'; len--) {
        str[len] = '\0';
    }

    return str;
}

gboolean
crm_str_eq(const char *a, const char *b, gboolean use_case)
{
    if (use_case) {
        return g_strcmp0(a, b) == 0;

        /* TODO - Figure out which calls, if any, really need to be case independent */
    } else if (a == b) {
        return TRUE;

    } else if (a == NULL || b == NULL) {
        /* shouldn't be comparing NULLs */
        return FALSE;

    } else if (strcasecmp(a, b) == 0) {
        return TRUE;
    }
    return FALSE;
}

static inline const char * null2emptystr(const char *);
static inline const char *
null2emptystr(const char *input)
{
    return (input == NULL) ? "" : input;
}

/*!
 * \brief Check whether a string starts with a certain sequence
 *
 * \param[in] str    String to check
 * \param[in] match  Sequence to match against beginning of \p str
 *
 * \return \c TRUE if \p str begins with match, \c FALSE otherwise
 * \note This is equivalent to !strncmp(s, prefix, strlen(prefix))
 *       but is likely less efficient when prefix is a string literal
 *       if the compiler optimizes away the strlen() at compile time,
 *       and more efficient otherwise.
 */
bool
crm_starts_with(const char *str, const char *prefix)
{
    const char *s = str;
    const char *p = prefix;

    if (!s || !p) {
        return FALSE;
    }
    while (*s && *p) {
        if (*s++ != *p++) {
            return FALSE;
        }
    }
    return (*p == 0);
}

static inline int crm_ends_with_internal(const char *, const char *, gboolean);
static inline int
crm_ends_with_internal(const char *s, const char *match, gboolean as_extension)
{
    if ((s == NULL) || (match == NULL)) {
        return 0;
    } else {
        size_t slen, mlen;

        if (match[0] != '\0'
            && (as_extension /* following commented out for inefficiency:
                || strchr(&match[1], match[0]) == NULL */))
                return !strcmp(null2emptystr(strrchr(s, match[0])), match);

        if ((mlen = strlen(match)) == 0)
            return 1;
        slen = strlen(s);
        return ((slen >= mlen) && !strcmp(s + slen - mlen, match));
    }
}

/*!
 * \internal
 * \brief Check whether a string ends with a certain sequence
 *
 * \param[in] s      String to check
 * \param[in] match  Sequence to match against end of \p s
 *
 * \return \c TRUE if \p s ends (verbatim, i.e., case sensitively)
 *         with match (including empty string), \c FALSE otherwise
 *
 * \see crm_ends_with_ext()
 */
gboolean
crm_ends_with(const char *s, const char *match)
{
    return crm_ends_with_internal(s, match, FALSE);
}

/*!
 * \internal
 * \brief Check whether a string ends with a certain "extension"
 *
 * \param[in] s      String to check
 * \param[in] match  Extension to match against end of \p s, that is,
 *                   its first character must not occur anywhere
 *                   in the rest of that very sequence (example: file
 *                   extension where the last dot is its delimiter,
 *                   e.g., ".html"); incorrect results may be
 *                   returned otherwise.
 *
 * \return \c TRUE if \p s ends (verbatim, i.e., case sensitively)
 *         with "extension" designated as \p match (including empty
 *         string), \c FALSE otherwise
 *
 * \note Main incentive to prefer this function over \c crm_ends_with
 *       where possible is the efficiency (at the cost of added
 *       restriction on \p match as stated; the complexity class
 *       remains the same, though: BigO(M+N) vs. BigO(M+2N)).
 *
 * \see crm_ends_with()
 */
gboolean
crm_ends_with_ext(const char *s, const char *match)
{
    return crm_ends_with_internal(s, match, TRUE);
}

/*
 * This re-implements g_str_hash as it was prior to glib2-2.28:
 *
 *   http://git.gnome.org/browse/glib/commit/?id=354d655ba8a54b754cb5a3efb42767327775696c
 *
 * Note that the new g_str_hash is presumably a *better* hash (it's actually
 * a correct implementation of DJB's hash), but we need to preserve existing
 * behaviour, because the hash key ultimately determines the "sort" order
 * when iterating through GHashTables, which affects allocation of scores to
 * clone instances when iterating through rsc->allowed_nodes.  It (somehow)
 * also appears to have some minor impact on the ordering of a few
 * pseudo_event IDs in the transition graph.
 */
guint
g_str_hash_traditional(gconstpointer v)
{
    const signed char *p;
    guint32 h = 0;

    for (p = v; *p != '\0'; p++)
        h = (h << 5) - h + *p;

    return h;
}

/* used with hash tables where case does not matter */
gboolean
crm_strcase_equal(gconstpointer a, gconstpointer b)
{
    return crm_str_eq((const char *) a, (const char *) b, FALSE);
}

guint
crm_strcase_hash(gconstpointer v)
{
    const signed char *p;
    guint32 h = 0;

    for (p = v; *p != '\0'; p++)
        h = (h << 5) - h + g_ascii_tolower(*p);

    return h;
}

static void
copy_str_table_entry(gpointer key, gpointer value, gpointer user_data)
{
    if (key && value && user_data) {
        g_hash_table_insert((GHashTable*)user_data, strdup(key), strdup(value));
    }
}

GHashTable *
crm_str_table_dup(GHashTable *old_table)
{
    GHashTable *new_table = NULL;

    if (old_table) {
        new_table = crm_str_table_new();
        g_hash_table_foreach(old_table, copy_str_table_entry, new_table);
    }
    return new_table;
}

char *
add_list_element(char *list, const char *value)
{
    int len = 0;
    int last = 0;

    if (value == NULL) {
        return list;
    }
    if (list) {
        last = strlen(list);
    }
    len = last + 2;             /* +1 space, +1 EOS */
    len += strlen(value);
    list = realloc_safe(list, len);
    sprintf(list + last, " %s", value);
    return list;
}

bool
crm_compress_string(const char *data, int length, int max, char **result, unsigned int *result_len)
{
    int rc;
    char *compressed = NULL;
    char *uncompressed = strdup(data);
#ifdef CLOCK_MONOTONIC
    struct timespec after_t;
    struct timespec before_t;
#endif

    if(max == 0) {
        max = (length * 1.1) + 600; /* recommended size */
    }

#ifdef CLOCK_MONOTONIC
    clock_gettime(CLOCK_MONOTONIC, &before_t);
#endif

    compressed = calloc(max, sizeof(char));
    CRM_ASSERT(compressed);

    *result_len = max;
    rc = BZ2_bzBuffToBuffCompress(compressed, result_len, uncompressed, length, CRM_BZ2_BLOCKS, 0,
                                  CRM_BZ2_WORK);

    free(uncompressed);

    if (rc != BZ_OK) {
        crm_err("Compression of %d bytes failed: %s " CRM_XS " bzerror=%d",
                length, bz2_strerror(rc), rc);
        free(compressed);
        return FALSE;
    }

#ifdef CLOCK_MONOTONIC
    clock_gettime(CLOCK_MONOTONIC, &after_t);

    crm_trace("Compressed %d bytes into %d (ratio %d:1) in %.0fms",
             length, *result_len, length / (*result_len),
             difftime (after_t.tv_sec, before_t.tv_sec) * 1000 +
             (after_t.tv_nsec - before_t.tv_nsec) / 1e6);
#else
    crm_trace("Compressed %d bytes into %d (ratio %d:1)",
             length, *result_len, length / (*result_len));
#endif

    *result = compressed;
    return TRUE;
}

/*!
 * \brief Compare two strings alphabetically (case-insensitive)
 *
 * \param[in] a  First string to compare
 * \param[in] b  Second string to compare
 *
 * \return 0 if strings are equal, -1 if a < b, 1 if a > b
 *
 * \note Usable as a GCompareFunc with g_list_sort().
 *       NULL is considered less than non-NULL.
 */
gint
crm_alpha_sort(gconstpointer a, gconstpointer b)
{
    if (!a && !b) {
        return 0;
    } else if (!a) {
        return -1;
    } else if (!b) {
        return 1;
    }
    return strcasecmp(a, b);
}

char *
crm_strdup_printf(char const *format, ...)
{
    va_list ap;
    int len = 0;
    char *string = NULL;

    va_start(ap, format);
    len = vasprintf (&string, format, ap);
    CRM_ASSERT(len > 0);
    va_end(ap);
    return string;
}

/*!
 * \brief Extract the name and value from an input string formatted as "name=value".
 * If unable to extract them, they are returned as NULL.
 *
 * \param[in]  input The input string, likely from the command line
 * \param[out] name  Everything before the first '=' in the input string
 * \param[out] value Everything after the first '=' in the input string
 *
 * \return 2 if both name and value could be extracted, 1 if only one could, and
 *         and error code otherwise
 */
int
pcmk_scan_nvpair(const char *input, char **name, char **value) {
#ifdef SSCANF_HAS_M
    *name = NULL;
    *value = NULL;
    if (sscanf(input, "%m[^=]=%ms", name, value) <= 0) {
        return -pcmk_err_bad_nvpair;
    }
#else
    char *sep = NULL;
    *name = NULL;
    *value = NULL;

    sep = strstr(optarg, "=");
    if (sep == NULL) {
        return -pcmk_err_bad_nvpair;
    }

    *name = strndup(input, sep-input);

    if (*name == NULL) {
        return -ENOMEM;
    }

    /* If the last char in optarg is =, the user gave no
     * value for the option.  Leave it as NULL.
     */
    if (*(sep+1) != '\0') {
        *value = strdup(sep+1);

        if (*value == NULL) {
            return -ENOMEM;
        }
    }
#endif

    if (*name != NULL && *value != NULL) {
        return 2;
    } else if (*name != NULL || *value != NULL) {
        return 1;
    } else {
        return -pcmk_err_bad_nvpair;
    }
}
