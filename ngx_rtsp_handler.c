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
    u_char hdr[4] = { '$', (u_char)channel,
                      (u_char)(plen>>8), (u_char)(plen & 0xff) };
    ssize_t n;

    n = c->send(c, hdr, 4);
    ngx_log_error(NGX_LOG_INFO, c->log, 0,
        "INTERLEAVED: sent header channel=%d plen=%uz -> ret=%zd",
        channel, plen, n);
    if (n != 4) {
        return -1;
    }

    n = c->send(c, payload, plen);
    ngx_log_error(NGX_LOG_INFO, c->log, 0,
        "INTERLEAVED: sent payload channel=%d len=%uz -> ret=%zd",
        channel, plen, n);
    return n == (ssize_t) plen ? plen : (size_t) -1;
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
    ngx_connection_t    *c   = wev->data;
    ngx_rtsp_session_t  *rs  = c->data;
    ngx_rtsp_nalu_pool_t *pool = &rs->stream->pool;
    ngx_rtsp_nalu_t     *nalu;
    ngx_queue_t         *q, *next;

    if (! rs->playing) {
        return;
    }

    // first PLAY => inject SPS/PPS with 0x00000001 start codes
    if (! rs->stream->sent_spspps) {
        static const u_char sc[4] = {0,0,0,1};
        // SPS
        ngx_rtsp_send_interleaved(c, 0, (u_char*)sc, 4);
        ngx_rtsp_send_interleaved(c, 0,
                                 rs->stream->sps.data,
                                 rs->stream->sps.len);
        // PPS
        ngx_rtsp_send_interleaved(c, 0, (u_char*)sc, 4);
        ngx_rtsp_send_interleaved(c, 0,
                                 rs->stream->pps.data,
                                 rs->stream->pps.len);
        rs->stream->sent_spspps = 1;
    }

    // then drain queued NALUs
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

    // keep write handler armed
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
    ngx_str_t mount;
    // parse mount exactly as in announce…
    char *p = strstr(uri, "rtsp://");
    if (p) {
        p += strlen("rtsp://");
        p = strchr(p, '/');
    }
    if (p) {
        mount.data = (u_char*)(p + 1);
        mount.len  = strcspn((const char *)mount.data, " \r\n");
    } else {
        mount.data = (u_char*)"";
        mount.len  = 0;
    }
    rs->stream = ngx_rtsp_get_stream(c->pool, &mount);

    ngx_log_error(NGX_LOG_INFO, c->log, 0,
                  "RTSP → DESCRIBE, uri='%s'", uri);

    ngx_log_error(NGX_LOG_INFO, c->log, 0,
        "DESCRIBE: advertising fmtp:96 sprop-parameter-sets=%V,%V",
            &rs->stream->sps_b64, &rs->stream->pps_b64);

    // build fmtp line
    u_char fmtp[256];
    ngx_snprintf(fmtp, sizeof(fmtp),
                 "packetization-mode=1;"
                 "sprop-parameter-sets=%V,%V\r\n",
                 &rs->stream->sps_b64,
                 &rs->stream->pps_b64);

    // assemble SDP
    u_char sdp[512];
    ngx_snprintf(sdp, sizeof(sdp),
                 "v=0\r\n"
                 "o=- 0 0 IN IP4 0.0.0.0\r\n"
                 "s=RTSP Session\r\n"
                 "t=0 0\r\n"
                 "m=video 0 RTP/AVP 96\r\n"
                 "a=control:*\r\n"
                 "a=rtpmap:96 H264/90000\r\n"
                 "a=fmtp:96 %s", fmtp);

    ngx_log_error(NGX_LOG_INFO, c->log, 0,
        "DESCRIBE: full SDP payload length=%uz:\n%.*s",
            ngx_strlen(sdp), (int) ngx_strlen(sdp), sdp);

    // send headers + SDP
    u_char hdrs[256];
    ngx_snprintf(hdrs, sizeof(hdrs),
                 "Content-Type: application/sdp\r\n"
                 "Content-Length: %uz\r\n",
                 ngx_strlen(sdp));
    send_reply(c, cseq, 200, "OK", (char*) hdrs);

    if (c->send(c, sdp, ngx_strlen(sdp))
        != (ssize_t)ngx_strlen(sdp))
    {
        ngx_log_error(NGX_LOG_ERR, c->log, 0,
                      "DESCRIBE: failed to send body");
        ngx_close_connection(c);
        return;
    }

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

    /* count queued NALUs */
    ngx_queue_t *q;
    size_t      count = 0;
    for (q = ngx_queue_head(&rs->stream->pool.queue);
         q != ngx_queue_sentinel(&rs->stream->pool.queue);
         q = ngx_queue_next(q))
    {
        count++;
    }

    ngx_log_error(NGX_LOG_INFO, c->log, 0,
        "PLAY: session=%d, queue has %uz NALUs, sent_spspps=%d",
        rs->session_id, count, rs->stream->sent_spspps);

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
    ngx_str_t    mount;
    u_char       buf[8192];
    ssize_t      n;
    char        *cl_hdr, *body_start;
    int          content_len = 0;

    // 1) parse the mount exactly as before…
    char *p = strstr(uri, "rtsp://");
    if (p) {
        p += strlen("rtsp://");
        p = strchr(p, '/');
    }
    if (p) {
        mount.data = (u_char*)(p + 1);
        mount.len  = strcspn((const char *)mount.data, " \r\n");
    } else {
        mount.data = (u_char*)"";
        mount.len  = 0;
    }
    rs->stream = ngx_rtsp_get_stream(c->pool, &mount);

    // 2) reply OK
    {
        u_char extra[64];
        ngx_snprintf(extra, sizeof(extra),
                     "Session: %d\r\n"
                     "Content-Length: 0\r\n",
                     rs->session_id);
        send_reply(c, cseq, 200, "OK", (char*) extra);
    }

    // 3) read headers+start of body
    n = c->recv(c, buf, sizeof(buf));
    if (n <= 0) {
        return;
    }
    buf[n] = '\0';

    // 4) find Content-Length:
    cl_hdr = strstr((char*)buf, "Content-Length:");
    if (cl_hdr) {
        content_len = atoi(cl_hdr + strlen("Content-Length:"));
    }

    // 5) find start of SDP body (after "\r\n\r\n")
    body_start = strstr((char*)buf, "\r\n\r\n");
    if (body_start) {
        body_start += 4;
        n -= (body_start - (char*)buf);
    } else {
        body_start = (char*)buf + n;
        n = 0;
    }

    // 6) if we haven’t yet read all SDP, read the rest
    if (content_len > n) {
        ssize_t m = c->recv(c,
            (u_char*)(body_start + n),
            content_len - n);
        if (m > 0) {
            n += m;
        }
    }
    // now `body_start[0..content_len-1]` is exactly the SDP

    // 7) extract sprop-parameter-sets
    {
        char *fmtp = strstr(body_start, "sprop-parameter-sets=");
        if (fmtp) {
            fmtp += strlen("sprop-parameter-sets=");
            char *comma = strchr(fmtp, ',');
            char *eol   = strpbrk(fmtp, "\r\n");
            if (comma && eol && comma < eol) {
                size_t sps_b64_len = comma - fmtp;
                size_t pps_b64_len = eol - (comma + 1);

                // copy SPS b64
                rs->stream->sps_b64.len = sps_b64_len;
                rs->stream->sps_b64.data = ngx_pnalloc(c->pool, sps_b64_len + 1);
                ngx_memcpy(rs->stream->sps_b64.data, fmtp, sps_b64_len);
                rs->stream->sps_b64.data[sps_b64_len] = '\0';

                // copy PPS b64
                rs->stream->pps_b64.len = pps_b64_len;
                rs->stream->pps_b64.data = ngx_pnalloc(c->pool, pps_b64_len + 1);
                ngx_memcpy(rs->stream->pps_b64.data,
                           comma + 1, pps_b64_len);
                rs->stream->pps_b64.data[pps_b64_len] = '\0';

                // decode them
                ngx_decode_base64(&rs->stream->sps,
                                 &rs->stream->sps_b64);
                ngx_decode_base64(&rs->stream->pps,
                                 &rs->stream->pps_b64);
            }
        }
    }

    /* --- after decoding sps_b64/pps_b64 and raw sps/pps --- */
    ngx_log_error(NGX_LOG_INFO, c->log, 0,
        "ANNOUNCE: got sps_b64=%V (raw %uz bytes), pps_b64=%V (raw %uz bytes)",
        &rs->stream->sps_b64, rs->stream->sps.len,
        &rs->stream->pps_b64, rs->stream->pps.len);

    // 8) re-arm for the next RTSP request
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