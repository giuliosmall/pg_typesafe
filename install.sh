#!/bin/sh
# pg_typesafe installer
#
#   curl -fsSL https://raw.githubusercontent.com/giuliosmall/pg_typesafe/main/install.sh | sh
#
# Finds every PostgreSQL 15+ server on this machine, installs a prebuilt
# binary for it (Linux glibc, x86_64/aarch64) or builds from source
# (everything else, installing build dependencies with the system package
# manager), and optionally enables the extension in a database.
#
# Options (pass after `sh -s --` when piping):
#   --db NAME           CREATE EXTENSION typesafe in NAME; if TYPESAFE_API_KEY
#                       is set, also store it in the data directory and point
#                       typesafe.api_key_file at it (no server restart)
#   --pg-config PATH    install only for this pg_config
#   --version TAG       release tag to install (default: latest release)
#   --source            skip prebuilt binaries, always build from source
#   --no-deps           do not install build dependencies
#   -h, --help
#
# Environment: TYPESAFE_API_KEY, TYPESAFE_DB, TYPESAFE_VERSION, PG_CONFIG,
# TYPESAFE_RELEASE_URL (mirror of the release assets), and the usual libpq
# variables (PGHOST, PGPORT, PGUSER) for --db.
set -eu

REPO="giuliosmall/pg_typesafe"
MIN_MAJOR=15

DB="${TYPESAFE_DB:-}"
VERSION="${TYPESAFE_VERSION:-}"
ONLY_PG_CONFIG="${PG_CONFIG:-}"
FROM_SOURCE=0
INSTALL_DEPS=1

say() { printf '\033[1m==>\033[0m %s\n' "$*" >&2; }
warn() { printf '\033[33mwarning:\033[0m %s\n' "$*" >&2; }
die() { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }

usage() {
	if [ -f "$0" ] && grep -q pg_typesafe "$0" 2>/dev/null; then
		sed -n '2,/^set -eu/p' "$0" | sed '$d; s/^# \{0,1\}//'
	else
		echo "see https://github.com/$REPO/blob/main/install.sh"
	fi
}

while [ $# -gt 0 ]; do
	case "$1" in
		--db) DB="${2:?--db needs a value}"; shift 2 ;;
		--db=*) DB="${1#*=}"; shift ;;
		--pg-config) ONLY_PG_CONFIG="${2:?--pg-config needs a value}"; shift 2 ;;
		--pg-config=*) ONLY_PG_CONFIG="${1#*=}"; shift ;;
		--version) VERSION="${2:?--version needs a value}"; shift 2 ;;
		--version=*) VERSION="${1#*=}"; shift ;;
		--source) FROM_SOURCE=1; shift ;;
		--no-deps) INSTALL_DEPS=0; shift ;;
		-h|--help) usage; exit 0 ;;
		*) die "unknown option: $1 (see --help)" ;;
	esac
done

TMP=$(mktemp -d 2>/dev/null || mktemp -d -t pg_typesafe)
trap 'rm -rf "$TMP"' EXIT INT TERM

# --- privileges -----------------------------------------------------------

SUDO=""
if [ "$(id -u)" != 0 ]; then
	if command -v sudo >/dev/null 2>&1; then
		SUDO="sudo"
	elif command -v doas >/dev/null 2>&1; then
		SUDO="doas"
	fi
fi

# run as root only when the target is not writable by us (Homebrew,
# Postgres.app and self-built trees usually are)
as_owner() {
	if [ -w "$1" ]; then
		shift
		"$@"
	elif [ -n "$SUDO" ]; then
		shift
		$SUDO "$@"
	else
		die "$1 is not writable and neither sudo nor doas is available; re-run as root"
	fi
}

as_root() {
	if [ "$(id -u)" = 0 ]; then
		"$@"
	elif [ -n "$SUDO" ]; then
		$SUDO "$@"
	else
		die "need root to run: $*"
	fi
}

# --- downloads ------------------------------------------------------------

fetch() {
	if command -v curl >/dev/null 2>&1; then
		curl -fsSL --retry 3 -o "$2" "$1"
	elif command -v wget >/dev/null 2>&1; then
		wget -q -O "$2" "$1"
	else
		return 1
	fi
}

sha256() {
	if command -v sha256sum >/dev/null 2>&1; then
		sha256sum "$1" | cut -d' ' -f1
	else
		shasum -a 256 "$1" | cut -d' ' -f1
	fi
}

release_url() {
	if [ -n "${TYPESAFE_RELEASE_URL:-}" ]; then
		echo "$TYPESAFE_RELEASE_URL/$1"
	elif [ -n "$VERSION" ]; then
		echo "https://github.com/$REPO/releases/download/$VERSION/$1"
	else
		echo "https://github.com/$REPO/releases/latest/download/$1"
	fi
}

# --- platform -------------------------------------------------------------

OS=$(uname -s)
case "$(uname -m)" in
	x86_64|amd64) ARCH=amd64 ;;
	aarch64|arm64) ARCH=arm64 ;;
	*) ARCH=$(uname -m) ;;
esac

LIBC=gnu
if [ "$OS" = Linux ] && { ldd --version 2>&1 | grep -qi musl; }; then
	LIBC=musl
fi

PKG=""
for pm in apt-get dnf yum zypper apk pacman brew; do
	if command -v "$pm" >/dev/null 2>&1; then
		PKG=$pm
		break
	fi
done

# --- discover PostgreSQL installations ------------------------------------

pg_major() {
	"$1" --version | sed -n 's/^PostgreSQL \([0-9][0-9]*\).*/\1/p'
}

candidates() {
	if [ -n "$ONLY_PG_CONFIG" ]; then
		echo "$ONLY_PG_CONFIG"
		return
	fi
	command -v pg_config 2>/dev/null || true
	for p in \
		/usr/lib/postgresql/*/bin/pg_config \
		/usr/pgsql-*/bin/pg_config \
		/usr/local/pgsql/bin/pg_config \
		/opt/homebrew/opt/postgresql@*/bin/pg_config \
		/opt/homebrew/opt/postgresql/bin/pg_config \
		/usr/local/opt/postgresql@*/bin/pg_config \
		/usr/local/opt/postgresql/bin/pg_config \
		/Applications/Postgres.app/Contents/Versions/*/bin/pg_config; do
		[ -x "$p" ] && echo "$p"
	done
	return 0
}

TARGETS=""
SEEN=""
for pgc in $(candidates); do
	[ -x "$pgc" ] || { [ -n "$ONLY_PG_CONFIG" ] && die "$pgc is not executable"; continue; }
	libdir=$("$pgc" --pkglibdir 2>/dev/null) || continue
	case " $SEEN " in *" $libdir "*) continue ;; esac
	SEEN="$SEEN $libdir"
	major=$(pg_major "$pgc")
	[ -n "$major" ] || continue
	if [ "$major" -lt "$MIN_MAJOR" ]; then
		warn "skipping PostgreSQL $major at $pgc (pg_typesafe needs $MIN_MAJOR+)"
		continue
	fi
	# a pg_config without a server next to it is a client-only install
	if [ -z "$ONLY_PG_CONFIG" ] && [ ! -x "$("$pgc" --bindir)/postgres" ]; then
		continue
	fi
	TARGETS="$TARGETS $pgc"
done

if [ -z "$TARGETS" ]; then
	die "no PostgreSQL $MIN_MAJOR+ server found.
  Install PostgreSQL first, pass --pg-config /path/to/pg_config, or skip the
  install entirely with Docker:
    docker run -e POSTGRES_PASSWORD=pw -e TYPESAFE_API_KEY=tsk_... ghcr.io/$REPO:17"
fi

# --- prebuilt binaries ----------------------------------------------------

install_prebuilt() {
	pgc=$1 major=$2
	[ "$OS" = Linux ] && [ "$LIBC" = gnu ] || return 1
	case "$ARCH" in amd64|arm64) ;; *) return 1 ;; esac

	name="pg_typesafe-pg$major-linux-$ARCH.tar.gz"
	dir="$TMP/bin-$major"
	mkdir -p "$dir"
	fetch "$(release_url "$name")" "$dir/$name" 2>/dev/null || return 1
	fetch "$(release_url SHA256SUMS)" "$dir/SHA256SUMS" 2>/dev/null || return 1
	want=$(grep " $name\$" "$dir/SHA256SUMS" | cut -d' ' -f1)
	got=$(sha256 "$dir/$name")
	if [ -z "$want" ] || [ "$want" != "$got" ]; then
		warn "checksum mismatch for $name; falling back to a source build"
		return 1
	fi
	# called in an `if`, so set -e is off here: check every step
	tar -xzf "$dir/$name" -C "$dir" || return 1
	[ -f "$dir/lib/typesafe.so" ] || return 1

	# the binary links libcurl.so.4 and glibc; if the loader can't satisfy
	# it here, build from source instead of installing something unloadable
	if ldd "$dir/lib/typesafe.so" 2>&1 | grep -q "not found"; then
		warn "prebuilt binary is not loadable on this system (missing libcurl or old glibc); building from source"
		return 1
	fi

	libdir=$("$pgc" --pkglibdir)
	extdir="$("$pgc" --sharedir)/extension"
	as_owner "$libdir" install -m 755 "$dir/lib/typesafe.so" "$libdir/" \
		|| die "could not install into $libdir"
	as_owner "$extdir" install -m 644 "$dir"/extension/typesafe* "$extdir/" \
		|| die "could not install into $extdir"
	return 0
}

# --- source build ---------------------------------------------------------

SRC=""
get_source() {
	[ -n "$SRC" ] && return 0
	here=$(cd "$(dirname "$0")" 2>/dev/null && pwd) || here=""
	if [ -n "$here" ] && [ -f "$here/typesafe.control" ] && [ -f "$here/typesafe.c" ]; then
		SRC="$TMP/src"
		mkdir -p "$SRC"
		cp -R "$here"/. "$SRC"/
		return 0
	fi
	if [ -n "$VERSION" ]; then
		url="https://github.com/$REPO/archive/refs/tags/$VERSION.tar.gz"
	else
		# newest release if there is one, else main
		url=""
		if fetch "$(release_url VERSION)" "$TMP/tag" 2>/dev/null; then
			tag=$(head -n 1 "$TMP/tag" | tr -cd 'A-Za-z0-9._-')
			[ -n "$tag" ] && url="https://github.com/$REPO/archive/refs/tags/$tag.tar.gz"
		fi
		[ -n "$url" ] || url="https://github.com/$REPO/archive/refs/heads/main.tar.gz"
	fi
	say "downloading source: $url"
	fetch "$url" "$TMP/src.tar.gz" || die "could not download $url"
	mkdir -p "$TMP/src"
	tar -xzf "$TMP/src.tar.gz" -C "$TMP/src" --strip-components=1
	SRC="$TMP/src"
}

install_deps() {
	major=$1
	[ "$INSTALL_DEPS" = 1 ] || return 0
	say "installing build dependencies for PostgreSQL $major ($PKG)"
	case "$PKG" in
		apt-get)
			as_root env DEBIAN_FRONTEND=noninteractive apt-get install -y -q --no-install-recommends \
				"postgresql-server-dev-$major" libcurl4-openssl-dev pkg-config gcc make \
				|| { as_root apt-get update -q && as_root env DEBIAN_FRONTEND=noninteractive \
					apt-get install -y -q --no-install-recommends \
					"postgresql-server-dev-$major" libcurl4-openssl-dev pkg-config gcc make; } ;;
		dnf|yum)
			as_root "$PKG" install -y "postgresql$major-devel" libcurl-devel gcc make \
				|| as_root "$PKG" install -y postgresql-server-devel libcurl-devel gcc make ;;
		zypper)
			as_root zypper --non-interactive install "postgresql$major-server-devel" libcurl-devel gcc make ;;
		apk)
			as_root apk add --no-cache "postgresql$major-dev" curl-dev pkgconf build-base ;;
		pacman)
			as_root pacman -S --needed --noconfirm postgresql curl base-devel ;;
		brew)
			xcode-select -p >/dev/null 2>&1 \
				|| die "Xcode Command Line Tools are required: run 'xcode-select --install' and retry" ;;
		*)
			warn "unknown package manager; assuming a C compiler, make, libcurl headers and PostgreSQL server headers are installed" ;;
	esac
}

install_source() {
	pgc=$1 major=$2
	curl_cflags=$(pkg-config --cflags libcurl 2>/dev/null || true)
	# shellcheck disable=SC2086  # a list of flags
	if ! [ -f "$("$pgc" --pgxs 2>/dev/null)" ] || ! command -v make >/dev/null 2>&1 \
		|| ! printf '#include <curl/curl.h>\n' | ${CC:-cc} $curl_cflags -x c -E - >/dev/null 2>&1; then
		install_deps "$major"
	fi
	[ -f "$("$pgc" --pgxs 2>/dev/null)" ] \
		|| die "PostgreSQL $major server headers not found (package postgresql-server-dev-$major / postgresql$major-devel)"

	get_source
	build="$TMP/build-$major"
	rm -rf "$build"
	cp -R "$SRC" "$build"
	say "building for PostgreSQL $major"
	(cd "$build" && { make -s clean >/dev/null 2>&1; true; })
	(cd "$build" && make -s with_llvm=no PG_CONFIG="$pgc") >"$TMP/build-$major.log" 2>&1 \
		|| { cat "$TMP/build-$major.log" >&2; die "build failed for PostgreSQL $major"; }
	(cd "$build" && as_owner "$("$pgc" --pkglibdir)" make -s with_llvm=no PG_CONFIG="$pgc" install) \
		>>"$TMP/build-$major.log" 2>&1 \
		|| { cat "$TMP/build-$major.log" >&2; die "install failed for PostgreSQL $major"; }
}

# --- install --------------------------------------------------------------

INSTALLED=""
for pgc in $TARGETS; do
	major=$(pg_major "$pgc")
	if [ "$FROM_SOURCE" = 0 ] && install_prebuilt "$pgc" "$major"; then
		say "installed prebuilt pg_typesafe for PostgreSQL $major -> $("$pgc" --pkglibdir)"
	else
		install_source "$pgc" "$major"
		say "built and installed pg_typesafe for PostgreSQL $major -> $("$pgc" --pkglibdir)"
	fi
	INSTALLED="$INSTALLED $major"
	PSQL_BIN="$("$pgc" --bindir)/psql"
done

# --- enable in a database -------------------------------------------------

psql_cmd() {
	psql=psql
	[ -x "${PSQL_BIN:-}" ] && psql=$PSQL_BIN
	# as root with no libpq settings, connect as the postgres OS user
	if [ "$(id -u)" = 0 ] && [ -z "${PGUSER:-}${PGHOST:-}" ] && id postgres >/dev/null 2>&1; then
		if command -v runuser >/dev/null 2>&1; then
			runuser -u postgres -- "$psql" "$@"
		else
			# shellcheck disable=SC2016  # expanded by the inner shell
			su postgres -s /bin/sh -c '"$0" "$@"' "$psql" "$@"
		fi
	else
		"$psql" "$@"
	fi
}

if [ -n "$DB" ]; then
	say "enabling typesafe in database \"$DB\""
	(cd / && psql_cmd -X -q -v ON_ERROR_STOP=1 -d "$DB" \
		-c "SET client_min_messages = warning" -c "CREATE EXTENSION IF NOT EXISTS typesafe") \
		|| die "could not CREATE EXTENSION in \"$DB\" (connect as a superuser: set PGUSER/PGHOST, or run as root)"

	# smoke test: the shared library loads and parses a (mock) response
	(cd / && psql_cmd -X -q -A -t -v ON_ERROR_STOP=1 -d "$DB") >/dev/null <<-'SQL' \
		|| die "typesafe installed but failed its load test in \"$DB\""
	BEGIN;
	SET LOCAL typesafe.mock_response = '{"model":"m","answers":{"flag":{"type":"noul","noul":1}},"usage":{"input_tokens":1,"output_tokens":1}}';
	SELECT typesafe_noul('x', 'y');
	ROLLBACK;
	SQL

	if [ -n "${TYPESAFE_API_KEY:-}" ]; then
		# The key reaches the server through COPY FROM STDIN, so it never
		# appears in statement text or logs.  The server writes the file
		# itself, owned by the server user inside the data directory (which
		# PostgreSQL keeps at mode 0700/0750).
		say "storing TYPESAFE_API_KEY in the data directory (typesafe.api_key_file)"
		key=$(printf '%s' "$TYPESAFE_API_KEY" | tr -d '\r\n')
		# shellcheck disable=SC1003  # a literal backslash
		case "$key" in *'\'*|*'	'*) die "TYPESAFE_API_KEY contains unexpected characters" ;; esac
		{
			cat <<-'SQL'
			-- PG15 rejects ALTER SYSTEM on a custom GUC until its module is loaded
			LOAD 'typesafe';
			CREATE TEMP TABLE typesafe_key_tmp (k text);
			COPY typesafe_key_tmp FROM STDIN;
			SQL
			printf '%s\n\\.\n' "$key"
			cat <<-'SQL'
			SELECT current_setting('data_directory') || '/typesafe.key' AS keypath \gset
			COPY typesafe_key_tmp TO :'keypath';
			ALTER SYSTEM SET typesafe.api_key_file = :'keypath';
			SELECT pg_reload_conf() \gset
			SQL
		} | (cd / && psql_cmd -X -q -v ON_ERROR_STOP=1 -d "$DB") >/dev/null 2>"$TMP/key.err" \
			|| { cat "$TMP/key.err" >&2; die "could not store the API key"; }
		KEY_STORED=1
	fi
fi

# --- next steps -----------------------------------------------------------

printf '\n\033[32mpg_typesafe installed for PostgreSQL%s.\033[0m\n\n' "$INSTALLED" >&2
if [ -z "$DB" ]; then
	cat >&2 <<-EOF
	Enable it (as a superuser) in your database:

	    CREATE EXTENSION typesafe;

	EOF
fi
if [ -z "${KEY_STORED:-}" ]; then
	cat >&2 <<-EOF
	Give the server your API key without a restart (the file must be
	readable by the server's OS user):

	    ALTER SYSTEM SET typesafe.api_key_file = '/path/to/typesafe.key';
	    SELECT pg_reload_conf();

	or re-run this installer with TYPESAFE_API_KEY=tsk_... and --db <name>.

	EOF
fi
cat >&2 <<-EOF
Try it:

    SELECT typesafe_noul('My payouts have failed for 3 days!', 'Is this urgent?');

EOF
