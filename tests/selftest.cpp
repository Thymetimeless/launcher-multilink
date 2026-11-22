#include <cassert>
#include <cstdio>

#include "../src/core/multilink_core.h"

constexpr uint32_t Ip(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { return a << 24 | b << 16 | c << 8 | d; }

int main() {
    using multilink::IsPublicIPv4;
    static_assert(IsPublicIPv4(Ip(8, 8, 8, 8)));
    static_assert(IsPublicIPv4(Ip(172, 32, 0, 1)) && IsPublicIPv4(Ip(100, 128, 0, 1)) && IsPublicIPv4(Ip(192, 169, 0, 1)));
    static_assert(!IsPublicIPv4(Ip(127, 0, 0, 1)) && !IsPublicIPv4(Ip(10, 1, 2, 3)) && !IsPublicIPv4(Ip(0, 0, 0, 0)));
    static_assert(!IsPublicIPv4(Ip(192, 168, 120, 1)) && !IsPublicIPv4(Ip(172, 16, 0, 1)) && !IsPublicIPv4(Ip(172, 31, 255, 255)));
    static_assert(!IsPublicIPv4(Ip(169, 254, 1, 1)) && !IsPublicIPv4(Ip(100, 64, 0, 1)) && !IsPublicIPv4(Ip(224, 0, 0, 1)));

    auto v = multilink::SplitList(L" Ethernet ,Wi-Fi,, ");
    assert(v.size() == 2 && v[0] == L"Ethernet" && v[1] == L"Wi-Fi");
    assert(multilink::SplitList(L"").empty());
    std::string out;
    assert(multilink::DisableHttp2(R"({"a":1,"enable_http2":true,"b":true})", out) && out == R"({"a":1,"enable_http2":false,"b":true})");
    assert(multilink::DisableHttp2("{\"enable_http2\" :\n 1}", out) && out == "{\"enable_http2\" :\n 0}");
    assert(!multilink::DisableHttp2(R"({"enable_http2":false})", out) && out == R"({"enable_http2":false})");
    assert(!multilink::DisableHttp2(R"({"enable_http2":10})", out));
    assert(!multilink::DisableHttp2(R"({"other":"enable_http2"})", out));
    assert(!multilink::DisableHttp2("", out) && out.empty());
    using multilink::IsOn;
    assert(IsOn(L"1") && IsOn(L"true") && IsOn(L"TRUE") && IsOn(L" True "));
    assert(!IsOn(L"0") && !IsOn(L"false") && !IsOn(L"") && !IsOn(L"yes") && !IsOn(L"10") && !IsOn(L"truex"));
    std::puts("selftest ok");
}
