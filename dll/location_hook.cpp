#include <windows.h>
#include <unknwn.h>
#include <MinHook.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <limits>

void** find_vtbl(HMODULE modlf);

namespace {
    HMODULE self{}, original{}, framework{};
    INIT_ONCE loadOnce = INIT_ONCE_STATIC_INIT;
    INIT_ONCE hookOnce = INIT_ONCE_STATIC_INIT;
    wchar_t iniPath[MAX_PATH]{}, logPath[MAX_PATH]{};
    struct Config { double latitude = 51.5074, longitude = -0.1278, altitude = 15, accuracy = 5; DWORD engine = 3; } config;
    bool enabled = true;

    void Log(const char* format, ...) noexcept {
        char body[1200]{};
        va_list args; va_start(args, format); vsnprintf_s(body, sizeof(body), _TRUNCATE, format, args); va_end(args);
        SYSTEMTIME t{}; GetSystemTime(&t);
        char line[1400]{};
        int n = sprintf_s(line, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ pid=%lu tid=%lu %s\r\n",
            t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentProcessId(), GetCurrentThreadId(), body);
        HANDLE file = CreateFileW(logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) { DWORD written; WriteFile(file, line, n, &written, nullptr); CloseHandle(file); }
    }
    bool ReadNumber(const wchar_t* key, const wchar_t* fallback, double& out) {
        wchar_t value[80]{}; GetPrivateProfileStringW(L"Location", key, fallback, value, 80, iniPath);
        wchar_t* end{}; out = wcstod(value, &end);
        return end != value && *end == 0 && std::isfinite(out);
    }

    BOOL CALLBACK LoadOriginal(PINIT_ONCE, void*, void**) {
        GetModuleFileNameW(self, iniPath, MAX_PATH);
        wchar_t* slash = wcsrchr(iniPath, L'\\');
        if (!slash) return FALSE;
        *(slash + 1) = 0;
        wcscpy_s(logPath, iniPath); wcscat_s(logPath, L"location-hook.log");
        wcscat_s(iniPath, L"location-hook.ini");
        wchar_t system[MAX_PATH]{};
        GetSystemDirectoryW(system, MAX_PATH); wcscat_s(system, L"\\lfsvc.dll");
        // Always load the genuine DLL from System32. Never follow the redirected ServiceDll recursively.
        original = LoadLibraryExW(system, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        Log("Load original lfsvc.dll: %p error=%lu", original, original ? 0 : GetLastError());
        return original != nullptr;
    }

    template<class T> T Export(const char* name) {
        if (!InitOnceExecuteOnce(&loadOnce, LoadOriginal, nullptr, nullptr)) return nullptr;
        return reinterpret_cast<T>(GetProcAddress(original, name));
    }

    // Private x64 ABI, verified against the matching Microsoft public PDB and disassembly.
    // The runtime checks the exact CodeView signature before using any of these offsets.
    struct PositionStatus {
        HRESULT result; DWORD engine; FILETIME sourceTime; FILETIME timestamp;
        DWORD unknown[4];
    };
    struct Coordinate { double latitude, longitude, altitude, heading, speed, accuracy; };
    struct Accuracy { std::array<unsigned char, 72> bytes; };
    struct Request { std::array<unsigned char, 32> fields; DWORD flags; };
    static_assert(sizeof(PositionStatus) == 40 && sizeof(Coordinate) == 48 && sizeof(Accuracy) == 72 && sizeof(Request) == 36);
    constexpr GUID internalInfo = { 0x8ac882be,0xeddb,0x4fc7,{0x9b,0x83,0xad,0x43,0x22,0x5a,0xf0,0x2e} };
    using StartFn = HRESULT(WINAPI*)(void*);
    using GetFn = HRESULT(WINAPI*)(void*, IUnknown**);
    StartFn realStart{}; GetFn realNew{}, realCached{};

    template<class F> F Slot(void* object, size_t index)
    {
        return reinterpret_cast<F>((*reinterpret_cast<void***>(object))[index]);
    }
    HRESULT CreateInfo(IUnknown** output) {
        // Same COM component-factory path as LocationHelper::CreateLocationInformation.
        constexpr GUID clsid = { 0xa7b9649e,0xfece,0x4ae3,{0x95,0x8c,0xc3,0x6b,0x13,0x3e,0x93,0x7f} };
        constexpr GUID iid = { 0xe0d94bd9,0xaf1c,0x4d49,{0x96,0xa6,0x9b,0x6a,0x71,0xc2,0x6d,0xf7} };
        constexpr GUID infoIid = { 0x76c23039,0x0d4b,0x4340,{0x9a,0x00,0x73,0x1c,0xfe,0x28,0xd7,0x9e} };
        IUnknown* factory{};
        auto hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, iid, reinterpret_cast<void**>(&factory));
        if (SUCCEEDED(hr)) {
            using Fn = HRESULT(WINAPI*)(void*, DWORD, REFIID, void**);
            hr = Slot<Fn>(factory, 3)(factory, 3, infoIid, reinterpret_cast<void**>(output));
            factory->Release();
        }
        return hr;
    }

    DWORD EngineFor(void* session) {
        Request request{};
        using Fn = HRESULT(WINAPI*)(void*, Request*);
        return SUCCEEDED(Slot<Fn>(session, 9)(session, &request)) && (request.flags & 1) ? 10 : 8;
    }

    HRESULT MakePosition(DWORD engine, IUnknown** output) {
        *output = nullptr;
        IUnknown* info{}; HRESULT hr = CreateInfo(&info);
        if (FAILED(hr) || !info) return FAILED(hr) ? hr : E_UNEXPECTED;
        IUnknown* internal{}; hr = info->QueryInterface(internalInfo, reinterpret_cast<void**>(&internal));
        if (SUCCEEDED(hr)) {
            PositionStatus status{}; status.result = S_OK; status.engine = engine;
            GetSystemTimeAsFileTime(&status.timestamp); status.sourceTime = status.timestamp;
            Coordinate coordinate{ config.latitude,config.longitude,config.altitude,
                std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::quiet_NaN(),config.accuracy };
            // Zero means no satellite/vertical-accuracy data. Horizontal accuracy is Coordinate+0x28.
            Accuracy accuracy{};
            using SetStatus = HRESULT(WINAPI*)(void*, const PositionStatus*);
            using SetCoordinate = HRESULT(WINAPI*)(void*, const Coordinate*);
            using SetAccuracy = HRESULT(WINAPI*)(void*, const Accuracy*);
            hr = Slot<SetStatus>(internal, 8)(internal, &status);
            if (SUCCEEDED(hr)) hr = Slot<SetCoordinate>(internal, 9)(internal, &coordinate);
            if (SUCCEEDED(hr)) hr = Slot<SetAccuracy>(internal, 10)(internal, &accuracy);
            internal->Release();
        }
        if (FAILED(hr)) info->Release(); else *output = info;
        return hr;
    }
    HRESULT ReplacePosition(IUnknown** output, HRESULT originalResult, const char* operation) {
        // Keep the real session's permission checks, queue draining, and notification reset.
        if (FAILED(originalResult) || !output || !*output) return originalResult;
        // Apply the configured source only after the original permission/consent checks.
        IUnknown* replacement{}; auto hr = MakePosition(config.engine, &replacement);
        if (SUCCEEDED(hr)) { (*output)->Release(); *output = replacement; }
        Log("%s original=0x%08lX replacement=0x%08lX lat=%.8f lon=%.8f", operation, originalResult, hr, config.latitude, config.longitude);
        return originalResult;
    }
    HRESULT WINAPI HookNew(void* session, IUnknown** output) {
        return ReplacePosition(output, realNew(session, output), "get_NewPositionInfo");
    }
    HRESULT WINAPI HookCached(void* session, IUnknown** output) {
        return ReplacePosition(output, realCached(session, output), "get_CachedPositionInfo");
    }
    HRESULT WINAPI HookStart(void* session) {
        auto hr = realStart(session);
        if (SUCCEEDED(hr)) {
            IUnknown* info{}; auto made = MakePosition(EngineFor(session), &info);
            if (SUCCEEDED(made)) {
                using OnNewPosition = HRESULT(WINAPI*)(void*, IUnknown*, BOOL);
                made = Slot<OnNewPosition>(session, 15)(session, info, TRUE); info->Release();
            }
            Log("StartSubscriberRequest original=0x%08lX dispatch=0x%08lX", hr, made);
        }
        else Log("StartSubscriberRequest rejected=0x%08lX", hr);
        return hr;
    }

    BOOL CALLBACK InstallHooks(PINIT_ONCE, void*, void**) {
        enabled = GetPrivateProfileIntW(L"Location", L"Enabled", 1, iniPath) != 0;
        if (!enabled) { Log("Hook disabled: forwarding only"); return TRUE; }
        wchar_t source[32]{};
        GetPrivateProfileStringW(L"Location", L"PositionSource", L"WiFi", source, 32, iniPath);
        if (!_wcsicmp(source, L"WiFi")) config.engine = 3;
        else if (!_wcsicmp(source, L"Default")) config.engine = 8;
        else if (!_wcsicmp(source, L"IPAddress")) config.engine = 6;
        else if (!_wcsicmp(source, L"Satellite")) config.engine = 2;
        else if (!_wcsicmp(source, L"Cellular")) config.engine = 4;
        else if (!_wcsicmp(source, L"Obfuscated")) config.engine = 10;
        else { Log("Invalid PositionSource: forwarding only"); return TRUE; }
        if (!ReadNumber(L"Latitude", L"51.5074", config.latitude) ||
            !ReadNumber(L"Longitude", L"-0.1278", config.longitude) ||
            !ReadNumber(L"Altitude", L"15", config.altitude) ||
            !ReadNumber(L"Accuracy", L"5", config.accuracy) ||
            std::abs(config.latitude) > 90 || std::abs(config.longitude) > 180 || config.accuracy <= 0) {
            Log("Invalid config: forwarding only"); return TRUE;
        }
        framework = LoadLibraryExW(L"LocationFramework.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        auto vtable = find_vtbl(framework);
        if (!vtable) {
            Log("Vtable not found: forwarding only, framework=%p", framework);
            return TRUE;
        }
        // MinHook trampolines also cover compiler-devirtualized calls to these virtual methods.
        const std::array<void*, 3> targets{ vtable[3],vtable[7],vtable[8] };
        const std::array<void*, 3> detours{ reinterpret_cast<void*>(HookStart),reinterpret_cast<void*>(HookCached),reinterpret_cast<void*>(HookNew) };
        const std::array<void**, 3> originals{ reinterpret_cast<void**>(&realStart),reinterpret_cast<void**>(&realCached),reinterpret_cast<void**>(&realNew) };
        auto result = MH_Initialize();
        if (result != MH_OK) { Log("MH_Initialize failed: %s", MH_StatusToString(result)); return TRUE; }
        for (size_t i = 0;i < targets.size();++i) {
            result = MH_CreateHook(targets[i], detours[i], originals[i]);
            if (result != MH_OK) { Log("MH_CreateHook failed: %s", MH_StatusToString(result)); MH_Uninitialize(); return TRUE; }
            MH_QueueEnableHook(targets[i]);
        }
        result = MH_ApplyQueued();
        if (result != MH_OK) { Log("MH_ApplyQueued failed: %s", MH_StatusToString(result)); MH_Uninitialize(); return TRUE; }
#if 0
        // Replace the actual COM vtable slots too (normal and aggregated ATL implementations).
        unsigned patched = 0;
        auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
        auto sections = IMAGE_FIRST_SECTION(nt);
        for (unsigned n = 0;n < nt->FileHeader.NumberOfSections;++n) {
            if (memcmp(sections[n].Name, ".rdata", 6)) continue;
            auto first = reinterpret_cast<void**>(base + sections[n].VirtualAddress);
            size_t count = sections[n].Misc.VirtualSize / sizeof(void*);
            for (size_t j = 0;j + 8 < count;++j) {
                if (first[j + 3] != targets[0] || first[j + 7] != targets[1] || first[j + 8] != targets[2]) continue;
                DWORD old{}; if (!VirtualProtect(first + j + 3, 6 * sizeof(void*), PAGE_READWRITE, &old)) continue;
                InterlockedExchangePointer(first + j + 3, detours[0]);
                InterlockedExchangePointer(first + j + 7, detours[1]);
                InterlockedExchangePointer(first + j + 8, detours[2]);
                DWORD unused{}; VirtualProtect(first + j + 3, 6 * sizeof(void*), old, &unused); ++patched;
            }
        }
#endif
        // Hook code and trampolines must remain alive until the service host exits.
        HMODULE pinned{};
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(self), &pinned);
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(framework), &pinned);
        Log("Hooks installed: latitude=%.8f longitude=%.8f accuracy=%.2f engine=%lu", config.latitude, config.longitude, config.accuracy, config.engine);
        return TRUE;
    }
} // namespace

extern "C" __declspec(dllexport) void WINAPI SvchostPushServiceGlobals(void* globals) {
    auto fn = Export<void(WINAPI*)(void*)>("SvchostPushServiceGlobals"); if (fn) fn(globals);
}
extern "C" __declspec(dllexport) void WINAPI ServiceMain(DWORD argc, LPWSTR* argv) {
    auto fn = Export<void(WINAPI*)(DWORD, LPWSTR*)>("ServiceMain");
    if (!fn) return;
    InitOnceExecuteOnce(&hookOnce, InstallHooks, nullptr, nullptr);
    Log("Forwarding ServiceMain to original lfsvc.dll"); fn(argc, argv);
    Log("Original ServiceMain returned");
}
extern "C" HRESULT WINAPI ProxyDllCanUnloadNow() {
    auto fn = Export<HRESULT(WINAPI*)()>("DllCanUnloadNow");
    return fn ? fn() : E_FAIL;
}
extern "C" HRESULT WINAPI ProxyDllGetClassObject(REFCLSID clsid, REFIID iid, void** result) {
    auto fn = Export<HRESULT(WINAPI*)(REFCLSID, REFIID, void**)>("DllGetClassObject");
    return fn ? fn(clsid, iid, result) : E_FAIL;
}
BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH) {
        self = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}
