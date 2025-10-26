#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include "ngx_http_early_hints_module.h"

static ngx_int_t ngx_http_early_hints_init(ngx_conf_t *cf);
static ngx_int_t ngx_http_early_hints_phase_handler(ngx_http_request_t *r);

static ngx_command_t ngx_http_early_hints_commands[] = {
    {
        ngx_string("early_hints_root"),
        NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
        ngx_conf_set_str_slot,
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_hint_loc_conf_t, early_hints_root),
        NULL
    },
    ngx_null_command
};

static ngx_http_module_t ngx_http_early_hints_module_ctx = {
    NULL,
    ngx_http_early_hints_init,
    NULL,
    NULL,
    NULL,
    NULL,
    ngx_http_early_hints_create_loc_conf,
    ngx_http_early_hints_merge_loc_conf
};

ngx_module_t ngx_http_early_hints_module = {
    NGX_MODULE_V1,
    &ngx_http_early_hints_module_ctx,
    ngx_http_early_hints_commands,
    NGX_HTTP_MODULE,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NGX_MODULE_V1_PADDING
};

static ngx_int_t
build_hint_file_path(ngx_http_request_t *r,
                     const ngx_str_t *root,
                     const ngx_str_t *uri,
                     const char *suffix,
                     ngx_str_t *out)
{
    ngx_uint_t    root_has_trailing_slash = 0;
    size_t        suffix_len = ngx_strlen(suffix);
    size_t        len;

    if (root->len == 0 || uri->len == 0 || uri->data[0] != '/') {
        return NGX_ERROR;
    }

    if (root->data[root->len - 1] == '/') {
        root_has_trailing_slash = 1;
    }

    len  = root->len;
    len += root_has_trailing_slash ? (uri->len - 1) : uri->len;
    len += suffix_len;

    out->data = ngx_pnalloc(r->pool, len + 1);
    if (out->data == NULL) {
        return NGX_ERROR;
    }

    u_char *p = out->data;
    p = ngx_cpymem(p, root->data, root->len);

    if (root_has_trailing_slash) {
        p = ngx_cpymem(p, uri->data + 1, uri->len - 1);
    } else {
        p = ngx_cpymem(p, uri->data, uri->len);
    }

    p = ngx_cpymem(p, (u_char *) suffix, suffix_len);
    *p = '\0';
    out->len = len;

    return NGX_OK;
}

/* strict detection of path segment "/amp/" only */
static ngx_flag_t
ngx_http_uri_has_strict_amp_dir(const ngx_str_t *uri)
{
    /* need at least 5 chars to contain "/amp/" */
    if (uri->len < 5) {
        return 0;
    }

    const u_char *p    = uri->data;
    const u_char *last = uri->data + uri->len - 5; /* stop so we can read p..p+4 safely */

    for (; p <= last; p++) {
        /* match exactly "/amp/" */
        if (p[0] == '/' && p[1] == 'a' && p[2] == 'm' && p[3] == 'p' && p[4] == '/') {
            return 1;
        }
    }

    return 0;
}

ngx_int_t ngx_http_add_custom_early_hint_links(ngx_http_request_t *r)
{
    ngx_http_hint_loc_conf_t *conf = ngx_http_get_module_loc_conf(r, ngx_http_early_hints_module);
    if (conf->early_hints_root.len == 0) return NGX_OK;

    ngx_str_t uri = r->uri;
    if (uri.len > 5 && ngx_strncmp(uri.data + uri.len - 5, ".html", 5) == 0) {
        uri.len -= 5;
    } else if (uri.len > 0 && uri.data[uri.len - 1] == '/') {
        ngx_str_t suffix = ngx_string("index");
        u_char *new_uri = ngx_pnalloc(r->pool, uri.len + suffix.len);
        if (new_uri == NULL) return NGX_ERROR;
        ngx_memcpy(new_uri, uri.data, uri.len);
        ngx_memcpy(new_uri + uri.len, suffix.data, suffix.len);
        uri.data = new_uri;
        uri.len += suffix.len;
    }

    ngx_str_t global_path, custom_path;
    ngx_str_t global_suffix;

    /*
    * Design/Behavior note:
    * Use "global-amp" only when the URI contains the strict path segment "/amp/".
    * This is intentional — no other variants are considered (e.g. "/amp" at end,
    * "/amplify/", "/example/").
    *
    * Although this split may look redundant, it is deliberate: we keep separate
    * global hint bundles for the main site and for AMP pages to avoid duplicating
    * instructions while ensuring AMP-specific styles/scripts are hinted only on
    * AMP routes.
    */
    if (ngx_http_uri_has_strict_amp_dir(&uri)) {
        global_suffix.len  = sizeof("/global-amp") - 1;
        global_suffix.data = (u_char *) "/global-amp";
    } else {
        global_suffix.len  = sizeof("/global") - 1;
        global_suffix.data = (u_char *) "/global";
    }

    if (build_hint_file_path(r, &conf->early_hints_root, &uri, ".txt", &custom_path) != NGX_OK ||
        build_hint_file_path(r, &conf->early_hints_root, &global_suffix, ".txt", &global_path) != NGX_OK) {
        return NGX_ERROR;
    }

    ngx_str_t files_to_try[2];
    ngx_uint_t count = 0;

    ngx_file_info_t fi;

    /*
     * Design note:
     * Only send early hints if a custom hint file exists for this URI.
     * The global hint file is used only as a supplement to an existing custom file.
     * If the custom file is missing, we intentionally skip all hint processing.
     * This is a deliberate design choice, not an oversight.
     */
    if (ngx_file_info((const char *) custom_path.data, &fi) != NGX_FILE_ERROR && ngx_is_file(&fi)) {
        files_to_try[count++] = custom_path;
        files_to_try[count++] = global_path;
    } else {
        ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
            "early hints: custom file not found, skipping global: %V", &custom_path);
        return NGX_OK;
    }

    ngx_array_t custom_buf;
    if (ngx_array_init(&custom_buf, r->pool, 8, sizeof(ngx_str_t)) != NGX_OK) {
        return NGX_ERROR;
    }

    for (ngx_uint_t i = 0; i < count; i++) {
        ngx_str_t  path = files_to_try[i];
        ngx_flag_t is_custom =
            (path.len == custom_path.len)
            && (ngx_memcmp(path.data, custom_path.data, custom_path.len) == 0);

        ngx_log_error(NGX_LOG_DEBUG, r->connection->log, 0,
            "early hints: attempting file: %V", &path);

        if (ngx_file_info((const char *) path.data, &fi) == NGX_FILE_ERROR || !ngx_is_file(&fi)) {
            ngx_log_error(NGX_LOG_DEBUG, r->connection->log, ngx_errno,
                "early hints: not found or not file: %V", &path);
            continue;
        }

        if (fi.st_size == 0) {
            ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                "early hints: file is empty: %V", &path);
            continue;
        }

        FILE *fp = fopen((const char *)path.data, "r");
        if (fp == NULL) {
            ngx_log_error(NGX_LOG_WARN, r->connection->log, ngx_errno,
                "early hints: failed to fopen: %V", &path);
            continue;
        }

        char line[2048];
        while (fgets(line, sizeof(line), fp)) {
            size_t len = ngx_strlen(line);
            if (len == 0 || ngx_strncmp(line, "Link:", 5) != 0) continue;

            // trimm \n
            if (len && line[len - 1] == '\n') line[--len] = '\0';
            // trimm \r
            if (len && line[len - 1] == '\r') line[--len] = '\0';
            // trim right spaces/tabs
            while (len && (line[len - 1] == ' ' || line[len - 1] == '\t')) line[--len] = '\0';

            const char *value_start = line + 5;

            /// Skip leading spaces and tabs
            while (*value_start == ' ' || *value_start == '\t') value_start++;

            if (*value_start == '\0') {
                ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                    "early hints: skipped empty value: %s", line);
                continue;
            }

            ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                "early hint parsed: %s", value_start);

            size_t value_len = len - (value_start - line);
            u_char *val = ngx_pnalloc(r->pool, value_len);
            if (val == NULL) { fclose(fp); return NGX_ERROR; }
            ngx_memcpy(val, value_start, value_len);

            if (is_custom) {
                ngx_str_t *dst = ngx_array_push(&custom_buf);
                if (dst == NULL) { fclose(fp); return NGX_ERROR; }
                dst->len  = value_len;
                dst->data = val;
            } else {
                ngx_table_elt_t *h = ngx_list_push(&r->headers_out.headers);
                if (h == NULL) { fclose(fp); return NGX_ERROR; }

                h->hash     = 2;
                h->key.len  = sizeof("Link") - 1;
                h->key.data = (u_char *) "Link";
                h->value.len  = value_len;
                h->value.data = val;
            }
        }

        fclose(fp);
    }
    if (custom_buf.nelts) {
    ngx_str_t *vals = custom_buf.elts;
        for (ngx_uint_t j = 0; j < custom_buf.nelts; j++) {
            ngx_table_elt_t *h = ngx_list_push(&r->headers_out.headers);
            if (h == NULL) return NGX_ERROR;

            h->hash     = 2;
            h->key.len  = sizeof("Link") - 1;
            h->key.data = (u_char *) "Link";
            h->value    = vals[j];
        }
    }

    return NGX_OK;
}

void *
ngx_http_early_hints_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_hint_loc_conf_t *conf;
    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_hint_loc_conf_t));
    if (conf == NULL) {
        return NULL;
    }
    conf->early_hints_root.len = 0;
    conf->early_hints_root.data = NULL;
    return conf;
}

char *
ngx_http_early_hints_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_hint_loc_conf_t *prev = parent;
    ngx_http_hint_loc_conf_t *conf = child;
    ngx_conf_merge_str_value(conf->early_hints_root, prev->early_hints_root, "");
    return NGX_CONF_OK;
}

static ngx_int_t
ngx_http_early_hints_init(ngx_conf_t *cf)
{
    ngx_http_core_main_conf_t *cmcf;
    ngx_http_handler_pt       *h;

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

    h = ngx_array_push(&cmcf->phases[NGX_HTTP_PRECONTENT_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_http_early_hints_phase_handler;
    return NGX_OK;
}

typedef struct {
    ngx_uint_t sent;
} ngx_http_eh_ctx_t;

static ngx_int_t
ngx_http_early_hints_phase_handler(ngx_http_request_t *r)
{
    ngx_http_eh_ctx_t         *ctx;
    ngx_http_hint_loc_conf_t  *hlcf;

    // main-request only
    if (r != r->main)                 return NGX_OK;
    if (r->header_sent)               return NGX_OK;
    if (r->http_version < NGX_HTTP_VERSION_20) return NGX_OK;

    // only for GET/HEAD
    if (!(r->method & (NGX_HTTP_GET|NGX_HTTP_HEAD))) return NGX_OK;

    // location has early_hints_root
    hlcf = ngx_http_get_module_loc_conf(r, ngx_http_early_hints_module);
    if (hlcf == NULL || hlcf->early_hints_root.len == 0) {
        return NGX_OK;
    }

    /*
     * Behavior note:
     * The phase handler may be invoked multiple times due to internal redirects
     * or subrequest processing.  We explicitly guard against duplicate
     * ngx_http_send_early_hints() calls by tracking 'ctx->sent'.
     * This is intentional and expected behavior.
     */
    ctx = ngx_http_get_module_ctx(r, ngx_http_early_hints_module);
    if (ctx && ctx->sent) {
        return NGX_OK;
    }
    if (ctx == NULL) {
        ctx = ngx_pcalloc(r->pool, sizeof(*ctx));
        if (ctx == NULL) {
            return NGX_ERROR;
        }
        ngx_http_set_ctx(r, ctx, ngx_http_early_hints_module);
    }

    ctx->sent = 1;

    return ngx_http_send_early_hints(r);
}