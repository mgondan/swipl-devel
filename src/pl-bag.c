/*  Part of SWI-Prolog

    Author:        Jan Wielemaker
    E-mail:        J.Wielemaker@vu.nl
    WWW:           http://www.swi-prolog.org
    Copyright (c)  1985-2024, University of Amsterdam
                              VU University Amsterdam
			      CWI Amsterdam
			      SWI-Prolog Solutions b.v.
    All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions
    are met:

    1. Redistributions of source code must retain the above copyright
       notice, this list of conditions and the following disclaimer.

    2. Redistributions in binary form must reproduce the above copyright
       notice, this list of conditions and the following disclaimer in
       the documentation and/or other materials provided with the
       distribution.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
    "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
    LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
    FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
    COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
    INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
    BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
    LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
    CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
    LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
    ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
    POSSIBILITY OF SUCH DAMAGE.
*/

/*#define O_DEBUG 1*/
#include "pl-bag.h"
#include "pl-rec.h"
#include "pl-gc.h"
#include "pl-fli.h"

#undef LD
#define LD LOCAL_LD

		 /*******************************
		 *	    TEMP MALLOC		*
		 *******************************/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Allocate memory for  findall  bags  in   chunks  that  can  be discarded
together  and  preallocate  the  first    chunk.  This  approach  avoids
fragmentation and reduces the number of  allocation calls. The latter is
notably needed to reduce allocation contention   due to intensive use of
findall/3.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#define FIRST_CHUNK_SIZE (256*sizeof(void*))

typedef struct mem_chunk
{ struct mem_chunk *prev;
  size_t	size;
  size_t	used;
} mem_chunk;

typedef struct mem_pool
{ mem_chunk    *chunks;
  size_t	chunk_count;
  mem_chunk	first;
  char		first_data[FIRST_CHUNK_SIZE];
} mem_pool;

static void
init_mem_pool(mem_pool *mp)
{ mp->chunks      = &mp->first;
  mp->chunk_count = 1;
  mp->first.size  = FIRST_CHUNK_SIZE;
  mp->first.used  = 0;
}

#define ROUNDUP(n,m) (((n) + (m - 1)) & ~(m-1))

static void *
alloc_mem_pool(mem_pool *mp, size_t bytes)
{ char *ptr;

  if ( mp->chunks->used + bytes <= mp->chunks->size )
  { ptr = &((char *)(mp->chunks+1))[mp->chunks->used];
    mp->chunks->used += ROUNDUP(bytes, sizeof(void*));
  } else
  { size_t chunksize = tmp_nalloc(4000*((size_t)1<<mp->chunk_count++)+sizeof(mem_chunk));
    mem_chunk *c;

    if ( bytes > chunksize-sizeof(mem_chunk) )
      chunksize = tmp_nalloc(bytes+sizeof(mem_chunk));

    if ( (c=tmp_malloc(chunksize)) )
    { c->size    = chunksize-sizeof(mem_chunk);
      c->used    = ROUNDUP(bytes, sizeof(void*));
      c->prev    = mp->chunks;
      mp->chunks = c;
      ptr        = (char *)(mp->chunks+1);
    } else
      return NULL;
  }

#ifdef O_DEBUG
  assert((uintptr_t)ptr%sizeof(void*) == 0);
#endif

  return ptr;
}

static void
clear_mem_pool(mem_pool *mp)
{ mem_chunk *c, *p;

  for(c=mp->chunks; c != &mp->first; c=p)
  { p = c->prev;
    tmp_free(c);
  }
  mp->chunk_count = 1;
  mp->chunks      = &mp->first;
  mp->first.used  = 0;
}


		 /*******************************
		 *        FINDALL SUPPORT	*
		 *******************************/

#define FINDALL_MAGIC	0x37ac78fe

typedef struct findall_bag
{ struct findall_bag *parent;		/* parent bag */
  int		magic;			/* FINDALL_MAGIC */
  int		suspended;		/* Used for findnsols/4  */
  size_t	suspended_solutions;	/* Already handed out solutions */
  size_t	solutions;		/* count # solutions */
  size_t	gsize;			/* required size on stack */
  mem_pool	records;		/* stored records */
  segstack	answers;		/* list of answers */
  Record	answer_buf[64];		/* tmp space */
} findall_bag;

typedef struct findall_state
{ findall_bag  *bags;			/* Known bags  */
#if defined(O_ATOMGC) && defined(O_PLMT)
  simpleMutex   mutex;			/* Atom GC scanning synchronization */
#endif
  segstack	bag_stack;		/* For allocating bags  */
  findall_bag	buf[1];			/* Default bag */
} findall_state;

static
PRED_IMPL("$new_findall_bag", 0, new_findall_bag, PL_FA_SIG_ATOMIC)
{ PRED_LD
  findall_bag *bag;
  findall_state *state = LD->bags;

  if ( !state )
  { if ( !(state = PL_malloc(sizeof(*LD->bags))) )
      return PL_no_memory();
    state->bags = NULL;
    initSegStack(&state->bag_stack, sizeof(findall_bag),
		 sizeof(state->buf), state->buf);
#if defined(O_ATOMGC) && defined(O_PLMT)
    simpleMutexInit(&state->mutex);
#endif
    LD->bags = state;
  }

  bag = pushSegStack_(&state->bag_stack, NULL);
  if ( !bag )
    return PL_no_memory();

  bag->magic		   = FINDALL_MAGIC;
  bag->suspended	   = false;
  bag->suspended_solutions = 0;
  bag->solutions	   = 0;
  bag->gsize		   = 0;
  bag->parent		   = state->bags;
  init_mem_pool(&bag->records);
  initSegStack(&bag->answers, sizeof(Record),
	       sizeof(bag->answer_buf), bag->answer_buf);
  MEMORY_BARRIER();
  LD->bags->bags = bag;

  return true;
}


void
cleanup_bags(PL_local_data_t *ld)
{ findall_state *state;

  if ( (state=ld->bags) )
  { ld->bags = NULL;
    clearSegStack(&state->bag_stack);
#if defined(O_ATOMGC) && defined(O_PLMT)
    simpleMutexDelete(&state->mutex);
#endif
    PL_free(state);
  }
}


static void *
alloc_record(void *ctx, size_t bytes)
{ findall_bag *bag = ctx;

  return alloc_mem_pool(&bag->records, bytes);
}


#define current_bag(_) LDFUNC(current_bag, _)
static findall_bag *
current_bag(DECL_LD)
{ findall_bag *bag = LD->bags->bags;

  while(bag && bag->suspended)
  { assert(bag->parent);
    bag = bag->parent;
  }

  return bag;
}


#define add_findall_bag(term, count) LDFUNC(add_findall_bag, term, count)
static foreign_t
add_findall_bag(DECL_LD term_t term, term_t count)
{ findall_bag *bag = current_bag();
  Record r;

  DEBUG(MSG_NSOLS, Sdprintf("Adding answer to %p\n", bag););

  if ( !bag )
  { static atom_t cbag;

    if ( !cbag )
      cbag = PL_new_atom("findall-bag");

    return PL_error(NULL, 0, "continuation in findall/3 generator?",
		    ERR_PERMISSION, ATOM_append, cbag, term);
  }

  if ( !(r = compileTermToHeap_ex(term, alloc_record, bag, R_NOLOCK)) )
    return PL_no_memory();
  if ( !pushRecordSegStack(&bag->answers, r) )
    return PL_no_memory();
  bag->gsize += r->gsize;
  bag->solutions++;

  if ( bag->gsize + bag->solutions*3 > globalStackLimit()/sizeof(word) )
    return outOfStack(&LD->stacks.global, STACK_OVERFLOW_RAISE);

  if ( count )
    return PL_unify_int64(count, bag->solutions + bag->suspended_solutions);
  else
    return false;
}

static
PRED_IMPL("$add_findall_bag", 1, add_findall_bag, 0)
{ PRED_LD

  return add_findall_bag(A1, 0);
}

static
PRED_IMPL("$add_findall_bag", 2, add_findall_bag, 0)
{ PRED_LD

  return add_findall_bag(A1, A2);
}


static
PRED_IMPL("$collect_findall_bag", 2, collect_findall_bag, 0)
{ PRED_LD
  findall_bag *bag = current_bag();

  if ( bag->solutions )
  { size_t space = bag->gsize + bag->solutions*3;
    term_t list = PL_copy_term_ref(A2);
    term_t answer = PL_new_term_ref();
    Record *rp;
    int rc;

    if ( !hasGlobalSpace(space) )
    { if ( (rc=ensureGlobalSpace(space, ALLOW_GC)) != true )
	return raiseStackOverflow(rc);
    }

    while ( (rp=topOfSegStack(&bag->answers)) )
    { Record r = *rp;
      DEBUG(MSG_NSOLS, Sdprintf("Retrieving answer\n"));
      copyRecordToGlobal(answer, r, ALLOW_GC);
      if (GD->atoms.gc_active)
        markAtomsRecord(r);
      PL_cons_list(list, answer, list);
#ifdef O_ATOMGC
		/* see comment with scanSegStack() for synchronization details */
      if ( !quickPopTopOfSegStack(&bag->answers) )
      { simpleMutexLock(&LD->bags->mutex);
	popTopOfSegStack(&bag->answers);
	simpleMutexUnlock(&LD->bags->mutex);
      }
#else
      popTopOfSegStack(&bag->answers);
#endif
    }
    DEBUG(CHK_SECURE, assert(emptySegStack(&bag->answers)));

    return PL_unify(A1, list);
  } else
    return PL_unify(A1, A2);
}

/** '$suspend_findall_bag'

Used by findnsols/4,5. It is called after a complete chunk is delivered.
On first call it empties the chunk and   puts it in `suspended' mode. On
redo, the bag is re-enabled and we fail to force backtracking the goal.

This is a hack. An alternative would  be to pass bug-ids explicitly, but
earlier experiments showed a significant  performance gain for findall/3
and friends by keeping the  bag  implicit   because  there  is  no extra
argument we need to unify, extract from and verify the result.
*/

static
PRED_IMPL("$suspend_findall_bag", 0, suspend_findall_bag, PL_FA_NONDETERMINISTIC)
{ PRED_LD
  findall_bag *bag;

  switch( CTX_CNTRL )
  { case FRG_FIRST_CALL:
      bag = current_bag();
      simpleMutexLock(&LD->bags->mutex);
      clear_mem_pool(&bag->records);
      simpleMutexUnlock(&LD->bags->mutex);
      bag->suspended_solutions += bag->solutions;
      bag->solutions = 0;
      bag->gsize = 0;
      DEBUG(MSG_NSOLS, Sdprintf("Suspend %p\n", bag));
      bag->suspended = true;
      ForeignRedoPtr(bag);
    case FRG_REDO:
      bag = CTX_PTR;
      DEBUG(MSG_NSOLS, Sdprintf("Resume %p\n", bag));
      bag->suspended = false;
      return false;
    case FRG_CUTTED:
      bag = CTX_PTR;
      DEBUG(MSG_NSOLS, Sdprintf("! Resume %p\n", bag));
      bag->suspended = false;
      return true;
    default:
      assert(0);
      return false;
  }
}


static
PRED_IMPL("$destroy_findall_bag", 0, destroy_findall_bag, 0)
{ PRED_LD
  findall_bag *bag = LD->bags->bags;

  assert(bag);
  assert(bag->magic == FINDALL_MAGIC);
  assert(bag->suspended == false);

#ifdef O_ATOMGC
  simpleMutexLock(&LD->bags->mutex);
#endif
  LD->bags->bags = bag->parent;
#ifdef O_ATOMGC
  simpleMutexUnlock(&LD->bags->mutex);
#endif

  bag->magic = 0;
  clearSegStack(&bag->answers);
  clear_mem_pool(&bag->records);
  popSegStack_(&LD->bags->bag_stack, NULL);

  return true;
}


		 /*******************************
		 *	  ATOM-GC SUPPORT	*
		 *******************************/

static void
markAtomsAnswers(void *data)
{ Record r = *((Record*)data);

  markAtomsRecord(r);
}


void
markAtomsFindall(PL_local_data_t *ld)
{ findall_state *state;

  if ( (state=ld->bags) )
  { findall_bag *bag;

    simpleMutexLock(&state->mutex);
    bag = state->bags;
    for( ; bag; bag = bag->parent )
      scanSegStack(&bag->answers, markAtomsAnswers);
    simpleMutexUnlock(&state->mutex);
  }
}


		 /*******************************
		 *      PUBLISH PREDICATES	*
		 *******************************/

BeginPredDefs(bag)
  PRED_DEF("$new_findall_bag",     0, new_findall_bag,     0)
  PRED_DEF("$add_findall_bag",     1, add_findall_bag,     0)
  PRED_DEF("$add_findall_bag",     2, add_findall_bag,     0)
  PRED_DEF("$collect_findall_bag", 2, collect_findall_bag, 0)
  PRED_DEF("$destroy_findall_bag", 0, destroy_findall_bag, 0)
  PRED_DEF("$suspend_findall_bag", 0, suspend_findall_bag, PL_FA_NONDETERMINISTIC)
EndPredDefs
