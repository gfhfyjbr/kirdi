mod vpn;

use vpn::AppState;

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_opener::init())
        .manage(AppState::new())
        .invoke_handler(tauri::generate_handler![
            vpn::vpn_connect,
            vpn::vpn_disconnect,
            vpn::vpn_status,
            vpn::vpn_version,
        ])
        .run(tauri::generate_context!())
        .expect("error while running kirdi");
}
