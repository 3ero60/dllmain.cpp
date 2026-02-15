#define NOMINMAX

#pragma comment(lib, "libMinHook.x64.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <MinHook.h>
#include <iostream>
#include <mutex>
#include <vector>
#include <random>
#include <cmath>

// ==================== Configuration ====================
#define ENABLE_CONSOLE 1
#define SCREEN_WIDTH 1920
#define SCREEN_HEIGHT 1440
#define CROSSHAIR_TOLERANCE 80
#define CROSSHAIR_TOLERANCE_SQ (CROSSHAIR_TOLERANCE * CROSSHAIR_TOLERANCE)
#define SHOOT_COOLDOWN 150
#define DELAY_MIN 85
#define DELAY_MAX 120

// ==================== Types ====================
typedef HRESULT(__stdcall* Present_t)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(__stdcall* ResizeBuffers_t)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

Present_t g_originalPresent = nullptr;
ResizeBuffers_t g_originalResizeBuffers = nullptr;

// ==================== COM Helper ====================
template<typename T>
class ComPtr {
public:
    ComPtr(T* ptr = nullptr) : m_ptr(ptr) {}
    ~ComPtr() { if (m_ptr) m_ptr->Release(); }
    T** operator&() { return &m_ptr; }
    T* operator->() { return m_ptr; }
    T* get() { return m_ptr; }
    explicit operator bool() { return m_ptr != nullptr; }
private:
    T* m_ptr;
};

// ==================== Global State ====================
std::mutex g_mutex;

bool g_hookInitialized = false;
bool g_unloading = false;

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
ID3D11RenderTargetView* g_renderTargetView = nullptr;

// Statistics
int g_framesProcessed = 0;
int g_shotsHit = 0;

// ==================== Utility Functions ====================

void Log(const char* fmt, ...) {
#if ENABLE_CONSOLE
    char buffer[512];
    va_list args;
    va_start(args, fmt);
    vsprintf_s(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    printf("%s\n", buffer);
#endif
}

// ==================== Present Hook ====================

HRESULT __stdcall HookedPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {
    std::lock_guard<std::mutex> lock(g_mutex);

    if (g_unloading) {
        return g_originalPresent(pSwapChain, SyncInterval, Flags);
    }

    static bool init = false;
    if (!init) {
        ComPtr<ID3D11Texture2D> backBuffer;
        if (SUCCEEDED(pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer))) {
            if (g_device) {
                g_device->CreateRenderTargetView(backBuffer.get(), nullptr, &g_renderTargetView);
                Log("[+] Render target view created");
            }
        }
        init = true;
    }

    g_framesProcessed++;

    return g_originalPresent(pSwapChain, SyncInterval, Flags);
}

// ==================== ResizeBuffers Hook ====================

HRESULT __stdcall HookedResizeBuffers(IDXGISwapChain* pThis, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags) {
    std::lock_guard<std::mutex> lock(g_mutex);

    Log("[*] Window resize: %dx%d", Width, Height);

    if (g_context) {
        g_context->ClearState();
    }

    if (g_renderTargetView) {
        g_renderTargetView->Release();
        g_renderTargetView = nullptr;
    }

    HRESULT hr = g_originalResizeBuffers(pThis, BufferCount, Width, Height, NewFormat, SwapChainFlags);

    if (SUCCEEDED(hr)) {
        ComPtr<ID3D11Texture2D> backBuffer;
        if (SUCCEEDED(pThis->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer))) {
            if (g_device) {
                g_device->CreateRenderTargetView(backBuffer.get(), nullptr, &g_renderTargetView);
            }
        }
        Log("[+] Resized successfully");
    }

    return hr;
}

// ==================== Hook Installation ====================

IDXGISwapChain* CreateDummySwapChain() {
    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.lpfnWndProc = DefWindowProc;
    wc.lpszClassName = L"DummyD3D11";
    wc.hInstance = GetModuleHandle(nullptr);

    if (!RegisterClassEx(&wc)) return nullptr;

    HWND hwnd = CreateWindow(wc.lpszClassName, L"Dummy", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        UnregisterClass(wc.lpszClassName, wc.hInstance);
        return nullptr;
    }

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = 100;
    sd.BufferDesc.Height = 100;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGISwapChain* swapChain = nullptr;

    D3D_FEATURE_LEVEL featureLevels[] = {D3D_FEATURE_LEVEL_11_0};

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        featureLevels, 1, D3D11_SDK_VERSION,
        &sd, &swapChain, &device, nullptr, &context
    );

    if (device) {
        g_device = device;
        if (context) {
            g_context = context;
        }
    }

    DestroyWindow(hwnd);
    UnregisterClass(wc.lpszClassName, wc.hInstance);

    if (FAILED(hr)) {
        Log("[-] D3D11CreateDeviceAndSwapChain failed: 0x%X", hr);
        return nullptr;
    }

    return swapChain;
}

bool InstallHooks() {
    Log("[*] Installing hooks...");

    IDXGISwapChain* dummySwapChain = CreateDummySwapChain();
    if (!dummySwapChain) {
        Log("[-] Failed to create dummy swap chain");
        return false;
    }

    void** swapChainVtable = *(void***)dummySwapChain;

    g_originalPresent = (Present_t)swapChainVtable[8];
    g_originalResizeBuffers = (ResizeBuffers_t)swapChainVtable[13];

    if (!g_originalPresent || !g_originalResizeBuffers) {
        Log("[-] Failed to get function pointers");
        dummySwapChain->Release();
        return false;
    }

    Log("[*] Present: 0x%p", (void*)g_originalPresent);
    Log("[*] ResizeBuffers: 0x%p", (void*)g_originalResizeBuffers);

    if (MH_Initialize() != MH_OK) {
        Log("[-] MH_Initialize failed");
        dummySwapChain->Release();
        return false;
    }

    if (MH_CreateHook((void*)g_originalPresent, &HookedPresent, nullptr) != MH_OK ||
        MH_CreateHook((void*)g_originalResizeBuffers, &HookedResizeBuffers, nullptr) != MH_OK) {
        Log("[-] MH_CreateHook failed");
        MH_Uninitialize();
        dummySwapChain->Release();
        return false;
    }

    if (MH_EnableHook((void*)g_originalPresent) != MH_OK ||
        MH_EnableHook((void*)g_originalResizeBuffers) != MH_OK) {
        Log("[-] MH_EnableHook failed");
        MH_RemoveHook((void*)g_originalPresent);
        MH_RemoveHook((void*)g_originalResizeBuffers);
        MH_Uninitialize();
        dummySwapChain->Release();
        return false;
    }

    dummySwapChain->Release();
    g_hookInitialized = true;

    Log("[+] Hooks installed!");
    return true;
}

// ==================== DLL Main ====================

DWORD WINAPI InitThread(LPVOID) {
    Sleep(2000);

#if ENABLE_CONSOLE
    AllocConsole();
    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
#endif

    Log("");
    Log("╔════════════════════════════════════════════════════╗");
    Log("║   CS2 TRIGGERBOT - STABLE FOUNDATION v2.3         ║");
    Log("║   Safe D3D11 Hooks (Present + ResizeBuffers)      ║");
    Log("╚════════════════════════════════════════════════════╝");
    Log("");

    if (!InstallHooks()) {
        Log("[-] Failed to install hooks!");
        return 0;
    }

    Log("[+] Triggerbot ACTIVE");
    Log("[*] Press DEL to unload");
    Log("");

    while (g_hookInitialized && !g_unloading) {
        if (GetAsyncKeyState(VK_DELETE) & 0x8000) {
            Sleep(200);
            if (GetAsyncKeyState(VK_DELETE) & 0x8000) {
                g_unloading = true;

                Log("[*] Unloading...");
                Log("[*] Frames: %d", g_framesProcessed);

                std::lock_guard<std::mutex> lock(g_mutex);

                MH_DisableHook((void*)g_originalPresent);
                MH_DisableHook((void*)g_originalResizeBuffers);
                MH_RemoveHook((void*)g_originalPresent);
                MH_RemoveHook((void*)g_originalResizeBuffers);
                MH_Uninitialize();

                if (g_renderTargetView) {
                    g_renderTargetView->Release();
                    g_renderTargetView = nullptr;
                }
                if (g_context) {
                    g_context->Release();
                    g_context = nullptr;
                }
                if (g_device) {
                    g_device->Release();
                    g_device = nullptr;
                }

                g_hookInitialized = false;

                Log("[+] Unloaded!");
                Sleep(1000);
                break;
            }
        }
        Sleep(100);
    }

    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
    }
    else if (reason == DLL_PROCESS_DETACH) {
        g_unloading = true;
        Sleep(500);
    }
    return TRUE;
}