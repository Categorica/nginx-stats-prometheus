#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <stdbool.h>

#if !(NGX_STAT_STUB)
#error "NGX_STAT_STUB is required for ngx_stats_prometheus module"
#endif

#ifndef NGX_HTTP_VERSION_30
#error "NGX_HTTP_VERSION_30 is required for ngx_stats_prometheus module"
#endif

// constants

#define STATS_BUCKET_COUNT   11
#define STATS_MAX_NAME_LEN   255
#define STATS_MAX_ZONES      512
#define STATS_SHM_SIZE       (1024 * 1024) // 1MB

#define STATS_RESOLVE_GLOBAL(fn, field) \
    static ngx_atomic_uint_t fn(void) { \
        return (ngx_atomic_uint_t) prom_shctx->global.field; \
    }

typedef ngx_atomic_uint_t (*stats_resolve_fn)(void);

typedef struct {
    const char      *label_value;
    stats_resolve_fn  resolve;
} stats_label_row_t;

typedef struct {
    const char              *name;
    const char              *help;
    const char              *type; // "counter", "gauge", "histogram"
    stats_resolve_fn         resolve;
    const char              *label_name;
    const stats_label_row_t *rows;
    size_t                   row_count;
} stats_metric_t;

typedef struct {
    ngx_atomic_t requests_total;
    ngx_atomic_t response_types[5]; // 1xx, 2xx, 3xx, 4xx, 5xx
    ngx_atomic_t request_bytes;
    ngx_atomic_t response_bytes;
} ngx_http_stats_prom_zone_stats_t;

typedef struct {
    ngx_rbtree_node_t                 node;
    ngx_queue_t                       queue;
    ngx_http_stats_prom_zone_stats_t  stats;
    u_short                           name_len;
    u_char                            name[1]; // var-len
} ngx_http_stats_prom_node_t;

typedef struct {
    ngx_atomic_t requests_total;
    ngx_atomic_t response_types[5]; // 1xx, 2xx, 3xx, 4xx, 5xx
    ngx_atomic_t method_get;
    ngx_atomic_t method_head;
    ngx_atomic_t method_post;
    ngx_atomic_t method_put;
    ngx_atomic_t method_delete;
    ngx_atomic_t method_options;
    ngx_atomic_t method_patch;
    ngx_atomic_t method_other;
    ngx_atomic_t request_bytes_total;
    ngx_atomic_t response_bytes_total;
    ngx_atomic_t http_10;
    ngx_atomic_t http_11;
    ngx_atomic_t http_20;
    ngx_atomic_t http_30;
    ngx_atomic_t duration_bucket[STATS_BUCKET_COUNT + 1];
    ngx_atomic_t duration_sum;
} ngx_http_stats_prom_global_t;

typedef struct {
    ngx_http_stats_prom_global_t    global;
    ngx_rbtree_t                    server_rbtree;
    ngx_rbtree_node_t               server_sentinel;
    ngx_queue_t                     server_queue;
    ngx_atomic_t                    server_count;

    ngx_rbtree_t                    upstream_rbtree;
    ngx_rbtree_node_t               upstream_sentinel;
    ngx_queue_t                     upstream_queue;
    ngx_atomic_t                    upstream_count;
} ngx_http_stats_prom_shctx_t;

typedef struct {
    ngx_shm_zone_t                 *shm_zone;
    size_t                          shm_size;
} ngx_http_stats_prom_conf_t;

// shared state (externed for workers via fork)
static ngx_http_stats_prom_shctx_t  *prom_shctx  = NULL;
static ngx_slab_pool_t              *prom_shpool = NULL;

static const double prom_buckets[STATS_BUCKET_COUNT] = {
    0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1, 2.5, 5, 10
};

// fwd decls
static ngx_int_t ngx_http_stats_prom_postconfiguration(ngx_conf_t *cf);
static void* ngx_http_stats_prom_create_main_conf(ngx_conf_t *cf);
static char* ngx_http_stats_prom_metrics_cmd(
    ngx_conf_t *cf,
    ngx_command_t *cmd,
    void *conf);

static ngx_int_t ngx_http_stats_prom_metrics_handler(ngx_http_request_t *r);
static ngx_int_t ngx_http_stats_prom_log_handler(ngx_http_request_t *r);
static ngx_int_t ngx_http_stats_prom_init_zone(
    ngx_shm_zone_t *shm_zone,
    void *data);

static void ngx_http_stats_prom_rbtree_insert(
    ngx_rbtree_node_t *temp,
    ngx_rbtree_node_t *node,
    ngx_rbtree_node_t *sentinel);

// module directives
static ngx_command_t ngx_http_stats_prom_commands[] = {
    {
        ngx_string("prometheus_metrics"),
        NGX_HTTP_LOC_CONF | NGX_CONF_NOARGS,
        ngx_http_stats_prom_metrics_cmd,
        NGX_HTTP_LOC_CONF_OFFSET,
        0,
        NULL
    },
    {
        ngx_string("prometheus_metrics_shm_size"),
        NGX_HTTP_MAIN_CONF | NGX_CONF_TAKE1,
        ngx_conf_set_size_slot,
        NGX_HTTP_MAIN_CONF_OFFSET,
        offsetof(ngx_http_stats_prom_conf_t, shm_size),
        NULL
    },
    ngx_null_command
};

// module context
static ngx_http_module_t ngx_http_stats_prom_module_ctx = {
    NULL,                                  /* preconfiguration */
    ngx_http_stats_prom_postconfiguration, /* postconfiguration */

    ngx_http_stats_prom_create_main_conf,  /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    NULL,                                  /* create location configuration */
    NULL                                   /* merge location configuration */
};

// module definition
ngx_module_t ngx_http_stats_prom_module = {
    NGX_MODULE_V1,
    &ngx_http_stats_prom_module_ctx, /* module context */
    ngx_http_stats_prom_commands,    /* module directives */
    NGX_HTTP_MODULE,                 /* module type */
    NULL,                            /* init master */
    NULL,                            /* init module */
    NULL,                            /* init process */
    NULL,                            /* init thread */
    NULL,                            /* exit thread */
    NULL,                            /* exit process */
    NULL,                            /* exit master */
    NGX_MODULE_V1_PADDING
};

static void
ngx_http_stats_prom_rbtree_insert(
    ngx_rbtree_node_t *temp,
    ngx_rbtree_node_t *node,
    ngx_rbtree_node_t *sentinel)
{
    ngx_rbtree_node_t **p;

    for (;;) {
        if (node->key < temp->key) {
            p = &temp->left;
        } else if (node->key > temp->key) {
            p = &temp->right;
        } else {
            ngx_http_stats_prom_node_t *n = (ngx_http_stats_prom_node_t *) node;
            ngx_http_stats_prom_node_t *t = (ngx_http_stats_prom_node_t *) temp;

            if (n->name_len < t->name_len) {
                p = &temp->left;
            } else if (n->name_len > t->name_len) {
                p = &temp->right;
            } else {
                p = (ngx_memcmp(n->name, t->name, n->name_len) < 0)
                    ? &temp->left
                    : &temp->right;
            }
        }

        if (*p == sentinel) {
            break;
        }

        temp = *p;
    }

    *p = node;
    node->parent = temp;
    node->left = sentinel;
    node->right = sentinel;
    ngx_rbt_red(node);
}

static ngx_http_stats_prom_node_t *
ngx_http_stats_prom_lookup(
    ngx_rbtree_t *tree,
    ngx_str_t *name,
    uint32_t hash)
{
    ngx_rbtree_node_t *node = tree->root;
    ngx_rbtree_node_t *sentinel = tree->sentinel;

    while (node != sentinel) {
        if (hash < node->key) {
            node = node->left;
            continue;
        }

        if (hash > node->key) {
            node = node->right;
            continue;
        }

        // hash matches, compare name
        ngx_http_stats_prom_node_t *pn = (ngx_http_stats_prom_node_t *) node;
        if (name->len == pn->name_len) {
            ngx_int_t rc = ngx_memcmp(name->data, pn->name, name->len);
            if (rc == 0) {
                return pn;
            }
            node = (rc < 0)
                ? node->left
                : node->right;
        } else {
            node = (name->len < pn->name_len)
                ? node->left
                : node->right;
        }
    }

    return NULL;
}

static ngx_http_stats_prom_node_t *
ngx_http_stats_prom_get_node(
    ngx_rbtree_t *tree,
    ngx_queue_t *queue,
    ngx_atomic_t *count,
    ngx_str_t *name)
{
    if (name->len == 0 || name->len > STATS_MAX_NAME_LEN) {
        return NULL;
    }

    uint32_t hash = ngx_crc32_short(name->data, name->len);

    ngx_shmtx_lock(&prom_shpool->mutex);
    ngx_http_stats_prom_node_t *node = ngx_http_stats_prom_lookup(tree, name, hash);
    if (node) {
        ngx_shmtx_unlock(&prom_shpool->mutex);
        return node;
    }

    if ((ngx_atomic_uint_t) *count >= STATS_MAX_ZONES) {
        ngx_shmtx_unlock(&prom_shpool->mutex);
        return NULL;
    }

    size_t alloc_size = offsetof(ngx_http_stats_prom_node_t, name) + name->len;
    node = ngx_slab_alloc_locked(prom_shpool, alloc_size);
    if (node == NULL) {
        ngx_shmtx_unlock(&prom_shpool->mutex);
        return NULL;
    }

    ngx_memzero(&node->stats, sizeof(ngx_http_stats_prom_zone_stats_t));
    node->node.key = hash;
    node->name_len = (u_short) name->len;
    ngx_memcpy(node->name, name->data, name->len);

    ngx_rbtree_insert(tree, &node->node);
    ngx_queue_insert_tail(queue, &node->queue);
    (void) ngx_atomic_fetch_add(count, 1);
    ngx_shmtx_unlock(&prom_shpool->mutex);
    return node;
}

static ngx_int_t
ngx_http_stats_prom_init_zone(
    ngx_shm_zone_t *shm_zone,
    void *data)
{
    ngx_slab_pool_t *shpool = (ngx_slab_pool_t *) shm_zone->shm.addr;

    if (data) {
        ngx_http_stats_prom_shctx_t *ctx = data;
        shm_zone->data = ctx;
        prom_shctx = ctx;
        prom_shpool = shpool;
        return NGX_OK;
    }

    ngx_http_stats_prom_shctx_t *ctx = ngx_slab_alloc(shpool, sizeof(ngx_http_stats_prom_shctx_t));
    if (ctx == NULL) {
        return NGX_ERROR;
    }

    ngx_memzero(ctx, sizeof(ngx_http_stats_prom_shctx_t));
    ngx_rbtree_init(
        &ctx->server_rbtree,
        &ctx->server_sentinel,
        ngx_http_stats_prom_rbtree_insert
    );
    ngx_queue_init(&ctx->server_queue);
    ngx_rbtree_init(
        &ctx->upstream_rbtree,
        &ctx->upstream_sentinel,
        ngx_http_stats_prom_rbtree_insert
    );
    ngx_queue_init(&ctx->upstream_queue);

    shm_zone->data = ctx;
    prom_shctx = ctx;
    prom_shpool = shpool;
    return NGX_OK;
}

// configuration callback

static void *
ngx_http_stats_prom_create_main_conf(ngx_conf_t *cf)
{
    ngx_http_stats_prom_conf_t *conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_stats_prom_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    conf->shm_size = NGX_CONF_UNSET_SIZE;

    return conf;
}

static char *
ngx_http_stats_prom_metrics_cmd(
    ngx_conf_t *cf,
    ngx_command_t *cmd,
    void *conf)
{
    ngx_http_core_loc_conf_t *clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    clcf->handler = ngx_http_stats_prom_metrics_handler;
    return NGX_CONF_OK;
}

// postconfiguration callback

static ngx_int_t
ngx_http_stats_prom_postconfiguration(ngx_conf_t *cf)
{
    ngx_str_t shm_name = ngx_string("ngx_stats_prometheus_shm");

    ngx_http_stats_prom_conf_t *conf = ngx_http_conf_get_module_main_conf(cf, ngx_http_stats_prom_module);
    if (conf->shm_size == (size_t) NGX_CONF_UNSET_SIZE) {
        conf->shm_size = STATS_SHM_SIZE;
    }
    conf->shm_zone = ngx_shared_memory_add(
        cf,
        &shm_name,
        conf->shm_size,
        &ngx_http_stats_prom_module
    );
    if (conf->shm_zone == NULL) {
        return NGX_ERROR;
    }

    conf->shm_zone->init = ngx_http_stats_prom_init_zone;
    ngx_http_core_main_conf_t *cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);
    ngx_http_handler_pt *h = ngx_array_push(&cmcf->phases[NGX_HTTP_LOG_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_http_stats_prom_log_handler;
    return NGX_OK;
}

static ngx_uint_t
status_class(ngx_uint_t status)
{
    if (status < 200) { return 0; }
    if (status < 300) { return 1; }
    if (status < 400) { return 2; }
    if (status < 500) { return 3; }
    return 4;
}

static void
ngx_http_stats_prom_bump_zone(
    ngx_http_stats_prom_zone_stats_t *stats,
    ngx_http_request_t *r)
{
    (void) ngx_atomic_fetch_add(&stats->requests_total, 1);
    if (r->headers_out.status >= 100 && r->headers_out.status < 600) {
        (void) ngx_atomic_fetch_add(
            &stats->response_types[status_class(r->headers_out.status)],
            1
        );
    }
    (void) ngx_atomic_fetch_add(&stats->request_bytes, r->request_length);
    (void) ngx_atomic_fetch_add(&stats->response_bytes, r->connection->sent);
}

static ngx_int_t
ngx_http_stats_prom_log_handler(ngx_http_request_t *r)
{
    if (prom_shctx == NULL) {
        return NGX_OK;
    }

    if (r != r->main) {
        return NGX_OK;
    }

    ngx_http_stats_prom_global_t *global = &prom_shctx->global;
    (void) ngx_atomic_fetch_add(&global->requests_total, 1);
    ngx_uint_t status = r->headers_out.status;
    if (status >= 100 && status < 600) {
        (void) ngx_atomic_fetch_add(
            &global->response_types[status_class(status)],
            1
        );
    }
    switch (r->method) {
    case NGX_HTTP_GET: (void) ngx_atomic_fetch_add(&global->method_get, 1);         break;
    case NGX_HTTP_HEAD: (void) ngx_atomic_fetch_add(&global->method_head, 1);       break;
    case NGX_HTTP_POST: (void) ngx_atomic_fetch_add(&global->method_post, 1);       break;
    case NGX_HTTP_PUT: (void) ngx_atomic_fetch_add(&global->method_put, 1);         break;
    case NGX_HTTP_DELETE: (void) ngx_atomic_fetch_add(&global->method_delete, 1);   break;
    case NGX_HTTP_OPTIONS: (void) ngx_atomic_fetch_add(&global->method_options, 1); break;
    case NGX_HTTP_PATCH: (void) ngx_atomic_fetch_add(&global->method_patch, 1);     break;
    default: (void) ngx_atomic_fetch_add(&global->method_other, 1);                 break;
    }

    (void) ngx_atomic_fetch_add(&global->request_bytes_total, r->request_length);
    (void) ngx_atomic_fetch_add(&global->response_bytes_total, r->connection->sent);

    if (r->http_version == NGX_HTTP_VERSION_10) {
        (void) ngx_atomic_fetch_add(&global->http_10, 1);
    } else if (r->http_version == NGX_HTTP_VERSION_11) {
        (void) ngx_atomic_fetch_add(&global->http_11, 1);
    } else if (r->http_version == NGX_HTTP_VERSION_20) {
        (void) ngx_atomic_fetch_add(&global->http_20, 1);
    } else if (r->http_version >= NGX_HTTP_VERSION_30) {
        (void) ngx_atomic_fetch_add(&global->http_30, 1);
    }

    ngx_time_t *tp = ngx_timeofday();
    ngx_msec_int_t duration = (ngx_msec_int_t) ((tp->sec - r->start_sec) * 1000 + (tp->msec - r->start_msec));
    if (duration < 0) {
        duration = 0;
    }
    double duration_sec = (double) duration / 1000;
    bool found_bucket = false;
    for (ngx_uint_t i = 0; i < STATS_BUCKET_COUNT; i++) {
        if (duration_sec <= prom_buckets[i]) {
            (void) ngx_atomic_fetch_add(&global->duration_bucket[i], 1);
            found_bucket = true;
            break;
        }
    }
    if (!found_bucket) {
        (void) ngx_atomic_fetch_add(&global->duration_bucket[STATS_BUCKET_COUNT], 1);
    }
    (void) ngx_atomic_fetch_add(&global->duration_sum, duration);

    // per server_name stats
    ngx_http_core_srv_conf_t *conf = ngx_http_get_module_srv_conf(r, ngx_http_core_module);
    ngx_str_t name = conf->server_name;

    if (name.len > 0) {
        ngx_http_stats_prom_node_t *node = ngx_http_stats_prom_get_node(
            &prom_shctx->server_rbtree,
            &prom_shctx->server_queue,
            &prom_shctx->server_count,
            &name
        );
        if (node) {
            ngx_http_stats_prom_bump_zone(&node->stats, r);
        }
    }

    // per upstream stats
    if (r->upstream != NULL
        && r->upstream->conf != NULL
        && r->upstream->conf->upstream != NULL)
    {
        name = r->upstream->conf->upstream->host;
        if (name.len > 0) {
            ngx_http_stats_prom_node_t *node = ngx_http_stats_prom_get_node(
                &prom_shctx->upstream_rbtree,
                &prom_shctx->upstream_queue,
                &prom_shctx->upstream_count,
                &name
            );
            if (node) {
                ngx_http_stats_prom_bump_zone(&node->stats, r);
            }
        }
    }

    return NGX_OK;
}

static ngx_atomic_uint_t
stats_resolve_up(void) { return 1; }

static ngx_atomic_uint_t
stats_resolve_pid(void) { return (ngx_atomic_uint_t) ngx_pid; }

STATS_RESOLVE_GLOBAL(stats_resolve_requests_total,    requests_total)

STATS_RESOLVE_GLOBAL(stats_resolve_resp_1xx,          response_types[0])
STATS_RESOLVE_GLOBAL(stats_resolve_resp_2xx,          response_types[1])
STATS_RESOLVE_GLOBAL(stats_resolve_resp_3xx,          response_types[2])
STATS_RESOLVE_GLOBAL(stats_resolve_resp_4xx,          response_types[3])
STATS_RESOLVE_GLOBAL(stats_resolve_resp_5xx,          response_types[4])

STATS_RESOLVE_GLOBAL(stats_resolve_method_get,        method_get)
STATS_RESOLVE_GLOBAL(stats_resolve_method_head,       method_head)
STATS_RESOLVE_GLOBAL(stats_resolve_method_post,       method_post)
STATS_RESOLVE_GLOBAL(stats_resolve_method_put,        method_put)
STATS_RESOLVE_GLOBAL(stats_resolve_method_delete,     method_delete)
STATS_RESOLVE_GLOBAL(stats_resolve_method_options,    method_options)
STATS_RESOLVE_GLOBAL(stats_resolve_method_patch,      method_patch)
STATS_RESOLVE_GLOBAL(stats_resolve_method_other,      method_other)

STATS_RESOLVE_GLOBAL(stats_resolve_request_bytes,     request_bytes_total)
STATS_RESOLVE_GLOBAL(stats_resolve_response_bytes,    response_bytes_total)

STATS_RESOLVE_GLOBAL(stats_resolve_http_10,           http_10)
STATS_RESOLVE_GLOBAL(stats_resolve_http_11,           http_11)
STATS_RESOLVE_GLOBAL(stats_resolve_http_20,           http_20)
STATS_RESOLVE_GLOBAL(stats_resolve_http_30,           http_30)

static ngx_atomic_uint_t
stats_resolve_conn_active(void) { return *ngx_stat_active; }

static ngx_atomic_uint_t
stats_resolve_conn_accepted(void) { return *ngx_stat_accepted; }

static ngx_atomic_uint_t
stats_resolve_conn_handled(void) { return *ngx_stat_handled; }

static ngx_atomic_uint_t
stats_resolve_conn_reading(void) { return *ngx_stat_reading; }

static ngx_atomic_uint_t
stats_resolve_conn_writing(void) { return *ngx_stat_writing; }

static ngx_atomic_uint_t
stats_resolve_conn_waiting(void) { return *ngx_stat_waiting; }

static const stats_label_row_t stats_response_class_rows[] = {
    { "1xx", stats_resolve_resp_1xx },
    { "2xx", stats_resolve_resp_2xx },
    { "3xx", stats_resolve_resp_3xx },
    { "4xx", stats_resolve_resp_4xx },
    { "5xx", stats_resolve_resp_5xx },
};

static const stats_label_row_t stats_method_rows[] = {
    { "GET",     stats_resolve_method_get },
    { "HEAD",    stats_resolve_method_head },
    { "POST",    stats_resolve_method_post },
    { "PUT",     stats_resolve_method_put },
    { "DELETE",  stats_resolve_method_delete },
    { "OPTIONS", stats_resolve_method_options },
    { "PATCH",   stats_resolve_method_patch },
    { "other",   stats_resolve_method_other },
};

static const stats_label_row_t stats_http_version_rows[] = {
    { "1.0", stats_resolve_http_10 },
    { "1.1", stats_resolve_http_11 },
    { "2.0", stats_resolve_http_20 },
    { "3.0", stats_resolve_http_30 },
};

#define STATS_PROM_SIMPLE(n, h, t, fn) \
    { n, h, t, fn, NULL, NULL, 0 }

#define STATS_PROM_LABELED(n, h, t, label, rows) \
    { n, h, t, NULL, label, rows, sizeof(rows) / sizeof(rows[0]) }

static const stats_metric_t stats_metrics_pre_hist[] = {
    STATS_PROM_SIMPLE(
        "nginx_up",
        "Whether the nginx process is up",
        "gauge",
        stats_resolve_up),

    STATS_PROM_SIMPLE(
        "nginx_process_pid",
        "PID of the worker process (serving metrics)",
        "gauge",
        stats_resolve_pid),

    STATS_PROM_SIMPLE(
        "nginx_http_requests_total",
        "Total number of HTTP requests",
        "counter",
        stats_resolve_requests_total),

    STATS_PROM_LABELED(
        "nginx_http_responses_total",
        "HTTP responses by status class",
        "counter",
        "status",
        stats_response_class_rows),

    STATS_PROM_LABELED(
        "nginx_http_requests_by_method_total",
        "HTTP requests by method",
        "counter",
        "method",
        stats_method_rows),

    STATS_PROM_SIMPLE(
        "nginx_http_request_bytes_total",
        "Total bytes received",
        "counter",
        stats_resolve_request_bytes),

    STATS_PROM_SIMPLE(
        "nginx_http_response_bytes_total",
        "Total bytes sent",
        "counter",
        stats_resolve_response_bytes),

    STATS_PROM_LABELED(
        "nginx_http_version_total",
        "Requests by HTTP version",
        "counter",
        "version",
        stats_http_version_rows),

    { .name = NULL }
};

static const stats_metric_t prom_metrics_post_hist[] = {

    STATS_PROM_SIMPLE(
        "nginx_connections_active",
        "Current active connections",
        "gauge",
        stats_resolve_conn_active),

    STATS_PROM_SIMPLE(
        "nginx_connections_accepted_total",
        "Total accepted connections",
        "counter",
        stats_resolve_conn_accepted),

    STATS_PROM_SIMPLE(
        "nginx_connections_handled_total",
        "Total handled connections",
        "counter",
        stats_resolve_conn_handled),

    STATS_PROM_SIMPLE(
        "nginx_connections_reading",
        "Connections currently reading request",
        "gauge",
        stats_resolve_conn_reading),

    STATS_PROM_SIMPLE(
        "nginx_connections_writing",
        "Connections currently writing response",
        "gauge",
        stats_resolve_conn_writing),

    STATS_PROM_SIMPLE(
        "nginx_connections_waiting",
        "Keep-alive connections waiting",
        "gauge",
        stats_resolve_conn_waiting),

    { .name = NULL }
};


static u_char *
stats_render_metrics(
    u_char *pos,
    u_char *end,
    const stats_metric_t *metrics)
{
    for (const stats_metric_t *m = metrics; m->name != NULL; m++) {
        pos = ngx_slprintf(
            pos,
            end,
            "# HELP %s %s\n# TYPE %s %s\n",
            m->name,
            m->help,
            m->name,
            m->type
        );

        if (m->label_name == NULL) {
            pos = ngx_slprintf(
                pos,
                end,
                "%s %uA\n\n",
                m->name,
                m->resolve()
            );
        } else {
            for (ngx_uint_t i = 0; i < m->row_count; i++) {
                pos = ngx_slprintf(
                    pos,
                    end,
                    "%s{%s=\"%s\"} %uA\n",
                    m->name,
                    m->label_name,
                    m->rows[i].label_value,
                    m->rows[i].resolve()
                );
            }
            pos = ngx_slprintf(pos, end, "\n");
        }
    }

    return pos;
}

static u_char *
stats_render_histogram(
    u_char *pos,
    u_char *end,
    ngx_http_stats_prom_global_t *global)
{
    pos = ngx_slprintf(
        pos,
        end,
        "# HELP nginx_http_request_duration_seconds Request duration in seconds\n"
        "# TYPE nginx_http_request_duration_seconds histogram\n"
    );
    ngx_atomic_uint_t cum = 0;
    for (ngx_uint_t i = 0; i < STATS_BUCKET_COUNT; i++) {
        cum += global->duration_bucket[i];
        ngx_uint_t whole = (ngx_uint_t) prom_buckets[i];
        ngx_uint_t frac = (ngx_uint_t) ((prom_buckets[i] - (double) whole) * 1000 + 0.5);
        pos = ngx_slprintf(
            pos,
            end,
            "nginx_http_request_duration_seconds_bucket{le=\"%ui.%03ui\"} %uA\n",
            whole,
            frac,
            cum
        );
    }
    cum += global->duration_bucket[STATS_BUCKET_COUNT];

    pos = ngx_slprintf(
        pos,
        end,
        "nginx_http_request_duration_seconds_bucket{le=\"+Inf\"} %uA\n",
        cum
    );
    ngx_atomic_uint_t sum_ms = (ngx_atomic_uint_t) global->duration_sum;
    pos = ngx_slprintf(
        pos,
        end,
        "nginx_http_request_duration_seconds_sum %uA.%03uA\n",
        (ngx_atomic_uint_t) (sum_ms / 1000),
        (ngx_atomic_uint_t) (sum_ms % 1000)
    );
    pos = ngx_slprintf(
        pos,
        end,
        "nginx_http_request_duration_seconds_count %uA\n\n",
        cum
    );
    return pos;
}

static ngx_uint_t
ngx_http_stats_prom_collect_nodes(
    ngx_queue_t *queue,
    ngx_http_stats_prom_node_t **out,
    ngx_uint_t max)
{
    ngx_uint_t count = 0;

    for (ngx_queue_t *q = ngx_queue_head(queue);
         q != ngx_queue_sentinel(queue) && count < max;
         q = ngx_queue_next(q))
    {
        ngx_http_stats_prom_node_t *node = ngx_queue_data(q, ngx_http_stats_prom_node_t, queue);
        out[count++] = node;
    }

    return count;
}

static const char *status_class_labels[] = { "1xx", "2xx", "3xx", "4xx", "5xx" };
typedef struct {
    const char *suffix;
    const char *help_fmt;
    size_t offset;
    ngx_uint_t nlabels;
} zone_metric_def_t;

static const zone_metric_def_t zone_metrics_defs[] = {
    { "requests_total", "Total requests by zone", offsetof(ngx_http_stats_prom_zone_stats_t, requests_total), 0 },
    { "response_total", "Response types by zone", offsetof(ngx_http_stats_prom_zone_stats_t, response_types), 5 },
    { "request_bytes_total", "Total request bytes by zone", offsetof(ngx_http_stats_prom_zone_stats_t, request_bytes), 0 },
    { "response_bytes_total", "Total response bytes by zone", offsetof(ngx_http_stats_prom_zone_stats_t, response_bytes), 0 },
};

static u_char *
prom_escape_label_value(u_char *pos, u_char *end, u_char *src, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        switch (src[i]) {

        case '\\':
        case '"':
            if (end - pos < 2) {
                return end;
            }
            *pos++ = '\\';
            *pos++ = src[i];
            break;

        case '\n':
            if (end - pos < 2) {
                return end;
            }
            *pos++ = '\\';
            *pos++ = 'n';
            break;

        default:
            if (pos >= end) {
                return end;
            }
            *pos++ = src[i];
            break;
        }
    }

    return pos;
}

static u_char *
render_zone_metrics(
    u_char *pos,
    u_char *end,
    const char *prefix,
    const char *label,
    ngx_http_stats_prom_node_t **node,
    ngx_uint_t count)
{
    if (count == 0) {
        return pos;
    }

    ngx_uint_t ndef = sizeof(zone_metrics_defs) / sizeof(zone_metrics_defs[0]);
    for (const zone_metric_def_t *d = zone_metrics_defs; d < zone_metrics_defs + ndef; d++) {
        pos = ngx_slprintf(
            pos,
            end,
            "# HELP %s_%s %s\n# TYPE %s_%s counter\n",
            prefix,
            d->suffix,
            d->help_fmt,
            prefix,
            d->suffix
        );

        for (ngx_uint_t i = 0; i < count; i++) {
            if (d->nlabels == 0) {
                ngx_atomic_t *val = (ngx_atomic_t *) ((u_char *) &node[i]->stats + d->offset);
                pos = ngx_slprintf(
                    pos,
                    end,
                    "%s_%s{%s=\"",
                    prefix,
                    d->suffix,
                    label
                );
                pos = prom_escape_label_value(
                    pos,
                    end,
                    node[i]->name,
                    node[i]->name_len
                );
                pos = ngx_slprintf(
                    pos,
                    end,
                    "\"} %uA\n",
                    *val
                );
            } else {
                for (ngx_uint_t j = 0; j < d->nlabels; j++) {
                    ngx_atomic_t *val = (ngx_atomic_t *) ((u_char *) &node[i]->stats
                        + d->offset + j * sizeof(ngx_atomic_t));
                    pos = ngx_slprintf(
                        pos,
                        end,
                        "%s_%s{%s=\"",
                        prefix,
                        d->suffix,
                        label
                    );
                    pos = prom_escape_label_value(
                        pos,
                        end,
                        node[i]->name,
                        node[i]->name_len
                    );
                    pos = ngx_slprintf(
                        pos,
                        end,
                        "\",status=\"%s\"} %uA\n",
                        status_class_labels[j],
                        *val
                    );
                }
            }
        }
    }
    pos = ngx_slprintf(
        pos,
        end,
        "\n"
    );
    return pos;
}

static ngx_int_t
ngx_http_stats_prom_metrics_handler(ngx_http_request_t *r)
{
    if (!(r->method & (NGX_HTTP_GET | NGX_HTTP_HEAD))) {
        ngx_int_t rc = ngx_http_discard_request_body(r);
        if (rc != NGX_OK) {
            return rc;
        }

        r->headers_out.status = NGX_HTTP_NOT_ALLOWED;
        r->headers_out.content_length_n = 0;
        r->header_only = 1;
        ngx_table_elt_t *h = ngx_list_push(&r->headers_out.headers);
        if (h != NULL) {
            h->hash = 1;
            ngx_str_set(&h->key, "Allow");
            ngx_str_set(&h->value, "GET, HEAD");
        }
        return ngx_http_send_header(r);
    }

    ngx_int_t rc = ngx_http_discard_request_body(r);
    if (rc != NGX_OK) {
        return rc;
    }

    if (prom_shctx == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    ngx_http_stats_prom_node_t **servers = ngx_palloc(
        r->pool,
        sizeof(ngx_http_stats_prom_node_t *) * STATS_MAX_ZONES
    );
    ngx_http_stats_prom_node_t **upstreams = ngx_palloc(
        r->pool,
        sizeof(ngx_http_stats_prom_node_t *) * STATS_MAX_ZONES
    );
    if (servers == NULL || upstreams == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    ngx_shmtx_lock(&prom_shpool->mutex);
    ngx_uint_t nservers = ngx_http_stats_prom_collect_nodes(
        &prom_shctx->server_queue,
        servers,
        STATS_MAX_ZONES
    );
    ngx_uint_t nupstreams = ngx_http_stats_prom_collect_nodes(
        &prom_shctx->upstream_queue,
        upstreams,
        STATS_MAX_ZONES
    );
    ngx_shmtx_unlock(&prom_shpool->mutex);

    if (nservers >= STATS_MAX_ZONES) {
        ngx_log_error(
            NGX_LOG_ERR,
            r->connection->log,
            0,
            "server zones exceed maximum limit (%d)",
            STATS_MAX_ZONES
        );
    }

    if (nupstreams >= STATS_MAX_ZONES) {
        ngx_log_error(
            NGX_LOG_ERR,
            r->connection->log,
            0,
            "upstream zones exceed maximum limit (%d)",
            STATS_MAX_ZONES
        );
    }

    // Base 16 KB handles all global and histogram metrics.  Per-zone
    // factor of 4096 bytes covers worst-case name lengths up to STATS_MAX_NAME_LEN
    size_t buf_size = 16 * 1024 + (nservers + nupstreams) * 4096;
    ngx_buf_t *b = ngx_create_temp_buf(r->pool, buf_size);
    if (b == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    u_char *pos = b->pos;
    u_char *end = b->end;

    pos = stats_render_metrics(pos, end, stats_metrics_pre_hist);
    pos = stats_render_histogram(pos, end, &prom_shctx->global);
    pos = stats_render_metrics(pos, end, prom_metrics_post_hist);

    if (pos >= end) {
        ngx_log_error(
            NGX_LOG_ERR,
            r->connection->log,
            0,
            "buffer size %uz is too small to render all metrics",
            buf_size
        );

        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    pos = render_zone_metrics(
        pos,
        end,
        "nginx_server",
        "server",
        servers,
        nservers
    );
    pos = render_zone_metrics(
        pos,
        end,
        "nginx_upstream",
        "upstream",
        upstreams,
        nupstreams
    );

    if (pos >= end) {
        ngx_log_error(
            NGX_LOG_ERR,
            r->connection->log,
            0,
            "buffer size %uz is too small to render all metrics",
            buf_size
        );

        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    b->last = pos;
    b->last_buf = 1;
    b->last_in_chain = 1;

    r->headers_out.status = NGX_HTTP_OK;
    r->headers_out.content_length_n = b->last - b->pos;
    ngx_str_set(&r->headers_out.content_type, "text/plain; version=0.0.4; charset=utf-8");

    rc = ngx_http_send_header(r);
    if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
        return rc;
    }
    ngx_chain_t out;
    out.buf = b;
    out.next = NULL;

    return ngx_http_output_filter(r, &out);

}