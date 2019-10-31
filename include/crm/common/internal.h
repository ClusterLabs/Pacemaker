/*
 * Copyright 2015-2019 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU Lesser General Public License
 * version 2.1 or later (LGPLv2.1+) WITHOUT ANY WARRANTY.
 */

#ifndef CRM_COMMON_INTERNAL__H
#define CRM_COMMON_INTERNAL__H

#include <glib.h>       /* for gboolean */
#include <dirent.h>     /* for struct dirent */
#include <unistd.h>     /* for getpid() */
#include <stdbool.h>    /* for bool */
#include <sys/types.h>  /* for uid_t and gid_t */

#include <crm/common/logging.h>

/* internal I/O utilities (from io.c) */

char *generate_series_filename(const char *directory, const char *series, int sequence,
                               gboolean bzip);
int get_last_sequence(const char *directory, const char *series);
void write_last_sequence(const char *directory, const char *series, int sequence, int max);
int crm_chown_last_sequence(const char *directory, const char *series, uid_t uid, gid_t gid);

bool pcmk__daemon_can_write(const char *dir, const char *file);
void crm_sync_directory(const char *name);

char *crm_read_contents(const char *filename);
int crm_write_sync(int fd, const char *contents);
int crm_set_nonblocking(int fd);
const char *crm_get_tmpdir(void);

void pcmk__close_fds_in_child(bool);


/* internal procfs utilities (from procfs.c) */

int crm_procfs_process_info(struct dirent *entry, char *name, int *pid);
int crm_procfs_pid_of(const char *name);
unsigned int crm_procfs_num_cores(void);


/* internal XML schema functions (from xml.c) */

void crm_schema_init(void);
void crm_schema_cleanup(void);


/* internal functions related to process IDs (from pid.c) */

/*!
 * \internal
 * \brief Detect if process per PID and optionally exe path (component) exists
 *
 * \param[in] pid     PID of process assumed alive, disproving of which to try
 * \param[in] daemon  exe path (component) to possibly match with procfs entry
 *
 * \return -1 on invalid PID specification, -2 when the calling process has no
 *         (is refused an) ability to (dis)prove the predicate,
 *         0 if the negation of the predicate is confirmed (check-through-kill
 *         indicates so, or the subsequent check-through-procfs-match on
 *         \p daemon when provided and procfs available at the standard path),
 *         1 if it cannot be disproved (reliably [modulo race conditions]
 *         when \p daemon provided, procfs available at the standard path
 *         and the calling process has permissions to access the respective
 *         procfs location, less so otherwise, since mere check-through-kill
 *         is exercised without powers to exclude PID recycled in the interim).
 *
 * \note This function cannot be used to verify \e authenticity of the process.
 */
int crm_pid_active(long pid, const char *daemon);

long crm_pidfile_inuse(const char *filename, long mypid, const char *daemon);
long crm_read_pidfile(const char *filename);
int crm_lock_pidfile(const char *filename, const char *name);


/* interal functions related to resource operations (from operations.c) */

char *generate_op_key(const char *rsc_id, const char *op_type,
                      guint interval_ms);
char *generate_notify_key(const char *rsc_id, const char *notify_type,
                          const char *op_type);
char *generate_transition_key(int action, int transition_id, int target_rc,
                              const char *node);
void filter_action_parameters(xmlNode *param_set, const char *version);


// miscellaneous utilities (from utils.c)

const char *pcmk_message_name(const char *name);


/* internal generic string functions (from strings.c) */

long long crm_int_helper(const char *text, char **end_text);
guint crm_parse_ms(const char *text);
bool crm_starts_with(const char *str, const char *prefix);
gboolean crm_ends_with(const char *s, const char *match);
gboolean crm_ends_with_ext(const char *s, const char *match);
char *add_list_element(char *list, const char *value);
bool crm_compress_string(const char *data, int length, int max, char **result,
                         unsigned int *result_len);
gint crm_alpha_sort(gconstpointer a, gconstpointer b);

static inline char *
crm_concat(const char *prefix, const char *suffix, char join)
{
    CRM_ASSERT(prefix && suffix);
    return crm_strdup_printf("%s%c%s", prefix, join, suffix);
}

static inline int
crm_strlen_zero(const char *s)
{
    return !s || *s == '\0';
}

static inline char *
crm_getpid_s()
{
    return crm_strdup_printf("%lu", (unsigned long) getpid());
}

/* convenience functions for failure-related node attributes */

#define CRM_FAIL_COUNT_PREFIX   "fail-count"
#define CRM_LAST_FAILURE_PREFIX "last-failure"

/*!
 * \internal
 * \brief Generate a failure-related node attribute name for a resource
 *
 * \param[in] prefix       Start of attribute name
 * \param[in] rsc_id       Resource name
 * \param[in] op           Operation name
 * \param[in] interval_ms  Operation interval
 *
 * \return Newly allocated string with attribute name
 *
 * \note Failure attributes are named like PREFIX-RSC#OP_INTERVAL (for example,
 *       "fail-count-myrsc#monitor_30000"). The '#' is used because it is not
 *       a valid character in a resource ID, to reliably distinguish where the
 *       operation name begins. The '_' is used simply to be more comparable to
 *       action labels like "myrsc_monitor_30000".
 */
static inline char *
crm_fail_attr_name(const char *prefix, const char *rsc_id, const char *op,
                   guint interval_ms)
{
    CRM_CHECK(prefix && rsc_id && op, return NULL);
    return crm_strdup_printf("%s-%s#%s_%u", prefix, rsc_id, op, interval_ms);
}

static inline char *
crm_failcount_name(const char *rsc_id, const char *op, guint interval_ms)
{
    return crm_fail_attr_name(CRM_FAIL_COUNT_PREFIX, rsc_id, op, interval_ms);
}

static inline char *
crm_lastfailure_name(const char *rsc_id, const char *op, guint interval_ms)
{
    return crm_fail_attr_name(CRM_LAST_FAILURE_PREFIX, rsc_id, op, interval_ms);
}

#endif /* CRM_COMMON_INTERNAL__H */
