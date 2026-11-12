// GoldHEN plugin: virtual Skylanders Portal of Power.
//
// The plugin is loaded by GoldHEN into the Skylanders game process. It
//   1. hooks libSceUsbd (Sony's libusb) so the game "sees" a portal
//      (VID 0x1430 / PID 0x0150) that does not physically exist, and
//   2. runs a mini HTTP server (port 8085) so the PC app can put .dump
//      figures on / take them off the virtual portal.
//
// Every libSceUsbd function is passed through to the original unless it
// targets the fake portal, so real USB devices keep working.
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <orbis/libkernel.h>

#include "hook_compat.h"
#include "hook.h"
#include "usbd_types.h"
#include "portal.h"
#include "http.h"
#include "log.h"
#include "audio.h"
#include <GoldHEN.h>      // sys_sdk_proc_info (title id)

attr_public const char *g_pluginName    = "goldportal";
attr_public const char *g_pluginDesc    = "Virtual Skylanders portal controlled over the network";
attr_public const char *g_pluginAuth    = "you";
attr_public uint32_t    g_pluginVersion = 0x0000010F;   // 1.15

// Minimum time between two interrupt IN reports (us). Recorded from a real PS4
// portal: its status counter advances about every 50-75 ms.
#define IN_INTERVAL_US 60000

// ---------------------------------------------------------------------------
// Fake device objects (only their addresses matter)
// ---------------------------------------------------------------------------
static char g_fake_dev_obj, g_fake_handle_obj;
#define FAKE_DEV    ((SceUsbdDevice *)&g_fake_dev_obj)
#define FAKE_HANDLE ((SceUsbdDeviceHandle *)&g_fake_handle_obj)
#define LIST_MAGIC  ((SceUsbdDevice *)0x474F4C44504F5254ULL)   // "GOLDPORT"

static const SceUsbdDeviceDescriptor g_dev_desc = {
    .bLength = 18, .bDescriptorType = 1, .bcdUSB = 0x0200,
    .bDeviceClass = 0, .bDeviceSubClass = 0, .bDeviceProtocol = 0,
    .bMaxPacketSize0 = 64, .idVendor = PORTAL_VID, .idProduct = PORTAL_PID,
    .bcdDevice = 0x0100, .iManufacturer = 1, .iProduct = 2,
    .iSerialNumber = 0, .bNumConfigurations = 1,
};

static const SceUsbdEndpointDescriptor g_endpoints[2] = {
    { .bLength = 7, .bDescriptorType = 5, .bEndpointAddress = 0x81,
      .bmAttributes = 3, .wMaxPacketSize = 32, .bInterval = 1 },
    { .bLength = 7, .bDescriptorType = 5, .bEndpointAddress = 0x02,
      .bmAttributes = 3, .wMaxPacketSize = 32, .bInterval = 1 },
};
static const SceUsbdInterfaceDescriptor g_iface_desc = {
    .bLength = 9, .bDescriptorType = 4, .bInterfaceNumber = 0,
    .bAlternateSetting = 0, .bNumEndpoints = 2, .bInterfaceClass = 3 /* HID */,
    .endpoint = g_endpoints,
};
static const SceUsbdInterface g_iface = { .altsetting = &g_iface_desc, .num_altsetting = 1 };
static const SceUsbdConfigDescriptor g_config = {
    .bLength = 9, .bDescriptorType = 2, .wTotalLength = 9 + 9 + 9 + 7 + 7,
    .bNumInterfaces = 1, .bConfigurationValue = 1, .bmAttributes = 0x80,
    .MaxPower = 50, .interface = &g_iface,
};

// ---------------------------------------------------------------------------
// Hook table
// ---------------------------------------------------------------------------
enum {
    H_GetDeviceList, H_FreeDeviceList, H_GetDeviceDescriptor,
    H_GetConfigDescriptor, H_GetActiveConfigDescriptor, H_FreeConfigDescriptor,
    H_Open, H_OpenDeviceWithVidPid, H_Close, H_RefDevice, H_UnrefDevice,
    H_GetDevice, H_GetBusNumber, H_GetDeviceAddress,
    H_ClaimInterface, H_ReleaseInterface, H_SetInterfaceAltSetting,
    H_SetConfiguration, H_GetConfiguration, H_KernelDriverActive,
    H_DetachKernelDriver, H_AttachKernelDriver, H_ClearHalt, H_ResetDevice,
    H_ControlTransfer, H_InterruptTransfer, H_BulkTransfer,
    H_SubmitTransfer, H_CancelTransfer,
    H_HandleEvents, H_HandleEventsTimeout,
    H_CheckConnected, H_GetDeviceSpeed, H_GetMaxPacketSize, H_GetMaxIsoPacketSize,
    H_GetConfigDescriptorByValue, H_GetStringDescriptorAscii, H_GetStringDescriptor,
    H_GetDescriptor, H_HandleEventsLocked, H_FreeTransfer,
    H_COUNT
};

typedef struct {
    const char *sym;
    void       *hook;
    hook_rec    rec;
    int         active;
} hook_t;

static hook_t g_hooks[H_COUNT];

#define ORIG(id, type) ((type)g_hooks[id].rec.orig)
#define HAS(id)        (g_hooks[id].active)

// Logs the first calls the game makes on the virtual portal, in order, so the
// log shows the game's exact call sequence (limited so it can't flood the log).
static int g_call_budget = 600;
#define CALL(...) do { if (g_call_budget > 0) { g_call_budget--; LOG("call: " __VA_ARGS__); } } while (0)
// Same, but each call site logs at most n times (for calls the game repeats every frame).
#define CALL_N(n, ...) do { static int _c; if (_c < (n)) { _c++; CALL(__VA_ARGS__); } } while (0)

static int  g_fake_open;
// Set only once *every* hook is installed. Newer games poll the device list in a
// tight loop; showing the virtual portal earlier let them call a not-yet-hooked
// function with the fake device, which crashed them.
static volatile int g_inject_ready;
static void (*g_FreeTransfer)(SceUsbdTransfer *);   // fallback if the hook isn't installed

// Async transfers submitted to the virtual portal that haven't completed yet.
#define MAX_PENDING 64
static pthread_mutex_t  g_pend_lock = PTHREAD_MUTEX_INITIALIZER;
static SceUsbdTransfer *g_pending[MAX_PENDING];
static int              g_pending_count;
static uint64_t         g_last_in_us;        // time of the last IN report (process time, us)

// ---------------------------------------------------------------------------
// Sniff mode (/data/goldportal/sniff): no fake portal, every call to a real
// USB device is passed through and logged, so a real portal's traffic can be
// recorded and compared with the emulation.
// ---------------------------------------------------------------------------
static int g_sniff;

static void sniff_hex(const char *tag, const uint8_t *d, int len)
{
    // Status reports ('S') arrive every few ms and only differ in the counter
    // byte (5): log them only when something else changes.
    static uint8_t last_s[32];
    static int have_last_s;
    if (len >= 7 && d && d[0] == 'S') {
        uint8_t cmp[32] = {0};
        int n = len < 32 ? len : 32;
        memcpy(cmp, d, n);
        cmp[5] = 0;
        if (have_last_s && memcmp(cmp, last_s, 32) == 0)
            return;
        memcpy(last_s, cmp, 32);
        have_last_s = 1;
    }
    char hex[3 * 64 + 1];
    int o = 0;
    for (int i = 0; d && i < len && i < 64; i++)
        o += snprintf(hex + o, sizeof(hex) - o, "%02X ", d[i]);
    hex[o] = 0;
    LOG("SNIFF %s len=%d: %s", tag, len, hex);
}

// Async transfers: remember the game's callback, log the result, then call it.
#define MAX_WRAPPED 64
static pthread_mutex_t g_wrap_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { SceUsbdTransfer *t; SceUsbdTransferCallback cb; } g_wrapped[MAX_WRAPPED];

// Interrupt OUT = speaker audio on the Trap Team portal: a continuous stream.
// Log the first 30 packets, then only every 1000th (with the running count).
static int sniff_audio_ok(void)
{
    static unsigned count;
    count++;
    if (count <= 30) return 1;
    if (count % 1000 == 0) { LOG("SNIFF audio: %u OUT packets so far", count); }
    return 0;
}

// Raw copy of the speaker stream (/data/goldportal/audio_<title>.raw, max 8 MB)
// so the audio format can be worked out on a PC.
static void sniff_audio_capture(const uint8_t *d, int len)
{
    static int fd = -2;
    static long total;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    if (!d || len <= 0) return;
    pthread_mutex_lock(&lock);
    if (fd == -2) {
        char path[96];
        snprintf(path, sizeof(path), GP_DATA_DIR "/audio_%s.raw", g_title_id);
        fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0777);
        LOG("SNIFF audio capture -> %s", path);
    }
    if (fd >= 0 && total < 8L * 1024 * 1024) {
        write(fd, d, (size_t)len);
        total += len;
        if (total >= 8L * 1024 * 1024) { fsync(fd); LOG("SNIFF audio capture full (8 MB)"); }
        else if ((total & 0xFFFF) < len) fsync(fd);   // roughly every 64 KB
    }
    pthread_mutex_unlock(&lock);
}

static void sniff_callback(SceUsbdTransfer *t)
{
    SceUsbdTransferCallback cb = NULL;
    pthread_mutex_lock(&g_wrap_lock);
    for (int i = 0; i < MAX_WRAPPED; i++)
        if (g_wrapped[i].t == t) { cb = g_wrapped[i].cb; g_wrapped[i].t = NULL; break; }
    pthread_mutex_unlock(&g_wrap_lock);

    char tag[64];
    snprintf(tag, sizeof(tag), "async done type=%d ep=0x%02X status=%d", t->type, t->endpoint, t->status);
    sniff_hex(tag, t->buffer, t->actual_length + (t->type == USBD_TRANSFER_TYPE_CONTROL ? USBD_CONTROL_SETUP_SIZE : 0));
    t->callback = cb;          // restore before handing back to the game
    if (cb) cb(t);
}

static void sniff_wrap(SceUsbdTransfer *t)
{
    if (!t || t->callback == sniff_callback) return;
    pthread_mutex_lock(&g_wrap_lock);
    for (int i = 0; i < MAX_WRAPPED; i++)
        if (!g_wrapped[i].t) {
            g_wrapped[i].t = t;
            g_wrapped[i].cb = t->callback;
            t->callback = sniff_callback;
            break;
        }
    pthread_mutex_unlock(&g_wrap_lock);
}

// ---------------------------------------------------------------------------
// Device enumeration
// ---------------------------------------------------------------------------
static ssize_t h_GetDeviceList(SceUsbdDevice ***list)
{
    SceUsbdDevice **real = NULL;
    ssize_t n = ORIG(H_GetDeviceList, ssize_t (*)(SceUsbdDevice ***))(&real);
    TRACE("GetDeviceList -> %ld", (long)n);
    if (n < 0 || g_sniff || !g_inject_ready || !HAS(H_FreeDeviceList)) {   // sniff: real devices only
        *list = real;
        return n;
    }

    // [MAGIC][real devices...][FAKE_DEV][NULL]; the game gets a pointer past MAGIC.
    SceUsbdDevice **mine = calloc((size_t)n + 3, sizeof(*mine));
    if (!mine) { *list = real; return n; }
    mine[0] = LIST_MAGIC;
    for (ssize_t i = 0; i < n; i++)
        mine[1 + i] = real[i];
    mine[1 + n] = FAKE_DEV;
    mine[2 + n] = NULL;
    // Free only the array; the references on the real devices now belong to our list.
    ORIG(H_FreeDeviceList, void (*)(SceUsbdDevice **, int))(real, 0);
    *list = mine + 1;
    CALL_N(5, "GetDeviceList -> %ld real + virtual portal", (long)n);
    return n + 1;
}

static void h_FreeDeviceList(SceUsbdDevice **list, int unref)
{
    if (list && list[-1] == LIST_MAGIC) {
        if (unref && HAS(H_UnrefDevice))
            for (SceUsbdDevice **d = list; *d; d++)
                if (*d != FAKE_DEV)
                    ORIG(H_UnrefDevice, void (*)(SceUsbdDevice *))(*d);
        CALL_N(5, "FreeDeviceList(our list, unref %d)", unref);
        free(list - 1);
        return;
    }
    ORIG(H_FreeDeviceList, void (*)(SceUsbdDevice **, int))(list, unref);
}

static int h_GetDeviceDescriptor(SceUsbdDevice *dev, SceUsbdDeviceDescriptor *desc)
{
    if (dev == FAKE_DEV) {
        CALL_N(5, "GetDeviceDescriptor(fake)");
        *desc = g_dev_desc;
        return 0;
    }
    int r = ORIG(H_GetDeviceDescriptor, int (*)(SceUsbdDevice *, SceUsbdDeviceDescriptor *))(dev, desc);
    if (g_sniff || g_trace)
        LOG("GetDeviceDescriptor(%p) -> %d  vid=%04x pid=%04x class=%d",
            (void *)dev, r, desc->idVendor, desc->idProduct, desc->bDeviceClass);
    return r;
}

static int h_GetConfigDescriptor(SceUsbdDevice *dev, uint8_t idx, const SceUsbdConfigDescriptor **cfg)
{
    if (dev == FAKE_DEV) {
        CALL("GetConfigDescriptor(fake, %d)", idx);
        if (idx != 0) return USBD_ERROR_NOT_FOUND;
        *cfg = &g_config;
        return 0;
    }
    return ORIG(H_GetConfigDescriptor, int (*)(SceUsbdDevice *, uint8_t, const SceUsbdConfigDescriptor **))(dev, idx, cfg);
}

static int h_GetActiveConfigDescriptor(SceUsbdDevice *dev, const SceUsbdConfigDescriptor **cfg)
{
    if (dev == FAKE_DEV) { CALL("GetActiveConfigDescriptor(fake)"); *cfg = &g_config; return 0; }
    return ORIG(H_GetActiveConfigDescriptor, int (*)(SceUsbdDevice *, const SceUsbdConfigDescriptor **))(dev, cfg);
}

static void h_FreeConfigDescriptor(const SceUsbdConfigDescriptor *cfg)
{
    if (cfg == &g_config) return;
    ORIG(H_FreeConfigDescriptor, void (*)(const SceUsbdConfigDescriptor *))(cfg);
}

static int h_Open(SceUsbdDevice *dev, SceUsbdDeviceHandle **handle)
{
    if (dev == FAKE_DEV) {
        CALL("Open(fake) -> 0");
        g_fake_open = 1;
        *handle = FAKE_HANDLE;
        return 0;
    }
    int r = ORIG(H_Open, int (*)(SceUsbdDevice *, SceUsbdDeviceHandle **))(dev, handle);
    if (g_sniff || g_trace)
        LOG("Open(%p) -> %d handle=%p", (void *)dev, r, handle ? (void *)*handle : NULL);
    return r;
}

static SceUsbdDeviceHandle *h_OpenDeviceWithVidPid(uint16_t vid, uint16_t pid)
{
    TRACE("OpenDeviceWithVidPid(%04x, %04x)", vid, pid);
    if (!g_sniff && g_inject_ready && vid == PORTAL_VID && pid == PORTAL_PID) {
        LOG("game opened the virtual portal (sceUsbdOpenDeviceWithVidPid)");
        g_fake_open = 1;
        return FAKE_HANDLE;
    }
    SceUsbdDeviceHandle *h = ORIG(H_OpenDeviceWithVidPid, SceUsbdDeviceHandle *(*)(uint16_t, uint16_t))(vid, pid);
    if (g_sniff)
        LOG("OpenDeviceWithVidPid(%04x, %04x) -> %p", vid, pid, (void *)h);
    return h;
}

static void h_Close(SceUsbdDeviceHandle *h)
{
    if (h == FAKE_HANDLE) {
        // Pending transfers of a closed device complete as cancelled.
        int n = 0;
        pthread_mutex_lock(&g_pend_lock);
        for (int i = 0; i < g_pending_count; i++)
            if (g_pending[i]->dev_handle == FAKE_HANDLE) { g_pending[i]->status = USBD_TRANSFER_CANCELLED; n++; }
        pthread_mutex_unlock(&g_pend_lock);
        CALL("Close(fake) - %d pending transfers cancelled", n);
        g_fake_open = 0;
        return;
    }
    ORIG(H_Close, void (*)(SceUsbdDeviceHandle *))(h);
}

static SceUsbdDevice *h_RefDevice(SceUsbdDevice *dev)
{
    if (dev == FAKE_DEV) { CALL("RefDevice(fake)"); return dev; }
    return ORIG(H_RefDevice, SceUsbdDevice *(*)(SceUsbdDevice *))(dev);
}

static void h_UnrefDevice(SceUsbdDevice *dev)
{
    if (dev == FAKE_DEV) { CALL("UnrefDevice(fake)"); return; }
    ORIG(H_UnrefDevice, void (*)(SceUsbdDevice *))(dev);
}

static SceUsbdDevice *h_GetDevice(SceUsbdDeviceHandle *h)
{
    if (h == FAKE_HANDLE) { CALL("GetDevice(fake)"); return FAKE_DEV; }
    return ORIG(H_GetDevice, SceUsbdDevice *(*)(SceUsbdDeviceHandle *))(h);
}

static uint8_t h_GetBusNumber(SceUsbdDevice *dev)
{
    if (dev == FAKE_DEV) { CALL_N(5, "GetBusNumber(fake) -> 1"); return 1; }
    return ORIG(H_GetBusNumber, uint8_t (*)(SceUsbdDevice *))(dev);
}

static uint8_t h_GetDeviceAddress(SceUsbdDevice *dev)
{
    if (dev == FAKE_DEV) { CALL_N(5, "GetDeviceAddress(fake) -> 99"); return 99; }
    return ORIG(H_GetDeviceAddress, uint8_t (*)(SceUsbdDevice *))(dev);
}

// --- functions found in the libSceUsbd export list that the game may use ---
static int h_CheckConnected(SceUsbdDeviceHandle *h)
{
    if (h == FAKE_HANDLE) { CALL_N(5, "CheckConnected(fake) -> 0"); return 0; }
    int r = ORIG(H_CheckConnected, int (*)(SceUsbdDeviceHandle *))(h);
    if (g_sniff || g_trace) LOG("CheckConnected(%p) -> 0x%x", (void *)h, r);
    return r;
}

static int h_GetDeviceSpeed(SceUsbdDevice *dev)
{
    if (dev == FAKE_DEV) { CALL("GetDeviceSpeed(fake) -> 2"); return 2; }   // full speed
    int r = ORIG(H_GetDeviceSpeed, int (*)(SceUsbdDevice *))(dev);
    if (g_sniff || g_trace) LOG("GetDeviceSpeed(%p) -> %d", (void *)dev, r);
    return r;
}

static int h_GetMaxPacketSize(SceUsbdDevice *dev, unsigned char ep)
{
    if (dev == FAKE_DEV) { CALL("GetMaxPacketSize(fake, %02X) -> 32", ep); return 32; }
    int r = ORIG(H_GetMaxPacketSize, int (*)(SceUsbdDevice *, unsigned char))(dev, ep);
    if (g_sniff || g_trace) LOG("GetMaxPacketSize(%p, %02X) -> %d", (void *)dev, ep, r);
    return r;
}

static int h_GetMaxIsoPacketSize(SceUsbdDevice *dev, unsigned char ep)
{
    if (dev == FAKE_DEV) { CALL("GetMaxIsoPacketSize(fake, %02X) -> 32", ep); return 32; }
    return ORIG(H_GetMaxIsoPacketSize, int (*)(SceUsbdDevice *, unsigned char))(dev, ep);
}

static int h_GetConfigDescriptorByValue(SceUsbdDevice *dev, uint8_t val, const SceUsbdConfigDescriptor **cfg)
{
    if (dev == FAKE_DEV) { CALL("GetConfigDescriptorByValue(fake, %d)", val); *cfg = &g_config; return 0; }
    return ORIG(H_GetConfigDescriptorByValue, int (*)(SceUsbdDevice *, uint8_t, const SceUsbdConfigDescriptor **))(dev, val, cfg);
}

// Raw descriptors, as the real portal would send them over the wire.
static const uint8_t g_raw_config[41] = {
    0x09, 0x02, 0x29, 0x00, 0x01, 0x01, 0x00, 0x80, 0xFA,   // configuration
    0x09, 0x04, 0x00, 0x00, 0x02, 0x03, 0x00, 0x00, 0x00,   // interface: HID, 2 endpoints
    0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x1D, 0x00,   // HID descriptor
    0x07, 0x05, 0x81, 0x03, 0x20, 0x00, 0x01,               // interrupt IN 0x81, 32 bytes
    0x07, 0x05, 0x02, 0x03, 0x20, 0x00, 0x01,               // interrupt OUT 0x02, 32 bytes
};

static int string_desc(uint8_t idx, unsigned char *data, int len, int ascii)
{
    const char *s = idx == 1 ? "Activision" : idx == 2 ? "Spyro Porta" : NULL;
    if (idx == 0 && !ascii) {                       // language id list: en-US
        static const uint8_t langs[4] = { 4, 3, 0x09, 0x04 };
        int n = len < 4 ? len : 4;
        memcpy(data, langs, n);
        return n;
    }
    if (!s) return USBD_ERROR_NOT_FOUND;
    int sl = (int)strlen(s);
    if (ascii) {
        int n = sl < len - 1 ? sl : len - 1;
        memcpy(data, s, n);
        data[n] = 0;
        return n;
    }
    int n = 2;                                       // UTF-16LE string descriptor
    if (len >= 2) { data[0] = (uint8_t)(2 + 2 * sl); data[1] = 3; }
    for (int i = 0; i < sl && n + 1 < len; i++) { data[n++] = (uint8_t)s[i]; data[n++] = 0; }
    return n;
}

static int h_GetStringDescriptorAscii(SceUsbdDeviceHandle *h, uint8_t idx, unsigned char *data, int len)
{
    if (h == FAKE_HANDLE) {
        int r = string_desc(idx, data, len, 1);
        CALL("GetStringDescriptorAscii(fake, %d) -> %d", idx, r);
        return r;
    }
    return ORIG(H_GetStringDescriptorAscii, int (*)(SceUsbdDeviceHandle *, uint8_t, unsigned char *, int))(h, idx, data, len);
}

static int h_GetStringDescriptor(SceUsbdDeviceHandle *h, uint8_t idx, uint16_t lang, unsigned char *data, int len)
{
    if (h == FAKE_HANDLE) {
        int r = string_desc(idx, data, len, 0);
        CALL("GetStringDescriptor(fake, %d) -> %d", idx, r);
        return r;
    }
    return ORIG(H_GetStringDescriptor, int (*)(SceUsbdDeviceHandle *, uint8_t, uint16_t, unsigned char *, int))(h, idx, lang, data, len);
}

static int h_GetDescriptor(SceUsbdDeviceHandle *h, uint8_t type, uint8_t idx, unsigned char *data, int len)
{
    if (h == FAKE_HANDLE) {
        const void *src = NULL; int sl = 0;
        if (type == 1) { src = &g_dev_desc; sl = sizeof(g_dev_desc); }
        else if (type == 2) { src = g_raw_config; sl = sizeof(g_raw_config); }
        int r;
        if (src) { r = len < sl ? len : sl; memcpy(data, src, r); }
        else if (type == 3) r = string_desc(idx, data, len, 0);
        else r = USBD_ERROR_NOT_FOUND;
        CALL("GetDescriptor(fake, type %d, idx %d) -> %d", type, idx, r);
        return r;
    }
    return ORIG(H_GetDescriptor, int (*)(SceUsbdDeviceHandle *, uint8_t, uint8_t, unsigned char *, int))(h, type, idx, data, len);
}

// Functions shaped like f(handle, int) that should simply succeed for the fake portal.
#define PASS_HANDLE_INT(name, fake_ret)                                           \
    static int h_##name(SceUsbdDeviceHandle *h, int arg)                          \
    {                                                                             \
        if (h == FAKE_HANDLE) { CALL(#name "(fake, %d) -> %d", arg, fake_ret); return fake_ret; } \
        return ORIG(H_##name, int (*)(SceUsbdDeviceHandle *, int))(h, arg);       \
    }
PASS_HANDLE_INT(ClaimInterface, 0)
PASS_HANDLE_INT(ReleaseInterface, 0)
PASS_HANDLE_INT(SetConfiguration, 0)
PASS_HANDLE_INT(KernelDriverActive, 0)
PASS_HANDLE_INT(DetachKernelDriver, 0)
PASS_HANDLE_INT(AttachKernelDriver, 0)
PASS_HANDLE_INT(ClearHalt, 0)

static int h_SetInterfaceAltSetting(SceUsbdDeviceHandle *h, int iface, int alt)
{
    if (h == FAKE_HANDLE) { CALL("SetInterfaceAltSetting(fake, %d, %d)", iface, alt); return 0; }
    return ORIG(H_SetInterfaceAltSetting, int (*)(SceUsbdDeviceHandle *, int, int))(h, iface, alt);
}

static int h_GetConfiguration(SceUsbdDeviceHandle *h, int *config)
{
    if (h == FAKE_HANDLE) { CALL("GetConfiguration(fake) -> 1"); *config = 1; return 0; }
    return ORIG(H_GetConfiguration, int (*)(SceUsbdDeviceHandle *, int *))(h, config);
}

static int h_ResetDevice(SceUsbdDeviceHandle *h)
{
    if (h == FAKE_HANDLE) { CALL("ResetDevice(fake)"); return 0; }
    return ORIG(H_ResetDevice, int (*)(SceUsbdDeviceHandle *))(h);
}

// ---------------------------------------------------------------------------
// Transfers
// ---------------------------------------------------------------------------
static void trace_report(const char *dir, const uint8_t *b, int n)
{
    if (!g_trace) return;
    char hex[3 * PORTAL_REPORT_LEN + 1];
    int o = 0;
    for (int i = 0; i < n && i < PORTAL_REPORT_LEN; i++)
        o += snprintf(hex + o, sizeof(hex) - o, "%02X ", b[i]);
    hex[o] = 0;
    LOG("%s %s", dir, hex);
}

// Handles a control transfer to the fake portal. Returns bytes transferred.
static int fake_control(uint8_t reqType, uint8_t req, uint16_t wValue,
                        unsigned char *data, uint16_t wLength)
{
    if ((reqType & 0x80) == 0) {                       // host -> device
        if (req == 0x09) {                             // HID SET_REPORT: portal command
            trace_report("CMD(ctrl)", data, wLength);
            portal_handle_command(data, wLength);
        }
        return wLength;
    }
    // device -> host
    if (req == 0x06 && (wValue >> 8) == 1) {           // GET_DESCRIPTOR(device)
        int n = wLength < sizeof(g_dev_desc) ? wLength : (int)sizeof(g_dev_desc);
        memcpy(data, &g_dev_desc, n);
        return n;
    }
    if (req == 0x01) {                                 // HID GET_REPORT
        uint8_t r[PORTAL_REPORT_LEN];
        portal_get_report(r);
        int n = wLength < PORTAL_REPORT_LEN ? wLength : PORTAL_REPORT_LEN;
        memcpy(data, r, n);
        return n;
    }
    memset(data, 0, wLength);
    return wLength;
}

// Handles an interrupt/bulk transfer to the fake portal. Returns bytes transferred.
static int fake_interrupt(uint8_t ep, unsigned char *data, int len, int *was_reply)
{
    *was_reply = 1;
    if (ep & 0x80) {                                   // IN: next report
        uint8_t r[PORTAL_REPORT_LEN];
        *was_reply = portal_get_report(r);
        int n = len < PORTAL_REPORT_LEN ? len : PORTAL_REPORT_LEN;
        memcpy(data, r, n);
        if (*was_reply) trace_report("RSP", r, n);
        return n;
    }
    // OUT on the interrupt endpoint: PS4 games send portal commands as control
    // transfers, so data here is speaker audio (Trap Team portal): play it on the
    // controller speaker. Synchronous path: wait (max ~0.5 s) until it fits.
    CALL_N(3, "interrupt OUT ep %02X, %d bytes -> controller speaker", ep, len);
    for (int i = 0; i < 250 && audio_space() < len; i++)
        usleep(2000);
    audio_push(data, len);
    return len;
}

static int h_ControlTransfer(SceUsbdDeviceHandle *h, uint8_t reqType, uint8_t req,
                             uint16_t wValue, uint16_t wIndex, unsigned char *data,
                             uint16_t wLength, unsigned int timeout)
{
    if (h == FAKE_HANDLE) {
        int r = fake_control(reqType, req, wValue, data, wLength);
        if (wLength && data[0] == 'C')     // light colour, sent every frame
            CALL_N(5, "ControlTransfer(fake) colour %02X %02X %02X", data[1], data[2], data[3]);
        else
            CALL("ControlTransfer(fake, type %02X req %02X val %04X len %d) cmd %02X %02X %02X -> %d",
                 reqType, req, wValue, wLength, wLength ? data[0] : 0,
                 wLength > 1 ? data[1] : 0, wLength > 2 ? data[2] : 0, r);
        return r;
    }
    int r = ORIG(H_ControlTransfer, int (*)(SceUsbdDeviceHandle *, uint8_t, uint8_t, uint16_t,
                 uint16_t, unsigned char *, uint16_t, unsigned int))
            (h, reqType, req, wValue, wIndex, data, wLength, timeout);
    if (g_sniff) {
        char tag[96];
        snprintf(tag, sizeof(tag), "ctrl %s type=%02X req=%02X val=%04X idx=%04X -> %d",
                 (reqType & 0x80) ? "IN " : "OUT", reqType, req, wValue, wIndex, r);
        sniff_hex(tag, data, (reqType & 0x80) ? (r > 0 ? r : 0) : wLength);
    }
    return r;
}

static int h_InterruptTransfer(SceUsbdDeviceHandle *h, unsigned char ep, unsigned char *data,
                               int len, int *transferred, unsigned int timeout)
{
    if (h == FAKE_HANDLE) {
        if (ep & 0x80) {                              // pace IN reports like the real portal
            static uint64_t last;
            uint64_t now = sceKernelGetProcessTime();
            if (now - last < IN_INTERVAL_US)
                usleep((useconds_t)(IN_INTERVAL_US - (now - last)));
            last = sceKernelGetProcessTime();
        }
        int was_reply;
        int n = fake_interrupt(ep, data, len, &was_reply);
        CALL("InterruptTransfer(fake, ep %02X, len %d) -> %d bytes, first %02X", ep, len, n, n ? data[0] : 0);
        if (transferred) *transferred = n;
        return 0;
    }
    int r = ORIG(H_InterruptTransfer, int (*)(SceUsbdDeviceHandle *, unsigned char, unsigned char *,
                 int, int *, unsigned int))(h, ep, data, len, transferred, timeout);
    if (g_sniff) {
        char tag[64];
        int n = (ep & 0x80) ? (transferred ? *transferred : 0) : len;
        snprintf(tag, sizeof(tag), "int %s ep=%02X -> %d", (ep & 0x80) ? "IN " : "OUT", ep, r);
        if (!(ep & 0x80)) sniff_audio_capture(data, n);
        if ((ep & 0x80) || sniff_audio_ok())
            sniff_hex(tag, data, n);
    }
    return r;
}

static int h_BulkTransfer(SceUsbdDeviceHandle *h, unsigned char ep, unsigned char *data,
                          int len, int *transferred, unsigned int timeout)
{
    if (h == FAKE_HANDLE)
        return h_InterruptTransfer(h, ep, data, len, transferred, timeout);
    return ORIG(H_BulkTransfer, int (*)(SceUsbdDeviceHandle *, unsigned char, unsigned char *,
                int, int *, unsigned int))(h, ep, data, len, transferred, timeout);
}

// --- asynchronous transfers: completed from inside the game's HandleEvents call ---

static int h_SubmitTransfer(SceUsbdTransfer *t)
{
    if (!t || t->dev_handle != FAKE_HANDLE) {
        int audio = t && t->type != USBD_TRANSFER_TYPE_CONTROL && !(t->endpoint & 0x80);
        if (g_sniff && audio) sniff_audio_capture(t->buffer, t->length);
        if (g_sniff && t && (!audio || sniff_audio_ok())) {
            char tag[64];
            snprintf(tag, sizeof(tag), "async submit type=%d ep=0x%02X len=%d", t->type, t->endpoint, t->length);
            int out = t->type == USBD_TRANSFER_TYPE_CONTROL ? !(t->buffer[0] & 0x80) : !(t->endpoint & 0x80);
            sniff_hex(tag, t->buffer, out ? t->length : (t->type == USBD_TRANSFER_TYPE_CONTROL ? USBD_CONTROL_SETUP_SIZE : 0));
            if (!audio)
                sniff_wrap(t);
        }
        int r = ORIG(H_SubmitTransfer, int (*)(SceUsbdTransfer *))(t);
        if (r < 0 && t && t->callback == sniff_callback) {   // not submitted: undo the wrap
            pthread_mutex_lock(&g_wrap_lock);
            for (int i = 0; i < MAX_WRAPPED; i++)
                if (g_wrapped[i].t == t) { t->callback = g_wrapped[i].cb; g_wrapped[i].t = NULL; break; }
            pthread_mutex_unlock(&g_wrap_lock);
        }
        return r;
    }

    CALL_N(10, "SubmitTransfer(fake, type %d, ep %02X, len %d, flags %02X)", t->type, t->endpoint, t->length, t->flags);
    t->status = USBD_TRANSFER_COMPLETED;
    if (t->type == USBD_TRANSFER_TYPE_CONTROL) {
        // buffer = 8 byte setup packet followed by data
        unsigned char *s = t->buffer;
        uint16_t wValue  = (uint16_t)(s[2] | (s[3] << 8));
        uint16_t wLength = (uint16_t)(s[6] | (s[7] << 8));
        t->actual_length = fake_control(s[0], s[1], wValue, s + USBD_CONTROL_SETUP_SIZE, wLength);
    }
    // IN transfers are filled when they complete, so they get the freshest report;
    // OUT (speaker audio) is handed to the speaker when it completes, paced by the buffer.

    pthread_mutex_lock(&g_pend_lock);
    int ok = g_pending_count < MAX_PENDING;
    if (ok) g_pending[g_pending_count++] = t;
    pthread_mutex_unlock(&g_pend_lock);
    return ok ? 0 : USBD_ERROR_NOT_SUPPORTED;
}

static int h_CancelTransfer(SceUsbdTransfer *t)
{
    if (!t || t->dev_handle != FAKE_HANDLE)
        return ORIG(H_CancelTransfer, int (*)(SceUsbdTransfer *))(t);
    int found = 0;
    pthread_mutex_lock(&g_pend_lock);
    for (int i = 0; i < g_pending_count; i++)
        if (g_pending[i] == t) { t->status = USBD_TRANSFER_CANCELLED; found = 1; }
    pthread_mutex_unlock(&g_pend_lock);
    CALL_N(20, "CancelTransfer(fake, ep %02X) -> %s", t->endpoint, found ? "cancelled" : "not pending");
    return found ? 0 : USBD_ERROR_NOT_FOUND;
}

// Removes a transfer from the pending list (it is being freed or its device closed).
static int forget_transfer(SceUsbdTransfer *t)
{
    int found = 0;
    pthread_mutex_lock(&g_pend_lock);
    int keep = 0;
    for (int i = 0; i < g_pending_count; i++) {
        if (g_pending[i] == t) { found = 1; continue; }
        g_pending[keep++] = g_pending[i];
    }
    g_pending_count = keep;
    pthread_mutex_unlock(&g_pend_lock);
    return found;
}

// The game may free a transfer we still hold as pending; drop it first so we never
// call back into freed memory.
static void h_FreeTransfer(SceUsbdTransfer *t)
{
    if (t && t->dev_handle == FAKE_HANDLE) {
        int was_pending = forget_transfer(t);
        CALL_N(20, "FreeTransfer(fake, ep %02X)%s", t->endpoint, was_pending ? " - was still pending, dropped" : "");
    }
    ORIG(H_FreeTransfer, void (*)(SceUsbdTransfer *))(t);
}

static void free_transfer(SceUsbdTransfer *t)
{
    if (HAS(H_FreeTransfer)) ORIG(H_FreeTransfer, void (*)(SceUsbdTransfer *))(t);
    else if (g_FreeTransfer) g_FreeTransfer(t);
}

static int is_in_transfer(const SceUsbdTransfer *t)
{
    return t->status == USBD_TRANSFER_COMPLETED &&
           t->type != USBD_TRANSFER_TYPE_CONTROL && (t->endpoint & 0x80);
}

static int is_audio_transfer(const SceUsbdTransfer *t)
{
    return t->status == USBD_TRANSFER_COMPLETED &&
           t->type != USBD_TRANSFER_TYPE_CONTROL && !(t->endpoint & 0x80);
}

// Completes queued fake transfers. Control/OUT/cancelled ones finish right away;
// IN reports are paced like the real portal (one per IN_INTERVAL_US), because
// the game only looks at the latest report each frame - faster reports made it
// miss the replies to its commands. Returns how many callbacks ran.
static int dispatch_fake_transfers(void)
{
    SceUsbdTransfer *batch[MAX_PENDING];
    int n = 0, took_in = 0;
    pthread_mutex_lock(&g_pend_lock);
    uint64_t now = sceKernelGetProcessTime();
    int allow_in = now - g_last_in_us >= IN_INTERVAL_US;
    int keep = 0;
    int audio_room = audio_space();
    for (int i = 0; i < g_pending_count; i++) {
        SceUsbdTransfer *t = g_pending[i];
        int in = is_in_transfer(t);
        int audio = is_audio_transfer(t);
        // Speaker packets complete only when the speaker buffer has room for them
        // (keeps their order: once one waits, the later ones wait too).
        if (audio && t->length > audio_room) { audio_room = 0; g_pending[keep++] = t; continue; }
        if (audio) audio_room -= t->length;
        if (!in || (allow_in && !took_in)) {
            batch[n++] = t;
            if (in) took_in = 1;
        } else {
            g_pending[keep++] = t;
        }
    }
    g_pending_count = keep;
    if (took_in) g_last_in_us = now;
    pthread_mutex_unlock(&g_pend_lock);

    for (int i = 0; i < n; i++) {
        SceUsbdTransfer *t = batch[i];
        if (is_in_transfer(t)) {
            int was_reply;
            t->actual_length = fake_interrupt(t->endpoint, t->buffer, t->length, &was_reply);
            if (was_reply)
                CALL("reply delivered to game: %02X %02X %02X %02X", t->buffer[0], t->buffer[1], t->buffer[2], t->buffer[3]);
        } else if (is_audio_transfer(t)) {
            CALL_N(3, "speaker packet %d bytes -> controller speaker", t->length);
            audio_push(t->buffer, t->length);
            t->actual_length = t->length;
        }
        if (t->status == USBD_TRANSFER_CANCELLED) t->actual_length = 0;
        int free_after = (t->flags & USBD_TRANSFER_FREE_TRANSFER) != 0;
        if (t->callback) t->callback(t);   // callback may resubmit, which is fine
        if (free_after) free_transfer(t);
    }
    return n;
}

// Time of the game's last HandleEvents call: callbacks are delivered on the game's
// own event thread whenever it is calling HandleEvents.
static volatile uint64_t g_last_he_us;

static int pending_fake(void)
{
    pthread_mutex_lock(&g_pend_lock);
    int n = g_pending_count;
    pthread_mutex_unlock(&g_pend_lock);
    return n;
}

// Fallback only: completes fake transfers if the game hasn't called any hooked
// HandleEvents variant for 100 ms (running callbacks on a foreign thread is a
// last resort, the game's own event thread is preferred).
static volatile int g_completer_stop;
static void *completion_thread(void *arg)
{
    (void)arg;
    int announced = 0;
    while (!g_completer_stop) {
        usleep(2000);
        if (!pending_fake()) continue;
        if (sceKernelGetProcessTime() - g_last_he_us < 100000) continue;
        if (!announced) { CALL("game is not calling HandleEvents - completing transfers from plugin thread"); announced = 1; }
        dispatch_fake_transfers();
    }
    return NULL;
}

static void note_handle_events(const char *which)
{
    g_last_he_us = sceKernelGetProcessTime();
    static const char *seen[4];
    for (int i = 0; i < 4; i++) {
        if (seen[i] == which) return;
        if (!seen[i]) { seen[i] = which; CALL("game uses %s for USB events", which); return; }
    }
}

static int h_HandleEventsTimeout(struct timeval *tv)
{
    note_handle_events("HandleEventsTimeout");
    if (dispatch_fake_transfers() > 0)
        return 0;
    // Don't let the real event loop sleep for long while our device may need service.
    struct timeval cap = { 0, 2000 };
    if (g_fake_open && (!tv || tv->tv_sec > 0 || tv->tv_usec > cap.tv_usec))
        tv = &cap;
    int r = ORIG(H_HandleEventsTimeout, int (*)(struct timeval *))(tv);
    if (pending_fake()) dispatch_fake_transfers();
    return r;
}

static int h_HandleEventsLocked(struct timeval *tv)
{
    note_handle_events("HandleEventsLocked");
    if (dispatch_fake_transfers() > 0)
        return 0;
    return ORIG(H_HandleEventsLocked, int (*)(struct timeval *))(tv);
}

static int h_HandleEvents(void)
{
    note_handle_events("HandleEvents");
    if (dispatch_fake_transfers() > 0)
        return 0;
    if (g_fake_open && HAS(H_HandleEventsTimeout)) {
        struct timeval cap = { 0, 2000 };
        int r = ORIG(H_HandleEventsTimeout, int (*)(struct timeval *))(&cap);
        if (pending_fake()) dispatch_fake_transfers();
        return r;
    }
    return ORIG(H_HandleEvents, int (*)(void))();
}

// ---------------------------------------------------------------------------
// Plugin entry points
// ---------------------------------------------------------------------------
#define DEF(id, name) [id] = { "sceUsbd" #name, (void *)h_##name }
static void fill_table(void)
{
    hook_t t[H_COUNT] = {
        DEF(H_GetDeviceList, GetDeviceList),           DEF(H_FreeDeviceList, FreeDeviceList),
        DEF(H_GetDeviceDescriptor, GetDeviceDescriptor),
        DEF(H_GetConfigDescriptor, GetConfigDescriptor),
        DEF(H_GetActiveConfigDescriptor, GetActiveConfigDescriptor),
        DEF(H_FreeConfigDescriptor, FreeConfigDescriptor),
        DEF(H_Open, Open),                             DEF(H_OpenDeviceWithVidPid, OpenDeviceWithVidPid),
        DEF(H_Close, Close),                           DEF(H_RefDevice, RefDevice),
        DEF(H_UnrefDevice, UnrefDevice),               DEF(H_GetDevice, GetDevice),
        DEF(H_GetBusNumber, GetBusNumber),             DEF(H_GetDeviceAddress, GetDeviceAddress),
        DEF(H_ClaimInterface, ClaimInterface),         DEF(H_ReleaseInterface, ReleaseInterface),
        DEF(H_SetInterfaceAltSetting, SetInterfaceAltSetting),
        DEF(H_SetConfiguration, SetConfiguration),     DEF(H_GetConfiguration, GetConfiguration),
        DEF(H_KernelDriverActive, KernelDriverActive), DEF(H_DetachKernelDriver, DetachKernelDriver),
        DEF(H_AttachKernelDriver, AttachKernelDriver), DEF(H_ClearHalt, ClearHalt),
        DEF(H_ResetDevice, ResetDevice),
        DEF(H_ControlTransfer, ControlTransfer),       DEF(H_InterruptTransfer, InterruptTransfer),
        DEF(H_BulkTransfer, BulkTransfer),
        DEF(H_SubmitTransfer, SubmitTransfer),         DEF(H_CancelTransfer, CancelTransfer),
        DEF(H_HandleEvents, HandleEvents),             DEF(H_HandleEventsTimeout, HandleEventsTimeout),
        DEF(H_CheckConnected, CheckConnected),         DEF(H_GetDeviceSpeed, GetDeviceSpeed),
        DEF(H_GetMaxPacketSize, GetMaxPacketSize),     DEF(H_GetMaxIsoPacketSize, GetMaxIsoPacketSize),
        DEF(H_GetConfigDescriptorByValue, GetConfigDescriptorByValue),
        DEF(H_GetStringDescriptorAscii, GetStringDescriptorAscii),
        DEF(H_GetStringDescriptor, GetStringDescriptor),
        DEF(H_GetDescriptor, GetDescriptor),           DEF(H_HandleEventsLocked, HandleEventsLocked),
        DEF(H_FreeTransfer, FreeTransfer),
    };
    memcpy(g_hooks, t, sizeof(t));
}

// ---------------------------------------------------------------------------
// Crash guard: hooking.txt names the hook being installed right now. If it is
// still there on the next start, that hook crashed the console and is added to
// skip.txt (one symbol per line, editable by hand) so it is never tried again.
// ---------------------------------------------------------------------------
// Per game: hooking_<title>.txt / skip_<title>.txt
static char GUARD_FILE[96], SKIP_FILE[96];
static char g_skip[2048];

static int read_text(const char *path, char *buf, size_t size)
{
    buf[0] = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, size - 1);
    close(fd);
    buf[n > 0 ? n : 0] = 0;
    return (int)(n > 0 ? n : 0);
}

static void write_text(const char *path, const char *text, int append)
{
    int fd = open(path, O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC), 0777);
    if (fd < 0) return;
    write(fd, text, strlen(text));
    fsync(fd);
    close(fd);
}

static int is_skipped(const char *sym)
{
    size_t n = strlen(sym);
    for (const char *p = g_skip; (p = strstr(p, sym)) != NULL; p += n) {
        int start_ok = p == g_skip || p[-1] == '\n' || p[-1] == '\r';
        int end_ok = p[n] == 0 || p[n] == '\n' || p[n] == '\r';
        if (start_ok && end_ok) return 1;
    }
    return 0;
}

static void guard_check_previous_crash(void)
{
    char sym[128];
    if (read_text(GUARD_FILE, sym, sizeof(sym)) > 0) {
        LOG("!! previous start crashed while hooking %s -> added to skip.txt", sym);
        char line[140];
        snprintf(line, sizeof(line), "%s\n", sym);
        write_text(SKIP_FILE, line, 1);
    }
    unlink(GUARD_FILE);
    read_text(SKIP_FILE, g_skip, sizeof(g_skip));
}

// Installs one hook, protected by the crash guard. Returns 1 if hooked.
static int guarded_install(const char *sym, hook_rec *rec, void *addr, void *fn)
{
    if (is_skipped(sym)) {
        LOG("  %s listed in skip.txt, not hooked", sym);
        return 0;
    }
    LOG("  hooking %s ...", sym);
    write_text(GUARD_FILE, sym, 0);
    const char *why;
    int ok = hook_install(rec, addr, fn, &why) == 0;
    unlink(GUARD_FILE);
    if (ok) LOG("    ok (patched %p, %zu bytes)", (void *)rec->target, rec->patched);
    else    LOG("    SKIPPED: %s", why);
    return ok;
}

// Finds an already loaded module by file name. Optionally logs all modules.
static int find_module(const char *name, int log_all)
{
    OrbisKernelModule mods[256];
    size_t n = 0;
    if (sceKernelGetModuleList(mods, 256, &n) != 0)
        return -1;
    if (n > 256) n = 256;
    int found = -1;
    for (size_t i = 0; i < n; i++) {
        static OrbisKernelModuleInfo info;
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelGetModuleInfo(mods[i], &info) != 0)
            continue;
        if (log_all)
            LOG("  module 0x%x %s", mods[i], info.name);
        if (name && strcmp(info.name, name) == 0)
            found = (int)mods[i];
    }
    return found;
}

static pthread_mutex_t g_usbd_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_usbd_done;

// Hooks libSceUsbd once the game itself has loaded it (we never load it ourselves:
// forcing it into the game crashed it).
static void try_hook_usbd(const char *who)
{
    pthread_mutex_lock(&g_usbd_lock);
    if (g_usbd_done) { pthread_mutex_unlock(&g_usbd_lock); return; }
    int mod = find_module("libSceUsbd.sprx", 0);
    if (mod < 0) { pthread_mutex_unlock(&g_usbd_lock); return; }
    g_usbd_done = 1;
    LOG("libSceUsbd is loaded (handle 0x%x, noticed by %s)", mod, who);

    // Resolve everything first: hooks call each other's originals.
    void *addr[H_COUNT] = {0};
    for (int i = 0; i < H_COUNT; i++)
        if (sceKernelDlsym(mod, g_hooks[i].sym, &addr[i]) != 0)
            addr[i] = NULL;
    void *ft = NULL;
    if (sceKernelDlsym(mod, "sceUsbdFreeTransfer", &ft) == 0)
        g_FreeTransfer = (void (*)(SceUsbdTransfer *))ft;
    for (int i = 0; i < H_COUNT; i++) {
        if (!addr[i]) { LOG("  %-36s not found", g_hooks[i].sym); continue; }
        const uint8_t *b = addr[i];
        LOG("  %-36s at %p: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
            g_hooks[i].sym, addr[i], b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
            b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    }
    for (int i = 0; i < H_COUNT; i++)
        if (addr[i] && guarded_install(g_hooks[i].sym, &g_hooks[i].rec, addr[i], g_hooks[i].hook))
            g_hooks[i].active = 1;
    LOG("libSceUsbd hooking finished");
    __sync_synchronize();
    g_inject_ready = 1;            // only now may the game see the virtual portal
    LOG("virtual portal is now visible to the game");
    if (!g_sniff) {
        pthread_t th;
        if (pthread_create(&th, NULL, completion_thread, NULL) == 0)
            pthread_detach(th);
    }
    pthread_mutex_unlock(&g_usbd_lock);
}

// Polls the module list until the game has loaded libSceUsbd, then hooks it.
// Logs every module that appears after startup (shows what the game loads for the portal).
static OrbisKernelModule g_seen[256];
static size_t g_seen_count;
static int g_stop;

static void log_new_modules(int quiet)
{
    OrbisKernelModule mods[256];
    size_t n = 0;
    if (sceKernelGetModuleList(mods, 256, &n) != 0) return;
    if (n > 256) n = 256;
    for (size_t i = 0; i < n; i++) {
        int known = 0;
        for (size_t j = 0; j < g_seen_count; j++)
            if (g_seen[j] == mods[i]) { known = 1; break; }
        if (known || g_seen_count >= 256) continue;
        g_seen[g_seen_count++] = mods[i];
        if (quiet) continue;
        static OrbisKernelModuleInfo info;
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelGetModuleInfo(mods[i], &info) == 0)
            LOG("NEW MODULE loaded by the game: 0x%x %s", mods[i], info.name);
    }
}

static void *usbd_watch_thread(void *arg)
{
    (void)arg;
    log_new_modules(1);                                          // remember the startup set
    for (int i = 0; i < 50 * 60 * 10 && !g_stop; i++) {          // every 20 ms, up to 10 minutes
        log_new_modules(0);
        if (!g_usbd_done)
            try_hook_usbd("watcher");
        usleep(20000);
    }
    return NULL;
}

static void notify(const char *fmt, ...)
{
    static OrbisNotificationRequest req;
    memset(&req, 0, sizeof(req));
    req.targetId = -1;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(req.message, sizeof(req.message), fmt, ap);
    va_end(ap);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

// All real work happens here, on our own thread with a big stack, so that
// plugin_load returns immediately and doesn't hold up the game's startup.
static void *worker_thread(void *arg)
{
    (void)arg;
    static struct proc_info pi;
    memset(&pi, 0, sizeof(pi));
    int pr = sys_sdk_proc_info(&pi);
    log_init(pr == 0 ? pi.titleid : "UNKNOWN");
    LOG("goldportal %d.%02d starting (worker thread) - game %s (proc_info %d, name %s, version %.6s)",
        g_pluginVersion >> 8 & 0xFF, g_pluginVersion & 0xFF, g_title_id, pr, pi.name, pi.version);
    snprintf(GUARD_FILE, sizeof(GUARD_FILE), GP_DATA_DIR "/hooking_%s.txt", g_title_id);
    snprintf(SKIP_FILE, sizeof(SKIP_FILE), GP_DATA_DIR "/skip_%s.txt", g_title_id);
    portal_init();
    fill_table();

    // Portal model reported to the game ('R' reply), overridable per game with
    // /data/goldportal/portal_id_<title>.txt containing two hex bytes, e.g. "02 03".
    {
        // Recorded from real portals: Swap Force portal answers 02 03 (works for Swap Force,
        // SuperChargers, Imaginators); Trap Team needs its own portal: 02 1B.
        static const struct { const char *title; uint8_t a, b; } defaults[] = {
            { "CUSA00697", 0x02, 0x1B },   // Skylanders Trap Team
        };
        uint8_t da = 0x02, db = 0x03;
        for (size_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++)
            if (strcmp(g_title_id, defaults[i].title) == 0) { da = defaults[i].a; db = defaults[i].b; }
        portal_set_id(da, db);

        char path[112], txt[32];
        snprintf(path, sizeof(path), GP_DATA_DIR "/portal_id_%s.txt", g_title_id);
        unsigned a, b;
        if (read_text(path, txt, sizeof(txt)) > 0 && sscanf(txt, "%x %x", &a, &b) == 2) {
            portal_set_id((uint8_t)a, (uint8_t)b);
            LOG("portal id from %s: %02X %02X", path, a, b);
        } else {
            LOG("portal id: %02X %02X (create %s to change)", da, db, path);
        }
    }

    LOG("step: starting web server");
    if (http_server_start(GP_HTTP_PORT) != 0)
        LOG("ERROR: could not start HTTP server");

    LOG("step: modules loaded in the game right now:");
    find_module(NULL, 1);
    LOG("step: module list done");

    if (file_exists(GP_DATA_DIR "/nohook")) {
        LOG("SAFE MODE (/data/goldportal/nohook exists): no hooks installed");
        return NULL;
    }
    LOG("step: crash guard check");
    guard_check_previous_crash();
    if (g_skip[0])
        notify("Goldportal: skipping hooks that crashed before (see skip.txt)");

    {
        char sp[96];
        snprintf(sp, sizeof(sp), GP_DATA_DIR "/sniff_%s", g_title_id);   // per game
        g_sniff = file_exists(GP_DATA_DIR "/sniff") || file_exists(sp);
    }
    if (g_sniff) {
        LOG("SNIFF MODE (/data/goldportal/sniff exists): no virtual portal, real USB traffic is logged");
        notify("Goldportal: sniff mode - plug in your real portal");
    }

    LOG("step: waiting for the game to load libSceUsbd (new modules are logged)");
    usbd_watch_thread(NULL);
    if (!g_usbd_done)
        LOG("libSceUsbd was never loaded by the game (gave up waiting)");
    return NULL;
}

int32_t attr_public plugin_load(int32_t argc, const char *argv[])
{
    (void)argc; (void)argv;
    notify("Goldportal %d.%02d loading", g_pluginVersion >> 8 & 0xFF, g_pluginVersion & 0xFF);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 512 * 1024);
    pthread_t th;
    if (pthread_create(&th, &attr, worker_thread, NULL) == 0)
        pthread_detach(th);
    else
        notify("Goldportal: could not start worker thread");
    pthread_attr_destroy(&attr);
    return 0;
}

void gp_get_state(gp_state *s)
{
    s->version = g_pluginVersion;
    s->usbd_loaded = g_usbd_done && !g_stop;
    s->hooks_ok = 0;
    for (int i = 0; i < H_COUNT; i++)
        s->hooks_ok += g_hooks[i].active;
    s->portal_opened = g_fake_open;
    s->sniff = g_sniff;
}

int32_t attr_public plugin_unload(int32_t argc, const char *argv[])
{
    (void)argc; (void)argv;
    g_usbd_done = 1;
    g_stop = 1;        // stops the watcher thread
    g_completer_stop = 1;
    for (int i = 0; i < H_COUNT; i++)
        if (g_hooks[i].active) { g_hooks[i].active = 0; hook_remove(&g_hooks[i].rec); }
    LOG("goldportal unloaded");
    return 0;
}

int32_t attr_module_hidden module_start(int64_t argc, const void *args)
{
    (void)argc; (void)args;
    return 0;
}

int32_t attr_module_hidden module_stop(int64_t argc, const void *args)
{
    (void)argc; (void)args;
    return 0;
}
