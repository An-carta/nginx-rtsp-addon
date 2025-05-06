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

static ssize_t
ngx_rtsp_send_interleaved(ngx_connection_t *c, int channel,
                          u_char *payload, size_t plen)
{
    u_char hdr[4];
    hdr[0] = '$';
    hdr[1] = (u_char) channel;
    hdr[2] = (u_char) (plen >> 8);
    hdr[3] = (u_char) (plen & 0xff);

    // write the 4-byte RTP-over-TCP header
    if (c->send(c, hdr, 4) != 4) {
        return -1;
    }
    // then the RTP payload itself
    return c->send(c, payload, plen);
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

static void
ngx_rtsp_send_rtp_interleaved(ngx_event_t *wev)
{
    ngx_connection_t    *c  = wev->data;
    ngx_rtsp_session_t  *rs = c->data;
    ngx_rtsp_nalu_pool_t *pool = &rs->stream->pool;
    ngx_rtsp_nalu_t     *nalu;
    ngx_queue_t         *q, *next;

    /* only in PLAY mode */
    if (! rs->playing) {
        return;
    }

    /* --- on first PLAY, send SPS/PPS as Annex-B NALUs --- */
    if (! rs->stream->sent_spspps) {
        /* 4-byte start code: 00 00 00 01 */
        u_char start4[4] = {0,0,0,1};
        /* SPS */
        ngx_rtsp_send_interleaved(c, 0, start4, 4);
        ngx_rtsp_send_interleaved(c, 0,
                                 rs->stream->sps.data,
                                 rs->stream->sps.len);
        /* PPS */
        ngx_rtsp_send_interleaved(c, 0, start4, 4);
        ngx_rtsp_send_interleaved(c, 0,
                                 rs->stream->pps.data,
                                 rs->stream->pps.len);

        rs->stream->sent_spspps = 1;
    }

    /* now drain your queued NALUs as before */
    for (q = ngx_queue_head(&pool->queue);
         q != ngx_queue_sentinel(&pool->queue);
         q = next)
    {
        next = ngx_queue_next(q);
        nalu = ngx_queue_data(q, ngx_rtsp_nalu_t, queue);

        if (ngx_rtsp_send_interleaved(c, 0,
                                      nalu->data, nalu->len) < 0)
        {
            ngx_log_error(NGX_LOG_ERR, c->log, 0,
                          "RTP→TCP: send interleaved failed");
            ngx_close_connection(c);
            return;
        }

        ngx_queue_remove(q);
    }

    /* re-arm… */
    if (ngx_handle_write_event(c->write, 0) != NGX_OK) {
        ngx_close_connection(c);
    }
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
    ngx_str_t  mount;
    char      *p, *start, *end;

    /* parse the “/live/test” path out of the URI */
    p = strstr(uri, "rtsp://");
    if (p) {
        p += strlen("rtsp://");
        p  = strchr(p, '/');
    }
    if (p) {
        start    = p + 1;
        end      = start;
        while (*end && *end != ' ' && *end != '\r' && *end != '\n') {
            end++;
        }
        mount.len  = end - start;
        mount.data = (u_char*) start;
    } else {
        mount.len  = 0;
        mount.data = (u_char*) "";
    }

    /* now that we know the mount, fetch the stream struct */
    rs->stream = ngx_rtsp_get_stream(c->pool, &mount);

    ngx_log_error(NGX_LOG_INFO, c->log, 0,
                  "RTSP → DESCRIBE, uri='%s'", uri);

    /* build the fmtp line from our stored sps_b64/pps_b64 */
    u_char fmtp_line[256];
    ngx_snprintf(fmtp_line, sizeof(fmtp_line),
                 "packetization-mode=1;"
                 " sprop-parameter-sets=%V,%V\r\n",
                 &rs->stream->sps_b64,
                 &rs->stream->pps_b64);

    /* assemble full SDP */
    u_char sdp[512];
    ngx_snprintf(sdp, sizeof(sdp),
                 "v=0\r\n"
                 "o=- 0 0 IN IP4 0.0.0.0\r\n"
                 "s=RTSP Session\r\n"
                 "t=0 0\r\n"
                 "m=video 0 RTP/AVP 96\r\n"
                 "a=control:*\r\n"
                 "a=rtpmap:96 H264/90000\r\n"
                 "a=fmtp:96 %s"
                 , fmtp_line);

    /* send with Content-Type: application/sdp */
    u_char hdrs[256];
    ngx_snprintf(hdrs, sizeof(hdrs),
                 "Content-Type: application/sdp\r\n"
                 "Content-Length: %uz\r\n",
                 ngx_strlen(sdp));

    send_reply(c, cseq, 200, "OK", (char*) hdrs);


    /* send the SDP body itself, and log what happened */
    ssize_t n = c->send(c, (u_char*) sdp, ngx_strlen(sdp));
    ngx_log_error(NGX_LOG_DEBUG, c->log, 0,
        "DESCRIBE: c->send(sdp) returned %zd (expected %uz)",
        n, ngx_strlen(sdp));

    if (n != (ssize_t) ngx_strlen(sdp)) {
        ngx_log_error(NGX_LOG_ERR, c->log, 0,
            "DESCRIBE: body send failed, aborting connection");
        ngx_close_connection(c);
        return;
    }

    /* re-arm the read handler */
    if (ngx_handle_read_event(c->read, 0) != NGX_OK) {
        ngx_close_connection(c);
    }
}

/* PLAY: confirm and switch to RTP interleaved */
static void
handle_play(ngx_connection_t *c, ngx_rtsp_session_t *rs, int cseq)
{
    rs->playing = 1;

    ngx_log_error(NGX_LOG_INFO, c->log, 0, "RTSP → PLAY");

    u_char extra[64];
    ngx_snprintf(extra, sizeof(extra),
                 "Session: %d\r\n"
                 "Content-Length: 0\r\n",
                 rs->session_id);
    send_reply(c, cseq, 200, "OK", (char*) extra);

    /* arm the write handler to send out RTP→TCP frames immediately */
    c->write->handler = ngx_rtsp_send_rtp_interleaved;
    if (ngx_handle_write_event(c->write, 0) != NGX_OK) {
        ngx_close_connection(c);
        return;
    }
    
    /* immediately invoke your write handler once to drain any queued frames */
    ngx_post_event(c->write, &ngx_posted_events);
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
    ngx_str_t  mount;
    u_char     buf[8192];
    ssize_t    n;
    int        to_read;
    char      *body, *cl;
    char    *p, *start, *end;
    u_char    *dst;

    // debug uri
    ngx_log_error(NGX_LOG_WARN, c->log, 0,
                  "[debug] handle_announce() got uri @%p: \"%s\"",
                  uri, uri);

    // locate the “/” just after host:port in "rtsp://host:port/path..."
    p = strstr(uri, "rtsp://");
    if (p) {
        p += strlen("rtsp://");
        p  = strchr(p, '/');
    }

    if (p) {
        start = p + 1;    // skip that slash
        end   = start;
        // consume until space or CR/LF
        while (*end && *end != ' ' && *end != '\r' && *end != '\n') {
            end++;
        }

        mount.len  = end - start;
        // allocate and copy into the connection pool (so it lives past buf[])
        dst = ngx_pnalloc(c->pool, mount.len + 1);
        if (dst == NULL) {
            ngx_log_error(NGX_LOG_ERR, c->log, 0,
                          "RTSP: pool alloc failed for mount");
            return;  
        }
        ngx_memcpy(dst, start, mount.len);
        dst[mount.len] = '\0';

        mount.data = dst;
    }
    else {
        // fallback: no path found
        mount.len  = 0;
        mount.data = (u_char*) "";
    }

    ngx_log_error(NGX_LOG_WARN, c->log, 0,
                  "[debug] mount parsed @%p len=%uz -> \"%s\"",
                  mount.data, mount.len,
                  mount.len ? (char*) mount.data : "<empty>");

    // 2) Create or look up the stream
    rs->stream = ngx_rtsp_get_stream(c->pool, &mount);

    // 3) Send our “200 OK” reply
    {
        u_char extra[64];
        ngx_snprintf(extra, sizeof(extra),
                     "Session: %d\r\n"
                     "Content-Length: 0\r\n",
                     rs->session_id);
        send_reply(c, cseq, 200, "OK", (char*) extra);
    }

    // 4) Drain the ANNOUNCE’s SDP body so the next recv() is the SETUP
    to_read = 0;
    // keep reading until we see "\r\n\r\n"
    while ((n = c->recv(c, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';

        // if we haven’t found headers → look for end-of-headers
        if (to_read == 0) {
            body = strstr((char*) buf, "\r\n\r\n");
            if (body) {
                // parse Content-Length:
                cl = strstr((char*) buf, "Content-Length:");
                if (cl) {
                    to_read = atoi(cl + 15);
                }
                // subtract any SDP bytes already read
                body += 4;            // skip the "\r\n\r\n"
                to_read -= (n - (body - (char*) buf));
                if (to_read <= 0) {
                    break;
                }
            }
        }
        else {
            // continue draining SDP
            to_read -= n;
            if (to_read <= 0) {
                break;
            }
        }
    }
    // (if n <= 0 here, connection closed or error; we’ll just continue)

    /* --- parse sprop-parameter-sets from the SDP in buf[] --- */
    {
            ngx_str_t raw = { (size_t)n, buf };
            u_char  *fmtp = ngx_strnstr(raw.data,
                                        "sprop-parameter-sets=",
                                        raw.len);
            if (fmtp) {
                fmtp += sizeof("sprop-parameter-sets=") - 1;
                u_char *eol = ngx_strlchr(fmtp,
                                          raw.data + raw.len,
                                          '\r');
                if (!eol) {
                    eol = ngx_strlchr(fmtp,
                                      raw.data + raw.len,
                                      '\n');
                }
                if (eol) {
                    u_char *comma = ngx_strlchr(fmtp, eol, ',');
                    if (comma) {
                        ngx_str_t *dst;
    
                        /* SPS-Base64 */
                        dst = &rs->stream->sps_b64;
                        dst->len = comma - fmtp;
                        dst->data = ngx_pnalloc(c->pool, dst->len + 1);
                        ngx_memcpy(dst->data, fmtp, dst->len);
                        dst->data[dst->len] = '\0';
    
                        /* PPS-Base64 */
                        dst = &rs->stream->pps_b64;
                        dst->len = eol - (comma + 1);
                        dst->data = ngx_pnalloc(c->pool, dst->len + 1);
                        ngx_memcpy(dst->data, comma + 1, dst->len);
                        dst->data[dst->len] = '\0';
    
                        /* decode to raw */
                        ngx_decode_base64(&rs->stream->sps,
                                         &rs->stream->sps_b64);
                        ngx_decode_base64(&rs->stream->pps,
                                         &rs->stream->pps_b64);
                    }
                }
            }
        }

    // 5) Re-arm the control parser for the next RTSP request
    if (ngx_handle_read_event(c->read, 0) != NGX_OK) {
        ngx_close_connection(c);
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
ngx_rtsp_recv_rtp_interleaved(ngx_event_t *ev)
{
    ngx_connection_t    *c   = ev->data;
    ngx_rtsp_session_t  *rs  = c->data;
    ngx_rtsp_nalu_pool_t *pool = &rs->stream->pool;
    u_char                ihdr[4];
    ssize_t               n;

    // 1) read the 4-byte interleaved header
    n = c->recv(c, ihdr, 4);
    if (n != 4 || ihdr[0] != '$') {
        ngx_log_error(NGX_LOG_ERR, c->log, 0,
                      "RTP→TCP: invalid interleaved header, closing");
        ngx_close_connection(c);
        return;
    }

    int channel = ihdr[1];
    int plen    = (ihdr[2] << 8) | ihdr[3];

    ngx_log_error(NGX_LOG_DEBUG, c->log, 0,
                  "RTP→TCP: got channel=%d length=%d", channel, plen);

    // 2) read the RTP packet
    u_char *pkt = ngx_palloc(c->pool, plen);
    if ((n = c->recv(c, pkt, plen)) != plen) {
        ngx_log_error(NGX_LOG_ERR, c->log, 0,
                      "RTP→TCP: packet read error (%zd/%d)", n, plen);
        ngx_close_connection(c);
        return;
    }

    if (! rs->playing) {
        //
        // --- RECORD mode: just parse & queue NALUs for later playback
        //

        uint8_t *rtp = pkt + 12;
        size_t   payload_len = plen - 12;
        //uint8_t   nal0       = rtp[0];
        //uint8_t   nal_type   = nal0 & 0x1F;

        // handle FU-A reassembly exactly as you had it…
        // when you finish a complete NAL, do:
        ngx_rtsp_nalu_t *nalu = ngx_palloc(c->pool,
                                   sizeof(*nalu) + payload_len);
        nalu->ts   = ntohl(*(uint32_t*)(rtp + 4)); /* or however you extracted ts */
        nalu->len  = payload_len;
        ngx_memcpy(nalu->data, rtp, payload_len);
        ngx_queue_insert_tail(&pool->queue, &nalu->queue);

    } else {
        //
        // --- PLAY mode: you want to *send* queued NALUs back to the client
        //

        ngx_rtsp_nalu_t  *n;
        ngx_queue_t      *q;

        // drain your queue
        for (q = ngx_queue_head(&pool->queue);
             q != ngx_queue_sentinel(&pool->queue);
             /**/ )
        {
            n = ngx_queue_data(q, ngx_rtsp_nalu_t, queue);
            q = ngx_queue_next(q);

            // send each NAL as an RTP→TCP interleaved frame
            if (ngx_rtsp_send_interleaved(c, channel,
                                          n->data, n->len) < 0)
            {
                ngx_log_error(NGX_LOG_ERR, c->log, 0,
                              "RTP→TCP: send interleaved failed");
                ngx_close_connection(c);
                return;
            }

            ngx_queue_remove(&n->queue);
            // note: you can free() or just let the pool be reclaimed on close
        }
    }

    // re-arm for the next chunk
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