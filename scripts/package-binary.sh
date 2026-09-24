#!/bin/sh
# Build a prebuilt binary tarball for install.sh.
#
# Usage: sh scripts/package-binary.sh /path/to/pg_config OUTDIR
#
# Produces OUTDIR/pg_typesafe-pg<major>-linux-<arch>.tar.gz containing
#   lib/typesafe.so
#   extension/typesafe.control, extension/typesafe--*.sql
set -eu

PGC=${1:?pg_config path}
OUT=${2:?output directory}
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)	# we cd into a staging tree below
SRC="$(cd "$(dirname "$0")/.." && pwd)"

major=$("$PGC" --version | sed -n 's/^PostgreSQL \([0-9][0-9]*\).*/\1/p')
case "$(uname -m)" in
	x86_64|amd64) arch=amd64 ;;
	aarch64|arm64) arch=arm64 ;;
	*) arch=$(uname -m) ;;
esac

stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
cp -R "$SRC"/. "$stage/src"
cd "$stage/src"
make -s clean >/dev/null 2>&1 || true
make -s COPT="${COPT:-}" with_llvm=no PG_CONFIG="$PGC"
make -s with_llvm=no PG_CONFIG="$PGC" DESTDIR="$stage/root" install

mkdir -p "$stage/pkg/lib" "$stage/pkg/extension"
cp "$stage/root$("$PGC" --pkglibdir)/typesafe.so" "$stage/pkg/lib/"
cp "$stage/root$("$PGC" --sharedir)"/extension/typesafe* "$stage/pkg/extension/"
strip --strip-unneeded "$stage/pkg/lib/typesafe.so" 2>/dev/null || true

mkdir -p "$OUT"
name="pg_typesafe-pg$major-linux-$arch.tar.gz"
tar -czf "$OUT/$name" -C "$stage/pkg" lib extension
echo "$OUT/$name"
