#include "log.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static FILE          *g_fp = NULL;
static char           g_path[256] = "";
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

bool log_enabled(void) { return g_fp != NULL; }
const char *log_path(void) { return g_path[0] ? g_path : NULL; }

bool log_open(const char *path) {
    pthread_mutex_lock(&g_mu);
    if (g_fp) { fclose(g_fp); g_fp = NULL; }
    g_path[0] = '\0';
    if (!path) {
        pthread_mutex_unlock(&g_mu);
        return true;
    }
    g_fp = fopen(path, "a");
    if (g_fp) {
        setvbuf(g_fp, NULL, _IOLBF, 0);
        size_t n = strlen(path);
        if (n >= sizeof(g_path)) n = sizeof(g_path) - 1;
        memcpy(g_path, path, n);
        g_path[n] = '\0';
    }
    bool ok = g_fp != NULL;
    pthread_mutex_unlock(&g_mu);
    return ok;
}

void log_close(void) {
    pthread_mutex_lock(&g_mu);
    if (g_fp) { fclose(g_fp); g_fp = NULL; }
    g_path[0] = '\0';
    pthread_mutex_unlock(&g_mu);
}

void log_vwritef(const char *fmt, va_list ap) {
    if (!g_fp) return;
    pthread_mutex_lock(&g_mu);
    if (!g_fp) { pthread_mutex_unlock(&g_mu); return; }

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    fprintf(g_fp, "%02d:%02d:%02d.%03ld | ",
            tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000);
    vfprintf(g_fp, fmt, ap);
    fputc('\n', g_fp);
    fflush(g_fp);
    pthread_mutex_unlock(&g_mu);
}

void log_writef(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_vwritef(fmt, ap);
    va_end(ap);
}
