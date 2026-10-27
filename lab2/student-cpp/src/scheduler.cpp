// Этап 1–2: реализация Scheduler, point() и await().
// Сейчас все методы бросают исключение «TODO»: заполните их по заданию этапов 1 и 2.
// Деструктор должен дождаться (join) всех запущенных потоков, в том числе после deadlock.
#include "scheduler.hpp"

#include <stdexcept>

namespace sched {

Scheduler::Scheduler() {}

Scheduler::~Scheduler() {}

void Scheduler::spawn(std::function<void()>) {
    throw std::logic_error("TODO этап 1: Scheduler::spawn");
}

void Scheduler::run(Strategy&) {
    throw std::logic_error("TODO этап 1: Scheduler::run");
}

std::string Scheduler::trace_string() const {
    throw std::logic_error("TODO этап 1: Scheduler::trace_string");
}

void point() {
    throw std::logic_error("TODO этап 1: sched::point");
}

void await(std::function<bool()>) {
    throw std::logic_error("TODO этап 1: sched::await");
}

}  // namespace sched
