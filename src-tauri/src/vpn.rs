//! Tauri commands for VPN operations.
//!
//! On macOS, the VPN needs root to create TUN devices.
//! Architecture:
//! 1. Show native macOS password dialog via osascript
//! 2. Run `kirdi-client` binary as root via `sudo -S`
//! 3. Parse stderr log lines in real-time → emit Tauri events
//! 4. On disconnect: SIGTERM the sudo process → propagates to kirdi-client

use serde::{Deserialize, Serialize};
use std::io::{BufRead, BufReader, Write};
use std::process::{Command, Stdio};
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use tauri::{AppHandle, Emitter, State};

// ── Types for frontend communication ────────────────────────────────────────

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct VpnStatusUpdate {
    #[serde(skip_serializing_if = "Option::is_none")]
    pub status: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub error: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub stats: Option<VpnStatsUpdate>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct VpnStatsUpdate {
    #[serde(skip_serializing_if = "Option::is_none")]
    pub server_ip: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub client_ip: Option<String>,
}

// ── Application State ───────────────────────────────────────────────────────

pub struct AppState {
    /// PID of the sudo child process (0 = not running)
    child_pid: AtomicU32,
    running: AtomicBool,
}

impl AppState {
    pub fn new() -> Self {
        Self {
            child_pid: AtomicU32::new(0),
            running: AtomicBool::new(false),
        }
    }
}

impl Default for AppState {
    fn default() -> Self {
        Self::new()
    }
}

// ── Find kirdi-client binary ────────────────────────────────────────────────

fn find_kirdi_client() -> Result<String, String> {
    // 1. Check next to our own binary (bundled app)
    if let Ok(exe) = std::env::current_exe() {
        if let Some(dir) = exe.parent() {
            let candidate = dir.join("kirdi-client");
            if candidate.exists() {
                return Ok(candidate.to_string_lossy().into());
            }
        }
    }

    // 2. Check the build-time path from kirdi-sys
    let build_path = kirdi_sys::CLIENT_BIN_PATH;
    if std::path::Path::new(build_path).exists() {
        return Ok(build_path.into());
    }

    // 3. Check PATH
    if let Ok(output) = Command::new("which").arg("kirdi-client").output() {
        if output.status.success() {
            let path = String::from_utf8_lossy(&output.stdout).trim().to_string();
            if !path.is_empty() {
                return Ok(path);
            }
        }
    }

    Err("kirdi-client binary not found".into())
}

// ── macOS password dialog ───────────────────────────────────────────────────

/// Show native macOS password dialog via osascript.
/// Returns the password entered by the user.
fn prompt_admin_password() -> Result<String, String> {
    let script = r#"
        set pw to text returned of (display dialog "kirdi requires administrator privileges to create a VPN tunnel." with title "kirdi — Authentication" default answer "" with hidden answer buttons {"Cancel", "Authenticate"} default button "Authenticate" with icon caution)
        return pw
    "#;

    let output = Command::new("osascript")
        .args(["-e", script])
        .output()
        .map_err(|e| format!("Failed to show password dialog: {}", e))?;

    if !output.status.success() {
        let err = String::from_utf8_lossy(&output.stderr);
        if err.contains("User canceled") || err.contains("(-128)") {
            return Err("Authentication cancelled by user".into());
        }
        return Err(format!("Password dialog failed: {}", err));
    }

    Ok(String::from_utf8_lossy(&output.stdout).trim().to_string())
}

// ── Log line parsing ────────────────────────────────────────────────────────

/// Parse a kirdi log line and return a status update if relevant.
/// Log format: `HH:MM:SS.mmm LEVEL [file.cpp:line] Message`
fn parse_log_line(line: &str) -> Option<VpnStatusUpdate> {
    // Skip empty lines and sudo noise
    let line = line.trim();
    if line.is_empty() {
        return None;
    }

    // Extract the message part (after the `] ` bracket)
    let msg = if let Some(idx) = line.find("] ") {
        &line[idx + 2..]
    } else {
        line
    };

    let is_error = line.contains(" ERROR ");

    // Match known patterns
    if msg.contains("Connecting to wss://") || msg.contains("Connecting to ws://") {
        Some(VpnStatusUpdate {
            status: Some("connecting".into()),
            error: None,
            stats: None,
        })
    } else if msg.contains("sending auth") {
        Some(VpnStatusUpdate {
            status: Some("connecting".into()),
            error: None,
            stats: None,
        })
    } else if msg.starts_with("Authenticated!") {
        // Parse: "Authenticated! VIP=10.8.0.12 server_tun=10.8.0.1 ..."
        let client_ip = msg.split("VIP=")
            .nth(1)
            .and_then(|s| s.split_whitespace().next())
            .map(String::from);
        let server_ip = msg.split("server_tun=")
            .nth(1)
            .and_then(|s| s.split_whitespace().next())
            .map(String::from);

        Some(VpnStatusUpdate {
            status: Some("connected".into()),
            error: None,
            stats: Some(VpnStatsUpdate { server_ip, client_ip }),
        })
    } else if msg.contains("Client stopping") || msg.contains("Restoring DNS") {
        Some(VpnStatusUpdate {
            status: Some("disconnecting".into()),
            error: None,
            stats: None,
        })
    } else if is_error && msg.contains("Failed to") {
        Some(VpnStatusUpdate {
            status: Some("error".into()),
            error: Some(msg.to_string()),
            stats: None,
        })
    } else if is_error {
        Some(VpnStatusUpdate {
            status: None,
            error: Some(msg.to_string()),
            stats: None,
        })
    } else {
        None
    }
}

// ── Config format conversion ────────────────────────────────────────────────

/// Convert frontend camelCase JSON config to C++ snake_case format.
///
/// Frontend sends:  { "serverHost": "...", "authToken": "...", "useTls": true, ... }
/// C++ expects:     { "server_host": "...", "auth_token": "...", "tls_enabled": true, ... }
fn convert_config_for_cpp(frontend_json: &str) -> Result<String, String> {
    let v: serde_json::Value = serde_json::from_str(frontend_json)
        .map_err(|e| format!("Invalid config JSON: {}", e))?;

    let obj = v.as_object().ok_or("Config must be a JSON object")?;

    let mut cpp = serde_json::Map::new();

    // Direct mappings: camelCase → snake_case
    let mappings: &[(&str, &str)] = &[
        ("serverHost",  "server_host"),
        ("serverPort",  "server_port"),
        ("wsPath",      "ws_path"),
        ("authToken",   "auth_token"),
        ("authUser",    "auth_user"),
        ("useTls",      "tls_enabled"),
        ("autoRoute",   "auto_route"),
        ("dns",         "dns_server"),
        ("mtu",         "mtu"),
        ("sniOverride", "sni_override"),
        ("logLevel",    "log_level"),
    ];

    for (camel, snake) in mappings {
        if let Some(val) = obj.get(*camel) {
            cpp.insert(snake.to_string(), val.clone());
        }
    }

    // Also pass through any snake_case keys that might already be correct
    for (key, val) in obj {
        if !cpp.contains_key(key) {
            // Check if it's already a known snake_case key
            let known_snake = [
                "server_host", "server_port", "ws_path", "auth_token",
                "auth_user", "tls_enabled", "auto_route", "dns_server",
                "mtu", "sni_override", "log_level",
            ];
            if known_snake.contains(&key.as_str()) {
                cpp.insert(key.clone(), val.clone());
            }
        }
    }

    serde_json::to_string_pretty(&serde_json::Value::Object(cpp))
        .map_err(|e| format!("Failed to serialize config: {}", e))
}

// ── Tauri Commands ──────────────────────────────────────────────────────────

/// Connect to VPN server with admin privileges.
///
/// Flow:
/// 1. Show macOS password dialog (osascript)
/// 2. Write config to temp file
/// 3. Spawn `sudo -S -k -p "" kirdi-client config.json` with password on stdin
/// 4. Read stderr in a thread → parse logs → emit Tauri events
/// 5. Store PID for disconnect
#[tauri::command]
pub async fn vpn_connect(
    app: AppHandle,
    state: State<'_, AppState>,
    config_json: String,
) -> Result<(), String> {
    if state.running.load(Ordering::Relaxed) {
        return Err("VPN is already running".into());
    }

    // Find the kirdi-client binary
    let kirdi_bin = find_kirdi_client()?;

    // Emit "connecting" immediately (before password dialog)
    let _ = app.emit("vpn-status", &VpnStatusUpdate {
        status: Some("connecting".into()),
        error: None,
        stats: None,
    });

    // Show password dialog
    let password = prompt_admin_password()?;

    // Convert frontend camelCase config to C++ snake_case format
    let cpp_config = convert_config_for_cpp(&config_json)?;
    let config_path = std::env::temp_dir().join("kirdi-vpn-config.json");
    std::fs::write(&config_path, &cpp_config)
        .map_err(|e| format!("Failed to write config: {}", e))?;

    // Spawn sudo with password on stdin
    // -S: read password from stdin
    // -k: invalidate cached credentials (force fresh auth)
    // -p "": suppress password prompt on stderr
    let mut child = Command::new("sudo")
        .args([
            "-S", "-k", "-p", "",
            &kirdi_bin,
            config_path.to_str().unwrap(),
        ])
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .map_err(|e| format!("Failed to start VPN: {}", e))?;

    // Write password to sudo's stdin and close it
    if let Some(mut stdin) = child.stdin.take() {
        let _ = writeln!(stdin, "{}", password);
        // stdin dropped here → closed
    }
    // Password is no longer in scope after this point
    drop(password);

    // Store PID and mark as running
    let pid = child.id();
    state.child_pid.store(pid, Ordering::Relaxed);
    state.running.store(true, Ordering::Relaxed);

    // Spawn thread to read stderr and parse log lines
    let app_for_logs = app.clone();
    let stderr = child.stderr.take();

    std::thread::spawn(move || {
        if let Some(stderr) = stderr {
            let reader = BufReader::new(stderr);
            for line in reader.lines() {
                match line {
                    Ok(line) => {
                        // Log to console for debugging
                        eprintln!("[kirdi] {}", line);

                        if let Some(update) = parse_log_line(&line) {
                            let _ = app_for_logs.emit("vpn-status", &update);
                        }
                    }
                    Err(_) => break,
                }
            }
        }

        // Process exited — emit final disconnected status
        let _ = app_for_logs.emit("vpn-status", &VpnStatusUpdate {
            status: Some("disconnected".into()),
            error: None,
            stats: None,
        });
    });

    // Spawn another thread to wait for the child and clean up
    let app_for_wait = app.clone();
    std::thread::spawn(move || {
        let status = child.wait();
        // Clean up temp config file
        let _ = std::fs::remove_file(&config_path);

        match status {
            Ok(exit) if !exit.success() => {
                // Non-zero exit — might be a sudo auth failure
                let code = exit.code().unwrap_or(-1);
                if code == 1 {
                    let _ = app_for_wait.emit("vpn-status", &VpnStatusUpdate {
                        status: Some("error".into()),
                        error: Some("Authentication failed (wrong password?)".into()),
                        stats: None,
                    });
                }
            }
            Err(e) => {
                let _ = app_for_wait.emit("vpn-status", &VpnStatusUpdate {
                    status: Some("error".into()),
                    error: Some(format!("VPN process error: {}", e)),
                    stats: None,
                });
            }
            _ => {}
        }
    });

    Ok(())
}

/// Disconnect from VPN server.
/// Sends SIGTERM to the sudo process, which propagates to kirdi-client.
#[tauri::command]
pub async fn vpn_disconnect(
    app: AppHandle,
    state: State<'_, AppState>,
) -> Result<(), String> {
    let pid = state.child_pid.load(Ordering::Relaxed);
    if pid == 0 {
        return Err("VPN is not running".into());
    }

    // Send SIGTERM to the sudo process → propagates to kirdi-client
    unsafe {
        libc::kill(pid as i32, libc::SIGTERM);
    }

    state.child_pid.store(0, Ordering::Relaxed);
    state.running.store(false, Ordering::Relaxed);

    let _ = app.emit("vpn-status", &VpnStatusUpdate {
        status: Some("disconnecting".into()),
        error: None,
        stats: None,
    });

    Ok(())
}

/// Get VPN connection status.
#[tauri::command]
pub fn vpn_status(state: State<'_, AppState>) -> bool {
    state.running.load(Ordering::Relaxed)
}

/// Get kirdi library version.
#[tauri::command]
pub fn vpn_version() -> String {
    kirdi_ffi::KirdiClient::version()
}
