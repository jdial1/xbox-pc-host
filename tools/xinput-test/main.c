// Polls the controller through XInput, statically (xinput1_4) and through LoadLibrary
// (xinput1_3), for 1.5 seconds. Exits 42 if both saw A held and the left stick pushed up,
// which host-test --inject pad does at 300 ms.
#include <windows.h>
#include <xinput.h>
#include <stdio.h>

typedef DWORD(WINAPI* GetStateFn)(DWORD, XINPUT_STATE*);

int main(void)
{
    HMODULE old = LoadLibraryA("xinput1_3.dll");
    GetStateFn dynamicGetState = old ? (GetStateFn)GetProcAddress(old, "XInputGetState") : NULL;
    int sawStatic = 0, sawDynamic = 0, connected = 0;
    DWORD lastPacket = 0, packets = 0;
    for (int ms = 0; ms < 1500; ms += 5)
    {
        XINPUT_STATE s;
        ZeroMemory(&s, sizeof(s)); // a stub that "succeeds" without filling it must not pass
        if (XInputGetState(0, &s) == ERROR_SUCCESS)
        {
            connected = 1;
            if (s.dwPacketNumber != lastPacket) { lastPacket = s.dwPacketNumber; ++packets; }
            if ((s.Gamepad.wButtons & XINPUT_GAMEPAD_A) && s.Gamepad.sThumbLY > 30000) sawStatic = 1;
        }
        ZeroMemory(&s, sizeof(s));
        if (dynamicGetState && dynamicGetState(0, &s) == ERROR_SUCCESS &&
            (s.Gamepad.wButtons & XINPUT_GAMEPAD_A) && s.Gamepad.sThumbLY > 30000)
            sawDynamic = 1;
        Sleep(5);
    }
    printf("xinput-test: connected=%d static=%d dynamic=%d (via %p) packets=%lu\n", connected, sawStatic, sawDynamic,
        (void*)dynamicGetState, packets);
    return sawStatic && sawDynamic ? 42 : 1;
}
