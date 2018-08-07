/*
 * ejabberd Business Edition, Copyright (C) 2002-2018   ProcessOne
 *
 * The ejabberd software is the exclusive property of the licensor
 * ProcessOne. It is protected by the law on copyright and
 * international conventions. As a result, the dealer
 * recognizes that will make every effort to ensure the confidentiality
 * on the software. It is recalled that a violation of the rights of
 * authors of the software is an infringement and that any
 * counterfeit is punishable in France by Article L339-2 of the Code of
 * Intellectual property and punishable by three years imprisonment and
 * 300000 euros.
 *
 * Any infringement liable to be so qualified and
 * would be caused by third parties and whose dealer has knowledge
 * should be terminated by the licensor that it will make its case
 * personal conduct of the proceedings. Any allegation of infringement
 * formed against the dealer because of the use of the Software will
 * be brought to the knowledge of the licensor which will assist
 * in defense of the dealer in the manner and form that
 * see fit and fix alone.
 *
 */

#include <erl_nif.h>
#include <stdio.h>
#include <errno.h>
#include "uthash.h"

/****************************************************************
 *               Structures/Globals definitions                 *
 ****************************************************************/
typedef struct __tree_t {
  char *key;
  char *val;
  int refc;
  struct __tree_t *sub;
  UT_hash_handle hh;
} tree_t;

typedef struct {
  tree_t *tree;
  ErlNifRWLock *lock;
} state_t;

static ErlNifResourceType *tree_state_t = NULL;

/****************************************************************
 *                   MQTT Tree Manipulation                     *
 ****************************************************************/
tree_t *tree_new(char *key, size_t len) {
  tree_t *tree = malloc(sizeof(tree_t));
  if (tree) {
    memset(tree, 0, sizeof(tree_t));
    if (key && len) {
      tree->key = malloc(len);
      if (tree->key) {
	memcpy(tree->key, key, len);
      } else {
	free(tree);
	tree = NULL;
      }
    }
  }
  return tree;
}

void tree_free(tree_t *t) {
  tree_t *found, *iter;
  if (t) {
    free(t->key);
    free(t->val);
    HASH_ITER(hh, t->sub, found, iter) {
      HASH_DEL(t->sub, found);
      tree_free(found);
    }
    memset(t, 0, sizeof(tree_t));
    free(t);
  }
}

void tree_clear(tree_t *root) {
  tree_t *found, *iter;
  HASH_ITER(hh, root->sub, found, iter) {
    HASH_DEL(root->sub, found);
    tree_free(found);
  }
}

int tree_add(tree_t *root, char *path, size_t size) {
  int i = 0;
  size_t len;
  tree_t *t = root;
  tree_t *found, *new;

  while (i<=size) {
    len = strlen(path+i) + 1;
    HASH_FIND_STR(t->sub, path+i, found);
    if (found) {
      i += len;
      t = found;
    } else {
      new = tree_new(path+i, len);
      if (new) {
	HASH_ADD_STR(t->sub, key, new);
	i += len;
	t = new;
      } else {
	free(path);
	return errno;
      }
    }
  }

  if (t->val) {
    free(path);
  } else {
    for (i=0; i<size; i++) {
      if (!path[i])
	path[i] = '/';
    }
    t->val = path;
  }
  t->refc++;
  return 0;
}

int tree_del(tree_t *root, char *path, size_t i, size_t size) {
  tree_t *found;

  if (i<=size) {
    HASH_FIND_STR(root->sub, path+i, found);
    if (found) {
      i += strlen(path+i) + 1;
      int deleted = tree_del(found, path, i, size);
      if (deleted) {
	HASH_DEL(root->sub, found);
	tree_free(found);
      }
    }
  } else if (root->refc) {
    root->refc--;
    if (!root->refc) {
      free(root->val);
      root->val = NULL;
    }
  }

  return !root->refc && !root->sub;
}

void tree_size(tree_t *tree, size_t *size) {
  tree_t *found, *iter;

  HASH_ITER(hh, tree->sub, found, iter) {
    if (found->refc) (*size)++;
    tree_size(found, size);
  }
}

int tree_refc(tree_t *tree, char *path, size_t i, size_t size) {
  tree_t *found;

  if (i<=size) {
    HASH_FIND_STR(tree->sub, path+i, found);
    if (found) {
      i += strlen(path+i) + 1;
      return tree_refc(found, path, i, size);
    } else {
      return 0;
    }
  } else
    return tree->refc;
}

/****************************************************************
 *                        NIF helpers                           *
 ****************************************************************/
static ERL_NIF_TERM cons(ErlNifEnv *env, char *str, ERL_NIF_TERM tail)
{
  if (str) {
    size_t len = strlen(str);
    ERL_NIF_TERM head;
    unsigned char *buf = enif_make_new_binary(env, len, &head);
    if (buf) {
      memcpy(buf, str, len);
      return enif_make_list_cell(env, head, tail);
    }
  }
  return tail;
}

static void match(ErlNifEnv *env, tree_t *root,
		  char *path, size_t i, size_t size, ERL_NIF_TERM *acc)
{
  tree_t *found;
  size_t len = 0;

  if (i<=size) {
    HASH_FIND_STR(root->sub, path+i, found);
    if (found) {
      len = strlen(path+i) + 1;
      match(env, found, path, i+len, size, acc);
    };
    HASH_FIND_STR(root->sub, "+", found);
    if (found) {
      len = strlen(path+i) + 1;
      match(env, found, path, i+len, size, acc);
    }
    HASH_FIND_STR(root->sub, "#", found);
    if (found) {
      *acc = cons(env, found->val, *acc);
    }
  } else {
    *acc = cons(env, root->val, *acc);
    HASH_FIND_STR(root->sub, "#", found);
    if (found)
      *acc = cons(env, found->val, *acc);
  }
}

static void to_list(ErlNifEnv *env, tree_t *root, ERL_NIF_TERM *acc)
{
  tree_t *found, *iter;

  HASH_ITER(hh, root->sub, found, iter) {
    if (found->val) {
      size_t len = strlen(found->val);
      ERL_NIF_TERM refc = enif_make_int(env, found->refc);
      ERL_NIF_TERM val;
      unsigned char *buf = enif_make_new_binary(env, len, &val);
      if (buf) {
	memcpy(buf, found->val, len);
	*acc = enif_make_list_cell(env, enif_make_tuple2(env, val, refc), *acc);
      }
    };
    to_list(env, found, acc);
  }
}

static ERL_NIF_TERM dump(ErlNifEnv *env, tree_t *tree)
{
  tree_t *found, *iter;
  ERL_NIF_TERM tail, head;

  tail = enif_make_list(env, 0);
  HASH_ITER(hh, tree->sub, found, iter) {
    head = dump(env, found);
    tail = enif_make_list_cell(env, head, tail);
  }
  if (tree->key) {
    ERL_NIF_TERM part, path;
    part = enif_make_string(env, tree->key, ERL_NIF_LATIN1);
    if (tree->val)
      path = enif_make_string(env, tree->val, ERL_NIF_LATIN1);
    else
      path = enif_make_atom(env, "none");
    return enif_make_tuple4(env, part, path, enif_make_int(env, tree->refc), tail);
  } else
    return tail;
}

static ERL_NIF_TERM raise(ErlNifEnv *env, int err)
{
  switch (err) {
  case ENOMEM:
    return enif_raise_exception(env, enif_make_atom(env, "enomem"));
  default:
    return enif_make_badarg(env);
  }
}

static char *prep_path(ErlNifBinary *bin) {
  int i;
  unsigned char c;
  char *buf = malloc(bin->size+1);
  if (buf) {
    buf[bin->size] = 0;
    for (i=0; i<bin->size; i++) {
      c = bin->data[i];
      if (c == '/') buf[i] = 0;
      else buf[i] = c;
    }
  }
  return buf;
}

/****************************************************************
 *                 Constructors/Destructors                     *
 ****************************************************************/
static state_t *init_tree_state(ErlNifEnv *env) {
  state_t *state = enif_alloc_resource(tree_state_t, sizeof(state_t));
  if (state) {
    memset(state, 0, sizeof(state_t));
    state->tree = tree_new(NULL, 0);
    state->lock = enif_rwlock_create("mqtree_lock");
    if (state->tree && state->lock)
      return state;
    else
      enif_release_resource(state);
  }
  return NULL;
}

static void destroy_tree_state(ErlNifEnv *env, void *data) {
  state_t *state = (state_t *) data;
  if (state) {
    tree_free(state->tree);
    if (state->lock) enif_rwlock_destroy(state->lock);
  }
  memset(state, 0, sizeof(state_t));
}

/****************************************************************
 *                      NIF definitions                         *
 ****************************************************************/
static int load(ErlNifEnv* env, void** priv, ERL_NIF_TERM max) {
  ErlNifResourceFlags flags = ERL_NIF_RT_CREATE | ERL_NIF_RT_TAKEOVER;
  tree_state_t = enif_open_resource_type(env, NULL, "mqtree_state",
					 destroy_tree_state,
					 flags, NULL);
  return 0;
}

static void unload(ErlNifEnv* env, void* priv) {}

static ERL_NIF_TERM new_0(ErlNifEnv* env, int argc,
			  const ERL_NIF_TERM argv[])
{
  ERL_NIF_TERM result;
  state_t *state = init_tree_state(env);
  if (state) {
    result = enif_make_resource(env, state);
    enif_release_resource(state);
  } else
    result = raise(env, ENOMEM);

  return result;
}

static ERL_NIF_TERM insert_2(ErlNifEnv* env, int argc,
			     const ERL_NIF_TERM argv[])
{
  state_t *state;
  ErlNifBinary path_bin;
  ERL_NIF_TERM result;

  if (!enif_get_resource(env, argv[0], tree_state_t, (void *) &state) ||
      !enif_inspect_iolist_as_binary(env, argv[1], &path_bin))
    return raise(env, EINVAL);

  if (!path_bin.size)
    return enif_make_atom(env, "ok");

  char *path = prep_path(&path_bin);
  if (path) {
    enif_rwlock_rwlock(state->lock);
    int ret = tree_add(state->tree, path, path_bin.size);
    enif_rwlock_rwunlock(state->lock);
    if (!ret)
      result = enif_make_atom(env, "ok");
    else
      result = raise(env, ENOMEM);
  } else
    result = raise(env, ENOMEM);

  return result;
}

static ERL_NIF_TERM delete_2(ErlNifEnv* env, int argc,
			     const ERL_NIF_TERM argv[])
{
  state_t *state;
  ErlNifBinary path_bin;
  ERL_NIF_TERM result;

  if (!enif_get_resource(env, argv[0], tree_state_t, (void *) &state) ||
      !enif_inspect_iolist_as_binary(env, argv[1], &path_bin))
    return raise(env, EINVAL);

  if (!path_bin.size)
    return enif_make_atom(env, "ok");

  char *path = prep_path(&path_bin);
  if (path) {
    enif_rwlock_rwlock(state->lock);
    tree_del(state->tree, path, 0, path_bin.size);
    enif_rwlock_rwunlock(state->lock);
    free(path);
    result = enif_make_atom(env, "ok");
  } else
    result = raise(env, ENOMEM);

  return result;
}

static ERL_NIF_TERM match_2(ErlNifEnv* env, int argc,
			    const ERL_NIF_TERM argv[])
{
  state_t *state;
  ErlNifBinary path_bin;
  ERL_NIF_TERM result = enif_make_list(env, 0);

  if (!enif_get_resource(env, argv[0], tree_state_t, (void *) &state) ||
      !enif_inspect_iolist_as_binary(env, argv[1], &path_bin))
    return raise(env, EINVAL);

  if (!path_bin.size)
    return result;

  char *path = prep_path(&path_bin);
  if (path) {
    enif_rwlock_rlock(state->lock);
    match(env, state->tree, path, 0, path_bin.size, &result);
    enif_rwlock_runlock(state->lock);
    free(path);
  } else
    result = raise(env, ENOMEM);

  return result;
}

static ERL_NIF_TERM refc_2(ErlNifEnv* env, int argc,
			   const ERL_NIF_TERM argv[])
{
  state_t *state;
  ErlNifBinary path_bin;

  if (!enif_get_resource(env, argv[0], tree_state_t, (void *) &state) ||
      !enif_inspect_iolist_as_binary(env, argv[1], &path_bin))
    return raise(env, EINVAL);

  if (!path_bin.size)
    return enif_make_int(env, 0);

  char *path = prep_path(&path_bin);
  if (path) {
    enif_rwlock_rlock(state->lock);
    int refc = tree_refc(state->tree, path, 0, path_bin.size);
    enif_rwlock_runlock(state->lock);
    free(path);
    return enif_make_int(env, refc);
  } else
    return raise(env, ENOMEM);
}

static ERL_NIF_TERM clear_1(ErlNifEnv* env, int argc,
			    const ERL_NIF_TERM argv[])
{
  state_t *state;
  if (!enif_get_resource(env, argv[0], tree_state_t, (void *) &state))
    return raise(env, EINVAL);

  enif_rwlock_rwlock(state->lock);
  tree_clear(state->tree);
  enif_rwlock_rwunlock(state->lock);

  return enif_make_atom(env, "ok");
}

static ERL_NIF_TERM size_1(ErlNifEnv* env, int argc,
			   const ERL_NIF_TERM argv[])
{
  state_t *state;
  size_t size = 0;
  if (!enif_get_resource(env, argv[0], tree_state_t, (void *) &state))
    return raise(env, EINVAL);

  enif_rwlock_rlock(state->lock);
  tree_size(state->tree, &size);
  enif_rwlock_runlock(state->lock);

  return enif_make_uint64(env, (ErlNifUInt64) size);
}

static ERL_NIF_TERM is_empty_1(ErlNifEnv* env, int argc,
			       const ERL_NIF_TERM argv[])
{
  state_t *state;
  if (!enif_get_resource(env, argv[0], tree_state_t, (void *) &state))
    return raise(env, EINVAL);

  enif_rwlock_rlock(state->lock);
  char *ret = state->tree->sub ? "false" : "true";
  enif_rwlock_runlock(state->lock);

  return enif_make_atom(env, ret);
}

static ERL_NIF_TERM to_list_1(ErlNifEnv* env, int argc,
			      const ERL_NIF_TERM argv[])
{
  state_t *state;
  ERL_NIF_TERM result = enif_make_list(env, 0);

  if (!enif_get_resource(env, argv[0], tree_state_t, (void *) &state))
    return raise(env, EINVAL);

  enif_rwlock_rlock(state->lock);
  to_list(env, state->tree, &result);
  enif_rwlock_runlock(state->lock);

  return result;
}

static ERL_NIF_TERM dump_1(ErlNifEnv* env, int argc,
			   const ERL_NIF_TERM argv[])
{
  state_t *state;
  if (!enif_get_resource(env, argv[0], tree_state_t, (void *) &state))
    return raise(env, EINVAL);

  enif_rwlock_rlock(state->lock);
  ERL_NIF_TERM result = dump(env, state->tree);
  enif_rwlock_runlock(state->lock);

  return result;
}

static ErlNifFunc nif_funcs[] =
  {
    {"new", 0, new_0},
    {"insert", 2, insert_2},
    {"delete", 2, delete_2},
    {"match", 2, match_2},
    {"refc", 2, refc_2},
    {"clear", 1, clear_1},
    {"size", 1, size_1},
    {"is_empty", 1, is_empty_1},
    {"to_list", 1, to_list_1},
    {"dump", 1, dump_1}
  };

ERL_NIF_INIT(mqtree, nif_funcs, load, NULL, NULL, unload)