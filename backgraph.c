/*
 * Copyright (c) 2001 by Hewlett-Packard Company. All rights reserved.
 *
 * THIS MATERIAL IS PROVIDED AS IS, WITH ABSOLUTELY NO WARRANTY EXPRESSED
 * OR IMPLIED.  ANY USE IS AT YOUR OWN RISK.
 *
 * Permission is hereby granted to use or copy this program
 * for any purpose,  provided the above notices are retained on all copies.
 * Permission to modify the code and to distribute modified code is granted,
 * provided the above notices are retained, and a notice that the code was
 * modified is included with the above copyright notice.
 *
 */

#include "private/dbg_mlc.h"

/*
 * This implements a full, though not well-tuned, representation of the
 * backwards points-to graph.  This is used to test for non-GC-robust
 * data structures; the code is not used during normal garbage collection.
 *
 * One restriction is that we drop all back-edges from nodes with very
 * high in-degree, and simply add them add them to a list of such
 * nodes.  They are then treated as permanent roots.  If this by itself
 * doesn't introduce a space leak, then such nodes can't contribute to
 * a growing space leak.
 */

#ifdef MAKE_BACK_GRAPH

#define MAX_IN  10      /* Maximum in-degree we handle directly */

#if (!defined(DBG_HDRS_ALL) || (ALIGNMENT != CPP_WORDSZ/8) \
     /* || !defined(UNIX_LIKE) */) && !defined(CPPCHECK)
# error The configuration does not support MAKE_BACK_GRAPH
#endif

/* We store single back pointers directly in the object's oh_bg_ptr field. */
/* If there is more than one ptr to an object, we store q | FLAG_MANY,     */
/* where q is a pointer to a back_edges object.                            */
/* Every once in a while we use a back_edges object even for a single      */
/* pointer, since we need the other fields in the back_edges structure to  */
/* be present in some fraction of the objects.  Otherwise we get serious   */
/* performance issues.                                                     */
#define FLAG_MANY 2

typedef struct back_edges_struct {
  word n_edges; /* Number of edges, including those in continuation     */
                /* structures.                                          */
  unsigned short flags;
#       define RETAIN 1 /* Directly points to a reachable object;       */
                        /* retain for next GC.                          */
  unsigned short height_gc_no;
                /* If height > 0, then the GC_gc_no value when it       */
                /* was computed.  If it was computed this cycle, then   */
                /* it is current.  If it was computed during the        */
                /* last cycle, then it represents the old height,       */
                /* which is only saved for live objects referenced by   */
                /* dead ones.  This may grow due to refs from newly     */
                /* dead objects.                                        */
  signed_word height;
                /* Longest path through unreachable nodes to this node  */
                /* that we found using depth first search.              */
# define HEIGHT_UNKNOWN      (-2)
# define HEIGHT_IN_PROGRESS  (-1)

  ptr_t edges[MAX_IN];
  struct back_edges_struct *cont;
                /* Pointer to continuation structure; we use only the   */
                /* edges field in the continuation.                     */
                /* also used as free list link.                         */
} back_edges;

/* Allocate a new back edge structure.  Should be more sophisticated    */
/* if this were production code.                                        */
#define MAX_BACK_EDGE_STRUCTS 100000
static back_edges *back_edge_space = 0;
STATIC int GC_n_back_edge_structs = 0;
                                /* Serves as pointer to never used      */
                                /* back_edges space.                    */
static back_edges *avail_back_edges = 0;
                                /* Pointer to free list of deallocated  */
                                /* back_edges structures.               */

static back_edges * new_back_edges(void)
{
  if (0 == back_edge_space) {
    size_t bytes_to_get = ROUNDUP_PAGESIZE_IF_MMAP(MAX_BACK_EDGE_STRUCTS
                                                   * sizeof(back_edges));

    GC_ASSERT(GC_page_size != 0);
    back_edge_space = (back_edges *)GET_MEM(bytes_to_get);
    if (NULL == back_edge_space)
      ABORT("Insufficient memory for back edges");
    GC_add_to_our_memory((ptr_t)back_edge_space, bytes_to_get);
  }
  if (0 != avail_back_edges) {
    back_edges * result = avail_back_edges;
    avail_back_edges = result -> cont;
    result -> cont = 0;
    return result;
  }
  if (GC_n_back_edge_structs >= MAX_BACK_EDGE_STRUCTS - 1) {
    ABORT("Needed too much space for back edges: adjust "
          "MAX_BACK_EDGE_STRUCTS");
  }
  return back_edge_space + (GC_n_back_edge_structs++);
}

/* Deallocate p and its associated continuation structures.     */
static void deallocate_back_edges(back_edges *p)
{
   back_edges *last = p;

   while (0 != last -> cont) last = last -> cont;
   last -> cont = avail_back_edges;
   avail_back_edges = p;
}

/* Table of objects that are currently on the depth-first search        */
/* stack.  Only objects with in-degree one are in this table.           */
/* Other objects are identified using HEIGHT_IN_PROGRESS.               */
/* FIXME: This data structure NEEDS IMPROVEMENT.                        */
#define INITIAL_IN_PROGRESS 10000
static ptr_t * in_progress_space = 0;
static size_t in_progress_size = 0;
static size_t n_in_progress = 0;

static void push_in_progress(ptr_t p)
{
  if (n_in_progress >= in_progress_size) {
    ptr_t * new_in_progress_space;

    GC_ASSERT(GC_page_size != 0);
    if (NULL == in_progress_space) {
      in_progress_size = ROUNDUP_PAGESIZE_IF_MMAP(INITIAL_IN_PROGRESS
                                                        * sizeof(ptr_t))
                                / sizeof(ptr_t);
      new_in_progress_space =
                        (ptr_t *)GET_MEM(in_progress_size * sizeof(ptr_t));
    } else {
      in_progress_size *= 2;
      new_in_progress_space = (ptr_t *)
                                GET_MEM(in_progress_size * sizeof(ptr_t));
      if (new_in_progress_space != NULL)
        BCOPY(in_progress_space, new_in_progress_space,
              n_in_progress * sizeof(ptr_t));
    }
    if (EXPECT(new_in_progress_space != NULL, TRUE))
      GC_add_to_our_memory((ptr_t)new_in_progress_space,
                           in_progress_size * sizeof(ptr_t));
#   ifndef GWW_VDB
      GC_scratch_recycle_no_gww(in_progress_space,
                                n_in_progress * sizeof(ptr_t));
#   elif defined(LINT2)
      /* TODO: implement GWW-aware recycling as in alloc_mark_stack */
      GC_noop1((word)in_progress_space);
#   endif
    in_progress_space = new_in_progress_space;
  }
  if (in_progress_space == 0)
      ABORT("MAKE_BACK_GRAPH: Out of in-progress space: "
            "Huge linear data structure?");
  in_progress_space[n_in_progress++] = p;
}

static GC_bool is_in_progress(ptr_t p)
{
  size_t i;
  for (i = 0; i < n_in_progress; ++i) {
    if (in_progress_space[i] == p) return TRUE;
  }
  return FALSE;
}

GC_INLINE void pop_in_progress(ptr_t p GC_ATTR_UNUSED)
{
  --n_in_progress;
  GC_ASSERT(in_progress_space[n_in_progress] == p);
}

#define GET_OH_BG_PTR(p) \
                (ptr_t)GC_REVEAL_POINTER(((oh *)(p)) -> oh_bg_ptr)
#define SET_OH_BG_PTR(p,q) (((oh *)(p)) -> oh_bg_ptr = GC_HIDE_POINTER(q))

/* Ensure that p has a back_edges structure associated with it. */
static void ensure_struct(ptr_t p)
{
  ptr_t old_back_ptr = GET_OH_BG_PTR(p);

  if (!((word)old_back_ptr & FLAG_MANY)) {
    back_edges *be = new_back_edges();
    be -> flags = 0;
    if (0 == old_back_ptr) {
      be -> n_edges = 0;
    } else {
      be -> n_edges = 1;
      be -> edges[0] = old_back_ptr;
    }
    be -> height = HEIGHT_UNKNOWN;
    be -> height_gc_no = (unsigned short)(GC_gc_no - 1);
    GC_ASSERT((word)be >= (word)back_edge_space);
    SET_OH_BG_PTR(p, (word)be | FLAG_MANY);
  }
}

/* Add the (forward) edge from p to q to the backward graph.  Both p    */
/* q are pointers to the object base, i.e. pointers to an oh.           */
static void add_edge(ptr_t p, ptr_t q)
{
    ptr_t pred = GET_OH_BG_PTR(q);
    back_edges * be, *be_cont;
    word i;

    GC_ASSERT(p == GC_base(p) && q == GC_base(q));
    if (!GC_HAS_DEBUG_INFO(q) || !GC_HAS_DEBUG_INFO(p)) {
      /* This is really a misinterpreted free list link, since we saw   */
      /* a pointer to a free list.  Don't overwrite it!                 */
      return;
    }
    if (NULL == pred) {
      static unsigned random_number = 13;
#     define GOT_LUCKY_NUMBER (((++random_number) & 0x7f) == 0)
        /* A not very random number we use to occasionally allocate a   */
        /* back_edges structure even for a single backward edge.  This  */
        /* prevents us from repeatedly tracing back through very long   */
        /* chains, since we will have some place to store height and    */
        /* in_progress flags along the way.                             */

        SET_OH_BG_PTR(q, p);
        if (GOT_LUCKY_NUMBER) ensure_struct(q);
        return;
    }

    /* Check whether it was already in the list of predecessors. */
    {
      back_edges *e = (back_edges *)((word)pred & ~FLAG_MANY);
      word n_edges;
      word total;
      int local = 0;

      if (((word)pred & FLAG_MANY) != 0) {
        n_edges = e -> n_edges;
      } else if (((word)COVERT_DATAFLOW(pred) & 1) == 0) {
        /* A misinterpreted freelist link.      */
        n_edges = 1;
        local = -1;
      } else {
        n_edges = 0;
      }
      for (total = 0; total < n_edges; ++total) {
        if (local == MAX_IN) {
          e = e -> cont;
          local = 0;
        }
        if (local >= 0)
          pred = e -> edges[local++];
        if (pred == p)
          return;
      }
    }

    ensure_struct(q);
    be = (back_edges *)((word)GET_OH_BG_PTR(q) & ~FLAG_MANY);
    for (i = be -> n_edges, be_cont = be; i > MAX_IN; i -= MAX_IN)
        be_cont = be_cont -> cont;
    if (i == MAX_IN) {
        be_cont -> cont = new_back_edges();
        be_cont = be_cont -> cont;
        i = 0;
    }
    be_cont -> edges[i] = p;
    be -> n_edges++;
#   ifdef DEBUG_PRINT_BIG_N_EDGES
      if (GC_print_stats == VERBOSE && be -> n_edges == 100) {
        GC_err_printf("The following object has big in-degree:\n");
        GC_print_heap_obj(q);
      }
#   endif
}

typedef void (*per_object_func)(ptr_t p, size_t n_bytes, word gc_descr);

static void per_object_helper(struct hblk *h, word fn)
{
  hdr * hhdr = HDR(h);
  size_t sz = (size_t)hhdr->hb_sz;
  word descr = hhdr -> hb_descr;
  per_object_func f = (per_object_func)fn;
  size_t i = 0;

  do {
    f((ptr_t)(h -> hb_body + i), sz, descr);
    i += sz;
  } while (i + sz <= BYTES_TO_WORDS(HBLKSIZE));
}

GC_INLINE void GC_apply_to_each_object(per_object_func f)
{
  GC_apply_to_all_blocks(per_object_helper, (word)f);
}

static void reset_back_edge(ptr_t p, size_t n_bytes GC_ATTR_UNUSED,
                            word gc_descr GC_ATTR_UNUSED)
{
  /* Skip any free list links, or dropped blocks */
  if (GC_HAS_DEBUG_INFO(p)) {
    ptr_t old_back_ptr = GET_OH_BG_PTR(p);
    if ((word)old_back_ptr & FLAG_MANY) {
      back_edges *be = (back_edges *)((word)old_back_ptr & ~FLAG_MANY);
      if (!(be -> flags & RETAIN)) {
        deallocate_back_edges(be);
        SET_OH_BG_PTR(p, 0);
      } else {

        GC_ASSERT(GC_is_marked(p));

        /* Back edges may point to objects that will not be retained.   */
        /* Delete them for now, but remember the height.                */
        /* Some will be added back at next GC.                          */
          be -> n_edges = 0;
          if (0 != be -> cont) {
            deallocate_back_edges(be -> cont);
            be -> cont = 0;
          }

        GC_ASSERT(GC_is_marked(p));

        /* We only retain things for one GC cycle at a time.            */
          be -> flags &= ~RETAIN;
      }
    } else /* Simple back pointer */ {
      /* Clear to avoid dangling pointer. */
      SET_OH_BG_PTR(p, 0);
    }
  }
}

static void add_back_edges(ptr_t p, size_t n_bytes, word gc_descr)
{
  ptr_t current_p = p + sizeof(oh);

  /* For now, fix up non-length descriptors conservatively.     */
    if((gc_descr & GC_DS_TAGS) != GC_DS_LENGTH) {
      gc_descr = n_bytes;
    }

  for (; (word)current_p < (word)(p + gc_descr); current_p += sizeof(word)) {
    word current;

    LOAD_WORD_OR_CONTINUE(current, current_p);
    FIXUP_POINTER(current);
    if (current >= (word)GC_least_plausible_heap_addr &&
        current <= (word)GC_greatest_plausible_heap_addr) {
       ptr_t target = (ptr_t)GC_base((void *)current);
       if (0 != target) {
         add_edge(p, target);
       }
    }
  }
}

/* Rebuild the representation of the backward reachability graph.       */
/* Does not examine mark bits.  Can be called before GC.                */
GC_INNER void GC_build_back_graph(void)
{
  GC_ASSERT(I_HOLD_LOCK());
  GC_apply_to_each_object(add_back_edges);
}

/* Return an approximation to the length of the longest simple path     */
/* through unreachable objects to p.  We refer to this as the height    */
/* of p.                                                                */
static word backwards_height(ptr_t p)
{
  word result;
  ptr_t pred = GET_OH_BG_PTR(p);
  back_edges *be;

  if (NULL == pred)
    return 1;
  if (((word)pred & FLAG_MANY) == 0) {
    if (is_in_progress(p)) return 0; /* DFS back edge, i.e. we followed */
                                     /* an edge to an object already    */
                                     /* on our stack: ignore            */
    push_in_progress(p);
    result = backwards_height(pred) + 1;
    pop_in_progress(p);
    return result;
  }
  be = (back_edges *)((word)pred & ~FLAG_MANY);
  if (be -> height >= 0 && be -> height_gc_no == (unsigned short)GC_gc_no)
      return be -> height;
  /* Ignore back edges in DFS */
    if (be -> height == HEIGHT_IN_PROGRESS) return 0;
  result = (be -> height > 0? be -> height : 1);
  be -> height = HEIGHT_IN_PROGRESS;

  {
      back_edges *e = be;
      word n_edges;
      word total;
      int local = 0;

      if (((word)pred & FLAG_MANY) != 0) {
        n_edges = e -> n_edges;
      } else if (((word)pred & 1) == 0) {
        /* A misinterpreted freelist link.      */
        n_edges = 1;
        local = -1;
      } else {
        n_edges = 0;
      }
      for (total = 0; total < n_edges; ++total) {
        word this_height;
        if (local == MAX_IN) {
          e = e -> cont;
          local = 0;
        }
        if (local >= 0)
          pred = e -> edges[local++];

        /* Execute the following once for each predecessor pred of p    */
        /* in the points-to graph.                                      */
        if (GC_is_marked(pred) && ((word)GET_OH_BG_PTR(p) & FLAG_MANY) == 0) {
          GC_COND_LOG_PRINTF("Found bogus pointer from %p to %p\n",
                             (void *)pred, (void *)p);
            /* Reachable object "points to" unreachable one.            */
            /* Could be caused by our lax treatment of GC descriptors.  */
          this_height = 1;
        } else {
          this_height = backwards_height(pred);
        }
        if (this_height >= result)
          result = this_height + 1;
      }
  }

  be -> height = result;
  be -> height_gc_no = (unsigned short)GC_gc_no;
  return result;
}

STATIC word GC_max_height = 0;
STATIC ptr_t GC_deepest_obj = NULL;

/* Compute the maximum height of every unreachable predecessor p of a   */
/* reachable object.  Arrange to save the heights of all such objects p */
/* so that they can be used in calculating the height of objects in the */
/* next GC.                                                             */
/* Set GC_max_height to be the maximum height we encounter, and         */
/* GC_deepest_obj to be the corresponding object.                       */
static void update_max_height(ptr_t p, size_t n_bytes GC_ATTR_UNUSED,
                              word gc_descr GC_ATTR_UNUSED)
{
  if (GC_is_marked(p) && GC_HAS_DEBUG_INFO(p)) {
    word p_height = 0;
    ptr_t p_deepest_obj = 0;
    ptr_t back_ptr;
    back_edges *be = 0;

    /* If we remembered a height last time, use it as a minimum.        */
    /* It may have increased due to newly unreachable chains pointing   */
    /* to p, but it can't have decreased.                               */
    back_ptr = GET_OH_BG_PTR(p);
    if (0 != back_ptr && ((word)back_ptr & FLAG_MANY)) {
      be = (back_edges *)((word)back_ptr & ~FLAG_MANY);
      if (be -> height != HEIGHT_UNKNOWN) p_height = be -> height;
    }

    {
      ptr_t pred = GET_OH_BG_PTR(p);
      back_edges *e = (back_edges *)((word)pred & ~FLAG_MANY);
      word n_edges;
      word total;
      int local = 0;

      if (((word)pred & FLAG_MANY) != 0) {
        n_edges = e -> n_edges;
      } else if (pred != NULL && ((word)pred & 1) == 0) {
        /* A misinterpreted freelist link.      */
        n_edges = 1;
        local = -1;
      } else {
        n_edges = 0;
      }
      for (total = 0; total < n_edges; ++total) {
        if (local == MAX_IN) {
          e = e -> cont;
          local = 0;
        }
        if (local >= 0)
          pred = e -> edges[local++];

        /* Execute the following once for each predecessor pred of p    */
        /* in the points-to graph.                                      */
        if (!GC_is_marked(pred) && GC_HAS_DEBUG_INFO(pred)) {
          word this_height = backwards_height(pred);
          if (this_height > p_height) {
            p_height = this_height;
            p_deepest_obj = pred;
          }
        }
      }
    }

    if (p_height > 0) {
      /* Remember the height for next time. */
        if (be == 0) {
          ensure_struct(p);
          back_ptr = GET_OH_BG_PTR(p);
          be = (back_edges *)((word)back_ptr & ~FLAG_MANY);
        }
        be -> flags |= RETAIN;
        be -> height = p_height;
        be -> height_gc_no = (unsigned short)GC_gc_no;
    }
    if (p_height > GC_max_height) {
        GC_max_height = p_height;
        GC_deepest_obj = p_deepest_obj;
    }
  }
}

STATIC word GC_max_max_height = 0;

GC_INNER void GC_traverse_back_graph(void)
{
  GC_ASSERT(I_HOLD_LOCK());
  GC_max_height = 0;
  GC_apply_to_each_object(update_max_height);
  if (0 != GC_deepest_obj)
    GC_set_mark_bit(GC_deepest_obj);  /* Keep it until we can print it. */
}

void GC_print_back_graph_stats(void)
{
  GC_ASSERT(I_HOLD_LOCK());
  GC_printf("Maximum backwards height of reachable objects"
            " at GC #%lu is %lu\n",
            (unsigned long)GC_gc_no, (unsigned long)GC_max_height);
  if (GC_max_height > GC_max_max_height) {
    ptr_t obj = GC_deepest_obj;

    GC_max_max_height = GC_max_height;
    UNLOCK();
    GC_err_printf(
            "The following unreachable object is last in a longest chain "
            "of unreachable objects:\n");
    GC_print_heap_obj(obj);
    LOCK();
  }
  GC_COND_LOG_PRINTF("Needed max total of %d back-edge structs\n",
                     GC_n_back_edge_structs);
  GC_apply_to_each_object(reset_back_edge);
  GC_deepest_obj = 0;
}

#endif /* MAKE_BACK_GRAPH */
