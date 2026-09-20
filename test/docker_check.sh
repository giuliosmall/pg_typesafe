#!/bin/sh
# Build + full test (mock, pg_regress, TAP) inside the official postgres
# Docker image. Usage: sh test/docker_check.sh [16|17]
set -eu

PGVER="${1:-17}"
IMG="postgres:${PGVER}"
SRC="$(cd "$(dirname "$0")/.." && pwd)"

docker run --rm -v "$SRC":/src:ro -e PGVER="$PGVER" "$IMG" bash -ec '
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq
  apt-get install -y -qq --no-install-recommends \
    postgresql-server-dev-$PGVER libcurl4-openssl-dev libipc-run-perl \
    pkg-config gcc make >/dev/null

  cp -r /src /build && cd /build
  rm -f typesafe.o typesafe.so typesafe.dylib typesafe.bc
  make with_llvm=no PG_CONFIG=/usr/lib/postgresql/$PGVER/bin/pg_config 2>&1 \
    | tee /tmp/build.log
  ! grep -E "warning|error" /tmp/build.log
  make with_llvm=no PG_CONFIG=/usr/lib/postgresql/$PGVER/bin/pg_config install

  # cluster owned by postgres, trust auth on the socket
  su postgres -c "/usr/lib/postgresql/$PGVER/bin/initdb -D /tmp/data --locale=C" >/dev/null
  su postgres -c "/usr/lib/postgresql/$PGVER/bin/pg_ctl -D /tmp/data -l /tmp/pg.log -o \"-k /tmp -c listen_addresses=\" start"

  export PGHOST=/tmp PGUSER=postgres PGDATABASE=postgres
  su postgres -c "/usr/lib/postgresql/$PGVER/bin/createdb -h /tmp typesafe_ci" || true
  PGDATABASE=typesafe_ci sh test/ci.sh

  chown -R postgres /build
  su postgres -c "cd /build && PGHOST=/tmp make with_llvm=no PG_CONFIG=/usr/lib/postgresql/$PGVER/bin/pg_config installcheck" \
    || { cat /build/regression.diffs 2>/dev/null; find /build/tmp_check -name "*.log" -exec tail -50 {} + 2>/dev/null; exit 1; }

  echo "=== PG$PGVER: ALL CHECKS PASSED ==="
'
