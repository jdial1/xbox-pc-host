#include "pch.h"
#define _WINSOCK_DEPRECATED_NO_WARNINGS // the game asks for WSAStringToAddressA by name
#include <winsock2.h>
#include <ws2tcpip.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include "loader/Win32Layer.h"
#include "Log.h"

// Sockets are the real Winsock (the manifest has internetClient), so a game's network
// code runs unchanged. The one thing missing from the app partition that a game asks for
// by name is the old RtlGenRandom, which BCrypt covers.
namespace
{
    BOOLEAN WINAPI Shim_SystemFunction036(PVOID buffer, ULONG length)
    {
        return BCryptGenRandom(nullptr, static_cast<PUCHAR>(buffer), length, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
    }

    // ---- The old IPv4 helpers. Winsock returns per-thread buffers; a ring of them stands in (no
    // thread_local in the host: its TLS block is the game's).
    std::mutex g_netLock;
    char g_addrText[16][16];
    unsigned g_addrNext = 0;

    char* WINAPI Shim_inet_ntoa(in_addr address)
    {
        std::lock_guard lock(g_netLock);
        char* out = g_addrText[g_addrNext++ % 16];
        if (!inet_ntop(AF_INET, &address, out, 16))
            strcpy_s(out, 16, "0.0.0.0");
        return out;
    }

    unsigned long WINAPI Shim_inet_addr(const char* text)
    {
        in_addr a{};
        if (!text || inet_pton(AF_INET, text, &a) != 1)
            return INADDR_NONE;
        return a.s_addr;
    }

    // A hostent for the name's IPv4 addresses (up to 8), in a ring of 4 so recent results stay valid.
    struct HostEntry
    {
        hostent entry{};
        char name[256]{};
        char* aliases[1]{};
        in_addr addresses[8]{};
        char* list[9]{};
    };
    HostEntry g_hosts[4];
    unsigned g_hostNext = 0;

    hostent* WINAPI Shim_gethostbyname(const char* name)
    {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        addrinfo* result = nullptr;
        if (getaddrinfo(name && *name ? name : "localhost", nullptr, &hints, &result) != 0 || !result)
        {
            WSASetLastError(WSAHOST_NOT_FOUND);
            return nullptr;
        }
        std::lock_guard lock(g_netLock);
        HostEntry& h = g_hosts[g_hostNext++ % 4];
        h = HostEntry{};
        strncpy_s(h.name, name ? name : "", _TRUNCATE);
        int n = 0;
        for (addrinfo* a = result; a && n < 8; a = a->ai_next)
            if (a->ai_family == AF_INET)
            {
                h.addresses[n] = reinterpret_cast<sockaddr_in*>(a->ai_addr)->sin_addr;
                h.list[n] = reinterpret_cast<char*>(&h.addresses[n]);
                ++n;
            }
        freeaddrinfo(result);
        h.entry.h_name = h.name;
        h.entry.h_aliases = h.aliases;
        h.entry.h_addrtype = AF_INET;
        h.entry.h_length = sizeof(in_addr);
        h.entry.h_addr_list = h.list;
        return &h.entry;
    }
}

// SSPI's entry: a table of the security functions (AcquireCredentialsHandle, InitializeSecurityContext,
// EncryptMessage, ...), all real ones. WindowsApp.lib exports it; only the desktop headers declare it.
extern "C" __declspec(dllimport) void* __stdcall InitSecurityInterfaceA();
extern "C" __declspec(dllimport) void* __stdcall InitSecurityInterfaceW();

void RegisterNet(Win32Layer& layer)
{
    for (const char* dll : { "secur32", "security", "sspicli" })
    {
        layer.Register((std::string(dll) + "!InitSecurityInterfaceA").c_str(), reinterpret_cast<void*>(&InitSecurityInterfaceA));
        layer.Register((std::string(dll) + "!InitSecurityInterfaceW").c_str(), reinterpret_cast<void*>(&InitSecurityInterfaceW));
    }
    layer.Register("ws2_32!inet_ntoa", reinterpret_cast<void*>(&Shim_inet_ntoa));
    layer.Register("ws2_32!inet_addr", reinterpret_cast<void*>(&Shim_inet_addr));
    layer.Register("ws2_32!gethostbyname", reinterpret_cast<void*>(&Shim_gethostbyname));
#define WINSOCK(fn) layer.Register("ws2_32!" #fn, reinterpret_cast<void*>(&fn))
    WINSOCK(WSAStartup);
    // The rest of Winsock is in the app partition too; servers and LAN games need these.
    WINSOCK(accept);
    WINSOCK(bind);
    WINSOCK(listen);
    WINSOCK(getpeername);
    WINSOCK(getsockname);
    WINSOCK(setsockopt);
    WINSOCK(recvfrom);
    WINSOCK(sendto);
    WINSOCK(htonl);
    WINSOCK(htons);
    WINSOCK(ntohs);
    WINSOCK(__WSAFDIsSet);
    WINSOCK(WSAIoctl);
    WINSOCK(getnameinfo);
    layer.Register("bcrypt!BCryptGenRandom", reinterpret_cast<void*>(&BCryptGenRandom));
    WINSOCK(WSACleanup);
    WINSOCK(WSAGetLastError);
    WINSOCK(WSAStringToAddressA);
    WINSOCK(socket);
    WINSOCK(closesocket);
    WINSOCK(connect);
    WINSOCK(shutdown);
    WINSOCK(ioctlsocket);
    WINSOCK(getsockopt);
    WINSOCK(getaddrinfo);
    WINSOCK(freeaddrinfo);
    WINSOCK(recv);
    WINSOCK(send);
    WINSOCK(select);
#undef WINSOCK

    // Certificate chain checks for TLS. CryptProtectData (saved logins) isn't in the app partition
    // and stays unimplemented, which games treat as "can't save".
#define CRYPT32(fn) layer.Register("crypt32!" #fn, reinterpret_cast<void*>(&fn))
    CRYPT32(CertOpenStore);
    CRYPT32(CertCloseStore);
    CRYPT32(CertAddEncodedCertificateToStore);
    CRYPT32(CertFreeCertificateContext);
    CRYPT32(CertGetCertificateChain);
    CRYPT32(CertFreeCertificateChain);
#undef CRYPT32
    layer.Register("advapi32!SystemFunction036", reinterpret_cast<void*>(&Shim_SystemFunction036));
}
