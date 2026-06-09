# TSZY-ULT

A C++17 header-only networking framework for building game server clusters.

## Features

- **Transport layer** (`zbf/`) — TCP server/client with async I/O, epoll (Linux) / poll (Windows), connection pooling, heartbeat, send queues
- **Crypto & utilities** (`fszm/`) — OpenSSL wrapper (RSA, DH, AES), HTTP client (libcurl), binary serialization, random number generation
- **Server framework** (`lygc/`) — Request/response routing, sync/async handlers, gateway with DH+RSA handshake + AES encryption, service discovery

## Quick Start

### Prerequisites

- C++17 compiler (GCC 9+, MSVC 2019+, Clang 10+)
- CMake 3.15+
- OpenSSL, libcurl
- Linux: [vcpkg](https://vcpkg.io/) with cpp-httplib, nlohmann-json
- Windows: MSYS2/UCRT64 with vendor dependencies

### Build

```bash
# Configure
cmake --preset debug

# Build all
cmake --build --preset debug

# Build a single target
cmake --build --preset debug --target lygate
```

Other presets: `release`, `relwithdebinfo` (Linux); `debug-mingw`, `release-mingw`, `relwithdebinfo-mingw` (Windows/MSYS2).

### Run Tests

```bash
./build/debug/testsslv2
./build/debug/testhandshake
```

Generate crypto keys for tests:

```bash
cd test && ./createkey.sh rsa   # RSA 4096-bit key pair
./createkey.sh dh               # DH CA + server + client certs
./createkey.sh clean             # Remove generated keys
```

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
│  TCP · poll/epoll · timer · queue · log         │
└─────────────────────────────────────────────────┘
```

**Message flow:** Client → `GateServer` (RSA handshake → AES encryption) → `NetServer` (routes by `origin`) → `UserHandler` (sync/async response) → `NetClient` (inter-service async request with serial matching).

**Key server roles:**

| Server | Role |
|--------|------|
| `lycentral` | Service registry (HTTP-based) |
| `lygate` | Gateway with encrypted handshake |
| `lylogic` | Business logic |
| `lycommon` | Common/shared service |

## Dependencies

| Library | Purpose |
|---------|---------|
| OpenSSL | TLS, RSA, DH, AES crypto |
| libcurl | HTTP client |
| cpp-httplib | HTTP server (central registry) |
| nlohmann-json | JSON parsing |

## License

[MIT](LICENSE)
