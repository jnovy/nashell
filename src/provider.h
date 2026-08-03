#ifndef PROVIDER_H
#define PROVIDER_H

#include "llm.h"
#include "cJSON.h"
#include <stdatomic.h>

/* ── Provider types ─────────────────────────────────────────────── */

typedef enum {
  PROVIDER_LOCAL,     /* llama.cpp / OpenAI-compatible local server */
  PROVIDER_OPENAI,    /* OpenAI API (GPT-4o, GPT-5, etc.) */
  PROVIDER_ANTHROPIC, /* Anthropic API (direct) */
  PROVIDER_VERTEX,    /* Anthropic via Google Vertex AI */
} provider_type_t;

/* ── Provider configuration ─────────────────────────────────────── */

typedef struct {
  provider_type_t type;
  /* NOTE: These are const char* because this struct is used both as a non-owning
     * parameter bag (on stack) and as owned storage (inside provider_t, where fields
     * are strdup'd).  provider_free() casts away const to free the owned copies. */
  const char *model_id;    /* model identifier for API calls */
  const char *api_base;    /* base URL (local server or API endpoint) */
  const char *api_key_env; /* env var name for API key (e.g. "OPENAI_API_KEY") */
  const char *project_id;  /* Vertex AI project ID */
  const char *region;      /* Vertex AI region (e.g. "us-east5") */
  int context_size;        /* context window size */
  float chars_per_token;   /* chars per token ratio (default 3.5) */
  int caching;             /* enable prompt caching (Anthropic) */
  int max_tokens;          /* max completion tokens */
  float temperature;       /* sampling temperature */
  float top_p;             /* nucleus sampling threshold (0.0-1.0, 1.0=disabled) */
  int top_k;               /* top-k sampling (0=disabled) */
  int enable_thinking;     /* 0=off, 1=on */
  int thinking_budget;     /* -1=unrestricted, 0=none, N>0=max */
  int llm_timeout;         /* per-call wall-clock timeout in seconds (0=no limit) */
  int max_retries;         /* max retries on transient errors (0=use default 10) */
  int retry_base_sec;      /* initial backoff seconds (0=use default 10) */
} provider_config_t;

/* ── Provider vtable ────────────────────────────────────────────── */

/* Forward declaration */
typedef struct provider provider_t;

/* Token callback for streaming */
typedef void (*provider_token_fn)(const char *token, void *userdata);

/* Prompt processing progress callback (llama.cpp return_progress) */
typedef void (*provider_progress_fn)(int processed, int total, void *userdata);

/* Provider function pointers — mirrors nashell's Provider base class */
struct provider {
  provider_type_t type;
  provider_config_t cfg;

  /* Build HTTP headers (adds auth). Returns curl_slist* (caller frees).
     * For local: just Content-Type. For OpenAI: + Authorization Bearer.
     * For Vertex: + Authorization Bearer <gcloud token>. */
  struct curl_slist *(*build_headers)(provider_t *p);

  /* Build request JSON body for chat completion.
     * Handles provider-specific fields and message format conversion.
     * Returns malloc'd JSON string (caller frees). */
  char *(*build_request)(provider_t *p, llm_chat_t *chat, int stream);

  /* Parse a non-streaming response JSON into unified format.
     * Returns malloc'd JSON string: {"thought":"...", "action":"name", ...}
     * Sets chat->last_tool_call_id and chat->last_tool_calls_json.
     * Returns NULL on parse failure. */
  char *(*parse_response)(provider_t *p, const char *response_json,
                          llm_chat_t *chat, llm_stats_t *stats);

  /* Parse one SSE data line during streaming.
     * Extracts content deltas and tool_call chunks.
     * Called by the shared SSE line processor. */
  void (*parse_sse_event)(provider_t *p, cJSON *data, void *sse_state);

  /* Get the API endpoint URL for chat completions.
     * Returns static string (do NOT free). */
  const char *(*get_endpoint)(provider_t *p);

  /* Build the tools array for the request.
     * Returns cJSON array (caller manages via request object). */
  cJSON *(*build_tools)(provider_t *p);

  /* Fetch server/model info (optional — local only).
     * Returns 0 on success, -1 on failure. */
  int (*fetch_model_info)(provider_t *p, int *context_size, char **model_name,
                          char **props_json);

  /* Provider-specific cleanup */
  void (*destroy)(provider_t *p);

  /* ── Provider-specific state ── */
  char *_cached_endpoint;    /* cached endpoint URL string (streaming) */
  char *_cached_endpoint_ns; /* cached endpoint URL string (non-streaming) */
  char *_cached_auth_token;  /* cached OAuth2 token (Vertex) */
  long _auth_token_expiry;   /* token expiry time (Vertex) */
  int _requesting_stream;    /* set before get_endpoint: 1=streaming, 0=non-streaming */

  /* ── Tool filter (set by caller before provider_complete) ── */
  const struct tool_filter_t *tool_filter; /* NULL = all tools */

  /* ── Abort flag for interruptible retry sleeps ── */
  _Atomic int abort_retry; /* set to 1 to cancel retry sleep early */

  /* ── Error diagnostics (populated on error, read by react.c) ── */
  char *last_error;          /* error message (curl error, HTTP error, etc.) */
  char *last_error_request;  /* raw request body that caused the error */
  char *last_error_response; /* raw server response body on error */
};

/* ── Provider lifecycle ─────────────────────────────────────────── */

/* Create a provider from configuration.
 * Returns NULL on invalid config. Caller must call provider_free(). */
provider_t *provider_create(const provider_config_t *cfg);

/* Free provider and all resources */
void provider_free(provider_t *p);

/* Build tool definitions from the shared registry for a given provider type.
 * Returns a cJSON array formatted for the provider's API. Caller owns result. */
cJSON *build_tools_from_registry(provider_type_t type);

/* Build tool definitions with an optional filter (whitelist/blacklist).
 * filter=NULL means all tools included. Uses tool_filter_t from tools.h. */
cJSON *build_tools_from_registry_filtered(provider_type_t type,
                                          const struct tool_filter_t *filter);

/* Shared utilities (used by local/openai providers) */

/* Build the "messages" JSON array from a chat history.
 * Returns cJSON array (caller owns). */
cJSON *build_messages_json(llm_chat_t *chat);

/* Build the common part of an OpenAI-compatible request body.
 * Returns cJSON object (caller owns, can add extra fields).
 * Shared by local and openai providers. */
cJSON *build_openai_base_request(provider_t *p, llm_chat_t *chat,
                                 int stream, const char *model_id,
                                 const char *max_token_field,
                                 provider_type_t provider_type);

/* Parse an OpenAI-compatible response JSON.
 * Returns unified JSON for tool calls or plain content string.
 * Shared by local and openai providers. */
char *parse_openai_response(provider_t *p, const char *response_json,
                            llm_chat_t *chat, llm_stats_t *stats);

/* Extract prompt/completion token stats from OpenAI-format response. */
void extract_openai_stats(cJSON *resp, llm_stats_t *stats);

/* Look up context window size by model ID prefix (unified table).
 * Returns 0 if no match found. */
int provider_lookup_context_size(const char *model_id);

/* Shared fetch_model_info for API providers (OpenAI, Anthropic, Vertex).
 * Uses static lookup table. Sets model_name, context_size, props_json=NULL. */
int provider_api_fetch_model_info(provider_t *p, int *context_size,
                                  char **model_name, char **props_json);

/* Cache an endpoint URL in provider->_cached_endpoint.
 * Returns the cached string (do NOT free). */
const char *provider_cache_endpoint(provider_t *p, const char *fmt, ...);

/* ── High-level API (uses vtable internally) ────────────────────── */

/* Non-streaming chat completion. Returns response string (caller frees). */
char *provider_complete(provider_t *p, llm_chat_t *chat, llm_stats_t *stats);

/* Streaming chat completion with token callback.
 * Returns unified JSON response string (caller frees). */
char *provider_complete_stream(provider_t *p, llm_chat_t *chat,
                               llm_stats_t *stats, provider_token_fn on_token,
                               void *userdata, int max_response_bytes,
                               int repeat_threshold,
                               provider_progress_fn on_progress,
                               void *progress_userdata);

/* ── Convenience: provider type from string ─────────────────────── */

provider_type_t provider_type_from_str(const char *s);
const char *provider_type_to_str(provider_type_t t);

#endif
