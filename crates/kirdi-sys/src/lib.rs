//! Raw FFI bindings to the kirdi VPN client C API.
//!
//! This crate provides unsafe `extern "C"` declarations matching `include/kirdi/kirdi.h`.
//! For a safe Rust API, use the `kirdi-ffi` crate instead.

#![allow(non_camel_case_types)]

use std::os::raw::{c_char, c_int, c_void};

/// Path to the kirdi-client CLI binary (set at build time by build.rs).
/// Used for running VPN with elevated privileges as a subprocess.
pub const CLIENT_BIN_PATH: &str = env!("KIRDI_CLIENT_BIN");

/// VPN client configuration (mirrors `kirdi_config_t` from kirdi.h)
#[repr(C)]
pub struct kirdi_config_t {
    pub server_host: *const c_char,
    pub server_port: u16,
    pub ws_path: *const c_char,
    pub auth_token: *const c_char,
    pub auth_user: *const c_char,
    pub tls_enabled: c_int,
    pub auto_route: c_int,
    pub dns_server: *const c_char,
    pub mtu: u32,
    pub sni_override: *const c_char,
    pub log_level: *const c_char,
}

/// Opaque client handle
pub type kirdi_client_t = c_void;

/// Status callback type.
pub type kirdi_status_cb_t =
    unsafe extern "C" fn(event: *const c_char, json_data: *const c_char, userdata: *mut c_void);

unsafe extern "C" {
    pub fn kirdi_config_init(cfg: *mut kirdi_config_t);
    pub fn kirdi_create(cfg: *const kirdi_config_t) -> *mut kirdi_client_t;
    pub fn kirdi_create_json(json_str: *const c_char) -> *mut kirdi_client_t;
    pub fn kirdi_set_callback(
        client: *mut kirdi_client_t,
        cb: kirdi_status_cb_t,
        userdata: *mut c_void,
    );
    pub fn kirdi_run(client: *mut kirdi_client_t) -> c_int;
    pub fn kirdi_stop(client: *mut kirdi_client_t);
    pub fn kirdi_destroy(client: *mut kirdi_client_t);
    pub fn kirdi_version() -> *const c_char;
}
