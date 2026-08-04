/*
 * index_page.h — Smart Surveillance System, Step 3 (Web Server)
 *
 * Builds the single dashboard HTML page in memory (no template files on
 * disk to keep track of — everything the server needs to run lives in the
 * binary + server.conf).
 */

#ifndef INDEX_PAGE_H
#define INDEX_PAGE_H

#include <stddef.h>
#include "config.h"

/*
 * Writes the full HTML document into `out` (caller-supplied buffer).
 * Returns the number of bytes written (excluding the null terminator),
 * or -1 if `out_size` was too small.
 */
int index_page_render(const server_config_t *cfg, char *out, size_t out_size);

#endif /* INDEX_PAGE_H */
