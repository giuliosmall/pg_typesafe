# pg_typesafe

**Pre-pre-pre-alpha.** A PostgreSQL extension that calls [TypeSafe AI](https://console.typesafe.ai/home) (System One / Jev) from SQL for categorical work: Choice, Noul, and Score.

Not affiliated with TypeSafe AI or the PostgreSQL Global Development Group.

## Install

Needs PostgreSQL 16+ (built with a C compiler) and libcurl 7.61+.

```bash
make
make install   # as a user who can write $libdir
```

```sql
CREATE EXTENSION typesafe;
```

## API key

Do **not** put the key in SQL in production. Set it on the **server** process:

```bash
export TYPESAFE_API_KEY=tsk_...
pg_ctl restart
```

Then queries contain no secret:

```sql
SELECT * FROM typesafe_detect(
    'Help! My payouts have been failing for 3 days.',
    'Does this convey urgency?'
);
```

`SET typesafe.api_key = '...'` works for a session but shows up in query logs. Prefer the env var.

## Functions

| Function | TypeSafe primitive | Use |
|---|---|---|
| `typesafe_classify` / `typesafe_label` | Choice | one category |
| `typesafe_detect` | Noul | yes/no probability |
| `typesafe_score` | Score | ordered rubric |
| `typesafe_ask` | mixed | several questions, one request |
| `typesafe_detect_many` / `typesafe_classify_many` / `typesafe_label_many` | batched | many texts, few HTTP round trips |

Scalars issue **one HTTP call per row**. For a table, pack distinct values:

```sql
WITH labels AS (
    SELECT state AS resolution, noul AS no_access
    FROM typesafe_detect_many(
        ARRAY(SELECT DISTINCT resolution FROM complaints
              WHERE resolution IS NOT NULL),
        'Does this resolution say the condition could not be found?'
    )
)
SELECT c.agency, count(*) 
FROM complaints c
JOIN labels l USING (resolution)
WHERE l.no_access >= 0.8
GROUP BY 1;
```

`typesafe.batch_size` (default 32) is how many texts share one TypeSafe request. Extra chunks overlap (`typesafe.http_concurrency`, default 4).

## Measured (laptop, live Jev)

1,000 real NYC 311 complaints, 38 unique resolution texts:

| Method | Time |
|---|---|
| `typesafe_detect` once per distinct text | 23 s |
| `typesafe_detect_many` | 0.86 s |

## Tests without the network

```sql
SET typesafe.mock_response = $${
  "model": "jev-latest",
  "answers": {
    "flag": {"type": "noul", "noul": 0.92}
  },
  "usage": {"input_tokens": 1, "output_tokens": 1}
}$$;

SELECT noul FROM typesafe_detect('anything', 'Is this urgent?');
```

## License

MIT. See [LICENSE](LICENSE).

Docs: [TypeSafe API](https://docs.typesafe.ai/api) · [Choice](https://docs.typesafe.ai/primitives/choice)
