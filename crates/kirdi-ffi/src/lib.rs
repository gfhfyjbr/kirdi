//! Safe Rust wrapper around the kirdi VPN client C API.
//!
//! Provides `KirdiClient` with a safe, idiomatic Rust API
//! for connecting/disconnecting VPN and receiving status updates.

use kirdi_sys;
use serde::{Deserialize, Serialize};
use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_void};

// ── Configuration ───────────────────────────────────────────────────────────

/// VPN client configuration (Rust-native version of kirdi_config_t)
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct KirdiConfig {
    pub server_host: String,
    #[serde(default = "default_port")]
    pub server_port: u16,
    #[serde(default = "default_ws_path")]
    pub ws_path: String,
    pub auth_token: String,
    #[serde(default = "default_auth_user")]
    pub auth_user: String,
    #[serde(default = "default_true")]
    pub use_tls: bool,
    #[serde(default = "default_true")]
    pub auto_route: bool,
    #[serde(default = "default_dns")]
    pub dns: String,
    #[serde(default = "default_mtu")]
    pub mtu: u32,
    #[serde(default)]
    pub sni_override: Option<String>,
    #[serde(default = "default_log_level")]
    pub log_level: String,
}

fn default_port() -> u16 {
    443
}
fn default_ws_path() -> String {
    "/tunnel/".into()
}
fn default_auth_user() -> String {
    "kirdi".into()
}
fn default_true() -> bool {
    true
}
fn default_dns() -> String {
    "1.1.1.1".into()
}
fn default_mtu() -> u32 {
    1400
}
fn default_log_level() -> String {
    "info".into()
}

impl Default for KirdiConfig {
    fn default() -> Self {
        Self {
            server_host: String::new(),
            server_port: 443,
            ws_path: "/tunnel/".into(),
            auth_token: String::new(),
            auth_user: "kirdi".into(),
            use_tls: true,
            auto_route: true,
            dns: "1.1.1.1".into(),
            mtu: 1400,
            sni_override: None,
            log_level: "info".into(),
        }
    }
}

// ── Callback infrastructure ─────────────────────────────────────────────────

type BoxedCallback = Box<dyn Fn(&str, &str) + Send + 'static>;

struct CallbackData {
    callback: BoxedCallback,
}

/// C-compatible callback that forwards to the Rust closure.
///
/// # Safety
/// Called from C code. The `userdata` pointer must be a valid
/// pointer to a `CallbackData` created by `set_callback`.
unsafe extern "C" fn status_callback_trampoline(
    event: *const c_char,
    json_data: *const c_char,
    userdata: *mut c_void,
) {
    if userdata.is_null() || event.is_null() || json_data.is_null() {
        return;
    }

    let cb_data = unsafe { &*(userdata as *const CallbackData) };
    let event_str = unsafe { CStr::from_ptr(event) }
        .to_str()
        .unwrap_or("unknown");
    let data_str = unsafe { CStr::from_ptr(json_data) }
        .to_str()
        .unwrap_or("{}");

    (cb_data.callback)(event_str, data_str);
}

// ── VpnStopHandle ───────────────────────────────────────────────────────────

/// A thread-safe handle that can only stop the VPN client.
///
/// This is extracted from `KirdiClient` before moving it to a worker thread,
/// allowing the main thread to call `stop()` while the worker is blocked in `run()`.
///
/// # Safety
/// `kirdi_stop()` is guaranteed thread-safe by the C API.
pub struct VpnStopHandle {
    handle: *mut kirdi_sys::kirdi_client_t,
}

// SAFETY: kirdi_stop() is documented as thread-safe in kirdi.h
unsafe impl Send for VpnStopHandle {}
unsafe impl Sync for VpnStopHandle {}

impl VpnStopHandle {
    /// Stop the VPN client. Thread-safe — can be called from any thread.
    /// Causes `KirdiClient::run()` to return on the worker thread.
    pub fn stop(&self) {
        if !self.handle.is_null() {
            unsafe { kirdi_sys::kirdi_stop(self.handle) };
        }
    }
}

// ── KirdiClient ─────────────────────────────────────────────────────────────

/// Safe wrapper around the kirdi VPN client.
///
/// # Thread Safety
/// - `new()` / `new_from_json()` — call from one thread
/// - `run()` — blocking, call from a worker thread
/// - `stop_handle()` — extract a thread-safe stop handle before `run()`
/// - `set_callback()` — call before `run()`
pub struct KirdiClient {
    handle: *mut kirdi_sys::kirdi_client_t,
    /// Prevent the callback data from being dropped while C++ holds a pointer
    _callback_data: Option<Box<CallbackData>>,
}

// SAFETY: The client handle is used from one thread at a time.
// run() on worker, stop via VpnStopHandle from any thread.
unsafe impl Send for KirdiClient {}

impl KirdiClient {
    /// Create a new VPN client from a JSON config string.
    pub fn new_from_json(json: &str) -> Result<Self, String> {
        let c_json = CString::new(json).map_err(|_| "Invalid JSON string")?;
        let handle = unsafe { kirdi_sys::kirdi_create_json(c_json.as_ptr()) };

        if handle.is_null() {
            return Err("Failed to create kirdi client from JSON (invalid config?)".into());
        }

        Ok(Self {
            handle,
            _callback_data: None,
        })
    }

    /// Create a new VPN client from a Rust config struct.
    pub fn new(config: &KirdiConfig) -> Result<Self, String> {
        let json = serde_json::to_string(config).map_err(|e| e.to_string())?;
        Self::new_from_json(&json)
    }

    /// Extract a thread-safe stop handle.
    ///
    /// Use this to stop the client from another thread while `run()` is blocking.
    /// The stop handle shares the same underlying C handle pointer.
    pub fn stop_handle(&self) -> VpnStopHandle {
        VpnStopHandle {
            handle: self.handle,
        }
    }

    /// Set status callback. Must be called before `run()`.
    ///
    /// The callback receives `(event, json_data)` strings from the C++ VPN core.
    /// Events: "connecting", "authenticating", "connected", "disconnected", "error", "reconnecting"
    pub fn set_callback<F>(&mut self, callback: F)
    where
        F: Fn(&str, &str) + Send + 'static,
    {
        let cb_data = Box::new(CallbackData {
            callback: Box::new(callback),
        });

        let userdata = &*cb_data as *const CallbackData as *mut c_void;

        unsafe {
            kirdi_sys::kirdi_set_callback(self.handle, status_callback_trampoline, userdata);
        }

        // Store to prevent drop — C++ holds a raw pointer to this
        self._callback_data = Some(cb_data);
    }

    /// Run the VPN client. **BLOCKS** until `stop()` is called or an error occurs.
    ///
    /// Call this from a worker thread!
    /// Returns `Ok(())` on clean shutdown, `Err(msg)` on error.
    pub fn run(&self) -> Result<(), String> {
        let result = unsafe { kirdi_sys::kirdi_run(self.handle) };
        if result == 0 {
            Ok(())
        } else {
            Err(format!("kirdi_run returned error code: {}", result))
        }
    }

    /// Stop the VPN client. Thread-safe — can be called from any thread.
    pub fn stop(&self) {
        unsafe { kirdi_sys::kirdi_stop(self.handle) };
    }

    /// Get the library version string.
    pub fn version() -> String {
        unsafe {
            let ver = kirdi_sys::kirdi_version();
            if ver.is_null() {
                return "unknown".into();
            }
            CStr::from_ptr(ver)
                .to_str()
                .unwrap_or("unknown")
                .to_string()
        }
    }
}

impl Drop for KirdiClient {
    fn drop(&mut self) {
        if !self.handle.is_null() {
            unsafe { kirdi_sys::kirdi_destroy(self.handle) };
            self.handle = std::ptr::null_mut();
        }
    }
}
