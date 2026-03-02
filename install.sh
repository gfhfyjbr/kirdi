#!/usr/bin/env bash
set -euo pipefail

# ── kirdi server installer ──────────────────────────────────────────────────
# Ставит kirdi-server на Linux VPS (Debian/Ubuntu, RHEL/Fedora/Alma, Arch)
# Запуск:  curl -fsSL https://raw.githubusercontent.com/paranoikcodit/kirdi/main/install.sh | sudo bash
# Или:     wget -qO- https://raw.githubusercontent.com/paranoikcodit/kirdi/main/install.sh | sudo bash

REPO="https://github.com/gfhfyjbr/kirdi.git"
BRANCH="main"
BUILD_DIR="/tmp/kirdi-build"
INSTALL_PREFIX="/usr/local"
CONFIG_DIR="/etc/kirdi"
SYSTEMD_DIR="/etc/systemd/system"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

log()   { echo -e "${GREEN}[kirdi]${NC} $*"; }
warn()  { echo -e "${YELLOW}[kirdi]${NC} $*"; }
err()   { echo -e "${RED}[kirdi]${NC} $*" >&2; }
die()   { err "$@"; exit 1; }

# ── проверка root ────────────────────────────────────────────────────────────
[[ $EUID -eq 0 ]] || die "запусти от root: sudo bash install.sh"

# ── определяем пакетный менеджер ─────────────────────────────────────────────
install_deps() {
    log "ставлю зависимости для сборки..."

    if command -v apt-get &>/dev/null; then
        apt-get update -qq
        apt-get install -y -qq \
            git cmake g++ make \
            libboost-all-dev libssl-dev \
            pkg-config >/dev/null 2>&1
    elif command -v dnf &>/dev/null; then
        dnf install -y -q \
            git cmake gcc-c++ make \
            boost-devel openssl-devel \
            pkg-config >/dev/null 2>&1
    elif command -v yum &>/dev/null; then
        yum install -y -q \
            git cmake3 gcc-c++ make \
            boost-devel openssl-devel \
            pkg-config >/dev/null 2>&1
        # на старых centos cmake может быть cmake3
        if ! command -v cmake &>/dev/null && command -v cmake3 &>/dev/null; then
            ln -sf "$(command -v cmake3)" /usr/local/bin/cmake
        fi
    elif command -v pacman &>/dev/null; then
        pacman -Sy --noconfirm --needed \
            git cmake gcc make \
            boost openssl \
            pkg-config >/dev/null 2>&1
    else
        die "неизвестный пакетный менеджер, ставь зависимости руками: git cmake g++ libboost-dev libssl-dev"
    fi

    log "зависимости установлены"
}

# ── проверяем что cmake достаточно свежий ────────────────────────────────────
check_cmake() {
    if ! command -v cmake &>/dev/null; then
        die "cmake не найден после установки зависимостей"
    fi

    local ver
    ver=$(cmake --version | head -1 | grep -oP '\d+\.\d+' | head -1)
    local major minor
    major=$(echo "$ver" | cut -d. -f1)
    minor=$(echo "$ver" | cut -d. -f2)

    if (( major < 3 || (major == 3 && minor < 20) )); then
        die "нужен cmake >= 3.20, а у тебя $ver. обнови cmake"
    fi

    log "cmake $ver - ок"
}

# ── проверяем поддержку C++23 ────────────────────────────────────────────────
check_compiler() {
    local cxx
    cxx=$(command -v g++ || command -v c++ || true)
    [[ -n "$cxx" ]] || die "компилятор C++ не найден"

    # проверяем что std::expected доступен (C++23)
    local test_file
    test_file=$(mktemp /tmp/kirdi_check_XXXXXX.cpp)
    cat > "$test_file" <<'CXXEOF'
#include <expected>
int main() { std::expected<int,int> e{42}; return e.value() == 42 ? 0 : 1; }
CXXEOF

    if ! "$cxx" -std=c++23 -o /dev/null "$test_file" 2>/dev/null; then
        rm -f "$test_file"
        warn "компилятор $cxx не поддерживает C++23 / std::expected"
        warn "попробую поставить свежий g++..."
        install_modern_gcc
        cxx=$(command -v g++-13 || command -v g++-14 || command -v g++)
        if ! "$cxx" -std=c++23 -o /dev/null "$test_file" 2>/dev/null; then
            rm -f "$test_file"
            die "не удалось получить компилятор с поддержкой C++23. нужен gcc >= 13 или clang >= 17"
        fi
    fi

    rm -f "$test_file"
    log "компилятор поддерживает C++23 - ок"
}

install_modern_gcc() {
    if command -v apt-get &>/dev/null; then
        apt-get install -y -qq g++-13 2>/dev/null || apt-get install -y -qq g++-14 2>/dev/null || true
    elif command -v dnf &>/dev/null; then
        dnf install -y -q gcc-c++ 2>/dev/null || true
    fi
}

# ── клонируем и собираем ─────────────────────────────────────────────────────
build_server() {
    log "клонирую репозиторий..."
    rm -rf "$BUILD_DIR"
    git clone --depth 1 --branch "$BRANCH" "$REPO" "$BUILD_DIR" 2>/dev/null

    log "конфигурирую cmake..."
    cmake -B "$BUILD_DIR/build" \
        -S "$BUILD_DIR" \
        -DCMAKE_BUILD_TYPE=Release \
        -DKIRDI_BUILD_CLIENT=OFF \
        -DKIRDI_BUILD_TESTS=OFF \
        -Wno-dev 2>&1 | tail -5

    local nproc
    nproc=$(nproc 2>/dev/null || echo 2)

    log "собираю kirdi-server (${nproc} потоков)..."
    cmake --build "$BUILD_DIR/build" --target kirdi-server -j"$nproc" 2>&1 | tail -5

    log "сборка завершена"
}

# ── устанавливаем ────────────────────────────────────────────────────────────
install_binary() {
    log "устанавливаю бинарник..."
    install -Dm755 "$BUILD_DIR/build/kirdi-server" "$INSTALL_PREFIX/bin/kirdi-server"
    log "kirdi-server -> $INSTALL_PREFIX/bin/kirdi-server"
}

# ── генерируем конфиг ────────────────────────────────────────────────────────
generate_config() {
    mkdir -p "$CONFIG_DIR"

    if [[ -f "$CONFIG_DIR/server.json" ]]; then
        warn "конфиг $CONFIG_DIR/server.json уже существует, не перезаписываю"
        return
    fi

    # генерируем случайный токен
    local token
    token=$(openssl rand -hex 32)

    # генерируем случайный path
    local ws_path
    ws_path="/$(openssl rand -hex 8)/"

    cat > "$CONFIG_DIR/server.json" <<EOF
{
    "listen_addr": "127.0.0.1",
    "listen_port": 8443,
    "ws_path": "${ws_path}",
    "auth_secret": "${token}",
    "tun_subnet": "10.8.0.0",
    "tun_mask": "255.255.255.0",
    "tun_server_ip": "10.8.0.1",
    "mtu": 1400,
    "keepalive_sec": 25,
    "tls_enabled": false,
    "log_level": "info"
}
EOF

    chmod 600 "$CONFIG_DIR/server.json"

    log "конфиг создан: $CONFIG_DIR/server.json"
    echo ""
    echo -e "  ${CYAN}auth_token:${NC} ${token}"
    echo -e "  ${CYAN}ws_path:${NC}    ${ws_path}"
    echo -e "  ${CYAN}listen:${NC}     127.0.0.1:8443"
    echo ""
    warn "tls_enabled=false - предполагается что перед kirdi стоит nginx с TLS"
}

# ── systemd сервис ───────────────────────────────────────────────────────────
install_service() {
    cat > "$SYSTEMD_DIR/kirdi-server.service" <<'EOF'
[Unit]
Description=kirdi VPN tunnel server
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
ExecStart=/usr/local/bin/kirdi-server /etc/kirdi/server.json
Restart=always
RestartSec=5
LimitNOFILE=65535

# hardening
NoNewPrivileges=no
ProtectSystem=strict
ProtectHome=yes
ReadWritePaths=/dev/net/tun
PrivateTmp=yes

[Install]
WantedBy=multi-user.target
EOF

    systemctl daemon-reload
    systemctl enable kirdi-server.service

    log "systemd сервис установлен и включен"
}

# ── настраиваем NAT и forwarding ─────────────────────────────────────────────
setup_nat() {
    log "настраиваю ip forwarding и NAT..."

    # ip forwarding
    if ! sysctl -n net.ipv4.ip_forward 2>/dev/null | grep -q 1; then
        sysctl -w net.ipv4.ip_forward=1 >/dev/null
        echo "net.ipv4.ip_forward = 1" >> /etc/sysctl.d/99-kirdi.conf
    fi

    # определяем основной интерфейс
    local iface
    iface=$(ip route show default 2>/dev/null | awk '/default/ {print $5}' | head -1)
    [[ -n "$iface" ]] || iface="eth0"

    # FORWARD: разрешаем форвардинг для kirdi TUN
    if ! iptables -C FORWARD -i kirdi0 -j ACCEPT 2>/dev/null; then
        iptables -A FORWARD -i kirdi0 -j ACCEPT
        log "добавлено FORWARD правило: kirdi0 -> ACCEPT"
    fi
    if ! iptables -C FORWARD -o kirdi0 -m state --state RELATED,ESTABLISHED -j ACCEPT 2>/dev/null; then
        iptables -A FORWARD -o kirdi0 -m state --state RELATED,ESTABLISHED -j ACCEPT
        log "добавлено FORWARD правило: -> kirdi0 (RELATED,ESTABLISHED)"
    fi

    # NAT для подсети kirdi
    if ! iptables -t nat -C POSTROUTING -s 10.8.0.0/24 -o "$iface" -j MASQUERADE 2>/dev/null; then
        iptables -t nat -A POSTROUTING -s 10.8.0.0/24 -o "$iface" -j MASQUERADE
        log "добавлено NAT правило: 10.8.0.0/24 -> $iface (MASQUERADE)"
    else
        log "NAT правило уже есть"
    fi

    # сохраняем iptables
    if command -v netfilter-persistent &>/dev/null; then
        netfilter-persistent save 2>/dev/null || true
    elif command -v iptables-save &>/dev/null; then
        iptables-save > /etc/iptables/rules.v4 2>/dev/null || true
    fi

    # TUN device
    if [[ ! -d /dev/net ]]; then
        mkdir -p /dev/net
    fi
    if [[ ! -c /dev/net/tun ]]; then
        mknod /dev/net/tun c 10 200
        chmod 666 /dev/net/tun
    fi

    log "NAT и forwarding настроены"
}

# ── пример nginx конфига ─────────────────────────────────────────────────────
print_nginx_hint() {
    local ws_path
    ws_path=$(grep -oP '"ws_path"\s*:\s*"\K[^"]+' "$CONFIG_DIR/server.json" 2>/dev/null || echo "/tunnel/")

    echo ""
    log "пример nginx конфига для /etc/nginx/sites-available/kirdi:"
    echo ""
    cat <<NGINX
    # внутри server { } блока с ssl
    location ${ws_path} {
        proxy_pass http://127.0.0.1:8443;
        proxy_http_version 1.1;
        proxy_set_header Upgrade \$http_upgrade;
        proxy_set_header Connection "upgrade";
        proxy_set_header Host \$host;
        proxy_set_header X-Real-IP \$remote_addr;
        proxy_read_timeout 86400s;
        proxy_send_timeout 86400s;
    }
NGINX
    echo ""
}

# ── подключение клиента ──────────────────────────────────────────────────────
print_client_hint() {
    local token ws_path
    token=$(grep -oP '"auth_token"\s*:\s*"\K[^"]+' "$CONFIG_DIR/server.json" 2>/dev/null || echo "<TOKEN>")
    ws_path=$(grep -oP '"ws_path"\s*:\s*"\K[^"]+' "$CONFIG_DIR/server.json" 2>/dev/null || echo "/tunnel/")

    echo ""
    log "подключение клиента:"
    echo ""
    echo -e "  ${CYAN}sudo ./kirdi-client --host your-domain.com --token ${token} --path ${ws_path}${NC}"
    echo ""
}

# ── чистим мусор сборки ──────────────────────────────────────────────────────
cleanup() {
    rm -rf "$BUILD_DIR"
    log "временные файлы удалены"
}

# ── main ─────────────────────────────────────────────────────────────────────
main() {
    echo ""
    echo -e "${CYAN}╔══════════════════════════════════════╗${NC}"
    echo -e "${CYAN}║        kirdi server installer        ║${NC}"
    echo -e "${CYAN}╚══════════════════════════════════════╝${NC}"
    echo ""

    install_deps
    check_cmake
    check_compiler
    build_server
    install_binary
    generate_config
    setup_nat

    if command -v systemctl &>/dev/null; then
        install_service
    else
        warn "systemd не найден, сервис не установлен. запускай вручную:"
        echo "  kirdi-server /etc/kirdi/server.json"
    fi

    cleanup
    print_nginx_hint
    print_client_hint

    echo ""
    log "установка завершена!"
    echo ""

    if command -v systemctl &>/dev/null; then
        echo -e "  ${GREEN}sudo systemctl start kirdi-server${NC}   — запустить"
        echo -e "  ${GREEN}sudo systemctl status kirdi-server${NC}  — статус"
        echo -e "  ${GREEN}sudo journalctl -fu kirdi-server${NC}    — логи"
    fi

    echo -e "  ${GREEN}cat /etc/kirdi/server.json${NC}            — конфиг"
    echo ""
}

main "$@"
