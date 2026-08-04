/*
 * sysinfo.c — see sysinfo.h
 */

#include "sysinfo.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/statvfs.h>

#define THERMAL_ZONE_PATH "/sys/class/thermal/thermal_zone0/temp"
#define MEMINFO_PATH "/proc/meminfo"
#define STAT_PATH "/proc/stat"
#define UPTIME_PATH "/proc/uptime"
#define LOADAVG_PATH "/proc/loadavg"

double sysinfo_read_cpu_temp_c(void) {
    FILE *f = fopen(THERMAL_ZONE_PATH, "r");
    if (!f) {
        return -1.0; /* board without this thermal zone, or running on a dev PC */
    }

    long millideg = 0;
    int matched = fscanf(f, "%ld", &millideg);
    fclose(f);

    if (matched != 1) {
        return -1.0;
    }
    return millideg / 1000.0;
}

int sysinfo_read_mem_info(mem_info_t *out) {
    FILE *f = fopen(MEMINFO_PATH, "r");
    if (!f) return -1;

    char key[64];
    long value;
    char unit[16];

    long total = -1, free_ = -1, available = -1;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%63s %ld %15s", key, &value, unit) >= 2) {
            if (strcmp(key, "MemTotal:") == 0) total = value;
            else if (strcmp(key, "MemFree:") == 0) free_ = value;
            else if (strcmp(key, "MemAvailable:") == 0) available = value;
        }
        /* Stop early once we have everything we need */
        if (total >= 0 && free_ >= 0 && available >= 0) break;
    }
    fclose(f);

    if (total < 0 || free_ < 0) return -1;

    out->total_kb = total;
    out->free_kb = free_;
    /* Older kernels may not expose MemAvailable; fall back to MemFree */
    out->available_kb = (available >= 0) ? available : free_;
    return 0;
}

/* Order of fields in /proc/stat's "cpu " line, per `man proc`:
 * user nice system idle iowait irq softirq steal guest guest_nice */
typedef struct {
    unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
} cpu_stat_t;

static int read_cpu_stat(cpu_stat_t *out) {
    FILE *f = fopen(STAT_PATH, "r");
    if (!f) return -1;

    char cpu_label[8];
    int matched = fscanf(f, "%7s %llu %llu %llu %llu %llu %llu %llu %llu",
                          cpu_label,
                          &out->user, &out->nice, &out->system, &out->idle,
                          &out->iowait, &out->irq, &out->softirq, &out->steal);
    fclose(f);

    /* label + 8 numeric fields expected */
    if (matched != 9 || strcmp(cpu_label, "cpu") != 0) return -1;
    return 0;
}

double sysinfo_read_cpu_percent(void) {
    static int have_prev = 0;
    static cpu_stat_t prev;

    cpu_stat_t cur;
    if (read_cpu_stat(&cur) != 0) {
        return -1.0;
    }

    if (!have_prev) {
        prev = cur;
        have_prev = 1;
        return 0.0; /* no delta available yet, see header note */
    }

    unsigned long long prev_idle = prev.idle + prev.iowait;
    unsigned long long cur_idle  = cur.idle + cur.iowait;

    unsigned long long prev_total = prev.user + prev.nice + prev.system +
        prev.idle + prev.iowait + prev.irq + prev.softirq + prev.steal;
    unsigned long long cur_total = cur.user + cur.nice + cur.system +
        cur.idle + cur.iowait + cur.irq + cur.softirq + cur.steal;

    unsigned long long total_delta = cur_total - prev_total;
    unsigned long long idle_delta = cur_idle - prev_idle;

    prev = cur;

    if (total_delta == 0) return 0.0;

    double percent = (double)(total_delta - idle_delta) * 100.0 / (double)total_delta;
    if (percent < 0.0) percent = 0.0;
    if (percent > 100.0) percent = 100.0;
    return percent;
}

double sysinfo_read_uptime_seconds(void) {
    FILE *f = fopen(UPTIME_PATH, "r");
    if (!f) return -1.0;

    double uptime = -1.0;
    if (fscanf(f, "%lf", &uptime) != 1) {
        uptime = -1.0;
    }
    fclose(f);
    return uptime;
}

int sysinfo_read_loadavg(double *load1, double *load5, double *load15) {
    FILE *f = fopen(LOADAVG_PATH, "r");
    if (!f) return -1;

    double l1, l5, l15;
    int matched = fscanf(f, "%lf %lf %lf", &l1, &l5, &l15);
    fclose(f);

    if (matched != 3) return -1;

    *load1 = l1;
    *load5 = l5;
    *load15 = l15;
    return 0;
}

int sysinfo_read_disk_info(const char *path, disk_info_t *out) {
    struct statvfs st;
    if (statvfs(path, &st) != 0) return -1;

    /* f_frsize is the fundamental block size; f_blocks/f_bavail are
     * counted in units of it. f_bavail (not f_bfree) is what's actually
     * available to a non-root process, matching what `df` reports. */
    double block_size = (double)st.f_frsize;
    out->total_mb = (double)st.f_blocks * block_size / (1024.0 * 1024.0);
    out->free_mb  = (double)st.f_bavail * block_size / (1024.0 * 1024.0);
    return 0;
}
