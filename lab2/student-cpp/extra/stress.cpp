// Stage 0: withdraw V1 or V2 with real threads and a real std::mutex, no scheduler.
// stress <runs> [none|yield|sleep] [v1|v2]  — what to put into window(), which version.
#include <barrier>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

static long balance;
static std::mutex mu;

static bool v2;

static void window_(const char* window) {
    if (!strcmp(window, "yield")) std::this_thread::yield();
    else if (!strcmp(window, "sleep")) std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

static bool withdraw(long amt, const char* window) {
    if (v2) {
        std::lock_guard g(mu);
        if (balance < amt) return false;
        window_(window);
        balance -= amt;
        return true;
    }
    bool ok;
    { std::lock_guard g(mu); ok = balance >= amt; }
    window_(window);
    if (ok) { std::lock_guard g(mu); balance -= amt; }
    return ok;
}

int main(int argc, char** argv) {
    int runs = std::atoi(argv[1]);
    const char* window = argc > 2 ? argv[2] : "none";
    v2 = argc > 3 && !strcmp(argv[3], "v2");
    std::barrier start(3), end(3);
    auto body = [&] { for (int i = 0; i < runs; i++) { start.arrive_and_wait(); withdraw(100, window); end.arrive_and_wait(); } };
    std::thread a(body), b(body);
    int bad = 0;
    for (int i = 0; i < runs; i++) {
        balance = 100;
        start.arrive_and_wait();      // both threads start withdraw together
        end.arrive_and_wait();        // both withdraws finished
        if (balance < 0) bad++;
    }
    a.join(); b.join();
    std::printf("stress %s, window=%s: %d runs, %d overdrafts (%.2f%%)\n", v2 ? "V2" : "V1", window, runs, bad, 100.0 * bad / runs);
}
