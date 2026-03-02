/**
 * kirdi VPN Client — Public C API
 *
 * This is the stable C interface for embedding the kirdi VPN client
 * into any application (Rust FFI, Python ctypes, Go CGo, etc.)
 *
 * Thread safety:
 *   - kirdi_create / kirdi_destroy: NOT thread-safe (call from one thread)
 *   - kirdi_run: blocking, call from a worker thread
 *   - kirdi_stop: thread-safe, can be called from any thread
 *   - kirdi_set_callback: call before kirdi_run
 *   - Status callback is invoked from the I/O thread
 */

#ifndef KIRDI_H
#define KIRDI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Opaque client handle ──────────────────────────────────────────────── */

typedef struct kirdi_client kirdi_client_t;

/* ── Configuration ─────────────────────────────────────────────────────── */

typedef struct {
    const char* server_host;    /* Required. Server hostname or IP */
    uint16_t    server_port;    /* Default: 443 */
    const char* ws_path;        /* Default: "/tunnel/" */
    const char* auth_token;     /* Required. Authentication token */
    const char* auth_user;      /* Default: "kirdi" */
    int         tls_enabled;    /* Default: 1 (true) */
    int         auto_route;     /* Default: 1 (true) — auto-configure routes */
    const char* dns_server;     /* Default: "1.1.1.1" */
    uint32_t    mtu;            /* Default: 1400 */
    const char* sni_override;   /* Optional. TLS SNI override (NULL = none) */
    const char* log_level;      /* Default: "info". Options: trace/debug/info/warn/error */
} kirdi_config_t;

/* Initialize config with sane defaults. Call this before setting fields. */
void kirdi_config_init(kirdi_config_t* cfg);

/* ── Status callback ───────────────────────────────────────────────────── */

/**
 * Status callback type.
 *
 * @param event     One of: "connecting", "authenticating", "connected",
 *                  "disconnected", "error", "reconnecting"
 * @param json_data JSON string with additional info. Examples:
 *                  connected:    {"clientIp":"10.8.0.2","serverIp":"vpn.example.com"}
 *                  error:        {"message":"Auth rejected"}
 *                  reconnecting: {"attempt":2,"delay":4}
 * @param userdata  Opaque pointer passed to kirdi_set_callback
 */
typedef void (*kirdi_status_cb_t)(
    const char* event,
    const char* json_data,
    void* userdata
);

/* ── Lifecycle ─────────────────────────────────────────────────────────── */

/**
 * Create a new VPN client from a config struct.
 * Returns NULL on validation error (check server_host, auth_token).
 */
kirdi_client_t* kirdi_create(const kirdi_config_t* cfg);

/**
 * Create a new VPN client from a JSON config string.
 * JSON keys: serverHost, serverPort, wsPath, authToken, authUser,
 *            useTls, autoRoute, dns, mtu, sniOverride, logLevel
 * Returns NULL on parse/validation error.
 */
kirdi_client_t* kirdi_create_json(const char* json_str);

/**
 * Set status callback. Must be called before kirdi_run().
 */
void kirdi_set_callback(kirdi_client_t* client, kirdi_status_cb_t cb, void* userdata);

/**
 * Run the VPN client. BLOCKS until kirdi_stop() is called or an error occurs.
 * Call this from a worker thread!
 * Returns 0 on clean shutdown, non-zero on error.
 */
int kirdi_run(kirdi_client_t* client);

/**
 * Stop the VPN client. Thread-safe — can be called from any thread.
 * Causes kirdi_run() to return.
 */
void kirdi_stop(kirdi_client_t* client);

/**
 * Destroy the client and free all resources.
 * Calls kirdi_stop() first if still running.
 */
void kirdi_destroy(kirdi_client_t* client);

/* ── Info ──────────────────────────────────────────────────────────────── */

/** Returns the library version string (e.g. "0.1.0"). */
const char* kirdi_version(void);

#ifdef __cplusplus
}
#endif

#endif /* KIRDI_H */
