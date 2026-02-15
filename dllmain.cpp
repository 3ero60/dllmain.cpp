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
#define SCREEN_WIDTH 2560
#define SCREEN_HEIGHT 1440
#define CROSSHAIR_TOLERANCE 80
#define CROSSHAIR_TOLERANCE_SQ (CROSSHAIR_TOLERANCE * CROSSHAIR_TOLERANCE)
#define SHOOT_COOLDOWN 150
#define DELAY_MIN 85
#define DELAY_MAX 120

// CS2 Model detection thresholds (IndexCount for player models)
#define PLAYER_MODEL_INDEX_COUNT_MIN 10000
#define PLAYER_MODEL_INDEX_COUNT_MAX 15000
#define PLAYER_MODEL_STRIDE 40  // Typical stride for player vertex data

// ==================== Offsets (Latest CS2) ====================
namespace Offsets {
    namespace client_dll {
        constexpr std::ptrdiff_t dwViewMatrix = 0x230ADE0;
        constexpr std::ptrdiff_t dwEntityList = 0x24AA0D8;
        constexpr std::ptrdiff_t dwLocalPlayerPawn = 0x2064AE0;
        constexpr std::ptrdiff_t dwLocalPlayerController = 0x22EF0B8;
    }
}

// ==================== Types ====================
typedef HRESULT(__stdcall* Present_t)(IDXGISwapChain*, UINT, UINT);
typedef void(__stdcall* DrawIndexed_t)(ID3D11DeviceContext*, UINT, UINT, INT);

Present_t g_originalPresent = nullptr;
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
bool g_hookInitialized = false;

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
ID3D11RenderTargetView* g_renderTargetView = nullptr;
ID3D11RasterizerState* g_savedRasterizerState = nullptr;
ID3D11BlendState* g_savedBlendState = nullptr;

// Module bases
HMODULE g_clientDll = nullptr;

// ViewMatrix (synced in render thread)
float g_viewMatrix[16] = {};
std::mutex g_viewMatrixMutex;

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
    int drawCallId;  // To track which draw call rendered this
};

std::vector<EnemyData> g_detectedEnemies;
std::mutex g_enemyDataMutex;

// ESP vertex buffer and index buffer
ID3D11Buffer* g_espVertexBuffer = nullptr;
ID3D11Buffer* g_espIndexBuffer = nullptr;
const int MAX_ESP_VERTICES = 5000;

// RNG for delays
std::random_device g_rd;
std::mt19937 g_gen(g_rd());
std::uniform_int_distribution<int> g_delayDist(DELAY_MIN, DELAY_MAX);

// Triggerbot state
enum TriggerState {
    IDLE,
    LOCKED,
    FIRING,
    COOLDOWN
};

TriggerState g_triggerState = IDLE;
DWORD g_fireTime = 0;
DWORD g_lastShotTime = 0;
int g_currentDelay = 0;
int g_localPlayerTeam = 3;  // 2=Terrorist, 3=Counter-Terrorist

// DrawIndexed tracking
int g_currentDrawCallId = 0;

// ==================== Utility Functions ====================

void Log(const char* fmt, ...) {
#if ENABLE_CONSOLE
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
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

    // Safe memcpy with mutex protection
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

    // Frustum culling
    return screenX > -100 && screenX < SCREEN_WIDTH + 100 &&
        screenY > -100 && screenY < SCREEN_HEIGHT + 100;
}

// ==================== ESP Rendering with Index Buffer ====================

void CreateESPBuffers() {
    // Create vertex buffer
    D3D11_BUFFER_DESC vbd = {};
    vbd.Usage = D3D11_USAGE_DYNAMIC;
    vbd.ByteWidth = sizeof(ESPVertex) * MAX_ESP_VERTICES;
    vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    if (FAILED(g_device->CreateBuffer(&vbd, nullptr, &g_espVertexBuffer))) {
        Log("[-] Failed to create ESP vertex buffer");
        return;
    }

    // Create index buffer
    std::vector<unsigned short> indices;

    // Generate indices for line list (12 lines per box = 24 indices)
    // For now, pre-allocate space
    indices.reserve(MAX_ESP_VERTICES);

    D3D11_BUFFER_DESC ibd = {};
    ibd.Usage = D3D11_USAGE_DYNAMIC;
    ibd.ByteWidth = sizeof(unsigned short) * MAX_ESP_VERTICES;
    ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    ibd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    if (FAILED(g_device->CreateBuffer(&ibd, nullptr, &g_espIndexBuffer))) {
        Log("[-] Failed to create ESP index buffer");
        return;
    }

    Log("[+] ESP buffers created successfully");
}

void DrawESPBoxes() {
    if (!g_espVertexBuffer || !g_espIndexBuffer || !g_context) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_enemyDataMutex);

    if (g_detectedEnemies.empty()) {
        return;
    }

    // Save render state
    ID3D11RasterizerState* oldRasterizer = nullptr;
    g_context->RSGetState(&oldRasterizer);

    float blendFactor[4] = { 1, 1, 1, 1 };
    UINT sampleMask = 0xffffffff;
    ID3D11BlendState* oldBlend = nullptr;
    g_context->OMGetBlendState(&oldBlend, blendFactor, &sampleMask);

    // Setup rendering
    D3D11_RASTERIZER_DESC rastDesc = {};
    rastDesc.FillMode = D3D11_FILL_SOLID;
    rastDesc.CullMode = D3D11_CULL_NONE;
    rastDesc.DepthClipEnable = FALSE;

    ComPtr<ID3D11RasterizerState> rasterizer;
    if (SUCCEEDED(g_device->CreateRasterizerState(&rastDesc, &rasterizer))) {
        g_context->RSSetState(rasterizer.get());
    }

    // Map vertex buffer
    D3D11_MAPPED_SUBRESOURCE mappedVB;
    if (FAILED(g_context->Map(g_espVertexBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedVB))) {
        goto cleanup;
    }

    ESPVertex* vertices = (ESPVertex*)mappedVB.pData;
    int vertexCount = 0;

    // Generate box vertices for each enemy
    for (auto& enemy : g_detectedEnemies) {
        if (!enemy.visible || enemy.health <= 0) continue;

        float left = enemy.screenX - enemy.boxWidth / 2;
        float right = enemy.screenX + enemy.boxWidth / 2;
        float top = enemy.screenY - enemy.boxHeight;
        float bottom = enemy.screenY;

        // Color: Red for enemy, Green for friendly
        unsigned int color = (enemy.teamNum != g_localPlayerTeam) ? 0xFF0000FF : 0xFF00FF00;

        // Box outline vertices (8 vertices for 4 lines when using line list)
        if (vertexCount + 8 <= MAX_ESP_VERTICES) {
            // Top-left to top-right
            vertices[vertexCount++] = { left, top, 0.5f, color };
            vertices[vertexCount++] = { right, top, 0.5f, color };

            // Top-right to bottom-right
            vertices[vertexCount++] = { right, top, 0.5f, color };
            vertices[vertexCount++] = { right, bottom, 0.5f, color };

            // Bottom-right to bottom-left
            vertices[vertexCount++] = { right, bottom, 0.5f, color };
            vertices[vertexCount++] = { left, bottom, 0.5f, color };

            // Bottom-left to top-left
            vertices[vertexCount++] = { left, bottom, 0.5f, color };
            vertices[vertexCount++] = { left, top, 0.5f, color };
        }

        // Health bar (vertical line on left)
        if (vertexCount + 2 <= MAX_ESP_VERTICES) {
            float healthPercent = enemy.health / 100.0f;
            float healthBarTop = top + (enemy.boxHeight * healthPercent);

            vertices[vertexCount++] = { left - 5, top, 0.5f, 0xFF00FF00 };
            vertices[vertexCount++] = { left - 5, healthBarTop, 0.5f, 0xFF00FF00 };
        }
    }

    g_context->Unmap(g_espVertexBuffer, 0);

    // Setup and draw
    UINT stride = sizeof(ESPVertex), offset = 0;
    g_context->IASetVertexBuffers(0, 1, &g_espVertexBuffer, &stride, &offset);
    g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
    g_context->Draw(vertexCount, 0);

cleanup:
    // Restore render state
    if (oldRasterizer) {
        g_context->RSSetState(oldRasterizer);
        oldRasterizer->Release();
    }
    if (oldBlend) {
        g_context->OMSetBlendState(oldBlend, blendFactor, sampleMask);
        oldBlend->Release();
    }
}

// ==================== DrawIndexed Hook for Enemy Detection ====================

void __stdcall HookedDrawIndexed(ID3D11DeviceContext* pThis, UINT IndexCount, UINT StartIndexLocation, INT BaseVertexLocation) {
    // Track draw call
    int drawCallId = g_currentDrawCallId++;

    // Detect player models by IndexCount and stride
    if (IndexCount >= PLAYER_MODEL_INDEX_COUNT_MIN && IndexCount <= PLAYER_MODEL_INDEX_COUNT_MAX) {

        // Get input layout to determine stride
        ComPtr<ID3D11InputLayout> inputLayout;
        pThis->IAGetInputLayout(&inputLayout);

        if (inputLayout) {
            // Extract vertex buffer info
            ID3D11Buffer* vertexBuffer = nullptr;
            UINT stride, offset;
            pThis->IAGetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);

            if (vertexBuffer && stride == PLAYER_MODEL_STRIDE) {
                // This appears to be a player model

                // Get world transformation matrix (from constant buffer)
                ID3D11Buffer* constantBuffer = nullptr;
                pThis->VSGetConstantBuffers(0, 1, &constantBuffer);

                if (constantBuffer) {
                    // Map and read world matrix from constant buffer slot 0
                    D3D11_MAPPED_SUBRESOURCE mapped;
                    if (SUCCEEDED(pThis->Map(constantBuffer, 0, D3D11_MAP_READ, 0, &mapped))) {
                        float* worldMatrix = (float*)mapped.pData;

                        // Extract position from world matrix (last row)
                        float enemyX = worldMatrix[12];
                        float enemyY = worldMatrix[13];
                        float enemyZ = worldMatrix[14];

                        pThis->Unmap(constantBuffer, 0);

                        // Convert to screen space
                        float* viewMatrix = GetViewMatrix();
                        float screenX, screenY;

                        if (WorldToScreen(viewMatrix, enemyX, enemyY, enemyZ, screenX, screenY)) {
                            // Add to detected enemies
                            std::lock_guard<std::mutex> lock(g_enemyDataMutex);

                            if (g_detectedEnemies.size() < 20) {  // Limit to 20 enemies
                                EnemyData enemy = {};
                                enemy.screenX = screenX;
                                enemy.screenY = screenY;
                                enemy.boxWidth = 40;
                                enemy.boxHeight = 80;
                                enemy.health = 100;  // Simplified
                                enemy.teamNum = 2;   // Default to enemy team
                                enemy.visible = true;
                                enemy.drawCallId = drawCallId;

                                g_detectedEnemies.push_back(enemy);
                            }
                        }
                    }

                    constantBuffer->Release();
                }
            }

            if (vertexBuffer) vertexBuffer->Release();
        }
    }

    // Call original
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

    Log("[+] SHOT FIRED!");
}

bool IsEnemyAtCrosshair() {
    int centerX = SCREEN_WIDTH / 2;
    int centerY = SCREEN_HEIGHT / 2;

    std::lock_guard<std::mutex> lock(g_enemyDataMutex);

    for (auto& enemy : g_detectedEnemies) {
        if (!enemy.visible || enemy.health <= 0) continue;
        if (enemy.teamNum == g_localPlayerTeam) continue;  // Skip friendly team

        // Optimized: distance squared instead of sqrt
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

    // Initialize on first call
    static bool init = false;
    if (!init) {
        ComPtr<ID3D11Texture2D> backBuffer;
        if (SUCCEEDED(pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer))) {
            if (g_device) {
                g_device->CreateRenderTargetView(backBuffer.get(), nullptr, &g_renderTargetView);
            }
        }
        CreateESPBuffers();
        init = true;
    }

    // Update viewMatrix from render thread
    UpdateViewMatrix();

    // Clear enemy list for fresh detection
    {
        std::lock_guard<std::mutex> enemyLock(g_enemyDataMutex);
        g_detectedEnemies.clear();
    }

    // Set render target
    if (g_renderTargetView && g_context) {
        g_context->OMSetRenderTargets(1, &g_renderTargetView, nullptr);

        // Draw ESP
        DrawESPBoxes();
    }

    // Update triggerbot logic
    UpdateTriggerBot();

    return g_originalPresent(pSwapChain, SyncInterval, Flags);
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
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
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

    if (FAILED(hr)) return nullptr;
    return swapChain;
}

bool InstallHooks() {
    Log("[*] Installing D3D11 hooks...");

    IDXGISwapChain* dummySwapChain = CreateDummySwapChain();
    if (!dummySwapChain) {
        Log("[-] Failed to create dummy swap chain");
        return false;
    }

    // Get Present address (index 8)
    void** swapChainVtable = *(void***)dummySwapChain;
    g_originalPresent = (Present_t)swapChainVtable[8];

    // Get DrawIndexed address (index 12 in ID3D11DeviceContext)
    if (g_context) {
        void** contextVtable = *(void***)g_context;
        g_originalDrawIndexed = (DrawIndexed_t)contextVtable[12];
    }

    if (!g_originalPresent || !g_originalDrawIndexed) {
        Log("[-] Failed to get function pointers");
        dummySwapChain->Release();
        return false;
    }

    Log("[*] Present at: 0x%p", g_originalPresent);
    Log("[*] DrawIndexed at: 0x%p", g_originalDrawIndexed);

    if (MH_Initialize() != MH_OK) {
        Log("[-] MH_Initialize failed");
        dummySwapChain->Release();
        return false;
    }

    // Hook Present
    if (MH_CreateHook((void*)g_originalPresent, &HookedPresent, nullptr) != MH_OK) {
        Log("[-] MH_CreateHook(Present) failed");
        MH_Uninitialize();
        dummySwapChain->Release();
        return false;
    }

    // Hook DrawIndexed
    if (MH_CreateHook((void*)g_originalDrawIndexed, &HookedDrawIndexed, nullptr) != MH_OK) {
        Log("[-] MH_CreateHook(DrawIndexed) failed");
        MH_RemoveHook((void*)g_originalPresent);
        MH_Uninitialize();
        dummySwapChain->Release();
        return false;
    }

    if (MH_EnableHook((void*)g_originalPresent) != MH_OK) {
        Log("[-] MH_EnableHook(Present) failed");
        MH_RemoveHook((void*)g_originalPresent);
        MH_RemoveHook((void*)g_originalDrawIndexed);
        MH_Uninitialize();
        dummySwapChain->Release();
        return false;
    }

    if (MH_EnableHook((void*)g_originalDrawIndexed) != MH_OK) {
        Log("[-] MH_EnableHook(DrawIndexed) failed");
        MH_RemoveHook((void*)g_originalPresent);
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

    Log("[*] CS2 Rendering-Based Triggerbot v2.0");
    Log("[*] Enemy Detection: DrawIndexed Hook");
    Log("[*] Rendering: D3D11 with Index Buffers");

    g_clientDll = GetModuleBaseAddress("client.dll");
    if (!g_clientDll) {
        Log("[-] Failed to get client.dll");
        return 0;
    }

    Log("[+] client.dll: 0x%p", g_clientDll);

    if (!InstallHooks()) {
        Log("[-] Failed to install hooks");
        return 0;
    }

    Log("[+] Triggerbot ACTIVE!");
    Log("[+] Press X1 to trigger | Press DEL to unload");

    while (g_hookInitialized) {
        if (GetAsyncKeyState(VK_DELETE) & 0x8000) {
            Log("[*] Unloading...");
            std::lock_guard<std::mutex> lock(g_mutex);

            MH_DisableHook((void*)g_originalPresent);
            MH_DisableHook((void*)g_originalDrawIndexed);
            MH_RemoveHook((void*)g_originalPresent);
            MH_RemoveHook((void*)g_originalDrawIndexed);
            MH_Uninitialize();

            if (g_espVertexBuffer) g_espVertexBuffer->Release();
            if (g_espIndexBuffer) g_espIndexBuffer->Release();
            if (g_renderTargetView) g_renderTargetView->Release();
            if (g_context) g_context->Release();
            if (g_device) g_device->Release();

            g_hookInitialized = false;
            Log("[+] Unloaded successfully");
            break;
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
    return TRUE;
}