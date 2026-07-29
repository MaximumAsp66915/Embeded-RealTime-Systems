/*
 * index_page.c — see index_page.h
 *
 * The page itself does very little: it embeds the MJPEG stream via a plain
 * <img> tag (browsers handle multipart/x-mixed-replace natively, no JS
 * needed for the video itself) and polls /stats.json on a timer to update
 * the numeric readouts.
 */

#include "index_page.h"

#include <stdio.h>

/* %s / %d / %d placeholders below get filled in with: student_name,
 * student_id (x2, title + header), and stats_poll_interval_ms. */
static const char *PAGE_TEMPLATE =
"<!DOCTYPE html>\n"
"<html lang=\"en\">\n"
"<head>\n"
"<meta charset=\"UTF-8\">\n"
"<title>%s - %s - Smart Surveillance System</title>\n"
"<style>\n"
"  body { font-family: -apple-system, Arial, sans-serif; background:#111; color:#eee; margin:0; padding:24px; }\n"
"  h1 { font-size: 1.3rem; font-weight: 600; margin-bottom: 4px; }\n"
"  .subtitle { color:#888; margin-bottom:20px; }\n"
"  .layout { display:flex; gap:24px; flex-wrap:wrap; }\n"
"  .stream-box { background:#000; border-radius:8px; overflow:hidden; border:1px solid #333; }\n"
"  .stream-box img { display:block; max-width:640px; width:100%%; }\n"
"  .stats { display:grid; grid-template-columns: repeat(2, minmax(140px,1fr)); gap:12px; min-width:280px; }\n"
"  .card { background:#1c1c1c; border:1px solid #333; border-radius:8px; padding:14px; }\n"
"  .card .label { color:#999; font-size:0.8rem; text-transform:uppercase; letter-spacing:0.05em; }\n"
"  .card .value { font-size:1.6rem; font-weight:700; margin-top:4px; }\n"
"  .stale { color:#e05555 !important; }\n"
"  footer { margin-top:20px; color:#555; font-size:0.75rem; }\n"
"</style>\n"
"</head>\n"
"<body>\n"
"  <h1>%s &mdash; Student ID: %s</h1>\n"
"  <div class=\"subtitle\">Smart Surveillance System &mdash; live dashboard</div>\n"
"  <div class=\"layout\">\n"
"    <div class=\"stream-box\">\n"
"      <img src=\"/stream.mjpg\" alt=\"live camera stream\">\n"
"    </div>\n"
"    <div class=\"stats\">\n"
"      <div class=\"card\"><div class=\"label\">Persons Detected</div><div class=\"value\" id=\"persons\">--</div></div>\n"
"      <div class=\"card\"><div class=\"label\">CPU Temp</div><div class=\"value\" id=\"temp\">--</div></div>\n"
"      <div class=\"card\"><div class=\"label\">Free Memory</div><div class=\"value\" id=\"mem\">--</div></div>\n"
"      <div class=\"card\"><div class=\"label\">CPU Usage</div><div class=\"value\" id=\"cpu\">--</div></div>\n"
"      <div class=\"card\"><div class=\"label\">Detector FPS</div><div class=\"value\" id=\"fps\">--</div></div>\n"
"      <div class=\"card\"><div class=\"label\">Last Update</div><div class=\"value\" id=\"updated\" style=\"font-size:0.95rem;\">--</div></div>\n"
"    </div>\n"
"  </div>\n"
"  <footer>Stats refresh every %d ms. Video updates as fast as the detector publishes frames.</footer>\n"
"\n"
"<script>\n"
"const REFRESH_MS = %d;\n"
"let missedPolls = 0;\n"
"\n"
"async function pollStats() {\n"
"  try {\n"
"    const res = await fetch('/stats.json', { cache: 'no-store' });\n"
"    if (!res.ok) throw new Error('bad status ' + res.status);\n"
"    const data = await res.json();\n"
"\n"
"    document.getElementById('persons').textContent = data.persons;\n"
"    document.getElementById('temp').textContent = (data.temp_c >= 0 ? data.temp_c.toFixed(1) + ' \\u00B0C' : 'n/a');\n"
"    document.getElementById('mem').textContent = data.mem_free_mb.toFixed(0) + ' MB';\n"
"    document.getElementById('cpu').textContent = data.cpu_percent.toFixed(1) + ' %%';\n"
"    document.getElementById('fps').textContent = data.detector_fps.toFixed(1);\n"
"    document.getElementById('updated').textContent = data.timestamp;\n"
"\n"
"    missedPolls = 0;\n"
"    document.querySelectorAll('.value').forEach(el => el.classList.remove('stale'));\n"
"  } catch (err) {\n"
"    missedPolls++;\n"
"    if (missedPolls >= 3) {\n"
"      document.querySelectorAll('.value').forEach(el => el.classList.add('stale'));\n"
"    }\n"
"    console.error('stats poll failed:', err);\n"
"  }\n"
"}\n"
"\n"
"pollStats();\n"
"setInterval(pollStats, REFRESH_MS);\n"
"</script>\n"
"</body>\n"
"</html>\n";

int index_page_render(const server_config_t *cfg, char *out, size_t out_size) {
    int written = snprintf(out, out_size, PAGE_TEMPLATE,
        cfg->student_name, cfg->student_id,   /* <title> */
        cfg->student_name, cfg->student_id,   /* <h1> */
        cfg->stats_poll_interval_ms,           /* footer text */
        cfg->stats_poll_interval_ms            /* REFRESH_MS in JS */
    );

    if (written < 0 || (size_t)written >= out_size) {
        return -1; /* buffer too small — caller should size up and retry */
    }
    return written;
}
