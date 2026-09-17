/* contrib/typesafe/typesafe--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION typesafe" to load this file. \quit

CREATE TYPE typesafe_choice AS (
	choice text,
	confidence double precision,
	probabilities jsonb,
	model text,
	input_tokens integer,
	output_tokens integer
);

CREATE TYPE typesafe_noul AS (
	noul double precision,
	model text,
	input_tokens integer,
	output_tokens integer
);

CREATE TYPE typesafe_score AS (
	score double precision,
	confidence double precision,
	legend jsonb,
	probabilities jsonb,
	model text,
	input_tokens integer,
	output_tokens integer
);

CREATE FUNCTION typesafe_classify(
	state text,
	instructions text,
	options jsonb,
	model text DEFAULT NULL
)
RETURNS typesafe_choice
AS 'MODULE_PATHNAME', 'typesafe_classify'
LANGUAGE C VOLATILE PARALLEL UNSAFE;

CREATE FUNCTION typesafe_detect(
	state text,
	instructions text,
	true_meaning text DEFAULT NULL,
	false_meaning text DEFAULT NULL,
	model text DEFAULT NULL
)
RETURNS typesafe_noul
AS 'MODULE_PATHNAME', 'typesafe_detect'
LANGUAGE C VOLATILE PARALLEL UNSAFE;

CREATE FUNCTION typesafe_score(
	state text,
	instructions text,
	levels text[],
	model text DEFAULT NULL
)
RETURNS typesafe_score
AS 'MODULE_PATHNAME', 'typesafe_score'
LANGUAGE C VOLATILE PARALLEL UNSAFE;

CREATE FUNCTION typesafe_ask(
	state jsonb,
	questions jsonb,
	model text DEFAULT NULL
)
RETURNS jsonb
AS 'MODULE_PATHNAME', 'typesafe_ask'
LANGUAGE C VOLATILE PARALLEL UNSAFE;

CREATE FUNCTION typesafe_ask(
	state text,
	questions jsonb,
	model text DEFAULT NULL
)
RETURNS jsonb
AS 'MODULE_PATHNAME', 'typesafe_ask_text'
LANGUAGE C VOLATILE PARALLEL UNSAFE;

CREATE FUNCTION typesafe_label(
	state text,
	instructions text,
	options jsonb,
	model text DEFAULT NULL
)
RETURNS text
AS 'MODULE_PATHNAME', 'typesafe_label'
LANGUAGE C VOLATILE PARALLEL UNSAFE;

CREATE FUNCTION typesafe_last_request()
RETURNS text
AS 'MODULE_PATHNAME', 'typesafe_last_request'
LANGUAGE C VOLATILE PARALLEL UNSAFE;

CREATE FUNCTION typesafe_detect_many(
	states text[],
	instructions text,
	true_meaning text DEFAULT NULL,
	false_meaning text DEFAULT NULL,
	model text DEFAULT NULL
)
RETURNS TABLE(
	ordinality integer,
	state text,
	noul double precision,
	model text,
	input_tokens integer,
	output_tokens integer
)
AS 'MODULE_PATHNAME', 'typesafe_detect_many'
LANGUAGE C VOLATILE PARALLEL UNSAFE;

CREATE FUNCTION typesafe_classify_many(
	states text[],
	instructions text,
	options jsonb,
	model text DEFAULT NULL
)
RETURNS TABLE(
	ordinality integer,
	state text,
	choice text,
	confidence double precision,
	probabilities jsonb,
	model text,
	input_tokens integer,
	output_tokens integer
)
AS 'MODULE_PATHNAME', 'typesafe_classify_many'
LANGUAGE C VOLATILE PARALLEL UNSAFE;

CREATE FUNCTION typesafe_label_many(
	states text[],
	instructions text,
	options jsonb,
	model text DEFAULT NULL
)
RETURNS TABLE(
	ordinality integer,
	state text,
	choice text
)
AS $$
	SELECT ordinality, state, choice
	FROM typesafe_classify_many(states, instructions, options, model);
$$ LANGUAGE SQL VOLATILE PARALLEL UNSAFE;
