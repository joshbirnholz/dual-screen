#pragma once

// A deliberately tiny check harness: the project has no test-framework
// dependency, and these tests only need "did this expression hold".

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace test {

inline int& failure_count() {
    static int failures = 0;
    return failures;
}

inline void report(bool passed, const char* expression, const char* file, int line,
                   const std::string& detail) {
    if (passed) {
        return;
    }
    ++failure_count();
    std::fprintf(stderr, "FAIL %s:%d: %s%s%s\n", file, line, expression,
                 detail.empty() ? "" : " -- ", detail.c_str());
}

inline bool nearly_equal(double a, double b, double tolerance = 1e-4) {
    return std::fabs(a - b) <= tolerance;
}

// Announced rather than silent: a test that could not run at all is not the
// same as one that passed, and the reason belongs in the output.
inline void skip(const std::string& reason) {
    std::fprintf(stderr, "SKIP: %s\n", reason.c_str());
}

inline int finish(const char* suite) {
    if (failure_count() == 0) {
        std::printf("ok - %s\n", suite);
        return 0;
    }
    std::fprintf(stderr, "not ok - %s (%d failures)\n", suite, failure_count());
    return 1;
}

}  // namespace test

namespace test {

// These take their arguments by value so each expression is evaluated exactly
// once — a check whose argument has a side effect (Sha1::finalize() resets the
// hasher) would otherwise report on a different call than the one it tested.
inline void check_equal_strings(const std::string& actual, const std::string& expected,
                                const char* expression, const char* file, int line) {
    report(actual == expected, expression, file, line,
           "got '" + actual + "', expected '" + expected + "'");
}

template <typename A, typename B>
void check_equal(const A& actual, const B& expected, const char* expression, const char* file,
                 int line) {
    report(actual == expected, expression, file, line,
           "got " + std::to_string(actual) + ", expected " + std::to_string(expected));
}

inline void check_near(double actual, double expected, const char* expression, const char* file,
                       int line) {
    report(nearly_equal(actual, expected), expression, file, line,
           "got " + std::to_string(actual) + ", expected " + std::to_string(expected));
}

}  // namespace test

#define CHECK(expression) \
    ::test::report((expression), #expression, __FILE__, __LINE__, std::string())

#define CHECK_MSG(expression, detail) \
    ::test::report((expression), #expression, __FILE__, __LINE__, (detail))

#define CHECK_EQ(actual, expected) \
    ::test::check_equal((actual), (expected), #actual " == " #expected, __FILE__, __LINE__)

#define CHECK_STR_EQ(actual, expected) \
    ::test::check_equal_strings((actual), (expected), #actual " == " #expected, __FILE__, __LINE__)

#define CHECK_NEAR(actual, expected) \
    ::test::check_near((actual), (expected), #actual " ~= " #expected, __FILE__, __LINE__)
