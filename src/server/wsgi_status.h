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
 * Initialize the status tracking database. Should be called during
 * Apache child process initialization. The db_path parameter specifies
 * where to store the SQLite database file.
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

