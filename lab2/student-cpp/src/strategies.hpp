#pragma once
// Стратегии выбора следующего потока.
// FirstAlive — этап 1, Replay — этап 2, RandomPick — этап 3, Dfs — этап 4 (ограничение bound — этап 5).
// enabled — номера потоков, которые могут сделать шаг, по возрастанию; last — прошлый выбранный (-1 в начале).
#include <cstdint>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "strategy.hpp"

namespace sched {

// Всегда первый поток из enabled.
struct FirstAlive : Strategy {
    int pick(const std::vector<int>&, int) override { throw std::logic_error("TODO этап 1: FirstAlive::pick"); }
};

// Проигрывает трассу: на i-м решении выбирает поток trace[i] ('A' -> 0, 'B' -> 1).
struct Replay : Strategy {
    explicit Replay(std::string) {}
    // Сбросить позицию в трассе перед прогоном.
    void begin_run() override {}
    int pick(const std::vector<int>&, int) override { throw std::logic_error("TODO этап 2: Replay::pick"); }
};

// Случайный поток: enabled[rnd() % enabled.size()], rnd — std::mt19937(seed).
struct RandomPick : Strategy {
    explicit RandomPick(uint32_t) {}
    int pick(const std::vector<int>&, int) override { throw std::logic_error("TODO этап 3: RandomPick::pick"); }
};

// Stateless DFS по дереву выборов: каждый прогон с нуля, повторяет префикс прошлого и берёт следующий вариант.
// bound — максимум вытеснений (этап 5); для dfs runner передаёт INT_MAX.
struct Dfs : Strategy {
    explicit Dfs(int) {}
    // Подготовиться к новому прогону (позиция в пути, счётчик вытеснений).
    void begin_run() override {}
    int pick(const std::vector<int>&, int) override { throw std::logic_error("TODO этап 4: Dfs::pick"); }
    // Выбрать следующий путь; false — дерево обойдено.
    bool end_run() override { throw std::logic_error("TODO этап 4: Dfs::end_run"); }
};

}  // namespace sched
