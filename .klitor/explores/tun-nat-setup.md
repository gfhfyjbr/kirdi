# TUN и NAT настройка в kirdi

> **Запрос:** Исследуй настройку TUN и NAT в проекте: tun_linux.cpp, tun_interface.hpp, install.sh, server.cpp — iptables правила, sysctl, TUN IP, NAT/MASQUERADE, MTU, системные вызовы
> **Дата:** 2026-03-02

## Обзор

kirdi — VPN-туннель поверх WebSocket. Сервер создаёт TUN-интерфейс `kirdi0` через `/dev/net/tun` + ioctl, настраивает IP/маску/MTU через POSIX ioctl-вызовы, а NAT/ip_forward настраивается **только скриптом install.sh** — сам сервер никаких системных команд (`iptables`, `sysctl`, `ip`) при старте не выполняет. Это ключевая архитектурная особенность с потенциальными проблемами.

## Структура

- `src/tun/tun_interface.hpp` — абстрактный интерфейс `TunDevice`, структура `TunConfig` (name, address, peer_address, netmask, mtu)
- `src/tun/tun_interface.cpp` — фабрика `create_tun_device()`, выбирает платформенную реализацию
- `src/tun/tun_linux.hpp` — объявление `LinuxTunDevice`
- `src/tun/tun_linux.cpp` — реализация: open/close/read/write + `configure_address()`
- `src/server/server.cpp` — создаёт TUN при старте, управляет сессиями, роутит пакеты
- `src/server/server.hpp` — `Server` класс, `next_client_ip_offset_` начинается с **2**
- `src/common/config.hpp` — `ServerConfig` со всеми дефолтами
- `src/common/config.cpp` — JSON-парсинг конфига
- `install.sh` — bash-инсталлятор: сборка, конфиг, systemd, NAT, ip_forward

## Ключевые находки

### Системные вызовы при создании TUN (tun_linux.cpp)

1. **`open("/dev/net/tun", O_RDWR)`** — `tun_linux.cpp:25` — открывает TUN-устройство
2. **`ioctl(fd_, TUNSETIFF, &ifr)`** — `tun_linux.cpp:43` — флаги `IFF_TUN | IFF_NO_PI` (TUN-режим, без 4-байтового PI-заголовка), имя интерфейса `kirdi0`
3. **`fcntl(fd_, F_SETFL, flags | O_NONBLOCK)`** — `tun_linux.cpp:57` — переводит fd в неблокирующий режим
4. **`socket(AF_INET, SOCK_DGRAM, 0)`** — `tun_linux.cpp:107` — вспомогательный сокет для ioctl-конфигурации
5. **`ioctl(sockfd, SIOCSIFADDR, &ifr)`** — `tun_linux.cpp:121` — устанавливает IP-адрес (из `config.address`, дефолт `10.8.0.1`)
6. **`ioctl(sockfd, SIOCSIFNETMASK, &ifr)`** — `tun_linux.cpp:132` — устанавливает маску (из `config.netmask`, дефолт `255.255.255.0`)
7. **`ioctl(sockfd, SIOCSIFMTU, &ifr)`** — `tun_linux.cpp:140` — устанавливает MTU (из `config.mtu`, дефолт **1400**)
8. **`ioctl(sockfd, SIOCGIFFLAGS, &ifr)`** + **`ioctl(sockfd, SIOCSIFFLAGS, &ifr)`** — `tun_linux.cpp:147-155` — читает флаги, добавляет `IFF_UP | IFF_RUNNING`, поднимает интерфейс

### MTU

9. **MTU = 1400** — `src/common/config.hpp:28`, `src/tun/tun_interface.hpp:29` — дефолт в двух местах. В конфиге install.sh тоже `"mtu": 1400` (`install.sh:179`). Комментарий: "leave room for WS + TLS overhead". Буфер чтения: `mtu_ + 64` (`tun_linux.cpp:79`).

### TUN IP на сервере

10. **Имя интерфейса** — `server.cpp:57` — жёстко задано `"kirdi0"`
11. **IP сервера** — `server.cpp:58` — берётся из `config_.tun_server_ip`, дефолт `10.8.0.1` (`config.hpp:27`)
12. **Маска** — `server.cpp:59` — из `config_.tun_mask`, дефолт `255.255.255.0`
13. **Подсеть клиентов** — `server.hpp:50` — `next_client_ip_offset_` стартует с **2**, т.е. первый клиент получает `10.8.0.2`, второй `10.8.0.3` и т.д. (offset прибавляется к base subnet `10.8.0.0`)

### iptables правила (install.sh)

14. **Единственное правило NAT** — `install.sh:245-247`:
    ```
    iptables -t nat -A POSTROUTING -s 10.8.0.0/24 -o <iface> -j MASQUERADE
    ```
    где `<iface>` — основной интерфейс из `ip route show default` (fallback: `eth0`)

15. **Проверка перед добавлением** — `install.sh:245` — `iptables -t nat -C ...` — идемпотентно, не дублирует правило

16. **Сохранение правил** — `install.sh:253-257` — пробует `netfilter-persistent save`, затем `iptables-save > /etc/iptables/rules.v4`. Оба через `|| true` — **ошибки игнорируются**, правила могут не сохраниться после перезагрузки

17. **FORWARD правила отсутствуют** — `install.sh:229-268` — нет `iptables -A FORWARD -i kirdi0 -j ACCEPT` и нет `iptables -A FORWARD -o kirdi0 -j ACCEPT`. Если на сервере стоит firewall с политикой `FORWARD DROP` — трафик не пройдёт даже при работающем MASQUERADE

### sysctl настройки (install.sh)

18. **ip_forward** — `install.sh:234-237`:
    ```bash
    sysctl -w net.ipv4.ip_forward=1
    echo "net.ipv4.ip_forward = 1" >> /etc/sysctl.d/99-kirdi.conf
    ```
    Применяется только если текущее значение != 1. Персистентно через `/etc/sysctl.d/99-kirdi.conf`.

19. **Только IPv4 forwarding** — нет `net.ipv6.conf.all.forwarding`. IPv6 не поддерживается.

### systemd unit (install.sh:199-226)

20. **`NoNewPrivileges=no`** — `install.sh:213` — процесс может получать новые привилегии (нужно для CAP_NET_ADMIN)
21. **`ProtectSystem=strict`** — `install.sh:214` — файловая система read-only, кроме явных исключений
22. **`ReadWritePaths=/dev/net/tun`** — `install.sh:216` — явно разрешает доступ к TUN-устройству
23. **Нет `AmbientCapabilities=CAP_NET_ADMIN`** — сервис запускается от root (нет `User=`), поэтому CAP_NET_ADMIN есть неявно. Но если кто-то добавит `User=kirdi` без capabilities — TUN сломается

### Сервер НЕ выполняет системные команды

24. **Нет `system()`, `popen()`, `exec*()`** — `server.cpp` (весь файл) — сервер не вызывает `iptables`, `ip addr`, `sysctl` при старте. Вся сетевая конфигурация — только через ioctl. NAT и ip_forward должны быть настроены заранее (install.sh или вручную).

## Data Flow

```
install.sh (один раз при деплое):
  sysctl -w net.ipv4.ip_forward=1
  iptables -t nat -A POSTROUTING -s 10.8.0.0/24 -o eth0 -j MASQUERADE
  mknod /dev/net/tun c 10 200  (если нет)

kirdi-server старт:
  open("/dev/net/tun", O_RDWR)
  ioctl(TUNSETIFF, IFF_TUN | IFF_NO_PI, name="kirdi0")
  fcntl(O_NONBLOCK)
  socket(AF_INET, SOCK_DGRAM)
  ioctl(SIOCSIFADDR,   addr=10.8.0.1)
  ioctl(SIOCSIFNETMASK, mask=255.255.255.0)
  ioctl(SIOCSIFMTU,    mtu=1400)
  ioctl(SIOCGIFFLAGS) → ioctl(SIOCSIFFLAGS, IFF_UP|IFF_RUNNING)

Клиент подключается:
  allocate_client_ip(): 10.8.0.0 + offset(2,3,4...) → 10.8.0.2, 10.8.0.3...
  ClientSession создаётся, ip_to_session_[client_ip] = sid

Пакет от клиента → TUN:
  WsSession → on_ip_packet → tun_->write_packet(pkt)
  Ядро: kirdi0 → routing → MASQUERADE → eth0 → интернет

Пакет из интернета → клиенту:
  eth0 → ядро → kirdi0 → tun_read_loop() → read_packet()
  route_to_client(): парсит dst IP из IP-заголовка (offset 16-19)
  ip_to_session_.find(dst_ip) → session->send_ip_packet()
```

## Зависимости

- `server.cpp` → `tun_interface.hpp` → `tun_linux.cpp` (платформа)
- `server.cpp` → `config.hpp` — все параметры TUN берутся из конфига
- `install.sh` → системные утилиты: `iptables`, `sysctl`, `ip`, `mknod`
- NAT работает **только если** install.sh был запущен до старта сервера

## Проблемы и заметки

### 🔴 КРИТИЧНО: Нет FORWARD правил в iptables

`install.sh` добавляет только MASQUERADE в POSTROUTING, но **не добавляет** правила в цепочку FORWARD:
```bash
# ЭТОГО НЕТ в install.sh:
iptables -A FORWARD -i kirdi0 -j ACCEPT
iptables -A FORWARD -o kirdi0 -j ACCEPT
```
На большинстве VPS (Ubuntu с ufw, Debian с nftables, CentOS с firewalld) политика FORWARD по умолчанию — **DROP**. Без этих правил NAT не работает: пакеты от клиентов будут дропаться ядром до POSTROUTING. Трафик через туннель не пойдёт.

### 🔴 КРИТИЧНО: Сохранение iptables через `|| true`

```bash
iptables-save > /etc/iptables/rules.v4 2>/dev/null || true
```
Ошибки игнорируются. Если `/etc/iptables/` не существует — правила не сохранятся. После перезагрузки NAT пропадёт.

### 🟡 ВАЖНО: ip_forward проверяется некорректно

```bash
if ! sysctl -n net.ipv4.ip_forward 2>/dev/null | grep -q 1; then
```
Это сработает если значение ровно `1`. Но если вывод `sysctl` содержит `10` или `11` — `grep -q 1` тоже вернёт true и блок пропустится. Маловероятно, но паттерн хрупкий. Надёжнее: `[ "$(sysctl -n net.ipv4.ip_forward)" = "1" ]`.

### 🟡 ВАЖНО: peer_address не используется на Linux

`TunConfig.peer_address` — `tun_interface.hpp:27` — поле есть, но `tun_linux.cpp` его **не использует** нигде. Это поле нужно для macOS utun (point-to-point). На Linux TUN работает как обычный L3-интерфейс, peer_address не нужен — это нормально, но поле висит неиспользованным.

### 🟡 ВАЖНО: Аллокация IP без проверки диапазона

`server.hpp:50` — `next_client_ip_offset_` атомарно инкрементируется без верхней границы. При 254+ клиентах (offset >= 255 для /24) IP выйдет за пределы подсети. Нет проверки на `max_clients` при аллокации IP.

### 🟡 ВАЖНО: systemd ProtectSystem=strict + ReadWritePaths только /dev/net/tun

`install.sh:214,216` — `ProtectSystem=strict` делает всю ФС read-only. `ReadWritePaths=/dev/net/tun` разрешает только TUN. Но сервер пишет логи — если логи идут в файл (не journald), это сломается. Пока логи идут в stdout → journald — ок.

### 🟢 ОК: MTU согласован

MTU=1400 задан в трёх местах: `config.hpp:28`, `tun_interface.hpp:29`, `install.sh:179` — все согласованы. Оставляет ~124 байта на WS-фреймирование + TLS overhead (при стандартном Ethernet MTU 1500).

### 🟢 ОК: IFF_NO_PI

`tun_linux.cpp:37` — `IFF_NO_PI` убирает 4-байтовый packet info заголовок. Сервер читает чистые IP-пакеты, что правильно для L3 VPN.

### 🟢 ОК: Неблокирующий режим

`tun_linux.cpp:56-57` — fd переводится в O_NONBLOCK. `read_packet()` корректно обрабатывает `EAGAIN/EWOULDBLOCK` возвращая пустой вектор (`tun_linux.cpp:83-84`).

## Выводы

1. **TUN создаётся корректно** через стандартный Linux ioctl API: `TUNSETIFF(IFF_TUN|IFF_NO_PI)` → `SIOCSIFADDR` → `SIOCSIFNETMASK` → `SIOCSIFMTU` → `SIOCSIFFLAGS(IFF_UP|IFF_RUNNING)`. MTU=1400, интерфейс `kirdi0`, IP `10.8.0.1/24`.

2. **NAT настраивается только install.sh**, сервер сам ничего не делает с iptables/sysctl. Это означает: если сервер запущен без предварительного запуска install.sh — NAT не работает.

3. **Критическая проблема: отсутствуют FORWARD правила**. `iptables -t nat -A POSTROUTING ... -j MASQUERADE` без `FORWARD ACCEPT` не работает на системах с DROP-политикой по умолчанию. Это самая вероятная причина "NAT не работает".

4. **ip_forward включается** через `sysctl -w net.ipv4.ip_forward=1` и персистируется в `/etc/sysctl.d/99-kirdi.conf`. Это сделано правильно.

5. **Сохранение iptables ненадёжно** — через `|| true`, может не сохраниться после ребута.
