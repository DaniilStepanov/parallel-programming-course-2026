#include <algorithm>
#include <numeric>
#include <stdexcept>

#include "aggregator.hpp"

namespace lab3 {

answer answer::of(const search& s, std::vector<route_result> results) {
    if (results.size() != s.routes.size()) throw std::invalid_argument("one result per route expected");
    answer a;
    a.routes = std::move(results);
    std::vector<int> priced;
    for (int i = 0; i < static_cast<int>(a.routes.size()); i++)
        if (a.routes[i].st == status::priced) priced.push_back(i);
    std::sort(priced.begin(), priced.end(), [&](int x, int y) {
        if (a.routes[x].price != a.routes[y].price) return a.routes[x].price < a.routes[y].price;
        int dx = s.routes[x].duration_minutes(), dy = s.routes[y].duration_minutes();
        if (dx != dy) return dx < dy;
        return x < y;
    });
    if (priced.size() > top_size) priced.resize(top_size);
    a.top = std::move(priced);
    return a;
}

bool answer::partial() const {
    return std::any_of(routes.begin(), routes.end(), [](auto& r) { return r.st == status::unpriced; });
}

int kit::limit(const std::string& airline) const {
    auto it = limits.find(airline);
    if (it == limits.end()) throw std::invalid_argument("unknown airline " + airline);
    return it->second;
}

int kit::int_option(const std::string& name, int fallback) const {
    auto it = options.find(name);
    return it == options.end() ? fallback : std::stoi(it->second);
}

std::map<std::string, factory>& registry() {
    static std::map<std::string, factory> r;
    return r;
}

}  // namespace lab3
