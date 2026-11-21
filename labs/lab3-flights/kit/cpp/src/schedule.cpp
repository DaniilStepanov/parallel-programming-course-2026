#include "schedule.hpp"

#include <cstring>
#include <fstream>
#include <stdexcept>

namespace lab3 {
namespace {
constexpr std::size_t header = 16, record = 20;

template <class T>
T read_le(const char* p) {   // the file is little-endian, as are x86 and ARM
    T v;
    std::memcpy(&v, p, sizeof v);
    return v;
}
}  // namespace

schedule schedule::open(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) throw std::runtime_error("cannot open " + path);
    schedule s;
    s.data_.resize(static_cast<std::size_t>(in.tellg()));
    in.seekg(0);
    in.read(s.data_.data(), static_cast<std::streamsize>(s.data_.size()));
    const char* b = s.data_.data();
    if (s.data_.size() < header || std::memcmp(b, "FLT1", 4) != 0) throw std::runtime_error(path + ": not a FLT1 file");
    s.n_airports = read_le<int32_t>(b + 4);
    s.n_airlines = read_le<int32_t>(b + 8);
    s.n_flights = read_le<int32_t>(b + 12);
    std::size_t p = header;
    for (int i = 0; i < s.n_airports; i++, p += 3) s.airports_.emplace_back(b + p, 3);
    for (int i = 0; i < s.n_airlines; i++, p += 2) s.airlines_.emplace_back(b + p, 2);
    s.records_ = p;
    return s;
}

flight schedule::get(int32_t id) const {
    if (id < 0 || id >= n_flights) throw std::out_of_range("flight id " + std::to_string(id));
    const char* r = data_.data() + records_ + static_cast<std::size_t>(id) * record;
    flight f;
    f.id = id;
    f.dep = read_le<int32_t>(r);
    f.arr = read_le<int32_t>(r + 4);
    f.from = airports_[read_le<uint16_t>(r + 8)];
    f.to = airports_[read_le<uint16_t>(r + 10)];
    f.airline = airlines_[read_le<uint16_t>(r + 12)];
    f.base_price = read_le<int32_t>(r + 16);
    return f;
}

}  // namespace lab3
