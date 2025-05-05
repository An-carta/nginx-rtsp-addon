// ngx_rtsp_handler.c

#include <ngx_config.h>
#include <ngx_core.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "ngx_rtsp_module.h"
#include <ngx_event.h>        // For ngx_add_timer()
#include <ngx_event_posted.h> // For event handling primitives
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#define NGX_RTSP_MAX_ANNOUNCE  (64*1024)

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
    ssize_t   content_length;
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
    int                      channel_rtp;
    int                      channel_rtcp;
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
// static void ngx_rtsp_handle_describe(ngx_connection_t *c, int cseq, const char *uri);
static void ngx_rtsp_send_simple_response(ngx_connection_t *c,
    int               cseq,
    int               code,
    const char       *reason,
    const char       *extra_headers);
static void ngx_rtsp_recv_rtp_interleaved(ngx_event_t *rev);
static ssize_t parse_content_length(u_char *data, size_t len);

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
    rs->announce.content_length = -1;
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
ngx_rtsp_recv_once(ngx_connection_t *c, u_char *p, size_t n)
{
    ssize_t  nn = c->recv(c, p, n);
    if (nn == NGX_AGAIN) {
        return 0;
    }
    return nn;
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
/*
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
*/

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

static void
ngx_rtsp_send_simple_response(ngx_connection_t *c,
                              int               cseq,
                              int               code,
                              const char       *status,
                              const char       *extra)
{
    u_char   buf[1024];
    u_char  *p;
    size_t   total, left;
    ssize_t  sent;
    int      fd, flags;

    // 1. Build the RTSP response into buf[]
    p = ngx_snprintf(buf, sizeof(buf),
        "RTSP/1.0 %d %s\r\n"
        "CSeq: %d\r\n"
        "%s"       // extra already ends in "\r\n"
        "\r\n",
        code, status,
        cseq,
        extra ? extra : "");

    total = p - buf;

    // 2. Grab the raw socket and make it blocking
    fd    = c->fd;
    flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

    // 3. Loop until we've written the entire response
    left = total;
    p    = buf;
    while (left > 0) {
        sent = write(fd, p, left);
        if (sent < 0) {
            // unrecoverable write error
            ngx_log_error(NGX_LOG_ERR, c->log, errno,
                          "RTSP send failed, closing connection");
            break;
        }
        left -= sent;
        p    += sent;
    }

    // 4. Restore nonblocking mode
    fcntl(fd, F_SETFL, flags);

    // 5. If we failed partway, close the connection
    if (left != 0) {
        ngx_close_connection(c);
    }
}
/*
static void
ngx_rtsp_egress(ngx_event_t *ev)
{
    ngx_connection_t   *c   = ev->data;
    ngx_rtsp_session_t *rs  = c->data;
    ngx_rtsp_nalu_pool_t *pool = &rs->stream->pool;
    ngx_queue_t        *q;
    ngx_rtsp_nalu_t    *n;

    // send SPS/PPS once 
    if (pool->sps) {
                ngx_rtsp_send_h264_nal(c, rs, pool->sps, pool->sps_len, 7, 0);
                ngx_rtsp_send_h264_nal(c, rs, pool->pps, pool->pps_len, 8, 1);
                pool->sps = pool->pps = NULL;
             }
    

    // pop & send as many as fit in 40ms tick 
    if (!ngx_queue_empty(&pool->queue)) {
                q = ngx_queue_head(&pool->queue);
        n = ngx_queue_data(q, ngx_rtsp_nalu_t, queue);
        ngx_queue_remove(q);

        // marker=1 on last NAL of each frame? simplify to always 1 
        ngx_rtsp_send_h264_nal(c, rs, n->data, n->len, n->data[0]&0x1F, 1);

        // free it 
        ngx_pfree(c->pool, n);
    }

    // reschedule if more pending 
    if (!ngx_queue_empty(&pool->queue)) {
                ngx_add_timer(&rs->egress_ev, 40);
            }
}
*/
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
/*
// DESCRIBE: return a tiny SDP payload
static void
ngx_rtsp_handle_describe(ngx_connection_t *c, int cseq, const char *uri)
{
    u_char  sdp_buf[1024];
    u_char  extra[256];
    u_char *p;
    size_t  n;

    // Get client IP dynamically 
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

    // Prepare headers 
    ngx_snprintf(extra, sizeof(extra),
        "Content-Type: application/sdp\r\n"
        "Content-Length: %uz\r\n",
        n);

    ngx_rtsp_send_simple_response(c, cseq, 200, "OK", (char*)extra);
    c->send(c, sdp_buf, n);
}
*/
static void
ngx_rtsp_handle_setup(ngx_connection_t *c, int cseq,
                      const char       *raw, ngx_rtsp_session_t *rs)
{
    // pick two unused channel numbers (e.g. 0 and 1)
    rs->channel_rtp  = 0;
    rs->channel_rtcp = 1;

    u_char hdr[128];
    ngx_snprintf(hdr, sizeof(hdr),
        "Transport: RTP/AVP/TCP;unicast;interleaved=%d-%d\r\n"
        "Session: %d\r\n"
        "Content-Length: 0\r\n",
        rs->channel_rtp, rs->channel_rtcp,
        rs->session_id);

    ngx_rtsp_send_simple_response(c, cseq, 200, "OK", (char*)hdr);

    // re-arm control parser
    c->read->handler = ngx_rtsp_recv;
    ngx_handle_read_event(c->read, 0);
}

/*
static void
ngx_rtsp_handle_play(ngx_connection_t *c,
                     int               cseq,
                     const char       *raw,
                     ngx_rtsp_session_t *rs)
{
    // send PLAY 200 OK 
    char extra[64];
    ngx_snprintf((u_char*)extra, sizeof(extra),
                 "Session: %d\r\n", rs->session_id);
    ngx_rtsp_send_simple_response(c, cseq, 200, "OK", extra);

    // schedule PLAY‐side egress timer 
    rs->egress_ev.handler    = ngx_rtsp_egress;
    rs->egress_ev.data       = c;
    rs->egress_ev.log        = c->log;
    rs->egress_ev.cancelable = 1;
    ngx_add_timer(&rs->egress_ev, 40);
}
*/
static void
ngx_rtsp_handle_announce(ngx_connection_t *c, int cseq,
                         u_char *raw, size_t raw_len)
{
    ngx_rtsp_session_t *rs = c->data;
    char header[8192];
    size_t header_len = 0, got = 0;
    ssize_t n, clen;
    //u_char *hdr_end = NULL;
    //u_char *body = NULL;
    u_char *sdp;
    ngx_str_t mount;
    char *method, *uri, *p, *line;

    // 0) Seed the header buffer with the initial read
    if (raw_len > 0 && raw_len < sizeof(header)) {
        ngx_memcpy(header, raw, raw_len);
        header_len = raw_len;
        header[header_len] = '\0';
    }

    ngx_log_error(NGX_LOG_DEBUG, c->log, 0,
                  "ANNOUNCE(%d): seeded header_len=%uz", cseq, header_len);

    // 1) Read until we see "\r\n\r\n"
    while (header_len < sizeof(header) - 1) {
        if (header_len >= 4 &&
            memcmp(header + header_len - 4, "\r\n\r\n", 4) == 0)
        {
            break;
        }
        n = c->recv(c, (u_char*)header + header_len, 1);
        if (n <= 0) {
            ngx_log_error(NGX_LOG_ERR, c->log, 0,
                          "ANNOUNCE(%d): recv header failed", cseq);
            ngx_rtsp_send_simple_response(c, cseq, 400, "Bad Request", NULL);
            return;
        }
        header_len += n;
        header[header_len] = '\0';
    }

    if (header_len < 4 ||
        memcmp(header + header_len - 4, "\r\n\r\n", 4) != 0)
    {
        ngx_log_error(NGX_LOG_ERR, c->log, 0,
                      "ANNOUNCE(%d): missing header terminator", cseq);
        ngx_rtsp_send_simple_response(c, cseq, 400, "Bad Request", NULL);
        return;
    }

    // 2) Split headers/body
    //hdr_end = (u_char*)header + header_len - 4;
    //body = hdr_end + 4;
    got = (raw_len > header_len) ? (raw_len - header_len) : 0;
    ngx_log_error(NGX_LOG_DEBUG, c->log, 0,
                  "ANNOUNCE(%d): header_len=%uz got=%uz", cseq, header_len, got);

    // 3) Parse Content-Length from header[]
    clen = parse_content_length((u_char*)header, header_len);
    ngx_log_error(NGX_LOG_DEBUG, c->log, 0,
                  "ANNOUNCE(%d): Content-Length=%zd", cseq, clen);
    if (clen <= 0) {
        ngx_log_error(NGX_LOG_ERR, c->log, 0,
                      "ANNOUNCE(%d): invalid Content-Length", cseq);
        ngx_rtsp_send_simple_response(c, cseq, 411, "Length Required", NULL);
        return;
    }

    // 4) Allocate buffer for full SDP
    sdp = ngx_palloc(c->pool, clen + 1);
    if (!sdp) {
        ngx_log_error(NGX_LOG_ERR, c->log, 0,
                      "ANNOUNCE(%d): failed to alloc SDP buffer", cseq);
        ngx_rtsp_send_simple_response(c, cseq, 500, "Internal Server Error", NULL);
        return;
    }
    ngx_log_error(NGX_LOG_DEBUG, c->log, 0,
                  "ANNOUNCE(%d): SDP buf=%p size=%zd", cseq, sdp, clen + 1);

    // 5) Copy already-read SDP chunk from header[]
    if (got > (size_t)clen) got = clen;
    if (got > 0)
        ngx_memcpy(sdp, header + header_len, got);
    ngx_log_error(NGX_LOG_DEBUG, c->log, 0,
                  "ANNOUNCE(%d): copied %uz bytes SDP", cseq, got);

    // 6) Blocking-read the rest of the SDP
    if (got < (size_t)clen) {
        ngx_log_error(NGX_LOG_DEBUG, c->log, 0,
                      "ANNOUNCE(%d): reading remaining %zd bytes", cseq, clen - got);
        if (read_exact(c, sdp + got, clen - got) != NGX_OK) {
            ngx_log_error(NGX_LOG_ERR, c->log, 0,
                          "ANNOUNCE(%d): SDP read_exact failed", cseq);
            ngx_rtsp_send_simple_response(c, cseq, 400, "Bad Request", NULL);
            return;
        }
    }
    sdp[clen] = '\0';
    rs->remote_sdp = sdp;
    ngx_log_error(NGX_LOG_DEBUG, c->log, 0,
                  "ANNOUNCE(%d): full SDP:\n%s", cseq, sdp);

    // 7) Extract method/URI from the first line
    line = ngx_palloc(c->pool, header_len + 1);
    ngx_memcpy(line, header, header_len);
    line[header_len] = '\0';
    method = strtok(line, " ");
    uri    = strtok(NULL, " ");
    ngx_log_error(NGX_LOG_DEBUG, c->log, 0,
                  "ANNOUNCE(%d): method='%s' uri='%s'", cseq, method, uri);
    if (!method || !uri || strcmp(method, "ANNOUNCE") != 0) {
        ngx_rtsp_send_simple_response(c, cseq, 400, "Bad Request", NULL);
        return;
    }

    // 8) Derive mountpoint
    if ((p = strstr(uri, "://")) && (p = strchr(p + 3, '/'))) {
        uri = p;
    }
    if (uri[0] != '/') {
        ngx_rtsp_send_simple_response(c, cseq, 400, "Bad Request", NULL);
        return;
    }
    mount.data = (u_char*)(uri + 1);
    mount.len  = ngx_strlen(uri + 1);
    ngx_log_error(NGX_LOG_DEBUG, c->log, 0,
                  "ANNOUNCE(%d): mount='%.*s'", cseq, (int)mount.len, mount.data);

    // 9) Bind or create shared stream
    rs->stream = get_stream(c->pool, &mount);
    if (!rs->stream) {
        ngx_rtsp_send_simple_response(c, cseq, 500, "Internal Server Error", NULL);
        return;
    }

    // 10) (optional) decode SPS/PPS...

    // 11) Send 200 OK
    ngx_rtsp_send_simple_response(c, cseq, 200, "OK", NULL);
    ngx_log_error(NGX_LOG_INFO, c->log, 0,
                  "ANNOUNCE(%d): replied 200 OK", cseq);

    // 12) Re-arm control parser
    c->read->handler = ngx_rtsp_recv;
    ngx_handle_read_event(c->read, 0);
}

static void
ngx_rtsp_handle_record(ngx_connection_t *c, int cseq,
                       ngx_rtsp_session_t *rs)
{
    u_char hdr[64];
    ngx_snprintf(hdr, sizeof(hdr),
                 "Session: %d\r\nContent-Length: 0\r\n",
                 rs->session_id);

    ngx_rtsp_send_simple_response(c, cseq, 200, "OK", (char*)hdr);

    // flip into RTP‐over‐TCP handler
    c->read->handler = ngx_rtsp_recv_rtp_interleaved;
    ngx_handle_read_event(c->read, 0);
}

// added function

/* Reads raw RTSP request, logs it, and sends back a placeholder */
static void
ngx_rtsp_recv(ngx_event_t *rev)
{
    ngx_connection_t    *c   = rev->data;
    ngx_rtsp_session_t  *rs  = c->data;
    u_char              *buf;
    size_t               total;
    ssize_t              n;
    int                  cseq;
    char                *method, *uri, *line, *p;

    if (rev->timedout) {
        ngx_close_connection(c);
        return;
    }

    buf = ngx_pcalloc(c->pool, 4096);
    if (!buf) {
        ngx_close_connection(c);
        return;
    }

    // 1) first socket read
    n = c->recv(c, buf, 4095);
    if (n <= 0) {
        ngx_close_connection(c);
        return;
    }
    total = (size_t) n;
    buf[total] = '\0';

    // 2) extract CSeq
    cseq = 0;
    if ((p = strstr((char*)buf, "CSeq:"))) {
        cseq = atoi(p + 5);
    }

    // 3) parse method and URI
    line   = (char*) buf;
    method = strsep(&line, " ");
    uri    = strsep(&line, " ");
    strsep(&line, "\r\n");

    ngx_log_error(NGX_LOG_INFO, c->log, 0,
                  "RTSP: method='%s' uri='%s' CSeq=%d",
                  method, uri, cseq);

    if (strcmp(method, "OPTIONS") == 0) {
        ngx_rtsp_handle_options(c, cseq);
    }
    else if (strcmp(method, "ANNOUNCE")==0) {
        ngx_rtsp_handle_announce(c, cseq, buf, total);
        return;
    }
    else if (strcmp(method, "SETUP") == 0) {
        ngx_rtsp_handle_setup(c, cseq, (const char*) buf, rs);
        return;
    }
    else if (strcmp(method, "RECORD") == 0) {
        ngx_rtsp_handle_record(c, cseq, rs);
        return;
    }
    else {
        ngx_rtsp_send_simple_response(c, cseq,
                                      501, "Not Implemented", NULL);
    }

    // re-arm for next RTSP control message
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