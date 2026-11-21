// Your stage 2 solution. Run: ./run.sh --impl student --rate 100
#include <stdexcept>

#include "aggregator.hpp"

namespace {

class student_aggregator : public lab3::aggregator {
public:
    explicit student_aggregator(lab3::kit& k) : kit_(k) {}

    void search(std::shared_ptr<const lab3::search> s, std::function<void(lab3::answer)> done) override {
        throw std::logic_error("TODO stage 2");
    }

private:
    lab3::kit& kit_;
};

LAB3_REGISTER("student", student_aggregator);

}  // namespace
