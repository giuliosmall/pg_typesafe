# pg_typesafe

[![ci](https://github.com/giuliosmall/pg_typesafe/actions/workflows/ci.yml/badge.svg)](https://github.com/giuliosmall/pg_typesafe/actions/workflows/ci.yml)

**Pre-alpha.** A PostgreSQL extension that calls [TypeSafe AI](https://console.typesafe.ai/home) (System One / Jev) from SQL for categorical work: Choice, Noul, and Score.

Not affiliated with TypeSafe AI or the PostgreSQL Global Development Group.

Tested on **PostgreSQL 16 and 17**, libcurl 7.61+.

## Install

```bash
git clone https://github.com/giuliosmall/pg_typesafe.git
cd pg_typesafe
make
make install   # needs write access to pkglibdir (often sudo)
```

```sql
CREATE EXTENSION typesafe;
```

`EXECUTE` is revoked from `PUBLIC`. The owner (usually a superuser) can run the functions. To let an app role call them:

```sql
GRANT EXECUTE ON FUNCTION typesafe_noul(text, text, text, text, text) TO app;
GRANT EXECUTE ON FUNCTION typesafe_detect_many(text[], text, text, text, text) TO app;
```

## API key

Set `TYPESAFE_API_KEY` on the **Postgres server** process. Do not put it in SQL.

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

1,000 closed NYC 311 complaints, **38 unique** resolution strings. `typesafe_detect_many` classifies those 38 in one TypeSafe request (not 1,000 HTTP calls).

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

## License

MIT. See [LICENSE](LICENSE).

[TypeSafe API](https://docs.typesafe.ai/api) · [Choice](https://docs.typesafe.ai/primitives/choice)
