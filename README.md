# Lumbre PHP Extension

Low-overhead PHP tracing extension. Hooks Zend Engine, captures I/O spans (HTTP, DB, Redis, file, socket), writes msgpack-encoded spans to a per-worker SPSC lock-free ring buffer in shared memory. Zero syscalls in hot path.

Part of the [Lumbre](https://github.com/ilbee/lumbre) tracing system.

## Requirements

| Dependency | Version |
|---|---|
| PHP (with dev headers) | 7.4+ |
| autoconf, make | - |
| libmsgpack-c-dev | - |
| CMocka (for unit tests) | - |

## Installation

From source:
```bash
phpize
./configure --enable-lumbre
make -j$(nproc)
sudo make install
```

Via Composer/PIE:
```bash
pie install ilbee/lumbre-ext
```

Then add to your `php.ini`:
```ini
extension=lumbre.so
```

## Configuration (php.ini)

| Directive | Default | Description |
|---|---|---|
| lumbre.enabled | 1 | Master switch |
| lumbre.mode | io | "io" (I/O only) or "full" (all function calls) |
| lumbre.shm_dir | /dev/shm | Shared memory directory |
| lumbre.buffer_size | 4194304 | Ring buffer size per worker (4MB) |
| lumbre.trigger_header | X-Trace-Debug | Header to activate full tracing per-request |
| lumbre.min_duration_ns | 0 | Minimum span duration to record |
| lumbre.max_query_len | 2048 | SQL query truncation limit |

## Testing

```bash
# Build with debug symbols
./configure --enable-lumbre-debug
make -j$(nproc)

# C unit tests (CMocka)
make test-unit

# PHP integration tests (.phpt)
make test TESTS=tests/
```

## How It Works

The extension hooks `zend_execute_internal` and `zend_execute_ex` to intercept PHP function calls. I/O operations are detected by matching against a whitelist of known functions (curl, PDO, Redis, file ops, sockets).

Each PHP-FPM worker gets its own ring buffer in `/dev/shm/lumbre_{pid}`. Spans are serialized as msgpack and written to the buffer using lock-free SPSC atomics. If the buffer is full, spans are silently dropped -- PHP never blocks.

The companion [lumbred](https://github.com/ilbee/lumbre-daemon) daemon reads these buffers and exports spans to ClickHouse or other backends.

## License

MIT -- see [LICENSE](LICENSE).
