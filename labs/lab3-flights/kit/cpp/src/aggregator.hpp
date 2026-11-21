// What you write in stage 2, and what the harness gives you.
#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "airline_client.hpp"
#include "schedule.hpp"

namespace lab3 {

using clock = std::chrono::steady_clock;

// One search: price the candidate routes and answer before `deadline` (300 ms after the planned start).
struct search {
    // A candidate route: 1–3 flights.
    struct route {
        std::vector<flight> flights;
        int duration_minutes() const { return flights.back().arr - flights.front().dep; }
    };

    uint64_t id = 0;   // send it with every airline request of this search
    std::string from, to;
    int day = 0;
    std::vector<route> routes;   // the answer refers to them by index in this list
    clock::time_point deadline;

    // time left until the deadline, never negative
    clock::duration remaining() const { return std::max(clock::duration::zero(), deadline - clock::now()); }
};

// Answer to a search: one result per candidate route (same order as search::routes) and the top.
//   priced(price)  every flight of the route has a live price; price = their sum
//   no_seats       an airline answered ok and one of the route's flights is not in its answer
//   unpriced       no ok answer by the deadline for at least one flight of the route
// Build it with answer::of, which also computes the top.
struct answer {
    enum class status { priced, no_seats, unpriced };
    struct route_result {
        status st = status::unpriced;
        int32_t price = 0;
        static route_result priced(int32_t p) { return {status::priced, p}; }
        static route_result no_seats() { return {status::no_seats, 0}; }
        static route_result unpriced() { return {status::unpriced, 0}; }
    };
    static constexpr std::size_t top_size = 10;

    std::vector<route_result> routes;
    std::vector<int> top;   // indices of priced routes by price, then duration, then index; at most top_size

    static answer of(const search& s, std::vector<route_result> results);
    bool partial() const;
};

// Everything the harness gives your aggregator.
struct kit {
    airline_client& client;                       // shared airline client
    const schedule& sched;                        // schedule.bin (not needed for stage 2, there if you want it)
    std::map<std::string, int> limits;            // max unanswered requests per airline, from airlines.conf
    std::map<std::string, std::string> options;   // --opt key=value from the command line, e.g. pool size

    int limit(const std::string& airline) const;
    int int_option(const std::string& name, int fallback) const;
};

// Your stage 2 solution. The harness calls search() at a fixed rate from one thread and does not wait for the
// result before the next call, so search() must return quickly. Call `done` exactly once, from any thread, when
// the answer is ready, at the latest at the search deadline. The search object stays alive until you call done
// (and after, as long as you hold the pointer).
struct aggregator {
    virtual void search(std::shared_ptr<const lab3::search> s, std::function<void(answer)> done) = 0;
    virtual ~aggregator() = default;   // called once after the run: stop your threads here
};

// Implementations are chosen by name: ./run.sh --impl <name>. Register yours with
//   LAB3_REGISTER("name", my_aggregator)   // my_aggregator has a constructor taking kit&
using factory = std::function<std::unique_ptr<aggregator>(kit&)>;
std::map<std::string, factory>& registry();

struct registrar {
    registrar(const std::string& name, factory f) { registry().emplace(name, std::move(f)); }
};

}  // namespace lab3

#define LAB3_CONCAT2(a, b) a##b
#define LAB3_CONCAT(a, b) LAB3_CONCAT2(a, b)
#define LAB3_REGISTER(name, type)                                                       \
    static ::lab3::registrar LAB3_CONCAT(lab3_registrar_, __LINE__){                   \
        name, [](::lab3::kit& k) -> std::unique_ptr<::lab3::aggregator> { return std::make_unique<type>(k); }}
