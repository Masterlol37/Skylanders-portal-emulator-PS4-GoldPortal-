#pragma once

#define GP_HTTP_PORT 8085

// Starts the mini web server on a background thread. Returns 0 on success.
int http_server_start(int port);

// Provided by main.c, reported in /api/status.
typedef struct {
    unsigned version;        // 0x0105 = 1.05
    int usbd_loaded;         // game has loaded libSceUsbd and we tried to hook it
    int hooks_ok;            // number of libSceUsbd hooks installed
    int portal_opened;       // game opened the virtual portal
    int sniff;               // sniff mode active
} gp_state;
void gp_get_state(gp_state *s);
