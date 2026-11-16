#pragma once

// Minimal dependency-free test harness for the core unit tests.

#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace plm_test {

struct Case {
    const char* name;
    std::function<void()> body;
};

std::vector<Case>& registry();
void fail(const char* file, int line, const std::string& message);

struct Registrar {
    Registrar(const char* name, std::function<void()> body) { registry().push_back({name, std::move(body)}); }
};

} // namespace plm_test

#define PLM_CONCAT_INNER(a, b) a##b
#define PLM_CONCAT(a, b) PLM_CONCAT_INNER(a, b)
#define TEST_CASE(name)                                                                                     \
    static void PLM_CONCAT(plm_test_fn_, __LINE__)();                                                       \
    static plm_test::Registrar PLM_CONCAT(plm_test_reg_, __LINE__)(name, PLM_CONCAT(plm_test_fn_, __LINE__)); \
    static void PLM_CONCAT(plm_test_fn_, __LINE__)()

#define CHECK(expr)                                                \
    do {                                                           \
        if (!(expr)) plm_test::fail(__FILE__, __LINE__, #expr);    \
    } while (0)

#define CHECK_EQ(actual, expected)                                                                  \
    do {                                                                                            \
        const auto& plm_a = (actual);                                                               \
        const auto& plm_e = (expected);                                                             \
        if (!(plm_a == plm_e)) plm_test::fail(__FILE__, __LINE__, #actual " == " #expected);         \
    } while (0)
