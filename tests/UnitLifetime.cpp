#include <ostream>
#include <string>
#include <string_view>

#include "doctest.h"
#include "librazobill/Lifetime.hpp"

namespace {
struct RZ_GSL_POINTER TestView {
    std::string_view sv;
};

struct RZ_GSL_OWNER TestOwner {
    std::string data;
    std::string_view view() const RZ_LIFETIMEBOUND { return data; }
};

TestView createView(std::string_view s RZ_LIFETIMEBOUND) {
    return TestView{s};
}
} // namespace

TEST_CASE("lifetime annotations compile cleanly and preserve semantics") {
    TestOwner owner{"hello world"};
    std::string_view v = owner.view();
    CHECK(v == "hello world");

    TestView tv = createView(owner.data);
    CHECK(tv.sv == "hello world");
}
