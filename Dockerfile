FROM nginx:stable AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    libpcre2-dev \
    libssl-dev \
    zlib1g-dev \
    wget \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /module
COPY config .
COPY Makefile .
COPY src/ src/

RUN make module && make install

FROM nginx:stable

COPY --from=builder /usr/lib/nginx/modules/ngx_http_stats_prom_module.so \
    /usr/lib/nginx/modules/
COPY nginx.conf.example /etc/nginx/nginx.conf

EXPOSE 80 8081 9100
