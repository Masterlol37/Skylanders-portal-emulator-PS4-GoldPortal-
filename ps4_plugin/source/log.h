#pragma once

#define GP_DATA_DIR "/data/goldportal"
#define GP_LOG_FILE GP_DATA_DIR "/log.txt"

// Set to 1 (or create /data/goldportal/trace) to log every USB call the game makes.
extern int g_trace;

extern char g_title_id[16];     // e.g. "CUSA00024"
extern char g_log_path[96];     // current log file (/data/goldportal/log_<title>.txt)

void log_init(const char *title_id);
int  file_exists(const char *path);
// sync=1 forces the line to disk immediately (survives a console crash).
void log_write(int sync, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#define LOG(...)   log_write(1, __VA_ARGS__)
// Trace lines are synced too: they exist to find crashes. Trace mode is slower.
#define TRACE(...) do { if (g_trace) log_write(1, __VA_ARGS__); } while (0)
