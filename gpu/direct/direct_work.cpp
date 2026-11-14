// The shader workers (direct_work.h).
#include "direct_work.h"

#include <pthread.h>
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>

#include "direct_internal.h"

namespace hsd::work {

struct Ticket {
    std::function<void()> f;
    int cls = -1;        // (the class it is queued in; -1 not queued yet, -2 taken by a worker; under mu)
    pid_t tid = 0;       // (the background worker running it at the lower priority, which a bump raises; under mu)
};

namespace {

std::mutex mu;
std::condition_variable cv_now, cv_bg;   // (the workers that take only P0; the background ones)
std::deque<TicketRef> q[kClasses];   // (newest at the front; a bumped ticket leaves a stale entry behind, skipped)
size_t live[kClasses]{};             // (tickets queued in each class, stale entries not counted)
int running = 0;
int64_t bumped = 0;
std::once_flag once;
int n_workers = 2, bg_cap = 1;

/// The next job a worker may take: P0 first, then P1 and P2 for a background worker (under mu)
TicketRef take(bool background, int& cls) {
    for (int c = 0; c < (background ? kClasses : P0 + 1); c++) {
        while (!q[c].empty()) {
            TicketRef t = std::move(q[c].front());
            q[c].pop_front();
            if (t->cls != c) continue;   // (bumped since)
            t->cls = -2, live[c]--, cls = c;
            return t;
        }
    }
    return nullptr;
}

/// A worker. A background one runs its P1 and P2 jobs at a lower scheduling priority (nice 10), so the game's own threads
/// go first when the machine is busy with a load, and its PW and P0 jobs at the game's; the others take only those
void worker(bool background) {
    pthread_setname_np(pthread_self(), background ? "shader bg" : "shader worker");
    const pid_t me = gettid();
    int nice = 0;
    for (;;) {
        TicketRef t;
        int cls = P0;
        {
            std::unique_lock lk(mu);
            (background ? cv_bg : cv_now).wait(lk, [&] { return (t = take(background, cls)) != nullptr; });
            running++;
            if (background) {
                const int want = cls <= P0 ? 0 : 10;
                if (want != nice && setpriority(PRIO_PROCESS, (id_t)me, want) == 0) nice = want;
                t->tid = nice ? me : 0;
            }
        }
        std::function<void()> f = std::move(t->f);
        f();
        std::lock_guard lk(mu);
        running--;
        t->tid = 0;
        if (background) nice = getpriority(PRIO_PROCESS, (id_t)me);   // (a bump may have raised it)
    }
}

void start() {
    std::call_once(once, [] {
        // (half the machine's threads: the game's main, module and executor threads keep the other half; HOS_DBG=workers:<n>
        // sets the count, for measuring)
        const unsigned hw = std::max(2u, std::thread::hardware_concurrency());
        n_workers = (int)std::clamp(hw / 2, 2u, 12u);
        if (const char* v = dbg_value("workers"); v && std::atoi(v) > 0) n_workers = std::clamp(std::atoi(v), 1, 64);
        bg_cap = std::max(1, n_workers / 2);
        for (int i = 0; i < n_workers; i++) std::thread(worker, i < bg_cap).detach();
        logf("direct: shader workers: %d (at most %d on work for programs set at the load or speculative)", n_workers, bg_cap);
    });
}

}  // namespace

TicketRef make(std::function<void()> f) {
    auto t = std::make_shared<Ticket>();
    t->f = std::move(f);
    return t;
}

void push(const TicketRef& t, Class c) {
    start();
    {
        std::lock_guard lk(mu);
        if (t->cls != -1) return;
        t->cls = c, live[c]++;
        q[c].push_front(t);
    }
    if (c <= P0) cv_now.notify_one();   // (a background worker takes it too when no other is free)
    cv_bg.notify_one();
}

bool bump(const TicketRef& t, Class to) {
    if (!t) return false;
    {
        std::lock_guard lk(mu);
        if (t->cls == -2 && t->tid) {   // (running at the lower priority: raised to the game's)
            const bool raised = setpriority(PRIO_PROCESS, (id_t)t->tid, 0) == 0;
            t->tid = 0, bumped += raised;
            return raised;
        }
        if (t->cls < 0 || t->cls <= to) return false;
        live[t->cls]--, live[to]++, bumped++;
        t->cls = to;
        q[to].push_front(t);
    }
    cv_now.notify_one(), cv_bg.notify_one();
    return true;
}

int workers() {
    start();
    return n_workers;
}

size_t queued() {
    std::lock_guard lk(mu);
    return live[PW] + live[P0] + live[P1] + live[P2];
}

size_t pending() {
    std::lock_guard lk(mu);
    return live[PW] + live[P0] + live[P1] + live[P2] + (size_t)running;
}

void stats(char* out, size_t cap) {
    std::lock_guard lk(mu);
    std::snprintf(out, cap, "PW %zu, P0 %zu, P1 %zu, P2 %zu queued, %d running (of %d workers, %d of them background), %lld bumped", live[PW], live[P0], live[P1],
                  live[P2], running, n_workers, bg_cap, (long long)bumped);
}

}  // namespace hsd::work
