// A TLS slot for game images. The host can give the Windows loader's per-thread TLS blocks to only as
// many images as it has modules with a TLS directory, and each game image (the exe and every game DLL
// with static TLS) needs a block of its own at gs:[0x58][its _tls_index]. This DLL is nothing but such a
// block: copies of it under different names (XboxPcHostTls1.dll, ...) are loaded for real, each gets its
// own index, and the host points a game image's _tls_index at one of them.
//
// Built with the static CRT (/MT): its tlssup makes the TLS directory. (A hand-made directory in a
// CRT-free DLL was ignored by the loader: the DLL never got an index.)

#include <windows.h>

#ifndef TLS_SLOT_BYTES
#define TLS_SLOT_BYTES 65536
#endif

extern "C" ULONG _tls_index;
extern "C" char _tls_start, _tls_end;

// The block's room.
__declspec(thread) volatile char t_slot[TLS_SLOT_BYTES];

// The host's way in: this copy's TLS index and how many bytes each thread's block holds.
extern "C" __declspec(dllexport) ULONG TlsSlotIndex(SIZE_T* bytes)
{
    if (bytes)
        *bytes = static_cast<SIZE_T>(&_tls_end - &_tls_start);
    t_slot[0] = 0; // keeps the buffer referenced
    return _tls_index;
}
