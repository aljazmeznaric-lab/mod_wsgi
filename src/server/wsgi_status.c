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

#include "wsgi_status.h"

#include <sqlite3.h>
#include <unistd.h>

/* ------------------------------------------------------------------------- */

/* Path to the status database */
const char *wsgi_status_db_path = NULL;

/* Database connection for this process (for writing) */
static sqlite3 *wsgi_status_db = NULL;

/* Prepared statements for efficiency */
static sqlite3_stmt *wsgi_status_insert_stmt = NULL;
static sqlite3_stmt *wsgi_status_delete_stmt = NULL;

/* Thread mutex for database access */
static apr_thread_mutex_t *wsgi_status_mutex = NULL;

/* ------------------------------------------------------------------------- */

/*
 * Cleanup callback registered with APR pool to handle process shutdown.
 */
static apr_status_t wsgi_status_pool_cleanup(void *data)
{
    pid_t pid = getpid();
    
    wsgi_status_cleanup(pid);
    
    return APR_SUCCESS;
}

/* ------------------------------------------------------------------------- */

int wsgi_status_init(apr_pool_t *pool, const char *db_path)
{
    int rc;
    char *errmsg = NULL;
    
    if (!db_path || !*db_path) {
        return -1;
    }
    
    wsgi_status_db_path = apr_pstrdup(pool, db_path);
    
    /* Open database connection */
    rc = sqlite3_open(db_path, &wsgi_status_db);
    if (rc != SQLITE_OK) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, NULL,
                     "mod_wsgi (pid=%d): Failed to open status database '%s': %s",
                     getpid(), db_path, sqlite3_errmsg(wsgi_status_db));
        return -1;
    }
    
    /* Enable WAL mode for concurrent access from multiple processes */
    rc = sqlite3_exec(wsgi_status_db, "PRAGMA journal_mode=WAL", NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        ap_log_error(APLOG_MARK, APLOG_WARNING, 0, NULL,
                     "mod_wsgi (pid=%d): Failed to enable WAL mode: %s",
                     getpid(), errmsg);
        sqlite3_free(errmsg);
    }
    
    /* Set synchronous mode to NORMAL for better performance */
    sqlite3_exec(wsgi_status_db, "PRAGMA synchronous=NORMAL", NULL, NULL, NULL);
    
    /* Set busy timeout to avoid lock contention issues */
    sqlite3_busy_timeout(wsgi_status_db, 5000);  /* 5 second timeout */
    
    /* Create table if it doesn't exist */
    rc = sqlite3_exec(wsgi_status_db,
        "CREATE TABLE IF NOT EXISTS active_requests ("
        "  request_id TEXT PRIMARY KEY,"
        "  pool_name TEXT NOT NULL,"
        "  worker_id INTEGER NOT NULL,"
        "  pid INTEGER NOT NULL,"
        "  uri TEXT,"
        "  method TEXT,"
        "  start_time REAL NOT NULL"
        ")",
        NULL, NULL, &errmsg);
    
    if (rc != SQLITE_OK) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, NULL,
                     "mod_wsgi (pid=%d): Failed to create active_requests table: %s",
                     getpid(), errmsg);
        sqlite3_free(errmsg);
        sqlite3_close(wsgi_status_db);
        wsgi_status_db = NULL;
        return -1;
    }
    
    /* Create index on pid for efficient cleanup */
    sqlite3_exec(wsgi_status_db,
        "CREATE INDEX IF NOT EXISTS idx_active_requests_pid ON active_requests(pid)",
        NULL, NULL, NULL);
    
    /* Prepare insert statement */
    rc = sqlite3_prepare_v2(wsgi_status_db,
        "INSERT OR REPLACE INTO active_requests "
        "(request_id, pool_name, worker_id, pid, uri, method, start_time) "
        "VALUES (?, ?, ?, ?, ?, ?, ?)",
        -1, &wsgi_status_insert_stmt, NULL);
    
    if (rc != SQLITE_OK) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, NULL,
                     "mod_wsgi (pid=%d): Failed to prepare insert statement: %s",
                     getpid(), sqlite3_errmsg(wsgi_status_db));
    }
    
    /* Prepare delete statement */
    rc = sqlite3_prepare_v2(wsgi_status_db,
        "DELETE FROM active_requests WHERE request_id = ?",
        -1, &wsgi_status_delete_stmt, NULL);
    
    if (rc != SQLITE_OK) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, NULL,
                     "mod_wsgi (pid=%d): Failed to prepare delete statement: %s",
                     getpid(), sqlite3_errmsg(wsgi_status_db));
    }
    
    /* Clean up any stale entries from previous instances of this PID */
    wsgi_status_cleanup(getpid());
    
    /* Create mutex for thread safety */
    apr_thread_mutex_create(&wsgi_status_mutex, APR_THREAD_MUTEX_DEFAULT, pool);
    
    /* Register cleanup callback */
    apr_pool_cleanup_register(pool, NULL, wsgi_status_pool_cleanup,
                              apr_pool_cleanup_null);
    
    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, NULL,
                 "mod_wsgi (pid=%d): Status database initialized at '%s'",
                 getpid(), db_path);
    
    return 0;
}

/* ------------------------------------------------------------------------- */

void wsgi_status_request_start(
    const char *request_id,
    const char *pool_name,
    int worker_id,
    pid_t pid,
    const char *uri,
    const char *method)
{
    if (!wsgi_status_db || !wsgi_status_insert_stmt) {
        return;
    }
    
    if (wsgi_status_mutex) {
        apr_thread_mutex_lock(wsgi_status_mutex);
    }
    
    sqlite3_reset(wsgi_status_insert_stmt);
    
    sqlite3_bind_text(wsgi_status_insert_stmt, 1, request_id ? request_id : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(wsgi_status_insert_stmt, 2, pool_name ? pool_name : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(wsgi_status_insert_stmt, 3, worker_id);
    sqlite3_bind_int(wsgi_status_insert_stmt, 4, pid);
    sqlite3_bind_text(wsgi_status_insert_stmt, 5, uri ? uri : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(wsgi_status_insert_stmt, 6, method ? method : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(wsgi_status_insert_stmt, 7, (double)apr_time_now() / APR_USEC_PER_SEC);
    
    if (sqlite3_step(wsgi_status_insert_stmt) != SQLITE_DONE) {
        ap_log_error(APLOG_MARK, APLOG_WARNING, 0, NULL,
                     "mod_wsgi (pid=%d): Failed to record request start: %s",
                     getpid(), sqlite3_errmsg(wsgi_status_db));
    }
    
    if (wsgi_status_mutex) {
        apr_thread_mutex_unlock(wsgi_status_mutex);
    }
}

/* ------------------------------------------------------------------------- */

void wsgi_status_request_end(const char *request_id)
{
    if (!wsgi_status_db || !wsgi_status_delete_stmt) {
        return;
    }
    
    if (wsgi_status_mutex) {
        apr_thread_mutex_lock(wsgi_status_mutex);
    }
    
    sqlite3_reset(wsgi_status_delete_stmt);
    
    sqlite3_bind_text(wsgi_status_delete_stmt, 1, request_id ? request_id : "", -1, SQLITE_TRANSIENT);
    
    if (sqlite3_step(wsgi_status_delete_stmt) != SQLITE_DONE) {
        ap_log_error(APLOG_MARK, APLOG_WARNING, 0, NULL,
                     "mod_wsgi (pid=%d): Failed to record request end: %s",
                     getpid(), sqlite3_errmsg(wsgi_status_db));
    }
    
    if (wsgi_status_mutex) {
        apr_thread_mutex_unlock(wsgi_status_mutex);
    }
}

/* ------------------------------------------------------------------------- */

void wsgi_status_cleanup(pid_t pid)
{
    sqlite3_stmt *stmt;
    
    if (!wsgi_status_db) {
        return;
    }
    
    /* Delete all entries for this PID (process shutting down) */
    if (sqlite3_prepare_v2(wsgi_status_db,
            "DELETE FROM active_requests WHERE pid = ?",
            -1, &stmt, NULL) == SQLITE_OK) {
        
        sqlite3_bind_int(stmt, 1, pid);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    
    /* If this is a full cleanup (process shutdown), close everything */
    if (pid == getpid()) {
        if (wsgi_status_insert_stmt) {
            sqlite3_finalize(wsgi_status_insert_stmt);
            wsgi_status_insert_stmt = NULL;
        }
        
        if (wsgi_status_delete_stmt) {
            sqlite3_finalize(wsgi_status_delete_stmt);
            wsgi_status_delete_stmt = NULL;
        }
        
        sqlite3_close(wsgi_status_db);
        wsgi_status_db = NULL;
    }
}

/* ------------------------------------------------------------------------- */

int wsgi_is_status_request(request_rec *r)
{
    /* Check if this is a request for /wsgi-status */
    if (r->uri && strcmp(r->uri, "/wsgi-status") == 0) {
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------------- */

/*
 * Helper function to escape JSON strings.
 */
static void wsgi_json_escape_string(request_rec *r, const char *str)
{
    const char *p;
    
    if (!str) {
        ap_rputs("null", r);
        return;
    }
    
    ap_rputc('"', r);
    for (p = str; *p; p++) {
        switch (*p) {
            case '"':
                ap_rputs("\\\"", r);
                break;
            case '\\':
                ap_rputs("\\\\", r);
                break;
            case '\b':
                ap_rputs("\\b", r);
                break;
            case '\f':
                ap_rputs("\\f", r);
                break;
            case '\n':
                ap_rputs("\\n", r);
                break;
            case '\r':
                ap_rputs("\\r", r);
                break;
            case '\t':
                ap_rputs("\\t", r);
                break;
            default:
                if ((unsigned char)*p < 32) {
                    ap_rprintf(r, "\\u%04x", (unsigned char)*p);
                } else {
                    ap_rputc(*p, r);
                }
                break;
        }
    }
    ap_rputc('"', r);
}

/* ------------------------------------------------------------------------- */

int wsgi_status_handler(request_rec *r)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    int rc;
    apr_time_t now;
    int first = 1;
    
    /* Only handle GET requests */
    if (r->method_number != M_GET) {
        return HTTP_METHOD_NOT_ALLOWED;
    }
    
    /* Open a read-only connection to the database */
    if (!wsgi_status_db_path) {
        ap_set_content_type(r, "application/json");
        ap_rputs("{\"error\": \"Status tracking not initialized\"}\n", r);
        return OK;
    }
    
    rc = sqlite3_open_v2(wsgi_status_db_path, &db, SQLITE_OPEN_READONLY, NULL);
    if (rc != SQLITE_OK) {
        ap_set_content_type(r, "application/json");
        ap_rprintf(r, "{\"error\": \"Failed to open status database: %s\"}\n",
                   sqlite3_errmsg(db));
        if (db) sqlite3_close(db);
        return OK;
    }
    
    /* Set busy timeout */
    sqlite3_busy_timeout(db, 1000);
    
    now = apr_time_now();
    
    ap_set_content_type(r, "application/json");
    
    ap_rputs("{\n", r);
    ap_rprintf(r, "  \"timestamp\": %.3f,\n", (double)now / APR_USEC_PER_SEC);
    ap_rputs("  \"active_requests\": [\n", r);
    
    /* Query all active requests */
    rc = sqlite3_prepare_v2(db,
        "SELECT request_id, pool_name, worker_id, pid, uri, method, start_time "
        "FROM active_requests ORDER BY start_time",
        -1, &stmt, NULL);
    
    if (rc == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *request_id = (const char *)sqlite3_column_text(stmt, 0);
            const char *pool_name = (const char *)sqlite3_column_text(stmt, 1);
            int worker_id = sqlite3_column_int(stmt, 2);
            int pid = sqlite3_column_int(stmt, 3);
            const char *uri = (const char *)sqlite3_column_text(stmt, 4);
            const char *method = (const char *)sqlite3_column_text(stmt, 5);
            double start_time = sqlite3_column_double(stmt, 6);
            double duration = ((double)now / APR_USEC_PER_SEC) - start_time;
            
            if (!first) {
                ap_rputs(",\n", r);
            }
            first = 0;
            
            ap_rputs("    {\n", r);
            
            ap_rputs("      \"request_id\": ", r);
            wsgi_json_escape_string(r, request_id);
            ap_rputs(",\n", r);
            
            ap_rputs("      \"pool_name\": ", r);
            wsgi_json_escape_string(r, pool_name);
            ap_rputs(",\n", r);
            
            ap_rprintf(r, "      \"worker_id\": %d,\n", worker_id);
            ap_rprintf(r, "      \"pid\": %d,\n", pid);
            
            ap_rputs("      \"uri\": ", r);
            wsgi_json_escape_string(r, uri);
            ap_rputs(",\n", r);
            
            ap_rputs("      \"method\": ", r);
            wsgi_json_escape_string(r, method);
            ap_rputs(",\n", r);
            
            ap_rprintf(r, "      \"start_time\": %.3f,\n", start_time);
            ap_rprintf(r, "      \"duration\": %.3f\n", duration);
            
            ap_rputs("    }", r);
        }
        
        sqlite3_finalize(stmt);
    }
    
    ap_rputs("\n  ]\n", r);
    ap_rputs("}\n", r);
    
    sqlite3_close(db);
    
    return OK;
}

/* ------------------------------------------------------------------------- */

/* vi: set sw=4 expandtab : */

