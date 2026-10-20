#include "pch.h"
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <propidl.h>
#include <winrt/Windows.Media.Devices.h>
#include "loader/Win32Layer.h"
#include "Log.h"

// WASAPI's device side for games (SDL's "wasapi" driver, and others): MMDeviceEnumerator is
// desktop-only, but the audio client it leads to isn't. This enumerator knows one device, the
// default render endpoint, and its Activate hands out the real IAudioClient from
// ActivateAudioInterfaceAsync, the app partition's way in; everything after that is real WASAPI.
// The interfaces are declared here because the app partition's headers leave them out; the names
// differ from the desktop headers' so both builds compile, and the layouts and ids are Windows'.
namespace
{
    enum DataFlow { kRender = 0, kCapture = 1, kAll = 2 };
    constexpr DWORD kStateActive = 1;

    struct DECLSPEC_UUID("D666063F-1587-4E43-81F1-B948E807363F") DECLSPEC_NOVTABLE GameMMDevice : IUnknown
    {
        virtual HRESULT STDMETHODCALLTYPE Activate(REFIID iid, DWORD context, PROPVARIANT* params, void** out) = 0;
        virtual HRESULT STDMETHODCALLTYPE OpenPropertyStore(DWORD access, IUnknown** store) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetId(LPWSTR* id) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetState(DWORD* state) = 0;
    };
    struct DECLSPEC_UUID("1BE09788-6894-4089-8586-9A2A6C265AC5") DECLSPEC_NOVTABLE GameMMEndpoint : IUnknown
    {
        virtual HRESULT STDMETHODCALLTYPE GetDataFlow(int* flow) = 0;
    };
    struct DECLSPEC_UUID("0BD7A1BE-7A1A-44DB-8397-CC5392387B5E") DECLSPEC_NOVTABLE GameMMDeviceCollection : IUnknown
    {
        virtual HRESULT STDMETHODCALLTYPE GetCount(UINT* count) = 0;
        virtual HRESULT STDMETHODCALLTYPE Item(UINT index, GameMMDevice** device) = 0;
    };
    struct DECLSPEC_UUID("A95664D2-9614-4F35-A746-DE8DB63617E6") DECLSPEC_NOVTABLE GameMMDeviceEnumerator : IUnknown
    {
        virtual HRESULT STDMETHODCALLTYPE EnumAudioEndpoints(int flow, DWORD stateMask, GameMMDeviceCollection** devices) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetDefaultAudioEndpoint(int flow, int role, GameMMDevice** device) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetDevice(LPCWSTR id, GameMMDevice** device) = 0;
        virtual HRESULT STDMETHODCALLTYPE RegisterEndpointNotificationCallback(IUnknown* client) = 0;
        virtual HRESULT STDMETHODCALLTYPE UnregisterEndpointNotificationCallback(IUnknown* client) = 0;
    };
    struct DECLSPEC_UUID("886d8eeb-8cf2-4446-8d02-cdba1dbdcf99") DECLSPEC_NOVTABLE GamePropertyStore : IUnknown
    {
        virtual HRESULT STDMETHODCALLTYPE GetCount(DWORD* count) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetAt(DWORD index, PROPERTYKEY* key) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetValue(const PROPERTYKEY& key, PROPVARIANT* value) = 0;
        virtual HRESULT STDMETHODCALLTYPE SetValue(const PROPERTYKEY& key, const PROPVARIANT& value) = 0;
        virtual HRESULT STDMETHODCALLTYPE Commit() = 0;
    };

    // {BCDE0395-E52F-467C-8E3D-C4579291692E}
    constexpr GUID kClsidMMDeviceEnumerator = { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };

    // The keys games read: the device's names, its endpoint GUID and the engine's mix format.
    constexpr PROPERTYKEY kFriendlyName = { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };
    constexpr PROPERTYKEY kDeviceDesc = { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 2 };
    constexpr PROPERTYKEY kInterfaceFriendlyName = { { 0x026e516e, 0xb814, 0x414b, { 0x83, 0xcd, 0x85, 0x6d, 0x6f, 0xef, 0x48, 0x22 } }, 2 };
    constexpr PROPERTYKEY kEndpointGuid = { { 0x1da5d803, 0xd492, 0x4edd, { 0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e } }, 4 };
    constexpr PROPERTYKEY kDeviceFormat = { { 0xf19f064d, 0x082c, 0x4e27, { 0xbc, 0x73, 0x68, 0x82, 0xa1, 0xbb, 0x8e, 0x4c } }, 0 };
    constexpr PROPERTYKEY kKeys[] = { kFriendlyName, kDeviceDesc, kInterfaceFriendlyName, kEndpointGuid, kDeviceFormat };
    constexpr wchar_t kName[] = L"Speakers (Xbox)";

    bool Same(PROPERTYKEY const& a, PROPERTYKEY const& b) { return a.pid == b.pid && IsEqualGUID(a.fmtid, b.fmtid); }

    wchar_t* CoString(std::wstring const& s)
    {
        auto p = static_cast<wchar_t*>(CoTaskMemAlloc((s.size() + 1) * sizeof(wchar_t)));
        if (p) memcpy(p, s.c_str(), (s.size() + 1) * sizeof(wchar_t));
        return p;
    }

    // The default render endpoint's id, or empty when the system has no audio output.
    std::wstring DefaultRenderId()
    {
        try
        {
            using namespace winrt::Windows::Media::Devices;
            return std::wstring(MediaDevice::GetDefaultAudioRenderId(AudioDeviceRole::Default));
        }
        catch (...)
        {
            return {};
        }
    }

    // ActivateAudioInterfaceAsync calls back on a worker thread; the caller waits for it.
    struct ActivateDone : winrt::implements<ActivateDone, IActivateAudioInterfaceCompletionHandler>
    {
        winrt::handle event{ CreateEventW(nullptr, TRUE, FALSE, nullptr) };
        HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation*) override
        {
            SetEvent(event.get());
            return S_OK;
        }
    };

    HRESULT ActivateAudio(std::wstring const& id, REFIID iid, PROPVARIANT* params, void** out)
    {
        auto done = winrt::make_self<ActivateDone>();
        winrt::com_ptr<IActivateAudioInterfaceAsyncOperation> operation;
        HRESULT hr = ActivateAudioInterfaceAsync(id.c_str(), iid, params, done.get(), operation.put());
        if (FAILED(hr))
            return hr;
        if (WaitForSingleObject(done->event.get(), 5000) != WAIT_OBJECT_0)
            return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        HRESULT result = E_FAIL;
        winrt::com_ptr<IUnknown> made;
        hr = operation->GetActivateResult(&result, made.put());
        if (FAILED(hr)) return hr;
        if (FAILED(result)) return result;
        return made->QueryInterface(iid, out);
    }

    struct Device;

    struct PropertyStore : winrt::implements<PropertyStore, GamePropertyStore>
    {
        explicit PropertyStore(Device* device);
        HRESULT STDMETHODCALLTYPE GetCount(DWORD* count) override
        {
            if (!count) return E_POINTER;
            *count = static_cast<DWORD>(std::size(kKeys));
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE GetAt(DWORD index, PROPERTYKEY* key) override
        {
            if (!key) return E_POINTER;
            if (index >= std::size(kKeys)) return E_INVALIDARG;
            *key = kKeys[index];
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE GetValue(const PROPERTYKEY& key, PROPVARIANT* value) override;
        HRESULT STDMETHODCALLTYPE SetValue(const PROPERTYKEY&, const PROPVARIANT&) override { return STG_E_ACCESSDENIED; }
        HRESULT STDMETHODCALLTYPE Commit() override { return STG_E_ACCESSDENIED; }

    private:
        winrt::com_ptr<GameMMDevice> m_device;
    };

    struct Device : winrt::implements<Device, GameMMDevice, GameMMEndpoint>
    {
        explicit Device(std::wstring id) : id(std::move(id)) {}

        HRESULT STDMETHODCALLTYPE Activate(REFIID iid, DWORD, PROPVARIANT* params, void** out) override
        {
            if (!out) return E_POINTER;
            *out = nullptr;
            HRESULT hr = ActivateAudio(id, iid, params, out);
            HOST_LOG(L"MMDevice::Activate {%08X-...} -> 0x%08X", static_cast<unsigned>(iid.Data1), static_cast<unsigned>(hr));
            return hr;
        }
        HRESULT STDMETHODCALLTYPE OpenPropertyStore(DWORD, IUnknown** store) override
        {
            if (!store) return E_POINTER;
            *store = winrt::make_self<PropertyStore>(this).as<GamePropertyStore>().detach();
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE GetId(LPWSTR* out) override
        {
            if (!out) return E_POINTER;
            *out = CoString(id);
            return *out ? S_OK : E_OUTOFMEMORY;
        }
        HRESULT STDMETHODCALLTYPE GetState(DWORD* state) override
        {
            if (!state) return E_POINTER;
            *state = kStateActive;
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE GetDataFlow(int* flow) override
        {
            if (!flow) return E_POINTER;
            *flow = kRender;
            return S_OK;
        }

        // The engine's shared-mode format, as WAVEFORMATEX(TENSIBLE) bytes; read once from the client.
        std::vector<uint8_t> const& MixFormat()
        {
            std::lock_guard lock(m_lock);
            if (m_mixFormat.empty())
            {
                winrt::com_ptr<IAudioClient> client;
                WAVEFORMATEX* format = nullptr;
                if (SUCCEEDED(ActivateAudio(id, __uuidof(IAudioClient), nullptr, client.put_void())) &&
                    SUCCEEDED(client->GetMixFormat(&format)) && format)
                {
                    auto bytes = reinterpret_cast<const uint8_t*>(format);
                    m_mixFormat.assign(bytes, bytes + sizeof(WAVEFORMATEX) + format->cbSize);
                    CoTaskMemFree(format);
                }
            }
            return m_mixFormat;
        }

        std::wstring const id;

    private:
        std::mutex m_lock;
        std::vector<uint8_t> m_mixFormat;
    };

    PropertyStore::PropertyStore(Device* device) { m_device.copy_from(static_cast<GameMMDevice*>(device)); }

    HRESULT STDMETHODCALLTYPE PropertyStore::GetValue(const PROPERTYKEY& key, PROPVARIANT* value)
    {
        if (!value) return E_POINTER;
        PropVariantInit(value);
        auto device = static_cast<Device*>(m_device.get()); // only ever a Device
        if (Same(key, kFriendlyName) || Same(key, kDeviceDesc) || Same(key, kInterfaceFriendlyName))
        {
            value->vt = VT_LPWSTR;
            value->pwszVal = CoString(kName);
        }
        else if (Same(key, kEndpointGuid))
        {
            // The id is "...{0.0.0.00000000}.{guid}#..."; the endpoint GUID is that second brace pair.
            std::wstring const& id = device->id;
            size_t open = id.find(L"}.{");
            size_t close = open == std::wstring::npos ? open : id.find(L'}', open + 3);
            if (close == std::wstring::npos) return S_OK; // VT_EMPTY: no such value
            value->vt = VT_LPWSTR;
            value->pwszVal = CoString(id.substr(open + 2, close - open - 1));
        }
        else if (Same(key, kDeviceFormat))
        {
            auto const& format = device->MixFormat();
            if (format.empty()) return S_OK;
            value->vt = VT_BLOB;
            value->blob.cbSize = static_cast<ULONG>(format.size());
            value->blob.pBlobData = static_cast<BYTE*>(CoTaskMemAlloc(format.size()));
            if (!value->blob.pBlobData) { value->vt = VT_EMPTY; return E_OUTOFMEMORY; }
            memcpy(value->blob.pBlobData, format.data(), format.size());
        }
        return S_OK;
    }

    struct Collection : winrt::implements<Collection, GameMMDeviceCollection>
    {
        std::vector<winrt::com_ptr<GameMMDevice>> devices;
        HRESULT STDMETHODCALLTYPE GetCount(UINT* count) override
        {
            if (!count) return E_POINTER;
            *count = static_cast<UINT>(devices.size());
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE Item(UINT index, GameMMDevice** device) override
        {
            if (!device) return E_POINTER;
            *device = nullptr;
            if (index >= devices.size()) return E_INVALIDARG;
            devices[index].copy_to(device);
            return S_OK;
        }
    };

    struct Enumerator : winrt::implements<Enumerator, GameMMDeviceEnumerator>
    {
        // The one render device, made when first asked for, while the system has one.
        winrt::com_ptr<GameMMDevice> Render()
        {
            std::wstring id = DefaultRenderId();
            if (id.empty())
                return nullptr;
            std::lock_guard lock(m_lock);
            if (!m_render || static_cast<Device*>(m_render.get())->id != id)
                m_render = winrt::make_self<Device>(id).as<GameMMDevice>();
            return m_render;
        }

        HRESULT STDMETHODCALLTYPE EnumAudioEndpoints(int flow, DWORD stateMask, GameMMDeviceCollection** out) override
        {
            if (!out) return E_POINTER;
            auto collection = winrt::make_self<Collection>();
            if ((flow == kRender || flow == kAll) && (stateMask & kStateActive))
                if (auto device = Render())
                    collection->devices.push_back(device);
            *out = collection.as<GameMMDeviceCollection>().detach();
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE GetDefaultAudioEndpoint(int flow, int, GameMMDevice** out) override
        {
            if (!out) return E_POINTER;
            *out = nullptr;
            if (flow != kRender) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND); // no capture devices
            auto device = Render();
            if (!device) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
            *out = device.detach();
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE GetDevice(LPCWSTR id, GameMMDevice** out) override
        {
            if (!out) return E_POINTER;
            *out = nullptr;
            auto device = Render();
            if (!device || !id || static_cast<Device*>(device.get())->id != id) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
            *out = device.detach();
            return S_OK;
        }
        // Device changes are never announced; holding the client keeps the game's pointer valid.
        HRESULT STDMETHODCALLTYPE RegisterEndpointNotificationCallback(IUnknown* client) override
        {
            if (!client) return E_POINTER;
            std::lock_guard lock(m_lock);
            m_clients.emplace_back().copy_from(client);
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE UnregisterEndpointNotificationCallback(IUnknown* client) override
        {
            std::lock_guard lock(m_lock);
            auto it = std::find_if(m_clients.begin(), m_clients.end(), [&](auto const& c) { return c.get() == client; });
            if (it == m_clients.end()) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
            m_clients.erase(it);
            return S_OK;
        }

    private:
        std::mutex m_lock;
        winrt::com_ptr<GameMMDevice> m_render;
        std::vector<winrt::com_ptr<IUnknown>> m_clients;
    };
}

bool IsMMDeviceEnumerator(REFCLSID clsid) { return IsEqualGUID(clsid, kClsidMMDeviceEnumerator) != FALSE; }

HRESULT CreateMMDeviceEnumerator(REFIID iid, void** out)
{
    if (!out) return E_POINTER;
    *out = nullptr;
    auto enumerator = winrt::make_self<Enumerator>();
    HRESULT hr = enumerator->QueryInterface(iid, out);
    HOST_LOG(L"MMDeviceEnumerator: default render device %s", DefaultRenderId().empty() ? L"none" : L"present");
    return hr;
}
