/*
 * redirect_server.h — Smart Surveillance System, Step 3 (Web Server)
 *
 * Plain HTTP listener whose only job is to 301-redirect every request to
 * the HTTPS version of the same host. Runs on http_port (default 80).
 */

#ifndef REDIRECT_SERVER_H
#define REDIRECT_SERVER_H

#include "config.h"

/*
 * Runs forever (intended to be called in its own thread). Never returns
 * under normal operation; only returns (with a message on stderr) if the
 * listening socket itself could not be set up.
 */
void *redirect_server_run(void *arg /* server_config_t* */);

#endif /* REDIRECT_SERVER_H */
