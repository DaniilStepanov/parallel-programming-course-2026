// Mini-jcstress, store buffering:  T1: x=1; r1=y    T2: y=1; r2=x
// (0,0) means each thread missed the other's write — forbidden under seq_cst.
// ministress <mode> <rounds> <cells>, mode: relaxed | acqrel | seqcst | naive
#include <atomic>
#include <barrier>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using std::memory_order;
static std::vector<std::atomic<int>> x, y;
static std::vector<int> r1, r2;

// The orders are template parameters on purpose: an order known only at run time makes
// GCC emit the strongest code (seq_cst, xchg on x86), and (0,0) would never appear.
template <memory_order ST, memory_order LD>
static void actor(std::vector<std::atomic<int>>& mine, std::vector<std::atomic<int>>& other,
                  std::vector<int>& r, int n) {
    for (int i = 0; i < n; i++) { mine[i].store(1, ST); r[i] = other[i].load(LD); }
}

template <memory_order ST, memory_order LD>
static void start(int cells, std::thread& t1, std::thread& t2, auto& loop) {
    t1 = std::thread([&, cells] { loop([&] { actor<ST, LD>(x, y, r1, cells); }); });
    t2 = std::thread([&, cells] { loop([&] { actor<ST, LD>(y, x, r2, cells); }); });
}

int main(int argc, char** argv) {
    std::string mode = argv[1];
    int rounds = std::atoi(argv[2]);
    bool naive = mode == "naive";                       // a barrier around every single pair
    int cells = naive ? 1 : std::atoi(argv[3]);
    if (naive) rounds *= std::atoi(argv[3]);
    x = std::vector<std::atomic<int>>(cells); y = std::vector<std::atomic<int>>(cells);
    r1.assign(cells, 0); r2.assign(cells, 0);
    long hist[4] = {};                                  // index = r1*2 + r2
    std::barrier go(3), done(3);
    auto loop = [&](auto body) { for (int k = 0; k < rounds; k++) { go.arrive_and_wait(); body(); done.arrive_and_wait(); } };
    std::thread t1, t2;
    using std::memory_order_relaxed, std::memory_order_release, std::memory_order_acquire, std::memory_order_seq_cst;
    if (mode == "acqrel")      start<memory_order_release, memory_order_acquire>(cells, t1, t2, loop);
    else if (mode == "seqcst") start<memory_order_seq_cst, memory_order_seq_cst>(cells, t1, t2, loop);
    else                       start<memory_order_relaxed, memory_order_relaxed>(cells, t1, t2, loop);
    for (int k = 0; k < rounds; k++) {
        go.arrive_and_wait();
        done.arrive_and_wait();
        for (int i = 0; i < cells; i++) { hist[r1[i] * 2 + r2[i]]++; x[i] = 0; y[i] = 0; }
    }
    t1.join(); t2.join();
    long total = hist[0] + hist[1] + hist[2] + hist[3];
    std::printf("mode=%s samples=%ld\n", mode.c_str(), total);
    const char* names[] = {"(0,0)", "(0,1)", "(1,0)", "(1,1)"};
    for (int k = 0; k < 4; k++)
        std::printf("  %s %14ld  %7.3f%%%s\n", names[k], hist[k], 100.0 * hist[k] / total, k == 0 ? "  forbidden under seq_cst" : "");
}
