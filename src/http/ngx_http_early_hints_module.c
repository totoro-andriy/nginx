#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include "ngx_http_early_hints_module.h"

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
    NULL,
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

static ngx_int_t build_hint_file_path(ngx_http_request_t *r, ngx_str_t *root, ngx_str_t *uri, const char *suffix, ngx_str_t *result) {
    size_t len = root->len + uri->len + ngx_strlen(suffix);
    result->data = ngx_pnalloc(r->pool, len + 1);
    if (result->data == NULL) return NGX_ERROR;
    u_char *p = ngx_cpymem(result->data, root->data, root->len);
    p = ngx_cpymem(p, uri->data, uri->len);
    p = ngx_cpymem(p, (u_char *)suffix, ngx_strlen(suffix));
    *p = '\0';
    result->len = len;
    return NGX_OK;
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

    if (ngx_strstr(uri.data, (u_char*)"/amp")) {
        global_suffix.len = sizeof("/global-amp") - 1;
        global_suffix.data = (u_char *)"/global-amp";
    } else {
        global_suffix.len = sizeof("/global") - 1;
        global_suffix.data = (u_char *)"/global";
    }

    if (build_hint_file_path(r, &conf->early_hints_root, &uri, ".txt", &custom_path) != NGX_OK ||
        build_hint_file_path(r, &conf->early_hints_root, &global_suffix, ".txt", &global_path) != NGX_OK) {
        return NGX_ERROR;
    }

    ngx_str_t files_to_try[2];
    ngx_uint_t count = 0;

    ngx_file_info_t fi;
    if (ngx_file_info((const char *) custom_path.data, &fi) != NGX_FILE_ERROR && ngx_is_file(&fi)) {
        files_to_try[count++] = custom_path;
        files_to_try[count++] = global_path;
    } else {
        ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
            "early hints: custom file not found, skipping global: %V", &custom_path);
        return NGX_OK;
    }

    for (ngx_uint_t i = 0; i < count; i++) {
        ngx_str_t path = files_to_try[i];

        ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
            "early hints: attempting file: %V", &path);

        ngx_file_info_t fi;
        if (ngx_file_info((const char *) path.data, &fi) == NGX_FILE_ERROR || !ngx_is_file(&fi)) {
            ngx_log_error(NGX_LOG_WARN, r->connection->log, ngx_errno,
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
            size_t len = strlen(line);
            if (len == 0 || ngx_strncmp(line, "Link:", 5) != 0) continue;
            if (line[len - 1] == '\n') line[--len] = '\0';
            const char *value_start = line + 5;
            while (*value_start == ' ') value_start++;

            if (*value_start == '\0') {
                ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                    "early hints: skipped empty value: %s", line);
                continue;
            }

            ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                "early hint parsed: %s", value_start);

            ngx_table_elt_t *h = ngx_list_push(&r->headers_out.headers);
            if (h == NULL) {
                fclose(fp);
                return NGX_ERROR;
            }

            h->hash = 2;
            h->key.len = sizeof("Link") - 1;
            h->key.data = (u_char *)"Link";
            h->value.len = len - (value_start - line);
            h->value.data = ngx_pnalloc(r->pool, h->value.len);
            if (h->value.data == NULL) {
                fclose(fp);
                return NGX_ERROR;
            }

            ngx_memcpy(h->value.data, value_start, h->value.len);
        }

        fclose(fp);
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