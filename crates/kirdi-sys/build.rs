use std::env;

fn main() {
    // ── Detect OpenSSL (macOS Homebrew) ──────────────────────────────────────
    let openssl_root = env::var("OPENSSL_ROOT_DIR")
        .ok()
        .or_else(|| {
            std::process::Command::new("brew")
                .args(["--prefix", "openssl@3"])
                .output()
                .ok()
                .and_then(|o| String::from_utf8(o.stdout).ok())
                .map(|s| s.trim().to_string())
        })
        .or_else(|| {
            std::process::Command::new("brew")
                .args(["--prefix", "openssl"])
                .output()
                .ok()
                .and_then(|o| String::from_utf8(o.stdout).ok())
                .map(|s| s.trim().to_string())
        });

    // ── Build libkirdi via cmake ────────────────────────────────────────────
    let mut cmake_cfg = cmake::Config::new("../..");
    cmake_cfg
        .define("KIRDI_BUILD_TESTS", "OFF")
        .define("KIRDI_BUILD_SERVER", "OFF")
        .define("KIRDI_BUILD_CLIENT", "ON")
        .define("KIRDI_BUILD_GUI", "OFF")
        .define("KIRDI_BUILD_LIB", "ON");

    if let Some(ref ssl_root) = openssl_root {
        cmake_cfg.define("OPENSSL_ROOT_DIR", ssl_root);
    }

    let dst = cmake_cfg.build_target("kirdi").build();
    let build_dir = dst.join("build");

    // ── Also build the kirdi-client CLI binary ──────────────────────────────
    let _client_build_status = std::process::Command::new("cmake")
        .args([
            "--build",
            build_dir.to_str().unwrap(),
            "--target",
            "kirdi-client",
        ])
        .status();

    // Export client binary path (available via env!() in this crate)
    let client_bin = build_dir.join("kirdi-client");
    if client_bin.exists() {
        println!("cargo:rustc-env=KIRDI_CLIENT_BIN={}", client_bin.display());
    } else {
        // Might be in a subdirectory on some cmake generators
        let alt = build_dir.join("src/client/kirdi-client");
        if alt.exists() {
            println!("cargo:rustc-env=KIRDI_CLIENT_BIN={}", alt.display());
        } else {
            eprintln!("cargo:warning=kirdi-client binary not found after cmake build");
            // Set a fallback so compilation doesn't fail
            println!("cargo:rustc-env=KIRDI_CLIENT_BIN=kirdi-client");
        }
    }

    // Also expose as DEP metadata for downstream crates
    println!("cargo:client_bin={}", client_bin.display());

    // ── Tell cargo where to find the static libraries ───────────────────────
    println!("cargo:rustc-link-search=native={}", build_dir.display());

    // Check common subdirectories where cmake might put libs
    for subdir in &["", "src/libkirdi", "src/common", "src/tun", "src/transport"] {
        let path = build_dir.join(subdir);
        if path.exists() {
            println!("cargo:rustc-link-search=native={}", path.display());
        }
    }

    // Link our static libraries (order matters for static linking!)
    println!("cargo:rustc-link-lib=static=kirdi");
    println!("cargo:rustc-link-lib=static=kirdi_common");
    println!("cargo:rustc-link-lib=static=kirdi_tun");
    println!("cargo:rustc-link-lib=static=kirdi_transport");

    // ── System dependencies: OpenSSL ────────────────────────────────────────
    let target_os = env::var("CARGO_CFG_TARGET_OS").unwrap_or_default();
    if let Some(ref dir) = openssl_root {
        println!("cargo:rustc-link-search=native={}/lib", dir);
    }
    println!("cargo:rustc-link-lib=dylib=ssl");
    println!("cargo:rustc-link-lib=dylib=crypto");

    // ── Platform-specific C++ stdlib and frameworks ─────────────────────────
    match target_os.as_str() {
        "macos" => {
            println!("cargo:rustc-link-lib=dylib=c++");
            println!("cargo:rustc-link-lib=framework=Security");
            println!("cargo:rustc-link-lib=framework=CoreFoundation");
            println!("cargo:rustc-link-lib=framework=SystemConfiguration");
        }
        "linux" => {
            println!("cargo:rustc-link-lib=dylib=stdc++");
        }
        "windows" => {
            println!("cargo:rustc-link-lib=dylib=ws2_32");
            println!("cargo:rustc-link-lib=dylib=bcrypt");
        }
        _ => {}
    }

    // ── Rerun triggers ──────────────────────────────────────────────────────
    println!("cargo:rerun-if-changed=../../src/");
    println!("cargo:rerun-if-changed=../../include/");
    println!("cargo:rerun-if-changed=../../CMakeLists.txt");
}
