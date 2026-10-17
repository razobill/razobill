#include <ostream>
#include <string>
#include <string_view>

#include "doctest.h"
#include "librazobill/Lifetime.hpp"
#include "librazobill/MemoryArena.hpp"
#include "librazobill/Scanner.hpp"
#include "librazobill/StringPool.hpp"

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

TEST_CASE("string pool lifetime bound lookup") {
    razobill::core::StringPool pool;
    const auto id = pool.intern("test_string");
    std::string_view sv = pool.lookup(id);
    CHECK(sv == "test_string");
}

TEST_CASE("scanner scan result lifetime bound") {
    std::string msg = "worker 42 reported status OK";
    std::size_t tokenCount = 0;
    auto res = razobill::core::scan(msg, [&](std::string_view, std::string_view) { ++tokenCount; });
    CHECK(tokenCount > 0);
    CHECK(res.tail.data() >= msg.data());
    CHECK(res.tail.data() <= msg.data() + msg.size());
}

TEST_CASE("memory arena lifetime bound allocation") {
    razobill::core::MemoryArena arena(4096);
    void* ptr = arena.allocate(64, 8);
    CHECK(ptr != nullptr);
}
