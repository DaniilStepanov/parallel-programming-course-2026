// Client of the airline mock. Thread-safe; the harness creates one and shares it through the kit.
//
// Requests go over a few TCP connections (all requests to one airline over the same one). Each connection has one
// reader thread that delivers the answers. The client does not retry, does not limit and has no deadlines of its
// own: that is your job.
//
// Every request counts against the airline's limit until it is answered or cancelled. A request that is neither
// answered nor cancelled when the search is done is an orphan.
#pragma once
#include <boost/asio.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include "schedule.hpp"

namespace lab3 {

// Airline answer.
//   ok      live prices of the airline's flights on the segment that still have seats;
//           a flight of this segment missing from the list has no seats
//   err     temporary error, the request may be repeated
//   limit   the airline already had its limit of unanswered requests; this one was refused
//   timeout no answer: get() timed out or was stopped, or async_get() was cancelled; the request was cancelled at the airline
struct price_result {
    enum class kind { ok, err, limit, timeout };
    kind k = kind::timeout;
    std::vector<std::pair<int32_t, int32_t>> flights;   // (flight id, live price)

    bool ok() const { return k == kind::ok; }
    // live price of the flight, or -1 if it is not in the answer (no seats). Only meaningful for ok.
    int32_t price_of(int32_t flight_id) const {
        for (auto& [id, p] : flights)
            if (id == flight_id) return p;
        return -1;
    }
};

const char* to_string(price_result::kind k);

class airline_client {
public:
    airline_client(const std::string& host, unsigned short port, int connections = 8);
    ~airline_client();
    airline_client(const airline_client&) = delete;
    airline_client& operator=(const airline_client&) = delete;

    // Blocking request. Returns the answer, or timeout if none came within `timeout` or `stop` was requested
    // (the C++ counterpart of interrupting a thread); in both cases the request is cancelled at the airline
    // (CANCEL is sent). Call it from your own threads, never from a thread that runs an io_context.
    price_result get(const price_key& key, uint64_t search_id, std::chrono::nanoseconds timeout,
                     std::stop_token stop = {});

    // Asynchronous request for coroutines. Supports Asio per-operation cancellation: if the operation is cancelled,
    // for example because it loses `co_await (client.async_get(k, id) || timer.async_wait(use_awaitable))`,
    // CANCEL is sent to the airline and the operation completes with timeout.
    // Only cancellation through Asio stops the request. If you simply stop waiting for it (co_spawn it detached and
    // forget it, ignore its result), the airline keeps working on it.
    //
    // The answer is delivered through the coroutine's executor. Run the coroutine on a strand (make_strand) or on a
    // single-threaded io_context: Asio cancellation slots are not thread-safe.
    boost::asio::awaitable<price_result> async_get(const price_key& key, uint64_t search_id);

private:
    struct connection;
    connection& pick(const price_key& key);
    std::vector<std::unique_ptr<connection>> conns_;
};

}  // namespace lab3
