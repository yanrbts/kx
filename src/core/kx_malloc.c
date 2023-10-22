/* zmalloc - total amount of allocated memory aware version of malloc()
 *
 * Copyright (c) 2023-2023, yanruibing <yanruibinghxu at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include "kx_malloc.h"

#ifdef HAVE_MALLOC_SIZE
#define PREFIX_SIZE (0)
#else
#if defined(__sun) || defined(__sparc) || defined(__sparc__)
#define PREFIX_SIZE (sizeof(long long))
#else
#define PREFIX_SIZE (sizeof(size_t))
#endif
#endif

/* Explicitly override malloc/free etc when using tcmalloc. */
#if defined(USE_TCMALLOC)
#define malloc(size) tc_malloc(size)
#define calloc(count,size) tc_calloc(count,size)
#define realloc(ptr,size) tc_realloc(ptr,size)
#define free(ptr) tc_free(ptr)
#elif defined(USE_JEMALLOC)
#define malloc(size) je_malloc(size)
#define calloc(count,size) je_calloc(count,size)
#define realloc(ptr,size) je_realloc(ptr,size)
#define free(ptr) je_free(ptr)
#endif

#ifdef HAVE_ATOMIC
#define update_kxmalloc_stat_add(__n) __sync_add_and_fetch(&used_memory, (__n))
#define update_kxmalloc_stat_sub(__n) __sync_sub_and_fetch(&used_memory, (__n))
#else
#define update_kxmalloc_stat_add(__n) do {      \
    pthread_mutex_lock(&used_memory_mutex);     \
    used_memory += (__n);                       \
    pthread_mutex_unlock(&used_memory_mutex);   \
} while(0)

#define update_kxmalloc_stat_sub(__n) do {      \
    pthread_mutex_lock(&used_memory_mutex);     \
    used_memory -= (__n);                       \
    pthread_mutex_unlock(&used_memory_mutex);   \
} while(0)
#endif

#define update_kxmalloc_stat_alloc(__n,__size) do {                         \
    size_t _n = (__n);                                                      \
    if (_n & (sizeof(long)-1)) _n += sizeof(long) - (_n & sizeof(long)-1);  \
    if (kmalloc_thread_safe) {                                              \
        update_kxmalloc_stat_add(_n);                                       \
    } else {                                                                \
        used_memory += _n;                                                  \
    }                                                                       \
} while(0)

#define update_kxmalloc_stat_free(__n) do {                                 \
    size_t _n = (__n);                                                      \
    if (_n & (sizeof(long)-1)) _n += sizeof(long) - (_n & (sizeof(long)-1));\
    if (kmalloc_thread_safe) {                                              \
        update_kxmalloc_stat_sub(_n);                                       \
    } else {                                                                \
        used_memory -= _n;                                                  \
    }                                                                       \
} while(0)

static size_t used_memory = 0;
static int kmalloc_thread_safe = 0;
pthread_mutex_t used_memory_mutex = PTHREAD_MUTEX_INITIALIZER;

static void kxmalloc_default_oom(size_t size) {
    fprintf(stderr, "kxmalloc: Out of memory trying to allocate %zu bytes\n", size);
    fflush(stderr);
    abort();
}

static void (*kmalloc_oom_handler)(size_t) = kxmalloc_default_oom;

void *kxmalloc(size_t size) {
    void *ptr = malloc(size + PREFIX_SIZE);

    if (!ptr) kmalloc_oom_handler(size);
#ifdef HAVE_MALLOC_SIZE
    update_kxmalloc_stat_alloc(kxmalloc_size(ptr), size);
    return ptr;
#else
    *((size_t*)ptr) = size;
    update_kxmalloc_stat_alloc(size + PREFIX_SIZE, size);
    return (char*)ptr+PREFIX_SIZE;
#endif
}

void *kxcalloc(size_t size) {
    void *ptr = calloc(1, size + PREFIX_SIZE);

    if (!ptr) kmalloc_oom_handler(size);
#ifdef HAVE_MALLOC_SIZE
    update_kxmalloc_stat_alloc(kxmalloc_size(ptr), size);
    return ptr;
#else
    *((size_t*)ptr) = size;
    update_kxmalloc_stat_alloc(size + PREFIX_SIZE, size);
    return (char*)ptr+PREFIX_SIZE;
#endif
}

void *kxrealloc(void *ptr, size_t size) {
#ifndef HAVE_MALLOC_SIZE
    void *realptr;
#endif
    size_t oldsize;
    void *newptr;

    if (ptr == NULL) return kxmalloc(size);
#ifdef HAVE_MALLOC_SIZE
    oldsize = kxmalloc_size(ptr);
    newptr = realloc(ptr, size);
    if (!newptr) kmalloc_oom_handler(size);

    update_kxmalloc_stat_free(oldsize);
    update_kxmalloc_stat_alloc(kxmalloc_size(newptr), size);
    return newptr;
#else
    realptr = (char*)ptr-PREFIX_SIZE;
    oldsize = *((size_t*)realptr);
    newptr = realloc(realptr, size+PREFIX_SIZE);
    if (!newptr) kmalloc_oom_handler(size);

    *((size_t*)newptr) = size;
    update_kxmalloc_stat_free(oldsize);
    update_kxmalloc_stat_alloc(size,size);
    return (char*)newptr+PREFIX_SIZE;
#endif
}

/* Provide kxmalloc_size() for systems where this function is not provided by
 * malloc itself, given that in that case we store an header with this
 * information as the first bytes of every allocation. */
#ifndef HAVE_MALLOC_SIZE
size_t kxmalloc_size(void *ptr) {
    void *realptr = (char*)ptr-PREFIX_SIZE;
    size_t size = *((size_t*)realptr);
    /* Assume at least that all the allocations are padded at sizeof(long) by
     * the underlying allocator. */
    if (size & (sizeof(long)-1)) size += sizeof(long) - (size & (sizeof(long)-1));
    return size+PREFIX_SIZE;
}
#endif

void kxfree(void *ptr) {
#ifndef HAVE_MALLOC_SIZE
    void *realptr;
    size_t oldsize;
#endif

    if (ptr == NULL) return;
#ifdef HAVE_MALLOC_SIZE
    update_kxmalloc_stat_free(zmalloc_size(ptr));
    free(ptr);
#else
    realptr = (char*)ptr-PREFIX_SIZE;
    oldsize = *((size_t*)realptr);
    update_kxmalloc_stat_free(oldsize+PREFIX_SIZE);
    free(realptr);
#endif
}

size_t kxmalloc_used_memory(void) {
    size_t um;

    if (kmalloc_thread_safe) {
#ifdef HAVE_ATOMIC
        um = __sync_add_and_fetch(&used_memory, 0);
#else
        pthread_mutex_lock(&used_memory_mutex);
        um = used_memory;
        pthread_mutex_unlock(&used_memory_mutex);
#endif
    } else {
        um = used_memory;
    }
    return um;
}

void kxmalloc_enable_thread_safeness(void) {
    kmalloc_thread_safe = 1;
}

void kxmalloc_set_oom_handler(void (*oom_handler)(size_t)) {
    kmalloc_oom_handler = oom_handler;
}