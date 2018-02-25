/*
Copyright: Boaz Segev, 2018
License: MIT

Feel free to copy, use and enjoy according to the license provided.
*/

#include "spnlock.inc"
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#if !FIO_FORCE_MALLOC

#include "fio_llist.h"
#include "fio_mem.h"

#if !defined(__clang__) && !defined(__GNUC__)
#define __thread _Thread_value
#endif

#undef malloc
#undef calloc
#undef free
#undef realloc

/* *****************************************************************************
Memory Copying by 16 byte units
***************************************************************************** */

#if __SIZEOF_INT128__ == 9 /* a 128bit type exists... but tests favor 64bit */
static inline void fio_memcpy(__uint128_t *__restrict dest,
                              __uint128_t *__restrict src, size_t units) {
  const uint8_t q = 1;
#elif SIZE_MAX == 0xFFFFFFFFFFFFFFFF /* 64 bit size_t */
static inline void fio_memcpy(size_t *__restrict dest, size_t *__restrict src,
                              size_t units) {
  const uint8_t q = 2;
#elif SIZE_MAX == 0xFFFFFFFF         /* 32 bit size_t */
static inline void fio_memcpy(size_t *__restrict dest, size_t *__restrict src,
                              size_t units) {
  const uint8_t q = 4;
#else                                /* unknow... assume 16 bit? */
static inline void fio_memcpy(uint16_t *__restrict dest,
                              uint16_t *__restrict src, size_t units) {
  const uint8_t q = 8;
#endif
  while (units) {
    switch (units) { /* unroll loop */
    default:
      dest[0] = src[0];
      dest[1] = src[1];
      dest[2] = src[2];
      dest[3] = src[3];
      dest[4] = src[4];
      dest[5] = src[5];
      dest[6] = src[6];
      dest[7] = src[7];
      dest[8] = src[8];
      dest[9] = src[9];
      dest[10] = src[10];
      dest[11] = src[11];
      dest[12] = src[12];
      dest[13] = src[13];
      dest[14] = src[14];
      dest[15] = src[15];
      units -= (16 / q);
      break;
    case 15: /* fallthrough */
      *(dest++) = *(src++);
    case 14: /* fallthrough */
      *(dest++) = *(src++);
    case 13: /* fallthrough */
      *(dest++) = *(src++);
    case 12: /* fallthrough */
      *(dest++) = *(src++);
    case 11: /* fallthrough */
      *(dest++) = *(src++);
    case 10: /* fallthrough */
      *(dest++) = *(src++);
    case 9: /* fallthrough */
      *(dest++) = *(src++);
    case 8: /* fallthrough */
      *(dest++) = *(src++);
    case 7: /* fallthrough */
      *(dest++) = *(src++);
    case 6: /* fallthrough */
      *(dest++) = *(src++);
    case 5: /* fallthrough */
      *(dest++) = *(src++);
    case 4: /* fallthrough */
      *(dest++) = *(src++);
    case 3: /* fallthrough */
      *(dest++) = *(src++);
    case 2: /* fallthrough */
      *(dest++) = *(src++);
    case 1: /* fallthrough */
      *(dest++) = *(src++);
      units = 0;
    }
  }
}

/* *****************************************************************************
System Memory wrappers
***************************************************************************** */

/*
 * allocates memory using `mmap`, but enforces block size alignment.
 * requires page aligned `len`.
 *
 * `align_shift` is used to move the memory page alignment to allow for a single
 * page allocation header. align_shift MUST be either 0 (normal) or 1 (single
 * page header). Other values might cause errors.
 */
static inline void *sys_alloc(size_t len, uint8_t is_indi) {
  void *result;
  static void *next_alloc = NULL;
/* hope for the best? */
#ifdef MAP_ALIGNED
  result = mmap(
      next_alloc, len, PROT_READ | PROT_WRITE | PROT_EXEC,
      MAP_PRIVATE | MAP_ANONYMOUS | MAP_ALIGNED(FIO_MEMORY_BLOCK_SIZE), -1, 0);
#else
  result = mmap(next_alloc, len, PROT_READ | PROT_WRITE | PROT_EXEC,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
  if (result == MAP_FAILED)
    return NULL;
  if (((uintptr_t)result & FIO_MEMORY_BLOCK_MASK)) {
    munmap(result, len);
    result = mmap(NULL, len + FIO_MEMORY_BLOCK_SIZE,
                  PROT_READ | PROT_WRITE | PROT_EXEC,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (result == MAP_FAILED)
      return NULL;
    const uintptr_t offset =
        (FIO_MEMORY_BLOCK_SIZE - ((uintptr_t)result & FIO_MEMORY_BLOCK_MASK));
    if (offset) {
      munmap(result, offset);
      result = (void *)((uintptr_t)result + offset);
    }
    munmap((void *)((uintptr_t)result + len), FIO_MEMORY_BLOCK_SIZE - offset);
  }
  next_alloc =
      (void *)((uintptr_t)result + FIO_MEMORY_BLOCK_SIZE +
               (is_indi * ((uintptr_t)1 << 30))); /* add 1TB for realloc */
  return result;
}

/* frees memory using `munmap`. requires exact, page aligned, `len` */
static inline void sys_free(void *mem, size_t len) { munmap(mem, len); }

static void *sys_realloc(void *mem, size_t prev_len, size_t new_len) {
  if (new_len > prev_len) {
#if defined(__linux__) && defined(MREMAP_MAYMOVE)
    void *result = mremap(mem, prev_len, new_len, MREMAP_MAYMOVE);
    if (result == MAP_FAILED)
      return NULL;
#else
    void *result = mmap((void *)((uintptr_t)mem + prev_len), new_len - prev_len,
                        PROT_READ | PROT_WRITE | PROT_EXEC,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (result == (void *)((uintptr_t)mem + prev_len)) {
      result = mem;
    } else {
      /* copy and free */
      munmap(result, new_len - prev_len); /* free the failed attempt */
      result = sys_alloc(new_len, 1);     /* allocate new memory */
      if (!result)
        return NULL;
      fio_memcpy(result, mem, prev_len >> 4); /* copy data */
      // memcpy(result, mem, prev_len);
      munmap(mem, prev_len); /* free original memory */
    }
#endif
    return result;
  }
  if (new_len + 4096 < prev_len) /* more than a single dangling page */
    munmap((void *)((uintptr_t)mem + new_len), prev_len - new_len);
  return mem;
}

static inline size_t sys_round_size(size_t size) {
  return (size & (~4095)) + (4096 * (!!(size & 4095)));
}

/* *****************************************************************************
Data Types
***************************************************************************** */

/* The basic block header. Starts a 64Kib memory block */
typedef struct block_s {
  uint16_t ref; /* reference count (per memory page) */
  uint16_t pos; /* position into the block */
  uint16_t max; /* available memory count */
  uint16_t pad; /* memory padding */
} block_s;

/* a per-CPU core "arena" for memory allocations  */
typedef struct {
  block_s *block;
  spn_lock_i lock;
} arena_s;

/* The memory allocators persistent state */
static struct {
  size_t active_size;      /* active array size */
  fio_ls_embd_s available; /* free list for memory blocks */
  intptr_t count;          /* free list counter */
  size_t cores;            /* the number of detected CPU cores*/
  spn_lock_i lock;         /* a global lock */
} memory = {
    .cores = 1,
    .available = FIO_LS_INIT(memory.available),
    .lock = SPN_LOCK_INIT,
};

/* The per-CPU arena array. */
static arena_s *arenas;

/* *****************************************************************************
Per-CPU Arena management
***************************************************************************** */

/* returned a locked arena. Attempts the preffered arena first. */
static inline arena_s *arena_lock(arena_s *preffered) {
  if (!preffered)
    preffered = arenas;
  if (!spn_trylock(&preffered->lock))
    return preffered;
  do {
    arena_s *arena = preffered;
    for (size_t i = (size_t)(arena - arenas); i < memory.cores; ++i) {
      if (arena != preffered && !spn_trylock(&arena->lock))
        return arena;
      ++arena;
    }
    if (preffered == arenas)
      reschedule_thread();
    preffered = arenas;
  } while (1);
}

static __thread arena_s *arena_last_used;

static void arena_enter(void) { arena_last_used = arena_lock(arena_last_used); }

static inline void arena_exit(void) { spn_unlock(&arena_last_used->lock); }

/* *****************************************************************************
Block management
***************************************************************************** */

// static inline block_s **block_find(void *mem_) {
//   const uintptr_t mem = (uintptr_t)mem_;
//   block_s *blk = memory.active;
// }

/* intializes the block header for an available block of memory. */
static inline block_s *block_init(void *blk_) {
  block_s *blk = blk_;
  *blk = (block_s){
      .ref = 1,
      .pos = (2 + (sizeof(block_s) >> 4)),
      .max = (FIO_MEMORY_BLOCK_SLICES - 1) -
             (sizeof(block_s) >> 4), /* count available units of 16 bytes */
  };
  return blk;
}

/* intializes the block header for an available block of memory. */
static inline void block_free(block_s *blk) {
  if (spn_sub(&blk->ref, 1))
    return;

  if (spn_add(&memory.count, 1) >
      (intptr_t)(FIO_MEM_MAX_BLOCKS_PER_CORE * memory.cores)) {
    /* TODO: return memory to the system */
    spn_sub(&memory.count, 1);
    sys_free(blk, FIO_MEMORY_BLOCK_SIZE);
    return;
  }
  memset(blk, 0, FIO_MEMORY_BLOCK_SIZE);
  spn_lock(&memory.lock);
  fio_ls_embd_push(&memory.available, (fio_ls_embd_s *)blk);
  spn_unlock(&memory.lock);
}

/* intializes the block header for an available block of memory. */
static inline block_s *block_new(void) {
  block_s *blk;
  spn_lock(&memory.lock);
  blk = (block_s *)fio_ls_embd_pop(&memory.available);
  spn_unlock(&memory.lock);
  if (blk) {
    spn_sub(&memory.count, 1);
    ((uintptr_t *)blk)[0] = 0;
    ((uintptr_t *)blk)[1] = 0;
    return block_init(blk);
  }
  /* TODO: collect memory from the system */
  blk = sys_alloc(FIO_MEMORY_BLOCK_SIZE, 0);
  if (!blk)
    return NULL;
  block_init(blk);
  return blk;
}

static inline void *block_slice(uint16_t units) {
  block_s *blk = arena_last_used->block;
  if (!blk) {
    /* arena is empty */
    blk = block_new();
    arena_last_used->block = blk;
  } else if (blk->pos + units > blk->max) {
    /* not enough memory in the block - rotate */
    block_free(blk);
    blk = block_new();
    arena_last_used->block = blk;
  }
  if (!blk) {
    /* no system memory available? */
    return NULL;
  }
  /* slice block starting at blk->pos and increase reference count */
  const void *mem = (void *)((uintptr_t)blk + ((uintptr_t)blk->pos << 4));
  spn_add(&blk->ref, 1);
  blk->pos += units;
  if (blk->pos >= blk->max) {
    /* it's true that a 16 bytes slice remains, but statistically... */
    /* ... the block was fully utilized, clear arena */
    block_free(blk);
    arena_last_used->block = NULL;
  }
  return (void *)mem;
}

static inline void block_slice_free(void *mem) {
  /* locate block boundary */
  block_s *blk = (block_s *)((uintptr_t)mem & (~FIO_MEMORY_BLOCK_MASK));
  block_free(blk);
}

/* *****************************************************************************
Non-Block allocations (direct from the system)
***************************************************************************** */

static inline void *big_alloc(size_t size) {
  size = sys_round_size(size + 16);
  size_t *mem = sys_alloc(size, 1);
  *mem = size;
  return (void *)(((uintptr_t)mem) + 16);
}

static inline void big_free(void *ptr) {
  size_t *mem = (void *)(((uintptr_t)ptr) - 16);
  sys_free(mem, *mem);
}

static inline void *big_realloc(void *ptr, size_t new_size) {
  size_t *mem = (void *)(((uintptr_t)ptr) - 16);
  new_size = sys_round_size(new_size + 16);
  mem = sys_realloc(mem, *mem, new_size);
  if (!mem)
    return NULL;
  *mem = new_size;
  return (void *)(((uintptr_t)mem) + 16);
}

/* *****************************************************************************
Library Initialization (initialize arenas and allocate a block for each CPU)
***************************************************************************** */

static void __attribute__((constructor)) fio_mem_init(void) {
  if (arenas)
    return;

#ifdef _SC_NPROCESSORS_ONLN
  ssize_t cpu_count = sysconf(_SC_NPROCESSORS_ONLN);
#else
#warning Dynamic CPU core count is unavailable - assuming 8 cores for memory allocation pools.
  ssize_t cpu_count = 8; /* fallback */
#endif
  memory.cores = cpu_count;
  memory.count = 0 - (intptr_t)cpu_count;
  arenas = big_alloc(sizeof(*arenas) * cpu_count);
  if (!arenas) {
    perror("FATAL ERROR: Couldn't initialize memory allocator");
    exit(errno);
  }
  size_t pre_pool = cpu_count > 32 ? 32 : cpu_count;
  for (size_t i = 0; i < pre_pool; ++i) {
    void *block = sys_alloc(FIO_MEMORY_BLOCK_SIZE, 0);
    if (block)
      fio_ls_embd_push(&memory.available, block);
  }
}

static void __attribute__((destructor)) fio_mem_destroy(void) {
  if (!arenas)
    return;

  arena_s *arena = arenas;
  for (size_t i = 0; i < memory.cores; ++i) {
    if (arena->block)
      block_free(arena->block);
    arena->block = NULL;
    ++arena;
  }
  block_s *b;
  while ((b = (void *)fio_ls_embd_pop(&memory.available))) {
    sys_free(b, FIO_MEMORY_BLOCK_SIZE);
  }
  big_free(arenas);
  arenas = NULL;
}

/* *****************************************************************************
Memory allocation / deacclocation API
***************************************************************************** */

void *fio_malloc(size_t size) {
  if (!size)
    return NULL;
  if (size >= FIO_MEMORY_BLOCK_ALLOC_LIMIT) {
    /* system allocation - must be block aligned */
    return big_alloc(size);
  }
  /* ceiling for 16 byte alignement, translated to 16 byte units */
  size = (size >> 4) + (!!(size & 15));
  arena_enter();
  void *mem = block_slice(size);
  arena_exit();
  return mem;
}

void *fio_calloc(size_t size, size_t count) {
  size = size * count;
  return fio_malloc(size); // memory is pre-initialized by mmap or pool.
}

void fio_free(void *ptr) {
  if (!ptr)
    return;
  if (((uintptr_t)ptr & FIO_MEMORY_BLOCK_MASK) == 16) {
    /* big allocation - direct from the system */
    big_free(ptr);
    return;
  }
  /* allocated within block */
  block_slice_free(ptr);
}

/**
 * Re-allocates memory. An attept to avoid copying the data is made only for
 * memory allocations larger than 64Kb.
 *
 * This variation is slightly faster as it might copy less data
 */
void *fio_realloc2(void *ptr, size_t original_size, size_t new_size) {
  if (!ptr)
    return fio_malloc(new_size);
  if (((uintptr_t)ptr & FIO_MEMORY_BLOCK_MASK) == 16) {
    /* big reallocation - direct from the system */
    return big_realloc(ptr, new_size);
  }
  /* allocated within block - don't even try to expand the allocation */
  /* ceiling for 16 byte alignement, translated to 16 byte units */
  void *new_mem = fio_malloc(new_size);
  if (!new_mem)
    return NULL;
  new_size = ((new_size >> 4) + (!!(new_size & 15))) << 4;
  original_size = ((original_size >> 4) + (!!(original_size & 15))) << 4;
  // memcpy(new_mem, ptr, (original_size > new_size ? new_size :
  //        original_size));
  fio_memcpy(new_mem, ptr,
             (original_size > new_size ? new_size : original_size) >> 4);
  block_slice_free(ptr);
  return new_mem;
}

void *fio_realloc(void *ptr, size_t new_size) {
  const size_t max_old =
      FIO_MEMORY_BLOCK_SIZE - ((uintptr_t)ptr & FIO_MEMORY_BLOCK_MASK);
  return fio_realloc2(ptr, max_old, new_size);
}

/* *****************************************************************************
FIO_OVERRIDE_MALLOC - override glibc / library malloc
***************************************************************************** */
#if FIO_OVERRIDE_MALLOC
void *malloc(size_t size) { return fio_malloc(size); }
void *calloc(size_t size, size_t count) { return fio_calloc(size, count); }
void free(void *ptr) { fio_free(ptr); }
void *realloc(void *ptr, size_t new_size) { return fio_realloc(ptr, new_size); }
#endif

/* *****************************************************************************
FIO_FORCE_MALLOC - use glibc / library malloc
***************************************************************************** */
#else

void *fio_malloc(size_t size) { return malloc(size); }

void *fio_calloc(size_t size, size_t count) { return calloc(size, count); }

void fio_free(void *ptr) { free(ptr); }

void *fio_realloc(void *ptr, size_t new_size) { return realloc(ptr, new_size); }
void *fio_realloc2(void *ptr, size_t old_size, size_t new_size) {
  return realloc(ptr, new_size);
  (void)old_size;
}

#endif

/* *****************************************************************************
Some Tests
***************************************************************************** */

#if DEBUG && !FIO_FORCE_MALLOC

void fio_malloc_test(void) {
#define TEST_ASSERT(cond, ...)                                                 \
  if (!(cond)) {                                                               \
    fprintf(stderr, "* " __VA_ARGS__);                                         \
    fprintf(stderr, "\nTesting failed.\n");                                    \
    exit(-1);                                                                  \
  }

  fprintf(stderr, "=== Testing facil.io memory allocator's system calls\n");
  char *mem = sys_alloc(FIO_MEMORY_BLOCK_SIZE, 0);
  TEST_ASSERT(mem, "sys_alloc failed to allocate memory!\n");
  TEST_ASSERT(!((uintptr_t)mem & FIO_MEMORY_BLOCK_MASK),
              "Memory allocation not aligned to FIO_MEMORY_BLOCK_SIZE!");
  mem[0] = 'a';
  mem[FIO_MEMORY_BLOCK_SIZE - 1] = 'z';
  fprintf(stderr, "* Testing reallocation from %p\n", (void *)mem);
  char *mem2 =
      sys_realloc(mem, FIO_MEMORY_BLOCK_SIZE, FIO_MEMORY_BLOCK_SIZE * 2);
  if (mem == mem2)
    fprintf(stderr, "* Performed system realloc without copy :-)\n");
  TEST_ASSERT(mem2[0] = 'a' && mem2[FIO_MEMORY_BLOCK_SIZE - 1] == 'z',
              "Reaclloc data was lost!");
  sys_free(mem2, FIO_MEMORY_BLOCK_SIZE * 2);
  fprintf(stderr, "=== Testing facil.io memory allocator's internal data.\n");
  TEST_ASSERT(arenas, "Missing arena data - library not initialized!");
  TEST_ASSERT(fio_malloc(0) == NULL, "fio_malloc 0 bytes should be NULL!\n");
  mem = fio_malloc(1);
  TEST_ASSERT(mem, "fio_malloc failed to allocate memory!\n");
  TEST_ASSERT(!((uintptr_t)mem & 15), "fio_malloc memory not aligned!\n");
  TEST_ASSERT(((uintptr_t)mem & FIO_MEMORY_BLOCK_MASK) != 16,
              "small fio_malloc memory indicates system allocation!\n");
  mem[0] = 'a';
  TEST_ASSERT(mem[0] == 'a', "allocate memory wasn't written to!\n");
  mem = fio_realloc(mem, 1);
  TEST_ASSERT(mem[0] == 'a', "fio_realloc memory wasn't copied!\n");
  TEST_ASSERT(arena_last_used, "arena_last_used wasn't initialized!\n");
  block_s *b = arena_last_used->block;
  size_t count = 2;
  do {
    mem2 = mem;
    mem = fio_malloc(1);
    fio_free(mem2); /* make sure we hold on to the block, so it rotates */
    TEST_ASSERT(mem, "fio_malloc failed to allocate memory!\n");
    TEST_ASSERT(!((uintptr_t)mem & 15),
                "fio_malloc memory not aligned at allocation #%zu!\n", count);
    TEST_ASSERT((((uintptr_t)mem & FIO_MEMORY_BLOCK_MASK) != 16),
                "fio_malloc memory indicates system allocation!\n");
    mem[0] = 'a';
    ++count;
  } while (arena_last_used->block == b);
  fio_free(mem);
  fprintf(stderr,
          "* Performed %zu allocations out of expected %zu allocations per "
          "block.\n",
          count,
          (size_t)((FIO_MEMORY_BLOCK_SLICES - 2) - (sizeof(block_s) >> 4) - 1));
  TEST_ASSERT(fio_ls_embd_any(&memory.available),
              "memory pool empty (memory block wasn't freed)!\n");
  TEST_ASSERT(memory.count, "memory.count == 0 (memory block not counted)!\n");
  mem = fio_calloc(FIO_MEMORY_BLOCK_ALLOC_LIMIT - 64, 1);
  TEST_ASSERT(mem,
              "failed to allocate FIO_MEMORY_BLOCK_ALLOC_LIMIT - 64 bytes!\n");
  TEST_ASSERT(((uintptr_t)mem & FIO_MEMORY_BLOCK_MASK) != 16,
              "fio_calloc (under limit) memory alignment error!\n");
  mem2 = fio_malloc(1);
  TEST_ASSERT(mem2, "fio_malloc(1) failed to allocate memory!\n");
  mem2[0] = 'a';
  fio_free(mem2);
  for (uintptr_t i = 0; i < (FIO_MEMORY_BLOCK_ALLOC_LIMIT - 64); ++i) {
    TEST_ASSERT(mem[i] == 0,
                "calloc returned memory that wasn't initialized?!\n");
  }
  fio_free(mem);

  mem = fio_malloc(FIO_MEMORY_BLOCK_SIZE);
  TEST_ASSERT(mem, "fio_malloc failed to FIO_MEMORY_BLOCK_SIZE bytes!\n");
  TEST_ASSERT(((uintptr_t)mem & FIO_MEMORY_BLOCK_MASK) == 16,
              "fio_malloc (big) memory isn't aligned!\n");
  mem = fio_realloc(mem, FIO_MEMORY_BLOCK_SIZE * 2);
  TEST_ASSERT(mem,
              "fio_realloc (big) failed on FIO_MEMORY_BLOCK_SIZE X2 bytes!\n");
  fio_free(mem);
  TEST_ASSERT(((uintptr_t)mem & FIO_MEMORY_BLOCK_MASK) == 16,
              "fio_realloc (big) memory isn't aligned!\n");

  fprintf(stderr, "* passed.\n");
}

#else

void fio_malloc_test(void) {}

#endif
