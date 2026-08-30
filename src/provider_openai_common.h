/* provider_openai_common.h - shared helpers for OpenAI-compatible providers.
 * Internal header: only provider.c, provider_openai.c, and provider_local.c
 * should include this. NOT part of the public provider API. */
#ifndef PROVIDER_OPENAI_COMMON_H
#define PROVIDER_OPENAI_COMMON_H

#include "provider.h"

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

#endif /* PROVIDER_OPENAI_COMMON_H */
