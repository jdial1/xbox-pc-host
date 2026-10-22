#include "pch.h"
#include "loader/Win32Layer.h"
#include "Log.h"

// Where a game's memory goes, for games near the console's limit: live bytes per heap (each heap labelled
// with the module that made it: msvcrt's is a MinGW game's malloc, Lua included; the process heap is
// MSVC code's, Mesa's and the drivers'), large VirtualAlloc commits with their caller, the app's total and
// the GPU's use. A line every 3 s while the total moves by 64 MB or more, starting with the game's first
// heap call.

namespace
{
    struct HeapStat
    {
        std::atomic<HANDLE> heap{ nullptr };
        std::atomic<int64_t> bytes{ 0 };
        std::atomic<int64_t> blocks{ 0 };
        wchar_t owner[48] = {};
    };
    constexpr int kHeaps = 32;
    HeapStat g_heaps[kHeaps];
    std::atomic<bool> g_reporter{ false };
    std::atomic<int64_t> g_bigVirtual{ 0 }; // committed so far by VirtualAlloc calls of 16 MB or more

    void ModuleName(const void* address, wchar_t* out, size_t size)
    {
        HMODULE module = nullptr;
        std::wstring name;
        if (HMODULE dll = GameDllContaining(address))
            name = GameDllPath(dll).filename().wstring();
        else if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     static_cast<LPCWSTR>(address), &module))
        {
            wchar_t path[MAX_PATH] = {};
            GetModuleFileNameW(module, path, MAX_PATH);
            name = std::filesystem::path(path).filename().wstring();
        }
        else
            name = L"game";
        wcsncpy_s(out, size, name.c_str(), _TRUNCATE);
    }

    HeapStat* StatFor(HANDLE heap)
    {
        for (auto& s : g_heaps)
        {
            HANDLE h = s.heap.load(std::memory_order_relaxed);
            if (h == heap) return &s;
            if (!h) break;
        }
        for (auto& s : g_heaps)
        {
            HANDLE expected = nullptr;
            if (s.heap.compare_exchange_strong(expected, heap))
            {
                if (!s.owner[0])
                    wcscpy_s(s.owner, heap == GetProcessHeap() ? L"process heap" : L"(heap)");
                return &s;
            }
            if (expected == heap) return &s;
        }
        return nullptr;
    }

    void Reporter();
    void StartReporter()
    {
        if (!g_reporter.exchange(true))
            std::thread(Reporter).detach();
    }

    void Reporter()
    {
        int64_t last = 0;
        for (;;)
        {
            Sleep(3000);
            int64_t app = 0;
            try { app = static_cast<int64_t>(winrt::Windows::System::MemoryManager::AppMemoryUsage()); }
            catch (...) {} // not in a package (the PC harness)
            if (!app)
                for (auto& s : g_heaps)
                    app += s.bytes.load();
            if (std::llabs(app - last) < (64ll << 20))
                continue;
            last = app;
            std::wstring line = L"mem: app " + std::to_wstring(app >> 20) + L" MB, gpu " +
                std::to_wstring(GameGpuMemoryUsage() >> 20) + L" MB, big VirtualAlloc " +
                std::to_wstring(g_bigVirtual.load() >> 20) + L" MB; heaps:";
            for (auto& s : g_heaps)
            {
                if (!s.heap.load()) break;
                int64_t b = s.bytes.load();
                if (b < (8ll << 20)) continue;
                line += L" " + std::wstring(s.owner) + L" " + std::to_wstring(b >> 20) + L" MB (" +
                    std::to_wstring(s.blocks.load()) + L")";
            }
            HOST_LOG(L"%s", line.c_str());
        }
    }

    HANDLE WINAPI Shim_HeapCreate(DWORD options, SIZE_T initial, SIZE_T maximum)
    {
        HANDLE heap = HeapCreate(options, initial, maximum);
        if (HeapStat* s = heap ? StatFor(heap) : nullptr)
        {
            wchar_t name[40];
            ModuleName(_ReturnAddress(), name, _countof(name));
            swprintf_s(s->owner, L"%s's heap", name);
            s->bytes = 0;
            s->blocks = 0;
        }
        return heap;
    }

    LPVOID WINAPI Shim_HeapAlloc(HANDLE heap, DWORD flags, SIZE_T size)
    {
        void* p = HeapAlloc(heap, flags, size);
        if (p)
            if (HeapStat* s = StatFor(heap))
            {
                s->bytes += static_cast<int64_t>(size);
                ++s->blocks;
                StartReporter();
            }
        return p;
    }

    BOOL WINAPI Shim_HeapFree(HANDLE heap, DWORD flags, LPVOID p)
    {
        if (p)
            if (HeapStat* s = StatFor(heap))
            {
                SIZE_T size = HeapSize(heap, 0, p);
                if (size != static_cast<SIZE_T>(-1))
                {
                    s->bytes -= static_cast<int64_t>(size);
                    --s->blocks;
                }
            }
        return HeapFree(heap, flags, p);
    }

    LPVOID WINAPI Shim_HeapReAlloc(HANDLE heap, DWORD flags, LPVOID p, SIZE_T size)
    {
        SIZE_T before = p ? HeapSize(heap, 0, p) : 0;
        void* q = HeapReAlloc(heap, flags, p, size);
        if (q && before != static_cast<SIZE_T>(-1))
            if (HeapStat* s = StatFor(heap))
                s->bytes += static_cast<int64_t>(size) - static_cast<int64_t>(before);
        return q;
    }

    // ntdll's forms, which msvcrt and Mesa's CRT may import directly.
    PVOID NTAPI Shim_RtlAllocateHeap(PVOID heap, ULONG flags, SIZE_T size) { return Shim_HeapAlloc(heap, flags, size); }
    BOOLEAN NTAPI Shim_RtlFreeHeap(PVOID heap, ULONG flags, PVOID p) { return Shim_HeapFree(heap, flags, p) ? TRUE : FALSE; }
    PVOID NTAPI Shim_RtlReAllocateHeap(PVOID heap, ULONG flags, PVOID p, SIZE_T size) { return Shim_HeapReAlloc(heap, flags, p, size); }
}

// From the VirtualAlloc shim: a large commit, logged with its caller and added to the running total.
void NoteGameVirtualAlloc(const void* caller, void* p, SIZE_T size, DWORD type)
{
    if (!p || !(type & MEM_COMMIT) || size < (16u << 20))
        return;
    g_bigVirtual += static_cast<int64_t>(size);
    wchar_t name[40];
    ModuleName(caller, name, _countof(name));
    HOST_LOG(L"VirtualAlloc %zu MB committed by %s (total in such blocks %lld MB)", size >> 20, name,
        static_cast<long long>(g_bigVirtual.load() >> 20));
    if (size >= (48u << 20))
        LogGameStack(L"  from"); // which code wants a block this big
}

void RegisterMemStats(Win32Layer& layer)
{
#define REG(key, fn) layer.Register(key, reinterpret_cast<void*>(&fn))
    REG("kernel32!HeapCreate", Shim_HeapCreate);
    REG("kernel32!HeapAlloc", Shim_HeapAlloc);
    REG("kernel32!HeapFree", Shim_HeapFree);
    REG("kernel32!HeapReAlloc", Shim_HeapReAlloc);
    REG("ntdll!RtlAllocateHeap", Shim_RtlAllocateHeap);
    REG("ntdll!RtlFreeHeap", Shim_RtlFreeHeap);
    REG("ntdll!RtlReAllocateHeap", Shim_RtlReAllocateHeap);
#undef REG
}
