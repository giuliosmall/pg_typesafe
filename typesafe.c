/*-------------------------------------------------------------------------
 *
 * typesafe.c
 *		TypeSafe AI client for categorical classification
 *
 * Copyright (c) 2026, Giulio Piccolo
 *
 * IDENTIFICATION
 *		  contrib/typesafe/typesafe.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <stdint.h>
#include <stdio.h>

#include <curl/curl.h>

#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "fmgr.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "storage/latch.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/json.h"
#include "utils/jsonb.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"
#include "utils/tuplestore.h"
#include "utils/wait_event.h"
#include "varatt.h"

#ifdef PG_MODULE_MAGIC_EXT
PG_MODULE_MAGIC_EXT(
					.name = "typesafe",
					.version = "0.0.1"
);
#else
PG_MODULE_MAGIC;
#endif

PG_FUNCTION_INFO_V1(typesafe_classify);
PG_FUNCTION_INFO_V1(typesafe_detect);
PG_FUNCTION_INFO_V1(typesafe_score);
PG_FUNCTION_INFO_V1(typesafe_ask);
PG_FUNCTION_INFO_V1(typesafe_ask_text);
PG_FUNCTION_INFO_V1(typesafe_label);
PG_FUNCTION_INFO_V1(typesafe_last_request);
PG_FUNCTION_INFO_V1(typesafe_detect_many);
PG_FUNCTION_INFO_V1(typesafe_classify_many);

#define TYPESAFE_DEFAULT_ENDPOINT \
	"https://api.typesafe.ai/v1/systemone"
#define TYPESAFE_DEFAULT_MODEL	"jev-latest"
#define TYPESAFE_USER_AGENT_HDR	"User-Agent: postgres-typesafe/1.0"
#define TYPESAFE_BODY_TRUNC		1024
#define TYPESAFE_MAX_RETRIES		3
#define TYPESAFE_MAX_RESPONSE_BYTES	(8 * 1024 * 1024)
#define TYPESAFE_RETRY_MAX_WAIT_MS	60000
#define TYPESAFE_WAIT_QUANTUM_MS	200

/* GUC variables */
static char *typesafe_api_key = NULL;
static char *typesafe_api_key_file = NULL;
static char *typesafe_endpoint = NULL;
static char *typesafe_model = NULL;
static int	typesafe_timeout_ms = 30000;
static char *typesafe_mock_response = NULL;
static int	typesafe_batch_size = 32;
static int	typesafe_http_concurrency = 4;

/* Last POST body, allocated in TopMemoryContext. */
static char *last_request_json = NULL;

static void json_shape_error(const char *detail) pg_attribute_noreturn();
static char *get_text_arg(FunctionCallInfo fcinfo, int argno, const char *name);
static Jsonb *get_jsonb_arg(FunctionCallInfo fcinfo, int argno,
							const char *name);
static char *resolve_model(FunctionCallInfo fcinfo, int argno);
static const char *resolve_api_key(void);
static void save_last_request(const char *body);
static size_t write_callback(char *ptr, size_t size, size_t nmemb,
							 void *userdata);
static char *truncate_body(const char *body, int len);
static void check_endpoint(const char *endpoint);
static void setup_easy_handle(CURL *curl, struct curl_slist *headers,
							  const char *payload, StringInfo response,
							  int idx);
static char **http_post_live(char **requests, int nrequests);
static char *http_post_json(const char *request_json);
static char *execute_request(const char *request_json);
static void append_request_prelude(StringInfo buf, const char *state_text,
								   Jsonb *state_jsonb, const char *model);
static void append_choice_criteria(StringInfo buf, Jsonb *options);
static void append_score_levels(StringInfo buf, ArrayType *levels);
static char *build_classify_request(FunctionCallInfo fcinfo);
static Jsonb *parse_response_json(const char *body);
static bool jsonb_str_eq(const JsonbValue *v, const char *s);
static char *jsonb_str_cstring(const JsonbValue *v);
static float8 jsonb_numeric_float8(const JsonbValue *v);
static int32 jsonb_numeric_int4(const JsonbValue *v);
static JsonbContainer *jsonb_value_object_container(JsonbValue *v);
static Jsonb *jsonb_value_object_to_jsonb(JsonbValue *v);
static char *response_model(Jsonb *resp);
static void extract_usage(Jsonb *resp, bool *in_isnull, int32 *in_tok,
						  bool *out_isnull, int32 *out_tok);
static JsonbContainer *get_answer_object(Jsonb *resp, const char *qid,
										 const char *expected_type);
static Datum form_composite(FunctionCallInfo fcinfo, Datum *values,
							bool *nulls);
static Datum form_choice_result(FunctionCallInfo fcinfo, Jsonb *resp);
static Datum form_noul_result(FunctionCallInfo fcinfo, Jsonb *resp);
static Datum form_score_result(FunctionCallInfo fcinfo, Jsonb *resp);
static char *choice_from_response(Jsonb *resp);
static void append_noul_criteria(StringInfo buf, const char *true_meaning,
								 const char *false_meaning);
static char **http_post_many(char **requests, int nrequests);

/*
 * Module load callback: register GUCs and initialize libcurl once.
 */
void
_PG_init(void)
{
	DefineCustomStringVariable("typesafe.api_key",
							   "API key for TypeSafe System One.",
							   NULL,
							   &typesafe_api_key,
							   "",
							   PGC_SUSET,
							   GUC_NO_SHOW_ALL | GUC_SUPERUSER_ONLY | GUC_NOT_IN_SAMPLE,
							   NULL,
							   NULL,
							   NULL);

	DefineCustomStringVariable("typesafe.api_key_file",
							   "File holding the TypeSafe API key (first line).",
							   NULL,
							   &typesafe_api_key_file,
							   "",
							   PGC_SUSET,
							   GUC_SUPERUSER_ONLY | GUC_NOT_IN_SAMPLE,
							   NULL,
							   NULL,
							   NULL);

	DefineCustomStringVariable("typesafe.endpoint",
							   "TypeSafe System One HTTP endpoint.",
							   NULL,
							   &typesafe_endpoint,
							   TYPESAFE_DEFAULT_ENDPOINT,
							   PGC_SUSET,
							   0,
							   NULL,
							   NULL,
							   NULL);

	DefineCustomStringVariable("typesafe.model",
							   "Default TypeSafe model name.",
							   NULL,
							   &typesafe_model,
							   TYPESAFE_DEFAULT_MODEL,
							   PGC_USERSET,
							   0,
							   NULL,
							   NULL,
							   NULL);

	DefineCustomIntVariable("typesafe.timeout_ms",
							"HTTP timeout for TypeSafe requests.",
							NULL,
							&typesafe_timeout_ms,
							30000,
							1,
							600000,
							PGC_USERSET,
							GUC_UNIT_MS,
							NULL,
							NULL,
							NULL);

	DefineCustomStringVariable("typesafe.mock_response",
							   "If set, use this string as the HTTP response body.",
							   NULL,
							   &typesafe_mock_response,
							   "",
							   PGC_USERSET,
							   0,
							   NULL,
							   NULL,
							   NULL);

	DefineCustomIntVariable("typesafe.batch_size",
							"Max items packed into one TypeSafe request.",
							NULL,
							&typesafe_batch_size,
							32,
							1,
							128,
							PGC_USERSET,
							0,
							NULL,
							NULL,
							NULL);

	DefineCustomIntVariable("typesafe.http_concurrency",
							"Max simultaneous HTTP requests for batched calls.",
							NULL,
							&typesafe_http_concurrency,
							4,
							1,
							16,
							PGC_USERSET,
							0,
							NULL,
							NULL,
							NULL);

	MarkGUCPrefixReserved("typesafe");

	if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("could not initialize HTTP client")));
}

static void
json_shape_error(const char *detail)
{
	ereport(ERROR,
			(errcode(ERRCODE_INVALID_JSON_TEXT),
			 errmsg("invalid typesafe response: %s", detail)));
}

static char *
get_text_arg(FunctionCallInfo fcinfo, int argno, const char *name)
{
	if (PG_ARGISNULL(argno))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("%s must not be null", name)));

	return text_to_cstring(PG_GETARG_TEXT_PP(argno));
}

static Jsonb *
get_jsonb_arg(FunctionCallInfo fcinfo, int argno, const char *name)
{
	if (PG_ARGISNULL(argno))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("%s must not be null", name)));

	return PG_GETARG_JSONB_P(argno);
}

/*
 * Function model argument, if present and non-empty; otherwise the GUC.
 */
static char *
resolve_model(FunctionCallInfo fcinfo, int argno)
{
	if (argno < PG_NARGS() && !PG_ARGISNULL(argno))
	{
		char	   *model = text_to_cstring(PG_GETARG_TEXT_PP(argno));

		if (model[0] != '\0')
			return model;
	}

	if (typesafe_model != NULL)
		return typesafe_model;

	return TYPESAFE_DEFAULT_MODEL;
}

/*
 * GUC, then typesafe.api_key_file, then TYPESAFE_API_KEY.  Never log the
 * value.
 */
static const char *
resolve_api_key(void)
{
	const char *env;

	if (typesafe_api_key != NULL && typesafe_api_key[0] != '\0')
		return typesafe_api_key;

	if (typesafe_api_key_file != NULL && typesafe_api_key_file[0] != '\0')
	{
		FILE	   *f;
		char		line[512];
		char	   *key = NULL;

		f = AllocateFile(typesafe_api_key_file, "r");
		if (f == NULL)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open typesafe.api_key_file \"%s\": %m",
							typesafe_api_key_file)));
		if (fgets(line, sizeof(line), f) != NULL)
		{
			int			len = strlen(line);

			while (len > 0 &&
				   (line[len - 1] == '\n' || line[len - 1] == '\r' ||
					line[len - 1] == ' ' || line[len - 1] == '	'))
				line[--len] = '\0';
			if (len > 0)
				key = pstrdup(line);
		}
		FreeFile(f);

		if (key == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("typesafe.api_key_file \"%s\" is empty",
							typesafe_api_key_file)));
		return key;
	}

	env = getenv("TYPESAFE_API_KEY");
	if (env != NULL && env[0] != '\0')
		return env;

	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("typesafe.api_key is not set")));
	return NULL;				/* keep compiler quiet */
}

static void
save_last_request(const char *body)
{
	char	   *copy;

	copy = MemoryContextStrdup(TopMemoryContext, body);
	if (last_request_json != NULL)
		pfree(last_request_json);
	last_request_json = copy;
}

static size_t
write_callback(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	StringInfo	buf = (StringInfo) userdata;
	size_t		nbytes = size * nmemb;

	/* Refuse to buffer absurdly large responses; curl aborts the transfer. */
	if ((size_t) buf->len + nbytes > TYPESAFE_MAX_RESPONSE_BYTES)
	{
		buf->cursor = 1;		/* flag: aborted due to size cap */
		return 0;
	}

	appendBinaryStringInfo(buf, ptr, nbytes);
	return nbytes;
}

static char *
truncate_body(const char *body, int len)
{
	if (len > TYPESAFE_BODY_TRUNC)
		len = TYPESAFE_BODY_TRUNC;

	return pnstrdup(body, len);
}

static struct curl_slist *
append_header(struct curl_slist *headers, const char *line)
{
	struct curl_slist *h = curl_slist_append(headers, line);

	if (h == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));

	return h;
}

/*
 * Reject endpoints that are not https://, except plain http:// to loopback
 * hosts (used by tests).  Defense in depth for SSRF/scheme confusion.
 */
static void
check_endpoint(const char *endpoint)
{
	if (endpoint == NULL || endpoint[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("typesafe.endpoint is not set")));

	if (pg_strncasecmp(endpoint, "https://", 8) == 0)
		return;

	if (pg_strncasecmp(endpoint, "http://127.0.0.1", 16) == 0 ||
		pg_strncasecmp(endpoint, "http://localhost", 16) == 0 ||
		pg_strncasecmp(endpoint, "http://[::1]", 12) == 0)
		return;

	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("typesafe.endpoint must use https (or http to localhost): %s",
					endpoint)));
}

/*
 * Common per-transfer options.  idx rides in CURLOPT_PRIVATE so the multi
 * loop can map a finished handle back to its slot.
 */
static void
setup_easy_handle(CURL *curl, struct curl_slist *headers,
				  const char *payload, StringInfo response, int idx)
{
	if (curl_easy_setopt(curl, CURLOPT_URL, typesafe_endpoint) != CURLE_OK ||
		curl_easy_setopt(curl, CURLOPT_POST, 1L) != CURLE_OK ||
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers) != CURLE_OK ||
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload) != CURLE_OK ||
		curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
						 (long) strlen(payload)) != CURLE_OK ||
		curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS,
						 (long) typesafe_timeout_ms) != CURLE_OK ||
		curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L) != CURLE_OK ||
		curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L) != CURLE_OK ||
		curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L) != CURLE_OK ||
		curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L) != CURLE_OK ||
		curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L) != CURLE_OK ||
#if LIBCURL_VERSION_NUM >= 0x075500	/* 7.85.0 */
		curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https") != CURLE_OK ||
#else
		curl_easy_setopt(curl, CURLOPT_PROTOCOLS,
						 CURLPROTO_HTTP | CURLPROTO_HTTPS) != CURLE_OK ||
#endif
		curl_easy_setopt(curl, CURLOPT_HTTP_VERSION,
						 (long) CURL_HTTP_VERSION_2TLS) != CURLE_OK ||
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
						 (curl_write_callback) write_callback) != CURLE_OK ||
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, response) != CURLE_OK ||
		curl_easy_setopt(curl, CURLOPT_PRIVATE,
						 (char *) (intptr_t) idx) != CURLE_OK)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("could not configure HTTP client")));
}

/*
 * Backoff before retry attempt 'attempt' (1-based), honoring the server's
 * Retry-After when libcurl exposes it.  Milliseconds, capped.
 */
static long
retry_delay_ms(CURL *eh, int attempt)
{
	long		delay = 200L * (1L << (attempt - 1));

#ifdef CURLINFO_RETRY_AFTER
	{
		curl_off_t	ra = 0;

		if (curl_easy_getinfo(eh, CURLINFO_RETRY_AFTER, &ra) == CURLE_OK &&
			ra > 0)
		{
			if (ra > TYPESAFE_RETRY_MAX_WAIT_MS / 1000)
				ra = TYPESAFE_RETRY_MAX_WAIT_MS / 1000;
			if (ra * 1000 > delay)
				delay = (long) ra * 1000;
		}
	}
#endif

	if (delay > TYPESAFE_RETRY_MAX_WAIT_MS)
		delay = TYPESAFE_RETRY_MAX_WAIT_MS;
	return delay;
}

/* Per-slot transfer state for http_post_live. */
typedef enum
{
	TS_SLOT_PENDING,			/* not yet started (or waiting on retry) */
	TS_SLOT_IN_FLIGHT,
	TS_SLOT_DONE
} ts_slot_state;

/*
 * POST nrequests bodies to typesafe.endpoint via curl_multi, at most
 * typesafe.http_concurrency in flight.  Retries HTTP 429/529 per handle
 * without stalling other transfers, stays responsive to query cancel
 * (CHECK_FOR_INTERRUPTS between waits, latch-based sleeps), and never
 * logs the API key.  Returns palloc'd response bodies in request order.
 */
static char **
http_post_live(char **requests, int nrequests)
{
	char	  **results;
	CURLM	   *volatile multi = NULL;
	struct curl_slist *volatile headers = NULL;
	CURL	  **volatile easies = NULL;
	StringInfoData *responses;
	const char **payloads;
	int		   *attempts;
	TimestampTz *retry_at;
	ts_slot_state *slots;
	int			concurrency;
	int			in_flight = 0;
	int			ndone = 0;
	const char *apikey;
	int			i;

	results = (char **) palloc0(sizeof(char *) * nrequests);
	if (nrequests <= 0)
		return results;

	check_endpoint(typesafe_endpoint);
	apikey = resolve_api_key();

	concurrency = typesafe_http_concurrency;
	if (concurrency > nrequests)
		concurrency = nrequests;

	easies = (CURL **) palloc0(sizeof(CURL *) * nrequests);
	responses = (StringInfoData *) palloc(sizeof(StringInfoData) * nrequests);
	payloads = (const char **) palloc(sizeof(char *) * nrequests);
	attempts = (int *) palloc0(sizeof(int) * nrequests);
	retry_at = (TimestampTz *) palloc0(sizeof(TimestampTz) * nrequests);
	slots = (ts_slot_state *) palloc0(sizeof(ts_slot_state) * nrequests);

	for (i = 0; i < nrequests; i++)
	{
		initStringInfo(&responses[i]);
		payloads[i] = pg_server_to_any(requests[i], strlen(requests[i]),
									   PG_UTF8);
	}

	multi = curl_multi_init();
	if (multi == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("could not initialize HTTP client")));

	PG_TRY();
	{
		char	   *auth;

		headers = append_header(NULL, "Content-Type: application/json");
		headers = append_header(headers, "Accept: application/json");
		headers = append_header(headers, TYPESAFE_USER_AGENT_HDR);
		/* suppress libcurl's Expect: 100-continue round trip */
		headers = append_header(headers, "Expect:");

		auth = psprintf("Authorization: Bearer %s", apikey);
		headers = append_header(headers, auth);
		pfree(auth);

		while (ndone < nrequests)
		{
			TimestampTz now;
			int			still_running = 0;
			CURLMsg    *msg;
			int			left;
			bool		waiting_retry = false;
			long		sleep_ms;

			CHECK_FOR_INTERRUPTS();

			now = GetCurrentTimestamp();

			/* Launch pending slots whose retry deadline (if any) passed. */
			for (i = 0; i < nrequests && in_flight < concurrency; i++)
			{
				if (slots[i] != TS_SLOT_PENDING)
					continue;
				if (retry_at[i] != 0 && retry_at[i] > now)
					continue;

				if (easies[i] == NULL)
				{
					easies[i] = curl_easy_init();
					if (easies[i] == NULL)
						ereport(ERROR,
								(errcode(ERRCODE_CONNECTION_FAILURE),
								 errmsg("could not initialize HTTP client")));
					setup_easy_handle(easies[i], headers, payloads[i],
									  &responses[i], i);
				}

				resetStringInfo(&responses[i]);
				responses[i].cursor = 0;	/* clear size-cap abort flag */
				if (curl_multi_add_handle(multi, easies[i]) != CURLM_OK)
					ereport(ERROR,
							(errcode(ERRCODE_CONNECTION_FAILURE),
							 errmsg("could not start HTTP request")));
				slots[i] = TS_SLOT_IN_FLIGHT;
				in_flight++;
			}

			if (curl_multi_perform(multi, &still_running) != CURLM_OK)
				ereport(ERROR,
						(errcode(ERRCODE_CONNECTION_FAILURE),
						 errmsg("typesafe HTTP request failed")));

			while ((msg = curl_multi_info_read(multi, &left)) != NULL)
			{
				int			idx;
				char	   *priv = NULL;
				long		status = 0;
				CURL	   *eh;

				if (msg->msg != CURLMSG_DONE)
					continue;

				eh = msg->easy_handle;
				curl_easy_getinfo(eh, CURLINFO_PRIVATE, &priv);
				idx = (int) (intptr_t) priv;

				if (msg->data.result != CURLE_OK)
				{
					if (msg->data.result == CURLE_WRITE_ERROR &&
						responses[idx].cursor == 1)
						ereport(ERROR,
								(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
								 errmsg("typesafe HTTP response exceeds %d bytes",
										TYPESAFE_MAX_RESPONSE_BYTES)));
					ereport(ERROR,
							(errcode(ERRCODE_CONNECTION_FAILURE),
							 errmsg("typesafe HTTP request failed: %s",
									curl_easy_strerror(msg->data.result))));
				}

				curl_easy_getinfo(eh, CURLINFO_RESPONSE_CODE, &status);
				curl_multi_remove_handle(multi, eh);
				in_flight--;

				if (status == 200)
				{
					char	   *converted;

					converted = pg_any_to_server(responses[idx].data,
												 responses[idx].len,
												 PG_UTF8);
					if (converted == responses[idx].data)
						converted = pstrdup(responses[idx].data);
					results[idx] = converted;
					curl_easy_cleanup(eh);
					easies[idx] = NULL;
					slots[idx] = TS_SLOT_DONE;
					ndone++;
					continue;
				}

				if ((status == 429 || status == 529) &&
					attempts[idx] < TYPESAFE_MAX_RETRIES)
				{
					long		delay;

					attempts[idx]++;
					delay = retry_delay_ms(eh, attempts[idx]);
					retry_at[idx] =
						TimestampTzPlusMilliseconds(GetCurrentTimestamp(),
													delay);
					slots[idx] = TS_SLOT_PENDING;
					continue;
				}

				ereport(ERROR,
						(errcode(ERRCODE_CONNECTION_FAILURE),
						 errmsg("typesafe HTTP request failed with status %ld: %s",
								status,
								truncate_body(responses[idx].data,
											  responses[idx].len))));
			}

			if (ndone >= nrequests)
				break;

			/* Anything parked on a retry deadline? */
			sleep_ms = TYPESAFE_WAIT_QUANTUM_MS;
			now = GetCurrentTimestamp();
			for (i = 0; i < nrequests; i++)
			{
				if (slots[i] == TS_SLOT_PENDING && retry_at[i] != 0)
				{
					long		ms;

					waiting_retry = true;
					ms = (long) ((retry_at[i] - now) / 1000);
					if (ms < 1)
						ms = 1;
					if (ms < sleep_ms)
						sleep_ms = ms;
				}
			}

			if (in_flight > 0)
			{
				int			numfds = 0;

				/*
				 * Short quantum keeps us responsive to cancel even though
				 * curl_multi_wait does not watch the process latch.
				 */
				curl_multi_wait(multi, NULL, 0, (int) sleep_ms, &numfds);
			}
			else if (waiting_retry)
			{
				(void) WaitLatch(MyLatch,
								 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
								 sleep_ms,
								 PG_WAIT_EXTENSION);
				ResetLatch(MyLatch);
			}
		}
	}
	PG_FINALLY();
	{
		if (multi != NULL)
		{
			for (i = 0; i < nrequests; i++)
			{
				if (easies != NULL && easies[i] != NULL)
				{
					curl_multi_remove_handle(multi, easies[i]);
					curl_easy_cleanup(easies[i]);
					easies[i] = NULL;
				}
			}
			curl_multi_cleanup(multi);
		}
		if (headers != NULL)
			curl_slist_free_all(headers);
	}
	PG_END_TRY();

	return results;
}

/*
 * POST one request body.  Same interrupt-safe machinery as the batch path.
 */
static char *
http_post_json(const char *request_json)
{
	char	  **results;

	results = http_post_live((char **) &request_json, 1);
	return results[0];
}

static char *
execute_request(const char *request_json)
{
	save_last_request(request_json);

	if (typesafe_mock_response != NULL &&
		typesafe_mock_response[0] != '\0')
		return pstrdup(typesafe_mock_response);

	return http_post_json(request_json);
}

static void
append_request_prelude(StringInfo buf, const char *state_text,
					   Jsonb *state_jsonb, const char *model)
{
	appendStringInfoString(buf, "{\"state\": ");
	if (state_jsonb != NULL)
		JsonbToCString(buf, &state_jsonb->root, VARSIZE(state_jsonb));
	else
		escape_json(buf, state_text);
	appendStringInfoString(buf, ", \"model\": ");
	escape_json(buf, model);
	appendStringInfoString(buf, ", \"questions\": ");
}

static void
append_choice_criteria(StringInfo buf, Jsonb *options)
{
	JsonbIterator *it;
	JsonbIteratorToken r;
	JsonbValue	v;
	int			count = 0;

	if (!JB_ROOT_IS_OBJECT(options))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("options must be a JSON object")));

	it = JsonbIteratorInit(&options->root);
	r = JsonbIteratorNext(&it, &v, true);
	if (r != WJB_BEGIN_OBJECT)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("options must be a JSON object")));

	appendStringInfoChar(buf, '{');

	while ((r = JsonbIteratorNext(&it, &v, true)) != WJB_END_OBJECT)
	{
		JsonbValue	val;
		char	   *key;

		if (r != WJB_KEY)
			elog(ERROR, "unexpected jsonb token: %d", (int) r);

		key = pnstrdup(v.val.string.val, v.val.string.len);

		r = JsonbIteratorNext(&it, &val, true);
		if (r != WJB_VALUE)
			elog(ERROR, "unexpected jsonb token: %d", (int) r);

		if (val.type != jbvString && val.type != jbvNull)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("options values must be strings or null")));

		if (count > 0)
			appendStringInfoString(buf, ", ");

		escape_json(buf, key);
		appendStringInfoString(buf, ": ");

		if (val.type == jbvNull)
			appendStringInfoString(buf, "null");
		else
		{
			char	   *s = pnstrdup(val.val.string.val, val.val.string.len);

			escape_json(buf, s);
		}

		count++;
	}

	if (count == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("options must be a non-empty JSON object")));

	appendStringInfoChar(buf, '}');
}

static void
append_score_levels(StringInfo buf, ArrayType *levels)
{
	Datum	   *datums;
	bool	   *nulls;
	int			n;
	int			i;
	int			nvalid = 0;

	if (ARR_NDIM(levels) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("levels must be a one-dimensional array with at least two non-null elements")));

	deconstruct_array_builtin(levels, TEXTOID, &datums, &nulls, &n);

	for (i = 0; i < n; i++)
	{
		if (nulls && nulls[i])
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("levels must not contain nulls")));
		nvalid++;
	}

	if (nvalid < 2)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("levels must be a one-dimensional array with at least two non-null elements")));

	appendStringInfoChar(buf, '[');
	for (i = 0; i < n; i++)
	{
		char	   *s = TextDatumGetCString(datums[i]);

		if (i > 0)
			appendStringInfoString(buf, ", ");
		escape_json(buf, s);
	}
	appendStringInfoChar(buf, ']');
}

static char *
build_classify_request(FunctionCallInfo fcinfo)
{
	char	   *state;
	char	   *instructions;
	Jsonb	   *options;
	char	   *model;
	StringInfoData buf;

	state = get_text_arg(fcinfo, 0, "state");
	instructions = get_text_arg(fcinfo, 1, "instructions");
	options = get_jsonb_arg(fcinfo, 2, "options");
	model = resolve_model(fcinfo, 3);

	initStringInfo(&buf);
	append_request_prelude(&buf, state, NULL, model);
	appendStringInfoString(&buf,
						   "{\"label\": {\"type\": \"choice\", \"instructions\": ");
	escape_json(&buf, instructions);
	appendStringInfoString(&buf, ", \"criteria\": ");
	append_choice_criteria(&buf, options);
	appendStringInfoString(&buf, "}}}");

	return buf.data;
}

static Jsonb *
parse_response_json(const char *body)
{
	Jsonb	   *jb;
	JsonbValue	vbuf;
	JsonbValue *v;

	jb = DatumGetJsonbP(DirectFunctionCall1(jsonb_in, CStringGetDatum(body)));

	if (!JB_ROOT_IS_OBJECT(jb))
		json_shape_error("top level must be a JSON object");

	v = getKeyJsonValueFromContainer(&jb->root, "model", 5, &vbuf);
	if (v == NULL || v->type != jbvString)
		json_shape_error("missing string field \"model\"");

	v = getKeyJsonValueFromContainer(&jb->root, "answers", 7, &vbuf);
	if (jsonb_value_object_container(v) == NULL)
		json_shape_error("missing object field \"answers\"");

	return jb;
}

static bool
jsonb_str_eq(const JsonbValue *v, const char *s)
{
	int			len;

	if (v == NULL || v->type != jbvString)
		return false;

	len = strlen(s);
	return v->val.string.len == len &&
		memcmp(v->val.string.val, s, len) == 0;
}

static char *
jsonb_str_cstring(const JsonbValue *v)
{
	return pnstrdup(v->val.string.val, v->val.string.len);
}

static float8
jsonb_numeric_float8(const JsonbValue *v)
{
	char	   *s;
	Datum		d;

	s = DatumGetCString(DirectFunctionCall1(numeric_out,
											NumericGetDatum(v->val.numeric)));
	d = DirectFunctionCall1(float8in, CStringGetDatum(s));
	return DatumGetFloat8(d);
}

static int32
jsonb_numeric_int4(const JsonbValue *v)
{
	char	   *s;
	Datum		d;

	s = DatumGetCString(DirectFunctionCall1(numeric_out,
											NumericGetDatum(v->val.numeric)));
	d = DirectFunctionCall1(int4in, CStringGetDatum(s));
	return DatumGetInt32(d);
}

static JsonbContainer *
jsonb_value_object_container(JsonbValue *v)
{
	if (v == NULL)
		return NULL;

	if (v->type == jbvBinary)
	{
		if (JsonContainerIsObject(v->val.binary.data))
			return v->val.binary.data;
		return NULL;
	}

	if (v->type == jbvObject)
	{
		Jsonb	   *jb = JsonbValueToJsonb(v);

		return &jb->root;
	}

	return NULL;
}

static Jsonb *
jsonb_value_object_to_jsonb(JsonbValue *v)
{
	Jsonb	   *jb;

	if (v->type == jbvBinary)
		jb = JsonbValueToJsonb(v);
	else
	{
		Jsonb	   *tmp;
		char	   *cstr;

		tmp = JsonbValueToJsonb(v);
		cstr = JsonbToCString(NULL, &tmp->root, VARSIZE(tmp));
		jb = DatumGetJsonbP(DirectFunctionCall1(jsonb_in,
												CStringGetDatum(cstr)));
	}

	if (!JB_ROOT_IS_OBJECT(jb))
		json_shape_error("expected a JSON object");

	return jb;
}

static char *
response_model(Jsonb *resp)
{
	JsonbValue	vbuf;
	JsonbValue *v;

	v = getKeyJsonValueFromContainer(&resp->root, "model", 5, &vbuf);
	if (v == NULL || v->type != jbvString)
		json_shape_error("missing string field \"model\"");

	return jsonb_str_cstring(v);
}

static void
extract_usage(Jsonb *resp, bool *in_isnull, int32 *in_tok,
			  bool *out_isnull, int32 *out_tok)
{
	JsonbValue	vbuf;
	JsonbValue *usage;
	JsonbContainer *cont;
	JsonbValue	ibuf;
	JsonbValue *iv;

	*in_isnull = true;
	*out_isnull = true;
	*in_tok = 0;
	*out_tok = 0;

	usage = getKeyJsonValueFromContainer(&resp->root, "usage", 5, &vbuf);
	cont = jsonb_value_object_container(usage);
	if (cont == NULL)
		return;

	iv = getKeyJsonValueFromContainer(cont, "input_tokens", 12, &ibuf);
	if (iv != NULL && iv->type == jbvNumeric)
	{
		*in_tok = jsonb_numeric_int4(iv);
		*in_isnull = false;
	}

	iv = getKeyJsonValueFromContainer(cont, "output_tokens", 13, &ibuf);
	if (iv != NULL && iv->type == jbvNumeric)
	{
		*out_tok = jsonb_numeric_int4(iv);
		*out_isnull = false;
	}
}

static JsonbContainer *
get_answer_object(Jsonb *resp, const char *qid, const char *expected_type)
{
	JsonbValue	abuf;
	JsonbValue *answers;
	JsonbContainer *acont;
	JsonbValue	qbuf;
	JsonbValue *qval;
	JsonbContainer *qcont;
	JsonbValue	tbuf;
	JsonbValue *typev;

	answers = getKeyJsonValueFromContainer(&resp->root, "answers", 7, &abuf);
	acont = jsonb_value_object_container(answers);
	if (acont == NULL)
		json_shape_error("missing object field \"answers\"");

	qval = getKeyJsonValueFromContainer(acont, qid, strlen(qid), &qbuf);
	qcont = jsonb_value_object_container(qval);
	if (qcont == NULL)
		json_shape_error("missing answer object");

	typev = getKeyJsonValueFromContainer(qcont, "type", 4, &tbuf);
	if (!jsonb_str_eq(typev, expected_type))
		json_shape_error("unexpected answer type");

	return qcont;
}

static Datum
form_composite(FunctionCallInfo fcinfo, Datum *values, bool *nulls)
{
	TupleDesc	tupdesc;
	HeapTuple	tuple;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("function returning record called in context that cannot accept type record")));

	tupdesc = BlessTupleDesc(tupdesc);
	tuple = heap_form_tuple(tupdesc, values, nulls);
	return HeapTupleGetDatum(tuple);
}

static Datum
form_choice_result(FunctionCallInfo fcinfo, Jsonb *resp)
{
	Datum		values[6];
	bool		nulls[6];
	JsonbContainer *ans;
	JsonbValue	vbuf;
	JsonbValue *v;
	char	   *choice;
	float8		confidence;
	Jsonb	   *probs;
	char	   *model;
	int32		in_tok;
	int32		out_tok;
	bool		in_null;
	bool		out_null;

	memset(nulls, 0, sizeof(nulls));
	memset(values, 0, sizeof(values));

	ans = get_answer_object(resp, "label", "choice");

	v = getKeyJsonValueFromContainer(ans, "choice", 6, &vbuf);
	if (v == NULL || v->type != jbvString)
		json_shape_error("missing string field \"choice\"");
	choice = jsonb_str_cstring(v);

	v = getKeyJsonValueFromContainer(ans, "confidence", 10, &vbuf);
	if (v == NULL || v->type != jbvNumeric)
		json_shape_error("missing numeric field \"confidence\"");
	confidence = jsonb_numeric_float8(v);

	v = getKeyJsonValueFromContainer(ans, "probabilities", 13, &vbuf);
	if (v == NULL)
		json_shape_error("missing object field \"probabilities\"");
	probs = jsonb_value_object_to_jsonb(v);

	model = response_model(resp);
	extract_usage(resp, &in_null, &in_tok, &out_null, &out_tok);

	values[0] = CStringGetTextDatum(choice);
	values[1] = Float8GetDatum(confidence);
	values[2] = JsonbPGetDatum(probs);
	values[3] = CStringGetTextDatum(model);
	if (in_null)
		nulls[4] = true;
	else
		values[4] = Int32GetDatum(in_tok);
	if (out_null)
		nulls[5] = true;
	else
		values[5] = Int32GetDatum(out_tok);

	return form_composite(fcinfo, values, nulls);
}

static Datum
form_noul_result(FunctionCallInfo fcinfo, Jsonb *resp)
{
	Datum		values[4];
	bool		nulls[4];
	JsonbContainer *ans;
	JsonbValue	vbuf;
	JsonbValue *v;
	float8		noul;
	char	   *model;
	int32		in_tok;
	int32		out_tok;
	bool		in_null;
	bool		out_null;

	memset(nulls, 0, sizeof(nulls));
	memset(values, 0, sizeof(values));

	ans = get_answer_object(resp, "flag", "noul");

	v = getKeyJsonValueFromContainer(ans, "noul", 4, &vbuf);
	if (v == NULL || v->type != jbvNumeric)
		json_shape_error("missing numeric field \"noul\"");
	noul = jsonb_numeric_float8(v);

	model = response_model(resp);
	extract_usage(resp, &in_null, &in_tok, &out_null, &out_tok);

	values[0] = Float8GetDatum(noul);
	values[1] = CStringGetTextDatum(model);
	if (in_null)
		nulls[2] = true;
	else
		values[2] = Int32GetDatum(in_tok);
	if (out_null)
		nulls[3] = true;
	else
		values[3] = Int32GetDatum(out_tok);

	return form_composite(fcinfo, values, nulls);
}

static Datum
form_score_result(FunctionCallInfo fcinfo, Jsonb *resp)
{
	Datum		values[7];
	bool		nulls[7];
	JsonbContainer *ans;
	JsonbValue	vbuf;
	JsonbValue *v;
	float8		score;
	float8		confidence;
	Jsonb	   *legend;
	Jsonb	   *probs;
	char	   *model;
	int32		in_tok;
	int32		out_tok;
	bool		in_null;
	bool		out_null;

	memset(nulls, 0, sizeof(nulls));
	memset(values, 0, sizeof(values));

	ans = get_answer_object(resp, "rating", "score");

	v = getKeyJsonValueFromContainer(ans, "score", 5, &vbuf);
	if (v == NULL || v->type != jbvNumeric)
		json_shape_error("missing numeric field \"score\"");
	score = jsonb_numeric_float8(v);

	v = getKeyJsonValueFromContainer(ans, "confidence", 10, &vbuf);
	if (v == NULL || v->type != jbvNumeric)
		json_shape_error("missing numeric field \"confidence\"");
	confidence = jsonb_numeric_float8(v);

	v = getKeyJsonValueFromContainer(ans, "legend", 6, &vbuf);
	if (v == NULL)
		json_shape_error("missing object field \"legend\"");
	legend = jsonb_value_object_to_jsonb(v);

	v = getKeyJsonValueFromContainer(ans, "probabilities", 13, &vbuf);
	if (v == NULL)
		json_shape_error("missing object field \"probabilities\"");
	probs = jsonb_value_object_to_jsonb(v);

	model = response_model(resp);
	extract_usage(resp, &in_null, &in_tok, &out_null, &out_tok);

	values[0] = Float8GetDatum(score);
	values[1] = Float8GetDatum(confidence);
	values[2] = JsonbPGetDatum(legend);
	values[3] = JsonbPGetDatum(probs);
	values[4] = CStringGetTextDatum(model);
	if (in_null)
		nulls[5] = true;
	else
		values[5] = Int32GetDatum(in_tok);
	if (out_null)
		nulls[6] = true;
	else
		values[6] = Int32GetDatum(out_tok);

	return form_composite(fcinfo, values, nulls);
}

static char *
choice_from_response(Jsonb *resp)
{
	JsonbContainer *ans;
	JsonbValue	vbuf;
	JsonbValue *v;
	char	   *choice;

	ans = get_answer_object(resp, "label", "choice");

	v = getKeyJsonValueFromContainer(ans, "choice", 6, &vbuf);
	if (v == NULL || v->type != jbvString)
		json_shape_error("missing string field \"choice\"");
	choice = jsonb_str_cstring(v);

	v = getKeyJsonValueFromContainer(ans, "confidence", 10, &vbuf);
	if (v == NULL || v->type != jbvNumeric)
		json_shape_error("missing numeric field \"confidence\"");

	v = getKeyJsonValueFromContainer(ans, "probabilities", 13, &vbuf);
	if (v == NULL)
		json_shape_error("missing object field \"probabilities\"");
	(void) jsonb_value_object_to_jsonb(v);

	return choice;
}

Datum
typesafe_classify(PG_FUNCTION_ARGS)
{
	char	   *req;
	char	   *body;
	Jsonb	   *resp;

	req = build_classify_request(fcinfo);
	body = execute_request(req);
	resp = parse_response_json(body);

	PG_RETURN_DATUM(form_choice_result(fcinfo, resp));
}

Datum
typesafe_detect(PG_FUNCTION_ARGS)
{
	char	   *state;
	char	   *instructions;
	char	   *true_meaning = NULL;
	char	   *false_meaning = NULL;
	char	   *model;
	StringInfoData buf;
	char	   *body;
	Jsonb	   *resp;

	state = get_text_arg(fcinfo, 0, "state");
	instructions = get_text_arg(fcinfo, 1, "instructions");
	if (!PG_ARGISNULL(2))
		true_meaning = text_to_cstring(PG_GETARG_TEXT_PP(2));
	if (!PG_ARGISNULL(3))
		false_meaning = text_to_cstring(PG_GETARG_TEXT_PP(3));
	model = resolve_model(fcinfo, 4);

	initStringInfo(&buf);
	append_request_prelude(&buf, state, NULL, model);
	appendStringInfoString(&buf,
						   "{\"flag\": {\"type\": \"noul\", \"instructions\": ");
	escape_json(&buf, instructions);
	if (true_meaning != NULL || false_meaning != NULL)
	{
		bool		first = true;

		appendStringInfoString(&buf, ", \"criteria\": {");
		if (true_meaning != NULL)
		{
			appendStringInfoString(&buf, "\"true\": ");
			escape_json(&buf, true_meaning);
			first = false;
		}
		if (false_meaning != NULL)
		{
			if (!first)
				appendStringInfoString(&buf, ", ");
			appendStringInfoString(&buf, "\"false\": ");
			escape_json(&buf, false_meaning);
		}
		appendStringInfoChar(&buf, '}');
	}
	appendStringInfoString(&buf, "}}}");

	body = execute_request(buf.data);
	resp = parse_response_json(body);

	PG_RETURN_DATUM(form_noul_result(fcinfo, resp));
}

Datum
typesafe_score(PG_FUNCTION_ARGS)
{
	char	   *state;
	char	   *instructions;
	ArrayType  *levels;
	char	   *model;
	StringInfoData buf;
	char	   *body;
	Jsonb	   *resp;

	state = get_text_arg(fcinfo, 0, "state");
	instructions = get_text_arg(fcinfo, 1, "instructions");
	if (PG_ARGISNULL(2))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("levels must not be null")));
	levels = PG_GETARG_ARRAYTYPE_P(2);
	model = resolve_model(fcinfo, 3);

	initStringInfo(&buf);
	append_request_prelude(&buf, state, NULL, model);
	appendStringInfoString(&buf,
						   "{\"rating\": {\"type\": \"score\", \"instructions\": ");
	escape_json(&buf, instructions);
	appendStringInfoString(&buf, ", \"criteria\": ");
	append_score_levels(&buf, levels);
	appendStringInfoString(&buf, "}}}");

	body = execute_request(buf.data);
	resp = parse_response_json(body);

	PG_RETURN_DATUM(form_score_result(fcinfo, resp));
}

Datum
typesafe_ask(PG_FUNCTION_ARGS)
{
	Jsonb	   *state;
	Jsonb	   *questions;
	char	   *model;
	StringInfoData buf;
	char	   *body;
	Jsonb	   *resp;

	state = get_jsonb_arg(fcinfo, 0, "state");
	questions = get_jsonb_arg(fcinfo, 1, "questions");
	model = resolve_model(fcinfo, 2);

	if (!JB_ROOT_IS_OBJECT(questions))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("questions must be a JSON object")));

	initStringInfo(&buf);
	append_request_prelude(&buf, NULL, state, model);
	JsonbToCString(&buf, &questions->root, VARSIZE(questions));
	appendStringInfoChar(&buf, '}');

	body = execute_request(buf.data);
	resp = parse_response_json(body);

	PG_RETURN_JSONB_P(resp);
}

Datum
typesafe_ask_text(PG_FUNCTION_ARGS)
{
	char	   *state;
	Jsonb	   *questions;
	char	   *model;
	StringInfoData buf;
	char	   *body;
	Jsonb	   *resp;

	state = get_text_arg(fcinfo, 0, "state");
	questions = get_jsonb_arg(fcinfo, 1, "questions");
	model = resolve_model(fcinfo, 2);

	if (!JB_ROOT_IS_OBJECT(questions))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("questions must be a JSON object")));

	initStringInfo(&buf);
	append_request_prelude(&buf, state, NULL, model);
	JsonbToCString(&buf, &questions->root, VARSIZE(questions));
	appendStringInfoChar(&buf, '}');

	body = execute_request(buf.data);
	resp = parse_response_json(body);

	PG_RETURN_JSONB_P(resp);
}

Datum
typesafe_label(PG_FUNCTION_ARGS)
{
	char	   *req;
	char	   *body;
	Jsonb	   *resp;
	char	   *choice;

	req = build_classify_request(fcinfo);
	body = execute_request(req);
	resp = parse_response_json(body);
	choice = choice_from_response(resp);

	PG_RETURN_TEXT_P(cstring_to_text(choice));
}

Datum
typesafe_last_request(PG_FUNCTION_ARGS)
{
	const char *s = (last_request_json != NULL) ? last_request_json : "";

	PG_RETURN_TEXT_P(cstring_to_text(s));
}

static void
append_noul_criteria(StringInfo buf, const char *true_meaning,
					 const char *false_meaning)
{
	bool		first = true;

	if (true_meaning == NULL && false_meaning == NULL)
		return;

	appendStringInfoString(buf, ", \"criteria\": {");
	if (true_meaning != NULL)
	{
		appendStringInfoString(buf, "\"true\": ");
		escape_json(buf, true_meaning);
		first = false;
	}
	if (false_meaning != NULL)
	{
		if (!first)
			appendStringInfoString(buf, ", ");
		appendStringInfoString(buf, "\"false\": ");
		escape_json(buf, false_meaning);
	}
	appendStringInfoChar(buf, '}');
}

/*
 * POST several request bodies.  Mocked requests go through execute_request;
 * live ones share http_post_live's interrupt-safe multi loop.
 */
static char **
http_post_many(char **requests, int nrequests)
{
	char	  **results;
	int			i;

	if (nrequests <= 0)
		return (char **) palloc0(sizeof(char *) * 1);

	if (typesafe_mock_response != NULL && typesafe_mock_response[0] != '\0')
	{
		results = (char **) palloc0(sizeof(char *) * nrequests);
		for (i = 0; i < nrequests; i++)
			results[i] = execute_request(requests[i]);
		return results;
	}

	save_last_request(requests[nrequests - 1]);

	return http_post_live(requests, nrequests);
}

static char *
build_detect_chunk(char **texts, int n, const char *instructions,
				   const char *true_meaning, const char *false_meaning,
				   const char *model)
{
	StringInfoData buf;
	int			i;

	initStringInfo(&buf);
	appendStringInfoString(&buf, "{\"state\": {");
	for (i = 0; i < n; i++)
	{
		if (i > 0)
			appendStringInfoString(&buf, ", ");
		appendStringInfo(&buf, "\"s%d\": ", i);
		escape_json(&buf, texts[i]);
	}
	appendStringInfoString(&buf, "}, \"model\": ");
	escape_json(&buf, model);
	appendStringInfoString(&buf, ", \"questions\": {");
	for (i = 0; i < n; i++)
	{
		if (i > 0)
			appendStringInfoString(&buf, ", ");
		appendStringInfo(&buf, "\"s%d\": {\"type\": \"noul\", \"instructions\": {\"question\": ", i);
		escape_json(&buf, instructions);
		appendStringInfo(&buf, ", \"evaluate\": \"s%d\"}", i);
		append_noul_criteria(&buf, true_meaning, false_meaning);
		appendStringInfoChar(&buf, '}');
	}
	appendStringInfoString(&buf, "}}");
	return buf.data;
}

static char *
build_classify_chunk(char **texts, int n, const char *instructions,
					 Jsonb *options, const char *model)
{
	StringInfoData buf;
	int			i;

	initStringInfo(&buf);
	appendStringInfoString(&buf, "{\"state\": {");
	for (i = 0; i < n; i++)
	{
		if (i > 0)
			appendStringInfoString(&buf, ", ");
		appendStringInfo(&buf, "\"s%d\": ", i);
		escape_json(&buf, texts[i]);
	}
	appendStringInfoString(&buf, "}, \"model\": ");
	escape_json(&buf, model);
	appendStringInfoString(&buf, ", \"questions\": {");
	for (i = 0; i < n; i++)
	{
		if (i > 0)
			appendStringInfoString(&buf, ", ");
		appendStringInfo(&buf, "\"s%d\": {\"type\": \"choice\", \"instructions\": {\"question\": ", i);
		escape_json(&buf, instructions);
		appendStringInfo(&buf, ", \"evaluate\": \"s%d\"}, \"criteria\": ", i);
		append_choice_criteria(&buf, options);
		appendStringInfoChar(&buf, '}');
	}
	appendStringInfoString(&buf, "}}");
	return buf.data;
}

static float8
noul_from_qid(Jsonb *resp, const char *qid)
{
	JsonbContainer *ans;
	JsonbValue	vbuf;
	JsonbValue *v;

	ans = get_answer_object(resp, qid, "noul");
	v = getKeyJsonValueFromContainer(ans, "noul", 4, &vbuf);
	if (v == NULL || v->type != jbvNumeric)
		json_shape_error("missing numeric field \"noul\"");
	return jsonb_numeric_float8(v);
}

static void
choice_from_qid(Jsonb *resp, const char *qid, char **choice, float8 *confidence,
				Jsonb **probs)
{
	JsonbContainer *ans;
	JsonbValue	vbuf;
	JsonbValue *v;

	ans = get_answer_object(resp, qid, "choice");
	v = getKeyJsonValueFromContainer(ans, "choice", 6, &vbuf);
	if (v == NULL || v->type != jbvString)
		json_shape_error("missing string field \"choice\"");
	*choice = jsonb_str_cstring(v);

	v = getKeyJsonValueFromContainer(ans, "confidence", 10, &vbuf);
	if (v == NULL || v->type != jbvNumeric)
		json_shape_error("missing numeric field \"confidence\"");
	*confidence = jsonb_numeric_float8(v);

	v = getKeyJsonValueFromContainer(ans, "probabilities", 13, &vbuf);
	if (v == NULL)
		json_shape_error("missing object field \"probabilities\"");
	*probs = jsonb_value_object_to_jsonb(v);
}

static void
deconstruct_text_array(FunctionCallInfo fcinfo, int argno,
					   Datum **datums, bool **nulls, int *n)
{
	ArrayType  *arr;

	if (PG_ARGISNULL(argno))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("states must not be null")));

	arr = PG_GETARG_ARRAYTYPE_P(argno);
	if (ARR_NDIM(arr) == 0)
	{
		*datums = NULL;
		*nulls = NULL;
		*n = 0;
		return;
	}
	if (ARR_NDIM(arr) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("states must be a one-dimensional array")));

	deconstruct_array_builtin(arr, TEXTOID, datums, nulls, n);
}

Datum
typesafe_detect_many(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo;
	Datum	   *datums;
	bool	   *anulls;
	int			n;
	int			i;
	char	   *instructions;
	char	   *true_meaning = NULL;
	char	   *false_meaning = NULL;
	char	   *model;
	char	  **texts;
	int			nwork = 0;
	int			nchunks;
	char	  **chunk_reqs;
	char	  **chunk_bodies;
	int		   *chunk_len;
	int			c;
	int			batch;

	deconstruct_text_array(fcinfo, 0, &datums, &anulls, &n);
	instructions = get_text_arg(fcinfo, 1, "instructions");
	if (!PG_ARGISNULL(2))
		true_meaning = text_to_cstring(PG_GETARG_TEXT_PP(2));
	if (!PG_ARGISNULL(3))
		false_meaning = text_to_cstring(PG_GETARG_TEXT_PP(3));
	model = resolve_model(fcinfo, 4);

	InitMaterializedSRF(fcinfo, 0);
	rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	if (n == 0)
		return (Datum) 0;

	texts = (char **) palloc(sizeof(char *) * n);
	for (i = 0; i < n; i++)
	{
		if (anulls && anulls[i])
			continue;
		texts[nwork] = TextDatumGetCString(datums[i]);
		nwork++;
	}

	batch = typesafe_batch_size;
	nchunks = (nwork + batch - 1) / batch;
	if (nwork == 0)
		nchunks = 0;

	chunk_reqs = (char **) palloc(sizeof(char *) * Max(nchunks, 1));
	chunk_len = (int *) palloc(sizeof(int) * Max(nchunks, 1));

	for (c = 0; c < nchunks; c++)
	{
		int			off = c * batch;
		int			len = Min(batch, nwork - off);

		chunk_len[c] = len;
		chunk_reqs[c] = build_detect_chunk(texts + off, len, instructions,
										   true_meaning, false_meaning, model);
	}

	chunk_bodies = nchunks > 0 ? http_post_many(chunk_reqs, nchunks) : NULL;

	{
		float8	   *nouls = (float8 *) palloc(sizeof(float8) * nwork);
		char	  **models = (char **) palloc(sizeof(char *) * nwork);
		int32	   *in_toks = (int32 *) palloc(sizeof(int32) * nwork);
		int32	   *out_toks = (int32 *) palloc(sizeof(int32) * nwork);
		bool	   *in_nulls = (bool *) palloc(sizeof(bool) * nwork);
		bool	   *out_nulls = (bool *) palloc(sizeof(bool) * nwork);
		int			work_i = 0;

		for (c = 0; c < nchunks; c++)
		{
			Jsonb	   *resp = parse_response_json(chunk_bodies[c]);
			char	   *rmodel = response_model(resp);
			int32		in_tok;
			int32		out_tok;
			bool		in_null;
			bool		out_null;
			int			j;

			extract_usage(resp, &in_null, &in_tok, &out_null, &out_tok);
			for (j = 0; j < chunk_len[c]; j++)
			{
				char		qid[32];

				snprintf(qid, sizeof(qid), "s%d", j);
				nouls[work_i] = noul_from_qid(resp, qid);
				models[work_i] = rmodel;

				/*
				 * Usage is per HTTP request, not per item; report it on
				 * the chunk's first row only so SUM() over the result is
				 * accurate.
				 */
				in_toks[work_i] = in_tok;
				out_toks[work_i] = out_tok;
				in_nulls[work_i] = in_null || (j > 0);
				out_nulls[work_i] = out_null || (j > 0);
				work_i++;
			}
		}

		work_i = 0;
		for (i = 0; i < n; i++)
		{
			Datum		values[6];
			bool		nulls[6];

			memset(nulls, 0, sizeof(nulls));
			values[0] = Int32GetDatum(i + 1);

			if (anulls && anulls[i])
			{
				nulls[1] = true;
				nulls[2] = true;
				nulls[3] = true;
				nulls[4] = true;
				nulls[5] = true;
			}
			else
			{
				values[1] = CStringGetTextDatum(texts[work_i]);
				values[2] = Float8GetDatum(nouls[work_i]);
				values[3] = CStringGetTextDatum(models[work_i]);
				if (in_nulls[work_i])
					nulls[4] = true;
				else
					values[4] = Int32GetDatum(in_toks[work_i]);
				if (out_nulls[work_i])
					nulls[5] = true;
				else
					values[5] = Int32GetDatum(out_toks[work_i]);
				work_i++;
			}

			tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc,
								 values, nulls);
		}
	}

	return (Datum) 0;
}

Datum
typesafe_classify_many(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo;
	Datum	   *datums;
	bool	   *anulls;
	int			n;
	int			i;
	char	   *instructions;
	Jsonb	   *options;
	char	   *model;
	char	  **texts;
	int			nwork = 0;
	int			nchunks;
	char	  **chunk_reqs;
	char	  **chunk_bodies;
	int		   *chunk_len;
	int			c;
	int			batch;

	deconstruct_text_array(fcinfo, 0, &datums, &anulls, &n);
	instructions = get_text_arg(fcinfo, 1, "instructions");
	options = get_jsonb_arg(fcinfo, 2, "options");
	model = resolve_model(fcinfo, 3);

	InitMaterializedSRF(fcinfo, 0);
	rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	if (n == 0)
		return (Datum) 0;

	texts = (char **) palloc(sizeof(char *) * n);
	for (i = 0; i < n; i++)
	{
		if (anulls && anulls[i])
			continue;
		texts[nwork++] = TextDatumGetCString(datums[i]);
	}

	batch = typesafe_batch_size;
	nchunks = (nwork + batch - 1) / batch;
	if (nwork == 0)
		nchunks = 0;

	chunk_reqs = (char **) palloc(sizeof(char *) * Max(nchunks, 1));
	chunk_len = (int *) palloc(sizeof(int) * Max(nchunks, 1));

	for (c = 0; c < nchunks; c++)
	{
		int			off = c * batch;
		int			len = Min(batch, nwork - off);

		chunk_len[c] = len;
		chunk_reqs[c] = build_classify_chunk(texts + off, len, instructions,
											 options, model);
	}

	chunk_bodies = nchunks > 0 ? http_post_many(chunk_reqs, nchunks) : NULL;

	{
		char	  **choices = (char **) palloc(sizeof(char *) * nwork);
		float8	   *confs = (float8 *) palloc(sizeof(float8) * nwork);
		Jsonb	  **probs = (Jsonb **) palloc(sizeof(Jsonb *) * nwork);
		char	  **models = (char **) palloc(sizeof(char *) * nwork);
		int32	   *in_toks = (int32 *) palloc(sizeof(int32) * nwork);
		int32	   *out_toks = (int32 *) palloc(sizeof(int32) * nwork);
		bool	   *in_nulls = (bool *) palloc(sizeof(bool) * nwork);
		bool	   *out_nulls = (bool *) palloc(sizeof(bool) * nwork);
		int			work_i = 0;

		for (c = 0; c < nchunks; c++)
		{
			Jsonb	   *resp = parse_response_json(chunk_bodies[c]);
			char	   *rmodel = response_model(resp);
			int32		in_tok;
			int32		out_tok;
			bool		in_null;
			bool		out_null;
			int			j;

			extract_usage(resp, &in_null, &in_tok, &out_null, &out_tok);
			for (j = 0; j < chunk_len[c]; j++)
			{
				char		qid[32];

				snprintf(qid, sizeof(qid), "s%d", j);
				choice_from_qid(resp, qid, &choices[work_i], &confs[work_i],
								&probs[work_i]);
				models[work_i] = rmodel;

				/*
				 * Usage is per HTTP request, not per item; report it on
				 * the chunk's first row only so SUM() over the result is
				 * accurate.
				 */
				in_toks[work_i] = in_tok;
				out_toks[work_i] = out_tok;
				in_nulls[work_i] = in_null || (j > 0);
				out_nulls[work_i] = out_null || (j > 0);
				work_i++;
			}
		}

		work_i = 0;
		for (i = 0; i < n; i++)
		{
			Datum		values[8];
			bool		nulls[8];

			memset(nulls, 0, sizeof(nulls));
			values[0] = Int32GetDatum(i + 1);

			if (anulls && anulls[i])
			{
				nulls[1] = true;
				nulls[2] = true;
				nulls[3] = true;
				nulls[4] = true;
				nulls[5] = true;
				nulls[6] = true;
				nulls[7] = true;
			}
			else
			{
				values[1] = CStringGetTextDatum(texts[work_i]);
				values[2] = CStringGetTextDatum(choices[work_i]);
				values[3] = Float8GetDatum(confs[work_i]);
				values[4] = JsonbPGetDatum(probs[work_i]);
				values[5] = CStringGetTextDatum(models[work_i]);
				if (in_nulls[work_i])
					nulls[6] = true;
				else
					values[6] = Int32GetDatum(in_toks[work_i]);
				if (out_nulls[work_i])
					nulls[7] = true;
				else
					values[7] = Int32GetDatum(out_toks[work_i]);
				work_i++;
			}

			tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc,
								 values, nulls);
		}
	}

	return (Datum) 0;
}
