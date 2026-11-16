#include "test.h"

namespace plm_test {

namespace {
int failures_in_case = 0;
}

std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

void fail(const char* file, int line, const std::string& message) {
    ++failures_in_case;
    std::cerr << "  " << file << ":" << line << ": CHECK failed: " << message << "\n";
}

} // namespace plm_test

int main() {
    int failed = 0;
    for (const auto& c : plm_test::registry()) {
        plm_test::failures_in_case = 0;
        c.body();
        const bool ok = plm_test::failures_in_case == 0;
        std::cout << (ok ? "PASS " : "FAIL ") << c.name << "\n";
        failed += ok ? 0 : 1;
    }
    std::cout << plm_test::registry().size() - failed << "/" << plm_test::registry().size() << " passed\n";
    return failed == 0 ? 0 : 1;
}
