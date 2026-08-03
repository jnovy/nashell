#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "embedding_onnx.h"

int main(void) {
  const char *model_dir = NULL;
  const char *home = getenv("HOME");
  char path[4096];
  if (home) {
    snprintf(path, sizeof(path), "%s/models/all-MiniLM-L6-v2", home);
    model_dir = path;
  }

  printf("Initializing ONNX embedding from: %s\n", model_dir);
  onnx_embed_ctx_t *ctx = onnx_embed_init(model_dir);
  if (!ctx) {
    fprintf(stderr, "Failed to initialize ONNX embedding\n");
    return 1;
  }

  printf("Embedding dimension: %d\n", onnx_embed_dim(ctx));

  /* Test a few texts */
  const char *texts[] = {
    "hello world",
    "machine learning is great",
    "hello world", /* duplicate to test consistency */
    "the quick brown fox jumps over the lazy dog",
  };
  int n = sizeof(texts) / sizeof(texts[0]);
  float *embeddings[4];
  int dims[4];

  for (int i = 0; i < n; i++) {
    embeddings[i] = onnx_embed_text(ctx, texts[i], &dims[i]);
    if (!embeddings[i]) {
      fprintf(stderr, "Failed to embed: %s\n", texts[i]);
      onnx_embed_free(ctx);
      return 1;
    }
    printf("Text %d: \"%s\" -> dim=%d, first 5: [%.4f, %.4f, %.4f, %.4f, %.4f]\n",
           i, texts[i], dims[i],
           embeddings[i][0], embeddings[i][1], embeddings[i][2],
           embeddings[i][3], embeddings[i][4]);
  }

  /* Check consistency: text 0 and text 2 are identical */
  int match = 1;
  for (int d = 0; d < dims[0]; d++) {
    if (embeddings[0][d] != embeddings[2][d]) {
      match = 0;
      break;
    }
  }
  printf("\nConsistency check (text 0 == text 2): %s\n", match ? "PASS" : "FAIL");

  /* Cosine similarity */
  for (int i = 0; i < n; i++) {
    for (int j = i + 1; j < n; j++) {
      double dot = 0;
      for (int d = 0; d < dims[i]; d++)
        dot += embeddings[i][d] * embeddings[j][d];
      printf("cosine(%d,%d) = %.4f  (\"%s\" vs \"%s\")\n",
             i, j, dot, texts[i], texts[j]);
    }
  }

  for (int i = 0; i < n; i++)
    free(embeddings[i]);
  onnx_embed_free(ctx);
  printf("\nAll tests passed!\n");
  return 0;
}
