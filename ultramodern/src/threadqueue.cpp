#include <cassert>
#include <cstdio>

#include "ultramodern/ultramodern.hpp"

static PTR(OSThread) running_queue_impl = NULLPTR;

static inline bool valid_rdram_ptr(int32_t ptr) {
    if (ptr == 0) return false;
    uint64_t off = (uint64_t)ptr - 0xFFFFFFFF80000000ULL;
    return off < 0x01000000ULL;
}

static PTR(OSThread)* queue_to_ptr(RDRAM_ARG PTR(PTR(OSThread)) queue) {
    if (queue == ultramodern::running_queue) {
        return &running_queue_impl;
    }
    return TO_PTR(PTR(OSThread), queue);
}

void ultramodern::thread_queue_insert(RDRAM_ARG PTR(PTR(OSThread)) queue_, PTR(OSThread) toadd_) {
    PTR(OSThread)* cur = queue_to_ptr(PASS_RDRAM queue_);
    if (!valid_rdram_ptr(toadd_)) return;
    if (*cur != NULLPTR && !valid_rdram_ptr(*cur)) {
        *cur = NULLPTR;
    }
    OSThread* toadd = TO_PTR(OSThread, toadd_);
    debug_printf("[Thread Queue] Inserting thread %d into queue 0x%08X\n", toadd->id, (uintptr_t)queue_);
    int limit = 64;
    while (*cur && --limit > 0) {
        if (!valid_rdram_ptr(*cur)) {
            *cur = NULLPTR;
            break;
        }
        if (TO_PTR(OSThread, *cur)->priority <= toadd->priority) break;
        cur = &TO_PTR(OSThread, *cur)->next;
    }
    toadd->next = (*cur);
    toadd->queue = queue_;
    *cur = toadd_;
}

PTR(OSThread) ultramodern::thread_queue_pop(RDRAM_ARG PTR(PTR(OSThread)) queue_) {
    PTR(OSThread)* queue = queue_to_ptr(PASS_RDRAM queue_);
    PTR(OSThread) ret = *queue;
    if (ret == NULLPTR) {
        return NULLPTR;
    }
    if (!valid_rdram_ptr(ret)) {
        fprintf(stderr, "[Thread Queue] CORRUPTION: queue head 0x%08X outside RDRAM, clearing\n",
                (uint32_t)ret);
        *queue = NULLPTR;
        return NULLPTR;
    }
    *queue = TO_PTR(OSThread, ret)->next;
    TO_PTR(OSThread, ret)->queue = NULLPTR;
    debug_printf("[Thread Queue] Popped thread %d from queue 0x%08X\n", TO_PTR(OSThread, ret)->id, (uintptr_t)queue_);
    return ret;
}

bool ultramodern::thread_queue_remove(RDRAM_ARG PTR(PTR(OSThread)) queue_, PTR(OSThread) t_) {
    if (!valid_rdram_ptr(t_)) return false;
    debug_printf("[Thread Queue] Removing thread %d from queue 0x%08X\n", TO_PTR(OSThread, t_)->id, (uintptr_t)queue_);

    PTR(OSThread)* cur_ptr = queue_to_ptr(PASS_RDRAM queue_);
    int limit = 64;
    while (*cur_ptr != NULLPTR && --limit > 0) {
        if (!valid_rdram_ptr(*cur_ptr)) {
            *cur_ptr = NULLPTR;
            return false;
        }
        if (*cur_ptr == t_) {
            *cur_ptr = TO_PTR(OSThread, *cur_ptr)->next;
            return true;
        }
        cur_ptr = &TO_PTR(OSThread, *cur_ptr)->next;
    }

    return false;
}

bool ultramodern::thread_queue_empty(RDRAM_ARG PTR(PTR(OSThread)) queue_) {
    PTR(OSThread)* queue = queue_to_ptr(PASS_RDRAM queue_);
    PTR(OSThread) head = *queue;
    if (head == NULLPTR) return true;
    if (!valid_rdram_ptr(head)) {
        fprintf(stderr, "[Thread Queue] CORRUPTION: queue head 0x%08X outside RDRAM, clearing\n",
                (uint32_t)head);
        *queue = NULLPTR;
        return true;
    }
    return false;
}

PTR(OSThread) ultramodern::thread_queue_peek(RDRAM_ARG PTR(PTR(OSThread)) queue_) {
    PTR(OSThread)* queue = queue_to_ptr(PASS_RDRAM queue_);
    return *queue;
}
