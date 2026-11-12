#include "log.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pthread.h>
#include <stdint.h>
#include <orbis/libkernel.h>

int g_trace = 0;
char g_title_id[16] = "UNKNOWN";
char g_log_path[96] = GP_LOG_FILE;
static int g_fd = -1;
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;

int file_exists(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    close(fd);
    return 1;
}

void log_init(const char *title_id)
{
    // The project used to be called "Sky Portal": move its old data folder
    // (logs, figure backups, settings files) over once. Fails harmlessly if
    // there is no old folder or the new one already exists.
    rename("/data/skyportal", GP_DATA_DIR);
    mkdir(GP_DATA_DIR, 0777);
    if (title_id && title_id[0])
        snprintf(g_title_id, sizeof(g_title_id), "%s", title_id);
    // One log per game, plus the previous run of that game.
    char prev[112];
    snprintf(g_log_path, sizeof(g_log_path), GP_DATA_DIR "/log_%s.txt", g_title_id);
    snprintf(prev, sizeof(prev), GP_DATA_DIR "/log_%s_prev.txt", g_title_id);
    rename(g_log_path, prev);
    g_fd = open(g_log_path, O_WRONLY | O_CREAT | O_TRUNC, 0777);
    if (g_fd >= 0)
        fsync(g_fd);
    if (file_exists(GP_DATA_DIR "/trace"))
        g_trace = 1;
}

void log_write(int sync, const char *fmt, ...)
{
    char buf[512];
    // Timestamp = time since the game process started (seconds.milliseconds).
    uint64_t us = sceKernelGetProcessTime();
    int t = snprintf(buf, sizeof(buf), "[%4u.%03u] ", (unsigned)(us / 1000000), (unsigned)(us / 1000 % 1000));
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + t, sizeof(buf) - 2 - t, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    n += t;
    if (n > (int)sizeof(buf) - 2) n = sizeof(buf) - 2;
    buf[n++] = '\n';

    pthread_mutex_lock(&g_log_lock);
    if (g_fd >= 0) {
        write(g_fd, buf, n);
        if (sync)
            fsync(g_fd);
    }
    pthread_mutex_unlock(&g_log_lock);
}
