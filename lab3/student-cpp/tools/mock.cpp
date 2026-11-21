// Airline mock for lab 3, stage 2. C++ port of student-java/mock/src/lab3mock/Mock.java (same protocol, same numbers,
// same delay distributions). One process plays all airlines.
//   out/mock [--schedule ../data/schedule.bin] [--airlines ../data/airlines.conf] [--port 7700] [--threads N]
//
// Protocol (text lines over TCP; reqId is unique within a connection):
//   -> PRICE reqId searchId AL FROM TO D      <- reqId OK [flightId:price ...] | reqId ERR | reqId LIMIT
//   -> CANCEL reqId                            (no answer)
//   -> DONE searchId                           <- DONE searchId orphans [AL:FROM:TO:D@ageMs ...]
//   -> STATS                                   <- {json}
//   -> RESET                                   <- RESET
//
// Every request waits on its own Asio timer: no thread per request.
#include <boost/asio.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../src/pricing.hpp"

namespace asio = boost::asio;
using asio::ip::tcp;
using clock_type = std::chrono::steady_clock;

namespace {

constexpr auto DONE_GRACE = std::chrono::milliseconds(20);

// ------------------------------------------------------------------ schedule and index

struct schedule_data {
    int32_t n_airports = 0, n_airlines = 0, n_flights = 0;
    std::vector<std::string> airports, airlines;
    std::unordered_map<std::string, int> airport_ids, airline_ids;
    std::vector<char> raw;
    std::size_t records = 0;

    template <class T>
    T field(int32_t f, std::size_t off) const {
        T v;
        std::memcpy(&v, raw.data() + records + static_cast<std::size_t>(f) * 20 + off, sizeof v);
        return v;
    }
    int32_t dep(int32_t f) const { return field<int32_t>(f, 0); }
    int from(int32_t f) const { return field<uint16_t>(f, 8); }
    int to(int32_t f) const { return field<uint16_t>(f, 10); }
    int airline(int32_t f) const { return field<uint16_t>(f, 12); }
    int32_t price(int32_t f) const { return field<int32_t>(f, 16); }

    static schedule_data open(const std::string& path) {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in) throw std::runtime_error("cannot open " + path);
        schedule_data s;
        s.raw.resize(static_cast<std::size_t>(in.tellg()));
        in.seekg(0);
        in.read(s.raw.data(), static_cast<std::streamsize>(s.raw.size()));
        const char* b = s.raw.data();
        if (s.raw.size() < 16 || std::memcmp(b, "FLT1", 4) != 0) throw std::runtime_error(path + ": not a FLT1 file");
        std::memcpy(&s.n_airports, b + 4, 4);
        std::memcpy(&s.n_airlines, b + 8, 4);
        std::memcpy(&s.n_flights, b + 12, 4);
        std::size_t p = 16;
        for (int i = 0; i < s.n_airports; i++, p += 3) {
            s.airports.emplace_back(b + p, 3);
            s.airport_ids[s.airports.back()] = i;
        }
        for (int i = 0; i < s.n_airlines; i++, p += 2) {
            s.airlines.emplace_back(b + p, 2);
            s.airline_ids[s.airlines.back()] = i;
        }
        s.records = p;
        return s;
    }

    int airport_id(const std::string& c) const { auto it = airport_ids.find(c); return it == airport_ids.end() ? -1 : it->second; }
    int airline_id(const std::string& c) const { auto it = airline_ids.find(c); return it == airline_ids.end() ? -1 : it->second; }
};

struct airline_conf {
    std::string code;
    int limit = 0;
    double median_ms = 0, sigma = 0, tail_prob = 0;
    int tail_ms = 0;
    double err_prob = 0;
};

std::vector<airline_conf> load_airlines(const std::string& path, const schedule_data& s) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open " + path);
    std::vector<airline_conf> by_id(s.n_airlines);
    std::vector<bool> seen(s.n_airlines);
    for (std::string line; std::getline(in, line);) {
        std::istringstream ls(line);
        airline_conf a;
        if (!(ls >> a.code) || a.code[0] == '#') continue;
        if (!(ls >> a.limit >> a.median_ms >> a.sigma >> a.tail_prob >> a.tail_ms >> a.err_prob))
            throw std::runtime_error("bad line in " + path + ": " + line);
        int id = s.airline_id(a.code);
        if (id < 0) throw std::runtime_error("unknown airline in " + path + ": " + a.code);
        by_id[id] = a;
        seen[id] = true;
    }
    for (int i = 0; i < s.n_airlines; i++)
        if (!seen[i]) throw std::runtime_error("airline " + s.airlines[i] + " missing in " + path);
    return by_id;
}

// ------------------------------------------------------------------ mock state

class connection;

struct pending {
    uint64_t req_id = 0, search_id = 0;
    int al = 0, from = 0, to = 0, day = 0;
    bool error = false;
    std::string key;
    std::shared_ptr<connection> conn;
    std::atomic<int> state{0};   // 0 in flight, 1 answered, 2 cancelled
    asio::steady_timer timer;
    explicit pending(asio::any_io_executor ex) : timer(ex) {}
};

struct search_state {
    std::mutex m;
    std::vector<std::shared_ptr<pending>> requests;
    std::unordered_map<std::string, clock_type::time_point> ok_at;
};

class mock {
public:
    mock(asio::io_context& io, schedule_data sch, std::vector<airline_conf> airlines)
        : io_(io), sch_(std::move(sch)), airlines_(std::move(airlines)),
          in_flight_(airlines_.size()), max_in_flight_(airlines_.size()) {
        build_index();
    }

    void price(const std::shared_ptr<connection>& c, const std::vector<std::string_view>& p);
    void cancel(connection& c, uint64_t req_id);
    void done(const std::shared_ptr<connection>& c, uint64_t search_id);
    std::string stats() const;
    void reset();
    void drop(connection& c);   // the connection closed: its requests stop without being counted as cancelled
    void bad_line() { bad_++; }

    const schedule_data& sch() const { return sch_; }

private:
    asio::io_context& io_;
    schedule_data sch_;
    std::vector<airline_conf> airlines_;
    std::vector<std::atomic<int>> in_flight_, max_in_flight_;
    std::vector<uint64_t> index_;   // sorted (segmentKey << 24 | flightId)
    std::mutex searches_m_;
    std::unordered_map<uint64_t, std::shared_ptr<search_state>> searches_;
    std::atomic<long> total_{0}, ok_{0}, err_{0}, limit_{0}, cancelled_{0}, orphans_{0}, dones_{0}, bad_{0};

    uint64_t seg_key(int al, int from, int to, int day) const {
        return ((static_cast<uint64_t>(al) * sch_.n_airports + from) * sch_.n_airports + to) * 16 + day;
    }

    void build_index() {
        index_.resize(sch_.n_flights);
        for (int32_t f = 0; f < sch_.n_flights; f++)
            index_[f] = seg_key(sch_.airline(f), sch_.from(f), sch_.to(f), sch_.dep(f) / 1440) << 24 | static_cast<uint64_t>(f);
        std::sort(index_.begin(), index_.end());
    }

    std::shared_ptr<search_state> search(uint64_t id) {
        std::lock_guard g(searches_m_);
        auto& s = searches_[id];
        if (!s) s = std::make_shared<search_state>();
        return s;
    }

    void answer(const std::shared_ptr<pending>& r);
};

// ------------------------------------------------------------------ connections

class connection : public std::enable_shared_from_this<connection> {
public:
    connection(tcp::socket sock, mock& m) : sock_(std::move(sock)), mock_(m) {}

    void start() { read(); }

    void send(std::string line) {
        std::lock_guard g(out_m_);
        out_ += line;
        out_ += '\n';
        if (!writing_ && !closed_) write_locked();
    }

    std::mutex pending_m;
    std::unordered_map<uint64_t, std::shared_ptr<pending>> pending_map;

private:
    tcp::socket sock_;
    mock& mock_;
    asio::streambuf in_;
    std::mutex out_m_;
    std::string out_, writing_buf_;
    bool writing_ = false, closed_ = false;

    void write_locked() {
        writing_ = true;
        writing_buf_.swap(out_);
        out_.clear();
        asio::async_write(sock_, asio::buffer(writing_buf_), [self = shared_from_this()](boost::system::error_code ec, std::size_t) {
            std::lock_guard g(self->out_m_);
            self->writing_ = false;
            if (ec) { self->closed_ = true; return; }
            if (!self->out_.empty()) self->write_locked();
        });
    }

    void read() {
        asio::async_read_until(sock_, in_, '\n', [self = shared_from_this()](boost::system::error_code ec, std::size_t n) {
            if (ec) { self->close(); return; }
            std::string line(asio::buffers_begin(self->in_.data()), asio::buffers_begin(self->in_.data()) + static_cast<std::ptrdiff_t>(n) - 1);
            self->in_.consume(n);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            self->handle(line);
            self->read();
        });
    }

    void handle(const std::string& line);

    void close() {
        mock_.drop(*this);
        {
            std::lock_guard g(out_m_);
            closed_ = true;
        }
        boost::system::error_code ignored;
        sock_.close(ignored);
    }
};

bool parse_u64(std::string_view s, uint64_t& v) {
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    return ec == std::errc() && p == s.data() + s.size();
}

void connection::handle(const std::string& line) {
    std::vector<std::string_view> p;
    std::string_view rest(line);
    for (std::size_t pos; (pos = rest.find(' ')) != std::string_view::npos; rest.remove_prefix(pos + 1)) p.push_back(rest.substr(0, pos));
    p.push_back(rest);
    auto self = shared_from_this();
    uint64_t id = 0;
    if (p[0] == "PRICE") mock_.price(self, p);
    else if (p[0] == "CANCEL" && p.size() >= 2 && parse_u64(p[1], id)) mock_.cancel(*this, id);
    else if (p[0] == "DONE" && p.size() >= 2 && parse_u64(p[1], id)) mock_.done(self, id);
    else if (p[0] == "STATS") send(mock_.stats());
    else if (p[0] == "RESET") { mock_.reset(); send("RESET"); }
    else mock_.bad_line();
}

// ------------------------------------------------------------------ requests

thread_local std::mt19937_64 rng{std::random_device{}()};

void mock::price(const std::shared_ptr<connection>& c, const std::vector<std::string_view>& p) {
    if (p.size() != 7) { bad_++; return; }
    uint64_t req_id = 0, search_id = 0, day_u = 0;
    if (!parse_u64(p[1], req_id) || !parse_u64(p[2], search_id) || !parse_u64(p[6], day_u)) { bad_++; return; }
    int al = sch_.airline_id(std::string(p[3])), from = sch_.airport_id(std::string(p[4])), to = sch_.airport_id(std::string(p[5]));
    int day = static_cast<int>(std::min<uint64_t>(day_u, 1000));
    total_++;
    if (al < 0 || from < 0 || to < 0 || day > 15) { bad_++; c->send(std::to_string(req_id) + " ERR"); return; }
    const airline_conf& a = airlines_[al];
    int now = ++in_flight_[al];
    if (now > a.limit) {
        --in_flight_[al];
        limit_++;
        c->send(std::to_string(req_id) + " LIMIT");
        return;
    }
    for (int m = max_in_flight_[al]; now > m && !max_in_flight_[al].compare_exchange_weak(m, now);) {}

    auto r = std::make_shared<pending>(io_.get_executor());
    r->req_id = req_id;
    r->search_id = search_id;
    r->al = al;
    r->from = from;
    r->to = to;
    r->day = day;
    r->key = std::string(p[3]) + ":" + std::string(p[4]) + ":" + std::string(p[5]) + ":" + std::to_string(day);
    r->conn = c;
    auto s = search(search_id);
    {
        std::lock_guard g(s->m);
        s->requests.push_back(r);
    }
    {
        std::lock_guard g(c->pending_m);
        c->pending_map[req_id] = r;
    }
    std::uniform_real_distribution<double> u(0.0, 1.0);
    std::normal_distribution<double> gauss(0.0, 1.0);
    r->error = u(rng) < a.err_prob;
    double ms;
    if (r->error) ms = 5 + u(rng) * 10;
    else if (u(rng) < a.tail_prob) ms = a.tail_ms;
    else ms = a.median_ms * std::exp(a.sigma * gauss(rng));
    r->timer.expires_after(std::chrono::microseconds(static_cast<long long>(ms * 1000)));
    r->timer.async_wait([this, r](boost::system::error_code ec) {
        if (!ec) answer(r);
    });
}

void mock::answer(const std::shared_ptr<pending>& r) {
    int expected = 0;
    if (!r->state.compare_exchange_strong(expected, 1)) return;
    auto c = std::move(r->conn);
    {
        std::lock_guard g(c->pending_m);
        c->pending_map.erase(r->req_id);
    }
    --in_flight_[r->al];
    if (r->error) {
        err_++;
        c->send(std::to_string(r->req_id) + " ERR");
        return;
    }
    std::string out = std::to_string(r->req_id) + " OK";
    uint64_t k = seg_key(r->al, r->from, r->to, r->day);
    auto lo = std::lower_bound(index_.begin(), index_.end(), k << 24);
    auto hi = std::lower_bound(lo, index_.end(), (k + 1) << 24);
    for (auto it = lo; it != hi; ++it) {
        auto f = static_cast<int32_t>(*it & 0xFFFFFF);
        if (lab3::pricing::no_seats(f, r->search_id)) continue;
        out += ' ';
        out += std::to_string(f);
        out += ':';
        out += std::to_string(lab3::pricing::price(f, r->search_id, sch_.price(f)));
    }
    std::shared_ptr<search_state> s;
    {
        std::lock_guard g(searches_m_);
        auto it = searches_.find(r->search_id);
        if (it != searches_.end()) s = it->second;
    }
    if (s) {
        std::lock_guard g(s->m);
        s->ok_at.try_emplace(r->key, clock_type::now());
    }
    ok_++;
    c->send(std::move(out));
}

void mock::cancel(connection& c, uint64_t req_id) {
    std::shared_ptr<pending> r;
    {
        std::lock_guard g(c.pending_m);
        auto it = c.pending_map.find(req_id);
        if (it == c.pending_map.end()) return;
        r = std::move(it->second);
        c.pending_map.erase(it);
    }
    int expected = 0;
    if (!r->state.compare_exchange_strong(expected, 2)) return;
    r->conn.reset();   // the timer still fires later; its handler sees state 2 and does nothing
    --in_flight_[r->al];
    cancelled_++;
}

void mock::drop(connection& c) {
    std::unordered_map<uint64_t, std::shared_ptr<pending>> open;
    {
        std::lock_guard g(c.pending_m);
        open.swap(c.pending_map);
    }
    for (auto& [id, r] : open) {
        int expected = 0;
        if (!r->state.compare_exchange_strong(expected, 2)) continue;
        r->conn.reset();
        --in_flight_[r->al];
    }
}

void mock::done(const std::shared_ptr<connection>& c, uint64_t search_id) {
    auto at = clock_type::now();
    std::shared_ptr<search_state> s;
    {
        std::lock_guard g(searches_m_);
        auto it = searches_.find(search_id);
        if (it != searches_.end()) { s = std::move(it->second); searches_.erase(it); }
    }
    dones_++;
    if (!s) { c->send("DONE " + std::to_string(search_id) + " 0"); return; }
    std::string keys;
    auto open = std::make_shared<std::vector<std::shared_ptr<pending>>>();
    {
        std::lock_guard g(s->m);
        for (auto& [key, t] : s->ok_at)
            keys += ' ' + key + '@' + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(at - t).count());
        for (auto& r : s->requests)
            if (r->state.load() == 0) open->push_back(r);
    }
    // CANCELs travel on other connections than DONE: give them a moment to arrive.
    auto t = std::make_shared<asio::steady_timer>(io_, DONE_GRACE);
    t->async_wait([this, t, c, open, search_id, keys = std::move(keys)](boost::system::error_code) {
        long n = 0;
        for (auto& r : *open)
            if (r->state.load() == 0) n++;
        orphans_ += n;
        c->send("DONE " + std::to_string(search_id) + " " + std::to_string(n) + keys);
    });
}

std::string mock::stats() const {
    std::string sb = "{";
    auto num = [&](const char* name, long v, bool comma = true) {
        if (comma) sb += ',';
        sb += '"';
        sb += name;
        sb += "\":" + std::to_string(v);
    };
    num("requests", total_, false);
    num("ok", ok_);
    num("err", err_);
    num("limit", limit_);
    num("cancelled", cancelled_);
    num("orphans", orphans_);
    num("searchesDone", dones_);
    num("badLines", bad_);
    long in = 0;
    for (auto& x : in_flight_) in += x.load();
    num("inFlightNow", in);
    sb += ",\"maxInFlight\":{";
    for (std::size_t i = 0; i < airlines_.size(); i++) {
        if (i) sb += ',';
        sb += '"' + airlines_[i].code + "\":" + std::to_string(max_in_flight_[i].load());
    }
    return sb + "}}";
}

void mock::reset() {
    for (auto* x : {&total_, &ok_, &err_, &limit_, &cancelled_, &orphans_, &dones_, &bad_}) x->store(0);
    for (std::size_t i = 0; i < airlines_.size(); i++) max_in_flight_[i] = in_flight_[i].load();
    std::lock_guard g(searches_m_);
    searches_.clear();
}

void accept_loop(tcp::acceptor& acc, mock& m) {
    acc.async_accept([&acc, &m](boost::system::error_code ec, tcp::socket sock) {
        if (!ec) {
            sock.set_option(tcp::no_delay(true));
            std::make_shared<connection>(std::move(sock), m)->start();
        }
        accept_loop(acc, m);
    });
}

}  // namespace

int main(int argc, char** argv) {
    std::map<std::string, std::string> a{{"--schedule", "../data/schedule.bin"}, {"--airlines", "../data/airlines.conf"},
                                         {"--port", "7700"},
                                         {"--threads", std::to_string(std::max(2u, std::thread::hardware_concurrency() / 4))}};
    for (int i = 1; i + 1 < argc; i += 2) a[argv[i]] = argv[i + 1];
    try {
        auto t0 = clock_type::now();
        auto sch = schedule_data::open(a["--schedule"]);
        auto airlines = load_airlines(a["--airlines"], sch);
        asio::io_context io;
        mock m(io, std::move(sch), std::move(airlines));
        auto port = static_cast<unsigned short>(std::stoi(a["--port"]));
        tcp::acceptor acc(io);
        tcp::endpoint ep(asio::ip::make_address("127.0.0.1"), port);
        acc.open(ep.protocol());
        acc.set_option(tcp::acceptor::reuse_address(true));
        acc.bind(ep);
        acc.listen(1024);
        std::printf("mock: %d flights, %d airlines, indexed in %lld ms, listening on 127.0.0.1:%d\n", m.sch().n_flights,
                    m.sch().n_airlines,
                    static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(clock_type::now() - t0).count()),
                    port);
        std::fflush(stdout);
        accept_loop(acc, m);
        int threads = std::stoi(a["--threads"]);
        std::vector<std::thread> pool;
        for (int i = 1; i < threads; i++) pool.emplace_back([&io] { io.run(); });
        io.run();
        for (auto& t : pool) t.join();
    } catch (const std::exception& e) {
        std::cerr << "mock: " << e.what() << "\n";
        return 1;
    }
}
