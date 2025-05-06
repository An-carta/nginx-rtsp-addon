#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>
#include <ngx_string.h>
#include <unistd.h>
#include <fcntl.h>
#include <ngx_rtsp_module.h>

/*── forward decls ─────────────────────────────────────────────────────────────*/
static void  send_reply             (ngx_connection_t *c, int cseq,
                                     int code, const char *status,
                                     const char *hdrs);
static void  handle_options         (ngx_connection_t *c, int cseq);
static void  handle_announce        (ngx_connection_t *c,
                                     ngx_rtsp_session_t *rs,
                                     int cseq, char *uri);
static void  handle_setup           (ngx_connection_t *c,
                                     ngx_rtsp_session_t *rs,
                                     int cseq, char *uri);
static void  handle_record          (ngx_connection_t *c,
                                     ngx_rtsp_session_t *rs,
                                     int cseq);
static void  ngx_rtsp_recv          (ngx_event_t *ev);
static void  ngx_rtsp_recv_rtp_interleaved(ngx_event_t *ev);
static void handle_describe(ngx_connection_t *c, ngx_rtsp_session_t *rs, int cseq, char *uri);
static void handle_play(ngx_connection_t *c, ngx_rtsp_session_t *rs, int cseq);

/*── connection initialization ─────────────────────────────────────────────────*/
void
ngx_rtsp_init_connection(ngx_connection_t *c)
{
    ngx_rtsp_session_t  *rs;

    ngx_log_error(NGX_LOG_WARN, c->log, 0,
                  ">>> RTSP INIT CONNECTION, fd=%d", c->fd);

    rs = ngx_pcalloc(c->pool, sizeof(*rs));
    if (rs == NULL) {
        ngx_close_connection(c);
        return;
    }

    rs->session_id = (int)(ngx_time()) & 0xffff;
    rs->fragbuf    = NULL;
    rs->fraglen    = 0;
    rs->stream     = NULL;

    c->data       = rs;
    c->read->handler = ngx_rtsp_recv;
    ngx_handle_read_event(c->read, 0);
}

/* find-or-create a mountpoint by name */
static ngx_rtsp_stream_t *
ngx_rtsp_get_stream(ngx_pool_t *pool, ngx_str_t *name)
{
    ngx_queue_t *q;
    ngx_rtsp_stream_t *s;

    for (q = ngx_queue_head(&streams_head);
         q != ngx_queue_sentinel(&streams_head);
         q = ngx_queue_next(q))
    {
        s = ngx_queue_data(q, ngx_rtsp_stream_t, link);
        if (s->name.len == name->len &&
            ngx_strncmp(s->name.data, name->data, name->len) == 0)
        {
            return s;
        }
    }

    /* not found → create */
    s = ngx_pcalloc(pool, sizeof(*s));
    s->name.len  = name->len;
    s->name.data = ngx_pnalloc(pool, name->len);
    ngx_memcpy(s->name.data, name->data, name->len);
    ngx_queue_init(&s->pool.queue);
    s->pool.sps = s->pool.pps = NULL;
    s->pool.sps_len = s->pool.pps_len = 0;

    ngx_queue_insert_tail(&streams_head, &s->link);
    return s;
}

/*── dispatcher ────────────────────────────────────────────────────────────────*/
static void
ngx_rtsp_recv(ngx_event_t *ev)
{
    ngx_connection_t   *c   = ev->data;
    ngx_rtsp_session_t *rs  = c->data;
    u_char              buf[4096];
    ssize_t             n;
    int                 cseq = 0;
    char               *method, *uri, *line;
    u_char             *h;

    if (ev->timedout) {
        ngx_close_connection(c);
        return;
    }

    n = c->recv(c, buf, sizeof(buf) - 1);
    if (n <= 0) {
        ngx_close_connection(c);
        return;
    }
    buf[n] = '\0';

    /* extract CSeq */
    if ((h = (u_char*) strstr((char*)buf, "CSeq:"))) {
        cseq = atoi((char*)h + 5);
    }

    /* split first line */
    line   = (char*) buf;
    method = strsep(&line, " ");
    uri    = strsep(&line, " \r\n");

    ngx_log_error(NGX_LOG_INFO, c->log, 0,
                  "RTSP ← method='%s' uri='%s' CSeq=%d",
                  method, uri, cseq);

    if (strcmp(method, "OPTIONS") == 0) {
        handle_options(c, cseq);
    }
    else if (strcmp(method, "ANNOUNCE") == 0) {
        handle_announce(c, rs, cseq, uri);
    }
    else if (strcmp(method, "SETUP") == 0) {
        handle_setup(c, rs, cseq, uri);
    }
    else if (strcmp(method, "RECORD") == 0) {
        handle_record(c, rs, cseq);
        return;  /* RECORD switches into RTP mode */
    }
    else if (strcmp(method, "DESCRIBE") == 0) {
                handle_describe(c, rs, cseq, uri);
            }
    else if (strcmp(method, "PLAY") == 0) {
                handle_play(c, rs, cseq);
                return;  /* PLAY also switches to RTP mode */
            }
    else {
        ngx_log_error(NGX_LOG_INFO, c->log, 0, "RTSP → Not Implemented");
        send_reply(c, cseq, 501, "Not Implemented", NULL);
    }

    /* re-arm control parser */
    if (ngx_handle_read_event(c->read, 0) != NGX_OK) {
        ngx_close_connection(c);
    }
}

/*── method handlers ──────────────────────────────────────────────────────────*/
/* DESCRIBE: build and send SDP */
static void
handle_describe(ngx_connection_t *c, ngx_rtsp_session_t *rs,
                int cseq, char *uri)
{
    ngx_log_error(NGX_LOG_INFO, c->log, 0,
                  "RTSP → DESCRIBE, uri='%s'", uri);

    /* TODO: parse mountpoint from URI and locate stream */
    /* TODO: build SDP payload based on stream parameters */
    const char *sdp =
        "v=0\r\n"
        "o=- 0 0 IN IP4 0.0.0.0\r\n"
        "s=RTSP Session\r\n"
        "t=0 0\r\n"
        "m=video 0 RTP/AVP 96\r\n"
        "a=rtpmap:96 H264/90000\r\n";

    /* send with Content-Type: application/sdp */
    u_char hdrs[256];
    ngx_snprintf(hdrs, sizeof(hdrs),
                 "Content-Type: application/sdp\r\n"
                 "Content-Length: %uz\r\n",
                 ngx_strlen(sdp));

    send_reply(c, cseq, 200, "OK", (char*) hdrs);
    /* write the SDP body itself */
    c->send(c, (u_char*) sdp, ngx_strlen(sdp));
}

/* PLAY: confirm and switch to RTP interleaved */
static void
handle_play(ngx_connection_t *c, ngx_rtsp_session_t *rs, int cseq)
{
    ngx_log_error(NGX_LOG_INFO, c->log, 0, "RTSP → PLAY");

    u_char extra[64];
    ngx_snprintf(extra, sizeof(extra),
                 "Session: %d\r\n"
                 "Content-Length: 0\r\n",
                 rs->session_id);
    send_reply(c, cseq, 200, "OK", (char*) extra);

    /* now consume interleaved RTP frames */
    c->read->handler = ngx_rtsp_recv_rtp_interleaved;
    ngx_handle_read_event(c->read, 0);
}

static void
handle_options(ngx_connection_t *c, int cseq)
{
    ngx_log_error(NGX_LOG_INFO, c->log, 0, "RTSP → replying OPTIONS");
    send_reply(c, cseq, 200, "OK",
               "Public: OPTIONS, ANNOUNCE, SETUP, RECORD\r\n");
}

static void
handle_announce(ngx_connection_t *c,
                ngx_rtsp_session_t *rs,
                int cseq, char *uri)
{
    ngx_str_t mount;
    char       *start, *end;

    ngx_log_error(NGX_LOG_INFO, c->log, 0, "RTSP → ANNOUNCE, uri='%s'", uri);

    start = strstr(uri, "://");
    if (start) {
        /* skip “://” */
        start += 3;
        /* now find the slash that begins the path */
        start = strchr(start, '/');
    }
    if (! start) {
        /* no slash found → empty mount */
        mount.data = (u_char *) "";
        mount.len  = 0;
    }
    else {
        /* advance past the slash */
        start++;

        /* 2) Find the end of the path (space or end‐of‐string) */
        end = strchr(start, ' ');
        if (! end) {
            end = start + strlen(start);
        }

        /* 3) Fill in the ngx_str_t */
        mount.data = (u_char *) start;
        mount.len  = end - start;
    }

    ngx_log_error(NGX_LOG_INFO, c->log, 0,
                  "RTSP: mount='%.*s' → creating stream",
                  (int) mount.len, mount.data);

    /* now look up or create your stream */
    rs->stream = ngx_rtsp_get_stream(c->pool, &mount);

    /* reply with our new session ID */
    {
        u_char extra[64];
        ngx_snprintf(extra, sizeof(extra),
                     "Session: %d\r\n"
                     "Content-Length: 0\r\n",
                     rs->session_id);
        send_reply(c, cseq, 200, "OK", (char*) extra);

        // **NEW**: drain the ANNOUNCE body so we don't confuse our parser on the next read
        //
        {
            // Find the Content-Length header in the client’s request buffer
            // (We could parse it again, or store it globally; for now assume no more than 10k)
            u_char  hdrbuf[128];
            ssize_t n;
            ngx_int_t content_len = 0;
    
            // Read up to (say) 1024 bytes to capture the headers+body start
            n = c->recv(c, hdrbuf, sizeof(hdrbuf));
            if (n > 0) {
                hdrbuf[n] = '\0';
                // Look for “Content-Length:”
                u_char *p = (u_char*) ngx_strstr(hdrbuf, "Content-Length:");
                if (p) {
                    content_len = atoi((char*) p + sizeof("Content-Length:") - 1);
                }
                // Now skip past the double-CRLF
                p = (u_char*) ngx_strstr(hdrbuf, "\r\n\r\n");
                if (p) {
                    p += 4;
                    n  -= (p - hdrbuf);
                    // If there’s still body left, subtract that from content_len
                    if (n > 0) {
                        content_len -= n;
                    }
                }
            }
            // If there’s any SDP body left, drain it
            while (content_len > 0) {
                u_char drain[4096];
                ssize_t got = c->recv(c, drain, ngx_min(content_len, (ngx_int_t) sizeof(drain)));
                if (got <= 0) {
                    break;  /* connection closed or error */
                }
                content_len -= got;
            }
        }
    
        // Re-arm control parser once the ANNOUNCE body is gone
         if (ngx_handle_read_event(c->read, 0) != NGX_OK) {
             ngx_close_connection(c);
        }
    }
}

static void
handle_setup(ngx_connection_t *c,
             ngx_rtsp_session_t *rs,
             int cseq, char *uri)
{
    ngx_log_error(NGX_LOG_INFO, c->log, 0, "RTSP → replying SETUP");
    send_reply(c, cseq, 200, "OK",
               "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n"
               "Session: 12345\r\n"
               "Content-Length: 0\r\n");
}

static void
handle_record(ngx_connection_t *c,
              ngx_rtsp_session_t *rs,
              int cseq)
{
    ngx_log_error(NGX_LOG_INFO, c->log, 0, "RTSP → replying RECORD");
    send_reply(c, cseq, 200, "OK",
               "Session: 12345\r\n"
               "Content-Length: 0\r\n");

    /* now consume interleaved RTP frames */
    c->read->handler = ngx_rtsp_recv_rtp_interleaved;
    ngx_handle_read_event(c->read, 0);
}

static void
ngx_rtsp_recv_rtp_interleaved(ngx_event_t *rev)
{
    ngx_connection_t    *c   = rev->data;
    ngx_rtsp_session_t  *rs  = c->data;
    ngx_rtsp_nalu_pool_t *pool = &rs->stream->pool;
    u_char                ihdr[4];
    ssize_t               n;

    // 1) read the 4-byte interleaved header
    n = c->recv(c, ihdr, 4);
    if (n != 4 || ihdr[0] != '$') {
                ngx_log_error(NGX_LOG_ERR, c->log, 0,
                              "RTP→TCP: invalid interleaved header, closing connection");
                ngx_close_connection(c);
                return;
            }

    int channel = ihdr[1];
    int plen    = (ihdr[2] << 8) | ihdr[3];

    ngx_log_error(NGX_LOG_DEBUG, c->log, 0,
                  "RTP→TCP: channel=%d length=%d", channel, plen);

    // 2) read the full RTP packet
    u_char *pkt = ngx_palloc(c->pool, plen);
    if ((n = c->recv(c, pkt, plen)) != plen) {
        ngx_log_error(NGX_LOG_ERR, c->log, 0,
                      "RTP→TCP: packet read error (%zd/%d)", n, plen);
        ngx_close_connection(c);
        return;
    }

    // 3) parse RTP header
    //    V=2 P X CC=0   PT & marker
    uint16_t seq = (pkt[2] << 8) | pkt[3];
    uint32_t ts  = (pkt[4] << 24)
                 | (pkt[5] << 16)
                 | (pkt[6] <<  8)
                 |  pkt[7];
    ngx_log_error(NGX_LOG_INFO, c->log, 0,
                  "RTP→TCP: seq=%u ts=%u", seq, ts);

    // 4) strip the 12-byte RTP header
    u_char *rtp_payload = pkt + 12;
    size_t payload_len   = plen - 12;
    u_char nal0         = rtp_payload[0];
    u_char nal_type     = nal0 & 0x1F;

    // 5) H.264 FU-A handling
    if (nal_type == 28) {
        u_char fu = rtp_payload[1];
        u_char start = fu & 0x80, end = fu & 0x40, orig = fu & 0x1F;

        if (start) {
            // allocate new reassembly buffer
            rs->fraglen = 1;
            rs->fragbuf = ngx_palloc(c->pool, payload_len);
            // rebuild original NAL header (F/NRI/orig)
            rs->fragbuf[0] = (nal0 & 0xE0) | orig;
            ngx_memcpy(rs->fragbuf + 1,
                       rtp_payload + 2,
                       payload_len - 2);
            rs->fraglen += payload_len - 2;
        }
        else {
            // append to existing
            u_char *newb = ngx_palloc(c->pool, rs->fraglen + payload_len - 2);
            ngx_memcpy(newb, rs->fragbuf, rs->fraglen);
            ngx_memcpy(newb + rs->fraglen,
                       rtp_payload + 2,
                       payload_len - 2);
            rs->fragbuf = newb;
            rs->fraglen += payload_len - 2;
        }

        if (end) {
            // complete NAL ready
            ngx_log_error(NGX_LOG_INFO, c->log, 0,
                          "RTP→TCP: FU-A complete, nal_type=%u size=%uz",
                          orig, rs->fraglen);

            // queue it
            ngx_rtsp_nalu_t *nalu = ngx_palloc(c->pool,
                                       sizeof(*nalu) + rs->fraglen);
            nalu->ts   = ts;
            nalu->len  = rs->fraglen;
            ngx_memcpy(nalu->data, rs->fragbuf, rs->fraglen);
            ngx_queue_insert_tail(&pool->queue, &nalu->queue);

            rs->fraglen = 0;
        }
    }
    else {
        // single NAL
        ngx_log_error(NGX_LOG_INFO, c->log, 0,
                      "RTP→TCP: single NAL, type=%u size=%uz",
                      nal_type, payload_len);

        ngx_rtsp_nalu_t *nalu = ngx_palloc(c->pool,
                                   sizeof(*nalu) + payload_len);
        nalu->ts   = ts;
        nalu->len  = payload_len;
        ngx_memcpy(nalu->data, rtp_payload, payload_len);
        ngx_queue_insert_tail(&pool->queue, &nalu->queue);
    }

    // 6) re-arm for next interleaved RTP chunk
    c->read->handler = ngx_rtsp_recv_rtp_interleaved;
    ngx_handle_read_event(c->read, 0);
}


/*------------------------------------------------------------------------------
 * send_reply()
 *   Build a minimal RTSP/1.0 response, toggle the socket blocking,
 *   write it out, restore nonblocking, and close-on-error.
 *----------------------------------------------------------------------------*/
/*── send_reply() ─────────────────────────────────────────────────────────────*/
static void
send_reply(ngx_connection_t *c, int cseq, int code,
           const char *status, const char *hdrs)
{
    u_char buf[1024], *p = buf;
    size_t total, left;
    ssize_t sent;
    int fd, flags;

    p = ngx_snprintf(p,
                     sizeof(buf) - (p - buf),
                     "RTSP/1.0 %d %s\r\n"
                     "CSeq: %d\r\n"
                     "%s"      /* ends in “\r\n” or empty */
                     "\r\n",
                     code, status, cseq, hdrs ? hdrs : "");

    total = p - buf;

    fd    = c->fd;
    flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

    left = total;
    p    = buf;
    while (left) {
        sent = write(fd, p, left);
        if (sent <= 0) {
            ngx_log_error(NGX_LOG_ERR, c->log, errno,
                          "RTSP send_reply() failed");
            ngx_close_connection(c);
            return;
        }
        left -= sent;
        p    += sent;
    }

    fcntl(fd, F_SETFL, flags);
}