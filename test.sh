#!/bin/sh
set -e

IMG=nginx-stats-prom-test
CTR=nginx-stats-prom-run

cleanup() { docker rm -f "$CTR" >/dev/null 2>&1 || true; }
trap cleanup EXIT

docker build -t "$IMG" .
cleanup
docker run -d --name "$CTR" -p 8080:80 -p 8081:8081 -p 9100:9100 "$IMG"
sleep 1

echo "--- sending requests ---"
curl -s http://localhost:8080/
curl -s http://localhost:8080/t
curl -s http://localhost:8080/proxy
curl -s http://localhost:8080/t
curl -s http://localhost:8080/
curl -s http://localhost:8081/
curl -s http://localhost:8081/
curl -s --max-time 2 http://localhost:8081/slow || true

echo ""
echo "--- metrics ---"
METRICS=$(curl -s http://localhost:9100/metrics)
echo "$METRICS"
