// Tiny single-threaded HTTP/1.0 server.
//
// API (all responses close the connection):
//   GET  /                         small web UI (works from a phone browser too)
//   GET  /api/status               JSON with the state of every slot
//   POST /api/load?slot=N&name=X   body = raw 1024 byte .dump file
//   POST /api/remove?slot=N        take the figure off the portal
//   GET  /api/dump?slot=N          download the figure (including game writes)
#include "http.h"
#include "portal.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define MAX_HEADER 4096
#define MAX_BODY   (FIGURE_SIZE * 4)

static const char INDEX_HTML[] =
"<!doctype html><html><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>Goldportal</title><style>"
"body{font-family:sans-serif;background:#111;color:#eee;max-width:640px;margin:auto;padding:16px}"
".s{background:#222;border-radius:8px;padding:10px;margin:8px 0;display:flex;gap:8px;align-items:center;flex-wrap:wrap}"
".n{flex:1;min-width:160px}button,label{background:#36c;color:#fff;border:0;border-radius:6px;padding:6px 10px;cursor:pointer}"
"input{display:none}.e{color:#888}</style></head><body><h2>Goldportal</h2><div id=st></div><div id=sl></div><script>"
"async function r(){let j=await (await fetch('/api/status')).json();"
"st.textContent='Game connected to portal: '+(j.active?'yes':'not yet');sl.innerHTML='';"
"j.slots.forEach(s=>{let d=document.createElement('div');d.className='s';"
"d.innerHTML=`<span class=n>${s.slot+1}: ${s.loaded?s.name+' (id '+s.id+')'+(s.dirty?' *':''):'<span class=e>empty</span>'}</span>`+"
"`<label>Load<input type=file onchange='ld(${s.slot},this.files[0])'></label>`+"
"(s.loaded?`<button onclick='rm(${s.slot})'>Remove</button><a href='/api/dump?slot=${s.slot}' download='${s.name}'><button>Save</button></a>`:'');"
"sl.appendChild(d)})}"
"async function ld(i,f){await fetch('/api/load?slot='+i+'&name='+encodeURIComponent(f.name),{method:'POST',body:await f.arrayBuffer()});r()}"
"async function rm(i){await fetch('/api/remove?slot='+i,{method:'POST'});r()}"
"r();setInterval(r,3000)</script></body></html>";

static void send_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, 0);
        if (n <= 0) return;
        p += n; len -= (size_t)n;
    }
}

static void respond(int fd, int code, const char *type, const void *body, size_t len)
{
    const char *msg = code == 200 ? "OK" : code == 400 ? "Bad Request" :
                      code == 404 ? "Not Found" : code == 413 ? "Too Large" : "Error";
    char hdr[256];
    int n = snprintf(hdr, sizeof(hdr),
        "HTTP/1.0 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
        "Access-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
        "Connection: close\r\n\r\n", code, msg, type, len);
    send_all(fd, hdr, (size_t)n);
    if (body && len)
        send_all(fd, body, len);
}

static void respond_json(int fd, int code, const char *json)
{
    respond(fd, code, "application/json", json, strlen(json));
}

// Finds key=value in a query string, url-decodes value into out. Returns 1 if found.
static int query_param(const char *query, const char *key, char *out, size_t outlen)
{
    size_t klen = strlen(key);
    const char *p = query;
    while (p && *p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            p += klen + 1;
            size_t o = 0;
            while (*p && *p != '&' && o + 1 < outlen) {
                if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
                    char hex[3] = { p[1], p[2], 0 };
                    out[o++] = (char)strtol(hex, NULL, 16);
                    p += 3;
                } else {
                    out[o++] = *p == '+' ? ' ' : *p;
                    p++;
                }
            }
            out[o] = 0;
            return 1;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    return 0;
}

static int query_slot(const char *query)
{
    char v[8];
    if (!query_param(query, "slot", v, sizeof(v)))
        return -1;
    char *end;
    long s = strtol(v, &end, 10);
    if (*end || s < 0 || s >= PORTAL_SLOTS)
        return -1;
    return (int)s;
}

static void sanitize(char *s)
{
    for (; *s; s++)
        if (*s == '"' || *s == '\\' || *s == '/' || (unsigned char)*s < 0x20)
            *s = '_';
}

static void save_backup(int slot)
{
    portal_slot_info info;
    portal_get_info(slot, &info);
    if (!info.dirty)
        return;
    static uint8_t buf[FIGURE_SIZE];
    if (portal_get_dump(slot, buf) != 0)
        return;
    char path[160];
    sanitize(info.name);
    snprintf(path, sizeof(path), GP_DATA_DIR "/backup_%s", info.name[0] ? info.name : "figure.dump");
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0777);
    if (fd >= 0) {
        write(fd, buf, FIGURE_SIZE);
        close(fd);
        LOG("saved backup %s", path);
    }
}

static void handle_status(int fd)
{
    char json[2048];
    gp_state st;
    gp_get_state(&st);
    size_t o = (size_t)snprintf(json, sizeof(json),
        "{\"version\":\"%u.%02u\",\"title\":\"%s\",\"usbd_loaded\":%s,\"hooks_ok\":%d,\"portal_opened\":%s,\"sniff\":%s,"
        "\"active\":%s,\"slots\":[",
        st.version >> 8 & 0xFF, st.version & 0xFF, g_title_id, st.usbd_loaded ? "true" : "false", st.hooks_ok,
        st.portal_opened ? "true" : "false", st.sniff ? "true" : "false",
        portal_is_active() ? "true" : "false");
    for (int i = 0; i < PORTAL_SLOTS; i++) {
        portal_slot_info info;
        portal_get_info(i, &info);
        sanitize(info.name);
        o += (size_t)snprintf(json + o, sizeof(json) - o,
            "%s{\"slot\":%d,\"loaded\":%s,\"dirty\":%s,\"id\":%u,\"variant\":%u,\"name\":\"%s\"}",
            i ? "," : "", i, info.loaded ? "true" : "false", info.dirty ? "true" : "false",
            info.loaded ? info.figure_id : 0, info.loaded ? info.variant : 0,
            info.loaded ? info.name : "");
    }
    snprintf(json + o, sizeof(json) - o, "]}");
    respond_json(fd, 200, json);
}

// Sends the end of a log file (the plugin's own log, straight from the PS4).
static void handle_log(int fd, const char *path)
{
    enum { MAX_LOG = 256 * 1024 };
    int f = open(path, O_RDONLY);
    if (f < 0) { respond(fd, 404, "text/plain", "no log file\n", 12); return; }
    off_t size = lseek(f, 0, SEEK_END);
    off_t start = size > MAX_LOG ? size - MAX_LOG : 0;
    lseek(f, start, SEEK_SET);
    char *buf = malloc(MAX_LOG);
    ssize_t n = buf ? read(f, buf, MAX_LOG) : -1;
    close(f);
    if (n < 0) { free(buf); respond(fd, 500, "text/plain", "read failed\n", 12); return; }
    respond(fd, 200, "text/plain; charset=utf-8", buf, (size_t)n);
    free(buf);
}

static void handle_client(int fd)
{
    static char req[MAX_HEADER + MAX_BODY + 1];
    size_t got = 0;
    char *hdr_end = NULL;

    while (got < MAX_HEADER && !hdr_end) {
        ssize_t n = recv(fd, req + got, MAX_HEADER - got, 0);
        if (n <= 0) return;
        got += (size_t)n;
        req[got] = 0;
        hdr_end = strstr(req, "\r\n\r\n");
    }
    if (!hdr_end) { respond_json(fd, 400, "{\"error\":\"bad request\"}"); return; }

    char method[8] = {0}, target[512] = {0};
    if (sscanf(req, "%7s %511s", method, target) != 2) {
        respond_json(fd, 400, "{\"error\":\"bad request\"}");
        return;
    }

    size_t content_len = 0;
    for (char *h = req; h < hdr_end; h = strstr(h, "\r\n") + 2) {
        if (strncasecmp(h, "Content-Length:", 15) == 0)
            content_len = (size_t)strtoul(h + 15, NULL, 10);
        if (!strstr(h, "\r\n")) break;
    }
    if (content_len > MAX_BODY) { respond_json(fd, 413, "{\"error\":\"file too large\"}"); return; }

    char *body = hdr_end + 4;
    size_t have = got - (size_t)(body - req);
    while (have < content_len) {
        ssize_t n = recv(fd, body + have, content_len - have, 0);
        if (n <= 0) break;
        have += (size_t)n;
    }

    char *query = strchr(target, '?');
    if (query) *query++ = 0; else query = "";

    if (strcmp(method, "OPTIONS") == 0) {
        respond(fd, 200, "text/plain", NULL, 0);
    } else if (strcmp(method, "GET") == 0 && strcmp(target, "/") == 0) {
        respond(fd, 200, "text/html", INDEX_HTML, sizeof(INDEX_HTML) - 1);
    } else if (strcmp(method, "GET") == 0 && strcmp(target, "/api/status") == 0) {
        handle_status(fd);
    } else if (strcmp(method, "GET") == 0 && strcmp(target, "/api/log") == 0) {
        if (strstr(query, "prev")) {
            char prev[112];
            snprintf(prev, sizeof(prev), GP_DATA_DIR "/log_%s_prev.txt", g_title_id);
            handle_log(fd, prev);
        } else {
            handle_log(fd, g_log_path);
        }
    } else if (strcmp(method, "POST") == 0 && strcmp(target, "/api/load") == 0) {
        int slot = query_slot(query);
        char name[FIGURE_NAME_LEN] = "figure.dump";
        query_param(query, "name", name, sizeof(name));
        if (slot < 0)
            respond_json(fd, 400, "{\"error\":\"bad slot\"}");
        else if (have < FIGURE_SIZE)
            respond_json(fd, 400, "{\"error\":\"dump must be at least 1024 bytes\"}");
        else {
            save_backup(slot);   // replacing a figure the game changed: keep its progress
            portal_load(slot, (uint8_t *)body, have, name);
            LOG("loaded '%s' into slot %d (%zu bytes)", name, slot, have);
            respond_json(fd, 200, "{\"ok\":true}");
        }
    } else if (strcmp(method, "POST") == 0 && strcmp(target, "/api/remove") == 0) {
        int slot = query_slot(query);
        if (slot < 0)
            respond_json(fd, 400, "{\"error\":\"bad slot\"}");
        else {
            save_backup(slot);
            portal_remove(slot);
            LOG("removed slot %d", slot);
            respond_json(fd, 200, "{\"ok\":true}");
        }
    } else if (strcmp(method, "GET") == 0 && strcmp(target, "/api/dump") == 0) {
        int slot = query_slot(query);
        static uint8_t buf[FIGURE_SIZE];
        if (slot < 0 || portal_get_dump(slot, buf) != 0)
            respond_json(fd, 404, "{\"error\":\"no figure in that slot\"}");
        else
            respond(fd, 200, "application/octet-stream", buf, FIGURE_SIZE);
    } else {
        respond_json(fd, 404, "{\"error\":\"not found\"}");
    }
}

static void *server_thread(void *arg)
{
    int port = (int)(intptr_t)arg;
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { LOG("http: socket() failed"); return NULL; }

    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
    setsockopt(srv, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(srv, 4) != 0) {
        LOG("http: bind/listen on port %d failed", port);
        close(srv);
        return NULL;
    }
    LOG("http: listening on port %d", port);

    for (;;) {
        int c = accept(srv, NULL, NULL);
        if (c < 0) { usleep(100000); continue; }
#ifdef SO_NOSIGPIPE
        setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
        handle_client(c);
        close(c);
    }
    return NULL;
}

int http_server_start(int port)
{
    pthread_t th;
    if (pthread_create(&th, NULL, server_thread, (void *)(intptr_t)port) != 0) {
        LOG("http: pthread_create failed");
        return -1;
    }
    pthread_detach(th);
    return 0;
}
