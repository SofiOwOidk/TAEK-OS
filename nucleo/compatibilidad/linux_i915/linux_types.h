#ifndef LINUX_I915_TYPES_H
#define LINUX_I915_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef uint64_t phys_addr_t;
typedef uint64_t dma_addr_t;
typedef unsigned int gfp_t;

#ifndef GFP_KERNEL
#define GFP_KERNEL  0x01
#endif
#ifndef GFP_ATOMIC
#define GFP_ATOMIC  0x02
#endif
#ifndef GFP_DMA32
#define GFP_DMA32   0x04
#endif
#ifndef GFP_ZERO
#define GFP_ZERO    0x08
#endif

#define PAGE_SHIFT  12
#define PAGE_SIZE   (1UL << PAGE_SHIFT) // 4096 bytes
#define PAGE_MASK   (~(PAGE_SIZE - 1))

#define ENOMEM      12
#define EBUSY       16
#define EEXIST      17
#define EINVAL      22
#define EFAULT      14
#define ETIMEDOUT   110
#define EDEADLK     35
#define EIO         5
#define EOVERFLOW   75

// ----------------------------------------------------------------------------
// LIST HEAD (Núcleo de listas circulares doblemente enlazadas de Linux)
// ----------------------------------------------------------------------------
struct list_head {
    struct list_head *next, *prev;
};

static inline void INIT_LIST_HEAD(struct list_head *list) {
    list->next = list;
    list->prev = list;
}

static inline void __list_add(struct list_head *new_node,
                              struct list_head *prev,
                              struct list_head *next) {
    next->prev = new_node;
    new_node->next = next;
    new_node->prev = prev;
    prev->next = new_node;
}

static inline void list_add(struct list_head *new_node, struct list_head *head) {
    __list_add(new_node, head, head->next);
}

static inline void list_add_tail(struct list_head *new_node, struct list_head *head) {
    __list_add(new_node, head->prev, head);
}

static inline void __list_del(struct list_head *prev, struct list_head *next) {
    next->prev = prev;
    prev->next = next;
}

static inline void list_del(struct list_head *entry) {
    __list_del(entry->prev, entry->next);
    entry->next = NULL;
    entry->prev = NULL;
}

static inline void list_del_init(struct list_head *entry) {
    __list_del(entry->prev, entry->next);
    INIT_LIST_HEAD(entry);
}

static inline int list_empty(const struct list_head *head) {
    return head->next == head;
}

#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

#define list_entry(ptr, type, member) \
    container_of(ptr, type, member)

#define list_first_entry(ptr, type, member) \
    list_entry((ptr)->next, type, member)

#define list_for_each_entry(pos, head, member) \
    for (pos = list_first_entry(head, __typeof__(*pos), member); \
         &pos->member != (head); \
         pos = list_entry(pos->member.next, __typeof__(*pos), member))

#define list_for_each_entry_safe(pos, n, head, member) \
    for (pos = list_first_entry(head, __typeof__(*pos), member), \
         n = list_entry(pos->member.next, __typeof__(*pos), member); \
         &pos->member != (head); \
         pos = n, n = list_entry(n->member.next, __typeof__(*pos), member))

// ----------------------------------------------------------------------------
// ATOMIC OPS (System V ABI & C11 atomics)
// ----------------------------------------------------------------------------
typedef struct {
    volatile int counter;
} atomic_t;

#define ATOMIC_INIT(i) { (i) }

static inline int atomic_read(const atomic_t *v) {
    return __atomic_load_n(&v->counter, __ATOMIC_SEQ_CST);
}

static inline void atomic_set(atomic_t *v, int i) {
    __atomic_store_n(&v->counter, i, __ATOMIC_SEQ_CST);
}

static inline void atomic_inc(atomic_t *v) {
    __atomic_fetch_add(&v->counter, 1, __ATOMIC_SEQ_CST);
}

static inline void atomic_dec(atomic_t *v) {
    __atomic_fetch_sub(&v->counter, 1, __ATOMIC_SEQ_CST);
}

static inline int atomic_dec_and_test(atomic_t *v) {
    return (__atomic_sub_fetch(&v->counter, 1, __ATOMIC_SEQ_CST) == 0);
}

static inline int atomic_fetch_add(atomic_t *v, int i) {
    return __atomic_fetch_add(&v->counter, i, __ATOMIC_SEQ_CST);
}

static inline int atomic_fetch_sub(atomic_t *v, int i) {
    return __atomic_fetch_sub(&v->counter, i, __ATOMIC_SEQ_CST);
}

// ----------------------------------------------------------------------------
// SPINLOCK CON CONTROL DE INTERRUPCIONES (IF FLAG RFLAGS)
// ----------------------------------------------------------------------------
typedef struct {
    volatile int lock;
} spinlock_t;

#define SPIN_LOCK_UNLOCKED { 0 }

static inline void spin_lock_init(spinlock_t *lock) {
    __atomic_store_n(&lock->lock, 0, __ATOMIC_RELAXED);
}

static inline void spin_lock(spinlock_t *lock) {
    while (__atomic_exchange_n(&lock->lock, 1, __ATOMIC_ACQUIRE)) {
        __asm__ volatile ("pause");
    }
}

static inline void spin_unlock(spinlock_t *lock) {
    __atomic_store_n(&lock->lock, 0, __ATOMIC_RELEASE);
}

#define RFLAGS_IF_BIT (1UL << 9)

#if defined(__x86_64__) && !defined(TAEK_HOST_TEST)
#define spin_lock_irqsave(_lock, _flags) \
    do { \
        unsigned long __rflags; \
        __asm__ volatile ("pushfq; pop %0; cli" : "=r"(__rflags) :: "memory"); \
        (_flags) = __rflags; \
        while (__atomic_exchange_n(&((_lock)->lock), 1, __ATOMIC_ACQUIRE)) { \
            __asm__ volatile ("pause"); \
        } \
    } while (0)

#define spin_unlock_irqrestore(_lock, _flags) \
    do { \
        __atomic_store_n(&((_lock)->lock), 0, __ATOMIC_RELEASE); \
        if ((_flags) & RFLAGS_IF_BIT) { \
            __asm__ volatile ("sti" ::: "memory"); \
        } \
    } while (0)
#else
// Versión para pruebas host con estado de interrupción emulado en variable lvalue
#define spin_lock_irqsave(_lock, _flags) \
    do { \
        (_flags) = RFLAGS_IF_BIT; \
        while (__atomic_exchange_n(&((_lock)->lock), 1, __ATOMIC_ACQUIRE)) {} \
    } while (0)

#define spin_unlock_irqrestore(_lock, _flags) \
    do { \
        (void)(_flags); \
        __atomic_store_n(&((_lock)->lock), 0, __ATOMIC_RELEASE); \
    } while (0)
#endif

#endif // LINUX_I915_TYPES_H
