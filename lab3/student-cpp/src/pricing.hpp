// Live price and seat availability of a flight in one search, as the airline mock reports them.
// Integer-only, same numbers as lab3.Pricing in Java.
//   key     = (flightId << 32) ^ searchId
//   price   = base * (800 + (mix(key) >> 32) % 601) / 1000
//   noSeats = (mix(key ^ 0x5EA75EA75EA75EA7) >> 32) % 100 < 10
#pragma once
#include <cstdint>

namespace lab3::pricing {

inline uint64_t mix(uint64_t z) {
    z += 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

inline uint64_t key(int32_t flight_id, uint64_t search_id) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(flight_id)) << 32) ^ search_id;
}

inline int32_t price(int32_t flight_id, uint64_t search_id, int32_t base_price) {
    int64_t factor = 800 + static_cast<int64_t>((mix(key(flight_id, search_id)) >> 32) % 601);
    return static_cast<int32_t>(base_price * factor / 1000);
}

inline bool no_seats(int32_t flight_id, uint64_t search_id) {
    return (mix(key(flight_id, search_id) ^ 0x5EA75EA75EA75EA7ull) >> 32) % 100 < 10;
}

}  // namespace lab3::pricing
