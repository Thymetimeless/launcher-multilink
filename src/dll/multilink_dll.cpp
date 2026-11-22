// Loaded into a launcher (HYP.exe, steam.exe, ...). Patches the downloader module's import table
// for ws2_32 bind/connect (module chosen by the ini profile whose process= matches this exe) so
// each new TCP connection gets a source address from the next configured adapter (round-robin).
// Windows then routes that connection out of that adapter.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "../core/multilink_core.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

namespace {

using BindFn = int(WSAAPI*)(SOCKET, const sockaddr*, int);
using ConnectFn = int(WSAAPI*)(SOCKET, const sockaddr*, int);

BindFn g_realBind = nullptr;
ConnectFn g_realConnect = nullptr;
std::vector<in_addr> g_sources;
std::vector<std::wstring> g_sourceNames;
std::atomic<uint32_t> g_next{0};
std::vector<std::atomic<uint32_t>>* g_perSource = nullptr;

HMODULE g_self = nullptr;
std::wstring g_dir;
FILE* g_log = nullptr;
std::mutex g_logMx;

void Log(const char* fmt, ...) {
    std::lock_guard lk(g_logMx);
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "%02u:%02u:%02u.%03u ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

std::string Ip(in_addr a) {
    char buf[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &a, buf, sizeof buf);
    return buf;
}

size_t PickSource() { return g_next.fetch_add(1, std::memory_order_relaxed) % g_sources.size(); }

bool IsTcp(SOCKET s) {
    int type = 0, len = sizeof type;
    return getsockopt(s, SOL_SOCKET, SO_TYPE, reinterpret_cast<char*>(&type), &len) == 0 && type == SOCK_STREAM;
}

bool IsUnbound(SOCKET s) {
    sockaddr_storage ss{};
    int len = sizeof ss;
    return getsockname(s, reinterpret_cast<sockaddr*>(&ss), &len) == SOCKET_ERROR && WSAGetLastError() == WSAEINVAL;
}

// bind(ANY:0) precedes ConnectEx-style connects; substitute a concrete adapter address.
int WSAAPI HookBind(SOCKET s, const sockaddr* name, int len) {
    if (name && len >= static_cast<int>(sizeof(sockaddr_in)) && name->sa_family == AF_INET) {
        const auto* in = reinterpret_cast<const sockaddr_in*>(name);
        if (in->sin_addr.s_addr == INADDR_ANY && in->sin_port == 0 && IsTcp(s)) {
            const size_t i = PickSource();
            sockaddr_in local = *in;
            local.sin_addr = g_sources[i];
            if (g_realBind(s, reinterpret_cast<const sockaddr*>(&local), sizeof local) == 0) {
                ++(*g_perSource)[i];
                Log("bind  sock=%llu src=%s (%ls)", static_cast<unsigned long long>(s), Ip(g_sources[i]).c_str(),
                    g_sourceNames[i].c_str());
                return 0;
            }
            Log("bind  sock=%llu src=%s failed err=%d, falling back to ANY", static_cast<unsigned long long>(s),
                Ip(g_sources[i]).c_str(), WSAGetLastError());
        }
    }
    return g_realBind(s, name, len);
}

int WSAAPI HookConnect(SOCKET s, const sockaddr* name, int len) {
    if (name && len >= static_cast<int>(sizeof(sockaddr_in)) && name->sa_family == AF_INET) {
        const auto* dst = reinterpret_cast<const sockaddr_in*>(name);
        if (multilink::IsPublicIPv4(ntohl(dst->sin_addr.s_addr)) && IsUnbound(s)) {
            const size_t i = PickSource();
            sockaddr_in local{};
            local.sin_family = AF_INET;
            local.sin_addr = g_sources[i];
            if (g_realBind(s, reinterpret_cast<const sockaddr*>(&local), sizeof local) == 0) {
                ++(*g_perSource)[i];
                Log("conn  sock=%llu dst=%s:%u src=%s (%ls)", static_cast<unsigned long long>(s),
                    Ip(dst->sin_addr).c_str(), ntohs(dst->sin_port), Ip(g_sources[i]).c_str(),
                    g_sourceNames[i].c_str());
            } else {
                Log("conn  sock=%llu pre-bind to %s failed err=%d, using default route",
                    static_cast<unsigned long long>(s), Ip(g_sources[i]).c_str(), WSAGetLastError());
            }
        }
    }
    return g_realConnect(s, name, len);  // last call, so the caller sees its WSAGetLastError
}

// Returns the IAT slot in `mod` that imports `target` from WS2_32.dll, or nullptr.
void** FindIatSlot(HMODULE mod, void* target) {
    auto* base = reinterpret_cast<BYTE*>(mod);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return nullptr;
    const DWORD imageSize = nt->OptionalHeader.SizeOfImage;
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress || dir.VirtualAddress + dir.Size > imageSize) return nullptr;

    for (auto* d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; ++d) {
        if (d->Name >= imageSize || d->FirstThunk >= imageSize) return nullptr;
        if (_stricmp(reinterpret_cast<char*>(base + d->Name), "WS2_32.dll") != 0) continue;
        for (auto* t = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + d->FirstThunk);
             t->u1.Function && reinterpret_cast<BYTE*>(t) < base + imageSize; ++t) {
            if (reinterpret_cast<void*>(t->u1.Function) == target) return reinterpret_cast<void**>(&t->u1.Function);
        }
    }
    return nullptr;
}

bool WriteSlot(void** slot, void* value) {
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    InterlockedExchangePointer(slot, value);
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

bool ResolveSources(const std::vector<std::wstring>& wanted) {
    ULONG size = 32 * 1024;
    std::vector<BYTE> buf;
    ULONG rc;
    for (int tries = 0; tries < 3; ++tries) {
        buf.resize(size);
        rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_MULTICAST,
                                  nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
        if (rc != ERROR_BUFFER_OVERFLOW) break;
    }
    if (rc != NO_ERROR) {
        Log("GetAdaptersAddresses failed rc=%lu", rc);
        return false;
    }
    for (const auto& name : wanted) {
        bool found = false;
        for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next) {
            if (_wcsicmp(a->FriendlyName, name.c_str()) != 0) continue;
            found = true;
            if (a->OperStatus != IfOperStatusUp || !a->FirstGatewayAddress || !a->FirstUnicastAddress) {
                Log("adapter '%ls' skipped: down or no IPv4 gateway", name.c_str());
                break;
            }
            const in_addr ip = reinterpret_cast<sockaddr_in*>(a->FirstUnicastAddress->Address.lpSockaddr)->sin_addr;
            g_sources.push_back(ip);
            g_sourceNames.push_back(name);
            Log("adapter '%ls' -> %s", name.c_str(), Ip(ip).c_str());
            break;
        }
        if (!found) Log("adapter '%ls' not found", name.c_str());
    }
    return g_sources.size() >= 2;
}

// ---- Sophon_InitEnv hook: turn off HTTP/2 so sophon spreads chunks over several connections ----

// Sophon_InitEnv(const char*, const char*, int, int) in sophon.dll 1.18.0.380 (RVA 0x28910),
// recognised by its prologue; a different build is left alone.
using InitEnvFn = void*(__fastcall*)(const char*, const char*, int, int);
constexpr BYTE kInitEnvPrologue[] = {0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x56, 0x57,
                                     0x53, 0x48, 0x83, 0xEC, 0x68, 0x44, 0x89, 0xCE};
constexpr size_t kJmpSize = 14;  // FF 25 00000000 <abs64>
static_assert(sizeof(kInitEnvPrologue) >= kJmpSize);

BYTE* g_initEnv = nullptr;
BYTE g_initEnvSaved[kJmpSize];
std::mutex g_initEnvMx;

void* WINAPI HookInitEnv(const char* a, const char* b, int c, int d);

bool WriteCode(BYTE* at, const BYTE* bytes, size_t n) {
    DWORD old;
    if (!VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(at, bytes, n);
    VirtualProtect(at, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, n);
    return true;
}

bool ArmInitEnv() {
    BYTE jmp[kJmpSize] = {0xFF, 0x25, 0, 0, 0, 0};
    const auto target = reinterpret_cast<uint64_t>(&HookInitEnv);
    memcpy(jmp + 6, &target, sizeof target);
    return WriteCode(g_initEnv, jmp, kJmpSize);
}

// Logs the part of an init string around a key, never the whole thing (it carries device ids).
void LogAround(const char* tag, const std::string& s, std::string_view key) {
    const size_t at = s.find(key);
    if (at == std::string::npos) {
        Log("%s: %zu chars, no %.*s", tag, s.size(), static_cast<int>(key.size()), key.data());
        return;
    }
    const size_t from = at > 40 ? at - 40 : 0;
    Log("%s: ...%s...", tag, s.substr(from, 140).c_str());
}

void* WINAPI HookInitEnv(const char* a, const char* b, int c, int d) {
    std::lock_guard lk(g_initEnvMx);
    WriteCode(g_initEnv, g_initEnvSaved, kJmpSize);  // call the original through its own bytes
    std::string na, nb;
    const bool ca = a && multilink::DisableHttp2(a, na);
    const bool cb = b && multilink::DisableHttp2(b, nb);
    if (a) LogAround(ca ? "InitEnv arg1 (http2 -> off)" : "InitEnv arg1", ca ? na : std::string(a), "enable_http2");
    if (b) LogAround(cb ? "InitEnv arg2 (http2 -> off)" : "InitEnv arg2", cb ? nb : std::string(b), "enable_http2");
    void* ret = reinterpret_cast<InitEnvFn>(g_initEnv)(ca ? na.c_str() : a, cb ? nb.c_str() : b, c, d);
    if (!ArmInitEnv()) Log("InitEnv re-hook failed err=%lu", GetLastError());
    return ret;
}

void* FindExport(BYTE* base, const char* name) {
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.VirtualAddress || dir.VirtualAddress + dir.Size > nt->OptionalHeader.SizeOfImage) return nullptr;
    auto* exp = reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
    auto* names = reinterpret_cast<DWORD*>(base + exp->AddressOfNames);
    auto* ords = reinterpret_cast<WORD*>(base + exp->AddressOfNameOrdinals);
    auto* funcs = reinterpret_cast<DWORD*>(base + exp->AddressOfFunctions);
    for (DWORD i = 0; i < exp->NumberOfNames; ++i)
        if (strcmp(reinterpret_cast<char*>(base + names[i]), name) == 0 && ords[i] < exp->NumberOfFunctions)
            return base + funcs[ords[i]];
    return nullptr;
}

// Runs under the loader lock right after sophon.dll is mapped, before anyone can call into it.
void HookSophonInitEnv(BYTE* base) {
    auto* fn = static_cast<BYTE*>(FindExport(base, "Sophon_InitEnv"));
    if (!fn) {
        Log("Sophon_InitEnv export not found, http2 left as is");
        return;
    }
    if (memcmp(fn, kInitEnvPrologue, sizeof kInitEnvPrologue) != 0) {
        Log("Sophon_InitEnv@%p prologue differs from build 1.18.0.380, http2 left as is", fn);
        return;
    }
    std::lock_guard lk(g_initEnvMx);
    g_initEnv = fn;
    memcpy(g_initEnvSaved, fn, kJmpSize);
    Log(ArmInitEnv() ? "Sophon_InitEnv@%p hooked" : "Sophon_InitEnv@%p hook failed", fn);
}

struct LdrUnicodeString { USHORT Length, MaximumLength; PWSTR Buffer; };
struct LdrDllNotificationData { ULONG Flags; const LdrUnicodeString* FullDllName; const LdrUnicodeString* BaseDllName; PVOID DllBase; ULONG SizeOfImage; };
using LdrNotifyFn = VOID(CALLBACK*)(ULONG, const LdrDllNotificationData*, PVOID);
using LdrRegisterFn = LONG(NTAPI*)(ULONG, LdrNotifyFn, PVOID, PVOID*);

VOID CALLBACK OnDllNotification(ULONG reason, const LdrDllNotificationData* data, PVOID) {
    constexpr ULONG kLoaded = 1;
    if (reason != kLoaded || !data || !data->BaseDllName || !data->BaseDllName->Buffer) return;
    const std::wstring_view name(data->BaseDllName->Buffer, data->BaseDllName->Length / sizeof(wchar_t));
    if (name.size() == 10 && _wcsnicmp(name.data(), L"sophon.dll", 10) == 0) HookSophonInitEnv(static_cast<BYTE*>(data->DllBase));
}

// The downloader module named by the ini profile whose process= is this exe, or "" when none is.
std::wstring TargetModule(const std::wstring& ini) {
    wchar_t self[MAX_PATH];
    const std::wstring path(self, GetModuleFileNameW(nullptr, self, MAX_PATH));
    const std::wstring exe = path.substr(path.find_last_of(L'\\') + 1);
    wchar_t sections[4096] = {};
    GetPrivateProfileSectionNamesW(sections, ARRAYSIZE(sections), ini.c_str());
    for (const wchar_t* s = sections; *s; s += wcslen(s) + 1) {
        wchar_t process[MAX_PATH] = {}, module[MAX_PATH] = {};
        GetPrivateProfileStringW(s, L"process", L"", process, MAX_PATH, ini.c_str());
        if (_wcsicmp(process, exe.c_str()) != 0) continue;
        GetPrivateProfileStringW(s, L"module", L"", module, MAX_PATH, ini.c_str());
        Log("profile [%ls]: process=%ls module=%ls", s, process, module);
        return module;
    }
    Log("no profile in the ini has process=%ls", exe.c_str());
    return {};
}

DWORD WINAPI InitThread(void*) {
    const std::wstring ini = g_dir + L"multilink.ini";
    wchar_t list[512] = {};
    GetPrivateProfileStringW(L"multilink", L"adapters", L"", list, ARRAYSIZE(list), ini.c_str());
    Log("pid=%lu ini=%ls adapters=%ls", GetCurrentProcessId(), ini.c_str(), list);
    const std::wstring module = TargetModule(ini);
    if (module.empty()) return 0;

    if (!ResolveSources(multilink::SplitList(list))) {
        Log("fewer than 2 usable adapters, nothing patched");
        return 0;
    }
    g_perSource = new std::vector<std::atomic<uint32_t>>(g_sources.size());

    HMODULE ws2 = GetModuleHandleW(L"ws2_32.dll");
    if (!ws2) ws2 = LoadLibraryW(L"ws2_32.dll");
    g_realBind = reinterpret_cast<BindFn>(GetProcAddress(ws2, "bind"));
    g_realConnect = reinterpret_cast<ConnectFn>(GetProcAddress(ws2, "connect"));
    if (!g_realBind || !g_realConnect) {
        Log("ws2_32 bind/connect not resolved");
        return 0;
    }

    // shortcut: polls for the module; connections it opens before the patch use the default route.
    HMODULE target = nullptr;
    for (int i = 0; i < 10 * 60 * 4 && !(target = GetModuleHandleW(module.c_str())); ++i) Sleep(250);
    if (!target) {
        Log("%ls not loaded in this process after 10 min, giving up", module.c_str());
        return 0;
    }
    HMODULE pin;  // keep the module resident while its IAT points at us
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       reinterpret_cast<LPCWSTR>(target), &pin);

    void** bindSlot = FindIatSlot(target, reinterpret_cast<void*>(g_realBind));
    void** connSlot = FindIatSlot(target, reinterpret_cast<void*>(g_realConnect));
    if (!bindSlot || !connSlot) {
        Log("%ls IAT slots not found (bind=%p connect=%p), nothing patched", module.c_str(), bindSlot, connSlot);
        return 0;
    }
    if (!WriteSlot(bindSlot, reinterpret_cast<void*>(&HookBind))) {
        Log("patch bind failed err=%lu", GetLastError());
        return 0;
    }
    if (!WriteSlot(connSlot, reinterpret_cast<void*>(&HookConnect))) {
        Log("patch connect failed err=%lu, rolling back bind", GetLastError());
        WriteSlot(bindSlot, reinterpret_cast<void*>(g_realBind));
        return 0;
    }
    Log("%ls@%p patched: %zu round-robin slots", module.c_str(), target, g_sources.size());

    for (;;) {  // per-adapter connection counters, once a minute (an adapter listed twice is summed)
        Sleep(60 * 1000);
        std::map<std::wstring, uint32_t> perName;
        for (size_t i = 0; i < g_sources.size(); ++i) perName[g_sourceNames[i]] += (*g_perSource)[i].load();
        std::string line;
        for (const auto& [name, count] : perName) {
            char item[160];
            snprintf(item, sizeof item, " %ls=%u", name.c_str(), count);
            line += item;
        }
        Log("connections per adapter:%s", line.c_str());
    }
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(mod);
    g_self = mod;
    HMODULE pin;  // hooks stay reachable for the process lifetime, so never unload
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       reinterpret_cast<LPCWSTR>(&DllMain), &pin);

    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(mod, path, MAX_PATH);
    g_dir.assign(path, n);
    g_dir.resize(g_dir.find_last_of(L'\\') + 1);
    g_log = _wfsopen((g_dir + L"multilink.log").c_str(), L"a", _SH_DENYNO);

    if (GetModuleHandleW(L"sophon.dll")) {  // HoYoPlay only; other launchers never load it
        Log("sophon.dll was loaded before multilink: http2 stays on. Start HoYoPlay through multilink_inject.exe");
    } else if (auto reg = reinterpret_cast<LdrRegisterFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification"))) {
        PVOID cookie;  // never unregistered: the DLL stays pinned
        if (reg(0, &OnDllNotification, nullptr, &cookie) != 0) Log("LdrRegisterDllNotification failed");
    }

    if (HANDLE h = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr)) CloseHandle(h);
    return TRUE;
}
