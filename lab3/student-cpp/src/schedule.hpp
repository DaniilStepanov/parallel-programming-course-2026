// schedule.bin: flights of the timetable (format in the lab README). Read-only after open(), thread-safe.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace lab3 {

// What one airline request asks: all flights of `airline` from `from` to `to` departing on `day`.
struct price_key {
    std::string airline, from, to;
    int day = 0;

    bool operator==(const price_key&) const = default;
    // "AL:FROM:TO:D", the form the mock uses in its DONE reply
    std::string str() const { return airline + ":" + from + ":" + to + ":" + std::to_string(day); }
};

// One flight. Times are minutes from 2026-12-01 00:00; base_price is the schedule price.
struct flight {
    int32_t id = 0;
    std::string airline, from, to;
    int32_t dep = 0, arr = 0, base_price = 0;

    // day of departure: the day the airline uses for this segment
    int day() const { return dep / 1440; }
    // the airline request that returns the live price of this flight (and of all flights of the airline on this segment and day)
    price_key key() const { return {airline, from, to, day()}; }
};

class schedule {
public:
    static schedule open(const std::string& path);

    int32_t n_airports = 0, n_airlines = 0, n_flights = 0;

    flight get(int32_t flight_id) const;
    const std::string& airport_code(int id) const { return airports_[id]; }
    const std::string& airline_code(int id) const { return airlines_[id]; }

private:
    std::vector<char> data_;
    std::vector<std::string> airports_, airlines_;
    std::size_t records_ = 0;
};

}  // namespace lab3

template <>
struct std::hash<lab3::price_key> {
    std::size_t operator()(const lab3::price_key& k) const noexcept {
        std::size_t h = std::hash<std::string>{}(k.airline);
        h = h * 31 + std::hash<std::string>{}(k.from);
        h = h * 31 + std::hash<std::string>{}(k.to);
        return h * 31 + static_cast<std::size_t>(k.day);
    }
};
