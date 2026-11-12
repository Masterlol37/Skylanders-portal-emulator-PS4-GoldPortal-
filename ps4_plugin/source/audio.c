#include "audio.h"
#include "log.h"
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <orbis/libkernel.h>

#define IN_RATE     8000
#define OUT_RATE    48000
#define OUT_GRAIN   256                    // samples per sceAudioOutOutput call
#define RING_SAMPLES (IN_RATE / 4)         // 250 ms of buffered speaker audio
#define PORT_PADSPK 4                      // SCE_AUDIO_OUT_PORT_TYPE_PADSPK
#define PORT_MAIN   0
#define FMT_S16_MONO 0
#define GAIN        2                      // portal audio peaks at ~8191: make it audible

static int16_t g_ring[RING_SAMPLES];
static int g_head, g_count;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_started, g_failed;

static int (*p_AudioOutOpen)(int32_t, int32_t, int32_t, uint32_t, uint32_t, uint32_t);
static int (*p_AudioOutOutput)(int32_t, const void *);
static int (*p_GetInitialUser)(int32_t *);

// Finds a loaded module by name (the game has already loaded both libraries).
static int module_handle(const char *name)
{
    OrbisKernelModule mods[256];
    size_t n = 0;
    if (sceKernelGetModuleList(mods, 256, &n) != 0) return -1;
    if (n > 256) n = 256;
    for (size_t i = 0; i < n; i++) {
        static OrbisKernelModuleInfo info;
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelGetModuleInfo(mods[i], &info) == 0 && strcmp(info.name, name) == 0)
            return (int)mods[i];
    }
    return -1;
}

static int pop_sample(int16_t *s)
{
    if (g_count == 0) return 0;
    *s = g_ring[g_head];
    g_head = (g_head + 1) % RING_SAMPLES;
    g_count--;
    return 1;
}

static void *audio_thread(void *arg)
{
    (void)arg;
    int32_t user = -1;
    if (p_GetInitialUser) p_GetInitialUser(&user);
    int h = p_AudioOutOpen(user, PORT_PADSPK, 0, OUT_GRAIN, OUT_RATE, FMT_S16_MONO);
    if (h < 0) {
        LOG("audio: controller speaker not available (0x%x, user 0x%x) - using TV output", h, user);
        h = p_AudioOutOpen(user, PORT_MAIN, 0, OUT_GRAIN, OUT_RATE, FMT_S16_MONO);
    }
    if (h < 0) {
        LOG("audio: sceAudioOutOpen failed (0x%x) - portal speaker disabled", h);
        g_failed = 1;
        return NULL;
    }
    LOG("audio: portal speaker -> controller speaker (port handle 0x%x, user 0x%x)", h, user);

    static int16_t out[OUT_GRAIN];
    int16_t cur = 0, next = 0;
    int phase = 0;                         // 0..5: position between cur and next (8k -> 48k = x6)
    for (;;) {
        pthread_mutex_lock(&g_lock);
        for (int i = 0; i < OUT_GRAIN; i++) {
            if (phase == 0) {
                cur = next;
                if (!pop_sample(&next)) next = 0;   // buffer empty: fade to silence
            }
            int v = (cur * (6 - phase) + next * phase) / 6 * GAIN;
            out[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
            phase = (phase + 1) % 6;
        }
        pthread_mutex_unlock(&g_lock);
        p_AudioOutOutput(h, out);          // blocks until the hardware wants more: real-time pacing
    }
    return NULL;
}

static void start_audio(void)
{
    g_started = 1;
    int ao = module_handle("libSceAudioOut.sprx");
    int us = module_handle("libSceUserService.sprx");
    void *a = NULL, *b = NULL, *c = NULL;
    if (ao >= 0) { sceKernelDlsym(ao, "sceAudioOutOpen", &a); sceKernelDlsym(ao, "sceAudioOutOutput", &b); }
    if (us >= 0) sceKernelDlsym(us, "sceUserServiceGetInitialUser", &c);
    p_AudioOutOpen = (int (*)(int32_t, int32_t, int32_t, uint32_t, uint32_t, uint32_t))a;
    p_AudioOutOutput = (int (*)(int32_t, const void *))b;
    p_GetInitialUser = (int (*)(int32_t *))c;
    if (!p_AudioOutOpen || !p_AudioOutOutput) {
        LOG("audio: libSceAudioOut functions not found - portal speaker disabled");
        g_failed = 1;
        return;
    }
    pthread_t th;
    if (pthread_create(&th, NULL, audio_thread, NULL) == 0)
        pthread_detach(th);
    else
        g_failed = 1;
}

int audio_space(void)
{
    if (g_failed) return 1 << 20;          // no output: accept and drop everything
    pthread_mutex_lock(&g_lock);
    int free_bytes = (RING_SAMPLES - g_count) * 2;
    pthread_mutex_unlock(&g_lock);
    return free_bytes;
}

int audio_push(const uint8_t *data, int len)
{
    if (!data || len <= 0) return 0;
    if (!g_started) start_audio();
    if (g_failed) return len;
    int samples = len / 2;
    pthread_mutex_lock(&g_lock);
    int taken = 0;
    for (; taken < samples && g_count < RING_SAMPLES; taken++) {
        int16_t s = (int16_t)(data[2 * taken] | (data[2 * taken + 1] << 8));
        g_ring[(g_head + g_count) % RING_SAMPLES] = s;
        g_count++;
    }
    pthread_mutex_unlock(&g_lock);
    return taken * 2;
}
