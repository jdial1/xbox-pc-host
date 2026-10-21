// Loads the slot DLLs and prints each one's TLS index and block, to check the loader gave each its own.
#include <windows.h>
#include <cstdio>
#include <intrin.h>

__declspec(thread) int t_own = 1; // so this exe has index 0

int main()
{
    auto blocks = reinterpret_cast<void**>(__readgsqword(0x58));
    printf("exe: block %p (t_own %d)\n", blocks[0], t_own);
    for (int i = 1; i <= 10; ++i)
    {
        wchar_t name[64];
        swprintf(name, 64, L"XboxPcHostTls%d.dll", i);
        HMODULE dll = LoadLibraryW(name);
        auto info = dll ? reinterpret_cast<ULONG (*)(SIZE_T*)>(GetProcAddress(dll, "TlsSlotIndex")) : nullptr;
        SIZE_T bytes = 0;
        ULONG index = info ? info(&bytes) : ~0u;
        blocks = reinterpret_cast<void**>(__readgsqword(0x58));
        printf("%ls: module %p index %lu bytes %zu block %p\n", name, dll, index, bytes, index != ~0u ? blocks[index] : nullptr);
    }
}
