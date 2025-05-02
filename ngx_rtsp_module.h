// ngx_rtsp_module.h
#ifndef NGX_RTSP_MODULE_H
#define NGX_RTSP_MODULE_H

#include <ngx_config.h>
#include <ngx_core.h>

/* shared across all sessions */
extern ngx_queue_t  streams_head;

/* master‐process init hook */
ngx_int_t ngx_rtsp_streams_init_module(ngx_cycle_t *cycle);

#endif  // NGX_RTSP_MODULE_H