/*
 *  IPTV - HTTP/HTTPS handler
 *
 *  Copyright (C) 2013 Adam Sutton
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "tvheadend.h"
#include "iptv_private.h"
#include "http.h"
#include "misc/m3u.h"

typedef struct http_priv {
  iptv_mux_t    *im;
  http_client_t *hc;
  int            m3u_header;
  uint64_t       off;
  char          *host_url;
  char          *hls_url;
  int            hls_url2;
  htsmsg_t      *hls_m3u;
  uint8_t       *hls_si;
  time_t         hls_last_si;
} http_priv_t;

/*
 *
 */
static char *
iptv_http_get_url( http_priv_t *hp, htsmsg_t *m )
{
  htsmsg_t *items, *item, *inf, *sel = NULL;
  htsmsg_field_t *f;
  int64_t bandwidth;
  int width, height, sel_width = 0, sel_height = 0;
  const char *s;

  items = htsmsg_get_list(m, "items");
  HTSMSG_FOREACH(f, items) {
    if ((item = htsmsg_field_get_map(f)) == NULL) continue;
    inf = htsmsg_get_map(item, "stream-inf");
    if (inf) {
      bandwidth = htsmsg_get_s64_or_default(inf, "BANDWIDTH", 0);
      s = htsmsg_get_str(inf, "RESOLUTION");
      width = height = 0;
      if (s)
        sscanf(s, "%dx%d", &width, &height);
      if (htsmsg_get_str(item, "m3u-url") &&
          bandwidth > 200000 &&
          sel_width < width &&
          sel_height < height) {
        sel = item;
        sel_width = width;
        sel_height = height;
      }
    } else {
      s = htsmsg_get_str(item, "m3u-url");
      if (s && s[0]) {
        s = strdup(s);
        htsmsg_field_destroy(items, f);
        if (hp->hls_url)
          hp->hls_url2 = 1;
        return (char *)s;
      }
    }
  }
  if (sel && hp->hls_url == NULL) {
    inf = htsmsg_get_map(sel, "stream-inf");
    bandwidth = htsmsg_get_s64_or_default(inf, "BANDWIDTH", 0);
    tvhdebug("iptv", "HLS - selected stream %s, %"PRId64"kb/s, %s\n",
             htsmsg_get_str(inf, "RESOLUTION"),
             bandwidth / 1024,
             htsmsg_get_str(inf, "CODECS"));
    s = htsmsg_get_str(sel, "m3u-url");
    if (s && s[0]) {
      free(hp->hls_url);
      hp->hls_url = strdup(s);
      return strdup(s);
    }
  }
  return NULL;
}


/*
 * Connected
 */
static int
iptv_http_header ( http_client_t *hc )
{
  http_priv_t *hp = hc->hc_aux;
  iptv_mux_t *im;
  char *argv[3], *s;
  int n;

  if (hp == NULL)
    return 0;
  im = hp->im;
  if (im == NULL)
    return 0;

  /* multiple headers for redirections */
  if (hc->hc_code != HTTP_STATUS_OK)
    return 0;

  s = http_arg_get(&hc->hc_args, "Content-Type");
  if (s) {
    n = http_tokenize(s, argv, ARRAY_SIZE(argv), ';');
    if (n > 0 &&
        (strcasecmp(s, "audio/mpegurl") == 0 ||
         strcasecmp(s, "audio/x-mpegurl") == 0 ||
         strcasecmp(s, "application/x-mpegurl") == 0 ||
         strcasecmp(s, "application/apple.vnd.mpegurl") == 0 ||
         strcasecmp(s, "application/vnd.apple.mpegurl") == 0)) {
      if (hp->m3u_header > 10) {
        hp->m3u_header = 0;
        return 0;
      }
      hp->m3u_header++;
      return 0;
    }
  }

  hp->m3u_header = 0;
  hp->off = 0;
  pthread_mutex_lock(&global_lock);
  iptv_input_mux_started(hp->im);
  pthread_mutex_unlock(&global_lock);
  return 0;
}

/*
 * Receive data
 */
static int
iptv_http_data
  ( http_client_t *hc, void *buf, size_t len )
{
  http_priv_t *hp = hc->hc_aux;
  iptv_mux_t *im;
  int pause = 0, rem;

  if (hp == NULL || hp->im == NULL || hc->hc_code != HTTP_STATUS_OK)
    return 0;

  im = hp->im;

  if (hp->m3u_header) {
    sbuf_append(&im->mm_iptv_buffer, buf, len);
    return 0;
  }

  if (hp->hls_url && hp->off < 2*188 && len >= 2*188) {
    free(hp->hls_si);
    hp->hls_si = malloc(2*188);
    memcpy(hp->hls_si, buf, 2*188);
  }

  pthread_mutex_lock(&iptv_lock);

  if (dispatch_clock != hp->hls_last_si) {
    /* do rounding to next MPEG-TS packet */
    rem = 188 - (hp->off % 188);
    if (rem < 188) {
      if (len < rem)
        goto next;
      sbuf_append(&im->mm_iptv_buffer, buf, rem);
      buf += rem;
      len -= rem;
      hp->off += rem;
    }
    sbuf_append(&im->mm_iptv_buffer, hp->hls_si, 2*188);
    hp->hls_last_si = dispatch_clock;
  }

next:
  hp->off += len;
  sbuf_append(&im->mm_iptv_buffer, buf, len);
  tsdebug_write((mpegts_mux_t *)im, buf, len);

  if (len > 0)
    if (iptv_input_recv_packets(im, len) == 1)
      pause = hc->hc_pause = 1;

  pthread_mutex_unlock(&iptv_lock);

  if (pause) {
    pthread_mutex_lock(&global_lock);
    if (im->mm_active)
      gtimer_arm(&im->im_pause_timer, iptv_input_unpause, im, 1);
    pthread_mutex_unlock(&global_lock);
  }
  return 0;
}

/*
 * Complete data
 */
static int
iptv_http_complete
  ( http_client_t *hc )
{
  http_priv_t *hp = hc->hc_aux;
  iptv_mux_t *im = hp->im;
  const char *host_url;
  char *url;
  htsmsg_t *m, *m2;
  char *p;
  url_t u;
  size_t l;
  int r;

  if (hp->m3u_header) {
    hp->m3u_header = 0;

    sbuf_append(&im->mm_iptv_buffer, "", 1);

    if ((p = http_arg_get(&hc->hc_args, "Host")) != NULL) {
      l = strlen(p) + 16;
      host_url = alloca(l);
      snprintf((char *)host_url, l, "%s://%s", hc->hc_ssl ? "https" : "http", p);
    } else if (im->mm_iptv_url_raw) {
      host_url = strdupa(im->mm_iptv_url_raw);
      if ((p = strchr(host_url, '/')) != NULL) {
        p++;
        if (*p == '/')
          p++;
        if ((p = strchr(p, '/')) != NULL)
          *p = '\0';
      }
      urlinit(&u);
      r = urlparse(host_url, &u);
      urlreset(&u);
      if (r)
        goto end;
    } else {
      host_url = NULL;
    }

    if (host_url) {
      free(hp->host_url);
      hp->host_url = strdup(host_url);
    }
    m = parse_m3u((char *)im->mm_iptv_buffer.sb_data, NULL, hp->host_url);
url:
    url = iptv_http_get_url(hp, m);
    if (hp->hls_url2) {
      hp->hls_m3u = m;
      m = NULL;
    }
    tvhtrace("iptv", "m3u url: '%s'", url);
    if (url == NULL) {
      tvherror("iptv", "m3u contents parsing failed");
      goto fin;
    }
new_m3u:
    urlinit(&u);
    if (!urlparse(url, &u)) {
      hc->hc_keepalive = 0;
      r = http_client_simple_reconnect(hc, &u, HTTP_VERSION_1_1);
      if (r < 0)
        tvherror("iptv", "cannot reopen http client: %d'", r);
    } else {
      tvherror("iptv", "m3u url invalid '%s'", url);
    }
    free(url);
    urlreset(&u);
fin:
    htsmsg_destroy(m);
end:
    sbuf_reset(&im->mm_iptv_buffer, IPTV_BUF_SIZE);
  } else {
    if (hp->hls_url && hp->hls_m3u) {
      m = hp->hls_m3u;
      hp->hls_m3u = NULL;
      m2 = htsmsg_get_list(m, "items");
      if (m2 && htsmsg_is_empty(m2)) {
        hp->hls_url2 = 0;
        if (!htsmsg_get_bool_or_default(m, "x-endlist", 0)) {
          url = strdup(hp->hls_url);
          goto new_m3u;
        }
      } else {
        goto url;
      }
    }
  }
  return 0;
}

/*
 * Custom headers
 */
static void
iptv_http_create_header
  ( http_client_t *hc, http_arg_list_t *h, const url_t *url, int keepalive )
{
  http_priv_t *hp = hc->hc_aux;

  http_client_basic_args(hc, h, url, keepalive);
  http_client_add_args(hc, h, hp->im->mm_iptv_hdr);
}

/*
 * Setup HTTP(S) connection
 */
static int
iptv_http_start
  ( iptv_mux_t *im, const char *raw, const url_t *u )
{
  http_priv_t *hp;
  http_client_t *hc;
  int r;

  hp = calloc(1, sizeof(*hp));
  hp->im = im;
  if (!(hc = http_client_connect(hp, HTTP_VERSION_1_1, u->scheme,
                                 u->host, u->port, NULL))) {
    free(hp);
    return SM_CODE_TUNING_FAILED;
  }
  hc->hc_hdr_create      = iptv_http_create_header;
  hc->hc_hdr_received    = iptv_http_header;
  hc->hc_data_received   = iptv_http_data;
  hc->hc_data_complete   = iptv_http_complete;
  hc->hc_handle_location = 1;        /* allow redirects */
  hc->hc_io_size         = 128*1024; /* increase buffering */
  http_client_register(hc);          /* register to the HTTP thread */
  r = http_client_simple(hc, u);
  if (r < 0) {
    http_client_close(hc);
    free(hp);
    return SM_CODE_TUNING_FAILED;
  }
  hp->hc = hc;
  im->im_data = hp;
  sbuf_init_fixed(&im->mm_iptv_buffer, IPTV_BUF_SIZE);

  return 0;
}

/*
 * Stop connection
 */
static void
iptv_http_stop
  ( iptv_mux_t *im )
{
  http_priv_t *hp = im->im_data;

  hp->hc->hc_aux = NULL;
  pthread_mutex_unlock(&iptv_lock);
  http_client_close(hp->hc);
  pthread_mutex_lock(&iptv_lock);
  im->im_data = NULL;
  htsmsg_destroy(hp->hls_m3u);
  free(hp->hls_url);
  free(hp->hls_si);
  free(hp->host_url);
  free(hp);
}


/*
 * Pause/Unpause
 */
static void
iptv_http_pause
  ( iptv_mux_t *im, int pause )
{
  http_priv_t *hp = im->im_data;

  assert(pause == 0);
  http_client_unpause(hp->hc);
}

/*
 * Initialise HTTP handler
 */

void
iptv_http_init ( void )
{
  static iptv_handler_t ih[] = {
    {
      .scheme = "http",
      .start  = iptv_http_start,
      .stop   = iptv_http_stop,
      .pause  = iptv_http_pause
    },
    {
      .scheme  = "https",
      .start  = iptv_http_start,
      .stop   = iptv_http_stop,
      .pause  = iptv_http_pause
    }
  };
  iptv_handler_register(ih, 2);
}
