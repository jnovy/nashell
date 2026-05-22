#ifndef LLM_H
#define LLM_H

#include "cJSON.h"

/* Configuration for the LLM endpoint */
typedef struct {
    const char *api_base;       /* e.g. "http://192.168.1.18:8080" */
    const char *model;          /* e.g. "qwen3.6-35b-a3b" */
    int   max_tokens;     /* max completion tokens */
    float temperature;    /* sampling temperature */
    int   context_size;   /* server's n_ctx (0 = unknown, fetched via /props) */
} llm_config_t;

/* A single chat message */
typedef struct {
    char *role;           /* "system", "user", "assistant" */
    char *content;
} llm_msg_t;

/* Chat completion request/response */
typedef struct {
    llm_msg_t *msgs;
    int        n_msgs;
    int        cap_msgs;
} llm_chat_t;

/* Initialize/free chat */
llm_chat_t *llm_chat_new(void);
void        llm_chat_free(llm_chat_t *chat);
void        llm_chat_add(llm_chat_t *chat, const char *role, const char *content);

/* LLM inference statistics from API response */
typedef struct {
    int    prompt_tokens;       /* number of prompt tokens processed */
    int    completion_tokens;   /* number of tokens generated */
    double prompt_per_second;   /* prompt processing speed (t/s) */
    double predicted_per_second;/* generation speed (t/s) */
    int    draft_n;             /* speculative decoding: total drafted */
    int    draft_accepted;      /* speculative decoding: accepted */
} llm_stats_t;

/* Send chat completion request. Returns assistant response content (caller frees).
 * If stats is non-NULL, fills it with timing/usage data from the API response.
 * On error returns NULL. */
char *llm_complete(const llm_config_t *cfg, llm_chat_t *chat, llm_stats_t *stats);

/* Token callback for streaming — called for each token as it arrives */
typedef void (*llm_token_fn)(const char *token, void *userdata);

/* Streaming chat completion. Calls on_token for each token as it arrives.
 * Returns the full assembled response content (caller frees).
 * If stats is non-NULL, fills it with timing/usage data from the final SSE event.
 * If on_token is NULL, behaves like llm_complete (non-streaming).
 * On error returns NULL. */
char *llm_complete_stream(const llm_config_t *cfg, llm_chat_t *chat,
                          llm_stats_t *stats, llm_token_fn on_token, void *userdata);

/* Parse the assistant's JSON response into action fields.
 * Returns cJSON object with thought, action, and tool-specific params.
 * Caller must cJSON_Delete. Returns NULL on parse failure. */
cJSON *llm_parse_action(const char *response);

/* Fetch the server's context window size (n_ctx) from /props endpoint.
 * Returns n_ctx on success, 0 on failure. */
int llm_fetch_context_size(const char *api_base);

/* Fetch the model name from /v1/models endpoint.
 * Returns strdup'd model name on success, NULL on failure. Caller must free. */
char *llm_fetch_model_name(const char *api_base);

#endif
