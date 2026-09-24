# pg_typesafe

[![ci](https://github.com/giuliosmall/pg_typesafe/actions/workflows/ci.yml/badge.svg)](https://github.com/giuliosmall/pg_typesafe/actions/workflows/ci.yml)

**Pre-alpha.** A PostgreSQL extension that calls [TypeSafe AI](https://console.typesafe.ai/home) (System One / Jev) from SQL for categorical work: Choice, Noul, and Score.

Not affiliated with TypeSafe AI or the PostgreSQL Global Development Group.

Supports **PostgreSQL 15, 16, 17 and 18** on Linux and macOS, libcurl 7.61+.

## Install

### One line

```bash
curl -fsSL https://raw.githubusercontent.com/giuliosmall/pg_typesafe/main/install.sh | sh
```

The installer finds every PostgreSQL 15+ server on the machine, installs a
checksum-verified prebuilt binary (Linux x86_64/arm64, glibc) or builds from
source (macOS, Alpine, anything else), and installs any missing build
dependencies with the system package manager (apt, dnf/yum, zypper, apk,
pacman, Homebrew). It uses `sudo` only where it has to.

Install, enable in a database, and hand the server your API key in one step,
with no restart:

```bash
curl -fsSL https://raw.githubusercontent.com/giuliosmall/pg_typesafe/main/install.sh \
  | sudo TYPESAFE_API_KEY=tsk_... sh -s -- --db mydb
```

`--db` runs `CREATE EXTENSION` and a load test. With `TYPESAFE_API_KEY` set,
the key is sent to the server over `COPY ... FROM STDIN` (never in statement
text or logs), written to `typesafe.key` in the data directory, and
`typesafe.api_key_file` is pointed at it.

| Option | |
|---|---|
| `--db NAME` | enable in `NAME` (connects as the `postgres` OS user when run as root, else via `PGHOST`/`PGUSER`) |
| `--pg-config PATH` | install for one PostgreSQL only |
| `--version TAG` | a specific release instead of the latest |
| `--source` | always build from source |
| `--no-deps` | do not install build dependencies |

### Docker

```bash
docker run -d -p 5432:5432 \
  -e POSTGRES_PASSWORD=pw \
  -e TYPESAFE_API_KEY=tsk_... \
  ghcr.io/giuliosmall/pg_typesafe:17
```

The official `postgres` image with the extension preinstalled and already
created in `POSTGRES_DB` and `template1` (so every new database has it). Tags:
`15`, `16`, `17`, `18`, `latest` (= 17), and `<major>-<version>`; amd64 and
arm64. To keep the key out of `docker inspect`, mount it as a secret and set
`TYPESAFE_API_KEY_FILE=/run/secrets/typesafe_key` instead.

```yaml
# compose.yaml
services:
  db:
    image: ghcr.io/giuliosmall/pg_typesafe:17
    environment:
      POSTGRES_PASSWORD: pw
      TYPESAFE_API_KEY_FILE: /run/secrets/typesafe_key
    secrets: [typesafe_key]
    ports: ["5432:5432"]
secrets:
  typesafe_key:
    file: ./typesafe.key
```

### PGXN

Coming soon: the PGXN release is pending account approval. Once published:

```bash
pgxn install typesafe
```

Until then, use the one-liner, Docker, or the source zip attached to the
[latest release](https://github.com/giuliosmall/pg_typesafe/releases/latest).

### From source

```bash
git clone https://github.com/giuliosmall/pg_typesafe.git
cd pg_typesafe
make
make install   # needs write access to pkglibdir (often sudo)
```

Needs the PostgreSQL server headers (`postgresql-server-dev-<major>` /
`postgresql<major>-devel`) and libcurl headers.

### Enable

```sql
CREATE EXTENSION typesafe;
```

`EXECUTE` is revoked from `PUBLIC`. The owner (usually a superuser) can run the functions. To let an app role call them:

```sql
GRANT EXECUTE ON FUNCTION typesafe_noul(text, text, text, text, text) TO app;
GRANT EXECUTE ON FUNCTION typesafe_detect_many(text[], text, text, text, text) TO app;
```

## API key

The installer (`--db` with `TYPESAFE_API_KEY`) and the Docker image set this
up for you. Manually: set `TYPESAFE_API_KEY` on the **Postgres server**
process. Do not put it in SQL.

```bash
export TYPESAFE_API_KEY=tsk_...
pg_ctl restart
```

Alternatively point `typesafe.api_key_file` (superuser-only GUC) at a file whose
first line is the key — this keeps the key out of SQL, logs, and
`postgresql.auto.conf`:

```sql
ALTER SYSTEM SET typesafe.api_key_file = '/etc/postgresql/typesafe.key';
SELECT pg_reload_conf();
```

```sql
SELECT typesafe_noul(
    'Help! My payouts have been failing for 3 days.',
    'Does this convey urgency?'
);
```

`SET typesafe.api_key` works for a session (superuser only) but appears in query logs.

`typesafe.endpoint` must be `https://`; plain `http://` is allowed only toward
localhost (used by the test suite). Responses are capped at 8 MB. HTTP 429/529
are retried up to 3 times per request with exponential backoff, honoring
`Retry-After`. Requests remain cancellable (Ctrl-C, `statement_timeout`) while
in flight.

## Demo (NYC 311)

From the repo root, with the key in the server environment:

```bash
psql -d postgres -v ON_ERROR_STOP=1 -f examples/311.sql
```

1,000 closed NYC 311 complaints, **38 unique** resolution strings. `typesafe_detect_many` classifies those 38 in two TypeSafe requests at the default `typesafe.batch_size = 32` (not 1,000 HTTP calls).

Measured on a laptop against live Jev:

| Method | Time |
|---|---|
| `typesafe_detect` once per distinct text | 23 s |
| `typesafe_detect_many` | 0.86 s |

## Functions

| Function | Primitive | Use |
|---|---|---|
| `typesafe_noul(text, text)` | Noul | yes/no probability (scalar) |
| `typesafe_detect` | Noul | same, plus model / tokens |
| `typesafe_classify` / `typesafe_label` | Choice | one category |
| `typesafe_score` | Score | ordered rubric |
| `typesafe_ask` | mixed | several questions, one request |
| `typesafe_detect_many` / `typesafe_classify_many` | batched | many texts, few HTTP round trips |

Scalars issue **one HTTP call per row**. For a table, batch distinct values:

```sql
SELECT resolution, typesafe_noul(resolution, 'Could the condition not be found?')
FROM (SELECT DISTINCT resolution FROM complaints) s;

-- Faster for a set:
SELECT state AS resolution, noul
FROM typesafe_detect_many(
    ARRAY(SELECT DISTINCT resolution FROM complaints),
    'Could the condition not be found?'
);
```

`typesafe.batch_size` (default 32) is how many texts share one TypeSafe request. Extra chunks overlap (`typesafe.http_concurrency`, default 4).

In `*_many` results, `input_tokens`/`output_tokens` are per HTTP request and
reported on each chunk's **first row only** (NULL on the rest), so
`SUM(input_tokens)` over the result is the true total.

## Tests without the network

`typesafe.mock_response` short-circuits the HTTP call and returns the given
body verbatim. It is superuser-only: a granted app role must not be able to
forge classification answers.

```sql
SET typesafe.mock_response = $${
  "model": "jev-latest",
  "answers": {
    "flag": {"type": "noul", "noul": 0.92}
  },
  "usage": {"input_tokens": 1, "output_tokens": 1}
}$$;

SELECT typesafe_noul('anything', 'Is this urgent?');
```

Mock regression: `sql/typesafe.sql` / `expected/typesafe.out`.

## Releasing

Bump `default_version` in `typesafe.control` and `version` in `META.json`, add
the matching `typesafe--<version>.sql` (plus an upgrade script), then push a
tag:

```bash
git tag v0.0.2 && git push origin v0.0.2
```

`.github/workflows/release.yml` builds and smoke-tests prebuilt binaries for
every supported major on amd64 and arm64, publishes the GitHub release that
`install.sh` downloads from, pushes the Docker images to ghcr.io, and uploads to
PGXN when the `PGXN_USERNAME` / `PGXN_PASSWORD` secrets are set.

## License

MIT. See [LICENSE](LICENSE).

[TypeSafe API](https://docs.typesafe.ai/api) · [Choice](https://docs.typesafe.ai/primitives/choice)
