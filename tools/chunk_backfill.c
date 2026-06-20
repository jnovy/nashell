/* chunk_backfill.c — Standalone tool to backfill chunk embeddings for all sessions.
 * Usage: chunk_backfill [sessions_dir] [onnx_model_dir]
 *
 * This processes all sessions/<ts>/ directories that have journal.jsonl
 * but no chunks.emb, extracts semantic chunks, embeds them via ONNX,
 * and saves chunks.emb + chunks.idx files. */

#include "../src/session_index.h"
#include "../src/journal.h"
#include "../src/embedding.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int main(int argc, char *argv[]) {
    const char *sessions_dir = NULL;
    const char *onnx_model_dir = NULL;

    if (argc >= 3) {
        sessions_dir = argv[1];
        onnx_model_dir = argv[2];
    } else if (argc == 2) {
        sessions_dir = argv[1];
        const char *home = getenv("HOME");
        if (!home) {
            fprintf(stderr, "Usage: chunk_backfill <sessions_dir> [onnx_model_dir]\n");
            return 1;
        }
        char path[4096];
        snprintf(path, sizeof(path), "%s/.nash/onnx", home);
        onnx_model_dir = strdup(path);
    } else {
        const char *home = getenv("HOME");
        if (!home) {
            fprintf(stderr, "Usage: chunk_backfill [sessions_dir] [onnx_model_dir]\n");
            return 1;
        }
        char spath[4096], mpath[4096];
        snprintf(spath, sizeof(spath), "%s/.nash/sessions", home);
        snprintf(mpath, sizeof(mpath), "%s/.nash/onnx", home);
        sessions_dir = strdup(spath);
        onnx_model_dir = strdup(mpath);
    }

    fprintf(stderr, "Sessions dir: %s\n", sessions_dir);
    fprintf(stderr, "ONNX model:   %s\n", onnx_model_dir);

    embed_config_t ecfg = {0};
    ecfg.type = EMBED_ONNX;
    ecfg.model_path = (char *)onnx_model_dir;

    embed_ctx_t *embed = embed_new(&ecfg);
    if (!embed) {
        fprintf(stderr, "ERROR: Failed to create embedding context\n");
        return 1;
    }

    if (!embed_probe(embed)) {
        fprintf(stderr, "ERROR: ONNX embedding not available (model_path=%s)\n",
                onnx_model_dir);
        embed_free(embed);
        return 1;
    }

    fprintf(stderr, "ONNX embedding ready (dim=%d)\n", embed->detected_dim);

    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    int count = session_index_chunk_backfill(sessions_dir, embed, NULL);

    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double elapsed = (double)(t1.tv_sec - t0.tv_sec)
                   + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;

    fprintf(stderr, "Done: backfilled %d sessions in %.1f seconds\n",
            count, elapsed);

    embed_free(embed);
    return 0;
}
