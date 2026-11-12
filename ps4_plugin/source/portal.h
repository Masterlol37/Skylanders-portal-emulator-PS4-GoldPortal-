// Skylanders Portal of Power emulation core.
// Platform independent (only needs pthreads) so it can be unit tested on a PC.
#pragma once
#include <stdint.h>
#include <stddef.h>

#define PORTAL_VID 0x1430
#define PORTAL_PID 0x0150

#define PORTAL_SLOTS      8      // figures that can be on the portal at once
#define PORTAL_REPORT_LEN 32     // every HID report is 32 bytes
#define FIGURE_SIZE       1024   // MIFARE Classic 1K: 64 blocks * 16 bytes
#define FIGURE_NAME_LEN   64

typedef struct {
    int      loaded;
    int      dirty;              // game wrote to the figure since it was loaded
    uint16_t figure_id;
    uint16_t variant;
    char     name[FIGURE_NAME_LEN];
} portal_slot_info;

void portal_init(void);
// Bytes 1-2 of the reply to 'R' identify the portal model (default 02 03).
void portal_set_id(uint8_t a, uint8_t b);

// --- called from the USB hooks (game side) ---
// Host -> portal: one 32 byte command report (via SET_REPORT or interrupt OUT).
void portal_handle_command(const uint8_t *buf, size_t len);
// Portal -> host: fills a 32 byte interrupt IN report.
// Returns 1 if it was a reply to a command, 0 if it is a plain status report.
int  portal_get_report(uint8_t out[PORTAL_REPORT_LEN]);

// --- called from the HTTP server (PC side) ---
int  portal_load(int slot, const uint8_t *data, size_t len, const char *name);
int  portal_remove(int slot);
int  portal_get_dump(int slot, uint8_t out[FIGURE_SIZE]);
void portal_get_info(int slot, portal_slot_info *info);
int  portal_is_active(void);
