# Client Packet Flow

> **Запрос:** Исследуй клиентскую часть VPN — полный путь пакета на стороне клиента: TUN→WS и WS→TUN, маршрутизация, MTU, ошибки, аутентификация.
> **Дата:** 2026-03-02

## Обзор

Kirdi — VPN-клиент на C++, использующий macOS utun + WebSocket over TLS (Boost.Beast). Клиент читает сырые IP-пакеты из utun, оборачивает их в 5-байтовый бинарный фрейм и отправляет на сервер через WSS. Обратный путь симметричен. Аутентификация — HMAC-SHA256 с временной меткой (защита от replay-атак).

## Структура

- `src/client/main.cpp` — точка входа, парсинг CLI/конфига, сигналы SIGINT/SIGTERM
- `src/client/client.hpp` — объявление класса `Client`, поля состояния
- `src/client/client.cpp` — вся логика клиента: connect, auth, TUN setup, route management, read loop
- `src/tun/tun_interface.hpp` — абстрактный интерфейс `TunDevice`, структура `TunConfig`
- `src/tun/tun_macos.hpp` — объявление `MacOSTunDevice`
- `src/tun/tun_macos.cpp` — реализация utun: open/close/read/write + configure via ifconfig
- `src/transport/ws_transport.hpp` — интерфейсы `WsClientTransport`, `IWsSession`
- `src/transport/ws_transport.cpp` — async Boost.Beast WSS клиент + серверные сессии
- `src/common/protocol.hpp` — wire-формат: 5-байтовый заголовок + payload, типы сообщений
- `src/common/protocol.cpp` — заглушка (логика inline в .hpp)
- `src/common/crypto.hpp` — объявления HMAC-SHA256, build/verify auth token
- `src/common/crypto.cpp` — реализация через OpenSSL
- `src/common/config.hpp` — структуры `ClientConfig`, `ServerConfig`

## Ключевые находки

1. **TUN read loop в отдельном потоке** — `src/client/client.cpp:170` — `std::thread tun_thread([this]() { tun_read_loop(); }); tun_thread.detach();` — поток детачится, нет join при shutdown
2. **Polling вместо epoll/kqueue** — `src/client/client.cpp:212-229` — TUN читается в busy-loop с `sleep_for(1ms)` при пустом результате; нет интеграции с io_context через `native_fd()`
3. **MTU по умолчанию 1400** — `src/tun/tun_interface.hpp:29` и `src/common/config.hpp:28,57` — оставляет 100 байт на WS+TLS overhead от стандартного 1500 MTU
4. **Нет проверки размера пакета перед отправкой** — `src/client/client.cpp:225-228` — `build_ip_packet(pkt)` бросит исключение если `pkt.size() > 65535`, но это не перехватывается в `tun_read_loop`
5. **Маршрутизация через system()** — `src/client/client.cpp:271-290` — все route-команды через `std::system()`, нет проверки успеха для catch-all маршрутов (только для exclude-маршрута)
6. **Аутентификация: HMAC-SHA256 с timestamp** — `src/common/crypto.cpp:73-83` — `data = timestamp_hex + ":" + user`, HMAC ключ = shared secret, результат hex-encoded
7. **TUN конфигурируется через ifconfig** — `src/tun/tun_macos.cpp:162-174` — `ifconfig utunN inet LOCAL PEER netmask MASK mtu MTU up` через `std::system()`
8. **utun 4-байтовый AF-заголовок** — `src/tun/tun_macos.cpp:21-23,117-118,137-140` — при чтении стрипается, при записи prepend'ится; определяется по IP-версии из первого байта
9. **WS write queue с mutex** — `src/transport/ws_transport.cpp:109-118` — очередь `std::queue<vector<uint8_t>>` с `std::mutex`, запись через `net::post` в executor WS-стрима
10. **Нет reconnect логики** — `src/client/client.cpp:205-207` — `on_error` только логирует, клиент не переподключается
11. **DNS не применяется** — `src/common/config.hpp:59` — поле `dns_server` есть в конфиге, но в `client.cpp` нигде не используется (не выставляется через `scutil` или `/etc/resolv.conf`)
12. **Нет фрагментации** — `src/common/protocol.cpp:8-9` — комментарий "Future: packet reassembly for fragmented IP packets" — не реализовано

## Data Flow

### Исходящий (приложение → сервер)

```
Приложение (TCP/UDP/ICMP)
    │
    ▼ (ядро macOS маршрутизирует через utunN)
utun kernel interface
    │
    ▼ ::read(fd_, buf, mtu+4+64)          [tun_macos.cpp:104]
[4-byte AF header][raw IP packet]
    │
    ▼ strip AF header                      [tun_macos.cpp:118]
std::vector<uint8_t> ip_pkt
    │
    ▼ tun_read_loop()                      [client.cpp:213]
    │
    ▼ protocol::build_ip_packet(pkt)       [client.cpp:225]
[0x03][len_be32][raw IP packet]  ← 5-byte header + payload
    │
    ▼ ws_->send(ws_pkt)                    [client.cpp:227]
    │
    ▼ write_queue_.push() + net::post      [ws_transport.cpp:113-116]
    │
    ▼ ws_->async_write(net::buffer(data))  [ws_transport.cpp:155]
    │
    ▼ Boost.Beast WSS frame (binary)
    │
    ▼ TLS 1.2+ encrypted
    │
    ▼ TCP → сервер
```

### Входящий (сервер → приложение)

```
TCP ← сервер
    │
    ▼ TLS decrypt
    │
    ▼ ws_->async_read(read_buf_)           [ws_transport.cpp:123]
    │
    ▼ on_read() → dispatch_packet()        [ws_transport.cpp:136]
    │
    ▼ deserialize_header({buf_data, size}) [ws_transport.cpp:14]
    │  [1 byte type][4 bytes len BE]
    │
    ▼ on_packet_ callback → Client::on_packet() [client.cpp:136]
    │
    ▼ case MsgType::IpPacket               [client.cpp:184]
    │
    ▼ tun_->write_packet(payload.data(), payload.size()) [client.cpp:186]
    │
    ▼ prepend 4-byte AF header             [tun_macos.cpp:138-140]
    │  af = UTUN_AF_INET(2) или UTUN_AF_INET6(30) по IP-версии
    │
    ▼ ::write(fd_, buf, 4+len)             [tun_macos.cpp:142]
    │
    ▼ utun kernel interface
    │
    ▼ Приложение получает пакет
```

## Маршрутизация (macOS)

Последовательность команд при `auto_route = true`:

```
1. Перед подключением:
   server_ip_ = resolve_host(config_.server_host)   // DNS lookup
   original_gateway_ = popen("route -n get default | grep gateway | awk '{print $2}'")

2. После успешной аутентификации (on_packet AuthResponse):
   tun_->open(tun_cfg)  →  ifconfig utunN inet 10.8.0.2 10.8.0.1 netmask 255.255.255.0 mtu 1400 up

3. setup_routes() [client.cpp:234]:
   a) route -n add -host <server_ip> <original_gateway>   ← КРИТИЧНО, abort если fail
   b) route -n add -net 0.0.0.0/1 -interface utunN
   c) route -n add -net 128.0.0.0/1 -interface utunN

4. teardown_routes() при stop():
   route -n delete -net 0.0.0.0/1
   route -n delete -net 128.0.0.0/1
   route -n delete -host <server_ip>
```

Трюк с двумя /1 маршрутами (0.0.0.0/1 + 128.0.0.0/1) покрывает весь IPv4-адресный пространство более специфичными маршрутами чем дефолтный /0, не удаляя его.

## Зависимости

```
main.cpp
  └── Client (client.cpp)
        ├── WsClientTransport (ws_transport.cpp)
        │     └── protocol:: (protocol.hpp — inline)
        ├── MacOSTunDevice (tun_macos.cpp)
        │     └── TunDevice interface (tun_interface.hpp)
        ├── crypto:: (crypto.cpp → OpenSSL)
        └── config:: (config.cpp → nlohmann/json)
```

**Внешние зависимости:**
- Boost.Asio + Boost.Beast — async I/O и WebSocket
- OpenSSL — TLS + HMAC-SHA256
- nlohmann/json — сериализация auth/config JSON

## Проблемы и заметки

### 🔴 Критические

1. **Detached thread без join** — `client.cpp:171` — `tun_thread.detach()` при `stop()` нет ожидания завершения потока. Если `tun_read_loop` обращается к `ws_` после его уничтожения — UB.

2. **Исключение в tun_read_loop не перехватывается** — `client.cpp:209-232` — если `build_ip_packet` бросит (пакет > 65535 байт), поток упадёт молча. Нет try/catch вокруг тела цикла.

3. **Нет reconnect** — `client.cpp:205-207` — при обрыве соединения клиент просто логирует ошибку. Маршруты остаются настроенными (трафик уходит в никуда), TUN остаётся открытым.

### 🟡 Потенциальные проблемы

4. **DNS не применяется** — `config.hpp:59` — поле `dns_server` есть, но нигде не используется. Через туннель DNS-запросы пойдут, но системный резолвер не переключается.

5. **Polling TUN** — `client.cpp:215` — `sleep_for(1ms)` при пустом read. Нет интеграции с kqueue/epoll через `native_fd()`. При высоком трафике — лишние задержки; при простое — CPU wake-ups каждую мс.

6. **Race condition на write_queue_** — `ws_transport.cpp:109-118` — `writing_` флаг не атомарный, защищён только `write_mutex_`. Но `net::post` выполняется в executor WS-стрима (strand), поэтому фактически безопасно — но неочевидно.

7. **system() для route/ifconfig** — `client.cpp:250,271,281,285` — нет санитизации входных данных (server_ip_, original_gateway_, iface). Если сервер вернёт вредоносный `tun_server_ip` или имя интерфейса — command injection. Хотя `if_name_` берётся из `getsockopt(UTUN_OPT_IFNAME)` (безопасно), `server_ip_` и `original_gateway_` — из DNS и popen.

8. **MTU фрагментация** — `tun_interface.hpp:29` — MTU=1400 по умолчанию. WS frame overhead: ~2-10 байт. TLS record overhead: ~29 байт. TCP/IP: 40 байт. Итого ~80 байт overhead. 1400 + 80 = 1480 < 1500 — в норме. Но если физический MTU меньше 1500 (например, PPPoE = 1492), возможна фрагментация на уровне TCP.

9. **Нет проверки timestamp на клиенте** — `client.cpp:115-117` — timestamp берётся из `system_clock`, но нет защиты от clock skew > 30 секунд (сервер отклонит токен).

### 🟢 Хорошо сделано

- Exclude-маршрут для server_ip добавляется **до** catch-all маршрутов и при ошибке abort — правильная защита от routing loop
- HMAC с временным окном ±30 сек — защита от replay
- Constant-time сравнение токенов (`CRYPTO_memcmp`) — защита от timing attacks
- `teardown_routes()` вызывается в `stop()` с `try/catch` — маршруты чистятся даже при исключениях

## Аутентификация — детали

**Что клиент отправляет серверу** (`client.cpp:114-133`):

```json
{
  "user": "<config.auth_user>",
  "token": "<HMAC-SHA256 hex>",
  "timestamp": <unix_seconds>
}
```

Обёрнуто в wire-фрейм: `[0x01][len_be32][json_bytes]`

**Как строится токен** (`crypto.cpp:73-83`):
```
data   = sprintf("%016llx", timestamp) + ":" + user
token  = hex(HMAC-SHA256(key=auth_secret, data=data))
```

**Что ожидает в ответ** (`client.cpp:138-181`):
```json
{
  "ok": true,
  "tun_ip": "10.8.0.2",
  "tun_server_ip": "10.8.0.1",
  "tun_mask": "255.255.255.0",
  "mtu": 1400
}
```
При `ok=false` — `stop()`.

## Выводы

Клиент реализует полный VPN-туннель через WSS с корректной логикой маршрутизации (split-tunnel trick через два /1 маршрута). Основные риски: отсутствие reconnect при обрыве, detached TUN-поток без синхронизации при shutdown, polling вместо event-driven TUN-чтения, и неприменение DNS-сервера из конфига. Аутентификация криптографически корректна.
