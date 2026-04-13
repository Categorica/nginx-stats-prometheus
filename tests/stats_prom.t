#!/usr/bin/perl

use warnings;
use strict;

use Test::More;

use Test::Nginx;

select STDERR; $| = 1;
select STDOUT; $| = 1;

my $long_name = 'a' x 256;

my $t = Test::Nginx->new()->has(qw/http proxy/)->plan(10);

$t->write_file_expand('nginx.conf', <<"EOF");

%%TEST_GLOBALS%%

daemon off;

events {
}

http {
    %%TEST_GLOBALS_HTTP%%

    server_names_hash_bucket_size 512;

    upstream u {
        server 127.0.0.1:8081;
    }

    server {
        listen       127.0.0.1:8080;
        server_name  localhost;

        location / { return 200 "ok"; }
        location /missing { return 404; }
        location /proxy { proxy_pass http://u/; }
        location /metrics { prometheus_metrics; }
    }

    server {
        listen       127.0.0.1:8080;
        server_name  $long_name;

        location / { return 200 "ok"; }
    }

    server {
        listen       127.0.0.1:8081;
        server_name  backend;

        location / { return 200 "backend"; }
    }
}

EOF

$t->run();

# 5 requests in total:
# - 3 on localhost (proxy + 1 backend),
# + 1 on the server with an over-long name

http_get('/');
http_get('/missing');
http_get('/proxy');
http(<<EOF);
GET / HTTP/1.0
Host: $long_name

EOF

my $m = http_get('/metrics');

like($m, qr!^Content-Type: text/plain; version=0\.0\.4!mi, 'content type');
like($m, qr/^nginx_http_requests_total 5$/m, 'requests total');
like($m, qr/^nginx_http_responses_total\{status="2xx"\} 4$/m, 'responses 2xx');
like($m, qr/^nginx_http_responses_total\{status="4xx"\} 1$/m, 'responses 4xx');
like($m, qr/^nginx_http_request_duration_seconds_count 5$/m,
	'histogram count');

like($m, qr/^nginx_server_requests_total\{server="localhost"\} 3$/m,
	'per server');
like($m, qr/^nginx_upstream_requests_total\{upstream="u"\} 1$/m,
	'per upstream');

unlike($m, qr/server="a{256}"/, 'over-long server name not tracked');

my $r = http(<<EOF);
POST /metrics HTTP/1.0
Host: localhost
Content-Length: 0

EOF

like($r, qr!^HTTP/1\.1 405 !, 'post not allowed');
like($r, qr/^Allow: GET, HEAD\r?$/mi, 'post allow header');
