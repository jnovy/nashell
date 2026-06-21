#ifndef LLM_H
#define LLM_H

#include "cJSON.h"
#include <stdarg.h>

/* Configuration for the LLM endpoint */
typedef struct {
    const char *api_base;       /* e.g. "http://localhost:8080" */
    const char *model;          /* e.g. "qwen3.6-35b-a3b" */
    int   max_tokens;     /* max completion tokens */
    float temperature;    /* sampling temperature */
    int   context_size;   /* server's n_ctx (0 = unknown, fetched via /props) */
    int   enable_thinking; /* 0=off, 1=on — set per-request by EDRM routing */
    int   thinking_budget; /* -1=unrestricted, 0=none, N>0=max thinking tokens */
    char *last_error;          /* populated on LLM error — server message, curl error, etc.
                                * Caller should free after reading. Set by provider_complete/stream. */
    char *last_error_response; /* raw server response body on error (for post-mortem).
                                * Contains the full JSON with error details + offset info. */
    char *last_error_request;  /* raw request body that triggered the error.
                                * The JSON we sent — shows exactly what was malformed. */
} llm_config_t;

/* EDRM entropy probe result — see [arXiv:2605.22873] */
typedef struct {
    float h_mean;    /* mean entropy over probe tokens */
    float rho_s;     /* Spearman rank correlation (entropy vs step) */
    float vnr;       /* von Neumann ratio (smoothness) */
    int   route;     /* 0=direct (thinking off), 1=cot (thinking on) */
} edrm_result_t;

/* Belief Entropy probe result — see MMPO [arXiv:2605.30159]
 * ℋ_BE(m_t) = H(y | m_t, q) — entropy of response to anchor question q
 * given current memory m_t. Lower = clearer belief, higher = ambiguous. */
typedef struct {
    float h_mean;    /* mean token-level entropy of response */
    float h_total;   /* total entropy (h_mean * n_tokens) */
    int   n_tokens;  /* number of tokens in response */
    int   ok;        /* 1 = probe succeeded, 0 = failed */
} belief_entropy_result_t;

/* EDRM entropy probe: generate a short completion and analyze entropy dynamics.
 * Returns routing decision based on entropy trajectory descriptors. */
/* Apply chat template via server /apply-template endpoint.
 * Returns malloc'd formatted prompt string, or NULL on failure. */
char *llm_apply_template(const char *api_base, const char *user_query);

edrm_result_t llm_edrm_probe(const char *api_base, const char *prompt,
                               int n_predict, int n_probs, float temperature,
                               float tau_rho, float tau_vnr, float tau_h);

/* Belief Entropy probe: send anchor question with memory context,
 * compute mean token-level entropy of response.
 * Returns belief_entropy_result_t with h_mean = ℋ_BE(m_t).
 * Pass NULL for memory_context to probe with no memory context. */
belief_entropy_result_t llm_belief_entropy_probe(const char *api_base,
                                                   const char *memory_context,
                                                   const char *anchor_question,
                                                   int n_predict, int n_probs,
                                                   float temperature);

/* Message type — typed alternative to content-prefix scanning.
 * Enables type-safe eviction policies and eliminates brittle
 * strncmp("[SCRATCHPAD]",...) / strstr("ERROR:") routing. */
typedef enum {
    LLM_MSG_GENERIC = 0,     /* untyped (legacy, assistant responses, user queries) */
    LLM_MSG_SYSTEM,          /* system prompt */
    LLM_MSG_MEMORY_INDEX,    /* [MEMORY INDEX] injection */
    LLM_MSG_PINNED,          /* [PINNED KNOWLEDGE] injection */
    LLM_MSG_SKILLS,          /* [RELEVANT SKILLS] injection */
    LLM_MSG_LESSONS,         /* [RELEVANT LESSONS] injection */
    LLM_MSG_STRATEGIES,      /* [RELEVANT STRATEGIES] injection */
    LLM_MSG_ANTIPATTERNS,    /* [RELEVANT ANTI-PATTERNS] injection */
    LLM_MSG_SCRATCHPAD,      /* [SCRATCHPAD] injection */
    LLM_MSG_PREV_RESULT,     /* [PREVIOUS RESULT] injection */
    LLM_MSG_USER_QUERY,      /* the actual user query */
    LLM_MSG_TOOL_RESULT,     /* tool execution result */
    LLM_MSG_ERROR,           /* error message (tool failure, parse error) */
    LLM_MSG_MEMORY_HINT,     /* [MEMORY HINT] error-triggered retrieval */
    LLM_MSG_THINKING,        /* thought-only assistant response */
    LLM_MSG_EVICTION_SUMMARY,/* scratchpad re-injection after eviction */
    LLM_MSG_TEMPORAL,        /* [TEMPORAL CONTEXT] chronological memory calendar */
    LLM_MSG_EPISODIC,        /* [RECALLED SESSION CHUNK] episodic journal recall */
} llm_msg_type_t;

/* Message importance level — controls eviction priority.
 * Inspired by Harness-1's 4-level importance tagging for curated documents.
 * See: arXiv 2606.02373 "Harness-1: RL for Search Agents with State-Externalizing Harnesses" */
typedef enum {
    LLM_MSG_IMPORTANCE_LOW = 0,     /* errors, stale hints — evict first */
    LLM_MSG_IMPORTANCE_NORMAL = 1,  /* regular tool results — default */
    LLM_MSG_IMPORTANCE_HIGH = 2,    /* recent results, grep matches — compress before evict */
    LLM_MSG_IMPORTANCE_CRITICAL = 3 /* system, user query, scratchpad — never evict */
} llm_msg_importance_t;

/* Message recoverability level — controls eviction priority.
 * Messages whose content is persisted elsewhere can be evicted more aggressively
 * because the agent can recover them via file_read.
 * Based on CWL [arXiv:2606.11213] recoverability-aware eviction. */
typedef enum {
    LLM_RECOVER_NONE = 0,       /* content exists only in context — evict last */
    LLM_RECOVER_SCRATCHPAD,     /* key findings saved to scratchpad */
    LLM_RECOVER_STORE,          /* content saved to store (file_read can recover) */
    LLM_RECOVER_FILE,           /* content written to a file (file_write/file_edit) */
    LLM_RECOVER_MEMORY,         /* content stored in long-term memory */
} llm_recoverability_t;

/* A single chat message — supports tool_calls API threading */
typedef struct {
    char *role;              /* "system", "user", "assistant", "tool" */
    char *content;
    size_t content_len;      /* cached strlen(content) — updated on creation/replacement.
                              * Eliminates hundreds of redundant strlen() calls in eviction,
                              * compression, and scoring (Review C4/B3). */
    char *tool_call_id;      /* for role:"tool" — the ID of the tool call being responded to */
    char *tool_calls_json;   /* for role:"assistant" — raw JSON of tool_calls array */
    char *tool_call_id_outbound; /* Cached outbound tool_call ID extracted from tool_calls_json.
                                  * For assistant msgs with tool_calls — stores the ID from the
                                  * first tool_call entry for O(1) partner matching (Proposal C). */
    llm_msg_type_t msg_type; /* typed message category (0 = generic/legacy) */
    llm_msg_importance_t importance; /* eviction priority (Harness-1 §3.2) */
    llm_recoverability_t recoverability; /* CWL §3: how recoverable is this content? */
    char *store_alias;       /* LCM-Lite: store ref alias (e.g. "R0S5") for breadcrumb eviction */
} llm_msg_t;

/* Chat completion request/response */
typedef struct {
    llm_msg_t *msgs;
    int        n_msgs;
    int        cap_msgs;
    long       total_chars;          /* cached sum of all msgs[i].content_len — updated
                                      * incrementally by add/remove/insert/replace operations.
                                      * Eliminates 17 O(n) react_calc_total_chars() loops (Review B1/C4). */
    /* Last tool call info (set by provider_complete/provider_complete_stream for react.c) */
    char      *last_tool_call_id;    /* tool_call_id from last response (caller frees) */
    char      *last_tool_calls_json; /* raw tool_calls JSON from last response (caller frees) */
    int        multi_tool_count;     /* >1 if model emitted multiple tool calls (only first executed) */
} llm_chat_t;

/* Initialize/free chat */
llm_chat_t *llm_chat_new(void);
void        llm_chat_free(llm_chat_t *chat);
void        llm_chat_add(llm_chat_t *chat, const char *role, const char *content);

/* Add a typed message — sets msg_type for structured routing.
 * Replaces content-prefix scanning with type-safe dispatch. */
void        llm_chat_add_typed(llm_chat_t *chat, const char *role,
                               const char *content, llm_msg_type_t type);

/* Add a typed message with printf-style formatting.
 * Handles the alloc + snprintf + add_typed + free pattern internally.
 * Returns 0 on success, -1 on allocation failure. */
int         llm_chat_add_formatted(llm_chat_t *chat, const char *role,
                                    llm_msg_type_t type,
                                    const char *fmt, ...)
            __attribute__((format(printf, 4, 5)));

/* Serialize entire chat into a human-readable markdown document.
 * Returns malloc'd string. Caller must free. */
char       *llm_chat_serialize(llm_chat_t *chat);

/* Free all heap fields of a single message (but not the struct itself).
 * Used by evict_sweep_marked for O(n) single-pass compaction (Review C2). */
void        llm_msg_free_fields(llm_msg_t *m);

/* Remove all messages of a given type.
 * Returns the number of messages removed. */
int         llm_chat_remove_by_type(llm_chat_t *chat, llm_msg_type_t type);

/* D1 FIX: Remove all messages matching ANY of the given types in a single pass.
 * Avoids O(n) × k scanning when removing k types sequentially.
 * Returns the total number of messages removed. */
int         llm_chat_remove_by_types(llm_chat_t *chat,
                                     const llm_msg_type_t *types, int n_types);

/* Find the first message of a given type.
 * Returns index, or -1 if not found.
 * Type-safe alternative to scanning content prefixes. */
int         llm_chat_find_by_type(llm_chat_t *chat, llm_msg_type_t type);

/* Remove a range of messages [start, end).
 * Properly frees all fields. Encapsulates the manual memmove pattern. */
void        llm_chat_remove_range(llm_chat_t *chat, int start, int end);

/* FIX D5: Insert a typed message at a specific position.
 * Shifts existing messages from pos..n_msgs-1 to make room.
 * Encapsulates the manual realloc+memmove pattern used by context eviction. */
void        llm_chat_insert_typed(llm_chat_t *chat, int pos,
                                  const char *role, const char *content,
                                  llm_msg_type_t type);

/* Add a tool result message (role: "tool" with tool_call_id) */
void llm_chat_add_tool_result(llm_chat_t *chat, const char *tool_call_id,
                               const char *content);

/* Add an assistant message with tool_calls (for conversation history) */
void llm_chat_add_assistant_tool_call(llm_chat_t *chat, const char *content,
                                       const char *tool_calls_json);

/* Replace the content of message at index `idx` with `new_content` (takes ownership).
 * Updates content_len and total_chars incrementally.
 * Review C4: Centralizes content replacement to maintain cached char counts. */
void llm_chat_replace_content(llm_chat_t *chat, int idx, char *new_content);

/* LLM inference statistics from API response */
typedef struct {
    int    prompt_tokens;
    int    completion_tokens;
    double prompt_per_second;
    double predicted_per_second;
    int    draft_n;
    int    draft_accepted;
} llm_stats_t;

/* Parse the assistant's JSON response into action fields.
 * If multi_count is non-NULL, detects concatenated JSON objects (common with
 * gemma4/qwen3.6 models that emit multiple tool calls as content). Sets
 * *multi_count to the number of JSON objects found (1 = normal). Only the
 * first object is returned; the rest are silently discarded. */
cJSON *llm_parse_action(const char *response, int *multi_count);

/* Fetch server info */
int llm_fetch_context_size(const char *api_base);
char *llm_fetch_model_name(const char *api_base);
char *llm_fetch_props_json(const char *api_base);

#endif
