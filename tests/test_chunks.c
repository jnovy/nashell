/* Quick test: extract chunks from a session and print them */
#include <stdio.h>
#include <string.h>
#include "../src/journal.h"

int main(int argc, char *argv[]) {
  const char *session_dir;
  if (argc > 1) {
    session_dir = argv[1];
  } else {
    fprintf(stderr, "Usage: test_chunks <session_dir>\n");
    return 1;
  }

  journal_chunks_t jc = journal_extract_chunks(session_dir, 900, 50);
  printf("Extracted %d chunks from %s\n", jc.n_chunks, session_dir);

  for (int i = 0; i < jc.n_chunks; i++) {
    printf("\n=== Chunk %d (%d chars) ===\n", i, (int)strlen(jc.texts[i]));
    /* Print first 300 chars of each chunk */
    int len = (int)strlen(jc.texts[i]);
    if (len > 300) len = 300;
    printf("%.*s\n", len, jc.texts[i]);
    if ((int)strlen(jc.texts[i]) > 300) printf("...\n");
  }

  journal_chunks_free(&jc);
  return 0;
}
