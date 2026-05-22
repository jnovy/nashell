#ifndef LLM_H
#define LLM_H

#include "cJSON.h"

/* Configuration for the LLM endpoint */
typedef struct {
    char *api_base;       /* e.g. "http://192.168.1.18:8080" */
    char *model;          /* e.g. "qwen3.6-35b-a3b" */
    int   max_tokens;     /* max completion tokens */
    float temperature;    /* sampling temperature */
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

/* Send chat completion request. Returns assistant response content (caller frees).
 * On error returns NULL. */
char *llm_complete(const llm_config_t *cfg, llm_chat_t *chat);

/* Parse the assistant's JSON response into action fields.
 * Returns cJSON object with thought, action, and tool-specific params.
 * Caller must cJSON_Delete. Returns NULL on parse failure. */
cJSON *llm_parse_action(const char *response);

#endif
