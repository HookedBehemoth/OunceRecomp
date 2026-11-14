// The shader workers: one pool that graphics and compute programs share for reading, translating and making shader
// objects. Jobs come in three classes: P0, a variant a draw or dispatch is missing now; P1, work for programs the game
// set at its load; P2, speculative variants. Each class runs newest first, P0 before P1 before P2. A queued job can be
// bumped to P0 when a draw or dispatch turns out to need it. P1 and P2 jobs run only on the background workers (half
// the pool), at a lower scheduling priority, so a load keeps its cores while that work goes on; a bump raises a running one.
#pragma once

#include <cstddef>
#include <functional>
#include <memory>

namespace hsd::work {

enum Class { P0 = 0, P1 = 1, P2 = 2, kClasses = 3 };

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
/// Moves a job still queued in P1 or P2 to the front of P0, or raises a running background job to the game's priority;
/// false when there was nothing to do
bool bump(const TicketRef& t);

int workers();
/// "P0 a, P1 b, P2 c queued, n running (of w workers, k of them background), b bumped"
void stats(char* out, size_t cap);
size_t queued();

}  // namespace hsd::work
