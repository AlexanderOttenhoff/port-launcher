/* A malloc for decomp game code whose heap must be part of a save state (docs/planning/SAVE_STATES.md).
 *
 * Why (2026-11-13, RP6 crash): Lighthouse replaced Banjo-Kazooie's own heap (bk_malloc over the static gHeapBase)
 * with the system calloc/free/realloc. Save states copy the game's sections (plm_game_section.h), so every object
 * the game allocated (particle emitters, actors, ...) lived OUTSIDE the state: loading restored pointers to objects
 * that had since been freed, and the game crashed in particleEmitter_update / scudo "invalid chunk state" on the
 * next frame. Routing the game's allocator through this arena (Lighthouse patch 0011) fixes that: the arena and
 * this allocator's own bookkeeping are static data compiled into the game section (module.cmake force-includes
 * plm_game_section.h into this file too), so a state captures the heap and its free list together, consistently.
 *
 * The allocator is deliberately simple (MVP): blocks laid out back to back in one static array, each with a 16-byte
 * header {size of this block | used bit, size of the previous block}; free blocks form one doubly linked list
 * (first fit, split on allocate, coalesce with both neighbours on free). O(free blocks) per allocation; BK has a
 * few thousand live objects at most, measured fine for now. Candidates for the hardening pass: size classes / TLSF,
 * and saving only the used part of the arena (it's copied whole today).
 *
 * Exhaustion: falls back to the system allocator and logs loudly. Such a block isn't in the state, so save states
 * become unreliable for the session; free/realloc tell the two kinds apart by address, so memory stays correct.
 * The high-water mark is logged on every save (plm_arena_log_usage) to size PLM_ARENA_SIZE from real play.
 */
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <android/log.h>

#ifndef PLM_ARENA_SIZE
/* BK's N64 heap was 2.1 MB with 4-byte pointers; the PC port's structs hold 8-byte pointers and it no longer
   evicts caches when memory runs low (that code went with the original heap), so leave plenty of room. Untouched
   pages cost nothing until used, but a save copies the whole range: ~32 MB per state. */
#define PLM_ARENA_SIZE (32u * 1024u * 1024u)
#endif

#define ALIGN 16u
#define HDR ((size_t)sizeof(struct block))
#define USED ((size_t)1)

struct block {
    size_t size;      /* whole block incl. header, multiple of ALIGN; low bit = used */
    size_t prev_size; /* whole size of the physically previous block, 0 for the first */
};
struct free_links {
    struct block* next;
    struct block* prev;
};

static _Alignas(16) unsigned char arena[PLM_ARENA_SIZE];
static struct block* free_head;
static int initialised;
static size_t in_use, peak, fallbacks;

static size_t bsize(const struct block* b) { return b->size & ~USED; }
static struct free_links* links(struct block* b) { return (struct free_links*)(b + 1); }
static struct block* next_block(struct block* b) {
    unsigned char* n = (unsigned char*)b + bsize(b);
    return n < arena + PLM_ARENA_SIZE ? (struct block*)n : NULL;
}
static struct block* prev_block(struct block* b) {
    return b->prev_size ? (struct block*)((unsigned char*)b - b->prev_size) : NULL;
}
static int in_arena(const void* p) {
    return (const unsigned char*)p >= arena && (const unsigned char*)p < arena + PLM_ARENA_SIZE;
}

static void list_remove(struct block* b) {
    struct free_links* l = links(b);
    if (l->prev) links(l->prev)->next = l->next; else free_head = l->next;
    if (l->next) links(l->next)->prev = l->prev;
}
static void list_push(struct block* b) {
    links(b)->prev = NULL;
    links(b)->next = free_head;
    if (free_head) links(free_head)->prev = b;
    free_head = b;
}

static void init(void) {
    struct block* b = (struct block*)arena;
    b->size = PLM_ARENA_SIZE;
    b->prev_size = 0;
    free_head = NULL;
    list_push(b);
    initialised = 1;
}

/* Smallest block that holds n bytes: header + payload rounded up, and room for the free-list links once freed. */
static size_t need_for(size_t n) {
    size_t need = (n + HDR + ALIGN - 1) & ~(size_t)(ALIGN - 1);
    return need < HDR + sizeof(struct free_links) ? ((HDR + sizeof(struct free_links) + ALIGN - 1) & ~(size_t)(ALIGN - 1)) : need;
}

/* Cuts b down to `need` bytes when the rest is big enough to be a block of its own; the rest goes on the free list. */
static void split(struct block* b, size_t need) {
    size_t total = bsize(b);
    if (total - need < need_for(1)) return;
    struct block* rest = (struct block*)((unsigned char*)b + need);
    rest->size = total - need;
    rest->prev_size = need;
    b->size = need | (b->size & USED);
    struct block* after = next_block(rest);
    // When realloc shrinks a used block, the block after it may already be free: merge, or the arena fragments into
    // runs of adjacent free blocks that are never joined (found by the host stress test, 2026-11-13).
    if (after && !(after->size & USED)) {
        list_remove(after);
        rest->size += bsize(after);
        after = next_block(rest);
    }
    if (after) after->prev_size = rest->size;
    list_push(rest);
}

static void* arena_malloc(size_t n) {
    if (!initialised) init();
    size_t need = need_for(n);
    for (struct block* b = free_head; b; b = links(b)->next) {
        if (bsize(b) < need) continue;
        list_remove(b);
        b->size |= USED;
        split(b, need);
        in_use += bsize(b);
        if (in_use > peak) peak = in_use;
        return b + 1;
    }
    if (fallbacks++ == 0) {
        __android_log_print(ANDROID_LOG_ERROR, "plm-savestate",
                            "game heap arena full (%zu MB): falling back to malloc, save states are unreliable now",
                            (size_t)(PLM_ARENA_SIZE >> 20));
    }
    return malloc(n ? n : 1);
}

static void* arena_calloc(size_t n) {
    void* p = arena_malloc(n);
    if (p && in_arena(p)) memset(p, 0, bsize((struct block*)p - 1) - HDR);
    else if (p) memset(p, 0, n);
    return p;
}

static void arena_free(void* p) {
    if (!p) return;
    if (!in_arena(p)) { free(p); return; } /* an exhaustion fallback block */
    struct block* b = (struct block*)p - 1;
    b->size &= ~USED;
    in_use -= bsize(b);
    struct block* n = next_block(b);
    if (n && !(n->size & USED)) { /* absorb the next block */
        list_remove(n);
        b->size += bsize(n);
    }
    struct block* pb = prev_block(b);
    if (pb && !(pb->size & USED)) { /* merge into the previous block */
        list_remove(pb);
        pb->size += bsize(b);
        b = pb;
    }
    struct block* after = next_block(b);
    if (after) after->prev_size = bsize(b);
    list_push(b);
}

static void* arena_realloc(void* p, size_t n) {
    if (!p) return arena_malloc(n);
    if (!in_arena(p)) return realloc(p, n ? n : 1);
    struct block* b = (struct block*)p - 1;
    size_t need = need_for(n);
    size_t have = bsize(b);
    if (have >= need) { /* shrink in place (n == 0 keeps a minimal block: BK reallocs arrays down to length 0) */
        in_use -= have;
        split(b, need);
        in_use += bsize(b);
        return p;
    }
    struct block* nx = next_block(b);
    if (nx && !(nx->size & USED) && have + bsize(nx) >= need) { /* grow into the free neighbour */
        list_remove(nx);
        in_use -= have;
        b->size += bsize(nx);
        struct block* after = next_block(b);
        if (after) after->prev_size = bsize(b);
        split(b, need);
        in_use += bsize(b);
        if (in_use > peak) peak = in_use;
        return p;
    }
    void* q = arena_malloc(n);
    if (!q) return NULL;
    memcpy(q, p, have - HDR < n ? have - HDR : n);
    arena_free(p);
    return q;
}

/* The game's entry points, serialised (why, 2026-11-13 BK load review): Lighthouse runs BK's other N64 threads as
   real OS threads, and some port code frees game allocations off the tick thread (Localization.cpp's font swap from
   the UI). System calloc/free were thread-safe; this allocator isn't, and a free-list race would corrupt the heap
   and surface as a crash anywhere, a load included. The lock lives OUTSIDE the game section (an explicit section
   attribute wins over the force-included #pragma clang section): a save copied while another thread held it would
   otherwise restore it locked, forever. Uncontended in practice, so a spinlock. */
static atomic_flag arena_lock __attribute__((section(".data.plm_arena_lock"))) = ATOMIC_FLAG_INIT;
static void lock(void) { while (atomic_flag_test_and_set_explicit(&arena_lock, memory_order_acquire)) {} }
static void unlock(void) { atomic_flag_clear_explicit(&arena_lock, memory_order_release); }

void* plm_arena_malloc(size_t n) { lock(); void* p = arena_malloc(n); unlock(); return p; }
void* plm_arena_calloc(size_t n) { lock(); void* p = arena_calloc(n); unlock(); return p; }
void plm_arena_free(void* p) { lock(); arena_free(p); unlock(); }
void* plm_arena_realloc(void* p, size_t n) { lock(); void* q = arena_realloc(p, n); unlock(); return q; }

/* plm_savestate holds the lock around its copies, so no allocation is half done in a state. */
void plm_arena_snapshot_begin(void) { lock(); }
void plm_arena_snapshot_end(void) { unlock(); }

/* Called by plm_savestate on every save: sizes PLM_ARENA_SIZE from real play (logcat tag plm-savestate). */
void plm_arena_log_usage(void) {
    __android_log_print(ANDROID_LOG_INFO, "plm-savestate", "game heap arena: %zu KB in use, peak %zu KB of %zu MB%s",
                        in_use >> 10, peak >> 10, (size_t)(PLM_ARENA_SIZE >> 20), fallbacks ? " (OVERFLOWED)" : "");
}
