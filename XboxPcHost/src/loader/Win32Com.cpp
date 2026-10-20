#include "pch.h"
#include "loader/Win32Layer.h"
#include "Log.h"

// COM objects the console doesn't have, in-process. CoCreateInstance hands out these for their
// class ids and goes to the real one for everything else.
//
// DirectInput 8: SDL (inside Factorio and others) creates it for rumble and joysticks and fails its
// whole startup if it can't. This one initializes, and enumerates no devices; controllers come
// through XInput or Windows.Gaming.Input instead.
namespace
{
    // {25E609E4-B259-11CF-BFC7-444553540000}
    constexpr GUID kClsidDirectInput8 = { 0x25E609E4, 0xB259, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
    // IDirectInput8W {BF798031-483A-4DA2-AA99-5D64ED369700}; the A version ends in ...00 too, with 0x30.
    constexpr GUID kIidDirectInput8W = { 0xBF798031, 0x483A, 0x4DA2, { 0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00 } };
    constexpr GUID kIidDirectInput8A = { 0xBF798030, 0x483A, 0x4DA2, { 0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00 } };
    constexpr HRESULT kDeviceNotRegistered = REGDB_E_CLASSNOTREG; // DIERR_DEVICENOTREG

    struct FakeDirectInput8
    {
        void** vtable;
    };

    HRESULT STDMETHODCALLTYPE DI_QueryInterface(FakeDirectInput8* self, REFIID iid, void** out);
    ULONG STDMETHODCALLTYPE DI_AddRef(FakeDirectInput8*) { return 2; }   // one static object, never freed
    ULONG STDMETHODCALLTYPE DI_Release(FakeDirectInput8*) { return 1; }
    HRESULT STDMETHODCALLTYPE DI_CreateDevice(FakeDirectInput8*, REFGUID, void** device, IUnknown*)
    {
        if (device) *device = nullptr;
        return kDeviceNotRegistered;
    }
    HRESULT STDMETHODCALLTYPE DI_EnumDevices(FakeDirectInput8*, DWORD, void*, void*, DWORD) { return S_OK; } // nothing attached
    HRESULT STDMETHODCALLTYPE DI_GetDeviceStatus(FakeDirectInput8*, REFGUID) { return S_FALSE; }       // DI_NOTATTACHED
    HRESULT STDMETHODCALLTYPE DI_RunControlPanel(FakeDirectInput8*, HWND, DWORD) { return S_OK; }
    HRESULT STDMETHODCALLTYPE DI_Initialize(FakeDirectInput8*, HINSTANCE, DWORD) { return S_OK; }
    HRESULT STDMETHODCALLTYPE DI_FindDevice(FakeDirectInput8*, REFGUID, LPCWSTR, GUID*) { return kDeviceNotRegistered; }
    HRESULT STDMETHODCALLTYPE DI_EnumDevicesBySemantics(FakeDirectInput8*, LPCWSTR, void*, void*, void*, DWORD) { return S_OK; }
    HRESULT STDMETHODCALLTYPE DI_ConfigureDevices(FakeDirectInput8*, void*, void*, DWORD, void*) { return E_NOTIMPL; }

    // IDirectInput8 method order: IUnknown, CreateDevice, EnumDevices, GetDeviceStatus, RunControlPanel,
    // Initialize, FindDevice, EnumDevicesBySemantics, ConfigureDevices. A and W share the layout.
    void* g_diVtable[] = {
        reinterpret_cast<void*>(&DI_QueryInterface), reinterpret_cast<void*>(&DI_AddRef), reinterpret_cast<void*>(&DI_Release),
        reinterpret_cast<void*>(&DI_CreateDevice), reinterpret_cast<void*>(&DI_EnumDevices), reinterpret_cast<void*>(&DI_GetDeviceStatus),
        reinterpret_cast<void*>(&DI_RunControlPanel), reinterpret_cast<void*>(&DI_Initialize), reinterpret_cast<void*>(&DI_FindDevice),
        reinterpret_cast<void*>(&DI_EnumDevicesBySemantics), reinterpret_cast<void*>(&DI_ConfigureDevices),
    };
    FakeDirectInput8 g_directInput{ g_diVtable };

    HRESULT STDMETHODCALLTYPE DI_QueryInterface(FakeDirectInput8* self, REFIID iid, void** out)
    {
        if (!out) return E_POINTER;
        if (IsEqualGUID(iid, __uuidof(IUnknown)) || IsEqualGUID(iid, kIidDirectInput8W) || IsEqualGUID(iid, kIidDirectInput8A))
        {
            *out = self;
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }

    HRESULT WINAPI Shim_DirectInput8Create(HINSTANCE, DWORD, REFIID iid, void** out, IUnknown*)
    {
        HOST_LOG(L"DirectInput8: the no-devices stand-in");
        return DI_QueryInterface(&g_directInput, iid, out);
    }

    HRESULT WINAPI Shim_CoCreateInstance(REFCLSID clsid, IUnknown* outer, DWORD context, REFIID iid, void** out)
    {
        if (IsEqualGUID(clsid, kClsidDirectInput8))
            return Shim_DirectInput8Create(nullptr, 0x0800, iid, out, outer);
        if (IsMMDeviceEnumerator(clsid))
            return CreateMMDeviceEnumerator(iid, out);
        HRESULT hr = CoCreateInstance(clsid, outer, context, iid, out);
        HOST_LOG(L"CoCreateInstance {%08X-%04X-%04X-...} -> 0x%08X", static_cast<unsigned>(clsid.Data1), clsid.Data2, clsid.Data3,
            static_cast<unsigned>(hr));
        return hr;
    }
}

void RegisterCom(Win32Layer& layer)
{
    for (const char* key : { "ole32!CoCreateInstance", "combase!CoCreateInstance", "kernel32!CoCreateInstance" })
        layer.Register(key, reinterpret_cast<void*>(&Shim_CoCreateInstance));
    layer.Register("dinput8!DirectInput8Create", reinterpret_cast<void*>(&Shim_DirectInput8Create));
}
