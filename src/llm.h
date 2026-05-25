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
    int   enable_thinking; /* 0=off, 1=on — set per-request by EDRM routing */
} llm_config_t;

/* EDRM entropy probe result — see [arXiv:2605.22873] */
typedef struct {
    float h_mean;    /* mean entropy over probe tokens */
    float rho_s;     /* Spearman rank correlation (entropy vs step) */
    float vnr;       /* von Neumann ratio (smoothness) */
    int   route;     /* 0=direct (thinking off), 1=cot (thinking on) */
} edrm_result_t;

/* EDRM entropy probe: generate a short completion and analyze entropy dynamics.
 * Returns routing decision based on entropy trajectory descriptors. */
edrm_result_t llm_edrm_probe(const char *api_base, const char *prompt,
                               int n_predict, int n_probs, float temperature,
                               float tau_rho, float tau_vnr, float tau_h);

/* A single chat message — supports tool_calls API threading */
typedef struct {
    char *role;              /* "system", "user", "assistant", "tool" */
    char *content;
    char *tool_call_id;      /* for role:"tool" — the ID of the tool call being responded to */
    char *tool_calls_json;   /* for role:"assistant" — raw JSON of tool_calls array */
} llm_msg_t;

/* Chat completion request/response */
typedef struct {
    llm_msg_t *msgs;
    int        n_msgs;
    int        cap_msgs;
    /* Last tool call info (set by llm_complete/llm_complete_stream for react.c) */
    char      *last_tool_call_id;    /* tool_call_id from last response (caller frees) */
    char      *last_tool_calls_json; /* raw tool_calls JSON from last response (caller frees) */
} llm_chat_t;

/* Initialize/free chat */
llm_chat_t *llm_chat_new(void);
void        llm_chat_free(llm_chat_t *chat);
void        llm_chat_add(llm_chat_t *chat, const char *role, const char *content);

/* Add a tool result message (role: "tool" with tool_call_id) */
void llm_chat_add_tool_result(llm_chat_t *chat, const char *tool_call_id,
                               const char *content);

/* Add an assistant message with tool_calls (for conversation history) */
void llm_chat_add_assistant_tool_call(llm_chat_t *chat, const char *content,
                                       const char *tool_calls_json);

/* LLM inference statistics from API response */
typedef struct {
    int    prompt_tokens;
    int    completion_tokens;
    double prompt_per_second;
    double predicted_per_second;
    int    draft_n;
    int    draft_accepted;
} llm_stats_t;

/* Send chat completion request (non-streaming). Returns response (caller frees). */
char *llm_complete(const llm_config_t *cfg, llm_chat_t *chat, llm_stats_t *stats);

/* Token callback for streaming */
typedef void (*llm_token_fn)(const char *token, void *userdata);

/* Streaming chat completion with native tool calling support.
 * Returns the response as a JSON string: {"thought":"...", "action":"name", ...params}
 * For tool calls, the response is assembled from streaming tool_calls chunks.
 * For plain text, the response is the raw content. */
char *llm_complete_stream(const llm_config_t *cfg, llm_chat_t *chat,
                          llm_stats_t *stats, llm_token_fn on_token, void *userdata,
                          int max_response_bytes, int repeat_threshold);

/* Parse the assistant's JSON response into action fields. */
cJSON *llm_parse_action(const char *response);

/* Fetch server info */
int llm_fetch_context_size(const char *api_base);
char *llm_fetch_model_name(const char *api_base);
char *llm_fetch_props_json(const char *api_base);

#endif
