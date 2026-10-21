#include "pch.h"
#include <d3dcompiler.h>
#include "loader/Win32Layer.h"
#include "Log.h"

// The mouse cursor for D3D games. On Windows the system draws it over the game's window; on the console
// nothing does, so games that leave the cursor to Windows (Unity, most engines) showed none. Each frame,
// just before Present, the host draws an arrow at the cursor into the game's back buffer. The draw runs
// in a private device context state (SwapDeviceContextState swaps the whole pipeline out and back), so
// the game's rendering state is untouched. ShowCursor and SetCursor(null) hide it, as they hide Windows' cursor;
// games that draw their own cursor hide the system one, and so this one too.
namespace
{
    // ---- What the game asked for
    std::atomic<int> g_showCount{ 0 };      // ShowCursor's display counter: shown while >= 0
    std::atomic<bool> g_cursorSet{ false }; // SetCursor was called at least once
    std::atomic<HCURSOR> g_current{ nullptr };

    std::atomic<int> g_logged{ 0 }; // the first calls, to see how a game shows and hides its cursor
    int WINAPI Shim_ShowCursor(BOOL show)
    {
        int n = show ? ++g_showCount : --g_showCount;
        if (g_logged++ < 12) HOST_LOG(L"ShowCursor(%d) -> %d", show, n);
        return n;
    }
    HCURSOR WINAPI Shim_SetCursor(HCURSOR cursor)
    {
        g_cursorSet = true;
        if (g_logged++ < 12) HOST_LOG(L"SetCursor(%p)", static_cast<void*>(cursor));
        return g_current.exchange(cursor);
    }
    HCURSOR WINAPI Shim_GetCursor() { return g_current; }

    bool Visible() { return g_showCount >= 0 && !(g_cursorSet && !g_current); }

    // ---- The arrow: 'X' outline, 'W' fill, anything else clear
    const char* const kArrow[] = {
        "X...........",
        "XX..........",
        "XWX.........",
        "XWWX........",
        "XWWWX.......",
        "XWWWWX......",
        "XWWWWWX.....",
        "XWWWWWWX....",
        "XWWWWWWWX...",
        "XWWWWWWWWX..",
        "XWWWWWWWWWX.",
        "XWWWWWWXXXXX",
        "XWWWXWWX....",
        "XWWXXWWX....",
        "XWX..XWWX...",
        "XX...XWWX...",
        "X.....XWWX..",
        "......XWWX..",
        ".......XWWX.",
        ".......XWWX.",
        "........XX..",
    };
    constexpr UINT kArrowW = 12, kArrowH = 21;
    constexpr float kScale = 3.0f; // on-screen pixels per arrow pixel at 1080p: big enough to see from the couch

    const char kShader[] = R"(
cbuffer Place : register(b0) { float4 rect; } // left, top, right, bottom in clip space
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID)
{
    float2 t = float2(id & 1, id >> 1);
    V o;
    o.pos = float4(lerp(rect.x, rect.z, t.x), lerp(rect.y, rect.w, t.y), 0, 1);
    o.uv = t;
    return o;
}
Texture2D arrow : register(t0);
SamplerState point_ : register(s0);
float4 ps(V i) : SV_Target { return arrow.Sample(point_, i.uv); }
)";

    struct Overlay
    {
        winrt::com_ptr<ID3D11Device> device;
        winrt::com_ptr<ID3D11DeviceContext1> context;    // the immediate context
        winrt::com_ptr<ID3DDeviceContextState> state;    // ours, swapped in for the draw
        winrt::com_ptr<ID3D11VertexShader> vs;
        winrt::com_ptr<ID3D11PixelShader> ps;
        winrt::com_ptr<ID3D11ShaderResourceView> arrow;
        winrt::com_ptr<ID3D11SamplerState> sampler;
        winrt::com_ptr<ID3D11BlendState> blend;
        winrt::com_ptr<ID3D11RasterizerState> raster;
        winrt::com_ptr<ID3D11DepthStencilState> depth;
        winrt::com_ptr<ID3D11Buffer> place;
        bool failed = false;
    };
    std::mutex g_lock;
    Overlay g_overlay;

    bool Compile(const char* entry, const char* target, winrt::com_ptr<ID3DBlob>& out)
    {
        winrt::com_ptr<ID3DBlob> errors;
        HRESULT hr = D3DCompile(kShader, sizeof(kShader) - 1, "cursor", nullptr, nullptr, entry, target, 0, 0, out.put(), errors.put());
        if (FAILED(hr))
            HOST_LOG(L"cursor overlay: %S failed to compile: %S", entry, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
        return SUCCEEDED(hr);
    }

    // Everything the overlay needs on this device, once.
    bool Setup(Overlay& o, ID3D11Device* device)
    {
        if (o.device.get() == device) return !o.failed;
        o = {};
        o.device.copy_from(device);
        o.failed = true;
        winrt::com_ptr<ID3DBlob> vsCode, psCode;
        winrt::com_ptr<ID3D11Device1> device1;
        winrt::com_ptr<ID3D11DeviceContext> immediate;
        device->GetImmediateContext(immediate.put());
        D3D_FEATURE_LEVEL level = device->GetFeatureLevel(), chosen;
        UINT stateFlags = (device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED) ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED : 0;
        HRESULT hr = device->QueryInterface(__uuidof(ID3D11Device1), device1.put_void());
        if (SUCCEEDED(hr)) hr = immediate->QueryInterface(__uuidof(ID3D11DeviceContext1), o.context.put_void());
        if (SUCCEEDED(hr)) hr = device1->CreateDeviceContextState(stateFlags, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device), &chosen, o.state.put());
        if (FAILED(hr))
        {
            HOST_LOG(L"cursor overlay: no device context state (0x%08X)", static_cast<unsigned>(hr));
            return false;
        }
        if (!Compile("vs", "vs_4_0", vsCode) || !Compile("ps", "ps_4_0", psCode) ||
            FAILED(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, o.vs.put())) ||
            FAILED(device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, o.ps.put())))
            return false;

        std::vector<uint32_t> pixels(kArrowW * kArrowH);
        for (UINT y = 0; y < kArrowH; ++y)
            for (UINT x = 0; x < kArrowW; ++x)
            {
                char c = kArrow[y][x];
                pixels[y * kArrowW + x] = c == 'X' ? 0xFF000000u : c == 'W' ? 0xFFFFFFFFu : 0u; // RGBA in memory
            }
        D3D11_TEXTURE2D_DESC t{ kArrowW, kArrowH, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D11_USAGE_IMMUTABLE,
            D3D11_BIND_SHADER_RESOURCE, 0, 0 };
        D3D11_SUBRESOURCE_DATA init{ pixels.data(), kArrowW * 4, 0 };
        winrt::com_ptr<ID3D11Texture2D> texture;
        if (FAILED(device->CreateTexture2D(&t, &init, texture.put())) ||
            FAILED(device->CreateShaderResourceView(texture.get(), nullptr, o.arrow.put())))
            return false;

        D3D11_SAMPLER_DESC s{};
        s.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        s.MaxLOD = D3D11_FLOAT32_MAX;
        D3D11_BLEND_DESC b{};
        b.RenderTarget[0].BlendEnable = TRUE;
        b.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        b.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        b.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        b.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        b.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        b.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        b.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        D3D11_RASTERIZER_DESC r{};
        r.FillMode = D3D11_FILL_SOLID;
        r.CullMode = D3D11_CULL_NONE;
        r.DepthClipEnable = TRUE;
        D3D11_DEPTH_STENCIL_DESC d{}; // depth and stencil off
        D3D11_BUFFER_DESC c{ 16, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
        if (FAILED(device->CreateSamplerState(&s, o.sampler.put())) || FAILED(device->CreateBlendState(&b, o.blend.put())) ||
            FAILED(device->CreateRasterizerState(&r, o.raster.put())) || FAILED(device->CreateDepthStencilState(&d, o.depth.put())) ||
            FAILED(device->CreateBuffer(&c, nullptr, o.place.put())))
            return false;
        o.failed = false;
        HOST_LOG(L"cursor overlay ready");
        return true;
    }
}

void DrawGameCursor(IDXGISwapChain* swapChain)
{
    if (!Visible())
        return;
    winrt::com_ptr<ID3D11Texture2D> buffer;
    if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), buffer.put_void())))
        return; // not a D3D11 swapchain
    winrt::com_ptr<ID3D11Device> device;
    buffer->GetDevice(device.put());
    std::lock_guard lock(g_lock);
    Overlay& o = g_overlay;
    if (!Setup(o, device.get()))
        return;

    D3D11_TEXTURE2D_DESC desc;
    buffer->GetDesc(&desc);
    winrt::com_ptr<ID3D11RenderTargetView> target;
    HRESULT rtv = device->CreateRenderTargetView(buffer.get(), nullptr, target.put());
    static std::atomic<int> s_logged{ 0 }; // constant-initialized: no TLS guard
    if (s_logged++ < 2)
        HOST_LOG(L"cursor overlay: back buffer %ux%u format %d, RTV 0x%08X, cursor %ld,%ld of %ux%u", desc.Width, desc.Height,
            static_cast<int>(desc.Format), static_cast<unsigned>(rtv), GameCursorPosition().x, GameCursorPosition().y,
            GameScreenWidth(), GameScreenHeight());
    if (FAILED(rtv))
        return;

    // Where the cursor is, from window pixels to the back buffer's clip space.
    POINT at = GameCursorPosition();
    float sx = static_cast<float>(desc.Width) / std::max<UINT>(GameScreenWidth(), 1);
    float sy = static_cast<float>(desc.Height) / std::max<UINT>(GameScreenHeight(), 1);
    float scale = kScale * desc.Height / 1080.0f;
    float x0 = at.x * sx, y0 = at.y * sy, x1 = x0 + kArrowW * scale, y1 = y0 + kArrowH * scale;
    float rect[4] = { x0 / desc.Width * 2 - 1, 1 - y0 / desc.Height * 2, x1 / desc.Width * 2 - 1, 1 - y1 / desc.Height * 2 };

    ID3D11DeviceContext1* dc = o.context.get();
    winrt::com_ptr<ID3DDeviceContextState> gameState;
    dc->SwapDeviceContextState(o.state.get(), gameState.put()); // the game's pipeline is set aside, whole
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(dc->Map(o.place.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        memcpy(mapped.pData, rect, sizeof(rect));
        dc->Unmap(o.place.get(), 0);
    }

    D3D11_VIEWPORT viewport{ 0, 0, static_cast<float>(desc.Width), static_cast<float>(desc.Height), 0, 1 };
    ID3D11RenderTargetView* targets[] = { target.get() };
    dc->OMSetRenderTargets(1, targets, nullptr);
    dc->OMSetBlendState(o.blend.get(), nullptr, 0xFFFFFFFF);
    dc->OMSetDepthStencilState(o.depth.get(), 0);
    dc->RSSetViewports(1, &viewport);
    dc->RSSetState(o.raster.get());
    dc->IASetInputLayout(nullptr);
    dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    dc->VSSetShader(o.vs.get(), nullptr, 0);
    ID3D11Buffer* place[] = { o.place.get() };
    dc->VSSetConstantBuffers(0, 1, place);
    dc->PSSetShader(o.ps.get(), nullptr, 0);
    ID3D11ShaderResourceView* views[] = { o.arrow.get() };
    dc->PSSetShaderResources(0, 1, views);
    ID3D11SamplerState* samplers[] = { o.sampler.get() };
    dc->PSSetSamplers(0, 1, samplers);
    dc->Draw(4, 0);
    ID3D11RenderTargetView* none[] = { nullptr };
    dc->OMSetRenderTargets(1, none, nullptr); // don't keep the game's back buffer bound in our state
    dc->SwapDeviceContextState(gameState.get(), nullptr); // and the game's pipeline is back as it was
}

void RegisterCursor(Win32Layer& layer)
{
    layer.Register("user32!ShowCursor", reinterpret_cast<void*>(&Shim_ShowCursor));
    layer.Register("user32!SetCursor", reinterpret_cast<void*>(&Shim_SetCursor));
    layer.Register("user32!GetCursor", reinterpret_cast<void*>(&Shim_GetCursor));
}
