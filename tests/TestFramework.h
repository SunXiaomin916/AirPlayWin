#pragma once

#include <cmath>
#include <stdexcept>
#include <string>
#include <string_view>

namespace airplaywin::tests {

inline void Expect(const bool condition,
                   const std::string_view expression,
                   const std::string_view file,
                   const int line) {
    if (!condition) {
        throw std::runtime_error(std::string{file} + ":" + std::to_string(line) +
                                 " expectation failed: " + std::string{expression});
    }
}

inline void ExpectNear(const double actual,
                       const double expected,
                       const double tolerance,
                       const std::string_view expression,
                       const std::string_view file,
                       const int line) {
    if (std::abs(actual - expected) > tolerance) {
        throw std::runtime_error(std::string{file} + ":" + std::to_string(line) +
                                 " expectation failed: " + std::string{expression} +
                                 " actual=" + std::to_string(actual) +
                                 " expected=" + std::to_string(expected) +
                                 " tolerance=" + std::to_string(tolerance));
    }
}

}  // namespace airplaywin::tests

#define APW_EXPECT(expression) \
    ::airplaywin::tests::Expect((expression), #expression, __FILE__, __LINE__)
#define APW_EXPECT_NEAR(actual, expected, tolerance) \
    ::airplaywin::tests::ExpectNear((actual), (expected), (tolerance), #actual, __FILE__, __LINE__)
