# TSZY-ULT

A C++17 header-only networking framework for building game server clusters. Cross-platform (Linux/Windows) with epoll-based async I/O, encrypted communication, and service discovery.

## Features

- **Transport layer** (`zbf/`) — TCP server/client with async I/O, epoll (Linux) / poll (Windows), connection pooling, heartbeat, send queues, thread-safe FIFO, timer wheel, per-request latency tracking, and memory leak detection. Alternative transport: standalone ASIO-based TCP.
- **Crypto & utilities** (`fszm/`) — OpenSSL wrapper (RSA, DH, ECDH, DSA, AES ECB/CBC/CFB/OFB/CTR, DES, Camellia, MD5, SHA, base64, hex), HTTP client (libcurl), binary serialization with DataTable support, and thread-local MT19937 random generation
- **Server framework** (`lygc/`) — Request/response routing by origin, sync/async handlers, gateway with DH+RSA handshake + AES-CFB encryption, HTTP-based service discovery, and JSON config management
- **Database proxy** (`wjp/` + `lygc/`) — MySQL, MongoDB, and Redis proxy servers with connection pooling, async request/response, and client SDKs for upstream servers

## Quick Start

### Prerequisites

- C++17 compiler (GCC 9+, MSVC 2019+, Clang 10+)
- CMake 3.15+
- OpenSSL, libcurl
- MySQL client library (`libmysqlclient-dev` or `libmariadb-dev`) for MySQL proxy
- Linux: [vcpkg](https://vcpkg.io/) with cpp-httplib, nlohmann-json, mongoc, hiredis
- Windows: MSYS2/UCRT64 with vendor dependencies

Install vcpkg dependencies (Linux):
```bash
vcpkg install cpp-httplib nlohmann-json openssl curl mongoc hiredis asio
```

### Build

```bash
# Configure (generates build/debug/)
cmake --preset debug

# Build all targets
cmake --build --preset debug

# Build a single target
cmake --build --preset debug --target lygate
```

Post-build: `lycentral` and `testsslv2` auto-copy their required files (`examples/lycentral-conf.json` and `test/key/`) to the build directory. Other targets (e.g., `lygate`, `testhandshake`) require manual key/config placement.

Available build targets: `tcpserver`, `tcpcltpool`, `lycentral`, `lylogic`, `lycommon`, `lycomasynclt`, `lygate`, `lygateclt`, `lydbproxy-mysql`, `lydbproxy-mongo`, `lydbproxy-redis`, `lydbagent`, `lydbagentclt`, `testsslv2`, `testhandshake`, `asio_tcpserver`, `asio_tcpclient`.

Presets are defined in `CMakePresets.json` (inherits a vcpkg toolchain from the hidden `base` preset):

| Preset | Build Type | Platform |
|--------|-----------|----------|
| `debug` | Debug (symbols, no opt, assertions) | Linux (vcpkg) |
| `release` | Release (optimized, NDEBUG) | Linux (vcpkg) |
| `relwithdebinfo` | Optimized + debug symbols | Linux (vcpkg) |
| `debug-mingw` | Debug | Windows/MSYS2 UCRT64 |
| `release-mingw` | Release | Windows/MSYS2 UCRT64 |
| `relwithdebinfo-mingw` | Optimized + debug symbols | Windows/MSYS2 UCRT64 |

Build artifacts go to `build/<preset-name>/`.

### Run Tests

```bash
./build/debug/testsslv2      # OpenSSL wrappers: RSA, DH, ECDH, DSA, AES, hash
./build/debug/testhandshake  # DH+RSA handshake protocol
```

Generate crypto keys for tests (required before first run):

```bash
cd test && ./createkey.sh rsa   # RSA 4096-bit key pair
./createkey.sh dh               # DH CA + server + client certs (default if no args)
./createkey.sh ecdh             # ECDH certs (secp384r1 curve)
./createkey.sh dsa              # DSA 2048-bit key pair
./createkey.sh clean            # Remove generated keys
```

Keys are output to `test/key/`. These keys are also needed by `lygate` and `lygateclt` for the gateway handshake (see Cluster Startup). Only `testsslv2` has a CMake post-build step to copy keys — if building only `testhandshake`, copy manually:
```bash
cp -r test/key build/debug/
```
`testhandshake` tests both DH+RSA and ECDH+RSA handshake variants.

## Architecture

```
┌─────────────────────────────────────────────────┐
│  lygc/  — Game server framework                 │
│  GateServer · NetServer · NetClient · Central   │
│  lymsg_protocol · handshake · config            │
├─────────────────────────────────────────────────┤
│  fszm/  — Crypto & utilities                    │
│  OpenSSL · curl · serialize · random            │
├─────────────────────────────────────────────────┤
│  zbf/   — Transport primitives                  │
│  TCP · ASIO · poll/epoll · timer · queue  │
├─────────────────────────────────────────────────┤
│  wjp/   — Database client drivers               │
│  MySQL · MongoDB · Redis                         │
└─────────────────────────────────────────────────┘
```

Two transport stacks are available:

| Transport | Header | Protocol | Key Classes |
|-----------|--------|----------|-------------|
| TCP (epoll/poll) | `socket_tcp_v5.hpp` | TCP on epoll(Linux)/poll(Windows) | `tcpsock_server`, `tcpsock_user`, `tcpsock_ha_asynclt` |
| ASIO TCP | `asio_socket_tcp.hpp` | Standalone ASIO TCP | `tcpsock_listener`, `tcpsock_user`, `tcpsock_client` (different interface) |

### Message Flow

Client connects → `tcpsock_user::onRecvMsg` → `NetUser::onRecvMsg` unpacks via `lymsg_protocol` → `NetUser::onRequest` routes to `UserHandler` by `origin` → handler returns one of:

| Return Value | Behavior |
|-------------|----------|
| `0` (`SYNC_RESPONSE`) | Response sent immediately to the caller |
| `1` (`NO_RESPONSE`) | An empty ACK is sent |
| Other (`>=2`, including `ASYNC_RESPONSE = 101`) | Request context saved; response delivered later via `NetServer::response(serial, data)` |

For inter-service communication, `NetClient::request` dispatches a message and registers a callback by serial number. When the response arrives, `NetClient::onResponse` matches the serial and invokes the handler.

### Gateway Encryption

`GateServer` enforces an encrypted channel between external clients and the server cluster:

1. **Handshake request:** Client generates DH/ECDH parameters + a temporary AES key, encrypts them with the server's RSA public key, and sends to the gateway.
2. **Handshake response:** Server decrypts with its RSA private key, generates a DH/ECDH keypair from the client's parameters, and responds with its public key encrypted by the temporary AES key.
3. **Session established:** Both sides compute a shared key from the DH/ECDH exchange (first 16 bytes = AES session key, next 16 bytes = IV). All subsequent messages carry the `LYMSG_TYPE_ENC` flag and are encrypted/decrypted with AES-CFB using this session key.

`GateUser` handles per-connection handshake state, automatically decrypting inbound encrypted messages and encrypting outbound responses.

### Service Discovery

The central registry enables dynamic service lookup:

- **`CentralServer`** (cpp-httplib HTTP server) — Maintains a key-value store of server configurations. In `lycentral`, the config JSON is loaded from file and stored via `set("config", data)`. Other servers then fetch it at startup.
- **`CentralServClt`** (aliased as `lygc::Central`) — Static libcurl-based client. Default endpoint is `http://localhost:8081`. Servers call `Central::get("config")` at startup to retrieve the cluster configuration.

### DB Proxy Architecture

Three-tier database access with proxy servers that handle connection pooling and async query routing:

```
[App Server]              [DB Proxy Server]         [Actual Database]
lylogic/lycommon           lydbproxy-mysql           MySQL
  uses DBClientMySQL  ->   DBProxyMySQL               (libmysqlclient)
                            wjp::mysql_client_pool

lylogic/lycommon           lydbproxy-mongo           MongoDB
  uses DBClientMongo  ->   DBProxyMongo               (libmongoc)
                            wjp::mongo_client_pool

lylogic/lycommon           lydbproxy-redis           Redis
  uses DBClientRedis  ->   DBProxyRedis               (hiredis)
                            wjp::redis_client_pool
```

Key features:
- Up to 64 independent database instances per proxy (`MAX_DB_SLOTS`)
- Connection pooling with configurable size per instance
- Async request/response with serial-number matching
- Exponential backoff retry (3 retries max)
- Automatic cleanup of pending requests on client disconnect

An aggregation pattern is demonstrated by `lydbagent`: a `NetServer` that uses `DBClientMySQL` to proxy client requests to `lydbproxy-mysql`, showing how app servers can rely exclusively on DB proxy servers for database access.

### Server Roles

| Server | Binary | Role |
|--------|--------|------|
| **Central** | `lycentral` | Service registry — loads `examples/lycentral-conf.json`, serves config via HTTP |
| **Gateway** | `lygate` | Entry point — RSA handshake + AES encryption, forwards to logic via `NetClient` |
| **Logic** | `lylogic` | Business logic — `NetServer` with handlers registered per origin |
| **Common** | `lycommon` | Shared service — `NetServer`, proxies to logic via async `NetClient::request` |
| **Gate Client** | `lygateclt` | Test client — performs handshake then sends encrypted requests |
| **Common Client** | `lycomasynclt` | Test client — async request/response test against common service |
| **MySQL Proxy** | `lydbproxy-mysql` | MySQL proxy — connection pool, async query routing |
| **MongoDB Proxy** | `lydbproxy-mongo` | MongoDB proxy — connection pool via mongoc |
| **Redis Proxy** | `lydbproxy-redis` | Redis proxy — connection pool via hiredis |
| **DB Agent** | `lydbagent` | DB agent — app server that proxies to MySQL proxy via `DBClientMySQL` |
| **DB Agent Client** | `lydbagentclt` | Test client — sends test messages to dbagent |
| **ASIO Server** | `asio_tcpserver` | Demo — ASIO-based TCP echo (argv: host port) |
| **ASIO Client** | `asio_tcpclient` | Demo — ASIO-based TCP client with sync & async modes |

Demo servers (`tcpserver`, `tcpcltpool`) demonstrate raw `tcpsock_server` and `tcpsock_cltpool` usage without the game server framework.

### Server Lifecycle

All servers (except `lycentral`) follow the same lifecycle pattern, including `std::atexit(onExit)` for memory tracking dump on exit:

```cpp
Server server(config);
server.open();          // Bind and listen
server.start(workers);  // Launch I/O worker threads + timer
server.serveUtilStop(); // Block until signal (SIGINT/SIGTERM)
server.close();         // Graceful shutdown
```

`start()` launches a configurable number of worker threads for I/O processing. `NetServer::start(serverWorkers, cltMgrWorkers)` also starts an optional async client manager (for outbound HA connections) when `cltMgrWorkers > 0`, and a 10-second timer wheel for periodic status reporting. Signal handlers call `stop()` which unblocks `serveUtilStop()`. In Debug builds, ASIO timeouts are shortened (UserTimeout=9s, HeartbeatTime=3s) for faster iteration; Release uses 90s/30s.

### Memory Tracking

Controlled by the `ZBF_TRACE_MEMORY` define (enabled by default). All allocations use `ZBF_MALLOC`/`ZBF_FREE` macros for per-file accounting. Key classes inherit from `object_tracker<T>` (CRTP) to count live instances. Call `logMemTrackStat()` to dump current allocation statistics — this is automatically called via `std::atexit` in most examples.

## Cluster Startup

To run the full server cluster (in order):

```bash
# 1. Start the service registry (HTTP on port 8081)
./lycentral
# 2. Start the logic server (listens on port 35101)
./lylogic
# 3. Start the common proxy (port 35102, proxies to logic)
./lycommon
# 4. Start the DB proxy servers (ports 35103-35105)
./lydbproxy-mysql root password
./lydbproxy-mongo root password
./lydbproxy-redis password
# 5. Start the DB agent (port 35106)
./lydbagent
# 6. Start the gateway (port 35107, needs key/rsa_private_key.pem)
./lygate
# 7. Test: encrypted client, async client, DB client
./lygateclt
./lycomasynclt
./lydbagentclt
```

Steps 6-7 require RSA keys from `test/key/` for the gateway handshake. Copy them to the build directory first:
```bash
cp -r test/key build/debug/
```

## Dependencies

| Library | Purpose |
|---------|---------|
| OpenSSL | RSA, DH, ECDH, DSA, AES, DES, Camellia, MD5, SHA, base64, hex |
| libcurl | HTTP client for service discovery |
| cpp-httplib | HTTP server for central registry |
| nlohmann-json | JSON config parsing |
| asio | Standalone ASIO TCP transport |
| libmysqlclient / libmariadb | MySQL C client |
| libmongoc | MongoDB C driver |
| hiredis | Redis C client |
| pthreads | Threading |

## License

[MIT](LICENSE)
