// Tiny test harness: one file, no dependency, prints ALL OK only when every
// check passed. Same shape as the coop-anything tools so the summary reads the
// same way in CI logs.
#pragma once

#include <cmath>
#include <type_traits>
#include <cstdio>
#include <string>
#include <vector>

namespace mt {

struct Test {
    const char* name;
    void (*fn)();
};

inline std::vector<Test>& tests() {
    static std::vector<Test> t;
    return t;
}
inline int& failures() {
    static int f = 0;
    return f;
}
inline int& checks() {
    static int c = 0;
    return c;
}
inline const char*& current() {
    static const char* c = "";
    return c;
}

struct Reg {
    Reg(const char* name, void (*fn)()) { tests().push_back({name, fn}); }
};

template <class A, class B>
void eqPrint(const A& a, const B& b) {
    if constexpr (std::is_arithmetic_v<A> && std::is_arithmetic_v<B>) {
        std::printf("  (%lld vs %lld)", static_cast<long long>(a), static_cast<long long>(b));
    } else {
        (void)a;
        (void)b;
    }
}

}  // namespace mt

#define TEST(name)                                        \
    static void test_##name();                            \
    static ::mt::Reg reg_##name(#name, test_##name);      \
    static void test_##name()

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++::mt::checks();                                                        \
        if (!(cond)) {                                                           \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);        \
            ++::mt::failures();                                                  \
        }                                                                        \
    } while (0)

#define CHECK_EQ(a, b)                                                                                   \
    do {                                                                                                 \
        ++::mt::checks();                                                                                \
        const auto& va = (a);                                                                            \
        const auto& vb = (b);                                                                            \
        if (!(va == vb)) {                                                                               \
            std::printf("  FAIL %s:%d  %s == %s", __FILE__, __LINE__, #a, #b);                           \
            ::mt::eqPrint(va, vb);                                                                    \
            std::printf("\n");                                                                           \
            ++::mt::failures();                                                                          \
        }                                                                                                \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                                   \
    do {                                                                                        \
        ++::mt::checks();                                                                       \
        const double va = (a);                                                                  \
        const double vb = (b);                                                                  \
        if (std::fabs(va - vb) > (eps)) {                                                       \
            std::printf("  FAIL %s:%d  |%s - %s| <= %s  (%f vs %f)\n", __FILE__, __LINE__, #a,  \
                        #b, #eps, va, vb);                                                      \
            ++::mt::failures();                                                                 \
        }                                                                                       \
    } while (0)

inline int runAll(const char* suite) {
    std::printf("== %s ==\n", suite);
    for (const ::mt::Test& t : ::mt::tests()) {
        const int before = ::mt::failures();
        const int checksBefore = ::mt::checks();
        t.fn();
        const bool ok = ::mt::failures() == before;
        std::printf("%s %s (%d checks)\n", ok ? "ok  " : "FAIL", t.name, ::mt::checks() - checksBefore);
    }
    std::printf("-- %d tests, %d checks, %d failures\n", static_cast<int>(::mt::tests().size()), ::mt::checks(),
                ::mt::failures());
    if (::mt::failures() == 0) {
        std::printf("ALL OK\n");
        return 0;
    }
    std::printf("FAILED\n");
    return 1;
}


