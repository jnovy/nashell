#include "tools_internal.h"
#include "tool_plugin.h"
#include "scratchpad.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── scratchpad section operations (GDN-2 inspired) ─── */

/* ── notes (section-based scratchpad) ────────────────── */

static void scratchpad_persist(tool_ctx_t *ctx) {
    scratchpad_save(&ctx->scratch, ctx->session_dir);
}

tool_result_t tool_notes(tool_ctx_t *ctx, cJSON *params) {
    cJSON *op_j = cJSON_GetObjectItem(params, "op");
    if (!op_j || !op_j->valuestring)
        return tools_make_error("missing 'op' parameter (write|append|clear)");

    const char *op = op_j->valuestring;
    cJSON *section_j = cJSON_GetObjectItem(params, "section");
    const char *section = section_j && section_j->valuestring ? section_j->valuestring : NULL;
    cJSON *content_j = cJSON_GetObjectItem(params, "content");
    const char *content = content_j && content_j->valuestring ? content_j->valuestring : NULL;
    cJSON *priority_j = cJSON_GetObjectItem(params, "priority");
    int priority = priority_j ? (int)cJSON_GetNumberValue(priority_j) : 5;

    if (strcmp(op, "write") == 0) {
        if (!section) return tools_make_error("'write' requires 'section' parameter");
        if (!content) return tools_make_error("'write' requires 'content' parameter");

        int rc = scratchpad_write(&ctx->scratch, section, content, priority);
        if (rc != 0) return tools_make_error("scratchpad full (max 32 sections)");

        scratchpad_persist(ctx);

        cJSON *meta = cJSON_CreateObject();
        cJSON_AddStringToObject(meta, "status", "ok");
        cJSON_AddStringToObject(meta, "op", "write");
        cJSON_AddStringToObject(meta, "section", section);
        cJSON_AddNumberToObject(meta, "sections", ctx->scratch.count);

        /* Store content for full audit trail */
        char *w_hash = store_save(ctx->store, content);
        char *w_alias = tool_register_alias(ctx, w_hash ? w_hash : "");
        tools_inject_thought(ctx, params);
        tool_journal(ctx, "notes", params, w_alias,
                       strlen(content), 0, NULL, NULL);
        free(w_alias); free(w_hash);
        return tools_make_result(1, meta, NULL);

    } else if (strcmp(op, "append") == 0) {
        if (!section) return tools_make_error("'append' requires 'section' parameter");
        if (!content) return tools_make_error("'append' requires 'content' parameter");

        int rc = scratchpad_append(&ctx->scratch, section, content, priority);
        if (rc != 0) return tools_make_error("scratchpad full");

        scratchpad_persist(ctx);

        int idx = scratchpad_find(&ctx->scratch, section);
        cJSON *meta = cJSON_CreateObject();
        cJSON_AddStringToObject(meta, "status", "ok");
        cJSON_AddStringToObject(meta, "op", "append");
        cJSON_AddStringToObject(meta, "section", section);
        cJSON_AddNumberToObject(meta, "total_chars",
                                idx >= 0 ? (double)strlen(ctx->scratch.sections[idx].content) : 0);

        /* Store content for full audit trail */
        char *a_hash = store_save(ctx->store, content);
        char *a_alias = tool_register_alias(ctx, a_hash ? a_hash : "");
        tools_inject_thought(ctx, params);
        tool_journal(ctx, "notes", params, a_alias,
                       strlen(content), 0, NULL, NULL);
        free(a_alias); free(a_hash);
        return tools_make_result(1, meta, NULL);

    } else if (strcmp(op, "clear") == 0) {
        if (!section) return tools_make_error("'clear' requires 'section' parameter");

        int rc = scratchpad_clear(&ctx->scratch, section);
        if (rc != 0) return tools_make_error("section not found");

        scratchpad_persist(ctx);

        cJSON *meta = cJSON_CreateObject();
        cJSON_AddStringToObject(meta, "status", "ok");
        cJSON_AddStringToObject(meta, "op", "clear");
        cJSON_AddStringToObject(meta, "section", section);
        cJSON_AddNumberToObject(meta, "sections", ctx->scratch.count);

        /* Store clear/delete status for audit trail */
        {
            char *c_str = cJSON_Print(meta);
            char *c_hash = store_save(ctx->store, c_str ? c_str : "{}");
            char *c_alias = tool_register_alias(ctx, c_hash ? c_hash : "");
            tools_inject_thought(ctx, params);
            tool_journal(ctx, "notes", params, c_alias,
                           0, 0, NULL, NULL);
            free(c_alias); free(c_hash); free(c_str);
        }
        return tools_make_result(1, meta, NULL);

    } else {
        return tools_make_error("unknown op (use: write, append, clear)");
    }
}

/* ── plugin registration ──────────────────────────────── */

static const tool_param_t notes_params[] = {
    TOOL_PARAM("op",       "string",  "Operation: write, append, clear",             1),
    TOOL_PARAM("section",  "string",  "Section name",                                0),
    TOOL_PARAM("content",  "string",  "Section content (for write/append)",          0),
    TOOL_PARAM("priority", "integer", "Section priority 1-9 (1=highest, default 5)", 0),
    TOOL_PARAM_END
};

static const tool_plugin_t notes_plugin =
    TOOL_DEF("notes",
             "Persistent scratchpad that survives context compaction. Supports section-based ops: notes(op=\"write\", section=\"name\", content=\"...\", priority=N) to write a section, notes(op=\"append\", section=\"name\", content=\"...\") to append, notes(op=\"clear\", section=\"name\") to delete a section. Priority 1=highest, 9=lowest (default 5). Record key findings in notes -- they survive context eviction. Save incrementally (every 3-5 file reads), not in one batch at the end.",
             notes_params, tool_notes);
TOOL_PLUGIN_REGISTER(notes_plugin)
