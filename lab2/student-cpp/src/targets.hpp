#pragma once
// Programs under test. setup() spawns threads; check() returns "" if the run is fine.
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "sched_mutex.hpp"
#include "scheduler.hpp"
#include "shared.hpp"

namespace targets {
using namespace sched;

struct Target {
    virtual ~Target() = default;
    virtual void setup(Scheduler& s) = 0;
    virtual std::string check() = 0;
};

// ---------------- withdraw V0 / V1 / V2 ----------------
struct Withdraw : Target {
    int version;
    shared<long> shared_balance{"balance", 100};   // V0: no lock
    long balance = 100;                             // V1, V2: under mu
    std::unique_ptr<sched_mutex> mu;
    bool ok[2] = {false, false};

    explicit Withdraw(int v) : version(v) {}

    bool withdraw(long amt) {
        if (version == 0) {
            bool ok = shared_balance >= amt;
            point();
            if (ok) shared_balance -= amt;
            return ok;
        }
        if (version == 1) {
            bool ok;
            { sched_guard g(*mu); ok = balance >= amt; }    // check
            point();
            if (ok) { sched_guard g(*mu); balance -= amt; }  // act
            return ok;
        }
        sched_guard g(*mu);
        if (balance < amt) return false;
        point();
        balance -= amt;
        return true;
    }

    void setup(Scheduler& s) override {
        mu = std::make_unique<sched_mutex>(s, "mu");
        for (int i = 0; i < 2; i++) s.spawn([this, i] { ok[i] = withdraw(100); });
    }

    std::string check() override {
        long b = version == 0 ? shared_balance.peek() : balance;
        int wins = ok[0] + ok[1];
        std::string state = "balance=" + std::to_string(b) + " results=" + (ok[0] ? "true" : "false") + "," +
                            (ok[1] ? "true" : "false");
        if (b < 0) return "OVERDRAFT " + state;
        if (100 - b != wins * 100) return "LOST UPDATE " + state;
        return "";
    }
};

// ---------------- transfer: lock order ABBA ----------------
struct Transfer : Target {
    std::unique_ptr<sched_mutex> a, b;
    long bal_a = 100, bal_b = 100;

    void transfer(sched_mutex& from, sched_mutex& to, bool a_to_b) {
        from.lock();
        point();
        to.lock();
        if (a_to_b) { bal_a -= 10; bal_b += 10; } else { bal_b -= 10; bal_a += 10; }
        to.unlock();
        from.unlock();
    }

    void setup(Scheduler& s) override {
        a = std::make_unique<sched_mutex>(s, "a");
        b = std::make_unique<sched_mutex>(s, "b");
        s.spawn([this] { transfer(*a, *b, true); });
        s.spawn([this] { transfer(*b, *a, false); });
    }

    std::string check() override { return bal_a + bal_b == 200 ? "" : "MONEY LOST"; }
};

// ---------------- double buffering from lab 1, one bucket ----------------
// collector-<variant>-<writers>x<n>, variant: nostep3 | plain | acqrel | ok
struct Collector : Target {
    bool step3, plain_inside, acqrel;
    int writers, n;
    shared<int> active{"active", 0};
    std::vector<std::unique_ptr<shared<int>>> inside;
    std::vector<int> inside_plain;
    std::vector<std::unique_ptr<shared<long>>> cnt;   // cnt[w*2 + b]
    long global = 0;

    Collector(const std::string& variant, int writers, int n) : writers(writers), n(n) {
        step3 = variant == "ok" || variant == "acqrel";
        plain_inside = variant == "plain";
        acqrel = variant == "acqrel";
        inside_plain.assign(writers, -1);
        for (int w = 0; w < writers; w++) {
            std::string W = "W" + std::to_string(w + 1);
            inside.push_back(std::make_unique<shared<int>>("inside[" + W + "]", -1));
            for (int b = 0; b < 2; b++)
                cnt.push_back(std::make_unique<shared<long>>("cnt[" + W + "][" + std::to_string(b) + "]", 0));
        }
    }

    int get_inside(int w) { return plain_inside ? inside_plain[w] : inside[w]->peek(); }
    void set_inside(int w, int v) {
        if (plain_inside) { inside_plain[w] = v; return; }
        if (acqrel) inside[w]->set_release(v); else *inside[w] = v;
    }

    void record(int w) {
        int b;
        while (true) {
            b = acqrel ? active.get_acquire() : int(active);
            set_inside(w, b);
            if (!step3 || active == b) break;    // step 3: did the reader flip meanwhile?
            set_inside(w, -1);
        }
        shared<long>& c = *cnt[w * 2 + b];
        c += 1;
        set_inside(w, -1);
    }

    void snapshot() {
        int old = active;
        active = 1 - old;
        for (int w = 0; w < writers; w++) await([this, w, old] { return get_inside(w) != old; });
        for (int w = 0; w < writers; w++) {
            shared<long>& c = *cnt[w * 2 + old];
            global += c;
            c = 0;
        }
    }

    void setup(Scheduler& s) override {
        for (int w = 0; w < writers; w++) s.spawn([this, w] { for (int i = 0; i < n; i++) record(w); });
        s.spawn([this] { snapshot(); });
    }

    std::string check() override {
        long total = global;
        for (auto& c : cnt) total += c->peek();
        long expected = long(writers) * n;
        if (total < expected)
            return "LOST " + std::to_string(expected - total) + " record(s): counted " + std::to_string(total) +
                   " of " + std::to_string(expected);
        if (total > expected)
            return "DUPLICATED " + std::to_string(total - expected) + ": counted " + std::to_string(total) + " of " +
                   std::to_string(expected);
        return "";
    }
};

inline std::unique_ptr<Target> make(const std::string& name) {
    if (name == "withdraw-v0") return std::make_unique<Withdraw>(0);
    if (name == "withdraw-v1") return std::make_unique<Withdraw>(1);
    if (name == "withdraw-v2") return std::make_unique<Withdraw>(2);
    if (name == "transfer") return std::make_unique<Transfer>();
    const std::string pre = "collector-";
    if (name.rfind(pre, 0) == 0) {
        std::string spec = name.substr(pre.size());
        std::string variant = spec.substr(0, spec.find('-'));
        int w = 1, n = 1;
        if (auto d = spec.find('-'); d != std::string::npos) {
            std::string wn = spec.substr(d + 1);
            w = std::stoi(wn.substr(0, wn.find('x')));
            n = std::stoi(wn.substr(wn.find('x') + 1));
        }
        if (variant != "nostep3" && variant != "plain" && variant != "acqrel" && variant != "ok")
            throw std::invalid_argument(variant);
        return std::make_unique<Collector>(variant, w, n);
    }
    throw std::invalid_argument("unknown target " + name);
}

}  // namespace targets
