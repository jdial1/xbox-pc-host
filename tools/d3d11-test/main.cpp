// The D3D11 setup newer engines use (Factorio and others): a factory from CreateDXGIFactory2, a
// swapchain from CreateSwapChainForHwnd, a pixel shader compiled at run time with D3DCompile.
// Draws a full-screen color cycle for 120 frames and exits 42; any failed step exits with its number.
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cmath>

static const char kShaders[] =
    "float4 vs(uint id : SV_VertexID) : SV_Position {"
    "  float2 p = float2((id << 1) & 2, id & 2); return float4(p * float2(2, -2) + float2(-1, 1), 0, 1); }"
    "cbuffer c : register(b0) { float t; };"
    "float4 ps(float4 pos : SV_Position) : SV_Target {"
    "  return float4(0.5 + 0.5 * sin(t + pos.x / 300), 0.5 + 0.5 * sin(t * 1.3), 0.5 + 0.5 * cos(t + pos.y / 200), 1); }";

static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

#define CHECK(step, expr) do { HRESULT hr_ = (expr); if (FAILED(hr_)) { printf("d3d11-test: step %d failed 0x%08lx\n", step, hr_); return step; } } while (0)

int main()
{
    WNDCLASSW wc{}; wc.lpfnWndProc = Proc; wc.lpszClassName = L"D3D11Test"; wc.hInstance = GetModuleHandleW(nullptr);
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, L"D3D11Test", L"d3d11-test", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 1280, 720, nullptr, nullptr, wc.hInstance, nullptr);

    IDXGIFactory2* factory = nullptr;
    CHECK(1, CreateDXGIFactory2(0, __uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory)));
    IDXGIAdapter1* adapter = nullptr;
    CHECK(2, factory->EnumAdapters1(0, &adapter));
    ID3D11Device* device = nullptr; ID3D11DeviceContext* ctx = nullptr;
    CHECK(3, D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &ctx));

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.SampleDesc.Count = 1; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; // width/height 0: the window's size
    IDXGISwapChain1* swap = nullptr;
    CHECK(4, factory->CreateSwapChainForHwnd(device, hwnd, &sd, nullptr, nullptr, &swap));

    ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
    CHECK(5, D3DCompile(kShaders, sizeof(kShaders) - 1, "shaders", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vsb, &err));
    CHECK(6, D3DCompile(kShaders, sizeof(kShaders) - 1, "shaders", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &psb, &err));
    ID3D11VertexShader* vs = nullptr; ID3D11PixelShader* ps = nullptr;
    CHECK(7, device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs));
    CHECK(8, device->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps));
    D3D11_BUFFER_DESC bd{ 16, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE };
    ID3D11Buffer* cb = nullptr;
    CHECK(9, device->CreateBuffer(&bd, nullptr, &cb));

    for (int frame = 0; frame < 120; ++frame)
    {
        ID3D11Texture2D* back = nullptr; ID3D11RenderTargetView* rtv = nullptr;
        CHECK(10, swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back)));
        CHECK(11, device->CreateRenderTargetView(back, nullptr, &rtv));
        D3D11_TEXTURE2D_DESC td; back->GetDesc(&td);
        D3D11_VIEWPORT vp{ 0, 0, float(td.Width), float(td.Height), 0, 1 };
        D3D11_MAPPED_SUBRESOURCE m;
        CHECK(12, ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m));
        *static_cast<float*>(m.pData) = frame / 20.0f;
        ctx->Unmap(cb, 0);
        ctx->OMSetRenderTargets(1, &rtv, nullptr); ctx->RSSetViewports(1, &vp);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs, nullptr, 0); ctx->PSSetShader(ps, nullptr, 0); ctx->PSSetConstantBuffers(0, 1, &cb);
        ctx->Draw(3, 0);
        CHECK(13, swap->Present(1, 0));
        if (frame == 0) printf("d3d11-test: first frame %ux%u\n", td.Width, td.Height);
        rtv->Release(); back->Release();
        MSG msg; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
    }
    printf("d3d11-test: 120 frames presented\n");
    return 42;
}
