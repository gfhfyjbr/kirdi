# VPN Server — Полный путь пакета

> **Запрос:** Исследуй серверную часть VPN: полный data flow клиент→интернет и обратно, потенциальные проблемы, NAT/iptables, authenticated_ проверка.
> **Дата:** 2026-03-02

## Обзор

Kirdi — WebSocket-based VPN сервер на C++23 (Boost.Beast + Boost.Asio). Клиент подключается по WSS/WS, аутентифицируется, после чего IP-пакеты туннелируются через WebSocket в TUN-устройство (`kirdi0`) и обратно. NAT настраивается внешним скриптом (`install.sh`), сам сервер iptables не трогает.

## Структура

- `src/server/main.cpp` — точка входа, парсинг конфига, запуск `Server::run()`
- `src/server/server.cpp` / `server.hpp` — ядро: accept loop, TUN read loop, routing таблица `ip_to_session_`
- `src/server/session.cpp` / `session.hpp` — `ClientSession`: обработка протокольных сообщений, auth state
- `src/transport/ws_transport.cpp` / `ws_transport.hpp` — WebSocket I/O (SSL и plain), очередь записи, `dispatch_packet()`
- `src/common/protocol.hpp` — wire protocol: 5-байтовый заголовок + payload; `build_packet()`, `deserialize_header()`, `ip4_dst()`
- `src/common/protocol.cpp` — заглушка (логика в header-only инлайнах)
- `src/tun/tun_linux.cpp` — Linux TUN: `/dev/net/tun`, `IFF_TUN | IFF_NO_PI`, non-blocking read/write
- `src/tun/tun_interface.hpp` — абстракция `TunDevice` (open/close/read_packet/write_packet)
- `install.sh` — bash-инсталлятор: NAT (`iptables MASQUERADE`), `ip_forward`, systemd unit

---

## Data Flow: Клиент → Интернет

```
[Клиент]
  │  IP-пакет (raw IPv4)
  │  Оборачивается в kirdi-фрейм: [0x03][len 4B][raw IP]
  │  Отправляется как бинарный WebSocket frame
  ▼
[WsServerSession / WsPlainServerSession]
  ws_transport.cpp:215-234  (SSL)  / :303-323 (plain)
  async_read() → on_read() → dispatch_packet()
  │
  dispatch_packet() [ws_transport.cpp:7-23]:
  │  1. Проверяет buf_size >= HEADER_SIZE (5 байт)
  │  2. deserialize_header() → PacketHeader{type=0x03, length=N}
  │  3. Проверяет buf_size >= HEADER_SIZE + hdr.length
  │  4. Копирует payload в vector<uint8_t>
  │  5. Вызывает on_packet_ callback
  ▼
[ClientSession::handle_packet()]  session.cpp:29-84
  switch(hdr.type):
  case IpPacket (0x03):
  │  1. Проверяет authenticated_ == true  [session.cpp:54-57]
  │     → если false: DROP, LOG_WARN, return
  │  2. Вызывает on_ip_packet_(id_, payload)  [session.cpp:59-61]
  ▼
[Server::register_session() lambda]  server.cpp:198-202
  on_ip_packet callback:
  │  1. Проверяет tun_ && tun_->is_open()
  │  2. tun_->write_packet(pkt.data(), pkt.size())
  ▼
[LinuxTunDevice::write_packet()]  tun_linux.cpp:94-104
  ::write(fd_, data, len)  → /dev/net/tun (kirdi0)
  │  EAGAIN/EWOULDBLOCK → возвращает 0 (пакет ТЕРЯЕТСЯ)
  │  Ошибка → TunError::WriteFailed (пакет ТЕРЯЕТСЯ)
  ▼
[Linux kernel: kirdi0 TUN interface]
  Пакет попадает в сетевой стек ядра
  src IP = виртуальный IP клиента (10.8.0.x)
  ▼
[iptables POSTROUTING MASQUERADE]  install.sh:245-247
  -s 10.8.0.0/24 -o eth0 -j MASQUERADE
  src IP заменяется на внешний IP сервера
  ▼
[Интернет]
```

---

## Data Flow: Интернет → Клиент (обратный путь)

```
[Интернет]
  Ответный пакет приходит на внешний IP сервера
  ▼
[iptables MASQUERADE conntrack]
  Восстанавливает dst IP → виртуальный IP клиента (10.8.0.x)
  ▼
[Linux kernel → kirdi0 TUN interface]
  Пакет с dst=10.8.0.x направляется в TUN
  ▼
[Server::tun_read_loop()]  server.cpp:217-237
  Отдельный detached thread (server.cpp:69-70)
  while(tun_->is_open()):
  │  tun_->read_packet()  → LinuxTunDevice::read_packet()
  │  tun_linux.cpp:78-92: ::read(fd_, buf, mtu+64)
  │    EAGAIN → возвращает пустой vector → sleep(1ms) → continue
  │    Ошибка → TunError::ReadFailed → sleep(1ms) → continue
  │  Если pkt.empty() → sleep(1ms) → continue
  │  route_to_client(pkt.data(), pkt.size())
  ▼
[Server::route_to_client()]  server.cpp:239-261
  1. Проверяет len >= 20 (минимальный IPv4 заголовок)
     → если < 20: DROP (return)
  2. dst_ip = protocol::ip4_dst({data, len})
     protocol.hpp:138-143: memcpy из байт [16..19] пакета (network byte order)
  3. lock(sessions_mutex_)
  4. ip_to_session_.find(dst_ip)
     → не найдено: LOG_DEBUG, DROP
  5. sessions_.find(session_id)
  6. sit->second->is_authenticated()
     → false: DROP (пакет теряется без лога!)
  7. sit->second->send_ip_packet(data, len)
  ▼
[ClientSession::send_ip_packet()]  session.cpp:86-89
  protocol::build_ip_packet({data, len})
  → build_packet(MsgType::IpPacket, ip_data)
  → vector: [0x03][len 4B big-endian][raw IP]
  ws_->send(std::move(pkt))
  ▼
[WsServerSession::send()]  ws_transport.cpp:204-213
  lock(write_mutex_)
  write_queue_.push(data)
  если !writing_: net::post(executor, do_write)
  ▼
[WsServerSession::do_write() / on_write()]  ws_transport.cpp:237-264
  ws_.async_write(net::buffer(data), ...)
  on_write: если ошибка → connected_=false, LOG_WARN, STOP (очередь не дренируется)
  ▼
[Клиент получает бинарный WebSocket frame]
```

---

## Wire Protocol

```
Формат фрейма (protocol.hpp:19-28):
┌──────────┬──────────────────────────┬─────────────────────────┐
│ 1 байт   │ 4 байта (big-endian)     │ N байт                  │
│ msg_type │ payload_length           │ payload                 │
└──────────┴──────────────────────────┴─────────────────────────┘
HEADER_SIZE = 5
MAX_PAYLOAD = 65535

MsgType:
  0x01 AuthRequest   — клиент → сервер: JSON {"user":..., "token":...}
  0x02 AuthResponse  — сервер → клиент: JSON {"ok":true, "tun_ip":..., ...}
  0x03 IpPacket      — двунаправленный: raw IPv4/IPv6
  0x04 Keepalive     — пустой payload, echo обратно
  0x05 TunConfig     — сервер → клиент (определён, но не используется в коде)
  0x06 Disconnect    — graceful close
  0x07 Ping          — клиент → сервер: timestamp
  0x08 Pong          — сервер → клиент: echo timestamp
```

---

## NAT / iptables

Сервер **сам iptables не настраивает**. Всё делает `install.sh`:

```bash
# install.sh:234-236 — включение IP forwarding
sysctl -w net.ipv4.ip_forward=1
echo "net.ipv4.ip_forward = 1" >> /etc/sysctl.d/99-kirdi.conf

# install.sh:245-247 — NAT правило
iptables -t nat -A POSTROUTING -s 10.8.0.0/24 -o "$iface" -j MASQUERADE
# $iface = первый default route интерфейс (ip route show default | awk '{print $5}')

# install.sh:253-257 — сохранение правил
netfilter-persistent save  # или iptables-save > /etc/iptables/rules.v4
```

**Важно:** правило добавляется только один раз при установке. При перезагрузке без `netfilter-persistent` правила теряются.

---

## authenticated_ — влияние на форвардинг

`authenticated_` — `bool` поле в `ClientSession` (session.hpp:46), по умолчанию `false`.

**Прямой путь (клиент → TUN):**
- `session.cpp:54-57`: если `!authenticated_` при получении `IpPacket` → DROP + LOG_WARN
- Пакет не доходит до TUN

**Обратный путь (TUN → клиент):**
- `server.cpp:255`: `sit->second->is_authenticated()` → если `false` → DROP **без лога**
- Это означает: если клиент прислал пакет до аутентификации (race condition невозможен т.к. auth синхронна), или если `authenticated_` сбросился — пакеты из интернета тихо дропаются

**Установка `authenticated_`:**
- `session.cpp:33`: `authenticated_ = true` — устанавливается **немедленно** при получении `AuthRequest`, **без валидации токена**
- Комментарий: `// TODO: validate HMAC token`
- `auth_secret` из конфига **нигде не используется** в текущем коде

---

## Зависимости

```
Server
  ├── TunDevice (LinuxTunDevice / MacOSTunDevice / WindowsTunDevice)
  ├── ClientSession
  │     └── IWsSession (WsServerSession | WsPlainServerSession)
  │           └── protocol:: (deserialize_header, build_packet, ip4_dst)
  └── protocol::ip4_dst (для routing в route_to_client)

ip_to_session_: map<uint32_t dst_ip (network order), uint32_t session_id>
sessions_:      map<uint32_t session_id, shared_ptr<ClientSession>>
```

---

## Проблемы и заметки

### 🔴 Критические

1. **Аутентификация не реализована** — `session.cpp:33`: `authenticated_ = true` без проверки токена. `auth_secret` из конфига нигде не используется. Любой клиент может подключиться и туннелировать трафик.

2. **TUN write при EAGAIN молча дропает пакет** — `tun_linux.cpp:97-99`: `EAGAIN → return 0`. Вызывающий код (`server.cpp:200`) не проверяет возвращаемое значение `write_packet()`. Пакеты теряются без лога при перегрузке TUN.

3. **Тихий дроп в route_to_client при !authenticated_** — `server.cpp:255-257`: нет лога когда пакет из интернета дропается из-за неаутентифицированной сессии. Сложно диагностировать.

4. **Сессии не удаляются из `sessions_` и `ip_to_session_`** — нет кода очистки при дисконнекте. `ClientSession::close()` закрывает WS, но не удаляет себя из `Server::sessions_`. Утечка памяти и IP-адресов при долгой работе.

### 🟡 Потенциальные проблемы

5. **IP allocation без освобождения** — `server.cpp:33`: `next_client_ip_offset_` только инкрементируется. При 254 клиентах (для /24) новые подключения получат IP за пределами подсети. Нет проверки переполнения.

6. **`tun_read_loop` — detached thread** — `server.cpp:69-70`: поток detach'ится. При `Server::stop()` → `tun_->close()` → поток завершится на следующей итерации, но нет синхронизации завершения. Возможен use-after-free если `Server` уничтожается быстро.

7. **`dispatch_packet` не обрабатывает частичные фреймы** — `ws_transport.cpp:13-22`: если WebSocket frame содержит неполный kirdi-фрейм (buf_size < HEADER_SIZE + hdr.length) — пакет молча дропается. Для WebSocket это маловероятно (фреймы атомарны), но edge case существует.

8. **`do_write` — data race на `writing_`** — `ws_transport.cpp:208-212` (WsServerSession): `writing_` — обычный `bool`, защищён `write_mutex_`, но `net::post` выполняется вне мьютекса. Если два потока вызовут `send()` одновременно — возможна двойная постановка `do_write` в очередь.

9. **`MsgType::TunConfig` (0x05) определён, но не обрабатывается** — `session.cpp:80-83`: попадает в `default` ветку с LOG_WARN.

10. **`allocate_client_ip` стартует с offset=2** — `server.cpp:50`: `next_client_ip_offset_{2}`. Первый клиент получит `tun_subnet + 2` (например `10.8.0.2`). `10.8.0.1` — сервер. Это корректно, но offset=1 пропускается.

11. **NAT правило привязано к hardcoded `10.8.0.0/24`** — `install.sh:245`: подсеть в iptables захардкожена, не берётся из конфига. Если в конфиге изменить `tun_subnet` — NAT не будет работать.

12. **`ip4_dst` возвращает network byte order** — `protocol.hpp:138-143`: `memcpy` из байт [16..19] без `ntohl`. В `ip_to_session_` ключ хранится в network byte order (`server.cpp:209`: `inet_pton` → `addr.s_addr` — тоже network order). Это **консистентно**, но неочевидно.

---

## Выводы

1. **Прямой путь** работает: WS frame → `dispatch_packet` → `handle_packet` → `on_ip_packet_` callback → `tun_->write_packet()`. Единственный guard — `authenticated_` флаг.

2. **Обратный путь** работает: `tun_read_loop` (detached thread) → `route_to_client` → lookup в `ip_to_session_` → `send_ip_packet` → WS write queue.

3. **Аутентификация — заглушка**: `authenticated_ = true` без проверки. Это самая критичная дыра.

4. **Сессии не очищаются**: утечка памяти и IP-пространства при долгой работе.

5. **NAT настраивается только install.sh**: сервер сам ничего не делает с iptables. Если правила не сохранены — после перезагрузки VPN не работает.

6. **TUN write при EAGAIN** — тихая потеря пакетов без счётчика/лога.
