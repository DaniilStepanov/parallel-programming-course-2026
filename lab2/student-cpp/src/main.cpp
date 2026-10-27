// lab2 <target> <strategy> [arg] [log]
// strategies: first | random <seed> | randoms <count> | replay <trace> | dfs | bounded <K>
// add "log" at the end of replay/first/random to print the steps of the run.
#include <chrono>
#include <climits>
#include <cstdio>
#include <iostream>
#include <string>

#include "strategies.hpp"
#include "targets.hpp"

using namespace sched;

struct Outcome { std::string trace, failure; };

static Outcome run_once(const std::string& target, Strategy& st, bool log) {
    Scheduler s;
    auto t = targets::make(target);
    s.logging = log;
    t->setup(s);
    st.begin_run();
    s.run(st);
    if (log) for (auto& l : s.log) std::cout << "  " << l << "\n";
    std::string failure = !s.deadlock.empty() ? "DEADLOCK " + s.deadlock : t->check();
    return {s.trace_string(), failure};
}

static void single(const std::string& target, Strategy& st, bool log) {
    Outcome o = run_once(target, st, log);
    std::cout << "schedule " << o.trace << " -> " << (o.failure.empty() ? "ok" : o.failure) << "\n";
}

static void explore(const std::string& target, int bound) {
    Dfs dfs(bound);
    auto t0 = std::chrono::steady_clock::now();
    long explored = 0, fail = 0;
    Outcome first;
    do {
        Outcome o = run_once(target, dfs, false);
        explored++;
        if (!o.failure.empty()) { if (fail++ == 0) first = o; }
    } while (dfs.end_run());
    double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("explored %ld schedules, %ld fail  (%.2f s)\n", explored, fail, sec);
    if (fail) std::cout << "first failing: " << first.trace << " -> " << first.failure << "\n";
}

static void randoms(const std::string& target, int count) {
    int fail = 0;
    long first_seed = -1;
    for (int seed = 0; seed < count; seed++) {
        RandomPick r(seed);
        Outcome o = run_once(target, r, false);
        if (!o.failure.empty()) { fail++; if (first_seed < 0) first_seed = seed; }
    }
    std::printf("random: %d seeds, %d fail (%.1f%%)\n", count, fail, 100.0 * fail / count);
    if (first_seed >= 0) {
        RandomPick r(first_seed);
        Outcome o = run_once(target, r, false);
        std::cout << "first failing seed " << first_seed << ": " << o.trace << " -> " << o.failure << "\n";
        int same = 0;
        for (int i = 0; i < 10; i++) {
            Replay rp(o.trace);
            Outcome x = run_once(target, rp, false);
            if (x.trace == o.trace && x.failure == o.failure) same++;
        }
        std::cout << "replay " << o.trace << ": same failure " << same << "/10\n";
    }
}

int main(int argc, char** argv) {
    if (argc < 3) { std::cout << "usage: lab2 <target> <strategy> [arg] [log]\n"; return 1; }
    std::string target = argv[1], strategy = argv[2];
    bool log = std::string(argv[argc - 1]) == "log";
    if (strategy == "first") { FirstAlive s; single(target, s, log); }
    else if (strategy == "random") { RandomPick s(std::stoul(argv[3])); single(target, s, log); }
    else if (strategy == "replay") { Replay s(argv[3]); single(target, s, log); }
    else if (strategy == "randoms") randoms(target, std::stoi(argv[3]));
    else if (strategy == "dfs") explore(target, INT_MAX);
    else if (strategy == "bounded") explore(target, std::stoi(argv[3]));
    else { std::cout << "unknown strategy " << strategy << "\n"; return 1; }
}
