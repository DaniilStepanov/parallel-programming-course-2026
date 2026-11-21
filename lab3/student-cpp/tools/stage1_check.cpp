// Compares stage 1 output with the reference, ignoring ms=. C++ port of student-java/tools/Stage1Check.java.
//   out/stage1_check <your-output.txt> [expected-stage1.txt]
// Without the second argument it looks for ../../data/expected-stage1.txt next to the binary, then ./expected-stage1.txt.
// Exit code 0 if every query matches.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

std::string squeeze(const std::string& s) {   // trim + collapse whitespace runs into one space
    std::istringstream in(s);
    std::string out, w;
    while (in >> w) out += (out.empty() ? "" : " ") + w;
    return out;
}

// query id -> [header without ms, route lines...], in file order
std::vector<std::pair<std::string, std::vector<std::string>>> parse(const std::filesystem::path& file) {
    std::ifstream in(file);
    if (!in) throw std::runtime_error("cannot read " + file.string());
    static const std::regex ms(R"(\s+ms=\S+)");
    std::vector<std::pair<std::string, std::vector<std::string>>> m;
    std::vector<std::string>* cur = nullptr;
    for (std::string l; std::getline(in, l);) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        if (squeeze(l).empty()) continue;
        if (l.rfind("Q", 0) == 0) {
            std::string id = l.substr(0, l.find_first_of(" \t"));
            for (auto& e : m)
                if (e.first == id) e.second.clear();
            auto it = std::find_if(m.begin(), m.end(), [&](auto& e) { return e.first == id; });
            if (it == m.end()) { m.emplace_back(id, std::vector<std::string>{}); it = std::prev(m.end()); }
            cur = &it->second;
            cur->push_back(squeeze(std::regex_replace(l, ms, "")));
        } else if (cur) cur->push_back(squeeze(l));
    }
    return m;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: stage1_check <your-output.txt> [expected-stage1.txt]\n";
        return 2;
    }
    std::filesystem::path expected;
    if (argc > 2) expected = argv[2];
    else {
        std::error_code ec;
        auto self = std::filesystem::canonical("/proc/self/exe", ec);
        expected = ec ? std::filesystem::path() : self.parent_path() / ".." / ".." / "data" / "expected-stage1.txt";
        if (expected.empty() || !std::filesystem::exists(expected)) expected = "expected-stage1.txt";
    }
    try {
        auto want = parse(expected);
        auto got_list = parse(argv[1]);
        std::map<std::string, std::vector<std::string>> got(got_list.begin(), got_list.end());
        int ok = 0, bad = 0;
        for (auto& [id, w] : want) {
            auto it = got.find(id);
            if (it == got.end()) { std::cout << id << ": MISSING\n"; bad++; continue; }
            const auto& g = it->second;
            if (w[0] != g[0]) {
                std::cout << id << ": header differs\n  expected: " << w[0] << "\n  got:      " << g[0] << "\n";
                bad++;
                continue;
            }
            int line = -1;
            for (std::size_t i = 1; i < std::max(w.size(), g.size()); i++)
                if (i >= w.size() || i >= g.size() || w[i] != g[i]) { line = static_cast<int>(i); break; }
            if (line >= 0) {
                std::cout << id << ": top-10 line " << line << " differs\n  expected: "
                          << (line < static_cast<int>(w.size()) ? w[line] : "<none>") << "\n  got:      "
                          << (line < static_cast<int>(g.size()) ? g[line] : "<none>") << "\n";
                bad++;
            } else ok++;
        }
        std::printf("%d/%zu queries match\n", ok, want.size());
        return bad == 0 ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 2;
    }
}
