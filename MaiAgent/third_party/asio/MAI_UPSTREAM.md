# Standalone Asio source

- Upstream: https://github.com/chriskohlhoff/asio
- Release: `asio-1-30-2`
- Source archive: `https://github.com/chriskohlhoff/asio/archive/refs/tags/asio-1-30-2.tar.gz`
- Archive SHA-256: `755bd7f85a4b269c67ae0ea254907c078d408cce8e1a352ad2ed664d233780e8`
- License: Boost Software License 1.0; see `asio/LICENSE_1_0.txt`.

Only the upstream header-only `asio/include` tree and license files are vendored.
WebSocket++ 0.8.2 compiles with this version in C++17 mode; Asio 1.38.2 removed
the `io_service` APIs it uses.
