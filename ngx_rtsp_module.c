// ngx_rtsp_module.c
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_rtsp_module.h>
#include <ngx_event.h>        /* for ngx_event_module_t */

/* One global list of every “mountpoint” that’s ever been ANNOUNCE’d */
ngx_queue_t  streams_head;

/* Called once in master, before we fork workers */
ngx_int_t
ngx_rtsp_streams_init_module(ngx_cycle_t *cycle)
{
    ngx_queue_init(&streams_head);
    ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0,
                  "RTSP: global streams_head initialized");
    /* quick‐check that this really ran in **this** process */
    ngx_log_error(NGX_LOG_WARN, cycle->log, 0,
                      "RTSP[%P]: streams_head at %p", ngx_getpid(), &streams_head);
    return NGX_OK;
}

/* Minimal core‐module declaration just for our init hook */
static ngx_core_module_t  ngx_rtsp_module_ctx = {
    ngx_string("rtsp"),
    NULL,
    NULL
};

ngx_module_t ngx_rtsp_module = {
    NGX_MODULE_V1,
    &ngx_rtsp_module_ctx,  // Module context
    NULL,                  // Module directives
    NGX_CORE_MODULE,       // Module type
    NULL,                  // init master
    NULL, // init module
    ngx_rtsp_streams_init_module,                  // init process
    NULL,                  // init thread
    NULL,                  // exit thread
    NULL,                  // exit process
    NULL,                  // exit master
    NGX_MODULE_V1_PADDING  // Padding
};