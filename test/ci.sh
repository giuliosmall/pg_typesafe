#!/bin/sh
set -eu
psql -v ON_ERROR_STOP=1 -f test/ci_mock.sql
psql -v ON_ERROR_STOP=1 -c "CREATE ROLE typesafe_nobody NOLOGIN;"
err=$(mktemp)
if psql -c "SET SESSION AUTHORIZATION typesafe_nobody; SELECT typesafe_noul('x', 'y');" 2>"$err"; then
	echo "expected permission denied for PUBLIC" >&2
	cat "$err" >&2
	exit 1
fi
if ! grep -qi "permission denied" "$err"; then
	echo "expected permission denied, got:" >&2
	cat "$err" >&2
	exit 1
fi
echo "ci mock tests ok"
