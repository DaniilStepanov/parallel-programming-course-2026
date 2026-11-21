// Lab 3 data generator, C++ port of student-java/tools/Gen.java. Deterministic by seed; for the same options it writes
// byte-identical schedule.bin, queries.txt and airlines.conf (it reproduces java.util.SplittableRandom).
//   out/gen [--seed 2026] [--out ../data] [--per-day 1200000] [--alpha 1.6]
// searches.txt (stage 2 input) is not generated here: it ships with the kit.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr int DAYS = 8, N_AIRPORTS = 400, N_HUBS = 8;
double ALPHA = 1.6;

const char* REAL_CODES =
    "IST FRA DXB AMS CDG DOH LHR MAD "
    "LED LIS SVO VKO BER VIE PRG WAW HEL CPH OSL ARN ZRH GVA BCN FCO MXP ATH BUD OTP SOF BEG "
    "TLV CAI JED RUH AUH KWI BAH MCT TAS ALA TBS EVN GYD KBP MSQ RIX VNO TLL DUB BRU LYS NCE "
    "MRS HAM DUS CGN STR LEJ NUE BLQ VCE NAP PMO OPO FAO AGP PMI ALC SVQ VLC BIO KRK GDN WRO KTW";
const char* AIRLINE_CODES =
    "TK LH EK KL AF QR BA IB "
    "SU TP U6 S7 FZ PC W6 FR U2 LX OS SK AY LO OK BT A3 RO FB JU MS RJ ME PS HY KC J2 VY UX AZ G9 DY";

std::vector<std::string> split(const char* s) {
    std::istringstream in(s);
    std::vector<std::string> r;
    for (std::string w; in >> w;) r.push_back(w);
    return r;
}

// java.util.SplittableRandom (JDK 17+), the methods Gen.java uses.
class splittable_random {
public:
    explicit splittable_random(int64_t seed) : seed_(static_cast<uint64_t>(seed)) {}

    int32_t next_int() { return mix32(next_seed()); }
    int64_t next_long() { return static_cast<int64_t>(mix64(next_seed())); }
    double next_double() { return static_cast<double>(static_cast<uint64_t>(next_long()) >> 11) * 0x1.0p-53; }
    bool next_boolean() { return next_int() < 0; }

    // RandomSupport.boundedNextInt
    int32_t next_int(int32_t bound) {
        int32_t r = next_int();
        int32_t m = bound - 1;
        if ((bound & m) == 0) return r & m;
        for (uint32_t u = static_cast<uint32_t>(r) >> 1;; u = static_cast<uint32_t>(next_int()) >> 1) {
            r = static_cast<int32_t>(u % static_cast<uint32_t>(bound));
            // Java: u + m - r < 0 in int arithmetic (overflow means "reject")
            if (static_cast<int32_t>(u + static_cast<uint32_t>(m) - static_cast<uint32_t>(r)) >= 0) return r;
        }
    }

private:
    static constexpr uint64_t GOLDEN_GAMMA = 0x9e3779b97f4a7c15ull;
    uint64_t seed_;

    uint64_t next_seed() { return seed_ += GOLDEN_GAMMA; }
    static uint64_t mix64(uint64_t z) {
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }
    static int32_t mix32(uint64_t z) {
        z = (z ^ (z >> 33)) * 0x62a9d9ed799705f5ull;
        return static_cast<int32_t>(((z ^ (z >> 28)) * 0xcb24d0a5c88c35b3ull) >> 32);
    }
};

uint64_t mix(uint64_t z) {
    z += 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

double unit(int64_t seed, int64_t k) {
    return static_cast<double>(mix(static_cast<uint64_t>(seed * 31 + k)) >> 11) * 0x1.0p-53;
}

// Arrays.binarySearch(double[], key) followed by Gen.pick's "i >= 0 ? i : -i - 1"
int pick(const std::vector<double>& cum, splittable_random& rnd) {
    double r = rnd.next_double() * cum.back();
    int lo = 0, hi = static_cast<int>(cum.size()) - 1;
    while (lo <= hi) {
        int mid = static_cast<int>(static_cast<unsigned>(lo + hi) >> 1);
        double v = cum[mid];
        if (v < r) lo = mid + 1;
        else if (v > r) hi = mid - 1;
        else return mid;   // values here are never NaN or -0.0, so plain comparison matches Double.compare
    }
    return lo;
}

int pair_airline(int a, int b, int64_t seed, int slot, int n_airlines) {
    int lo = std::min(a, b), hi = std::max(a, b);
    uint64_t h = mix(static_cast<uint64_t>(seed) ^ (static_cast<uint64_t>(lo) << 20 | static_cast<uint64_t>(hi)));
    int served = 1 + static_cast<int>(h % 3);
    slot %= served;
    int hub = a < N_HUBS ? a : b < N_HUBS ? b : -1;
    if (slot == 0 && hub >= 0) return hub;
    return N_HUBS + static_cast<int>(mix(h + static_cast<uint64_t>(slot)) % static_cast<uint64_t>(n_airlines - N_HUBS));
}

void write_queries(std::ostream& w, const std::vector<std::string>& codes, splittable_random& rnd, int count,
                   int from_lo, int from_hi, int to_lo, int to_hi, std::set<std::pair<int, int>>& seen) {
    for (int k = 0; k < count;) {
        int a = from_lo + rnd.next_int(from_hi - from_lo);
        int b = to_lo + rnd.next_int(to_hi - to_lo);
        if (a == b || !seen.insert({a, b}).second) continue;
        int day = rnd.next_int(7);
        w << codes[a] << ' ' << codes[b] << ' ' << day << '\n';
        k++;
    }
}

template <class T>
void put_le(std::string& buf, T v) {
    char b[sizeof(T)];
    std::memcpy(b, &v, sizeof v);   // x86 and ARM are little-endian, as is the file
    buf.append(b, sizeof b);
}

// Java's Math.round(double) for the positive, moderate values used here
int64_t java_round(double x) { return static_cast<int64_t>(std::floor(x + 0.5)); }

}  // namespace

int main(int argc, char** argv) {
    int64_t seed = 2026;
    std::filesystem::path out = ".";
    int per_day = 1'200'000;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (i + 1 >= argc) { std::cerr << "missing value for " << a << "\n"; return 2; }
        if (a == "--seed") seed = std::stoll(argv[++i]);
        else if (a == "--out") out = argv[++i];
        else if (a == "--per-day") per_day = std::stoi(argv[++i]);
        else if (a == "--alpha") ALPHA = std::stod(argv[++i]);
        else { std::cerr << "unknown option " << a << "\n"; return 2; }
    }
    std::filesystem::create_directories(out);
    splittable_random rnd(seed);
    const auto real = split(REAL_CODES);
    const auto airlines = split(AIRLINE_CODES);

    std::vector<std::string> codes(N_AIRPORTS);
    std::set<std::string> used;
    for (int i = 0; i < N_AIRPORTS; i++) {
        std::string c;
        if (i < static_cast<int>(real.size())) c = real[i];
        else do {
            c.clear();
            for (int k = 0; k < 3; k++) c += static_cast<char>('A' + rnd.next_int(26));
        } while (used.count(c));
        used.insert(c);
        codes[i] = c;
    }
    std::vector<double> x(N_AIRPORTS), y(N_AIRPORTS);
    for (int i = 0; i < N_AIRPORTS; i++) {
        x[i] = rnd.next_double() * 6000;
        y[i] = rnd.next_double() * 4000;
    }

    std::vector<double> cum(N_AIRPORTS);
    double s = 0;
    for (int i = 0; i < N_AIRPORTS; i++) cum[i] = s += i < N_HUBS ? 0 : 1.0 / std::pow(i - N_HUBS + 3, ALPHA);

    const int n_al = static_cast<int>(airlines.size());
    std::vector<double> al_factor(n_al);
    for (int a = 0; a < n_al; a++) al_factor[a] = 0.75 + 0.6 * rnd.next_double();

    // One day template.
    std::vector<int32_t> t_from(per_day), t_to(per_day), t_dep(per_day), t_dur(per_day), t_al(per_day), t_no(per_day),
        t_base(per_day);
    std::vector<int32_t> next_no(n_al);
    for (int a = 0; a < n_al; a++) next_no[a] = 100 + rnd.next_int(200);
    for (int i = 0; i < per_day; i++) {
        int a, b;
        if (rnd.next_double() < 0.7) {
            a = rnd.next_int(N_HUBS);
            b = rnd.next_double() < 0.25 ? rnd.next_int(N_HUBS) : pick(cum, rnd);
            while (b == a) b = pick(cum, rnd);
            if (rnd.next_boolean()) std::swap(a, b);
        } else {
            a = pick(cum, rnd);
            do b = pick(cum, rnd); while (b == a);
        }
        double dist = std::hypot(x[a] - x[b], y[a] - y[b]);
        int al = pair_airline(a, b, seed, rnd.next_int(3), n_al);
        t_from[i] = a;
        t_to[i] = b;
        t_dep[i] = rnd.next_int(1440);
        t_dur[i] = 45 + static_cast<int32_t>(dist / 750 * 60);
        t_al[i] = al;
        t_no[i] = next_no[al]++ & 0xFFFF;
        t_base[i] = static_cast<int32_t>((1500 + dist * 3.5) * al_factor[al]);
    }

    const int n = per_day * DAYS;
    std::vector<int32_t> order(n);
    std::iota(order.begin(), order.end(), 0);
    for (int i = n - 1; i > 0; i--) std::swap(order[i], order[rnd.next_int(i + 1)]);

    const auto sched = out / "schedule.bin";
    {
        std::ofstream f(sched, std::ios::binary | std::ios::trunc);
        if (!f) { std::cerr << "cannot write " << sched << "\n"; return 1; }
        std::string buf = "FLT1";
        put_le<int32_t>(buf, N_AIRPORTS);
        put_le<int32_t>(buf, n_al);
        put_le<int32_t>(buf, n);
        for (const auto& c : codes) buf += c;
        for (const auto& c : airlines) buf += c;
        buf.reserve(20 * 65536 + buf.size());
        for (int k = 0; k < n; k++) {
            int src = order[k], day = src / per_day, i = src % per_day;
            int32_t dep = day * 1440 + t_dep[i];
            // the same flight costs a bit differently on different days
            double day_factor = 0.85 + 0.4 * unit(seed, src);
            int32_t price = static_cast<int32_t>(java_round(t_base[i] * day_factor / 10.0)) * 10;
            put_le<int32_t>(buf, dep);
            put_le<int32_t>(buf, dep + t_dur[i]);
            put_le<int16_t>(buf, static_cast<int16_t>(t_from[i]));
            put_le<int16_t>(buf, static_cast<int16_t>(t_to[i]));
            put_le<int16_t>(buf, static_cast<int16_t>(t_al[i]));
            put_le<int16_t>(buf, static_cast<int16_t>(t_no[i]));
            put_le<int32_t>(buf, price);
            if (buf.size() >= 20 * 65536) { f.write(buf.data(), static_cast<std::streamsize>(buf.size())); buf.clear(); }
        }
        f.write(buf.data(), static_cast<std::streamsize>(buf.size()));
    }

    {
        std::ofstream w(out / "queries.txt", std::ios::trunc);
        std::set<std::pair<int, int>> seen;
        // heavy: a small airport to a hub or a large airport -> few first flights, each opening a huge subtree
        write_queries(w, codes, rnd, 6, 330, N_AIRPORTS, 0, N_HUBS, seen);
        write_queries(w, codes, rnd, 4, 330, N_AIRPORTS, N_HUBS, N_HUBS + 12, seen);
        // light: small to small
        write_queries(w, codes, rnd, 10, 250, N_AIRPORTS, 250, N_AIRPORTS, seen);
    }

    {
        std::ofstream w(out / "airlines.conf", std::ios::trunc);
        w << "# AL limit medianMs sigma tailProb tailMs errProb\n";
        int tail_a = N_HUBS + 3 + rnd.next_int(10);
        int tail_b = N_HUBS + 15 + rnd.next_int(10);
        for (int a = 0; a < n_al; a++) {
            int limit = a < N_HUBS ? 150 + 10 * rnd.next_int(6) : 20 + 10 * rnd.next_int(10);
            int median = 30 + 5 * rnd.next_int(19);
            double sigma = 0.3 + 0.05 * rnd.next_int(7);
            double tail_prob = a == tail_a ? 0.08 : a == tail_b ? 0.04 : 0.0;
            int tail_ms = a == tail_a ? 3000 : a == tail_b ? 2000 : 0;
            double err = 0.0;
            if (rnd.next_double() < 0.4) err = 0.01 * (1 + rnd.next_int(5));
            char line[128];
            std::snprintf(line, sizeof line, "%s %d %d %.2f %.2f %d %.2f\n", airlines[a].c_str(), limit, median, sigma,
                          tail_prob, tail_ms, err);
            w << line;
        }
    }
    std::printf("flights=%d airports=%d airlines=%d file=%.1f MB\n", n, N_AIRPORTS, n_al,
                static_cast<double>(std::filesystem::file_size(sched)) / 1e6);
}
