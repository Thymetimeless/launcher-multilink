// Starts launchers if needed and loads multilink.dll into them, per profile in multilink.ini.
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <tlhelp32.h>
#include <shellapi.h>

#include <cstdio>
#include <string>
#include <vector>

#include "../core/multilink_core.h"

namespace {

// Process I/O (disk + network) at or above this counts as "downloading". A paused launcher idles
// far below it; the slowest link here alone moves ~2.5 MB/s.
constexpr double kDownloadingMBps = 1.0;
constexpr DWORD kSampleMs = 3000;

std::wstring ExeDir() {
    wchar_t p[MAX_PATH];
    std::wstring d(p, GetModuleFileNameW(nullptr, p, MAX_PATH));
    return d.substr(0, d.find_last_of(L'\\') + 1);
}

std::vector<DWORD> FindProcesses(const wchar_t* exe) {
    std::vector<DWORD> pids;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return pids;
    PROCESSENTRY32W pe{sizeof pe};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (_wcsicmp(pe.szExeFile, exe) == 0) pids.push_back(pe.th32ProcessID);
    CloseHandle(snap);
    return pids;
}

std::vector<DWORD> ChildProcesses(DWORD parent) {
    std::vector<DWORD> pids;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return pids;
    PROCESSENTRY32W pe{sizeof pe};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (pe.th32ParentProcessID == parent && pe.th32ProcessID != parent) pids.push_back(pe.th32ProcessID);
    CloseHandle(snap);
    return pids;
}

bool HasModule(DWORD pid, const wchar_t* dll) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W me{sizeof me};
    bool found = false;
    for (BOOL ok = Module32FirstW(snap, &me); ok && !found; ok = Module32NextW(snap, &me))
        found = _wcsicmp(me.szModule, dll) == 0;
    CloseHandle(snap);
    return found;
}

// Bytes/s the process reads and writes (files, sockets, everything), sampled over kSampleMs. -1 if unknown.
double IoRateMBps(DWORD pid) {
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return -1;
    IO_COUNTERS a{}, b{};
    double rate = -1;
    if (GetProcessIoCounters(proc, &a)) {
        Sleep(kSampleMs);
        if (GetProcessIoCounters(proc, &b)) {
            const ULONGLONG bytes = (b.ReadTransferCount - a.ReadTransferCount) + (b.WriteTransferCount - a.WriteTransferCount) +
                                    (b.OtherTransferCount - a.OtherTransferCount);
            rate = bytes / (kSampleMs / 1000.0) / 1e6;
        }
    }
    CloseHandle(proc);
    return rate;
}

bool Inject(DWORD pid, const std::wstring& dll) {
    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                                  PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
    if (!proc) {
        wprintf(L"  OpenProcess(%lu) failed: %lu\n", pid, GetLastError());
        return false;
    }
    wchar_t image[MAX_PATH] = L"?";
    DWORD len = MAX_PATH;
    QueryFullProcessImageNameW(proc, 0, image, &len);
    BOOL wow64 = FALSE;
    IsWow64Process(proc, &wow64);
    wprintf(L"  pid %lu %ls\n", pid, image);

    bool ok = false;
    const SIZE_T bytes = (dll.size() + 1) * sizeof(wchar_t);
    void* remote = nullptr;
    if (wow64) {
        wprintf(L"  32-bit process, skipped\n");
    } else if (!(remote = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE))) {
        wprintf(L"  VirtualAllocEx failed: %lu\n", GetLastError());
    } else if (!WriteProcessMemory(proc, remote, dll.c_str(), bytes, nullptr)) {
        wprintf(L"  WriteProcessMemory failed: %lu\n", GetLastError());
    } else {
        // kernel32 is mapped at the same address in every process of a boot session.
        auto loadLib = reinterpret_cast<LPTHREAD_START_ROUTINE>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
        HANDLE th = CreateRemoteThread(proc, nullptr, 0, loadLib, remote, 0, nullptr);
        if (!th) {
            wprintf(L"  CreateRemoteThread failed: %lu\n", GetLastError());
        } else {
            WaitForSingleObject(th, 15000);
            DWORD code = 0;
            GetExitCodeThread(th, &code);  // truncated HMODULE; 0 = LoadLibraryW failed
            CloseHandle(th);
            ok = code != 0 && HasModule(pid, L"multilink.dll");
            wprintf(ok ? L"  multilink.dll loaded\n" : L"  LoadLibraryW failed in target\n");
        }
    }
    if (remote) VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    CloseHandle(proc);
    return ok;
}

// The DLL sets Local\multilink-ready-<pid> once its hooks are in place.
bool WaitHooksReady(DWORD pid, DWORD timeoutMs) {
    const std::wstring name = L"Local\\multilink-ready-" + std::to_wstring(pid);
    for (DWORD waited = 0; waited <= timeoutMs; waited += 100) {
        if (HANDLE ev = OpenEventW(SYNCHRONIZE, FALSE, name.c_str())) {
            const bool set = WaitForSingleObject(ev, timeoutMs - waited) == WAIT_OBJECT_0;
            CloseHandle(ev);
            return set;
        }
        Sleep(100);
    }
    return false;
}

// Drops the process's open IPv4 connections to public addresses, so the launcher reconnects
// through the hooks instead of reusing sockets opened before the injection. Needs admin.
int ResetConnections(DWORD pid) {
    ULONG size = 0;
    GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_CONNECTIONS, 0);
    std::vector<BYTE> buf(size + 4096);
    size = static_cast<ULONG>(buf.size());
    if (GetExtendedTcpTable(buf.data(), &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_CONNECTIONS, 0) != NO_ERROR) return -1;
    const auto* table = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(buf.data());
    int reset = 0;
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const MIB_TCPROW_OWNER_PID& r = table->table[i];
        if (r.dwOwningPid != pid || r.dwState != MIB_TCP_STATE_ESTAB) continue;
        if (!multilink::IsPublicIPv4(_byteswap_ulong(r.dwRemoteAddr))) continue;
        MIB_TCPROW row{};
        row.dwState = MIB_TCP_STATE_DELETE_TCB;
        row.dwLocalAddr = r.dwLocalAddr;
        row.dwLocalPort = r.dwLocalPort;
        row.dwRemoteAddr = r.dwRemoteAddr;
        row.dwRemotePort = r.dwRemotePort;
        if (SetTcpEntry(&row) == NO_ERROR) ++reset;
    }
    return reset;
}

bool KillWithChildren(DWORD pid) {
    for (DWORD child : ChildProcesses(pid)) KillWithChildren(child);
    HANDLE proc = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
    if (!proc) return GetLastError() == ERROR_INVALID_PARAMETER;  // already gone
    const bool ok = TerminateProcess(proc, 0) && WaitForSingleObject(proc, 10000) == WAIT_OBJECT_0;
    CloseHandle(proc);
    return ok;
}

bool StartLauncher(const wchar_t* process, const wchar_t* start, const std::wstring& profile, const std::wstring& ini,
                   std::vector<DWORD>& pids) {
    // An empty or wrong path would make ShellExecute open the working directory (System32) instead.
    const DWORD attr = GetFileAttributesW(start);
    if (!*start || attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        wprintf(L"  start= in [%ls] is not a file: \"%ls\". Set it to the launcher exe in %ls\n", profile.c_str(), start, ini.c_str());
        return false;
    }
    wprintf(L"  starting %ls\n", start);
    if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", start, nullptr, nullptr, SW_SHOWNORMAL)) <= 32) {
        wprintf(L"  failed to start the launcher\n");
        return false;
    }
    for (int i = 0; i < 120 && pids.empty(); ++i) {
        Sleep(500);
        pids = FindProcesses(process);
    }
    if (pids.empty()) wprintf(L"  %ls did not appear within 60 s\n", process);
    return !pids.empty();
}

// Starts (if allowed) and injects one profile's launcher. Returns false on any failure.
bool RunProfile(const std::wstring& profile, const std::wstring& ini, const std::wstring& dll) {
    wprintf(L"[%ls]\n", profile.c_str());
    wchar_t process[MAX_PATH] = {}, start[MAX_PATH] = {}, autostart[16] = {}, late[16] = {};
    GetPrivateProfileStringW(profile.c_str(), L"process", L"", process, MAX_PATH, ini.c_str());
    GetPrivateProfileStringW(profile.c_str(), L"start", L"", start, MAX_PATH, ini.c_str());
    GetPrivateProfileStringW(profile.c_str(), L"autostart", L"0", autostart, ARRAYSIZE(autostart), ini.c_str());
    GetPrivateProfileStringW(profile.c_str(), L"late", L"reset", late, ARRAYSIZE(late), ini.c_str());
    if (!*process) {
        wprintf(L"  no process= for [%ls] in %ls\n", profile.c_str(), ini.c_str());
        return false;
    }
    const bool restartLate = _wcsicmp(late, L"restart") == 0;

    auto pids = FindProcesses(process);
    if (pids.empty()) {
        // An elevated injector would start the launcher elevated too; only allowed where the launcher is elevated anyway.
        if (!multilink::IsOn(autostart)) {
            wprintf(L"  %ls is not running. Start it normally, then run this again.\n", process);
            return false;
        }
        wprintf(L"  %ls not running\n", process);
        if (!StartLauncher(process, start, profile, ini, pids)) return false;
        bool ok = true;
        for (DWORD pid : pids) ok = Inject(pid, dll) && ok;
        return ok;
    }

    // Already running: refuse while it downloads, otherwise make the late injection count.
    std::vector<DWORD> todo;
    for (DWORD pid : pids) {
        if (HasModule(pid, L"multilink.dll")) {
            wprintf(L"  pid %lu already has multilink.dll\n", pid);
            continue;
        }
        wprintf(L"  pid %lu: checking for an active download (%lu s)...\n", pid, kSampleMs / 1000);
        const double rate = IoRateMBps(pid);
        if (rate >= kDownloadingMBps) {
            wprintf(L"  pid %lu is downloading (%.1f MB/s of I/O). Pause the download in the launcher, then run this again.\n", pid, rate);
            return false;
        }
        if (rate < 0) wprintf(L"  pid %lu: could not read its I/O counters, assuming it is idle\n", pid);
        todo.push_back(pid);
    }
    if (todo.empty()) return true;

    if (restartLate) {
        // HoYoPlay fixes HTTP/2 at start-up; only a fresh start lets the hook turn it off. Paused
        // progress is kept on disk and resumes from there.
        if (!multilink::IsOn(autostart)) {
            wprintf(L"  late=restart needs autostart=1 in [%ls]\n", profile.c_str());
            return false;
        }
        wprintf(L"  %ls is idle: restarting it so the download can use every link (progress is kept)\n", process);
        for (DWORD pid : todo)
            if (!KillWithChildren(pid)) {
                wprintf(L"  could not close pid %lu (%lu)\n", pid, GetLastError());
                return false;
            }
        pids.clear();
        if (!StartLauncher(process, start, profile, ini, pids)) return false;
        bool ok = true;
        for (DWORD pid : pids) ok = Inject(pid, dll) && ok;
        if (ok) wprintf(L"  resume the download in the launcher\n");
        return ok;
    }

    bool ok = true;
    for (DWORD pid : todo) {
        if (!Inject(pid, dll)) {
            ok = false;
            continue;
        }
        if (!WaitHooksReady(pid, 15000)) {
            wprintf(L"  hooks not reported ready within 15 s (see the log); open connections left as they are\n");
            continue;
        }
        const int n = ResetConnections(pid);
        if (n < 0) wprintf(L"  could not read the TCP table, open connections left as they are\n");
        else wprintf(L"  reset %d open connection(s) so they reconnect through every link\n", n);
    }
    if (ok) wprintf(L"  resume the download in the launcher\n");
    return ok;
}

bool IsElevated() {
    HANDLE token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const bool elevated = GetTokenInformation(token, TokenElevation, &elevation, sizeof elevation, &size) && elevation.TokenIsElevated;
    CloseHandle(token);
    return elevated;
}

void WaitIfOwnConsole() {
    DWORD ids[2];
    if (GetConsoleProcessList(ids, 2) == 1) {  // our own console (double-clicked): keep it open to be read
        wprintf(L"Press Enter to close.");
        (void)getwchar();
    }
}

}  // namespace

// Usage: multilink_inject.exe            every profile in multilink.ini with enabled=true (or 1)
//        multilink_inject.exe <profile>  just that profile, even if it is disabled
int wmain(int argc, wchar_t** argv) {
    // The manifest asks for elevation, but a shell can still start it without (e.g. RunAsInvoker),
    // and then OpenProcess on the elevated launcher fails with 5.
    if (!IsElevated()) {
        wprintf(L"NOT running as administrator.\n"
                L"Right-click multilink_inject.exe -> Run as administrator, then try again.\n");
        WaitIfOwnConsole();
        return 1;
    }
    const std::wstring dir = ExeDir();
    const std::wstring dll = dir + L"multilink.dll";
    const std::wstring ini = dir + L"multilink.ini";
    if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) {
        wprintf(L"missing %ls\n", dll.c_str());
        return 1;
    }

    std::vector<std::wstring> profiles;
    if (argc > 1) {
        profiles.push_back(argv[1]);
    } else {
        wchar_t sections[4096] = {};
        GetPrivateProfileSectionNamesW(sections, ARRAYSIZE(sections), ini.c_str());
        for (const wchar_t* s = sections; *s; s += wcslen(s) + 1) {
            wchar_t enabled[16] = {};
            GetPrivateProfileStringW(s, L"enabled", L"0", enabled, ARRAYSIZE(enabled), ini.c_str());
            if (_wcsicmp(s, L"multilink") == 0) continue;
            if (multilink::IsOn(enabled)) profiles.push_back(s);
            else wprintf(L"[%ls] disabled (enabled=%ls), skipped\n", s, enabled);
        }
        if (profiles.empty()) wprintf(L"no profile has enabled=true in %ls\n", ini.c_str());
    }

    int failures = 0;
    for (const auto& p : profiles) failures += RunProfile(p, ini, dll) ? 0 : 1;
    wprintf(L"done, log: %lsmultilink.log\n", dir.c_str());
    WaitIfOwnConsole();
    return failures ? 1 : 0;
}
