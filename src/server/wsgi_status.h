/* ------------------------------------------------------------------------- */

/*
 * Copyright 2007-2024 GRAHAM DUMPLETON
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* ------------------------------------------------------------------------- */

#ifndef WSGI_STATUS_H
#define WSGI_STATUS_H

#include "wsgi_apache.h"

/* ------------------------------------------------------------------------- */

/*
 * Create the status tracking database file. Should be called during
 * Apache post_config hook (in the parent process) before daemon processes
 * are forked. This ensures the database file is created with proper
 * permissions that daemon processes can access.
 */
extern int wsgi_status_create_db(apr_pool_t *pool, const char *db_path);

/*
 * Initialize the status tracking database connection. Should be called during
 * daemon process initialization. The database file should already exist
 * (created by wsgi_status_create_db in the parent process).
 */
extern int wsgi_status_init(apr_pool_t *pool, const char *db_path);

/*
 * Called when a request starts processing in a daemon worker thread.
 * Records the request in the shared database.
 */
extern void wsgi_status_request_start(
    const char *request_id,
    const char *pool_name,
    int worker_id,
    pid_t pid,
    const char *uri,
    const char *method
);

/*
 * Called when a request finishes (success or failure).
 * Removes the request from the shared database.
 */
extern void wsgi_status_request_end(const char *request_id);

/*
 * Apache handler for the /wsgi-status endpoint.
 * Returns JSON with all currently active requests across all daemon processes.
 * This handler runs in the Apache worker (embedded mode) and is always
 * available even when all WSGI daemon workers are busy.
 */
extern int wsgi_status_handler(request_rec *r);

/*
 * Cleanup function to close database connections and remove stale entries.
 * Called during process shutdown.
 */
extern void wsgi_status_cleanup(pid_t pid);

/*
 * Check if wsgi-status is enabled and this request should be handled
 * by the status endpoint.
 */
extern int wsgi_is_status_request(request_rec *r);

/* Path to the status database (set during init) */
extern const char *wsgi_status_db_path;

/* ------------------------------------------------------------------------- */

#endif

/* vi: set sw=4 expandtab : */

