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
}

void RegisterNet(Win32Layer& layer)
{
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
