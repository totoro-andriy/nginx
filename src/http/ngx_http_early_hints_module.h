#ifndef _NGX_HTTP_EARLY_HINTS_H_INCLUDED_
#define _NGX_HTTP_EARLY_HINTS_H_INCLUDED_

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

typedef struct {
    ngx_str_t early_hints_root;
} ngx_http_hint_loc_conf_t;

ngx_int_t ngx_http_add_custom_early_hint_links(ngx_http_request_t *r);

void *ngx_http_early_hints_create_loc_conf(ngx_conf_t *cf);
char *ngx_http_early_hints_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child);

extern ngx_module_t ngx_http_early_hints_module;

#endif // _NGX_HTTP_EARLY_HINTS_H_INCLUDED_
