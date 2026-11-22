// Starts HoYoPlay if needed and loads multilink.dll into every HYP.exe process.
#include <windows.h>
#include <tlhelp32.h>
#include <shellapi.h>

#include <cstdio>
#include <string>
#include <vector>

#include "../core/multilink_core.h"

namespace {

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

// Starts (if allowed) and injects one profile's launcher. Returns false on any failure.
bool RunProfile(const std::wstring& profile, const std::wstring& ini, const std::wstring& dll) {
    wprintf(L"[%ls]\n", profile.c_str());
    wchar_t process[MAX_PATH] = {}, start[MAX_PATH] = {}, autostart[16] = {};
    GetPrivateProfileStringW(profile.c_str(), L"process", L"", process, MAX_PATH, ini.c_str());
    GetPrivateProfileStringW(profile.c_str(), L"start", L"", start, MAX_PATH, ini.c_str());
    GetPrivateProfileStringW(profile.c_str(), L"autostart", L"0", autostart, ARRAYSIZE(autostart), ini.c_str());
    if (!*process) {
        wprintf(L"  no process= for [%ls] in %ls\n", profile.c_str(), ini.c_str());
        return false;
    }

    auto pids = FindProcesses(process);
    if (pids.empty()) {
        // An elevated injector would start the launcher elevated too; only allowed where the launcher is elevated anyway.
        if (!multilink::IsOn(autostart)) {
            wprintf(L"  %ls is not running. Start it normally, then run this again.\n", process);
            return false;
        }
        // An empty or wrong path would make ShellExecute open the working directory (System32) instead.
        const DWORD attr = GetFileAttributesW(start);
        if (!*start || attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) {
            wprintf(L"  start= in [%ls] is not a file: \"%ls\". Set it to the launcher exe in %ls\n", profile.c_str(), start, ini.c_str());
            return false;
        }
        wprintf(L"  %ls not running, starting %ls\n", process, start);
        if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", start, nullptr, nullptr, SW_SHOWNORMAL)) <= 32) {
            wprintf(L"  failed to start the launcher\n");
            return false;
        }
        for (int i = 0; i < 120 && pids.empty(); ++i) {
            Sleep(500);
            pids = FindProcesses(process);
        }
        if (pids.empty()) {
            wprintf(L"  %ls did not appear within 60 s\n", process);
            return false;
        }
    }

    bool ok = true;
    for (DWORD pid : pids) {
        if (HasModule(pid, L"multilink.dll")) {
            wprintf(L"  pid %lu already has multilink.dll\n", pid);
            continue;
        }
        ok = Inject(pid, dll) && ok;
    }
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
