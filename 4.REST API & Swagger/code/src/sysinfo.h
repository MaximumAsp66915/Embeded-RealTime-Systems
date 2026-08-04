/*
 * sysinfo.h — Smart Surveillance System, Step 3 (Web Server)
 *
 * Reads CPU temperature, memory usage, and CPU utilization DIRECTLY from
 * the kernel's /proc and /sys interfaces. Deliberately does NOT shell out
 * to commands like `vmstat`, `top`, or `cat` — the assignment specifically
 * requires this to be read "directly within the C code".
 */

#ifndef SYSINFO_H
#define SYSINFO_H

/* Returns the SoC temperature in Celsius, or -1.0 if it could not be read
 * (e.g. running this on a dev machine without a thermal_zone0). */
double sysinfo_read_cpu_temp_c(void);

/* Fills in memory stats in kilobytes, as reported by /proc/meminfo.
 * Returns 0 on success, -1 on failure. */
typedef struct {
    long total_kb;
    long free_kb;
    long available_kb; /* what most tools call "free" in the practical sense */
} mem_info_t;

int sysinfo_read_mem_info(mem_info_t *out);

/*
 * Returns overall CPU utilization as a percentage (0-100), computed from
 * the delta between the current and the previous call to /proc/stat.
 *
 * NOTE: this function is stateful (keeps the previous /proc/stat sample in
 * a static variable). The first call always returns 0.0 because there is
 * no prior sample to diff against — this is expected and documented
 * behavior, not a bug. Call it periodically (e.g. every 2s from the stats
 * endpoint) and ignore the very first reading if you want to be strict.
 */
double sysinfo_read_cpu_percent(void);

/* Seconds since boot, from /proc/uptime. Returns -1.0 on failure. */
double sysinfo_read_uptime_seconds(void);

/* 1/5/15-minute load averages, from /proc/loadavg. Returns 0 on success,
 * -1 on failure (values left unmodified). */
int sysinfo_read_loadavg(double *load1, double *load5, double *load15);

/* Disk space in megabytes for the filesystem containing `path`, via
 * statvfs() — a direct syscall, not a shelled-out `df`. Returns 0 on
 * success, -1 on failure. */
typedef struct {
    double total_mb;
    double free_mb;
} disk_info_t;

int sysinfo_read_disk_info(const char *path, disk_info_t *out);

#endif /* SYSINFO_H */
