#ifndef YAML_PARSE_H
#define YAML_PARSE_H

/*
 * Minimal YAML parser for playbook files.
 * Supports: mappings, sequences, block scalars (|, >), quoted/unquoted strings,
 * booleans, integers. Does NOT support: anchors, aliases, tags, flow collections,
 * multi-document streams, complex keys.
 *
 * The parser produces a tree of yaml_node_t nodes.
 */

typedef enum {
  YAML_SCALAR,   /* string value */
  YAML_MAPPING,  /* key-value pairs */
  YAML_SEQUENCE, /* ordered list */
} yaml_type_t;

typedef struct yaml_node {
  yaml_type_t type;
  char *scalar; /* for YAML_SCALAR: the string value */

  /* For YAML_MAPPING: parallel arrays of keys and values */
  char **keys;
  struct yaml_node **values;
  int n_children;
  int cap_children;

  /* For YAML_SEQUENCE: array of items */
  struct yaml_node **items;
  int n_items;
  int cap_items;
} yaml_node_t;

/* Parse a YAML string into a tree. Returns root node (caller frees with yaml_free).
 * Returns NULL on parse error. */
yaml_node_t *yaml_parse(const char *input);

/* Parse a YAML file. Returns root node or NULL. */
yaml_node_t *yaml_parse_file(const char *path);

/* Free a YAML tree */
void yaml_free(yaml_node_t *node);

/* Accessors — return NULL if not found or wrong type */

/* Get a mapping child by key */
yaml_node_t *yaml_get(yaml_node_t *node, const char *key);

/* Get scalar value (returns NULL if not a scalar) */
const char *yaml_str(yaml_node_t *node);

/* Get scalar as int (returns def if not found/not parseable) */
int yaml_int(yaml_node_t *node, int def);

/* Get scalar as bool (returns def if not found/not parseable) */
int yaml_bool(yaml_node_t *node, int def);

/* Get sequence length (returns 0 if not a sequence) */
int yaml_len(yaml_node_t *node);

/* Get sequence item by index (returns NULL if out of range) */
yaml_node_t *yaml_item(yaml_node_t *node, int index);

#endif /* YAML_PARSE_H */
