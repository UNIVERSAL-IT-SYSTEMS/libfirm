/*
 * Project:     libFIRM
 * File name:   ir/tr/type.c
 * Purpose:     Representation of types.
 * Author:      Goetz Lindenmaier
 * Modified by:
 * Created:
 * CVS-ID:      $Id$
 * Copyright:   (c) 2001-2003 Universitšt Karlsruhe
 * Licence:     This file protected by GPL -  GNU GENERAL PUBLIC LICENSE.
 */

/**
 *
 *   file type.c - implementation of the datastructure to hold
 *   type information.
 *  (C) 2004 by Universitaet Karlsruhe
 *  Goetz Lindenmaier
 *
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "type_identify.h"

#include <stdlib.h>
#include <stddef.h>
#include <string.h>

#include "type_t.h"
#include "tpop_t.h"
#include "irprog_t.h"
#include "typegmod.h"
#include "array.h"
#include "irprog_t.h"
#include "mangle.h"
#include "pset.h"

/* The hash set for types. */
static pset *type_table = NULL;


int compare_names (const void *tp1, const void *tp2) {
  type *t1 = (type *) tp1;
  type *t2 = (type *) tp2;

  return (t1 != t2 &&
	  (t1->type_op !=  t2->type_op ||
	   t1->name    !=  t2->name      )  );
}


/* stuff for comparing two types. */
//int compare_strict (type *tp1, type *tp2) {
int compare_strict (const void *tp1, const void *tp2) {
  type *t1 = (type *) tp1;
  type *t2 = (type *) tp2;
  return t1 != t2;
}

compare_types_func_tp compare_types_func = compare_strict;

/* stuff to compute a hash value for a type. */
int hash_name (type *tp) {
  unsigned h = (unsigned)tp->type_op;
  h = 9*h + (unsigned)tp->name;
  return h;
}

hash_types_func_tp hash_types_func = hash_name;


/* The function that hashes a type. */
type *mature_type(type *tp) {
  type *o;

  assert(type_table);

  o = pset_insert (type_table, tp, hash_types_func(tp) );

  if (!o || o == tp) return tp;

  exchange_types(tp, o);

  return o;
}


/* The function that hashes a type. */
type *mature_type_free(type *tp) {
  type *o;

  assert(type_table);

  o = pset_insert (type_table, tp, hash_types_func(tp) );

  if (!o || o == tp) return tp;

  free_type_entities(tp);
  free_type(tp);

  return o;
}

/* The function that hashes a type. */
type *mature_type_free_entities(type *tp) {
  type *o;

  assert(type_table);

  o = pset_insert (type_table, tp, hash_types_func(tp) );

  if (!o || o == tp) return tp;

  free_type_entities(tp);
  exchange_types(tp, o);

  return o;
}

void init_type_identify(void) {
  //type_table = new_pset ((int (const void *, const void *))compare_types_func, 8);

  type_table = new_pset (compare_types_func, 8);
  //type_table = new_pset (compare_names, 8);
}
