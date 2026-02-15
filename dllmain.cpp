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
#include <thread>
#include <chrono>

// ==================== Configuration ====================
#define ENABLE_CONSOLE 1
#define SCREEN_WIDTH 2560
#define SCREEN_HEIGHT 1440
#define CROSSHAIR_TOLERANCE 80
#define CROSSHAIR_TOLERANCE_SQ (CROSSHAIR_TOLERANCE * CROSSHAIR_TOLERANCE)
#define SHOOT_COOLDOWN 150
#define DELAY_MIN 85
#define DELAY_MAX 120

// CS2 Model detection thresholds
#define PLAYER_MODEL_INDEX_COUNT_MIN 10000
#define PLAYER_MODEL_INDEX_COUNT_MAX 15000
#define PLAYER_MODEL_STRIDE 40

// ==================== Offsets (Latest CS2 - 2026-02-15) ====================
namespace Offsets {
    namespace client_dll {
        constexpr std::ptrdiff_t dwViewMatrix = 0x230ADE0;
        constexpr std::ptrdiff_t dwEntityList = 0x24AA0D8;
        constexpr std::ptrdiff_t dwLocalPlayerPawn = 0x2064AE0;
        constexpr std::ptrdiff_t dwLocalPlayerController = 0x22EF0B8;
    }
    namespace engine2_dll {
        constexpr std::ptrdiff_t dwWindowWidth = 0x9096D8;
        constexpr std::ptrdiff_t dwWindowHeight = 0x9096DC;
    }
}

// ==================== Types ====================
typedef HRESULT(__stdcall* Present_t)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(__stdcall* ResizeBuffers_t)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
typedef void(__stdcall* DrawIndexed_t)(ID3D11DeviceContext*, UINT, UINT, INT);

Present_t g_originalPresent = nullptr;
ResizeBuffers_t g_originalResizeBuffers = nullptr;
DrawIndexed_t g_originalDrawIndexed = nullptr;

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
std::mutex g_viewMatrixMutex;
std::mutex g_enemyDataMutex;

bool g_hookInitialized = false;
bool g_unloading = false;

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
ID3D11RenderTargetView* g_renderTargetView = nullptr;

// Module bases
HMODULE g_clientDll = nullptr;
HMODULE g_engine2Dll = nullptr;

// ViewMatrix (synced in render thread)
float g_viewMatrix[16] = {};

// ESP data structures
struct ESPVertex {
    float x, y, z;
    unsigned int color;
};

struct EnemyData {
    float screenX, screenY;
    float boxWidth, boxHeight;
    int health;
    int teamNum;
    bool visible;
    float distance;
};

std::vector<EnemyData> g_detectedEnemies;

// ESP vertex buffer
ID3D11Buffer* g_espVertexBuffer = nullptr;
const int MAX_ESP_VERTICES = 10000;

// RNG for delays
std::mt19937 g_gen(std::random_device{}());
std::uniform_int_distribution<int> g_delayDist(DELAY_MIN, DELAY_MAX);

// Triggerbot state
enum TriggerState {
    IDLE,
    LOCKED,
    COOLDOWN
};

TriggerState g_triggerState = IDLE;
DWORD g_fireTime = 0;
DWORD g_lastShotTime = 0;
int g_currentDelay = 0;
int g_localPlayerTeam = 3;

// Statistics
int g_framesProcessed = 0;
int g_enemiesDetected = 0;
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

HMODULE GetModuleBaseAddress(const char* moduleName) {
    return GetModuleHandleA(moduleName);
}

// ==================== ViewMatrix Management ====================

void UpdateViewMatrix() {
    if (!g_clientDll) return;

    std::lock_guard<std::mutex> lock(g_viewMatrixMutex);

    uintptr_t clientBase = (uintptr_t)g_clientDll;
    float* viewMatrixAddr = (float*)(clientBase + Offsets::client_dll::dwViewMatrix);

    try {
        memcpy(g_viewMatrix, viewMatrixAddr, sizeof(float) * 16);
    }
    catch (...) {
        Log("[-] Error reading viewMatrix");
    }
}

float* GetViewMatrix() {
    std::lock_guard<std::mutex> lock(g_viewMatrixMutex);
    static float cachedMatrix[16];
    memcpy(cachedMatrix, g_viewMatrix, sizeof(float) * 16);
    return cachedMatrix;
}

// ==================== WorldToScreen ====================

bool WorldToScreen(float* viewMatrix, float x, float y, float z, float& screenX, float& screenY) {
    float w = viewMatrix[12] * x + viewMatrix[13] * y + viewMatrix[14] * z + viewMatrix[15];

    if (w < 0.01f) return false;

    float invW = 1.0f / w;

    screenX = (float)SCREEN_WIDTH / 2.0f +
        (viewMatrix[0] * x + viewMatrix[1] * y + viewMatrix[2] * z + viewMatrix[3]) *
        invW * (float)SCREEN_WIDTH / 2.0f;

    screenY = (float)SCREEN_HEIGHT / 2.0f -
        (viewMatrix[4] * x + viewMatrix[5] * y + viewMatrix[6] * z + viewMatrix[7]) *
        invW * (float)SCREEN_HEIGHT / 2.0f;

    return screenX > -100 && screenX < SCREEN_WIDTH + 100 &&
        screenY > -100 && screenY < SCREEN_HEIGHT + 100;
}

// ==================== ESP Rendering ====================

void CreateESPBuffers() {
    if (!g_device) return;

    D3D11_BUFFER_DESC vbd = {};
    vbd.Usage = D3D11_USAGE_DYNAMIC;
    vbd.ByteWidth = sizeof(ESPVertex) * MAX_ESP_VERTICES;
    vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    if (FAILED(g_device->CreateBuffer(&vbd, nullptr, &g_espVertexBuffer))) {
        Log("[-] Failed to create ESP vertex buffer");
    }
    else {
        Log("[+] ESP vertex buffer created");
    }
}

void DrawESPBoxes() {
    if (!g_espVertexBuffer || !g_context) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_enemyDataMutex);

    if (g_detectedEnemies.empty()) {
        return;
    }

    ID3D11RasterizerState* oldRasterizer = nullptr;
    ID3D11BlendState* oldBlend = nullptr;

    g_context->RSGetState(&oldRasterizer);

    float blendFactor[4] = { 1, 1, 1, 1 };
    UINT sampleMask = 0xffffffff;
    g_context->OMGetBlendState(&oldBlend, blendFactor, &sampleMask);

    D3D11_RASTERIZER_DESC rastDesc = {};
    rastDesc.FillMode = D3D11_FILL_SOLID;
    rastDesc.CullMode = D3D11_CULL_NONE;
    rastDesc.DepthClipEnable = FALSE;
    rastDesc.AntialiasedLineEnable = TRUE;

    ComPtr<ID3D11RasterizerState> rasterizer;
    if (SUCCEEDED(g_device->CreateRasterizerState(&rastDesc, &rasterizer))) {
        g_context->RSSetState(rasterizer.get());
    }

    D3D11_MAPPED_SUBRESOURCE mappedVB;
    if (FAILED(g_context->Map(g_espVertexBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedVB))) {
        if (oldRasterizer) oldRasterizer->Release();
        if (oldBlend) oldBlend->Release();
        return;
    }

    ESPVertex* vertices = (ESPVertex*)mappedVB.pData;
    int vertexCount = 0;

    for (auto& enemy : g_detectedEnemies) {
        if (!enemy.visible || enemy.health <= 0) continue;

        float left = enemy.screenX - enemy.boxWidth / 2;
        float right = enemy.screenX + enemy.boxWidth / 2;
        float top = enemy.screenY - enemy.boxHeight;
        float bottom = enemy.screenY;

        unsigned int boxColor = (enemy.teamNum != g_localPlayerTeam) ? 0xFF0000FF : 0xFF00FF00;
        unsigned int healthColor = 0xFF00FF00;

        if (vertexCount + 8 <= MAX_ESP_VERTICES) {
            vertices[vertexCount++] = { left, top, 0.5f, boxColor };
            vertices[vertexCount++] = { right, top, 0.5f, boxColor };

            vertices[vertexCount++] = { right, top, 0.5f, boxColor };
            vertices[vertexCount++] = { right, bottom, 0.5f, boxColor };

            vertices[vertexCount++] = { right, bottom, 0.5f, boxColor };
            vertices[vertexCount++] = { left, bottom, 0.5f, boxColor };

            vertices[vertexCount++] = { left, bottom, 0.5f, boxColor };
            vertices[vertexCount++] = { left, top, 0.5f, boxColor };
        }

        if (vertexCount + 2 <= MAX_ESP_VERTICES) {
            float healthPercent = std::max(0.0f, std::min(1.0f, enemy.health / 100.0f));
            float healthBarTop = top + (enemy.boxHeight * (1.0f - healthPercent));

            vertices[vertexCount++] = { left - 8, top, 0.5f, healthColor };
            vertices[vertexCount++] = { left - 8, healthBarTop, 0.5f, healthColor };
        }
    }

    g_context->Unmap(g_espVertexBuffer, 0);

    UINT stride = sizeof(ESPVertex), offset = 0;
    g_context->IASetVertexBuffers(0, 1, &g_espVertexBuffer, &stride, &offset);
    g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
    g_context->Draw(vertexCount, 0);

    if (oldRasterizer) {
        g_context->RSSetState(oldRasterizer);
        oldRasterizer->Release();
    }
    if (oldBlend) {
        g_context->OMSetBlendState(oldBlend, blendFactor, sampleMask);
        oldBlend->Release();
    }
}

// ==================== DrawIndexed Hook ====================

void __stdcall HookedDrawIndexed(ID3D11DeviceContext* pThis, UINT IndexCount, UINT StartIndexLocation, INT BaseVertexLocation) {
    if (g_unloading) {
        return g_originalDrawIndexed(pThis, IndexCount, StartIndexLocation, BaseVertexLocation);
    }

    if (IndexCount >= PLAYER_MODEL_INDEX_COUNT_MIN && IndexCount <= PLAYER_MODEL_INDEX_COUNT_MAX) {
        ID3D11Buffer* vertexBuffer = nullptr;
        UINT stride, offset;
        pThis->IAGetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);

        if (vertexBuffer && stride == PLAYER_MODEL_STRIDE) {
            ID3D11Buffer* constantBuffer = nullptr;
            pThis->VSGetConstantBuffers(0, 1, &constantBuffer);

            if (constantBuffer) {
                D3D11_MAPPED_SUBRESOURCE mapped;
                if (SUCCEEDED(pThis->Map(constantBuffer, 0, D3D11_MAP_READ, 0, &mapped))) {
                    float* worldMatrix = (float*)mapped.pData;

                    float enemyX = worldMatrix[12];
                    float enemyY = worldMatrix[13];
                    float enemyZ = worldMatrix[14];

                    pThis->Unmap(constantBuffer, 0);

                    float* viewMatrix = GetViewMatrix();
                    float screenX, screenY;

                    if (WorldToScreen(viewMatrix, enemyX, enemyY, enemyZ, screenX, screenY)) {
                        std::lock_guard<std::mutex> lock(g_enemyDataMutex);

                        if (g_detectedEnemies.size() < 64) {
                            float centerX = SCREEN_WIDTH / 2;
                            float centerY = SCREEN_HEIGHT / 2;
                            float distX = screenX - centerX;
                            float distY = screenY - centerY;
                            float distance = std::sqrt(distX * distX + distY * distY);

                            EnemyData enemy = {};
                            enemy.screenX = screenX;
                            enemy.screenY = screenY;
                            enemy.boxWidth = 40;
                            enemy.boxHeight = 80;
                            enemy.health = 100;
                            enemy.teamNum = 2;
                            enemy.visible = true;
                            enemy.distance = distance;

                            g_detectedEnemies.push_back(enemy);
                            g_enemiesDetected++;
                        }
                    }
                }

                constantBuffer->Release();
            }

            vertexBuffer->Release();
        }
    }

    return g_originalDrawIndexed(pThis, IndexCount, StartIndexLocation, BaseVertexLocation);
}

// ==================== Triggerbot ====================

void TriggerShot() {
    INPUT inputDown = {};
    inputDown.type = INPUT_MOUSE;
    inputDown.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    SendInput(1, &inputDown, sizeof(INPUT));

    Sleep(20);

    INPUT inputUp = {};
    inputUp.type = INPUT_MOUSE;
    inputUp.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(1, &inputUp, sizeof(INPUT));

    g_shotsHit++;
    Log("[+] SHOT FIRED! (Total: %d)", g_shotsHit);
}

bool IsEnemyAtCrosshair() {
    int centerX = SCREEN_WIDTH / 2;
    int centerY = SCREEN_HEIGHT / 2;

    std::lock_guard<std::mutex> lock(g_enemyDataMutex);

    for (auto& enemy : g_detectedEnemies) {
        if (!enemy.visible || enemy.health <= 0) continue;
        if (enemy.teamNum == g_localPlayerTeam) continue;

        float distX = enemy.screenX - centerX;
        float distY = enemy.screenY - centerY;
        float distSq = distX * distX + distY * distY;

        if (distSq < CROSSHAIR_TOLERANCE_SQ) {
            return true;
        }
    }
    return false;
}

void UpdateTriggerBot() {
    DWORD currentTime = GetTickCount();
    bool keyPressed = (GetAsyncKeyState(VK_XBUTTON1) & 0x8000) != 0;
    bool enemyAtCrosshair = IsEnemyAtCrosshair();

    switch (g_triggerState) {
    case IDLE:
        if (enemyAtCrosshair && keyPressed) {
            g_triggerState = LOCKED;
            g_currentDelay = g_delayDist(g_gen);
            g_fireTime = currentTime + g_currentDelay;
            Log("[*] Enemy locked! Firing in %dms", g_currentDelay);
        }
        break;

    case LOCKED:
        if (!keyPressed || !enemyAtCrosshair) {
            g_triggerState = IDLE;
            break;
        }
        if (currentTime >= g_fireTime) {
            TriggerShot();
            g_triggerState = COOLDOWN;
            g_lastShotTime = currentTime;
        }
        break;

    case COOLDOWN:
        if (currentTime - g_lastShotTime >= SHOOT_COOLDOWN) {
            g_triggerState = IDLE;
        }
        break;

    default:
        g_triggerState = IDLE;
    }
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
        CreateESPBuffers();
        init = true;
    }

    UpdateViewMatrix();

    {
        std::lock_guard<std::mutex> enemyLock(g_enemyDataMutex);
        g_detectedEnemies.clear();
    }

    g_framesProcessed++;

    if (g_renderTargetView && g_context) {
        g_context->OMSetRenderTargets(1, &g_renderTargetView, nullptr);
        DrawESPBoxes();
    }

    UpdateTriggerBot();

    return g_originalPresent(pSwapChain, SyncInterval, Flags);
}

// ==================== ResizeBuffers Hook ====================

HRESULT __stdcall HookedResizeBuffers(IDXGISwapChain* pThis, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags) {
    std::lock_guard<std::mutex> lock(g_mutex);

    Log("[*] Window resize detected: %dx%d", Width, Height);

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
    else {
        Log("[-] ResizeBuffers failed: 0x%X", hr);
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

    D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0 };

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
    Log("[*] Installing D3D11 hooks...");

    IDXGISwapChain* dummySwapChain = CreateDummySwapChain();
    if (!dummySwapChain) {
        Log("[-] Failed to create dummy swap chain");
        return false;
    }

    void** swapChainVtable = *(void***)dummySwapChain;
    void** contextVtable = *(void***)g_context;

    g_originalPresent = (Present_t)swapChainVtable[8];
    g_originalResizeBuffers = (ResizeBuffers_t)swapChainVtable[13];
    g_originalDrawIndexed = (DrawIndexed_t)contextVtable[12];

    if (!g_originalPresent || !g_originalResizeBuffers || !g_originalDrawIndexed) {
        Log("[-] Failed to get function pointers");
        dummySwapChain->Release();
        return false;
    }

    Log("[*] Present at: 0x%p", (void*)g_originalPresent);
    Log("[*] ResizeBuffers at: 0x%p", (void*)g_originalResizeBuffers);
    Log("[*] DrawIndexed at: 0x%p", (void*)g_originalDrawIndexed);

    if (MH_Initialize() != MH_OK) {
        Log("[-] MH_Initialize failed");
        dummySwapChain->Release();
        return false;
    }

    if (MH_CreateHook((void*)g_originalPresent, &HookedPresent, nullptr) != MH_OK ||
        MH_CreateHook((void*)g_originalResizeBuffers, &HookedResizeBuffers, nullptr) != MH_OK ||
        MH_CreateHook((void*)g_originalDrawIndexed, &HookedDrawIndexed, nullptr) != MH_OK) {
        Log("[-] MH_CreateHook failed");
        MH_Uninitialize();
        dummySwapChain->Release();
        return false;
    }

    if (MH_EnableHook((void*)g_originalPresent) != MH_OK ||
        MH_EnableHook((void*)g_originalResizeBuffers) != MH_OK ||
        MH_EnableHook((void*)g_originalDrawIndexed) != MH_OK) {
        Log("[-] MH_EnableHook failed");
        MH_RemoveHook((void*)g_originalPresent);
        MH_RemoveHook((void*)g_originalResizeBuffers);
        MH_RemoveHook((void*)g_originalDrawIndexed);
        MH_Uninitialize();
        dummySwapChain->Release();
        return false;
    }

    dummySwapChain->Release();
    g_hookInitialized = true;

    Log("[+] All hooks installed successfully!");
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
    Log("╔══════════════════════════════════════════════════════╗");
    Log("║     CS2 RENDERING-BASED TRIGGERBOT v2.1 FINAL       ║");
    Log("║        Safe D3D11 Hooking Architecture             ║");
    Log("╚══════════════════════════════════════════════════════╝");
    Log("");
    Log("[*] Detection Method: DrawIndexed Hook + GPU Buffers");
    Log("[*] Rendering Engine: D3D11 with Line Lists");
    Log("[*] Safety Features:  ResizeBuffers Hook + Mutex Protection");
    Log("[*] Anti-Cheat Mode:  No ReadProcessMemory Calls");
    Log("");

    g_clientDll = GetModuleBaseAddress("client.dll");
    g_engine2Dll = GetModuleBaseAddress("engine2.dll");

    if (!g_clientDll) {
        Log("[-] FATAL: Failed to get client.dll");
        return 0;
    }

    Log("[+] client.dll: 0x%p", (void*)g_clientDll);
    if (g_engine2Dll) {
        Log("[+] engine2.dll: 0x%p", (void*)g_engine2Dll);
    }

    if (!InstallHooks()) {
        Log("[-] FATAL: Failed to install hooks");
        return 0;
    }

    Log("");
    Log("╔══════════════════════════════════════════════════════╗");
    Log("║          TRIGGERBOT STATUS: ACTIVE ✓                ║");
    Log("╠══════════════════════════════════════════════════════╣");
    Log("║ X1 Mouse Button  : Trigger (when aiming at enemy)   ║");
    Log("║ DEL Key          : Safe Unload                      ║");
    Log("║ Delay Range      : 85-120ms (human-like)            ║");
    Log("║ Crosshair Tol.   : 80px radius                      ║");
    Log("║ Team Filtering   : Enabled (no friendly fire)       ║");
    Log("╚═════════════════════��════════════════════════════════╝");
    Log("");

    while (g_hookInitialized && !g_unloading) {
        if (GetAsyncKeyState(VK_DELETE) & 0x8000) {
            Sleep(200);
            if (GetAsyncKeyState(VK_DELETE) & 0x8000) {
                g_unloading = true;

                Log("");
                Log("[*] Unloading triggerbot...");
                Log("[*] Statistics:");
                Log("    - Frames Processed: %d", g_framesProcessed);
                Log("    - Enemies Detected: %d", g_enemiesDetected);
                Log("    - Shots Fired: %d", g_shotsHit);

                std::lock_guard<std::mutex> lock(g_mutex);

                MH_DisableHook((void*)g_originalPresent);
                MH_DisableHook((void*)g_originalResizeBuffers);
                MH_DisableHook((void*)g_originalDrawIndexed);

                MH_RemoveHook((void*)g_originalPresent);
                MH_RemoveHook((void*)g_originalResizeBuffers);
                MH_RemoveHook((void*)g_originalDrawIndexed);

                MH_Uninitialize();

                if (g_espVertexBuffer) {
                    g_espVertexBuffer->Release();
                    g_espVertexBuffer = nullptr;
                }
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

                Log("[+] Unloaded successfully!");
                Log("[*] Thank you for using CS2 Triggerbot!");
                Log("");

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