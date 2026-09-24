#!/bin/sh
# Runs once, when the container initializes an empty data directory.
#
# Enables typesafe in template1 (so every later CREATE DATABASE has it) and
# in POSTGRES_DB.  TYPESAFE_API_KEY_FILE (e.g. a Docker secret) is persisted
# as typesafe.api_key_file; TYPESAFE_API_KEY in the container environment
# works without any setup.
set -eu

for db in template1 "${POSTGRES_DB:-$POSTGRES_USER}"; do
	psql -X -q -v ON_ERROR_STOP=1 --username "$POSTGRES_USER" --no-password --dbname "$db" \
		-c 'CREATE EXTENSION IF NOT EXISTS typesafe'
done

if [ -n "${TYPESAFE_API_KEY_FILE:-}" ]; then
	echo "ALTER SYSTEM SET typesafe.api_key_file = :'keyfile';" \
		| psql -X -q -v ON_ERROR_STOP=1 --username "$POSTGRES_USER" --no-password \
			--dbname postgres -v keyfile="$TYPESAFE_API_KEY_FILE"
fi
