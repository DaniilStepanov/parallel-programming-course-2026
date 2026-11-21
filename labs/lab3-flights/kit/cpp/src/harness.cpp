// Stage 2 harness: starts searches at a fixed rate (open loop), measures latency from the planned start,
// checks every answer against the airline mock and prints the metrics. Same columns as the Java harness.
//
//   ./run.sh --impl student --rate 200 [--seconds 20] [--warmup 5] [--sweep 50,100,200]
//            [--data ../data] [--host 127.0.0.1 --port 7700] [--connections 8] [--opt key=value ...]
#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <optional>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "aggregator.hpp"
#include "pricing.hpp"

namespace asio = boost::asio;
using asio::ip::tcp;
using namespace std::chrono;
using namespace lab3;

namespace {

constexpr auto deadline_after = 300ms;
// An OK answer the mock sent less than this before it got DONE may not have reached the aggregator in time.
constexpr long in_transit_ms = 20;
constexpr double min_answered_pct = 98;

struct tmpl {
    std::string from, to;
    int day = 0;
    std::vector<search::route> routes;
};

std::vector<tmpl> load_searches(const std::string& file, const schedule& s) {
    std::ifstream in(file);
    if (!in) throw std::runtime_error("cannot open " + file);
    std::vector<tmpl> list;
    std::string line;
    while (std::getline(in, line)) {
        auto bar = line.find('|');
        if (bar == std::string::npos) continue;
        std::istringstream head(line.substr(0, bar));
        long id;
        tmpl t;
        head >> id >> t.from >> t.to >> t.day;
        std::istringstream rest(line.substr(bar + 1));
        std::string r;
        while (rest >> r) {
            search::route route;
            std::size_t p = 0;
            while (p <= r.size()) {
                std::size_t q = r.find('-', p);
                if (q == std::string::npos) q = r.size();
                int32_t fid = 0;
                std::from_chars(r.data() + p, r.data() + q, fid);
                route.flights.push_back(s.get(fid));
                p = q + 1;
            }
            t.routes.push_back(std::move(route));
        }
        list.push_back(std::move(t));
    }
    return list;
}

std::map<std::string, int> load_limits(const std::string& file) {
    std::ifstream in(file);
    if (!in) throw std::runtime_error("cannot open " + file);
    std::map<std::string, int> m;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream l(line);
        std::string al;
        int limit;
        if (l >> al >> limit) m[al] = limit;
    }
    return m;
}

long proc_status(const char* field) {   // Threads: n / VmRSS: n kB
    std::ifstream in("/proc/self/status");
    std::string line;
    std::size_t len = std::strlen(field);
    while (std::getline(in, line))
        if (line.compare(0, len, field) == 0) return std::stol(line.substr(len));
    return -1;
}

long json_long(const std::string& json, const std::string& key) {
    auto i = json.find("\"" + key + "\":");
    if (i == std::string::npos) return 0;
    i += key.size() + 3;
    long v = 0;
    std::from_chars(json.data() + i, json.data() + json.size(), v);
    return v;
}

// Control connection: DONE / STATS / RESET.
class control {
public:
    struct done_info {
        long orphans = 0;
        std::unordered_map<std::string, long> answered_age_ms;
    };

    control(const std::string& host, unsigned short port) {
        try {
            sock_.connect(tcp::endpoint(asio::ip::make_address(host), port));
        } catch (const std::exception& e) {
            throw std::runtime_error("cannot connect to the airline mock at " + host + ":" + std::to_string(port) +
                                     " (is ./run-mock.sh running?)");
        }
        sock_.set_option(tcp::no_delay(true));
        reader_ = std::thread([this] { read_loop(); });
    }

    ~control() {
        boost::system::error_code ec;
        sock_.shutdown(tcp::socket::shutdown_both, ec);
        reader_.join();
    }

    // cb runs on the control reader thread, or with nullopt after 5 s without an answer
    void done(uint64_t search_id, std::function<void(std::optional<done_info>)> cb) {
        {
            std::lock_guard lk(mu_);
            dones_.emplace(search_id, pending{steady_clock::now() + 5s, std::move(cb)});
        }
        write("DONE " + std::to_string(search_id) + "\n");
    }

    std::string stats() { write("STATS\n"); return reply(); }
    void reset() { write("RESET\n"); reply(); }

    // DONE answers that never came: fail them
    void expire() {
        std::vector<std::function<void(std::optional<done_info>)>> late;
        {
            std::lock_guard lk(mu_);
            auto now = steady_clock::now();
            for (auto it = dones_.begin(); it != dones_.end();)
                if (it->second.until < now) { late.push_back(std::move(it->second.cb)); it = dones_.erase(it); }
                else ++it;
        }
        for (auto& cb : late) cb(std::nullopt);
    }

private:
    struct pending {
        steady_clock::time_point until;
        std::function<void(std::optional<done_info>)> cb;
    };

    void write(const std::string& s) {
        std::lock_guard lk(write_mu_);
        asio::write(sock_, asio::buffer(s));
    }

    std::string reply() {
        std::unique_lock lk(mu_);
        if (!cv_.wait_for(lk, 5s, [&] { return !replies_.empty(); })) throw std::runtime_error("mock did not reply");
        std::string r = std::move(replies_.front());
        replies_.erase(replies_.begin());
        return r;
    }

    void read_loop() {
        asio::streambuf buf;
        boost::system::error_code ec;
        while (true) {
            std::size_t n = asio::read_until(sock_, buf, '\n', ec);
            if (ec) return;
            std::string line(asio::buffers_begin(buf.data()), asio::buffers_begin(buf.data()) + static_cast<std::ptrdiff_t>(n) - 1);
            buf.consume(n);
            if (line.compare(0, 5, "DONE ") != 0) {
                std::lock_guard lk(mu_);
                replies_.push_back(std::move(line));
                cv_.notify_all();
                continue;
            }
            std::istringstream in(line.substr(5));
            uint64_t id;
            done_info d;
            in >> id >> d.orphans;
            std::string tok;
            while (in >> tok) {
                auto at = tok.find('@');
                d.answered_age_ms[tok.substr(0, at)] = std::stol(tok.substr(at + 1));
            }
            std::function<void(std::optional<done_info>)> cb;
            {
                std::lock_guard lk(mu_);
                auto it = dones_.find(id);
                if (it == dones_.end()) continue;
                cb = std::move(it->second.cb);
                dones_.erase(it);
            }
            cb(std::move(d));
        }
    }

    asio::io_context io_;
    tcp::socket sock_{io_};
    std::mutex write_mu_, mu_;
    std::condition_variable cv_;
    std::vector<std::string> replies_;
    std::unordered_map<uint64_t, pending> dones_;
    std::thread reader_;
};

// @return empty if the answer is right, otherwise what is wrong
std::string check(const search& s, const answer& a, const control::done_info& d) {
    if (a.routes.size() != s.routes.size())
        return "answer has " + std::to_string(a.routes.size()) + " routes, search has " + std::to_string(s.routes.size());
    for (std::size_t i = 0; i < s.routes.size(); i++) {
        auto& fl = s.routes[i].flights;
        auto& r = a.routes[i];
        bool all_answered = true, settled_before = true, any_no_seats = false, no_seats_seen = false;
        int32_t sum = 0;
        for (auto& f : fl) {
            auto it = d.answered_age_ms.find(f.key().str());
            if (it == d.answered_age_ms.end()) { all_answered = false; continue; }
            if (it->second < in_transit_ms) settled_before = false;
            if (pricing::no_seats(f.id, s.id)) any_no_seats = no_seats_seen = true;
            else sum += pricing::price(f.id, s.id, f.base_price);
        }
        std::string route = "route " + std::to_string(i) + ": ";
        switch (r.st) {
            case answer::status::priced:
                if (!all_answered) return route + "PRICED, but the mock did not answer OK for every flight";
                if (any_no_seats) return route + "PRICED, but a flight has no seats";
                if (r.price != sum) return route + "price " + std::to_string(r.price) + ", expected " + std::to_string(sum);
                break;
            case answer::status::no_seats:
                if (!no_seats_seen) return route + "NO_SEATS, but no answered flight of the route lacks seats";
                break;
            case answer::status::unpriced:
                if (all_answered && settled_before && !any_no_seats)
                    return route + "UNPRICED, but the mock had answered OK for every flight before the answer";
                if (all_answered && settled_before && any_no_seats) return route + "UNPRICED, but the answers show NO_SEATS";
                break;
        }
    }
    answer expected = answer::of(s, a.routes);
    if (expected.top != a.top) return "top differs from the expected order";
    return {};
}

struct result {
    double rate = 0;
    int measured = 0;
    double p50 = 0, p99 = 0, max = 0;
    int late = 0, partial = 0;
    double answered = 0;
    int wrong = 0;
    long orphans = 0, limit = 0;
    double requests_per_search = 0;
    long threads = 0, rss_mb = 0;
    std::vector<std::string> errors;
};

class harness {
public:
    harness(std::map<std::string, std::string> args, std::map<std::string, std::string> opts)
        : args_(std::move(args)),
          sched_(schedule::open(args_["--data"] + "/schedule.bin")),
          templates_(load_searches(args_["--data"] + "/searches.txt", sched_)),
          limits_(load_limits(args_["--data"] + "/airlines.conf")),
          options_(std::move(opts)),
          control_(args_["--host"], static_cast<unsigned short>(std::stoi(args_["--port"]))),
          client_(args_["--host"], static_cast<unsigned short>(std::stoi(args_["--port"])), std::stoi(args_["--connections"])) {
        control_.reset();
    }

    std::size_t n_templates() const { return templates_.size(); }

    result run(const std::string& impl, double rate, double seconds, double warmup);

private:
    std::map<std::string, std::string> args_;
    schedule sched_;
    std::vector<tmpl> templates_;
    std::map<std::string, int> limits_;
    std::map<std::string, std::string> options_;
    control control_;
    airline_client client_;
    uint64_t next_id_ = 1;
};

struct run_state {
    std::mutex mu;
    std::condition_variable cv;
    std::vector<long> lat_ns;
    std::atomic<int> late{0}, partial{0}, wrong{0}, measured{0}, outstanding{0};
    std::atomic<long> orphans{0}, keys{0}, keys_answered{0};
    std::vector<std::string> errors;

    void error(uint64_t id, const std::string& why) {
        wrong++;
        std::lock_guard lk(mu);
        if (errors.size() < 10) errors.push_back("search " + std::to_string(id) + ": " + why);
    }
    void arrive() {
        if (--outstanding == 0) { std::lock_guard lk(mu); cv.notify_all(); }
    }
};

result harness::run(const std::string& impl, double rate, double seconds, double warmup) {
    auto it = registry().find(impl);
    if (it == registry().end()) {
        std::string names;
        for (auto& [n, f] : registry()) names += " " + n;
        throw std::runtime_error("unknown --impl " + impl + "; registered:" + names);
    }
    kit k{client_, sched_, limits_, options_};
    auto agg = it->second(k);
    auto st = std::make_shared<run_state>();

    std::atomic<bool> sampling{true};
    std::atomic<long> peak_threads{0}, peak_rss{0};
    std::thread sampler([&] {
        while (sampling) {
            peak_threads = std::max(peak_threads.load(), proc_status("Threads:"));
            peak_rss = std::max(peak_rss.load(), proc_status("VmRSS:") / 1024);
            control_.expire();
            std::this_thread::sleep_for(200ms);
        }
    });

    auto warmup_ns = nanoseconds(static_cast<long>(warmup * 1e9));
    auto total_ns = nanoseconds(static_cast<long>((warmup + seconds) * 1e9));
    auto period = nanoseconds(static_cast<long>(1e9 / rate));
    auto t0 = steady_clock::now();
    std::string stats_at_warmup;
    st->outstanding = 1;
    for (long n = 0;; n++) {
        auto planned = t0 + n * period;
        if (planned - t0 >= total_ns) break;
        bool counted = planned - t0 >= warmup_ns;
        if (counted && stats_at_warmup.empty()) stats_at_warmup = control_.stats();
        std::this_thread::sleep_until(planned);

        auto& t = templates_[next_id_ % templates_.size()];
        auto s = std::make_shared<search>();
        s->id = next_id_++;
        s->from = t.from;
        s->to = t.to;
        s->day = t.day;
        s->routes = t.routes;
        s->deadline = planned + deadline_after;
        if (counted) st->measured++;
        st->outstanding++;
        auto called = std::make_shared<std::atomic<bool>>(false);
        auto finished = [this, st, s, planned, counted, called](std::optional<answer> a, std::string failure) {
            if (called->exchange(true)) { st->error(s->id, "done called more than once"); return; }
            long lat = duration_cast<nanoseconds>(steady_clock::now() - planned).count();
            control_.done(s->id, [st, s, lat, counted, a = std::move(a), failure](std::optional<control::done_info> d) {
                if (counted) {
                    {
                        std::lock_guard lk(st->mu);
                        st->lat_ns.push_back(lat);
                    }
                    if (lat > duration_cast<nanoseconds>(deadline_after).count()) st->late++;
                    if (!d) st->error(s->id, "no DONE answer from mock");
                    else {
                        st->orphans += d->orphans;
                        if (!a) st->error(s->id, "search failed: " + failure);
                        else {
                            if (a->partial()) st->partial++;
                            std::unordered_set<std::string> uniq;
                            for (auto& r : s->routes)
                                for (auto& f : r.flights) uniq.insert(f.key().str());
                            st->keys += static_cast<long>(uniq.size());
                            for (auto& key : uniq)
                                if (d->answered_age_ms.count(key)) st->keys_answered++;
                            std::string why = check(*s, *a, *d);
                            if (!why.empty()) st->error(s->id, why);
                        }
                    }
                }
                st->arrive();
            });
        };
        try {
            agg->search(s, [finished](answer a) { finished(std::move(a), {}); });
        } catch (const std::exception& e) {
            finished(std::nullopt, e.what());
        }
    }
    std::string stats_at_end = control_.stats();
    st->arrive();
    {
        std::unique_lock lk(st->mu);
        if (!st->cv.wait_for(lk, 10s, [&] { return st->outstanding.load() == 0; }))
            st->errors.push_back("some searches never completed (10 s after the last start)");
    }
    sampling = false;
    sampler.join();
    agg.reset();

    result r;
    r.rate = rate;
    std::vector<long> lat;
    {
        std::lock_guard lk(st->mu);
        lat = st->lat_ns;
        r.errors = st->errors;
    }
    std::sort(lat.begin(), lat.end());
    auto pct = [&](double p) {
        if (lat.empty()) return 0.0;
        std::size_t i = std::min(lat.size() - 1, static_cast<std::size_t>(std::ceil(p * static_cast<double>(lat.size()))) - 1);
        return static_cast<double>(lat[i]) / 1e6;
    };
    r.measured = st->measured;
    int unanswered = r.measured - static_cast<int>(lat.size());
    r.p50 = pct(0.50);
    r.p99 = pct(0.99);
    r.max = lat.empty() ? 0 : static_cast<double>(lat.back()) / 1e6;
    r.late = st->late + unanswered;
    r.partial = st->partial;
    r.answered = 100.0 * static_cast<double>(st->keys_answered) / static_cast<double>(std::max(1L, st->keys.load()));
    r.wrong = st->wrong + unanswered;
    r.orphans = st->orphans;
    long reqs = json_long(stats_at_end, "requests") - json_long(stats_at_warmup, "requests");
    r.limit = json_long(control_.stats(), "limit") - json_long(stats_at_warmup, "limit");
    r.requests_per_search = r.measured == 0 ? 0 : static_cast<double>(reqs) / std::max(1.0, std::floor(rate * seconds));
    r.threads = peak_threads;
    r.rss_mb = peak_rss;
    return r;
}

bool holds(const result& r) {
    return r.measured > 0 && r.p99 <= 300 && r.late <= 0.01 * r.measured && r.answered >= min_answered_pct && r.wrong == 0 &&
           r.orphans == 0 && r.limit == 0;
}

void print(const result& r, bool header) {
    if (header)
        std::printf("%7s %7s %8s %8s %8s %6s %8s %9s %6s %8s %6s %9s %8s %7s\n", "rate", "n", "p50ms", "p99ms", "maxms", "late%",
                    "partial%", "answered%", "wrong", "orphans", "LIMIT", "req/srch", "threads", "rssMB");
    double m = std::max(1, r.measured);
    std::printf("%7.0f %7d %8.1f %8.1f %8.1f %6.2f %8.2f %9.2f %6d %8ld %6ld %9.1f %8ld %7ld   %s\n", r.rate, r.measured, r.p50,
                r.p99, r.max, 100.0 * r.late / m, 100.0 * r.partial / m, r.answered, r.wrong, r.orphans, r.limit,
                r.requests_per_search, r.threads, r.rss_mb, holds(r) ? "HOLDS" : "fails");
    std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
    std::map<std::string, std::string> a{{"--data", "../data"}, {"--host", "127.0.0.1"}, {"--port", "7700"},
                                         {"--seconds", "20"},   {"--warmup", "5"},       {"--connections", "8"},
                                         {"--impl", "student"}};
    std::map<std::string, std::string> opts;
    for (int i = 1; i < argc; i++) {
        std::string k = argv[i];
        if (k.rfind("--", 0) != 0 || i + 1 >= argc) { std::cerr << "bad argument: " << k << "\n"; return 2; }
        std::string v = argv[++i];
        if (k == "--opt") {
            auto eq = v.find('=');
            if (eq == std::string::npos) { std::cerr << "--opt expects key=value: " << v << "\n"; return 2; }
            opts[v.substr(0, eq)] = v.substr(eq + 1);
        } else a[k] = v;
    }
    if (!a.count("--rate") && !a.count("--sweep")) {
        std::cerr << "usage: run.sh --impl <name> (--rate R | --sweep R1,R2,...) [--seconds 20] [--warmup 5] [--opt key=value]\n";
        return 2;
    }
    try {
        harness h(a, opts);
        std::vector<double> rates;
        std::stringstream list(a.count("--sweep") ? a["--sweep"] : a["--rate"]);
        for (std::string r; std::getline(list, r, ',');) rates.push_back(std::stod(r));
        std::string optstr;
        for (auto& [k, v] : opts) optstr += (optstr.empty() ? "" : ", ") + k + "=" + v;
        std::printf("impl %s, %zu search templates, deadline 300 ms, %s s + %s s warm-up per rate, options {%s}\n",
                    a["--impl"].c_str(), h.n_templates(), a["--seconds"].c_str(), a["--warmup"].c_str(), optstr.c_str());
        bool header = true;
        std::vector<std::string> all_errors;
        for (double rate : rates) {
            result r = h.run(a["--impl"], rate, std::stod(a["--seconds"]), std::stod(a["--warmup"]));
            print(r, header);
            header = false;
            for (auto& e : r.errors) all_errors.push_back("rate " + std::to_string(static_cast<long>(rate)) + ": " + e);
            std::this_thread::sleep_for(1s);   // let the mock drain before the next rate
        }
        if (!all_errors.empty()) {
            std::printf("first problems:\n");
            for (std::size_t i = 0; i < all_errors.size() && i < 10; i++) std::printf("  %s\n", all_errors[i].c_str());
        }
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
