/*
 * Copyright 2019 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU Lesser General Public License
 * version 2.1 or later (LGPLv2.1+) WITHOUT ANY WARRANTY.
 */

#ifndef _GNU_SOURCE
#  define _GNU_SOURCE
#endif

#include <config.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <crm/crm.h>
#include <crm/common/output.h>
#include <crm/common/xml.h>
#include <crm/common/xml_internal.h>  /* pcmk__xml_serialize_fd_formatted */
#include <glib.h>

static gboolean legacy_xml = FALSE;

GOptionEntry pcmk__xml_output_entries[] = {
    { "output-legacy-xml", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_NONE, &legacy_xml,
      NULL,
      NULL },

    { NULL }
};

typedef struct private_data_s {
    xmlNode *root;
    GQueue *parent_q;
    GSList *errors;
    bool legacy_xml;
} private_data_t;

static void
xml_free_priv(pcmk__output_t *out) {
    private_data_t *priv = out->priv;

    if (priv == NULL) {
        return;
    }

    xmlFreeNode(priv->root);
    g_queue_free(priv->parent_q);
    g_slist_free(priv->errors);
    free(priv);
}

static bool
xml_init(pcmk__output_t *out) {
    private_data_t *priv = NULL;

    /* If xml_init was previously called on this output struct, just return. */
    if (out->priv != NULL) {
        return true;
    } else {
        out->priv = calloc(1, sizeof(private_data_t));
        if (out->priv == NULL) {
            return false;
        }

        priv = out->priv;
    }

    if (legacy_xml) {
        priv->root = create_xml_node(NULL, "crm_mon");
        xmlSetProp(priv->root, (pcmkXmlStr) "version", (pcmkXmlStr) VERSION);
    } else {
        priv->root = create_xml_node(NULL, "pacemaker-result");
        xmlSetProp(priv->root, (pcmkXmlStr) "api-version", (pcmkXmlStr) PCMK__API_VERSION);

        if (out->request != NULL) {
            xmlSetProp(priv->root, (pcmkXmlStr) "request", (pcmkXmlStr) out->request);
        }
    }

    priv->parent_q = g_queue_new();
    priv->errors = NULL;
    g_queue_push_tail(priv->parent_q, priv->root);

    /* Copy this from the file-level variable.  This means that it is only settable
     * as a command line option, and that pcmk__output_new must be called after all
     * command line processing is completed.
     */
    priv->legacy_xml = legacy_xml;

    return true;
}

static void
add_error_node(gpointer data, gpointer user_data) {
    char *str = (char *) data;
    xmlNodePtr node = (xmlNodePtr) user_data;
    pcmk_create_xml_text_node(node, "error", str);
}

static void
xml_finish(pcmk__output_t *out, crm_exit_t exit_status, bool print, void **copy_dest) {
    xmlNodePtr node;
    private_data_t *priv = out->priv;

    /* If root is NULL, xml_init failed and we are being called from pcmk__output_free
     * in the pcmk__output_new path.
     */
    if (priv->root == NULL) {
        return;
    }

    if (!legacy_xml) {
        char *rc_as_str = crm_itoa(exit_status);

        node = create_xml_node(priv->root, "status");
        xmlSetProp(node, (pcmkXmlStr) "code", (pcmkXmlStr) rc_as_str);
        xmlSetProp(node, (pcmkXmlStr) "message", (pcmkXmlStr) crm_exit_str(exit_status));

        if (g_slist_length(priv->errors) > 0) {
            xmlNodePtr errors_node = create_xml_node(node, "errors");
            g_slist_foreach(priv->errors, add_error_node, (gpointer) errors_node);
        }

        free(rc_as_str);
    }

    if (print) {
        pcmk__xml_serialize_fd_formatted(fileno(out->dest), priv->root);
    }

    if (copy_dest != NULL) {
        *copy_dest = copy_xml(priv->root);
    }
}

static void
xml_reset(pcmk__output_t *out) {
    private_data_t *priv = out->priv;

    CRM_ASSERT(priv != NULL);

    pcmk__xml_serialize_fd_formatted(fileno(out->dest), priv->root);

    xml_free_priv(out);
    xml_init(out);
}

static void
xml_subprocess_output(pcmk__output_t *out, int exit_status,
                      const char *proc_stdout, const char *proc_stderr) {
    xmlNodePtr node, child_node;
    char *rc_as_str = NULL;

    rc_as_str = crm_itoa(exit_status);

    node = pcmk__output_xml_create_parent(out, "command");
    xmlSetProp(node, (pcmkXmlStr) "code", (pcmkXmlStr) rc_as_str);

    if (proc_stdout != NULL) {
        child_node = pcmk_create_xml_text_node(node, "output", proc_stdout);
        xmlSetProp(child_node, (pcmkXmlStr) "source", (pcmkXmlStr) "stdout");
    }

    if (proc_stderr != NULL) {
        child_node = pcmk_create_xml_text_node(node, "output", proc_stderr);
        xmlSetProp(child_node, (pcmkXmlStr) "source", (pcmkXmlStr) "stderr");
    }

    pcmk__output_xml_add_node(out, node);
    free(rc_as_str);
}

static void
xml_version(pcmk__output_t *out, bool extended) {
    xmlNodePtr node;
    private_data_t *priv = out->priv;
    CRM_ASSERT(priv != NULL);

    node = pcmk__output_create_xml_node(out, "version");
    xmlSetProp(node, (pcmkXmlStr) "program", (pcmkXmlStr) "Pacemaker");
    xmlSetProp(node, (pcmkXmlStr) "version", (pcmkXmlStr) PACEMAKER_VERSION);
    xmlSetProp(node, (pcmkXmlStr) "author", (pcmkXmlStr) "Andrew Beekhof");
    xmlSetProp(node, (pcmkXmlStr) "build", (pcmkXmlStr) BUILD_VERSION);
    xmlSetProp(node, (pcmkXmlStr) "features", (pcmkXmlStr) CRM_FEATURES);
}

G_GNUC_PRINTF(2, 3)
static void
xml_err(pcmk__output_t *out, const char *format, ...) {
    private_data_t *priv = out->priv;
    int len = 0;
    char *buf = NULL;
    va_list ap;

    CRM_ASSERT(priv != NULL);
    va_start(ap, format);
    len = vasprintf(&buf, format, ap);
    CRM_ASSERT(len > 0);
    va_end(ap);

    priv->errors = g_slist_append(priv->errors, buf);
}

G_GNUC_PRINTF(2, 3)
static void
xml_info(pcmk__output_t *out, const char *format, ...) {
    /* This function intentially left blank */
}

static void
xml_output_xml(pcmk__output_t *out, const char *name, const char *buf) {
    xmlNodePtr parent = NULL;
    xmlNodePtr cdata_node = NULL;
    private_data_t *priv = out->priv;

    CRM_ASSERT(priv != NULL);

    parent = pcmk__output_create_xml_node(out, name);
    cdata_node = xmlNewCDataBlock(getDocPtr(parent), (pcmkXmlStr) buf, strlen(buf));
    xmlAddChild(parent, cdata_node);
}

static void
xml_begin_list(pcmk__output_t *out, const char *name,
               const char *singular_noun, const char *plural_noun) {
    if (legacy_xml) {
        pcmk__output_xml_create_parent(out, name);
    } else {
        xmlNodePtr list_node = NULL;

        list_node = pcmk__output_xml_create_parent(out, "list");
        xmlSetProp(list_node, (pcmkXmlStr) "name", (pcmkXmlStr) name);
    }
}

static void
xml_list_item(pcmk__output_t *out, const char *name, const char *content) {
    private_data_t *priv = out->priv;
    xmlNodePtr item_node = NULL;

    CRM_ASSERT(priv != NULL);

    item_node = pcmk__output_create_xml_text_node(out, "item", content);
    xmlSetProp(item_node, (pcmkXmlStr) "name", (pcmkXmlStr) name);
}

static void
xml_end_list(pcmk__output_t *out) {
    private_data_t *priv = out->priv;

    CRM_ASSERT(priv != NULL);

    if (priv->legacy_xml) {
        g_queue_pop_tail(priv->parent_q);
    } else {
        char *buf = NULL;
        xmlNodePtr node;

        node = g_queue_pop_tail(priv->parent_q);
        buf = crm_strdup_printf("%lu", xmlChildElementCount(node));
        xmlSetProp(node, (pcmkXmlStr) "count", (pcmkXmlStr) buf);
        free(buf);
    }
}

pcmk__output_t *
pcmk__mk_xml_output(char **argv) {
    pcmk__output_t *retval = calloc(1, sizeof(pcmk__output_t));

    if (retval == NULL) {
        return NULL;
    }

    retval->fmt_name = "xml";
    retval->request = g_strjoinv(" ", argv);
    retval->supports_quiet = false;

    retval->init = xml_init;
    retval->free_priv = xml_free_priv;
    retval->finish = xml_finish;
    retval->reset = xml_reset;

    retval->register_message = pcmk__register_message;
    retval->message = pcmk__call_message;

    retval->subprocess_output = xml_subprocess_output;
    retval->version = xml_version;
    retval->info = xml_info;
    retval->err = xml_err;
    retval->output_xml = xml_output_xml;

    retval->begin_list = xml_begin_list;
    retval->list_item = xml_list_item;
    retval->end_list = xml_end_list;

    return retval;
}

xmlNodePtr
pcmk__output_xml_create_parent(pcmk__output_t *out, const char *name) {
    xmlNodePtr node = pcmk__output_create_xml_node(out, name);
    pcmk__output_xml_push_parent(out, node);
    return node;
}

void
pcmk__output_xml_add_node(pcmk__output_t *out, xmlNodePtr node) {
    private_data_t *priv = out->priv;

    CRM_ASSERT(priv != NULL);
    CRM_ASSERT(node != NULL);

    xmlAddChild(g_queue_peek_tail(priv->parent_q), node);
}

xmlNodePtr
pcmk__output_create_xml_node(pcmk__output_t *out, const char *name) {
    private_data_t *priv = out->priv;

    CRM_ASSERT(priv != NULL);

    return create_xml_node(g_queue_peek_tail(priv->parent_q), name);
}

xmlNodePtr
pcmk__output_create_xml_text_node(pcmk__output_t *out, const char *name, const char *content) {
    xmlNodePtr node = pcmk__output_create_xml_node(out, name);
    xmlNodeSetContent(node, (pcmkXmlStr) content);
    return node;
}

void
pcmk__output_xml_push_parent(pcmk__output_t *out, xmlNodePtr parent) {
    private_data_t *priv = out->priv;

    CRM_ASSERT(priv != NULL);
    CRM_ASSERT(parent != NULL);

    g_queue_push_tail(priv->parent_q, parent);
}

void
pcmk__output_xml_pop_parent(pcmk__output_t *out) {
    private_data_t *priv = out->priv;

    CRM_ASSERT(priv != NULL);
    CRM_ASSERT(g_queue_get_length(priv->parent_q) > 0);

    g_queue_pop_tail(priv->parent_q);
}

xmlNodePtr
pcmk__output_xml_peek_parent(pcmk__output_t *out) {
    private_data_t *priv = out->priv;

    CRM_ASSERT(priv != NULL);

    /* If queue is empty NULL will be returned */
    return g_queue_peek_tail(priv->parent_q);
}
