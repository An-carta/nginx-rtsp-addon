// ngx_rtsp_handler.c
// ngx_rtsp_handler.c
#include <ngx_config.h>
#include <ngx_core.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "ngx_rtsp_module.h"
#include <ngx_event.h>        // For ngx_add_timer()
#include <ngx_event_posted.h> // For event handling primitives
#include <string.h>

#define NGX_RTSP_MAX_ANNOUNCE  65536

// shared NALU pool (one per mountpoint)
typedef struct {
    ngx_queue_t        queue;     /* head of NALU FIFO */
    u_char            *sps;       /* decoded SPS */
    size_t             sps_len;
    u_char            *pps;       /* decoded PPS */
    size_t             pps_len;
} ngx_rtsp_nalu_pool_t;


typedef struct {
    ngx_str_t            name;    /* e.g. "live/test" */
    ngx_rtsp_nalu_pool_t pool;
    ngx_queue_t          link;    /* link into global streams list */
} ngx_rtsp_stream_t;

typedef struct {
    u_char     buf[NGX_RTSP_MAX_ANNOUNCE];
    size_t    buflen;     /* bytes stored in buf */
    size_t     hdr_len;     /* offset of end‐of‐headers +4 */
    ssize_t   content_len;
    int       state;      /* 0 = reading headers, 1 = reading body */
} ngx_rtsp_announce_ctx_t;

// Defined RTSP session struct
typedef struct {
    int          session_id;      /* a small random or monotonic ID */

    /* (if you ever use UDP fallback, you can keep these) */
    int          client_port_lo;
    int          client_port_hi;
    int          server_port_lo;
    int          server_port_hi;

    uint16_t     rtp_seq;
    uint32_t     rtp_ts;

    /* << NEW fields to support TCP‐interleaved RTP • MUST be here! >> */
    u_char       rtp_channel;     /* “0” from interleaved=0-1 */
    u_char       rtcp_channel;    /* “1” from interleaved=0-1 */
    ngx_event_t  rtp_event;       /* your 40 ms timer event */
    ngx_event_t             egress_ev;    /* timer for PLAY‐side egress */
    u_char       *remote_sdp;       /* pointer to the SDP text from ANNOUNCE */
    unsigned      recording:1;      /* are we in RECORD mode? */
    u_char        *fragbuf;          /* buffer for FU‐A assembly */
    size_t         fraglen;
    ngx_uint_t     setup_count;    // Track number of SETUPs
    ngx_uint_t     rtp_audio_channel;
    ngx_uint_t     rtcp_audio_channel;
    ngx_rtsp_stream_t  *stream;  /* the shared stream object */
    ngx_rtsp_announce_ctx_t  announce;
} ngx_rtsp_session_t;

typedef struct {
    ngx_queue_t   queue;
    uint32_t      ts;
    size_t        len;
    u_char       *data;
} ngx_rtsp_nalu_t;


static void ngx_rtsp_recv(ngx_event_t *rev);        // added function
void ngx_rtsp_init_connection(ngx_connection_t *c); //added function
static void ngx_rtsp_handle_options(ngx_connection_t *c, int cseq);
static void ngx_rtsp_handle_describe(ngx_connection_t *c, int cseq, const char *uri);
static void ngx_rtsp_send_simple_response(ngx_connection_t *c,
    int               cseq,
    int               code,
    const char       *reason,
    const char       *extra_headers);
static void ngx_rtsp_recv_rtp_interleaved(ngx_event_t *rev);
static ssize_t
parse_content_length(u_char *data, size_t len);

// adding function

/* Called when someone connects on port 554 */
void
ngx_rtsp_init_connection(ngx_connection_t *c)
{
    ngx_log_error(NGX_LOG_WARN, c->log, 0,
                          ">>> RTSP INIT CONNECTION fired, fd=%d  <<<", c->fd);

    ngx_rtsp_session_t *rs;

    ngx_log_error(NGX_LOG_ERR, c->log, 0,
                  ">>> RTSP INIT CONNECTION <<<");

    /* create and zero our per-connection RTSP state */
    rs = ngx_pcalloc(c->pool, sizeof(*rs));
    if (rs == NULL) {
        ngx_close_connection(c);
        ngx_log_error(NGX_LOG_ERR, c->log, 0,
                                  "FAILED to allocate RTSP session state");
        return;
    }

    /* pick a session ID (here just use ngx_time() low bits) */
    rs->session_id = (int)(ngx_time()) & 0xffff;
    rs->rtp_seq = 0;
    rs->rtp_ts = 0;
    rs->fragbuf = NULL;
    rs->fraglen = 0;
    rs->remote_sdp = NULL;
    rs->setup_count = 0;
    rs->rtp_audio_channel = 2;
    rs->rtcp_audio_channel = 3;
    rs->announce.buflen     = 0;
    rs->announce.hdr_len    = 0;
    rs->announce.content_len = -1;
    rs->announce.state      = 0;

    ngx_log_error(NGX_LOG_WARN, c->log, 0,
                          "    assigned session_id=%d", rs->session_id);

    /* attach it */
    c->data = rs;

    /* install our read handler and arm the event */
    c->read->handler = ngx_rtsp_recv;
    if (ngx_handle_read_event(c->read, 0) != NGX_OK) {
        ngx_close_connection(c);
    }
}

// nonblocking helper for one‐shot reads
static ngx_inline ssize_t
ngx_rtsp_recv_once(ngx_connection_t *c, u_char *p, size_t len)
{
    ssize_t n = c->recv(c, p, len);
    return (n == NGX_AGAIN) ? 0 : n;
}

static ngx_rtsp_stream_t *
get_stream(ngx_pool_t *pool, ngx_str_t *mount)
{
    ngx_queue_t       *q;
    ngx_rtsp_stream_t *s;

    // scan existing
    for (q = ngx_queue_head(&streams_head);
         q != ngx_queue_sentinel(&streams_head);
         q = ngx_queue_next(q))
    {
        s = ngx_queue_data(q, ngx_rtsp_stream_t, link);
        if (s->name.len == mount->len
         && ngx_strncmp(s->name.data, mount->data, mount->len) == 0)
        {
            return s;
        }
    }

    // not found → create
    s = ngx_pcalloc(pool, sizeof(*s));
    s->name.len  = mount->len;
    s->name.data = ngx_pnalloc(pool, mount->len);
    ngx_memcpy(s->name.data, mount->data, mount->len);
    ngx_queue_init(&s->pool.queue);
    s->pool.sps = s->pool.pps = NULL;

    ngx_queue_insert_tail(&streams_head, &s->link);
    return s;
}

//added function
static void
ngx_rtsp_send_h264_nal(ngx_connection_t   *c,
                       ngx_rtsp_session_t *rs,
                       const u_char      *nal, 
                       size_t             nal_len,
                       u_char             nal_type,
                       ngx_uint_t         marker)
{
    u_char  buf[1500];
    u_char *p = buf;

    //
    // 1) RTP header (12 bytes)
    //
    *p++ = 0x80;                         // V=2, P=0, X=0, CC=0
    *p++ = (marker ? 0x80 : 0) | 96;     // M=marker, PT=96
    *p++ = (rs->rtp_seq >> 8) & 0xFF;    // sequence #
    *p++ = rs->rtp_seq & 0xFF;
    rs->rtp_seq++;

    // timestamp
    *p++ = (rs->rtp_ts >> 24) & 0xFF;
    *p++ = (rs->rtp_ts >> 16) & 0xFF;
    *p++ = (rs->rtp_ts >>  8) & 0xFF;
    *p++ = rs->rtp_ts & 0xFF;
    rs->rtp_ts += 3600;                  // e.g. 90000/25fps = 3600

    // SSRC (fixed)
    *p++ = 0x12; *p++ = 0x34; *p++ = 0x56; *p++ = 0x78;

    //
    // 2) H.264 NAL header + payload
    //
    //    NAL header: F=0, NRI=0 (or set bits 5–6), type=nal_type
    *p++ = (nal_type & 0x1F);
    ngx_memcpy(p, nal, nal_len);
    p += nal_len;

    //
    // 3) Interleaved TCP header (“$” + channel + 16-bit BE length)
    //
    size_t payload_len = p - buf;
    u_char ihdr[4];
    ihdr[0] = 0x24;                     // ‘$’
    ihdr[1] = (u_char) rs->rtp_channel; // channel (0 for RTP)
    ihdr[2] = (payload_len >> 8) & 0xFF;
    ihdr[3] = payload_len & 0xFF;

    //
    // 4) Send it
    //
    c->send(c, ihdr, 4);
    c->send(c, buf, payload_len);

    ngx_log_error(NGX_LOG_WARN, c->log, 0,
        "RTSP RTP→TCP ch=%d len=%uz seq=%ui ts=%ui",
        rs->rtp_channel, payload_len, rs->rtp_seq, rs->rtp_ts);
}


static ssize_t
parse_content_length(u_char *data, size_t len)
{
    const char *key   = "Content-Length:";
    size_t      keylen = ngx_strlen(key);
    u_char     *p     = data;
    u_char     *end   = data + len;

    while (p + keylen < end) {
        if (ngx_strncasecmp(p, (u_char*)key, keylen) == 0) {
            p += keylen;
            while (p < end && (*p == ' ' || *p == '\t')) p++;
            ssize_t v = 0;
            while (p < end && *p >= '0' && *p <= '9') {
                v = v * 10 + (*p++ - '0');
            }
            return v;
        }
        /* skip to next line */
        while (p < end && *p != '\n') p++;
        if (p < end) p++;
    }
    return -1;
}
/*
static ngx_int_t
read_exact(ngx_connection_t *c, u_char *buf, size_t len)
{
    ssize_t  total = 0, n;
    while ((size_t) total < len) {
        n = c->recv(c, buf + total, len - total);
        if (n <= 0) {
            return NGX_ERROR;
        }
        total += n;
    }
    return NGX_OK;
}
*/
static void
ngx_rtsp_send_simple_response(ngx_connection_t *c,
                              int               cseq,
                              int               code,
                              const char       *status,
                              const char       *extra)
{
    u_char  buf[1024];
    u_char *p;

    // snprintf returns pointer to end of written data
    p = ngx_snprintf(buf, sizeof(buf),
        "RTSP/1.0 %d %s\r\n"
        "CSeq: %d\r\n"
        "%s"           // extra headers, already ending in "\r\n"
        "\r\n",       // blank line
        code, status,
        cseq,
        extra ? extra : "");

    // length is end minus start
    size_t n = p - buf;

    ngx_log_error(NGX_LOG_DEBUG, c->log, 0,
                  "RTSP → send response: %*s", n, buf);

    c->send(c, buf, n);
}

static void
ngx_rtsp_egress(ngx_event_t *ev)
{
    ngx_connection_t   *c   = ev->data;
    ngx_rtsp_session_t *rs  = c->data;
    ngx_rtsp_nalu_pool_t *pool = &rs->stream->pool;
    ngx_queue_t        *q;
    ngx_rtsp_nalu_t    *n;

    /* send SPS/PPS once */
    if (pool->sps) {
                ngx_rtsp_send_h264_nal(c, rs, pool->sps, pool->sps_len, 7, 0);
                ngx_rtsp_send_h264_nal(c, rs, pool->pps, pool->pps_len, 8, 1);
                pool->sps = pool->pps = NULL;
             }
    

    /* pop & send as many as fit in 40ms tick */
    if (!ngx_queue_empty(&pool->queue)) {
                q = ngx_queue_head(&pool->queue);
        n = ngx_queue_data(q, ngx_rtsp_nalu_t, queue);
        ngx_queue_remove(q);

        /* marker=1 on last NAL of each frame? simplify to always 1 */
        ngx_rtsp_send_h264_nal(c, rs, n->data, n->len, n->data[0]&0x1F, 1);

        /* free it */
        ngx_pfree(c->pool, n);
    }

    /* reschedule if more pending */
    if (!ngx_queue_empty(&pool->queue)) {
                ngx_add_timer(&rs->egress_ev, 40);
            }
}

// function to handle options

// OPTIONS: advertise which methods we support
static void
ngx_rtsp_handle_options(ngx_connection_t *c, int cseq)
{
    const char *hdrs =
        "Public: OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN\r\n";
    ngx_rtsp_send_simple_response(c, cseq, 200, "OK", hdrs);
}

// function to handle describe

// DESCRIBE: return a tiny SDP payload
static void
ngx_rtsp_handle_describe(ngx_connection_t *c, int cseq, const char *uri)
{
    u_char  sdp_buf[1024];
    u_char  extra[256];
    u_char *p;
    size_t  n;

    /* Get client IP dynamically */
    struct sockaddr_in *sin = (struct sockaddr_in *)c->sockaddr;
    char *client_ip = inet_ntoa(sin->sin_addr);

    p = ngx_snprintf(sdp_buf, sizeof(sdp_buf),
        "v=0\r\n"
        "o=- 0 0 IN IP4 %s\r\n"
        "s=Stream\r\n"
        "c=IN IP4 %s\r\n"
        "t=0 0\r\n"
        "a=control:*\r\n"
        "m=video 0 RTP/AVP 96\r\n"
        "a=rtpmap:96 H264/90000\r\n"
        "a=fmtp:96 packetization-mode=1\r\n"
        "a=control:trackID=0\r\n",
        client_ip, client_ip);

    n = p - sdp_buf;

    /* Prepare headers */
    ngx_snprintf(extra, sizeof(extra),
        "Content-Type: application/sdp\r\n"
        "Content-Length: %uz\r\n",
        n);

    ngx_rtsp_send_simple_response(c, cseq, 200, "OK", (char*)extra);
    c->send(c, sdp_buf, n);
}

static void
ngx_rtsp_handle_setup(ngx_connection_t    *c,
                      int                  cseq,
                      const char         *raw,
                      ngx_rtsp_session_t *rs)
{
    /* lazy‐init session_id if somehow zero */
    if (rs->session_id == 0) {
        rs->session_id = (int)(ngx_time()) & 0xffff;
        ngx_log_error(NGX_LOG_WARN, c->log, 0,
                      "RTSP SETUP: lazily init session_id=%d",
                      rs->session_id);
    }

    /* NEW: Track setup count per session */
    if (rs->setup_count % 2 == 0) {
        // First setup (video): channels 0-1
        rs->rtp_channel = 0;
        rs->rtcp_channel = 1;
        ngx_log_error(NGX_LOG_DEBUG, c->log, 0,
                      "SETUP: assigned video channels %d-%d",
                      rs->rtp_channel, rs->rtcp_channel);
    } else {
        // Second setup (audio): channels 2-3
        rs->rtp_channel = 2;
        rs->rtcp_channel = 3;
        ngx_log_error(NGX_LOG_DEBUG, c->log, 0,
                      "SETUP: assigned audio channels %d-%d",
                      rs->rtp_channel, rs->rtcp_channel);
    }
    rs->setup_count++;

    /* Modified logging */
    ngx_log_error(NGX_LOG_WARN, c->log, 0,
                  "RTSP SETUP: interleaved=%d-%d, session=%d",
                  rs->rtp_channel, rs->rtcp_channel, rs->session_id);

    /* Updated Transport header */
    u_char extra[128];
    ngx_snprintf(extra, sizeof(extra),
        "Transport: RTP/AVP/TCP;unicast;interleaved=%d-%d\r\n"
        "Session: %d\r\n",
        rs->rtp_channel, rs->rtcp_channel,  // Dynamic channels
        rs->session_id);

    ngx_rtsp_send_simple_response(c, cseq, 200, "OK", (char*)extra);
}


static void
ngx_rtsp_handle_play(ngx_connection_t *c,
                     int               cseq,
                     const char       *raw,
                     ngx_rtsp_session_t *rs)
{
    /* send PLAY 200 OK */
    char extra[64];
    ngx_snprintf((u_char*)extra, sizeof(extra),
                 "Session: %d\r\n", rs->session_id);
    ngx_rtsp_send_simple_response(c, cseq, 200, "OK", extra);

    /* schedule PLAY‐side egress timer */
    rs->egress_ev.handler    = ngx_rtsp_egress;
    rs->egress_ev.data       = c;
    rs->egress_ev.log        = c->log;
    rs->egress_ev.cancelable = 1;
    ngx_add_timer(&rs->egress_ev, 40);
}

static void
ngx_rtsp_handle_announce(ngx_connection_t *c, int cseq)
{
    ngx_rtsp_session_t     *rs   = c->data;
    ngx_rtsp_announce_ctx_t *ctx = &rs->announce;
    u_char                 *buf  = ctx->buf;
    u_char                 *hdr_end;
    ssize_t                  n;
    char                     line1[256], *method, *uri, *p;
    ngx_str_t                mount;
    ngx_rtsp_nalu_pool_t    *pool;
    u_char                  *sdp;
    size_t                   got;

    /* 1) Read as many bytes as we can into buf[] */
    for ( ;; ) {
        n = c->recv(c, buf + ctx->buflen,
                    NGX_RTSP_MAX_ANNOUNCE - ctx->buflen - 1);
        if (n == NGX_AGAIN) {
            /* need more data later */
            if (ngx_handle_read_event(c->read, 0) != NGX_OK) {
                ngx_close_connection(c);
            }
            return;
        }
        if (n <= 0) {
            ngx_rtsp_send_simple_response(c, cseq,
                                          400, "Bad Request", NULL);
            return;
        }
        ctx->buflen += n;
        buf[ctx->buflen] = '\0';

        /* if we haven’t located headers yet, look for CRLFCRLF */
        if (ctx->state == 0) {
            hdr_end = ngx_strnstr(buf, "\r\n\r\n", ctx->buflen);
            if (hdr_end) {
                /* mark header end +4, switch to body‐reading */
                ctx->hdr_len = (hdr_end + 4) - buf;
                ctx->state   = 1;

                /* parse Content-Length now */
                ctx->content_len =
                    parse_content_length(buf, ctx->hdr_len);
                if (ctx->content_len <= 0
                 || ctx->content_len > (ssize_t) (NGX_RTSP_MAX_ANNOUNCE - ctx->hdr_len))
                {
                    ngx_rtsp_send_simple_response(c, cseq,
                                                  411, "Length Required", NULL);
                    return;
                }
                break;
            }
            /* else continue loop to read more headers */
        } else {
            /* already found headers, break to read body */
            break;
        }
    }

    /* 2) Read the rest of the SDP body if needed */
    got = ctx->buflen - ctx->hdr_len;
    while ((ssize_t) got < ctx->content_len) {
        n = c->recv(c, buf + ctx->hdr_len + got,
                    ctx->content_len - got);
        if (n == NGX_AGAIN) {
            if (ngx_handle_read_event(c->read, 0) != NGX_OK) {
                ngx_close_connection(c);
            }
            return;
        }
        if (n <= 0) {
            ngx_rtsp_send_simple_response(c, cseq,
                                          400, "Bad Request", NULL);
            return;
        }
        got += n;
        ctx->buflen = ctx->hdr_len + got;
        buf[ctx->buflen] = '\0';
    }

    /* 3) Now buf[0..hdr_len+content_len] holds full ANNOUNCE */
    /* 3a) parse Request‐Line from the buffered headers */
    {
        size_t L = ctx->hdr_len;
        if (L >= sizeof(line1)) {
            ngx_rtsp_send_simple_response(c, cseq,
                                          400, "Bad Request", NULL);
            return;
        }
        ngx_memcpy(line1, buf, L);
        line1[L] = '\0';
        method = strtok(line1, " ");
        uri    = strtok(NULL, " ");
        if (!method || !uri || strcmp(method, "ANNOUNCE") != 0) {
            ngx_rtsp_send_simple_response(c, cseq,
                                          400, "Bad Request", NULL);
            return;
        }
    }

    /* 3b) derive mount from URI */
    if ((p = strstr(uri, "://")) && (p = strchr(p + 3, '/'))) {
        uri = p;
    }
    if (uri[0] != '/') {
        ngx_rtsp_send_simple_response(c, cseq,
                                      400, "Bad Request", NULL);
        return;
    }
    mount.data = (u_char*)(uri + 1);
    mount.len  = ngx_strlen(uri + 1);

    /* 4) stash SDP pointer */
    sdp = buf + ctx->hdr_len;
    sdp[ctx->content_len] = '\0';
    rs->remote_sdp = sdp;

    /* 5) bind to or create the shared stream */
    rs->stream = get_stream(c->pool, &mount);
    if (rs->stream == NULL) {
        ngx_rtsp_send_simple_response(c, cseq,
                                      500, "Internal Error", NULL);
        return;
    }

    /* 6) decode SPS/PPS into stream’s NALU pool */
    pool = &rs->stream->pool;
    {
        char *fmtp = strstr((char*)sdp, "sprop-parameter-sets=");
        if (fmtp) {
            fmtp += sizeof("sprop-parameter-sets=") - 1;
            char *comma = strchr(fmtp, ',');
            if (comma) {
                *comma = '\0';
                /* SPS */
                size_t b64l = strlen(fmtp);
                pool->sps_len = ngx_base64_decoded_length(b64l);
                pool->sps     = ngx_palloc(c->pool, pool->sps_len);
                ngx_decode_base64(&(ngx_str_t){b64l,(u_char*)fmtp},
                                  &(ngx_str_t){pool->sps_len,pool->sps});
                /* PPS */
                char *pps = comma + 1;
                size_t b64l2 = strlen(pps);
                pool->pps_len = ngx_base64_decoded_length(b64l2);
                pool->pps     = ngx_palloc(c->pool, pool->pps_len);
                ngx_decode_base64(&(ngx_str_t){b64l2,(u_char*)pps},
                                  &(ngx_str_t){pool->pps_len,pool->pps});
            }
        }
    }

    /* 7) send 200 OK */
    {
        char extra[64];
        ngx_snprintf((u_char*)extra, sizeof(extra),
                     "Session: %d\r\nContent-Length: 0\r\n",
                     rs->session_id);
        ngx_rtsp_send_simple_response(c, cseq, 200, "OK", extra);
    }

    /* 8) reset state for the next ANNOUNCE */
    ctx->buflen      = 0;
    ctx->hdr_len     = 0;
    ctx->content_len = -1;
    ctx->state       = 0;

    /* 9) re‐arm RTSP control parser */
    c->read->handler = ngx_rtsp_recv;
    ngx_handle_read_event(c->read, 0);
}

static void
ngx_rtsp_handle_record(ngx_connection_t *c, int cseq, ngx_rtsp_session_t *rs)
{
    // Reply OK, echo Session header
    char extra[64];
    ngx_snprintf((u_char*)extra, sizeof(extra),
                 "Session: %d\r\n", rs->session_id);
    ngx_rtsp_send_simple_response(c, cseq, 200, "OK", extra);

    // Mark that we’re now in “recording” mode
    rs->recording = 1;

    // Switch the control‐plane read handler into an RTP‐over‐TCP parser
    c->read->handler = ngx_rtsp_recv_rtp_interleaved;
    if (ngx_handle_read_event(c->read, 0) != NGX_OK) {
        ngx_close_connection(c);
    }
}

// added function

/* Reads raw RTSP request, logs it, and sends back a placeholder */
static void
ngx_rtsp_recv(ngx_event_t *rev)
{
    ngx_connection_t   *c  = rev->data;
    ngx_rtsp_session_t *rs = c->data;
    u_char             *buf;
    size_t              total = 0;
    ssize_t             n;
    int                 cseq = 0;
    char               *method, *uri, *line;

    if (rev->timedout) {
        ngx_close_connection(c);
        return;
    }

    buf = ngx_palloc(c->pool, 4096);
    if (!buf) {
        ngx_close_connection(c);
        return;
    }

    // read first chunk of RTSP request
    n = c->recv(c, buf, 4095);
    if (n <= 0) {
        ngx_close_connection(c);
        return;
    }
    total = n;
    buf[total] = '\0';

    // extract CSeq
    {
        char *h = strstr((char*)buf, "CSeq:");
        if (h) cseq = atoi(h + 5);
    }

    // parse Request-Line
    line   = (char*) buf;
    method = strsep(&line, " ");
    uri    = strsep(&line, " ");
    strsep(&line, "\r\n");

    ngx_log_error(NGX_LOG_ERR, c->log, 0,
        "RTSP: method='%s' uri='%s' CSeq=%d",
        method, uri, cseq);

    if (strcmp(method, "OPTIONS") == 0) {
        ngx_rtsp_handle_options(c, cseq);
    }
    else if (strcmp(method, "DESCRIBE") == 0) {
        ngx_rtsp_handle_describe(c, cseq, uri);
    }
    else if (strcmp(method, "SETUP") == 0) {
        ngx_rtsp_handle_setup(c, cseq, (const char*) buf, rs);
    }
    else if (strcmp(method, "PLAY") == 0) {
        ngx_rtsp_handle_play(c, cseq, (const char*) buf, rs);
    }
    else if (strcmp(method, "ANNOUNCE") == 0) {
        ngx_rtsp_handle_announce(c, cseq);
        return;
    }
    else if (strcmp(method, "RECORD") == 0) {
        ngx_rtsp_handle_record(c, cseq, rs);
        return;
    }
    else if (strcmp(method, "TEARDOWN") == 0) {
        ngx_rtsp_send_simple_response(c, cseq, 200, "OK", NULL);
        ngx_close_connection(c);
        return;
    }
    else {
        ngx_rtsp_send_simple_response(c, cseq, 501, "Not Implemented", NULL);
    }

    /* re-arm for next request */
    if (ngx_handle_read_event(c->read, 0) != NGX_OK) {
        ngx_close_connection(c);
    }
}

static void
ngx_rtsp_recv_rtp_interleaved(ngx_event_t *rev)
{
    ngx_connection_t   *c = rev->data;
    ngx_rtsp_session_t *rs = c->data;
    u_char              ihdr[4];
    ssize_t             n;
    ngx_rtsp_nalu_pool_t *pool = &rs->stream->pool;

    // 1) pull in the 4-byte interleaved header
    n = c->recv(c, ihdr, 4);
    if (n != 4 || ihdr[0] != '$') {
        // not an RTP packet? fall back to control parsing
        ngx_rtsp_recv(rev);
        return;
    }
    int channel = ihdr[1];
    int plen    = (ihdr[2] << 8) | ihdr[3];

    // 2) read the full RTP packet
    u_char *pkt = ngx_palloc(c->pool, plen);
    n = c->recv(c, pkt, plen);
    if (n != plen) {
        ngx_close_connection(c);
        return;
    }

    ngx_log_error(NGX_LOG_DEBUG, c->log, 0,
                  "RTP packet: channel=%d len=%d", channel, plen);

    // 3) strip RTP header (12 bytes)
    u_char *rtp_payload = pkt + 12;
    size_t payload_len   = plen - 12;
    u_char nal_type      = rtp_payload[0] & 0x1F;

    // 4) handle single-NAL vs FU-A fragmentation
    if (nal_type == 28 /* FU-A */) {
        u_char fu_hdr = rtp_payload[1];
        u_char start  = fu_hdr & 0x80;
        u_char end    = fu_hdr & 0x40;
        u_char orig   = fu_hdr & 0x1F;

        if (start) {
            // begin a new fragment buffer
            rs->fraglen    = 1;
            rs->fragbuf    = ngx_palloc(c->pool, payload_len + 1);
            rs->fragbuf[0] = (rtp_payload[0] & 0xE0) | orig;
            ngx_memcpy(rs->fragbuf + 1, rtp_payload + 2, payload_len - 2);
            rs->fraglen += (payload_len - 2);
        }
        else {
            // append to existing fragment
            u_char *newb = ngx_palloc(c->pool, rs->fraglen + payload_len - 2);
            ngx_memcpy(newb, rs->fragbuf, rs->fraglen);
            ngx_memcpy(newb + rs->fraglen, rtp_payload + 2, payload_len - 2);
            rs->fragbuf = newb;
            rs->fraglen += (payload_len - 2);
        }

        if (end) {
            // complete fragment ready
            // **NO-OP**: do not send back to the RECORD socket
            // ngx_rtsp_send_h264_nal(c, rs, rs->fragbuf, rs->fraglen, orig, 1);
            rs->fraglen = 0;
            /* allocate a NALU entry */
            ngx_rtsp_nalu_t *n = ngx_palloc(c->pool, sizeof(*n) + payload_len);
            n->ts     = rs->rtp_ts;
            n->len    = rs->fraglen;
            n->data   = (u_char*)(n + 1);
            ngx_memcpy(n->data, rs->fragbuf, rs->fraglen);
            ngx_queue_insert_tail(&pool->queue, &n->queue);
            rs->fraglen = 0;
        }
    }
    else {
        // single NAL—instead of echoing it back, just drop it
        // ngx_rtsp_send_h264_nal(c, rs, rtp_payload, payload_len, nal_type, /*marker=*/1);
        ngx_rtsp_nalu_t *n = ngx_palloc(c->pool, sizeof(*n) + payload_len);
        n->ts   = rs->rtp_ts;
        n->len  = payload_len;
        n->data = (u_char*)(n + 1);
        ngx_memcpy(n->data, rtp_payload, payload_len);
        ngx_queue_insert_tail(&pool->queue, &n->queue);
    }

    // 5) re-arm for next packet
    if (ngx_handle_read_event(c->read, 0) != NGX_OK) {
        ngx_close_connection(c);
    }
}