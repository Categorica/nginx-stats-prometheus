# nginx-stats-prometheus

An NGINX module that exposes server metrics in Prometheus format.

## Metrics

Exposed at a configurable location via the `prometheus_metrics` directive:

| Metric | Type | Labels |
| --- | --- | --- |
| `nginx_up` | gauge | |
| `nginx_process_pid` | gauge | |
| `nginx_http_requests_total` | counter | |
| `nginx_http_responses_total` | counter | `status` (`1xx`-`5xx`) |
| `nginx_http_requests_by_method_total` | counter | `method` (`GET`, `HEAD`, `POST`, `PUT`, `DELETE`, `OPTIONS`, `PATCH`, `other`) |
| `nginx_http_request_bytes_total` | counter | |
| `nginx_http_response_bytes_total` | counter | |
| `nginx_http_version_total` | counter | `version` (`1.0`, `1.1`, `2.0`, `3.0`) |
| `nginx_http_request_duration_seconds` | histogram | `le` (0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1, 2.5, 5, 10) |
| `nginx_connections_active` | gauge | |
| `nginx_connections_reading` | gauge | |
| `nginx_connections_writing` | gauge | |
| `nginx_connections_waiting` | gauge | |
| `nginx_connections_accepted_total` | counter | |
| `nginx_connections_handled_total` | counter | |

Per server/per upstream, where `<prefix>` is `nginx_server` or `nginx_upstream`:

| Metric | Type | Labels |
| --- | --- | --- |
| `<prefix>_requests_total` | counter | `server` / `upstream` |
| `<prefix>_response_total` | counter | `server` / `upstream`, `status` (`1xx`-`5xx`) |
| `<prefix>_request_bytes_total` | counter | `server` / `upstream` |
| `<prefix>_response_bytes_total` | counter | `server` / `upstream` |

## Requirements

- NGINX 1.25.0 or newer, built with `--with-compat` and `--with-http_stub_status_module`
- A C compiler, PCRE, OpenSSL, and zlib development headers

## Building

```sh
make module
make install
```

The Makefile auto-detects your installed NGINX version and modules path using the `nginx` binary.

- **`NGINX_SRC`** -- Path to NGINX source tree. Set this to skip the automatic download. Default: auto-fetched source matching `nginx -v`.
- **`NGINX_MODULES_PATH`** -- Installation directory for the compiled `.so`. Default: `--modules-path` from `nginx -V`, else `<prefix>/modules` or `/usr/local/nginx/modules`.
- **`CC`** -- C compiler, passed to `./configure --with-cc`. Default: `cc`.
- **`CFLAGS`** -- Extra compiler flags, passed to `./configure --with-cc-opt`. Default: `-O2 -g`.

`make clean` removes build output, including the downloaded NGINX source and nginx-tests checkout.

Examples:

```sh
# Use a local source tree and custom module path
make module NGINX_SRC=/path/to/nginx-1.28.0
make install NGINX_MODULES_PATH=/opt/nginx/modules

# Default: detects version from nginx -v, fetches source, installs to detected modules path
make install
```

## Configuration

```nginx
load_module modules/ngx_http_stats_prom_module.so;

http {
    # Optional: set shared memory size (default 1m)
    # prometheus_metrics_shm_size 2m;

    server {
        listen 9100;

        # limit access here via allow 127.0.0.1, deny all etc.

        location /metrics {
            prometheus_metrics;
        }
    }
}
```

## Docker (for testing on systems without nginx)

NB: this consumes the nginx.conf.example

```sh
docker build -t nginx-stats-prom .
docker run -p 80:80 -p 8081:8081 -p 9100:9100 nginx-stats-prom
curl http://localhost:9100/metrics
```

## Testing

```sh
make test
```

Builds the module and runs the tests in `tests/` with `prove`, using [nginx-tests](https://github.com/nginx/nginx-tests). Requires Perl and an `nginx`.

- **`TEST_NGINX_BINARY`** -- NGINX binary to test against. Default: `nginx` from `PATH`.
- **`NGINX_TESTS`** -- Path to an nginx-tests checkout. Default: `./build/nginx-tests`.

For a quick somewhat self-contained check, `./test.sh` builds the Docker image, sends sample requests, and prints the metrics output.

## Limitations

- At most 512 servers/512 upstreams are tracked; names longer than 255 bytes, are silently skipped. They are also skipped if the shared memory zone fills up (raise `prometheus_metrics_shm_size`)
