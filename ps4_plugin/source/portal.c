// Skylanders Portal of Power emulation core.
//
// Protocol summary (PS3/PS4/Wii-U portal, VID 0x1430 PID 0x0150):
//   Host sends 32 byte command reports, first byte is an ASCII command:
//     'A' activate/deactivate   -> reply 'A' <on> 0xFF 0x77
//     'C' set colour            -> no reply
//     'J' fade colour           -> reply 'J'
//     'L' set trap LED          -> no reply
//     'M' audio/unknown         -> reply 'M' <arg> 0x00 0x19
//     'Q' read block            -> reply 'Q' 0x10|slot block <16 bytes>  (0x01 if no figure)
//     'R' reset/ready           -> reply 'R' 0x02 0x1B
//     'S' status                -> reply is the normal status report
//     'W' write block           -> reply 'W' 0x10|slot block
//   When no reply is pending, every interrupt IN read returns a status report:
//     'S' <u32 LE, 2 bits per slot> <counter> <active>
//   Slot bits: 0 = empty, 1 = present, 3 = just placed, 2 = just removed.
#include "portal.h"
#include <string.h>
#include <pthread.h>

#define QUEUE_LEN    32
#define STATUS_QUEUE 4

typedef struct {
    uint8_t  data[FIGURE_SIZE];
    int      loaded;
    int      dirty;
    char     name[FIGURE_NAME_LEN];
    uint8_t  status;                 // current 2 bit status
    uint8_t  queued[STATUS_QUEUE];   // pending status transitions
    int      queued_count;
} slot_t;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static slot_t   g_slots[PORTAL_SLOTS];
static uint8_t  g_queue[QUEUE_LEN][PORTAL_REPORT_LEN];
static int      g_q_head, g_q_count;
static int      g_active;
static uint8_t  g_counter;
static uint8_t  g_portal_id[2] = { 0x02, 0x03 };   // 'R' reply; recorded from a real portal with Swap Force

void portal_set_id(uint8_t a, uint8_t b)
{
    g_portal_id[0] = a;
    g_portal_id[1] = b;
}

void portal_init(void)
{
    pthread_mutex_lock(&g_lock);
    memset(g_slots, 0, sizeof(g_slots));
    g_q_head = g_q_count = 0;
    g_active = 0;
    g_counter = 0;
    pthread_mutex_unlock(&g_lock);
}

// --- helpers, call with lock held ---

static void queue_reply(const uint8_t *r, size_t n)
{
    if (g_q_count == QUEUE_LEN) {            // drop oldest rather than block the game
        g_q_head = (g_q_head + 1) % QUEUE_LEN;
        g_q_count--;
    }
    uint8_t *dst = g_queue[(g_q_head + g_q_count) % QUEUE_LEN];
    memset(dst, 0, PORTAL_REPORT_LEN);
    memcpy(dst, r, n > PORTAL_REPORT_LEN ? PORTAL_REPORT_LEN : n);
    g_q_count++;
}

static void push_status(slot_t *s, uint8_t a, uint8_t b)
{
    s->queued_count = 0;
    s->queued[s->queued_count++] = a;
    s->queued[s->queued_count++] = b;
}

static void build_status(uint8_t out[PORTAL_REPORT_LEN])
{
    uint32_t bits = 0;
    for (int i = PORTAL_SLOTS - 1; i >= 0; i--) {
        slot_t *s = &g_slots[i];
        if (s->queued_count > 0) {
            s->status = s->queued[0];
            memmove(s->queued, s->queued + 1, --s->queued_count);
        }
        bits = (bits << 2) | (s->status & 3);
    }
    memset(out, 0, PORTAL_REPORT_LEN);
    out[0] = 'S';
    out[1] = (uint8_t)(bits);
    out[2] = (uint8_t)(bits >> 8);
    out[3] = (uint8_t)(bits >> 16);
    out[4] = (uint8_t)(bits >> 24);
    out[5] = g_counter++;
    out[6] = g_active ? 1 : 0;
}

// --- game side ---

void portal_handle_command(const uint8_t *buf, size_t len)
{
    if (!buf || len == 0)
        return;

    uint8_t r[PORTAL_REPORT_LEN] = {0};
    pthread_mutex_lock(&g_lock);

    switch (buf[0]) {
    case 'A':
        g_active = len > 1 ? buf[1] : 1;
        r[0] = 'A'; r[1] = len > 1 ? buf[1] : 0; r[2] = 0xFF; r[3] = 0x77;
        queue_reply(r, 4);
        break;
    case 'C':
    case 'L':
        break;                                   // LED colours: nothing to do
    case 'J':
        r[0] = 'J';
        queue_reply(r, 1);
        break;
    case 'M':
        r[0] = 'M'; r[1] = len > 1 ? buf[1] : 0; r[2] = 0x00; r[3] = 0x19;
        queue_reply(r, 4);
        break;
    case 'Q': {
        if (len < 3) break;
        uint8_t idx = buf[1] & 0x0F, block = buf[2];
        r[0] = 'Q'; r[2] = block;
        if (idx < PORTAL_SLOTS && g_slots[idx].loaded && block < FIGURE_SIZE / 16) {
            r[1] = 0x10 | idx;
            memcpy(r + 3, g_slots[idx].data + block * 16, 16);
        } else {
            r[1] = idx;                          // no figure: real portal answers 51 0<slot> <block>
        }
        queue_reply(r, PORTAL_REPORT_LEN);
        break;
    }
    case 'R':
        r[0] = 'R'; r[1] = g_portal_id[0]; r[2] = g_portal_id[1];   // portal model id
        queue_reply(r, 3);
        break;
    case 'S':
        build_status(r);
        queue_reply(r, PORTAL_REPORT_LEN);
        break;
    case 'W': {
        if (len < 19) break;
        uint8_t idx = buf[1] & 0x0F, block = buf[2];
        r[0] = 'W'; r[2] = block;
        if (idx < PORTAL_SLOTS && g_slots[idx].loaded && block < FIGURE_SIZE / 16) {
            memcpy(g_slots[idx].data + block * 16, buf + 3, 16);
            g_slots[idx].dirty = 1;
            r[1] = 0x10 | idx;
        } else {
            r[1] = 0x01;
        }
        queue_reply(r, 3);
        break;
    }
    default:
        break;                                   // 'V' and unknown commands: ignore
    }

    pthread_mutex_unlock(&g_lock);
}

int portal_get_report(uint8_t out[PORTAL_REPORT_LEN])
{
    int was_reply = 0;
    pthread_mutex_lock(&g_lock);
    if (g_q_count > 0) {
        memcpy(out, g_queue[g_q_head], PORTAL_REPORT_LEN);
        g_q_head = (g_q_head + 1) % QUEUE_LEN;
        g_q_count--;
        was_reply = 1;
    } else {
        build_status(out);
    }
    pthread_mutex_unlock(&g_lock);
    return was_reply;
}

// --- PC side ---

int portal_load(int slot, const uint8_t *data, size_t len, const char *name)
{
    if (slot < 0 || slot >= PORTAL_SLOTS || !data || len < FIGURE_SIZE)
        return -1;
    pthread_mutex_lock(&g_lock);
    slot_t *s = &g_slots[slot];
    memcpy(s->data, data, FIGURE_SIZE);
    s->loaded = 1;
    s->dirty = 0;
    memset(s->name, 0, sizeof(s->name));
    if (name)
        strncpy(s->name, name, sizeof(s->name) - 1);
    // If something was already here, report "removed" first so the game re-reads it.
    if (s->status & 1) {
        s->queued_count = 0;
        s->queued[s->queued_count++] = 2;
        s->queued[s->queued_count++] = 0;
        s->queued[s->queued_count++] = 3;
        s->queued[s->queued_count++] = 1;
    } else {
        push_status(s, 3, 1);
    }
    pthread_mutex_unlock(&g_lock);
    return 0;
}

int portal_remove(int slot)
{
    if (slot < 0 || slot >= PORTAL_SLOTS)
        return -1;
    pthread_mutex_lock(&g_lock);
    slot_t *s = &g_slots[slot];
    int was_loaded = s->loaded;
    s->loaded = 0;
    if (was_loaded)
        push_status(s, 2, 0);
    pthread_mutex_unlock(&g_lock);
    return was_loaded ? 0 : -1;
}

int portal_get_dump(int slot, uint8_t out[FIGURE_SIZE])
{
    if (slot < 0 || slot >= PORTAL_SLOTS)
        return -1;
    pthread_mutex_lock(&g_lock);
    // Data stays in memory after removal so the PC can still pull the last state.
    int ok = g_slots[slot].name[0] || g_slots[slot].loaded;
    if (ok)
        memcpy(out, g_slots[slot].data, FIGURE_SIZE);
    pthread_mutex_unlock(&g_lock);
    return ok ? 0 : -1;
}

void portal_get_info(int slot, portal_slot_info *info)
{
    memset(info, 0, sizeof(*info));
    if (slot < 0 || slot >= PORTAL_SLOTS)
        return;
    pthread_mutex_lock(&g_lock);
    slot_t *s = &g_slots[slot];
    info->loaded = s->loaded;
    info->dirty = s->dirty;
    // Block 1 holds the figure id (bytes 0-1) and variant (bytes 12-13), little endian.
    info->figure_id = (uint16_t)(s->data[0x10] | (s->data[0x11] << 8));
    info->variant   = (uint16_t)(s->data[0x1C] | (s->data[0x1D] << 8));
    memcpy(info->name, s->name, sizeof(info->name));
    pthread_mutex_unlock(&g_lock);
}

int portal_is_active(void)
{
    return g_active;
}
