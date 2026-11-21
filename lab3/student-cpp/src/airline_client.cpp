#include "airline_client.hpp"

#include <atomic>
#include <charconv>
#include <condition_variable>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace asio = boost::asio;
using asio::ip::tcp;

namespace lab3 {

const char* to_string(price_result::kind k) {
    switch (k) {
        case price_result::kind::ok: return "OK";
        case price_result::kind::err: return "ERR";
        case price_result::kind::limit: return "LIMIT";
        case price_result::kind::timeout: return "TIMEOUT";
    }
    return "?";
}

namespace {
price_result parse(std::string_view s) {
    price_result r;
    if (s.starts_with("ERR")) { r.k = price_result::kind::err; return r; }
    if (s.starts_with("LIMIT")) { r.k = price_result::kind::limit; return r; }
    r.k = price_result::kind::ok;   // "OK f:p f:p ..."
    std::size_t i = 2;
    while (i < s.size()) {
        while (i < s.size() && s[i] == ' ') i++;
        if (i >= s.size()) break;
        int32_t id = 0, price = 0;
        auto [p1, e1] = std::from_chars(s.data() + i, s.data() + s.size(), id);
        auto [p2, e2] = std::from_chars(p1 + 1, s.data() + s.size(), price);
        r.flights.emplace_back(id, price);
        i = static_cast<std::size_t>(p2 - s.data());
    }
    return r;
}
}  // namespace

// One TCP connection: writes under a mutex from any thread, one reader thread delivers answers.
struct airline_client::connection {
    using deliver_fn = std::function<void(price_result)>;

    asio::io_context io;   // only owns the socket; all operations on it are synchronous
    tcp::socket sock{io};
    std::mutex write_mu;
    std::mutex calls_mu;
    std::unordered_map<uint64_t, deliver_fn> calls;
    std::atomic<uint64_t> ids{0};
    std::thread reader;

    connection(const std::string& host, unsigned short port) {
        sock.connect(tcp::endpoint(asio::ip::make_address(host), port));
        sock.set_option(tcp::no_delay(true));
        reader = std::thread([this] { read_loop(); });
    }

    ~connection() {
        boost::system::error_code ec;
        sock.shutdown(tcp::socket::shutdown_both, ec);
        if (reader.joinable()) reader.join();
        sock.close(ec);
    }

    void write(const std::string& line) {
        std::lock_guard lk(write_mu);
        asio::write(sock, asio::buffer(line));
    }

    uint64_t send(const price_key& k, uint64_t search_id, deliver_fn fn) {
        uint64_t id = ++ids;
        {
            std::lock_guard lk(calls_mu);
            calls.emplace(id, std::move(fn));
        }
        write("PRICE " + std::to_string(id) + " " + std::to_string(search_id) + " " + k.airline + " " + k.from + " " +
              k.to + " " + std::to_string(k.day) + "\n");
        return id;
    }

    // Takes the request out of the table; whoever gets it delivers the result exactly once.
    std::optional<deliver_fn> take(uint64_t id) {
        std::lock_guard lk(calls_mu);
        auto it = calls.find(id);
        if (it == calls.end()) return std::nullopt;
        deliver_fn fn = std::move(it->second);
        calls.erase(it);
        return fn;
    }

    // @return the delivery function if the request was still unanswered (CANCEL was sent), nullopt otherwise
    std::optional<deliver_fn> cancel(uint64_t id) {
        auto fn = take(id);
        if (fn) write("CANCEL " + std::to_string(id) + "\n");
        return fn;
    }

    void read_loop() {
        asio::streambuf buf;
        boost::system::error_code ec;
        while (true) {
            std::size_t n = asio::read_until(sock, buf, '\n', ec);
            if (ec) break;
            std::string line(asio::buffers_begin(buf.data()), asio::buffers_begin(buf.data()) + static_cast<std::ptrdiff_t>(n) - 1);
            buf.consume(n);
            std::size_t sp = line.find(' ');
            uint64_t id = 0;
            std::from_chars(line.data(), line.data() + sp, id);
            if (auto fn = take(id)) (*fn)(parse(std::string_view(line).substr(sp + 1)));   // else: cancelled meanwhile
        }
        std::unordered_map<uint64_t, deliver_fn> left;
        {
            std::lock_guard lk(calls_mu);
            left.swap(calls);
        }
        if (!left.empty()) std::cerr << "airline mock connection closed, " << left.size() << " requests unanswered\n";
        for (auto& [id, fn] : left) fn(price_result{});
    }
};

airline_client::airline_client(const std::string& host, unsigned short port, int connections) {
    for (int i = 0; i < connections; i++) conns_.push_back(std::make_unique<connection>(host, port));
}

airline_client::~airline_client() = default;

// One airline always uses the same connection: a CANCEL then reaches the mock before any later request to that
// airline, so freeing a limit slot right after a cancellation is safe.
airline_client::connection& airline_client::pick(const price_key& key) {
    return *conns_[std::hash<std::string>{}(key.airline) % conns_.size()];
}

price_result airline_client::get(const price_key& key, uint64_t search_id, std::chrono::nanoseconds timeout,
                                 std::stop_token stop) {
    struct state {
        std::mutex m;
        std::condition_variable_any cv;
        std::optional<price_result> r;
    };
    auto st = std::make_shared<state>();
    connection& c = pick(key);
    uint64_t id = c.send(key, search_id, [st](price_result r) {
        std::lock_guard lk(st->m);
        st->r = std::move(r);
        st->cv.notify_all();
    });
    std::unique_lock lk(st->m);
    if (st->cv.wait_for(lk, stop, timeout, [&] { return st->r.has_value(); })) return std::move(*st->r);
    lk.unlock();
    if (c.cancel(id)) return price_result{};   // timed out or stopped: cancelled at the airline
    lk.lock();                                  // the answer is being delivered right now
    st->cv.wait(lk, [&] { return st->r.has_value(); });
    return std::move(*st->r);
}

namespace {

// A pending async_get. Its handler (a coroutine frame) and executor belong to the caller's execution context and
// must never be touched after that context is destroyed; `context_ops` guarantees it.
struct async_op;

// Pending async_gets of one execution context (io_context). Shared by the context's service and by the ops, so the
// reader thread can still lock it after the context is gone.
struct context_ops {
    std::mutex mu;
    std::unordered_set<std::shared_ptr<async_op>> ops;
};

struct async_op {
    asio::any_completion_handler<void(price_result)> h;   // empty once delivered or dropped
    asio::any_io_executor ex;
    asio::cancellation_slot slot;
    std::shared_ptr<context_ops> owner;
    std::function<bool()> cancel_request;   // takes the request out of the client and sends CANCEL
};

// Registered in every execution context that calls async_get. When the context is destroyed (for example the
// aggregator is destroyed while forgotten coroutines still wait for answers), it cancels their requests at the
// airline and destroys their handlers while the context still exists; answers that arrive later are dropped.
class client_service : public asio::execution_context::service {
public:
    using key_type = client_service;
    static inline asio::execution_context::id id;

    explicit client_service(asio::execution_context& ctx) : service(ctx), ops_(std::make_shared<context_ops>()) {}
    const std::shared_ptr<context_ops>& ops() const { return ops_; }

    void shutdown() override {
        std::unordered_set<std::shared_ptr<async_op>> left;
        std::vector<asio::any_completion_handler<void(price_result)>> handlers;
        {
            std::lock_guard lk(ops_->mu);
            left.swap(ops_->ops);
            for (auto& o : left) {
                handlers.push_back(std::move(o->h));
                o->ex = {};
            }
        }
        for (auto& o : left) o->cancel_request();
        handlers.clear();   // destroys the waiting coroutine frames, inside the still living context
    }

private:
    std::shared_ptr<context_ops> ops_;
};

// Hands the result to the coroutine through its executor, at most once, and only while its context is alive.
void deliver(const std::shared_ptr<async_op>& o, price_result r) {
    std::lock_guard lk(o->owner->mu);
    if (!o->h) return;   // already delivered, or the context was destroyed
    o->owner->ops.erase(o);
    asio::post(o->ex, [h = std::move(o->h), slot = o->slot, r = std::move(r)]() mutable {
        if (slot.is_connected()) slot.clear();
        std::move(h)(std::move(r));
    });
    o->ex = {};
}

}  // namespace

asio::awaitable<price_result> airline_client::async_get(const price_key& key, uint64_t search_id) {
    connection& c = pick(key);
    co_return co_await asio::async_initiate<decltype(asio::use_awaitable), void(price_result)>(
        [&c, key, search_id](auto handler) {
            auto o = std::make_shared<async_op>();
            o->slot = asio::get_associated_cancellation_slot(handler);
            o->ex = asio::get_associated_executor(handler);
            auto& ctx = asio::query(o->ex, asio::execution::context);
            o->owner = asio::use_service<client_service>(ctx).ops();
            o->h = std::move(handler);
            // The request table decides who delivers: the reader thread on an answer, the cancellation handler,
            // or the context's shutdown.
            uint64_t id = c.send(key, search_id, [o](price_result r) { deliver(o, std::move(r)); });
            o->cancel_request = [&c, id] { return c.cancel(id).has_value(); };
            {
                std::lock_guard lk(o->owner->mu);
                if (o->h) o->owner->ops.insert(o);   // not yet answered
            }
            if (o->slot.is_connected())
                o->slot.assign([&c, id](asio::cancellation_type t) {
                    if (t == asio::cancellation_type::none) return;
                    if (auto fn = c.cancel(id)) (*fn)(price_result{});
                });
        },
        asio::use_awaitable);
}

}  // namespace lab3
