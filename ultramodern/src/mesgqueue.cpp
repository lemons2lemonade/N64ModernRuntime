#include <bitset>
#include <cstdio>
#include <unordered_map>
#include <vector>
#include <mutex>

#include "blockingconcurrentqueue.h"

#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"

// Host-side shadow queue — immune to RDRAM corruption.
// The game's BSS can overwrite the RDRAM-resident OSMesgQueue with garbage;
// we keep the real state here and the RDRAM copy is never read back.
struct HostMQ {
    std::vector<OSMesg> buf;
    s32 msgCount;
    s32 validCount;
    s32 first;
    PTR(OSMesg) rdram_msg;
    PTR(OSThread) blocked_on_recv;
    PTR(OSThread) blocked_on_send;
};

static std::mutex hmq_mutex;
static std::unordered_map<int32_t, HostMQ> host_queues;

static HostMQ* get_hmq(int32_t mq_) {
    auto it = host_queues.find(mq_);
    if (it != host_queues.end()) return &it->second;
    return nullptr;
}

static std::mutex affinity_mutex;
static std::unordered_map<std::thread::id, int32_t> send_affinity;
static std::unordered_map<std::thread::id, int32_t> recv_affinity;

static void record_send_affinity(int32_t mq_) {
    std::lock_guard<std::mutex> lock(affinity_mutex);
    send_affinity[std::this_thread::get_id()] = mq_;
}

static void record_recv_affinity(int32_t mq_) {
    std::lock_guard<std::mutex> lock(affinity_mutex);
    recv_affinity[std::this_thread::get_id()] = mq_;
}

static int32_t recover_send_mq(int32_t bad_mq) {
    std::lock_guard<std::mutex> lock(affinity_mutex);
    auto it = send_affinity.find(std::this_thread::get_id());
    if (it != send_affinity.end() && get_hmq(it->second)) {
        static int s_log = 0;
        if (s_log++ < 20)
            std::fprintf(stderr, "[MQ] RECOVERED send mq=0x%08X -> 0x%08X (thread affinity)\n",
                         (uint32_t)bad_mq, (uint32_t)it->second);
        return it->second;
    }
    return 0;
}

static int32_t recover_recv_mq(int32_t bad_mq) {
    std::lock_guard<std::mutex> lock(affinity_mutex);
    auto it = recv_affinity.find(std::this_thread::get_id());
    if (it != recv_affinity.end() && get_hmq(it->second)) {
        static int s_log = 0;
        if (s_log++ < 20)
            std::fprintf(stderr, "[MQ] RECOVERED recv mq=0x%08X -> 0x%08X (thread affinity)\n",
                         (uint32_t)bad_mq, (uint32_t)it->second);
        return it->second;
    }
    return 0;
}

static inline bool valid_mq_ptr(int32_t ptr) {
    if (ptr == 0) return false;
    uint64_t off = (uint64_t)ptr - 0xFFFFFFFF80000000ULL;
    return off < 0x01000000ULL;
}

static void host_tq_insert(RDRAM_ARG PTR(OSThread)* head, PTR(OSThread) toadd_) {
    if (!valid_mq_ptr(toadd_)) return;
    OSThread* toadd = TO_PTR(OSThread, toadd_);
    PTR(OSThread)* cur = head;
    int limit = 64;
    while (*cur && --limit > 0) {
        if (!valid_mq_ptr(*cur)) { *cur = NULLPTR; break; }
        if (TO_PTR(OSThread, *cur)->priority <= toadd->priority) break;
        cur = &TO_PTR(OSThread, *cur)->next;
    }
    toadd->next = *cur;
    *cur = toadd_;
}

static PTR(OSThread) host_tq_pop(RDRAM_ARG PTR(OSThread)* head) {
    PTR(OSThread) ret = *head;
    if (ret == NULLPTR) return NULLPTR;
    if (!valid_mq_ptr(ret)) { *head = NULLPTR; return NULLPTR; }
    *head = TO_PTR(OSThread, ret)->next;
    TO_PTR(OSThread, ret)->queue = NULLPTR;
    return ret;
}

static bool host_tq_empty(PTR(OSThread)* head) {
    PTR(OSThread) h = *head;
    if (h == NULLPTR) return true;
    if (!valid_mq_ptr(h)) { *head = NULLPTR; return true; }
    return false;
}

struct QueuedMessage {
    PTR(OSMesgQueue) mq;
    OSMesg mesg;
    bool jam;
    bool requeue_if_blocked;
};

static moodycamel::BlockingConcurrentQueue<QueuedMessage> external_messages {};
std::bitset<32> requeue_enabled;

void ultramodern::set_message_queue_control(const ultramodern::MessageQueueControl& mqc) {
    requeue_enabled.reset();
    requeue_enabled.set(static_cast<int>(EventMessageSource::Timer), mqc.requeue_timer);
    requeue_enabled.set(static_cast<int>(EventMessageSource::Sp), mqc.requeue_sp);
    requeue_enabled.set(static_cast<int>(EventMessageSource::Si), mqc.requeue_si);
    requeue_enabled.set(static_cast<int>(EventMessageSource::Ai), mqc.requeue_ai);
    requeue_enabled.set(static_cast<int>(EventMessageSource::Vi), mqc.requeue_vi);
    requeue_enabled.set(static_cast<int>(EventMessageSource::Pi), mqc.requeue_pi);
    requeue_enabled.set(static_cast<int>(EventMessageSource::Dp), mqc.requeue_dp);
}

void ultramodern::enqueue_external_message_src(PTR(OSMesgQueue) mq, OSMesg msg, bool jam, EventMessageSource src) {
    external_messages.enqueue({mq, msg, jam, requeue_enabled[static_cast<int>(src)]});
}

void ultramodern::enqueue_external_message(PTR(OSMesgQueue) mq, OSMesg msg, bool jam, bool requeue_if_blocked) {
    external_messages.enqueue({mq, msg, jam, requeue_if_blocked});
}

bool do_send(RDRAM_ARG PTR(OSMesgQueue) mq_, OSMesg msg, bool jam, bool block);

void dequeue_external_messages(RDRAM_ARG1) {
    QueuedMessage to_send;
    std::vector<QueuedMessage> requeued_messages{};
    while (external_messages.try_dequeue(to_send)) {
        if (!do_send(PASS_RDRAM to_send.mq, to_send.mesg, to_send.jam, false) && to_send.requeue_if_blocked) {
            requeued_messages.push_back(to_send);
        }
    }
    for (QueuedMessage& cur_mesg : requeued_messages) {
        external_messages.enqueue(cur_mesg);
    }
}

void ultramodern::wait_for_external_message(RDRAM_ARG1) {
    QueuedMessage to_send;
    external_messages.wait_dequeue(to_send);
    if (!do_send(PASS_RDRAM to_send.mq, to_send.mesg, to_send.jam, false) && to_send.requeue_if_blocked) {
        external_messages.enqueue(to_send);
    }
}

void ultramodern::wait_for_external_message_timed(RDRAM_ARG u32 millis) {
    QueuedMessage to_send;
    if (external_messages.wait_dequeue_timed(to_send, std::chrono::milliseconds{millis})) {
        if (!do_send(PASS_RDRAM to_send.mq, to_send.mesg, to_send.jam, false) && to_send.requeue_if_blocked) {
            external_messages.enqueue(to_send);
        }
    }
}

extern "C" void osCreateMesgQueue(RDRAM_ARG PTR(OSMesgQueue) mq_, PTR(OSMesg) msg, s32 count) {
    OSMesgQueue *mq = TO_PTR(OSMesgQueue, mq_);
    mq->blocked_on_recv = NULLPTR;
    mq->blocked_on_send = NULLPTR;
    mq->msgCount = count;
    mq->msg = msg;
    mq->validCount = 0;
    mq->first = 0;

    std::lock_guard<std::mutex> lock(hmq_mutex);
    HostMQ& hmq = host_queues[mq_];
    hmq.buf.resize(count);
    hmq.msgCount = count;
    hmq.validCount = 0;
    hmq.first = 0;
    hmq.rdram_msg = msg;
    hmq.blocked_on_recv = NULLPTR;
    hmq.blocked_on_send = NULLPTR;
}

bool do_send(RDRAM_ARG PTR(OSMesgQueue) mq_, OSMesg msg, bool jam, bool block) {
    if (!valid_mq_ptr(mq_)) {
        return false;
    }

    HostMQ* hmq = get_hmq(mq_);
    if (!hmq) {
        return false;
    }

    if (!block) {
        if (hmq->validCount >= hmq->msgCount) {
            return false;
        }
    }
    else {
        while (hmq->validCount >= hmq->msgCount) {
            debug_printf("[Message Queue] Thread %d is blocked on send\n", TO_PTR(OSThread, ultramodern::this_thread())->id);
            host_tq_insert(PASS_RDRAM &hmq->blocked_on_send, ultramodern::this_thread());
            ultramodern::run_next_thread_and_wait(PASS_RDRAM1);
        }
    }

    if (jam) {
        hmq->first = (hmq->first + hmq->msgCount - 1) % hmq->msgCount;
        hmq->buf[hmq->first] = msg;
        hmq->validCount++;
    }
    else {
        s32 last = (hmq->first + hmq->validCount) % hmq->msgCount;
        hmq->buf[last] = msg;
        hmq->validCount++;
    }

    if (!host_tq_empty(&hmq->blocked_on_recv)) {
        ultramodern::schedule_running_thread(PASS_RDRAM host_tq_pop(PASS_RDRAM &hmq->blocked_on_recv));
    }

    return true;
}

bool do_recv(RDRAM_ARG PTR(OSMesgQueue) mq_, PTR(OSMesg) msg_, bool block) {
    if (!valid_mq_ptr(mq_)) {
        return false;
    }

    HostMQ* hmq = get_hmq(mq_);
    if (!hmq) {
        return false;
    }

    if (!block) {
        if (hmq->validCount == 0) {
            return false;
        }
    } else {
        while (hmq->validCount == 0) {
            debug_printf("[Message Queue] Thread %d is blocked on receive\n", TO_PTR(OSThread, ultramodern::this_thread())->id);
            host_tq_insert(PASS_RDRAM &hmq->blocked_on_recv, ultramodern::this_thread());
            ultramodern::run_next_thread_and_wait(PASS_RDRAM1);
        }
    }

    if (msg_ != NULLPTR) {
        *TO_PTR(OSMesg, msg_) = hmq->buf[hmq->first];
    }

    hmq->first = (hmq->first + 1) % hmq->msgCount;
    hmq->validCount--;

    if (!host_tq_empty(&hmq->blocked_on_send)) {
        ultramodern::schedule_running_thread(PASS_RDRAM host_tq_pop(PASS_RDRAM &hmq->blocked_on_send));
    }

    return true;
}

extern "C" s32 osSendMesg(RDRAM_ARG PTR(OSMesgQueue) mq_, OSMesg msg, s32 flags) {
    if (!valid_mq_ptr(mq_) || !get_hmq(mq_)) {
        int32_t recovered = recover_send_mq(mq_);
        if (recovered && get_hmq(recovered)) {
            mq_ = recovered;
        } else {
            return 0;
        }
    }
    record_send_affinity(mq_);
    bool jam = false;

    if (!ultramodern::is_game_thread()) {
        ultramodern::enqueue_external_message(mq_, msg, jam, false);
        return 0;
    }

    dequeue_external_messages(PASS_RDRAM1);
    bool sent = do_send(PASS_RDRAM mq_, msg, jam, flags == OS_MESG_BLOCK);
    ultramodern::check_running_queue(PASS_RDRAM1);

    return sent ? 0 : -1;
}

extern "C" s32 osJamMesg(RDRAM_ARG PTR(OSMesgQueue) mq_, OSMesg msg, s32 flags) {
    if (!valid_mq_ptr(mq_) || !get_hmq(mq_)) {
        int32_t recovered = recover_send_mq(mq_);
        if (recovered && get_hmq(recovered)) {
            mq_ = recovered;
        } else {
            return 0;
        }
    }
    record_send_affinity(mq_);
    bool jam = true;

    if (!ultramodern::is_game_thread()) {
        ultramodern::enqueue_external_message(mq_, msg, jam, false);
        return 0;
    }

    dequeue_external_messages(PASS_RDRAM1);
    bool sent = do_send(PASS_RDRAM mq_, msg, jam, flags == OS_MESG_BLOCK);
    ultramodern::check_running_queue(PASS_RDRAM1);

    return sent ? 0 : -1;
}

extern "C" s32 osRecvMesg(RDRAM_ARG PTR(OSMesgQueue) mq_, PTR(OSMesg) msg_, s32 flags) {
    if (!valid_mq_ptr(mq_) || !get_hmq(mq_)) {
        int32_t recovered = recover_recv_mq(mq_);
        if (recovered && get_hmq(recovered)) {
            mq_ = recovered;
        } else {
            if (msg_ != NULLPTR && valid_mq_ptr(msg_))
                *TO_PTR(OSMesg, msg_) = (OSMesg)0;
            return 0;
        }
    }
    record_recv_affinity(mq_);

    assert(ultramodern::is_game_thread() && "RecvMesg not allowed outside of game threads.");

    dequeue_external_messages(PASS_RDRAM1);
    bool received = do_recv(PASS_RDRAM mq_, msg_, flags == OS_MESG_BLOCK);
    ultramodern::check_running_queue(PASS_RDRAM1);

    return received ? 0 : -1;
}
