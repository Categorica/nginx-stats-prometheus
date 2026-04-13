NGINX_VERSION := $(shell nginx -v 2>&1 | sed -nE 's|.*/([0-9]+\.[0-9]+\.[0-9]+).*|\1|p')
NGINX_PREFIX := $(shell nginx -V 2>&1 | sed -nE 's|.*--prefix=([^ ]+).*|\1|p')
NGINX_CONF_MODULES_PATH := $(shell nginx -V 2>&1 | sed -nE 's|.*--modules-path=([^ ]+).*|\1|p')

# --modules-path if nginx was built with one, else <prefix>/modules, else nginx's default prefix
NGINX_MODULES_PATH ?= $(or $(NGINX_CONF_MODULES_PATH),$(if $(NGINX_PREFIX),$(NGINX_PREFIX)/modules),/usr/local/nginx/modules)

NGINX_SRC ?= /usr/local/src/nginx-$(NGINX_VERSION)
CC ?= cc
CFLAGS ?= -O2 -g

MODULE_NAME = ngx_http_stats_prom_module.so

NGINX_TESTS ?= ./build/nginx-tests
TEST_NGINX_BINARY ?= $(shell command -v nginx)

ifeq ($(wildcard $(NGINX_SRC)),)
    NGINX_SRC := ./build/nginx-$(NGINX_VERSION)
    FETCH_NGINX := true
endif

.PHONY: module install test clean

$(NGINX_SRC):
ifdef FETCH_NGINX
	@test -n "$(NGINX_VERSION)" || { echo "error: could not detect nginx version (is nginx in PATH?); set NGINX_SRC" >&2; exit 1; }
	@echo "NGINX_SRC not found, fetching NGINX version $(NGINX_VERSION) to $(NGINX_SRC)"
	mkdir -p ./build
	wget -qO- http://nginx.org/download/nginx-$(NGINX_VERSION).tar.gz | tar xz -C ./build
endif

module: $(NGINX_SRC)
	cd $(NGINX_SRC) && ./configure \
		--with-cc="$(CC)" \
		--with-cc-opt="$(CFLAGS)" \
		--with-compat \
		--with-http_stub_status_module \
		--add-dynamic-module=$(CURDIR)
	$(MAKE) -C $(NGINX_SRC) modules

install: module
	install -d $(NGINX_MODULES_PATH)
	install -m 0644 $(NGINX_SRC)/objs/$(MODULE_NAME) $(NGINX_MODULES_PATH)/

$(NGINX_TESTS):
	git clone --depth 1 https://github.com/nginx/nginx-tests.git $(NGINX_TESTS)

test: module $(NGINX_TESTS)
	TEST_NGINX_BINARY=$(TEST_NGINX_BINARY) \
	TEST_NGINX_GLOBALS="load_module $(abspath $(NGINX_SRC)/objs/$(MODULE_NAME));" \
	prove -I $(NGINX_TESTS)/lib tests/

clean:
	if [ -f $(NGINX_SRC)/Makefile ]; then $(MAKE) -C $(NGINX_SRC) clean; fi
	rm -rf ./build
