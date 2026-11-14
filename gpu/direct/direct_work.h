// The shader workers: one pool that graphics and compute programs share for reading, translating and making shader
// objects. Jobs come in four classes: PW, what a dispatch holding up the game's load waits for; P0, a variant a draw or
// dispatch is missing now; P1, work for programs the game set at its load; P2, speculative variants. Each class runs
// newest first, in that order. A queued job can be bumped to PW or P0 when a draw or dispatch turns out to need it. P1 and P2 jobs run only on the background workers (half
// the pool), at a lower scheduling priority, so a load keeps its cores while that work goes on; a bump raises a running one.
#pragma once

#include <cstddef>
#include <functional>
#include <memory>

namespace hsd::work {

enum Class { PW = 0, P0 = 1, P1 = 2, P2 = 3, kClasses = 4 };

struct Ticket;
using TicketRef = std::shared_ptr<Ticket>;

/// A job not yet queued: queue it with push. Made first so its owner can keep the ticket before the job can run.
TicketRef make(std::function<void()> f);
void push(const TicketRef& t, Class c);
inline TicketRef run(Class c, std::function<void()> f) {
    TicketRef t = make(std::move(f));
    push(t, c);
    return t;
}
/// Moves a job still queued in a later class to the front of `to` (PW or P0), or raises a running background job to the
/// game's priority; false when there was nothing to do
bool bump(const TicketRef& t, Class to = P0);

int workers();
/// "PW w, P0 a, P1 b, P2 c queued, n running (of w workers, k of them background), b bumped"
void stats(char* out, size_t cap);
size_t queued();
/// Jobs queued and running: 0 when the workers have nothing left to do
size_t pending();

}  // namespace hsd::work
