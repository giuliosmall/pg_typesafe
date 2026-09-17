-- NYC 311 demo for pg_typesafe.
-- Run from the repo root, against a server that has TYPESAFE_API_KEY
-- in its process environment (not in this file):
--
--   psql -d postgres -v ON_ERROR_STOP=1 -f examples/311.sql
--
-- Data: 1000 closed NYC 311 requests (NYC Open Data, 311 Service Requests).
-- 38 distinct resolution texts. typesafe_detect_many packs those into
-- one or two TypeSafe round trips (~0.9s here), not 1000 HTTP calls.

\set ON_ERROR_STOP on
\timing off

CREATE EXTENSION IF NOT EXISTS typesafe;

DROP TABLE IF EXISTS complaints;
CREATE TABLE complaints (
	id              bigint PRIMARY KEY,
	agency          text,
	complaint_type  text,
	resolution      text,
	created_at      timestamptz,
	closed_at       timestamptz
);

\copy complaints FROM 'examples/nyc311.tsv'

SELECT count(*) AS rows,
       count(DISTINCT resolution) AS unique_resolutions
FROM complaints;

-- Scalar, DuckDB-style. One HTTP call.
\timing on
SELECT typesafe_noul(
	'The Department of Sanitation (DSNY) couldn''t find the condition. Please file a new Service Request if it still exists.',
	'Does this resolution say the condition could not be found, or that access was unavailable?'
) AS no_access;
\timing off

-- Batched: one TypeSafe request for all distinct resolutions, then join.
\timing on
WITH labels AS (
	SELECT state AS resolution, noul AS no_access
	FROM typesafe_detect_many(
		ARRAY(SELECT DISTINCT resolution FROM complaints),
		'Does this resolution explicitly say the condition could not be found, or that an inspection/investigation could not be completed because access or entry was unavailable?'
	)
)
SELECT
	c.agency,
	c.complaint_type,
	count(*) AS closed,
	round(
		(percentile_cont(0.5) WITHIN GROUP (
			ORDER BY extract(epoch FROM (c.closed_at - c.created_at))
		) / 86400.0)::numeric,
		1
	) AS median_days
FROM complaints c
JOIN labels l USING (resolution)
WHERE l.no_access >= 0.8
GROUP BY c.agency, c.complaint_type
ORDER BY closed DESC, c.agency, c.complaint_type
LIMIT 5;
