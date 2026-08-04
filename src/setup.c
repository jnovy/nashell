/* Suppress -Wformat-truncation for URL scheme prepending.
 * snprintf with sizeof(buf) handles truncation safely. */
#pragma GCC diagnostic ignored "-Wformat-truncation"

/* setup.c - Interactive first-time setup wizard for Nash.
 *
 * Pure C, uses only stdio (printf/fgets) - no ncurses dependency.
 * Creates correctly-configured named providers and routing.
 * Writes config.toml + credentials.toml (optional). */

#include "setup.h"
#include "config.h"
#include "provider.h"
#include "str.h"
#include "cJSON.h"
#include "nash_limits.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <curl/curl.h>

/* ── Helpers ──────────────────────────────────────────────────────── */

/* Read a line from stdin, stripping newline.  Returns 0 on success. */
static int read_line(char *buf, size_t bufsz) {
  if (!fgets(buf, (int)bufsz, stdin)) return -1;
  size_t len = strlen(buf);
  if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';
  return 0;
}

/* Write a TOML key = "value" pair, escaping \ and " in value. */
static void fprint_toml_str(FILE *f, const char *key, const char *val) {
  fprintf(f, "%s = \"", key);
  for (const char *p = val; *p; p++) {
    if (*p == '\\' || *p == '"') fputc('\\', f);
    fputc(*p, f);
  }
  fprintf(f, "\"\n");
}

/* Prompt with a default value.  Fills buf with user input or default. */
static void prompt(const char *label, const char *def, char *buf, size_t bufsz) {
  if (def && def[0])
    fprintf(stderr, "%s [%s]: ", label, def);
  else
    fprintf(stderr, "%s: ", label);
  fflush(stderr);
  if (read_line(buf, bufsz) != 0 || buf[0] == '\0') {
    if (def)
      snprintf(buf, bufsz, "%s", def);
    else
      buf[0] = '\0';
  }
}

/* Prompt for a choice (1-based).  Returns choice number. */
static int prompt_choice(const char *label, int n_choices, int default_choice) {
  char buf[32];
  char def_str[8];
  snprintf(def_str, sizeof(def_str), "%d", default_choice);
  prompt(label, def_str, buf, sizeof(buf));
  int c = atoi(buf);
  if (c < 1 || c > n_choices) c = default_choice;
  return c;
}

/* Prompt yes/no.  Returns 1 for yes, 0 for no. */
static int prompt_yn(const char *label, int default_yes) {
  char buf[16];
  const char *def = default_yes ? "Y/n" : "y/N";
  fprintf(stderr, "%s [%s]: ", label, def);
  fflush(stderr);
  if (read_line(buf, sizeof(buf)) != 0 || buf[0] == '\0')
    return default_yes;
  return (buf[0] == 'y' || buf[0] == 'Y');
}

/* Mask an API key for display: show first 6 and last 3 chars. */
static void mask_key(const char *key, char *out, size_t outsz) {
  size_t len = key ? strlen(key) : 0;
  if (len < 12) {
    snprintf(out, outsz, "***");
  } else {
    snprintf(out, outsz, "%.6s...%.3s", key, key + len - 3);
  }
}

/* ── File download helper ────────────────────────────────────────── */

/* libcurl write callback that writes directly to a FILE*. */
static size_t file_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
  return fwrite(ptr, size, nmemb, (FILE *)userdata);
}

/* Download a URL to a local file.  Shows progress on stderr.
 * Returns 0 on success, -1 on failure (partial file removed). */
static int download_to_file(const char *url, const char *dest) {
  CURL *curl = curl_easy_init();
  if (!curl) return -1;

  FILE *f = fopen(dest, "wb");
  if (!f) {
    curl_easy_cleanup(curl);
    return -1;
  }

  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, file_write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, f);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L); /* show progress meter */
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 600L);  /* 10 min for large files */
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "nash/1.0");
  curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);

  CURLcode res = curl_easy_perform(curl);
  fclose(f);
  curl_easy_cleanup(curl);

  if (res != CURLE_OK) {
    unlink(dest); /* remove partial file */
    return -1;
  }
  return 0;
}

/* ── Provider testing ─────────────────────────────────────────────── */

/* Test connection to a local server.  Returns 0 on success.
 * On success, fills model_name (caller frees) and ctx_size. */
static int test_local(const char *url, char **model_name, int *ctx_size) {
  /* Try /v1/models first */
  char endpoint[NASH_PATH_MAX];
  str_t resp = {0};

  snprintf(endpoint, sizeof(endpoint), "%s/v1/models", url);
  if (http_get(endpoint, 5, &resp) == 0 && resp.data) {
    cJSON *root = cJSON_Parse(resp.data);
    if (root) {
      cJSON *data = cJSON_GetObjectItem(root, "data");
      if (cJSON_IsArray(data) && cJSON_GetArraySize(data) > 0) {
        cJSON *first = cJSON_GetArrayItem(data, 0);
        const char *mid = json_str(first, "id");
        if (mid)
          *model_name = xstrdup(mid);
      }
      cJSON_Delete(root);
    }
  }
  str_free(&resp);

  /* Try /props for context size */
  snprintf(endpoint, sizeof(endpoint), "%s/props", url);
  resp = (str_t){0};
  if (http_get(endpoint, 5, &resp) == 0 && resp.data) {
    cJSON *root = cJSON_Parse(resp.data);
    if (root) {
      cJSON *dgs = cJSON_GetObjectItem(root, "default_generation_settings");
      if (dgs) {
        *ctx_size = json_int(dgs, "n_ctx", *ctx_size);
      }
      cJSON_Delete(root);
    }
  }
  str_free(&resp);

  return (*model_name != NULL) ? 0 : -1;
}

/* Test an API provider with a trivial request.
 * For cloud APIs we just verify the key is non-empty;
 * actual connectivity test would need a real API call. */
static int test_api_provider(const char *type, const char *api_key_env,
                             const char *model_id) {
  (void)model_id;
  if (!api_key_env || !api_key_env[0]) return -1;
  const char *key = getenv(api_key_env);
  if (!key || !key[0]) {
    fprintf(stderr, "  Environment variable %s is not set.\n", api_key_env);
    return -1;
  }
  (void)type;
  return 0;
}

/* ── Provider configuration collector ─────────────────────────────── */

typedef struct {
  char name[64];
  char type[32];
  char api_base[512];
  char api_key_env[128];
  char api_key_value[256]; /* actual key value (for credentials.toml) */
  char model_id[256];
  char project_id[256];
  char region[128];
  int caching;
  int store_key_in_file; /* 1 = write to credentials.toml */
} setup_provider_t;

/* Configure a local provider.  Returns 0 on success. */
static int configure_local(setup_provider_t *sp) {
  snprintf(sp->type, sizeof(sp->type), "local");

  prompt("Local server URL", "http://localhost:8080",
         sp->api_base, sizeof(sp->api_base));

  /* Auto-prepend http:// if no scheme */
  if (sp->api_base[0] && strncmp(sp->api_base, "http://", 7) != 0 &&
      strncmp(sp->api_base, "https://", 8) != 0) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "http://%s", sp->api_base);
    snprintf(sp->api_base, sizeof(sp->api_base), "%s", tmp);
  }

  fprintf(stderr, "Testing connection... ");
  fflush(stderr);

  char *model_name = NULL;
  int ctx_size = 0;
  if (test_local(sp->api_base, &model_name, &ctx_size) != 0) {
    fprintf(stderr, "FAILED\n");
    fprintf(stderr, "  Could not connect to %s\n", sp->api_base);
    fprintf(stderr, "  Make sure your LLM server is running.\n");
    if (!prompt_yn("Continue anyway?", 0)) {
      free(model_name);
      return -1;
    }
  } else {
    if (ctx_size > 0)
      fprintf(stderr, "OK (%s, ctx=%d)\n", model_name ? model_name : "unknown", ctx_size);
    else
      fprintf(stderr, "OK (%s)\n", model_name ? model_name : "unknown");
  }

  prompt("Provider name", "local", sp->name, sizeof(sp->name));

  free(model_name);
  return 0;
}

/* Configure an Anthropic provider.  Returns 0 on success. */
static int configure_anthropic(setup_provider_t *sp) {
  snprintf(sp->type, sizeof(sp->type), "anthropic");
  snprintf(sp->api_key_env, sizeof(sp->api_key_env), "ANTHROPIC_API_KEY");

  /* Check if key is already in env */
  const char *existing = getenv("ANTHROPIC_API_KEY");
  if (existing && existing[0]) {
    char masked[64];
    mask_key(existing, masked, sizeof(masked));
    fprintf(stderr, "API key (ANTHROPIC_API_KEY) [from env: %s]: ", masked);
    fflush(stderr);
    char buf[256];
    if (read_line(buf, sizeof(buf)) == 0 && buf[0]) {
      snprintf(sp->api_key_value, sizeof(sp->api_key_value), "%s", buf);
    }
  } else {
    fprintf(stderr, "ANTHROPIC_API_KEY not found in environment.\n");
    prompt("API key", NULL, sp->api_key_value, sizeof(sp->api_key_value));
  }

  if (sp->api_key_value[0]) {
    fprintf(stderr, "\nStore API key in:\n");
    fprintf(stderr, "  1) Environment variable (recommended - add to shell rc)\n");
    fprintf(stderr, "  2) Config file (~/.nash/credentials.toml)\n");
    int choice = prompt_choice("Choice", 2, 1);
    sp->store_key_in_file = (choice == 2);
  }

  const char *env_model = getenv("ANTHROPIC_MODEL");
  prompt("Default model", env_model ? env_model : "claude-opus-4-6",
         sp->model_id, sizeof(sp->model_id));
  prompt("Provider name", "anthropic", sp->name, sizeof(sp->name));

  /* Test */
  if (existing || sp->api_key_value[0]) {
    fprintf(stderr, "Testing connection... ");
    fflush(stderr);
    if (test_api_provider("anthropic", sp->api_key_env, sp->model_id) == 0)
      fprintf(stderr, "OK\n");
    else
      fprintf(stderr, "WARNING: key may not be set\n");
  }

  return 0;
}

/* Configure an OpenAI provider.  Returns 0 on success. */
static int configure_openai(setup_provider_t *sp) {
  snprintf(sp->type, sizeof(sp->type), "openai");
  snprintf(sp->api_key_env, sizeof(sp->api_key_env), "OPENAI_API_KEY");

  const char *existing = getenv("OPENAI_API_KEY");
  if (existing && existing[0]) {
    char masked[64];
    mask_key(existing, masked, sizeof(masked));
    fprintf(stderr, "API key (OPENAI_API_KEY) [from env: %s]: ", masked);
    fflush(stderr);
    char buf[256];
    if (read_line(buf, sizeof(buf)) == 0 && buf[0]) {
      snprintf(sp->api_key_value, sizeof(sp->api_key_value), "%s", buf);
    }
  } else {
    fprintf(stderr, "OPENAI_API_KEY not found in environment.\n");
    prompt("API key", NULL, sp->api_key_value, sizeof(sp->api_key_value));
  }

  if (sp->api_key_value[0]) {
    fprintf(stderr, "\nStore API key in:\n");
    fprintf(stderr, "  1) Environment variable (recommended - add to shell rc)\n");
    fprintf(stderr, "  2) Config file (~/.nash/credentials.toml)\n");
    int choice = prompt_choice("Choice", 2, 1);
    sp->store_key_in_file = (choice == 2);
  }

  const char *env_model = getenv("OPENAI_MODEL");
  prompt("Default model", env_model ? env_model : "gpt-4o",
         sp->model_id, sizeof(sp->model_id));
  prompt("Provider name", "openai", sp->name, sizeof(sp->name));

  if (existing || sp->api_key_value[0]) {
    fprintf(stderr, "Testing connection... ");
    fflush(stderr);
    if (test_api_provider("openai", sp->api_key_env, sp->model_id) == 0)
      fprintf(stderr, "OK\n");
    else
      fprintf(stderr, "WARNING: key may not be set\n");
  }

  return 0;
}

/* Configure a Vertex AI provider.  Returns 0 on success. */
static int configure_vertex(setup_provider_t *sp) {
  snprintf(sp->type, sizeof(sp->type), "vertex");

  /* Use well-known env vars as defaults (same ones config.c checks at runtime) */
  const char *env_proj = getenv("ANTHROPIC_VERTEX_PROJECT_ID");
  const char *env_region = getenv("CLOUD_ML_REGION");
  const char *env_model = getenv("ANTHROPIC_MODEL");

  prompt("GCP Project ID", env_proj, sp->project_id, sizeof(sp->project_id));
  if (!sp->project_id[0]) {
    fprintf(stderr, "Project ID is required for Vertex AI.\n");
    return -1;
  }
  prompt("Region", env_region ? env_region : "us-east5",
         sp->region, sizeof(sp->region));
  prompt("Model", env_model ? env_model : "claude-opus-4-6",
         sp->model_id, sizeof(sp->model_id));
  sp->caching = prompt_yn("Enable prompt caching?", 1);
  prompt("Provider name", "vertex", sp->name, sizeof(sp->name));

  fprintf(stderr, "Note: Vertex AI uses 'gcloud auth print-access-token' for auth.\n");
  fprintf(stderr, "Make sure you have gcloud CLI configured.\n");

  return 0;
}

/* Configure a custom OpenAI-compatible endpoint.  Returns 0 on success. */
static int configure_custom(setup_provider_t *sp) {
  snprintf(sp->type, sizeof(sp->type), "openai");

  prompt("API base URL", NULL, sp->api_base, sizeof(sp->api_base));
  if (!sp->api_base[0]) {
    fprintf(stderr, "API base URL is required.\n");
    return -1;
  }

  /* Auto-prepend https:// if no scheme */
  if (strncmp(sp->api_base, "http://", 7) != 0 &&
      strncmp(sp->api_base, "https://", 8) != 0) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "https://%s", sp->api_base);
    snprintf(sp->api_base, sizeof(sp->api_base), "%s", tmp);
  }

  prompt("API key env var name", "CUSTOM_API_KEY",
         sp->api_key_env, sizeof(sp->api_key_env));

  const char *existing = getenv(sp->api_key_env);
  if (!existing || !existing[0]) {
    prompt("API key value", NULL, sp->api_key_value, sizeof(sp->api_key_value));
    if (sp->api_key_value[0]) {
      fprintf(stderr, "\nStore API key in:\n");
      fprintf(stderr, "  1) Environment variable (recommended - add to shell rc)\n");
      fprintf(stderr, "  2) Config file (~/.nash/credentials.toml)\n");
      int choice = prompt_choice("Choice", 2, 1);
      sp->store_key_in_file = (choice == 2);
    }
  }

  prompt("Model name", NULL, sp->model_id, sizeof(sp->model_id));
  prompt("Provider name", "custom", sp->name, sizeof(sp->name));

  return 0;
}

/* ── Embedding configuration ──────────────────────────────────────── */

typedef struct {
  char type[32];        /* "onnx", "ollama", "openai", "none" */
  char model_path[512]; /* ONNX model path */
  char model[256];      /* model name for ollama/openai */
  char api_base[512];   /* API base for ollama/openai */
} setup_embedding_t;

/* HuggingFace URLs for all-MiniLM-L6-v2 ONNX model */
#define HF_MINILM_BASE \
  "https://huggingface.co/sentence-transformers/all-MiniLM-L6-v2/resolve/main"
#define HF_MINILM_ONNX  HF_MINILM_BASE "/onnx/model.onnx"
#define HF_MINILM_VOCAB HF_MINILM_BASE "/vocab.txt"

/* Download the all-MiniLM-L6-v2 ONNX model into model_dir.
 * Creates model_dir/onnx/ and downloads model.onnx + vocab.txt.
 * Returns 0 on success, -1 on failure. */
static int download_onnx_model(const char *model_dir) {
  /* Create directories */
  char onnx_dir[NASH_PATH_MAX];
  snprintf(onnx_dir, sizeof(onnx_dir), "%s/onnx", model_dir);
  if (mkdir_p(model_dir, 0755) != 0) {
    fprintf(stderr, "  Failed to create %s: %s\n", model_dir, strerror(errno));
    return -1;
  }
  if (mkdir_p(onnx_dir, 0755) != 0) {
    fprintf(stderr, "  Failed to create %s: %s\n", onnx_dir, strerror(errno));
    return -1;
  }

  /* Download vocab.txt */
  char dest[NASH_PATH_MAX];
  snprintf(dest, sizeof(dest), "%s/vocab.txt", model_dir);
  fprintf(stderr, "  Downloading vocab.txt...\n");
  if (download_to_file(HF_MINILM_VOCAB, dest) != 0) {
    fprintf(stderr, "  Failed to download vocab.txt\n");
    return -1;
  }

  /* Download model.onnx (~86 MB) */
  snprintf(dest, sizeof(dest), "%s/onnx/model.onnx", model_dir);
  fprintf(stderr, "  Downloading model.onnx (~86 MB)...\n");
  if (download_to_file(HF_MINILM_ONNX, dest) != 0) {
    fprintf(stderr, "  Failed to download model.onnx\n");
    return -1;
  }

  fprintf(stderr, "  Download complete.\n");
  return 0;
}

static void configure_embedding(setup_embedding_t *emb,
                                const char *nash_dir) {
  fprintf(stderr, "\nEmbedding model for semantic memory:\n");
  fprintf(stderr, "  1) ONNX (built-in, fast, no server needed)\n");
  fprintf(stderr, "  2) Server-provided (Ollama or OpenAI-compatible)\n");
  fprintf(stderr, "  3) None (disable semantic memory matching)\n");
  int choice = prompt_choice("Choice", 3, 1);

  switch (choice) {
    case 1: {
      snprintf(emb->type, sizeof(emb->type), "onnx");

      /* Build default path under nash_dir */
      char default_path[NASH_PATH_MAX];
      snprintf(default_path, sizeof(default_path),
               "%s/models/all-MiniLM-L6-v2", nash_dir);

      prompt("ONNX model path", default_path,
             emb->model_path, sizeof(emb->model_path));

      /* Check if model files already exist */
      char check_path[NASH_PATH_MAX];
      snprintf(check_path, sizeof(check_path),
               "%s/onnx/model.onnx", emb->model_path);
      if (access(check_path, R_OK) == 0) {
        fprintf(stderr, "  Model already present at %s\n", emb->model_path);
        break;
      }

      /* Offer to download */
      if (prompt_yn("Download all-MiniLM-L6-v2 (~86 MB)?", 1)) {
        if (download_onnx_model(emb->model_path) != 0) {
          fprintf(stderr, "  Download failed. You can download manually later.\n");
        }
      } else {
        fprintf(stderr, "  Skipped. Place model.onnx and vocab.txt manually:\n");
        fprintf(stderr, "    %s/onnx/model.onnx\n", emb->model_path);
        fprintf(stderr, "    %s/vocab.txt\n", emb->model_path);
      }
      break;
    }
    case 2:
      snprintf(emb->type, sizeof(emb->type), "ollama");
      prompt("Embedding model name", "nomic-embed-text",
             emb->model, sizeof(emb->model));
      prompt("Embedding API base", "http://localhost:11434",
             emb->api_base, sizeof(emb->api_base));
      break;
    case 3:
      snprintf(emb->type, sizeof(emb->type), "none");
      break;
  }
}

/* ── Routing configuration ────────────────────────────────────────── */

typedef struct {
  char default_provider[64];
  char planner[64];
  char reflection[64];
  char consolidation[64];
} setup_routing_t;

static void configure_routing(setup_routing_t *rt,
                              setup_provider_t *providers, int n_providers) {
  if (n_providers < 2) {
    /* Single provider: use it for everything */
    snprintf(rt->default_provider, sizeof(rt->default_provider),
             "%s", providers[0].name);
    return;
  }

  fprintf(stderr, "\nConfigure role-based routing?\n");
  fprintf(stderr, "  Available providers:");
  for (int i = 0; i < n_providers; i++)
    fprintf(stderr, " %s", providers[i].name);
  fprintf(stderr, "\n");

  if (!prompt_yn("Configure routing?", 0)) {
    snprintf(rt->default_provider, sizeof(rt->default_provider),
             "%s", providers[0].name);
    return;
  }

  prompt("Default provider", providers[0].name,
         rt->default_provider, sizeof(rt->default_provider));
  prompt("Planning provider", rt->default_provider,
         rt->planner, sizeof(rt->planner));
  prompt("Reflection provider", rt->default_provider,
         rt->reflection, sizeof(rt->reflection));
  prompt("Consolidation provider", rt->default_provider,
         rt->consolidation, sizeof(rt->consolidation));
}

/* ── File writers ─────────────────────────────────────────────────── */

static int write_config_toml(const char *nash_dir,
                             setup_provider_t *providers, int n_providers,
                             setup_routing_t *routing,
                             setup_embedding_t *embedding) {
  char path[NASH_PATH_MAX];
  snprintf(path, sizeof(path), "%s/config.toml", nash_dir);

  /* Back up existing config before overwriting (destructive rewrite) */
  struct stat cfg_st;
  if (stat(path, &cfg_st) == 0) {
    char bak[NASH_PATH_MAX];
    snprintf(bak, sizeof(bak), "%s.bak", path);
    if (rename(path, bak) == 0)
      fprintf(stderr, "Backed up existing config to %s\n", bak);
    else
      fprintf(stderr, "Warning: could not back up %s: %s\n",
              path, strerror(errno));
  }

  FILE *f = fopen(path, "w");
  if (!f) {
    fprintf(stderr, "Error: cannot write %s: %s\n", path, strerror(errno));
    return -1;
  }

  /* WARNING: This overwrites the entire config with defaults + wizard answers.
     * Custom settings (limits, search, paths) are reset to defaults.
     * The old config is preserved as config.toml.bak. */
  fprintf(f, "# Nash configuration file\n");
  fprintf(f, "# Generated by 'nash --setup'\n");
  fprintf(f, "# See also: credentials.toml (API keys), models/*.toml (per-model profiles)\n");
  fprintf(f, "\n");

  /* Named providers */
  for (int i = 0; i < n_providers; i++) {
    setup_provider_t *sp = &providers[i];
    fprintf(f, "[providers.%s]\n", sp->name);
    fprintf(f, "type = \"%s\"\n", sp->type);
    if (sp->api_base[0])
      fprintf(f, "api_base = \"%s\"\n", sp->api_base);
    if (sp->model_id[0])
      fprintf(f, "model_id = \"%s\"\n", sp->model_id);
    if (sp->api_key_env[0])
      fprintf(f, "api_key_env = \"%s\"\n", sp->api_key_env);
    if (sp->project_id[0])
      fprintf(f, "project_id = \"%s\"\n", sp->project_id);
    if (sp->region[0])
      fprintf(f, "region = \"%s\"\n", sp->region);
    if (sp->caching)
      fprintf(f, "caching = true\n");
    fprintf(f, "\n");
  }

  /* Routing */
  fprintf(f, "[routing]\n");
  fprintf(f, "default = \"%s\"\n", routing->default_provider);
  if (routing->planner[0])
    fprintf(f, "planner = \"%s\"\n", routing->planner);
  if (routing->reflection[0])
    fprintf(f, "reflection = \"%s\"\n", routing->reflection);
  if (routing->consolidation[0])
    fprintf(f, "consolidation = \"%s\"\n", routing->consolidation);
  fprintf(f, "\n");

  /* Client defaults */
  fprintf(f, "[client]\n");
  fprintf(f, "temperature = 0.7\n");
  fprintf(f, "max_tokens = 16384\n");
  fprintf(f, "stream = true\n");
  fprintf(f, "\n");

  /* Thinking */
  fprintf(f, "[thinking]\n");
  fprintf(f, "mode = \"yes\"\n");
  fprintf(f, "\n");

  /* Embedding */
  fprintf(f, "[embedding]\n");
  fprintf(f, "type = \"%s\"\n", embedding->type);
  if (embedding->model_path[0])
    fprintf(f, "model_path = \"%s\"\n", embedding->model_path);
  if (embedding->model[0])
    fprintf(f, "model = \"%s\"\n", embedding->model);
  if (embedding->api_base[0])
    fprintf(f, "api_base = \"%s\"\n", embedding->api_base);
  fprintf(f, "\n");

  /* Limits - sensible defaults */
  fprintf(f, "[limits]\n");
  fprintf(f, "shell_timeout = 30\n");
  fprintf(f, "shell_max_output = 512000\n");
  fprintf(f, "file_max_size = 52428800\n");
  fprintf(f, "grep_timeout = 60\n");
  fprintf(f, "grep_max_matches = 50\n");
  fprintf(f, "web_timeout = 30\n");
  fprintf(f, "web_max_size = 512000\n");
  fprintf(f, "llm_max_response = 10485760\n");
  fprintf(f, "llm_repeat_threshold = 100\n");
  fprintf(f, "llm_timeout = 600\n");
  fprintf(f, "provider_max_retries = 10\n");
  fprintf(f, "provider_retry_base = 10\n");
  fprintf(f, "cycling_detection = true\n");
  fprintf(f, "max_react_steps = -1\n");
  fprintf(f, "context_eviction_pct = 70\n");
  fprintf(f, "memory_index_max = 50\n");
  fprintf(f, "max_reflection_steps = 4\n");
  fprintf(f, "reflection_gate = \"user_ask\"\n");
  fprintf(f, "\n");

  /* Paths */
  fprintf(f, "[paths]\n");
  fprintf(f, "data_dir = \"\"\n");
  fprintf(f, "plugin_dir = \"\"\n");
  fprintf(f, "\n");

  /* Search */
  fprintf(f, "[search]\n");
  fprintf(f, "engine = \"searxng\"\n");
  fprintf(f, "searxng_url = \"http://localhost:8888/search\"\n");

  fclose(f);
  fprintf(stderr, "Wrote %s\n", path);
  return 0;
}

static int write_credentials_toml(const char *nash_dir,
                                  setup_provider_t *providers, int n_providers) {
  /* Check if any provider needs credentials written */
  int need_creds = 0;
  for (int i = 0; i < n_providers; i++) {
    if (providers[i].store_key_in_file && providers[i].api_key_value[0]) {
      need_creds = 1;
      break;
    }
  }
  if (!need_creds) return 0;

  char path[NASH_PATH_MAX];
  snprintf(path, sizeof(path), "%s/credentials.toml", nash_dir);

  /* Back up existing credentials before overwriting */
  struct stat cred_st;
  if (stat(path, &cred_st) == 0) {
    char bak[NASH_PATH_MAX];
    snprintf(bak, sizeof(bak), "%s.bak", path);
    rename(path, bak);
  }

  FILE *f = fopen(path, "w");
  if (!f) {
    fprintf(stderr, "Error: cannot write %s: %s\n", path, strerror(errno));
    return -1;
  }

  fprintf(f, "# Nash credentials file\n");
  fprintf(f, "# API keys for named providers. Env vars take precedence over this file.\n");
  fprintf(f, "# This file should be chmod 0600 (readable only by you).\n");
  fprintf(f, "\n");

  for (int i = 0; i < n_providers; i++) {
    setup_provider_t *sp = &providers[i];
    if (sp->store_key_in_file && sp->api_key_value[0]) {
      fprintf(f, "[providers.%s]\n", sp->name);
      fprint_toml_str(f, "api_key", sp->api_key_value);
      fprintf(f, "\n");
    }
  }

  fclose(f);
  chmod(path, 0600);
  fprintf(stderr, "Wrote %s (chmod 0600)\n", path);
  return 0;
}

/* ── Main entry point ─────────────────────────────────────────────── */

int setup_run(const char *nash_dir, int add_only) {
  if (add_only) {
    fprintf(stderr, "Warning: --add-only mode is not yet implemented; "
                    "running full setup instead.\n");
  }

  /* Check for existing config */
  char config_path[NASH_PATH_MAX];
  snprintf(config_path, sizeof(config_path), "%s/config.toml", nash_dir);
  struct stat st;
  if (stat(config_path, &st) == 0) {
    if (!prompt_yn("\nConfig exists. Reconfigure?", 0)) {
      fprintf(stderr, "Setup cancelled.\n");
      return 0;
    }
  }

  fprintf(stderr, "\nNash - First-time setup\n");
  fprintf(stderr, "\nWhat LLM provider would you like to use?\n\n");
  fprintf(stderr, "  1) Local server (llama.cpp, Ollama, vLLM, or any OpenAI-compatible)\n");
  fprintf(stderr, "  2) Anthropic (Claude)\n");
  fprintf(stderr, "  3) OpenAI (GPT-4o, GPT-5)\n");
  fprintf(stderr, "  4) Google Vertex AI (Claude via Google Cloud)\n");
  fprintf(stderr, "  5) Custom OpenAI-compatible cloud endpoint\n");
  fprintf(stderr, "\n");

  setup_provider_t providers[8];
  int n_providers = 0;
  memset(providers, 0, sizeof(providers));

  /* First provider */
  int choice = prompt_choice("Choice", 5, 1);
  int rc = -1;
  switch (choice) {
    case 1:
      rc = configure_local(&providers[0]);
      break;
    case 2:
      rc = configure_anthropic(&providers[0]);
      break;
    case 3:
      rc = configure_openai(&providers[0]);
      break;
    case 4:
      rc = configure_vertex(&providers[0]);
      break;
    case 5:
      rc = configure_custom(&providers[0]);
      break;
  }
  if (rc != 0) {
    fprintf(stderr, "Setup aborted.\n");
    return 1;
  }
  n_providers = 1;

  /* Additional providers */
  while (n_providers < 8 && prompt_yn("\nAdd another provider?", 0)) {
    fprintf(stderr, "\n");
    fprintf(stderr, "  1) Local server\n");
    fprintf(stderr, "  2) Anthropic (Claude)\n");
    fprintf(stderr, "  3) OpenAI\n");
    fprintf(stderr, "  4) Google Vertex AI\n");
    fprintf(stderr, "  5) Custom endpoint\n");
    fprintf(stderr, "\n");
    choice = prompt_choice("Choice", 5, 1);
    rc = -1;
    switch (choice) {
      case 1:
        rc = configure_local(&providers[n_providers]);
        break;
      case 2:
        rc = configure_anthropic(&providers[n_providers]);
        break;
      case 3:
        rc = configure_openai(&providers[n_providers]);
        break;
      case 4:
        rc = configure_vertex(&providers[n_providers]);
        break;
      case 5:
        rc = configure_custom(&providers[n_providers]);
        break;
    }
    if (rc == 0) n_providers++;
  }

  /* Embedding */
  setup_embedding_t embedding = {0};
  configure_embedding(&embedding, nash_dir);

  /* Routing */
  setup_routing_t routing = {0};
  configure_routing(&routing, providers, n_providers);

  /* Write files */
  fprintf(stderr, "\n");
  if (write_config_toml(nash_dir, providers, n_providers, &routing, &embedding) != 0)
    return 1;
  if (write_credentials_toml(nash_dir, providers, n_providers) != 0)
    return 1;

  /* Create models/ directory */
  {
    char models_dir[NASH_PATH_MAX];
    snprintf(models_dir, sizeof(models_dir), "%s/models", nash_dir);
    mkdir(models_dir, 0755);
  }

  /* Env var reminders for keys not stored in file */
  int need_env_hint = 0;
  for (int i = 0; i < n_providers; i++) {
    if (providers[i].api_key_value[0] && !providers[i].store_key_in_file) {
      if (!need_env_hint) {
        fprintf(stderr, "\nTo complete setup, add to your shell profile:\n");
        need_env_hint = 1;
      }
      /* Mask key: show only last 4 chars to avoid leaking secrets */
      const char *key = providers[i].api_key_value;
      size_t klen = strlen(key);
      if (klen > 4)
        fprintf(stderr, "  export %s=\"...%s\"\n",
                providers[i].api_key_env, key + klen - 4);
      else
        fprintf(stderr, "  export %s=\"****\"\n",
                providers[i].api_key_env);
    }
  }

  fprintf(stderr, "\nReady! Run: nash \"your task here\"\n");

  /* Clear sensitive data from stack */
  memset(providers, 0, sizeof(providers));
  return 0;
}
