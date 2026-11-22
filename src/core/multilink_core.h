#pragma once
// Pure helpers shared by the DLL and the self-test (no Winsock state).
#include <cstdint>
#include <cwctype>
#include <string>
#include <string_view>
#include <vector>

namespace multilink {

// hostOrder: IPv4 address in host byte order. True only for globally routable unicast,
// i.e. destinations that every bonded interface can reach through its own gateway.
constexpr bool IsPublicIPv4(uint32_t hostOrder) {
    const uint32_t a = hostOrder >> 24, b = (hostOrder >> 16) & 0xFF;
    if (a == 0 || a == 10 || a == 127 || a >= 224) return false;  // this-net, private, loopback, multicast/reserved
    if (a == 169 && b == 254) return false;                       // link-local
    if (a == 172 && b >= 16 && b <= 31) return false;             // private
    if (a == 192 && b == 168) return false;                       // private
    if (a == 100 && b >= 64 && b <= 127) return false;            // CGNAT / Tailscale
    return true;
}

// Rewrites `"enable_http2": true` to false in a JSON text. Returns true when it changed something.
// HTTP/2 puts every chunk of a host on one TCP connection, which per-connection bonding can't split.
inline bool DisableHttp2(std::string_view json, std::string& out) {
    constexpr std::string_view key = "\"enable_http2\"";
    out.assign(json);
    bool changed = false;
    for (size_t pos = out.find(key); pos != std::string::npos; pos = out.find(key, pos + 1)) {
        size_t v = pos + key.size();
        while (v < out.size() && (out[v] == ' ' || out[v] == '\t' || out[v] == '\n' || out[v] == '\r')) ++v;
        if (v >= out.size() || out[v] != ':') continue;
        ++v;
        while (v < out.size() && (out[v] == ' ' || out[v] == '\t' || out[v] == '\n' || out[v] == '\r')) ++v;
        if (out.compare(v, 4, "true") == 0) {
            out.replace(v, 4, "false");
            changed = true;
        } else if (out.compare(v, 1, "1") == 0 && (v + 1 >= out.size() || out[v + 1] < '0' || out[v + 1] > '9')) {
            out.replace(v, 1, "0");
            changed = true;
        }
    }
    return changed;
}

// ini switch: "1" or "true" (any case, surrounding blanks ignored) is on; anything else is off.
inline bool IsOn(std::wstring_view v) {
    const size_t b = v.find_first_not_of(L" \t"), e = v.find_last_not_of(L" \t");
    if (b == std::wstring_view::npos) return false;
    v = v.substr(b, e - b + 1);
    if (v == L"1") return true;
    if (v.size() != 4) return false;
    for (size_t i = 0; i < 4; ++i)
        if (towlower(v[i]) != L"true"[i]) return false;
    return true;
}

// "Ethernet, Wi-Fi" -> {"Ethernet", "Wi-Fi"}; empty items dropped.
inline std::vector<std::wstring> SplitList(const std::wstring& s) {
    std::vector<std::wstring> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t end = s.find(L',', start);
        if (end == std::wstring::npos) end = s.size();
        std::wstring item = s.substr(start, end - start);
        const size_t b = item.find_first_not_of(L" \t"), e = item.find_last_not_of(L" \t");
        if (b != std::wstring::npos) out.push_back(item.substr(b, e - b + 1));
        start = end + 1;
    }
    return out;
}

}  // namespace multilink
