/**
 * php_lumbre.c -- PHP module integration layer for the lumbre extension.
 *
 * Module lifecycle (GINIT/MINIT/RINIT/RSHUTDOWN/MSHUTDOWN), Zend hook
 * installation, INI entries, and execute_internal/execute_ex hooks.
 *
 * This file is the ONLY file in the extension that includes PHP headers.
 * All other .c files are pure C with no Zend dependency.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "zend_API.h"
#include "zend_execute.h"
#include "SAPI.h"

/* --------------------------------------------------------------------------
 * Compat: PG(http_globals) changed from zval* to zval in PHP 8.x.
 * We need a pointer to the SERVER superglobal for hash lookups.
 * ----------------------------------------------------------------------- */
static inline zval *lumbre_get_server_vars(void) {
    zval *sv = &PG(http_globals)[TRACK_VARS_SERVER];
    if (Z_TYPE_P(sv) == IS_ARRAY) {
        return sv;
    }
    return NULL;
}

#include "php_lumbre.h"
#include "lumbre_ringbuf.h"
#include "lumbre_msgpack.h"
#include "lumbre_span.h"
#include "lumbre_whitelist.h"

#include <string.h>
#include <unistd.h>

#ifdef ZTS
#include <stdatomic.h>
#endif

/* --------------------------------------------------------------------------
 * Module globals definition
 * ----------------------------------------------------------------------- */

ZEND_DECLARE_MODULE_GLOBALS(lumbre)

/* --------------------------------------------------------------------------
 * Saved original Zend handlers (process-wide, not per-thread)
 * ----------------------------------------------------------------------- */

static void (*lumbre_original_execute_internal)(zend_execute_data *execute_data, zval *return_value);
static void (*lumbre_original_execute_ex)(zend_execute_data *execute_data);

/* --------------------------------------------------------------------------
 * Worker ID counter (ZTS only)
 * ----------------------------------------------------------------------- */

#ifdef ZTS
static _Atomic uint64_t lumbre_worker_counter = 0;
#endif

static uint64_t lumbre_worker_id(void)
{
#ifdef ZTS
    return atomic_fetch_add(&lumbre_worker_counter, 1);
#else
    return 0;
#endif
}

/* --------------------------------------------------------------------------
 * Helper: PHP random bytes wrapper
 * ----------------------------------------------------------------------- */

#if PHP_VERSION_ID >= 80200
#include "ext/random/php_random.h"
#elif PHP_VERSION_ID >= 70000
#include "ext/standard/php_random.h"
#endif
static int lumbre_php_random_bytes(void *buf, size_t len)
{
    return php_random_bytes(buf, len, 0) == SUCCESS ? 0 : -1;
}

/* --------------------------------------------------------------------------
 * Helper: hex string to binary (for X-Trace-Id parsing)
 * ----------------------------------------------------------------------- */

static int lumbre_hex_to_bin(const char *hex, size_t hex_len,
                             uint8_t *out, size_t out_len)
{
    size_t i;

    if (hex_len != out_len * 2) {
        return -1;
    }

    for (i = 0; i < out_len; i++) {
        unsigned int hi, lo;
        char c;

        c = hex[i * 2];
        if (c >= '0' && c <= '9') {
            hi = (unsigned int)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            hi = (unsigned int)(c - 'a') + 10;
        } else if (c >= 'A' && c <= 'F') {
            hi = (unsigned int)(c - 'A') + 10;
        } else {
            return -1;
        }

        c = hex[i * 2 + 1];
        if (c >= '0' && c <= '9') {
            lo = (unsigned int)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            lo = (unsigned int)(c - 'a') + 10;
        } else if (c >= 'A' && c <= 'F') {
            lo = (unsigned int)(c - 'A') + 10;
        } else {
            return -1;
        }

        out[i] = (uint8_t)((hi << 4) | lo);
    }

    return 0;
}

/* --------------------------------------------------------------------------
 * INI: custom OnUpdate for lumbre.mode ("io" -> 0, "full" -> 1)
 * ----------------------------------------------------------------------- */

static PHP_INI_MH(lumbre_OnUpdateMode)
{
    if (ZSTR_LEN(new_value) == 2
        && ZSTR_VAL(new_value)[0] == 'i'
        && ZSTR_VAL(new_value)[1] == 'o') {
        LUMBRE_G(mode) = 0;
    } else if (ZSTR_LEN(new_value) == 4
               && memcmp(ZSTR_VAL(new_value), "full", 4) == 0) {
        LUMBRE_G(mode) = 1;
    } else {
        /* Try numeric: "0" or "1" */
        zend_long val = ZEND_STRTOL(ZSTR_VAL(new_value), NULL, 10);
        if (val == 0 || val == 1) {
            LUMBRE_G(mode) = val;
        } else {
            return FAILURE;
        }
    }

    return SUCCESS;
}

/* --------------------------------------------------------------------------
 * INI entries
 * ----------------------------------------------------------------------- */

PHP_INI_BEGIN()
    STD_PHP_INI_BOOLEAN("lumbre.enabled", "1",
        PHP_INI_ALL,
        OnUpdateBool,
        enabled, zend_lumbre_globals, lumbre_globals)

    PHP_INI_ENTRY("lumbre.mode", "io",
        PHP_INI_SYSTEM,
        lumbre_OnUpdateMode)

    STD_PHP_INI_ENTRY("lumbre.buffer_size", "4194304",
        PHP_INI_SYSTEM,
        OnUpdateLong,
        buffer_size, zend_lumbre_globals, lumbre_globals)

    STD_PHP_INI_ENTRY("lumbre.shm_dir", "/dev/shm",
        PHP_INI_SYSTEM,
        OnUpdateString,
        shm_dir, zend_lumbre_globals, lumbre_globals)

    STD_PHP_INI_ENTRY("lumbre.trigger_header", "X-Trace-Debug",
        PHP_INI_SYSTEM,
        OnUpdateString,
        trigger_header, zend_lumbre_globals, lumbre_globals)

    STD_PHP_INI_ENTRY("lumbre.max_query_len", "2048",
        PHP_INI_SYSTEM,
        OnUpdateLong,
        max_query_len, zend_lumbre_globals, lumbre_globals)

    STD_PHP_INI_ENTRY("lumbre.min_duration_ns", "0",
        PHP_INI_ALL,
        OnUpdateLong,
        min_duration_ns, zend_lumbre_globals, lumbre_globals)

    STD_PHP_INI_ENTRY("lumbre.trace_namespaces", "",
        PHP_INI_SYSTEM,
        OnUpdateString,
        trace_namespaces, zend_lumbre_globals, lumbre_globals)
PHP_INI_END()

/* --------------------------------------------------------------------------
 * Forward declarations for hooks
 * ----------------------------------------------------------------------- */

static void lumbre_execute_internal(zend_execute_data *execute_data, zval *return_value);
static void lumbre_execute_ex(zend_execute_data *execute_data);

/* --------------------------------------------------------------------------
 * GINIT — called once per thread (ZTS) or once per process (NTS)
 * ----------------------------------------------------------------------- */

static PHP_GINIT_FUNCTION(lumbre)
{
#if defined(COMPILE_DL_LUMBRE) && defined(ZTS)
    ZEND_TSRMLS_CACHE_UPDATE();
#endif
    memset(lumbre_globals, 0, sizeof(*lumbre_globals));
    lumbre_whitelist_init(&lumbre_globals->whitelist);
}

/* --------------------------------------------------------------------------
 * MINIT — called once at process startup
 * ----------------------------------------------------------------------- */

static PHP_MINIT_FUNCTION(lumbre)
{
    REGISTER_INI_ENTRIES();

    /* Save original Zend handlers and install our hooks */
    lumbre_original_execute_internal = zend_execute_internal;
    zend_execute_internal = lumbre_execute_internal;

    lumbre_original_execute_ex = zend_execute_ex;
    zend_execute_ex = lumbre_execute_ex;

    return SUCCESS;
}

/* --------------------------------------------------------------------------
 * RINIT — called at the start of each request
 * ----------------------------------------------------------------------- */

static PHP_RINIT_FUNCTION(lumbre)
{
    if (!LUMBRE_G(enabled)) {
        return SUCCESS;
    }

    /* ------------------------------------------------------------------
     * Lazy ring buffer init (first request for this worker/thread)
     * ---------------------------------------------------------------- */
    if (!LUMBRE_G(ringbuf_initialized)) {
        uint64_t wid = lumbre_worker_id();
        LUMBRE_G(worker_id) = wid;

        /* Allocate persistent msgpack serialisation buffer */
        if (!LUMBRE_G(mp_storage)) {
            LUMBRE_G(mp_storage) = pemalloc(LUMBRE_MSGPACK_BUF_SIZE, 1);
            if (!LUMBRE_G(mp_storage)) {
                LUMBRE_G(enabled) = 0;
                return SUCCESS;
            }
        }
        LUMBRE_G(mp_buf).buf      = LUMBRE_G(mp_storage);
        LUMBRE_G(mp_buf).capacity = LUMBRE_MSGPACK_BUF_SIZE;
        LUMBRE_G(mp_buf).pos      = 0;

        if (lumbre_ringbuf_init(&LUMBRE_G(ringbuf),
                                LUMBRE_G(shm_dir),
                                (uint32_t)getpid(),
                                wid,
                                (uint32_t)LUMBRE_G(buffer_size)) != 0) {
            php_error_docref(NULL, E_WARNING,
                             "lumbre: failed to init ring buffer in %s",
                             LUMBRE_G(shm_dir));
            LUMBRE_G(enabled) = 0;
            return SUCCESS;
        }

        LUMBRE_G(ringbuf_initialized) = 1;
    }

    /* ------------------------------------------------------------------
     * Trace ID propagation: look for X-Trace-Id in $_SERVER
     * ---------------------------------------------------------------- */
    uint8_t propagated_id[16];
    const uint8_t *propagated = NULL;

    {
        zval *server_vars = lumbre_get_server_vars();
        if (server_vars) {
            zval *ztrace = zend_hash_str_find(
                Z_ARRVAL_P(server_vars),
                "HTTP_X_TRACE_ID", sizeof("HTTP_X_TRACE_ID") - 1
            );
            if (ztrace && Z_TYPE_P(ztrace) == IS_STRING
                && Z_STRLEN_P(ztrace) == 32) {
                if (lumbre_hex_to_bin(Z_STRVAL_P(ztrace), 32,
                                      propagated_id, 16) == 0) {
                    propagated = propagated_id;
                }
            }
        }
    }

    lumbre_context_init(&LUMBRE_G(ctx), propagated, lumbre_php_random_bytes);

    /* ------------------------------------------------------------------
     * Trigger header: check for X-Trace-Debug (or configured header)
     * ---------------------------------------------------------------- */
    if (LUMBRE_G(trigger_header) && LUMBRE_G(trigger_header)[0] != '\0') {
        zval *server_vars_trigger = lumbre_get_server_vars();
        if (server_vars_trigger) {
            /*
             * HTTP headers in $_SERVER are prefixed with HTTP_ and uppercased.
             * Convert "X-Trace-Debug" -> "HTTP_X_TRACE_DEBUG".
             */
            char header_key[256];
            size_t hdr_len = strlen(LUMBRE_G(trigger_header));
            size_t key_len;
            size_t i;

            if (hdr_len + 6 < sizeof(header_key)) { /* "HTTP_" + header + NUL */
                memcpy(header_key, "HTTP_", 5);
                for (i = 0; i < hdr_len; i++) {
                    char c = LUMBRE_G(trigger_header)[i];
                    if (c == '-') {
                        header_key[5 + i] = '_';
                    } else if (c >= 'a' && c <= 'z') {
                        header_key[5 + i] = (char)(c - 'a' + 'A');
                    } else {
                        header_key[5 + i] = c;
                    }
                }
                key_len = 5 + hdr_len;
                header_key[key_len] = '\0';

                zval *ztrigger = zend_hash_str_find(
                    Z_ARRVAL_P(server_vars_trigger),
                    header_key, key_len
                );
                if (ztrigger) {
                    LUMBRE_G(ctx).full_trace = 1;
                }
            }
        }
    }

    /* ------------------------------------------------------------------
     * Start root span
     * ---------------------------------------------------------------- */
    lumbre_span_start(&LUMBRE_G(root_span), LUMBRE_SPAN_HTTP_IN, &LUMBRE_G(ctx));

    {
        const char *uri;
        size_t uri_len;
        const char *method;
        size_t method_len;

        if (sapi_module.name && strcmp(sapi_module.name, "cli") == 0) {
            /* CLI SAPI: use script filename */
            uri = SG(request_info).path_translated
                  ? SG(request_info).path_translated : "";
            uri_len = strlen(uri);
            method = "CLI";
            method_len = 3;
        } else {
            /* FPM / FrankenPHP / other web SAPIs */
            uri = SG(request_info).request_uri
                  ? SG(request_info).request_uri : "";
            uri_len = strlen(uri);
            method = SG(request_info).request_method
                     ? SG(request_info).request_method : "";
            method_len = strlen(method);
        }

        lumbre_span_set_http_in(&LUMBRE_G(root_span),
                                uri, (uint32_t)uri_len,
                                method, (uint32_t)method_len);
    }

    LUMBRE_G(ctx).root_span_id = LUMBRE_G(root_span).span_id;

    return SUCCESS;
}

/* --------------------------------------------------------------------------
 * RSHUTDOWN — called at the end of each request
 * ----------------------------------------------------------------------- */

static PHP_RSHUTDOWN_FUNCTION(lumbre)
{
    if (!LUMBRE_G(ctx).active) {
        return SUCCESS;
    }

    /* Finish root span: min_duration=0 (root is never filtered) */
    lumbre_span_finish(&LUMBRE_G(root_span),
                       &LUMBRE_G(ringbuf),
                       &LUMBRE_G(mp_buf),
                       &LUMBRE_G(ctx),
                       0,
                       (uint32_t)LUMBRE_G(max_query_len));

    lumbre_context_reset(&LUMBRE_G(ctx));

    return SUCCESS;
}

/* --------------------------------------------------------------------------
 * MSHUTDOWN — called once at process shutdown
 * ----------------------------------------------------------------------- */

static PHP_MSHUTDOWN_FUNCTION(lumbre)
{
    /* Restore original Zend handlers */
    zend_execute_internal = lumbre_original_execute_internal;
    zend_execute_ex = lumbre_original_execute_ex;

    /* Destroy ring buffer. In CLI, skip unlink so daemon can drain. */
    if (LUMBRE_G(ringbuf_initialized)) {
        int skip_unlink = (sapi_module.name
                           && strcmp(sapi_module.name, "cli") == 0);
        lumbre_ringbuf_destroy(&LUMBRE_G(ringbuf), skip_unlink);
    }

    /* Free persistent msgpack buffer */
    if (LUMBRE_G(mp_storage)) {
        pefree(LUMBRE_G(mp_storage), 1);
        LUMBRE_G(mp_storage) = NULL;
    }

    lumbre_whitelist_destroy(&LUMBRE_G(whitelist));

    UNREGISTER_INI_ENTRIES();

    return SUCCESS;
}

/* --------------------------------------------------------------------------
 * MINFO — phpinfo() output
 * ----------------------------------------------------------------------- */

static PHP_MINFO_FUNCTION(lumbre)
{
    php_info_print_table_start();
    php_info_print_table_header(2, "lumbre support", "enabled");
    php_info_print_table_row(2, "Version", PHP_LUMBRE_VERSION);
    php_info_print_table_row(2, "Mode",
                             LUMBRE_G(mode) == 1 ? "full" : "io");

    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%ld", (long)LUMBRE_G(buffer_size));
        php_info_print_table_row(2, "Buffer size", buf);
    }

    php_info_print_table_row(2, "SHM directory",
                             LUMBRE_G(shm_dir) ? LUMBRE_G(shm_dir) : "(null)");
    php_info_print_table_end();

    DISPLAY_INI_ENTRIES();
}

/* --------------------------------------------------------------------------
 * Helper: extract class and function names from execute_data
 * ----------------------------------------------------------------------- */

static inline void lumbre_extract_names(
    zend_execute_data *execute_data,
    const char **class_name, size_t *class_len,
    const char **func_name,  size_t *func_len)
{
    zend_function *func = execute_data->func;

    if (func->common.function_name) {
        *func_name = ZSTR_VAL(func->common.function_name);
        *func_len  = ZSTR_LEN(func->common.function_name);
    } else {
        *func_name = NULL;
        *func_len  = 0;
    }

    if (func->common.scope) {
        *class_name = ZSTR_VAL(func->common.scope->name);
        *class_len  = ZSTR_LEN(func->common.scope->name);
    } else {
        *class_name = NULL;
        *class_len  = 0;
    }
}

/* --------------------------------------------------------------------------
 * Helper: infer DB type from class name
 * ----------------------------------------------------------------------- */

static inline void lumbre_infer_db_type(
    const char *class_name, size_t class_len,
    const char **db_type, uint32_t *db_type_len)
{
    if (!class_name) {
        *db_type     = "unknown";
        *db_type_len = 7;
        return;
    }

    /*
     * Case-insensitive comparison (PHP class names are case-insensitive).
     * We check lowercase forms since the whitelist canonicalises to lower.
     * But here we get the original casing from Zend, so compare both.
     */
    if (class_len == 3
        && (class_name[0] == 'P' || class_name[0] == 'p')
        && (class_name[1] == 'D' || class_name[1] == 'd')
        && (class_name[2] == 'O' || class_name[2] == 'o')) {
        *db_type     = "pdo";
        *db_type_len = 3;
    } else if (class_len >= 6
               && (class_name[0] == 'm' || class_name[0] == 'M')
               && (class_name[1] == 'y' || class_name[1] == 'Y')
               && (class_name[2] == 's' || class_name[2] == 'S')
               && (class_name[3] == 'q' || class_name[3] == 'Q')
               && (class_name[4] == 'l' || class_name[4] == 'L')
               && (class_name[5] == 'i' || class_name[5] == 'I')) {
        *db_type     = "mysql";
        *db_type_len = 5;
    } else if (class_len >= 12
               && (class_name[0] == 'P' || class_name[0] == 'p')
               && (class_name[1] == 'D' || class_name[1] == 'd')
               && (class_name[2] == 'O' || class_name[2] == 'o')
               && class_name[3] == 'S') {
        /* PDOStatement — infer from PDO (v1: just "pdo") */
        *db_type     = "pdo";
        *db_type_len = 3;
    } else {
        /* pg_*, etc. — for pg_query and similar global functions */
        *db_type     = "pgsql";
        *db_type_len = 5;
    }
}

/* --------------------------------------------------------------------------
 * Helper: extract first string argument from execute_data
 * ----------------------------------------------------------------------- */

static inline void lumbre_extract_first_str_arg(
    zend_execute_data *execute_data,
    const char **str, uint32_t *str_len)
{
    uint32_t num_args = ZEND_CALL_NUM_ARGS(execute_data);

    if (num_args >= 1) {
        zval *arg = ZEND_CALL_ARG(execute_data, 1);
        if (arg && Z_TYPE_P(arg) == IS_STRING) {
            *str     = Z_STRVAL_P(arg);
            *str_len = (uint32_t)Z_STRLEN_P(arg);
            return;
        }
    }

    *str     = NULL;
    *str_len = 0;
}

/* --------------------------------------------------------------------------
 * Hook: lumbre_execute_internal (internal/C function calls)
 * ----------------------------------------------------------------------- */

static void lumbre_execute_internal(zend_execute_data *execute_data,
                                    zval *return_value)
{
    const char *class_name;
    size_t      class_len;
    const char *func_name;
    size_t      func_len;
    const lumbre_whitelist_entry_t *entry;

    /* Early exit: tracing disabled or no active request context */
    if (!LUMBRE_G(enabled) || !LUMBRE_G(ctx).active) {
        if (lumbre_original_execute_internal) {
            lumbre_original_execute_internal(execute_data, return_value);
        } else {
            execute_internal(execute_data, return_value);
        }
        return;
    }

    /* Extract class + function from execute_data */
    lumbre_extract_names(execute_data,
                         &class_name, &class_len,
                         &func_name, &func_len);

    if (!func_name) {
        if (lumbre_original_execute_internal) {
            lumbre_original_execute_internal(execute_data, return_value);
        } else {
            execute_internal(execute_data, return_value);
        }
        return;
    }

    /* Whitelist lookup */
    entry = lumbre_whitelist_match(&LUMBRE_G(whitelist),
                                   class_name, class_len,
                                   func_name, func_len);

    if (!entry) {
        /* No match: call original and return (cost: ~15ns) */
        if (lumbre_original_execute_internal) {
            lumbre_original_execute_internal(execute_data, return_value);
        } else {
            execute_internal(execute_data, return_value);
        }
        return;
    }

    /* Create span on stack */
    lumbre_span_t span;
    lumbre_span_start(&span, entry->span_type, &LUMBRE_G(ctx));

    /* Extract payload PRE-call based on span type */
    switch (entry->span_type) {
    case LUMBRE_SPAN_DB: {
        const char *query;
        uint32_t query_len;
        const char *db_type;
        uint32_t db_type_len;

        lumbre_extract_first_str_arg(execute_data, &query, &query_len);
        lumbre_infer_db_type(class_name, class_len, &db_type, &db_type_len);

        if (query) {
            lumbre_span_set_db(&span, query, query_len,
                               db_type, db_type_len,
                               0, (uint32_t)LUMBRE_G(max_query_len));
        }
        break;
    }
    case LUMBRE_SPAN_REDIS:
    case LUMBRE_SPAN_MEMCACHED:
    case LUMBRE_SPAN_CACHE: {
        const char *key;
        uint32_t key_len;

        lumbre_extract_first_str_arg(execute_data, &key, &key_len);
        lumbre_span_set_cache(&span,
                              func_name, (uint32_t)func_len,
                              key ? key : "", key_len);
        break;
    }
    case LUMBRE_SPAN_FILE_IO: {
        const char *path;
        uint32_t path_len;

        lumbre_extract_first_str_arg(execute_data, &path, &path_len);
        if (path) {
            lumbre_span_set_file_io(&span, path, path_len);
        }
        break;
    }
    case LUMBRE_SPAN_HTTP_OUT:
        /* curl v1: no URL/status extraction, timing only */
        break;

    default:
        break;
    }

    /* Call original handler */
    if (lumbre_original_execute_internal) {
        lumbre_original_execute_internal(execute_data, return_value);
    } else {
        execute_internal(execute_data, return_value);
    }

    /*
     * POST-call payload extraction would go here (e.g. status codes).
     * v1: skipped for simplicity — curl URL/status deferred to v2.
     * Note: even if the original handler threw an exception (EG(exception)),
     * we still finish the span. Exceptions in PHP are flags, not longjmps.
     */

    /* Finish span: encode to msgpack and write to ring buffer */
    lumbre_span_finish(&span,
                       &LUMBRE_G(ringbuf),
                       &LUMBRE_G(mp_buf),
                       &LUMBRE_G(ctx),
                       (uint64_t)LUMBRE_G(min_duration_ns),
                       (uint32_t)LUMBRE_G(max_query_len));
}

/* --------------------------------------------------------------------------
 * Hook: lumbre_execute_ex (userland function calls — full mode only)
 * ----------------------------------------------------------------------- */

static void lumbre_execute_ex(zend_execute_data *execute_data)
{
    /* Early exit: tracing disabled or no active context */
    if (!LUMBRE_G(enabled) || !LUMBRE_G(ctx).active) {
        lumbre_original_execute_ex(execute_data);
        return;
    }

    /* In io mode without full_trace trigger, pass through */
    if (LUMBRE_G(mode) != 1 && !LUMBRE_G(ctx).full_trace) {
        lumbre_original_execute_ex(execute_data);
        return;
    }

    /* Guard: must have a named function */
    if (!execute_data->func || !execute_data->func->common.function_name) {
        lumbre_original_execute_ex(execute_data);
        return;
    }

    /* Extract names */
    const char *class_name = NULL;
    size_t class_len = 0;
    const char *func_name = ZSTR_VAL(execute_data->func->common.function_name);
    size_t func_len = ZSTR_LEN(execute_data->func->common.function_name);

    if (execute_data->func->common.scope) {
        class_name = ZSTR_VAL(execute_data->func->common.scope->name);
        class_len  = ZSTR_LEN(execute_data->func->common.scope->name);
    }

    /* Extract file and line from the op_array */
    const char *file = NULL;
    uint32_t file_len = 0;
    uint32_t line = 0;

    if (execute_data->func->type == ZEND_USER_FUNCTION) {
        if (execute_data->func->op_array.filename) {
            file     = ZSTR_VAL(execute_data->func->op_array.filename);
            file_len = (uint32_t)ZSTR_LEN(execute_data->func->op_array.filename);
        }
        if (execute_data->opline) {
            line = execute_data->opline->lineno;
        }
    }

    /* Create FUNC span */
    lumbre_span_t span;
    lumbre_span_start(&span, LUMBRE_SPAN_FUNC, &LUMBRE_G(ctx));
    lumbre_span_set_func(&span,
                         class_name, (uint32_t)class_len,
                         func_name,  (uint32_t)func_len,
                         file ? file : "", file_len,
                         line);

    /* Call original */
    lumbre_original_execute_ex(execute_data);

    /* Finish span */
    lumbre_span_finish(&span,
                       &LUMBRE_G(ringbuf),
                       &LUMBRE_G(mp_buf),
                       &LUMBRE_G(ctx),
                       (uint64_t)LUMBRE_G(min_duration_ns),
                       (uint32_t)LUMBRE_G(max_query_len));
}

/* --------------------------------------------------------------------------
 * Module entry
 * ----------------------------------------------------------------------- */

zend_module_entry lumbre_module_entry = {
    STANDARD_MODULE_HEADER,
    "lumbre",                          /* name */
    NULL,                              /* functions (none exposed in v1) */
    PHP_MINIT(lumbre),                 /* MINIT */
    PHP_MSHUTDOWN(lumbre),             /* MSHUTDOWN */
    PHP_RINIT(lumbre),                 /* RINIT */
    PHP_RSHUTDOWN(lumbre),             /* RSHUTDOWN */
    PHP_MINFO(lumbre),                 /* MINFO */
    PHP_LUMBRE_VERSION,                /* version */
    PHP_MODULE_GLOBALS(lumbre),        /* globals */
    PHP_GINIT(lumbre),                 /* GINIT */
    NULL,                              /* GSHUTDOWN */
    NULL,                              /* post deactivate */
    STANDARD_MODULE_PROPERTIES_EX
};

#ifdef COMPILE_DL_LUMBRE
#ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
#endif
ZEND_GET_MODULE(lumbre)
#endif
