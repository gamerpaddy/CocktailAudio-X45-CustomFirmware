#!/bin/bash
echo "=== DNS resolution:"
getent hosts buildroot.org 2>&1 | head -2 || echo "  DNS FAILED"
echo "=== default route:"
ip route 2>&1 | head -4
echo "=== raw TCP to 1.1.1.1:443 (8s):"
if timeout 8 bash -c 'exec 3<>/dev/tcp/1.1.1.1/443' 2>/dev/null; then
  echo "  OPEN"
else
  echo "  FAILED"
fi
echo "=== tools:"
for t in curl wget git make gcc g++ python3 bzip2 unzip rsync cpio bc; do
  p=$(command -v $t 2>/dev/null); printf '  %-8s %s\n' "$t" "${p:-MISSING}"
done
echo "=== HTTPS fetch (20s):"
if command -v curl >/dev/null; then
  timeout 20 curl -sS -o /dev/null -w '  curl: HTTP %{http_code} in %{time_total}s\n' https://buildroot.org/downloads/ 2>&1 | tail -2
elif command -v wget >/dev/null; then
  timeout 20 wget -q -O /dev/null https://buildroot.org/downloads/ && echo "  wget OK" || echo "  wget FAILED"
else
  echo "  no http client"
fi
echo "=== native gcc present? (needed for Buildroot host tools):"
gcc --version 2>&1 | head -1 || echo "  no native gcc"
echo "=== cpu/mem for a Buildroot run:"
nproc; free -h 2>/dev/null | head -2
df -h / 2>/dev/null | tail -1
