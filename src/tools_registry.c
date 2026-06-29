#include "tools_registry.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The canonical tool registry — single source of truth for all tools. */
const tool_def_t TOOL_REGISTRY[] = {
    {"shell_exec",
     "Execute a shell command (git, make, docker, gh, npm, etc.). "
     "For file reading use file_read, for content search use grep_search, "
     "for file search use glob_search, for URL fetching use web_fetch. "
     "Limit output: pipe through head -50, tail, jq, grep.",
     "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\",\"description\":\"Shell command\"}},\"required\":[\"command\"]}"},

    {"file_read",
     "Read contents of a file. Supports line ranges to avoid reading entire large files. "
     "Use start_line/end_line for specific sections (1-based). "
     "Negative start_line reads from end (e.g., -20 = last 20 lines).",
     "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"File path\"},\"start_line\":{\"type\":\"integer\",\"description\":\"First line to read (1-based, default: 1). Negative = from end (-20 = last 20 lines)\"},\"end_line\":{\"type\":\"integer\",\"description\":\"Last line to read (1-based inclusive, default: EOF)\"}},\"required\":[\"path\"]}"},

    {"file_write",
     "Write content to a file (under workspace dir).",
     "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"File path\"},\"content\":{\"type\":\"string\",\"description\":\"File content\"}},\"required\":[\"path\",\"content\"]}"},

    {"file_edit",
     "Edit a file by replacing exact text. Always file_read first to copy exact text.",
     "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"File path\"},\"old_text\":{\"type\":\"string\",\"description\":\"Exact text to find (must match)\"},\"new_text\":{\"type\":\"string\",\"description\":\"Replacement text\"}},\"required\":[\"path\",\"old_text\",\"new_text\"]}"},

    {"grep_search",
     "Search file contents with a regex pattern.",
     "{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":\"string\",\"description\":\"Regex pattern\"},\"path\":{\"type\":\"string\",\"description\":\"Directory or file to search in\"}},\"required\":[\"pattern\"]}"},

    {"web_fetch",
     "Fetch content from a URL.",
     "{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\",\"description\":\"URL to fetch\"}},\"required\":[\"url\"]}"},

    {"web_search",
     "Search the web for information.",
     "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"Search query\"}},\"required\":[\"query\"]}"},

    {"glob_search",
     "Search for files matching a glob pattern.",
     "{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":\"string\",\"description\":\"Glob pattern (e.g. **/*.py)\"},\"path\":{\"type\":\"string\",\"description\":\"Directory or file to search in\"}},\"required\":[\"pattern\"]}"},

    {"memory_store",
     "Store reusable knowledge in long-term memory.",
     "{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\",\"description\":\"Memory key\"},\"value\":{\"type\":\"string\",\"description\":\"Content to store\"},\"refs\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Related memory keys for cross-references\"},\"supersedes\":{\"type\":\"string\",\"description\":\"Key of the memory this entry replaces (lesson lineage tracking)\"},\"global\":{\"type\":\"boolean\",\"description\":\"Store in global memory instead of workspace (default: false)\"}},\"required\":[\"key\",\"value\"]}"},

    {"memory_search",
     "Search long-term memory and past session journals. Use when the task may depend on user preferences, prior decisions, or historical context. "
     "Supports semantic search (query), exact key lookup (key), lexical/regex search across session journals (pattern), or any combination. "
     "Returns interleaved results from curated memory and session history, ranked by relevance.",
     "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"Search query\"},\"key\":{\"type\":\"string\",\"description\":\"Exact key to recall\"},\"pattern\":{\"type\":\"string\",\"description\":\"Substring pattern to search for (case-insensitive). When regex=true, this is a POSIX Extended Regular Expression.\"},\"regex\":{\"type\":\"boolean\",\"description\":\"Use POSIX Extended Regular Expression matching instead of substring (default: false).\"},\"max_results\":{\"type\":\"integer\",\"description\":\"Maximum results to return (default: 20, max: 100)\"},\"days\":{\"type\":\"integer\",\"description\":\"Only search sessions up to N days old (default: all)\"}}}"},

    {"memory_pin",
     "Pin an existing memory so it is always injected into the system prompt.",
     "{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\",\"description\":\"Memory key to pin\"}},\"required\":[\"key\"]}"},

    {"memory_unpin",
     "Unpin a memory so it is no longer always injected into the system prompt.",
     "{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\",\"description\":\"Memory key to unpin\"}},\"required\":[\"key\"]}"},

    {"done",
     "Signal task completion. Include all concrete data (paths, numbers, URLs) in result. "
     "The user CANNOT see notes/scratchpad — never say \"see above\" or reference data only in notes. "
     "Copy all relevant content (tables, lists, data) directly into the result text.",
     "{\"type\":\"object\",\"properties\":{\"result\":{\"type\":\"string\",\"description\":\"Complete answer with details\"}},\"required\":[\"result\"]}"},

    {"plan",
     "Outline a numbered execution plan (3-8 steps) before starting work.",
     "{\"type\":\"object\",\"properties\":{\"result\":{\"type\":\"string\",\"description\":\"Numbered plan: 1. step (tool)\\n2. ...\"}},\"required\":[\"result\"]}"},

    {"notes",
     "Persistent scratchpad that survives context compaction. "
     "Supports section-based ops: notes(op=\"write\", section=\"name\", content=\"...\", priority=N) to write a section, "
     "notes(op=\"append\", section=\"name\", content=\"...\") to append, "
     "notes(op=\"clear\", section=\"name\") to delete a section. "
     "Priority 1=highest, 9=lowest (default 5).",
     "{\"type\":\"object\",\"properties\":{\"op\":{\"type\":\"string\",\"description\":\"Operation: write, append, clear\"},\"section\":{\"type\":\"string\",\"description\":\"Section name\"},\"content\":{\"type\":\"string\",\"description\":\"Section content (for write/append)\"},\"priority\":{\"type\":\"integer\",\"description\":\"Section priority 1-9 (1=highest, default 5)\"}},\"required\":[\"op\"]}"},

    {"user_ask",
     "Ask the user a clarifying question. Use when you need information "
     "that cannot be determined from the codebase or context. The react loop "
     "pauses until the user responds. "
     "Prefer calling this EARLY (step 0-2) when the task is ambiguous, rather "
     "than guessing and discovering the wrong assumption later.",
     "{\"type\":\"object\",\"properties\":{\"question\":{\"type\":\"string\",\"description\":\"Question to ask the user\"}},\"required\":[\"question\"]}"},

    {"memory_delete",
     "Delete a memory entry by key.",
     "{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\",\"description\":\"Memory key to delete\"}},\"required\":[\"key\"]}"},

    {"image_analyze",
     "Analyze an image file using the LLM's vision capabilities. "
     "Reads the image, base64-encodes it, and sends it to the provider "
     "for multimodal analysis. Returns a textual description/analysis. "
     "Supports: png, jpg/jpeg, gif, webp, bmp, svg, tiff. Max 20 MB.",
     "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Path to the image file\"},\"question\":{\"type\":\"string\",\"description\":\"What to analyze or ask about the image (default: describe in detail)\"}},\"required\":[\"path\"]}"},
    {"todo",
     "Persistent per-workspace TODO list that survives across sessions. "
     "Use to park findings, ideas, or action items for later. "
     "Stored in todo.md within the active workspace directory (human-editable).",
     "{\"type\":\"object\",\"properties\":{\"op\":{\"type\":\"string\",\"description\":\"Operation: add, list, done, remove, purge\"},\"text\":{\"type\":\"string\",\"description\":\"TODO text (for add)\"},\"index\":{\"type\":\"integer\",\"description\":\"Item number (for done/remove)\"}},\"required\":[\"op\"]}"},
    {"device_control",
     "Control a device's GUI (computer, phone, tablet, kiosk). "
     "Workflow: screenshot to see+parse the screen (OmniParser + OCR), then act, then screenshot to verify.\\n"
     "Commands and parameters:\\n"
     "- screenshot: capture + run OmniParser widget detection + Tesseract OCR. Returns detected widgets with coordinates and all screen text. Optional delay_ms (max 10000)\\n"
     "- left_click: x, y (required). Standard click\\n"
     "- right_click: x, y (required). Context menu\\n"
     "- middle_click: x, y (required)\\n"
     "- double_click: x, y (required). Open file / select word\\n"
     "- triple_click: x, y (required). Select entire line or paragraph\\n"
     "- type: text (required). Types into the focused element\\n"
     "- key: key_name (required). Examples: enter, tab, escape, backspace, space, delete, ctrl+c, alt+tab, super\\n"
     "- scroll: direction (required: up/down/left/right), optional x, y, amount (default 3)\\n"
     "- drag: start_x, start_y, end_x, end_y (all required), optional button\\n"
     "- move: x, y (required). Move cursor without clicking\\n"
     "- long_press: x, y (required), hold_ms (default 500). For mobile/tablet context menus\\n"
     "Only for visual/GUI tasks. For CLI delays, use shell_exec with sleep.",
     "{\"type\":\"object\",\"properties\":{"
       "\"command\":{\"type\":\"string\",\"description\":"
         "\"Command to execute\","
         "\"enum\":[\"screenshot\",\"left_click\",\"right_click\",\"middle_click\","
         "\"double_click\",\"triple_click\",\"type\",\"key\",\"scroll\",\"drag\","
         "\"move\",\"long_press\"]},"
       "\"x\":{\"type\":\"integer\",\"description\":\"X coordinate (pixels)\"},"
       "\"y\":{\"type\":\"integer\",\"description\":\"Y coordinate (pixels)\"},"
       "\"text\":{\"type\":\"string\",\"description\":\"Text to type (for type command)\"},"
       "\"key_name\":{\"type\":\"string\",\"description\":"
         "\"Key name (for key command): enter, tab, escape, ctrl+c, etc.\"},"
       "\"button\":{\"type\":\"string\",\"description\":"
         "\"Mouse button for drag: left, right, middle (default: left)\"},"
       "\"direction\":{\"type\":\"string\",\"description\":"
         "\"Scroll direction: up, down, left, right\"},"
       "\"amount\":{\"type\":\"integer\",\"description\":\"Scroll amount (default: 3)\"},"
       "\"start_x\":{\"type\":\"integer\",\"description\":\"Drag start X\"},"
       "\"start_y\":{\"type\":\"integer\",\"description\":\"Drag start Y\"},"
       "\"end_x\":{\"type\":\"integer\",\"description\":\"Drag end X\"},"
       "\"end_y\":{\"type\":\"integer\",\"description\":\"Drag end Y\"},"
       "\"hold_ms\":{\"type\":\"integer\",\"description\":"
         "\"Hold duration in ms for long_press (default: 500, min: 100, max: 10000)\"},"
       "\"delay_ms\":{\"type\":\"integer\",\"description\":"
         "\"Delay in ms before screenshot capture (max 10000). For UI animations/loading\"}"
     "},\"required\":[\"command\"]}"},
    {NULL, NULL, NULL}  /* sentinel */
};

/* FIX #14: TOOL_REGISTRY_COUNT is now a #define in tools_registry.h */

/* Build a comma-separated list of all tool names from the registry.
 * Caller must free() the returned string. */
char *tool_registry_names_csv(void) {
    size_t total = 0;
    for (int i = 0; i < TOOL_REGISTRY_COUNT; i++) {
        total += strlen(TOOL_REGISTRY[i].name) + 2; /* ", " */
    }
    char *buf = malloc(total + 1);
    if (!buf) return NULL;
    buf[0] = '\0';
    for (int i = 0; i < TOOL_REGISTRY_COUNT; i++) {
        if (i > 0) strcat(buf, ", ");
        strcat(buf, TOOL_REGISTRY[i].name);
    }
    return buf;
}
