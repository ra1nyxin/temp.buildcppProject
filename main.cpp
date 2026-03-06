//Enable OS-ImGui internal mode - must be defined before including any OS-ImGui headers
#define OSIMGUI_INTERNAL

#include <Windows.h>
#include <vector>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <d3d11.h>
#include <functional>   //for std::function
#include "imgui.h"
#include "OS-ImGui_Struct.h"    //提供DirectXType枚举定义 //Provides DirectXType enum definition
#include "OS-ImGui.h" // Imgui/imgui.h

//偏移量定义(基于补丁12.03) - 感谢Tops的付出与努力，dump出了一份珍贵的地址，为我为数不多的内存省下来一大笔空间！
//Offsets (based on patch 12.03)
#define OFFSET_GWORLD                   0xBE4A800
#define OFFSET_FNAME_POOL                0xBFD4B40
#define OFFSET_FNAME_STATE                0xC1BB5C0
#define OFFSET_FNAME_KEY                  0xC1BB5F8

#define OFFSET_SET_OUTLINE_MODE           0x3F7A9F0
#define OFFSET_SET_OUTLINE_COLORS         0x3639E89
#define OFFSET_K2_DRAW_BOX                0x6D0ED60
#define OFFSET_K2_DRAW_TEXT                0x459E57C

#define OFFSET_ACTOR_ARRAY                0x00A0
#define OFFSET_TEAM_ID                    0x00E8
#define OFFSET_ROOT_COMPONENT              0x0288
#define OFFSET_RELATIVE_LOCATION            0x0170
#define OFFSET_RELATIVE_ROTATION            0x0188
#define OFFSET_PLAYER_STATE                0x0480
#define OFFSET_CAMERA_MANAGER              0x0520
#define OFFSET_CAMERA_PRIVATE              0x17B0
#define OFFSET_CAMERA_POS                  0x0384
#define OFFSET_CAMERA_ROT                  0x0396
#define OFFSET_CAMERA_FOV                  0x03A2
#define OFFSET_MESH_COMPONENT              0x04E8
#define OFFSET_BONE_ARRAY                  0x0730
#define OFFSET_COMPONENT_TO_WORLD          0x02D0
#define OFFSET_HEALTH                      0x0200

#define OFFSET_NAME_POOL_CHUNK              (OFFSET_FNAME_POOL+0x38)

// 骨骼索引枚举
// Bone indices enumeration
enum BoneIndex {
    BONE_PELVIS = 1,
    BONE_CHEST = 3,
    BONE_NECK = 4,
    BONE_HEAD = 5,
    BONE_LEFT_ARM = 7,
    BONE_RIGHT_ARM = 10,
    BONE_LEFT_LEG = 13,
    BONE_RIGHT_LEG = 16
};

// 基础结构体
// Basic structures
struct FVector { float X, Y, Z; };
struct FRotator { float Pitch, Yaw, Roll; };
struct FMatrix { float M[4][4]; };
struct FNameEntryHeader { uint16_t Len; uint16_t Pad; };
struct FNameEntry { FNameEntryHeader Header; char AnsiName[256]; };
struct FColor { uint8_t B, G, R, A; };

// 全局变量
// Global variables
uintptr_t g_BaseAddress = 0;
uintptr_t g_NameXorKey = 0;

// 配置变量
// Configuration variables
bool g_AimbotEnabled = true;
bool g_ESPEnabled = true;
bool g_OutlineEnabled = true;
float g_AimbotFOV = 10.0f;
float g_SmoothX = 2.0f, g_SmoothY = 2.0f;
ImColor g_EnemyColor = ImColor(255, 0, 0);
ImColor g_AllyColor = ImColor(0, 255, 0);
int g_AimbotKey = VK_LBUTTON;

// 目标信息
// Target information
struct Target {
    uintptr_t actor;
    uintptr_t mesh;
    int team;
    FVector headPos;
    FVector rootPos;
    float distance;
    std::string name;
};
std::vector<Target> g_Targets;

// 菜单可见性标志
// Menu visibility flag
bool g_MenuVisible = false;

// 工具函数
// Utility functions
template<typename T> T Read(uintptr_t addr) {
    T buf{};
    ReadProcessMemory(GetCurrentProcess(), (LPCVOID)addr, &buf, sizeof(T), nullptr);
    return buf;
}
uintptr_t ReadPtr(uintptr_t addr) { return Read<uintptr_t>(addr); }
template<typename T> void Write(uintptr_t addr, T val) {
    WriteProcessMemory(GetCurrentProcess(), (LPVOID)addr, &val, sizeof(T), nullptr);
}

// FName解密
// FName decryption
uint64_t DecryptXorKeys(uint32_t key, const uint64_t* state) {
    uint64_t hash = 0x2545F4914F6CDD1Dui64 * (key ^ ((key ^ (key >> 15)) >> 12) ^ (key << 25));
    uint64_t idx = hash % 7;
    uint64_t val = state[idx];
    uint32_t hi = (uint32_t)(hash >> 32);
    uint32_t mod7 = (uint32_t)idx;
    if (mod7 == 0) {
        uint8_t q = (uint8_t)(((int)hi - 1) / 0x3F);
        uint8_t rshift = (uint8_t)hi - 63 * q;
        uint8_t lshift = 63 * q - ((uint8_t)hi - 1) + 63;
        val = ((val >> rshift) | (val << lshift)) - hi;
    }
    else if (mod7 == 1) {
        uint32_t rot = hi + 2 * (uint32_t)idx;
        uint8_t lshift = (uint8_t)(rot % 0x3F) + 1;
        uint8_t rshift = 63 * (uint8_t)(rot / 0x3F) - (uint8_t)hi - 2 * (uint8_t)idx + 63;
        val = ((val << lshift) | (val >> rshift)) + (uint32_t)(hi + idx);
    }
    else if (mod7 == 3) {
        uint32_t rot = hi + 2 * (uint32_t)idx;
        uint8_t rshift = (uint8_t)(rot % 0x3F) + 1;
        uint8_t lshift = 63 * (uint8_t)(rot / 0x3F) - (uint8_t)hi - 2 * (uint8_t)idx + 63;
        val = ~((val >> rshift) | (val << lshift));
    }
    else if (mod7 == 4) {
        val = val ^ (uint32_t)(hi + idx);
    }
    else if (mod7 == 5) {
        val = (val >> 1) ^ ~(uint64_t)(uint32_t)(hi + idx) ^ (((val >> 1) ^ (2 * val)) & 0xAAAAAAAAAAAAAAAAui64);
    }
    return val ^ key;
}

bool InitNameDecryption() {
    uintptr_t nameKeyAddr = ReadPtr(g_BaseAddress + OFFSET_FNAME_KEY);
    if (!nameKeyAddr) return false;
    struct State { uint64_t keys[7]; };
    State state = Read<State>(g_BaseAddress + OFFSET_FNAME_STATE);
    uintptr_t xorAddr = DecryptXorKeys(Read<uint32_t>(nameKeyAddr), (uint64_t*)&state);
    g_NameXorKey = ReadPtr(xorAddr);
    return (g_NameXorKey != 0);
}

std::string GetFName(int key) {
    uint32_t chunkOffset = (uint32_t)(key >> 16);
    uint16_t nameOffset = (uint16_t)key;
    uintptr_t namePoolChunk = ReadPtr(g_BaseAddress + OFFSET_NAME_POOL_CHUNK + ((chunkOffset + 2) * 8));
    uintptr_t entryOffset = namePoolChunk + (uintptr_t)(4 * nameOffset);
    FNameEntry entry = Read<FNameEntry>(entryOffset);
    std::string name(entry.AnsiName, entry.Header.Len);
    if (entry.Header.Len <= 365) {
        for (uint16_t i = 0; i < entry.Header.Len; i++) {
            BYTE b = i & 3;
            name[i] ^= entry.Header.Len ^ *((LPBYTE)&g_NameXorKey + b);
        }
    }
    return name;
}

FVector GetBoneLocation(uintptr_t mesh, int boneIndex) {
    uintptr_t boneArray = ReadPtr(mesh + OFFSET_BONE_ARRAY);
    if (!boneArray) return FVector{ 0,0,0 };
    FMatrix boneMatrix = Read<FMatrix>(boneArray + boneIndex * sizeof(FMatrix));
    FMatrix componentToWorld = Read<FMatrix>(mesh + OFFSET_COMPONENT_TO_WORLD);
    FVector bonePos = { boneMatrix.M[3][0], boneMatrix.M[3][1], boneMatrix.M[3][2] };
    // 简化:直接返回bonePos(实际需要componentToWorld变换)
    // Simplified: directly return bonePos (actual transformation using componentToWorld is needed)
    return bonePos;
}

FRotator CalcAngle(const FVector& localPos, const FVector& targetPos) {
    FVector delta = { targetPos.X - localPos.X, targetPos.Y - localPos.Y, targetPos.Z - localPos.Z };
    float length = sqrtf(delta.X * delta.X + delta.Y * delta.Y + delta.Z * delta.Z);
    FRotator result;
    result.Pitch = -asinf(delta.Z / length) * (180.0f / 3.14159265f);
    result.Yaw = atan2f(delta.Y, delta.X) * (180.0f / 3.14159265f);
    result.Roll = 0;
    if (result.Pitch > 180) result.Pitch -= 360;
    else if (result.Pitch < -180) result.Pitch += 360;
    if (result.Yaw > 180) result.Yaw -= 360;
    else if (result.Yaw < -180) result.Yaw += 360;
    return result;
}

float GetAngleDifference(const FRotator& a, const FRotator& b) {
    float dp = a.Pitch - b.Pitch;
    float dy = a.Yaw - b.Yaw;
    if (dy > 180) dy -= 360;
    else if (dy < -180) dy += 360;
    return sqrtf(dp * dp + dy * dy);
}

FRotator SmoothAim(const FRotator& current, const FRotator& target, float smoothX, float smoothY) {
    FRotator result;
    result.Pitch = current.Pitch + (target.Pitch - current.Pitch) / smoothX;
    result.Yaw = current.Yaw + (target.Yaw - current.Yaw) / smoothY;
    result.Roll = 0;
    return result;
}

// 游戏函数指针类型
// Game function pointer types
typedef void(__fastcall* SetOutlineMode_t)(uintptr_t mesh, bool enable);
typedef void(__fastcall* SetOutlineColors_t)(uintptr_t mesh, FColor color1, FColor color2);
typedef void(__fastcall* K2DrawBox_t)(uintptr_t worldContext, FVector center, FVector extent, FColor color, float thickness, float duration, uint8_t depthPriority);
typedef void(__fastcall* K2DrawText_t)(uintptr_t worldContext, const wchar_t* text, FVector position, FColor color, float scale, bool alignCenter, float duration);

SetOutlineMode_t SetOutlineMode = nullptr;
SetOutlineColors_t SetOutlineColors = nullptr;
K2DrawBox_t K2DrawBox = nullptr;
K2DrawText_t K2DrawText = nullptr;

// 配置文件读写
// Config file I/O
void LoadConfig() {
    std::ifstream f("config.ini");
    if (f.is_open()) {
        f >> g_AimbotEnabled >> g_ESPEnabled >> g_OutlineEnabled
            >> g_AimbotFOV >> g_SmoothX >> g_SmoothY
            >> g_EnemyColor.Value.x >> g_EnemyColor.Value.y >> g_EnemyColor.Value.z
            >> g_AllyColor.Value.x >> g_AllyColor.Value.y >> g_AllyColor.Value.z;
        f.close();
    }
}
void SaveConfig() {
    std::ofstream f("config.ini");
    f << g_AimbotEnabled << " " << g_ESPEnabled << " " << g_OutlineEnabled << "\n"
        << g_AimbotFOV << " " << g_SmoothX << " " << g_SmoothY << "\n"
        << g_EnemyColor.Value.x << " " << g_EnemyColor.Value.y << " " << g_EnemyColor.Value.z << "\n"
        << g_AllyColor.Value.x << " " << g_AllyColor.Value.y << " " << g_AllyColor.Value.z << "\n";
    f.close();
}

// 主循环线程
// Main loop thread
void MainLoop() {
    if (!InitNameDecryption()) {
        MessageBoxA(nullptr, "FName decryption failed", "Error", MB_ICONERROR);
        return;
    }
    // 获取函数地址
    // Get function addresses
    SetOutlineMode = (SetOutlineMode_t)(g_BaseAddress + OFFSET_SET_OUTLINE_MODE);
    SetOutlineColors = (SetOutlineColors_t)(g_BaseAddress + OFFSET_SET_OUTLINE_COLORS);
    K2DrawBox = (K2DrawBox_t)(g_BaseAddress + OFFSET_K2_DRAW_BOX);
    K2DrawText = (K2DrawText_t)(g_BaseAddress + OFFSET_K2_DRAW_TEXT);

    while (true) {
        Sleep(1);
        // 读取游戏数据
        // Read game data
        uintptr_t gWorld = ReadPtr(g_BaseAddress + OFFSET_GWORLD);
        if (!gWorld) continue;
        uintptr_t persistentLevel = ReadPtr(gWorld + 0x38);
        if (!persistentLevel) continue;
        uintptr_t actorArray = ReadPtr(persistentLevel + OFFSET_ACTOR_ARRAY);
        int actorCount = Read<int>(persistentLevel + OFFSET_ACTOR_ARRAY + 8);
        if (!actorArray || actorCount <= 0) continue;

        uintptr_t gameInstance = ReadPtr(gWorld + 0x1D8);
        if (!gameInstance) continue;
        uintptr_t localPlayer = ReadPtr(gameInstance + 0x40);
        if (!localPlayer) continue;
        uintptr_t localPlayerController = ReadPtr(localPlayer + 0x38);
        if (!localPlayerController) continue;
        uintptr_t localPawn = ReadPtr(localPlayerController + 0x510);
        if (!localPawn) continue;

        int localTeam = Read<int>(localPawn + OFFSET_TEAM_ID);

        uintptr_t cameraManager = ReadPtr(localPlayerController + OFFSET_CAMERA_MANAGER);
        if (!cameraManager) continue;
        uintptr_t cameraCache = cameraManager + OFFSET_CAMERA_PRIVATE;
        FVector cameraPos = Read<FVector>(cameraCache + OFFSET_CAMERA_POS);
        FRotator cameraRot = Read<FRotator>(cameraCache + OFFSET_CAMERA_ROT);

        // 更新目标列表
        // Update target list
        g_Targets.clear();
        for (int i = 0; i < actorCount; i++) {
            uintptr_t actor = ReadPtr(actorArray + i * 0x8);
            if (!actor) continue;
            int nameId = Read<int>(actor + 0x18);
            std::string name = GetFName(nameId);
            if (name.find("ValorantCharacter") == std::string::npos) continue;
            int team = Read<int>(actor + OFFSET_TEAM_ID);
            if (team == localTeam) continue;
            int health = Read<int>(actor + OFFSET_HEALTH);
            if (health <= 0 || health > 150) continue;
            uintptr_t mesh = ReadPtr(actor + OFFSET_MESH_COMPONENT);
            if (!mesh) continue;
            FVector headPos = GetBoneLocation(mesh, BONE_HEAD);
            FVector rootPos = GetBoneLocation(mesh, BONE_PELVIS);
            float dist = sqrtf((headPos.X - cameraPos.X) * (headPos.X - cameraPos.X) +
                (headPos.Y - cameraPos.Y) * (headPos.Y - cameraPos.Y) +
                (headPos.Z - cameraPos.Z) * (headPos.Z - cameraPos.Z));
            Target t;
            t.actor = actor;
            t.mesh = mesh;
            t.team = team;
            t.headPos = headPos;
            t.rootPos = rootPos;
            t.distance = dist;
            t.name = name;
            g_Targets.push_back(t);
        }

        // 自瞄
        // Aimbot
        if (g_AimbotEnabled && (GetAsyncKeyState(g_AimbotKey) & 0x8000)) {
            Target* bestTarget = nullptr;
            float bestAngleDiff = g_AimbotFOV;
            for (auto& t : g_Targets) {
                FRotator targetAngle = CalcAngle(cameraPos, t.headPos);
                float angleDiff = GetAngleDifference(cameraRot, targetAngle);
                if (angleDiff < bestAngleDiff) {
                    bestAngleDiff = angleDiff;
                    bestTarget = &t;
                }
            }
            if (bestTarget) {
                FRotator targetAngle = CalcAngle(cameraPos, bestTarget->headPos);
                FRotator controlRot = Read<FRotator>(localPlayerController + 0x288);
                FRotator newRot = SmoothAim(controlRot, targetAngle, g_SmoothX, g_SmoothY);
                Write<FRotator>(localPlayerController + 0x288, newRot);
            }
        }

        // 换肤（轮廓）
        // Outline (skin)
        if (g_OutlineEnabled && SetOutlineMode && SetOutlineColors) {
            for (auto& t : g_Targets) {
                SetOutlineMode(t.mesh, true);
                ImColor col = (t.team == localTeam) ? g_AllyColor : g_EnemyColor;
                FColor fcol = { (uint8_t)(col.Value.z * 255), (uint8_t)(col.Value.y * 255), (uint8_t)(col.Value.x * 255), 255 };
                SetOutlineColors(t.mesh, fcol, fcol);
            }
        }
    }
}

// ImGui回调函数
// ImGui callback function
void ImGuiCallback() {
    if (!g_MenuVisible) return;
    ImGui::SetNextWindowSize(ImVec2(400, 300), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Valorant Cheat", &g_MenuVisible, ImGuiWindowFlags_NoCollapse)) {
        ImGui::Checkbox("Aimbot", &g_AimbotEnabled);
        ImGui::SliderFloat("FOV", &g_AimbotFOV, 1, 30);
        ImGui::SliderFloat("Smooth X", &g_SmoothX, 1, 10);
        ImGui::SliderFloat("Smooth Y", &g_SmoothY, 1, 10);
        ImGui::Checkbox("ESP", &g_ESPEnabled);
        ImGui::Checkbox("Outline", &g_OutlineEnabled);
        ImGui::ColorEdit3("Enemy Color", (float*)&g_EnemyColor);
        ImGui::ColorEdit3("Ally Color", (float*)&g_AllyColor);
        if (ImGui::Button("Save Config")) SaveConfig();
        ImGui::SameLine();
        if (ImGui::Button("Load Config")) LoadConfig();
    }
    ImGui::End();
}

// DLL入口线程
// DLL entry thread
DWORD WINAPI MainThread(LPVOID) {
    // 启动OS-ImGui内部渲染
    // Start OS-ImGui internal rendering
    // 修复：使用 OSImGui::DX11 代替 DirectXType::DX11（因为 DirectXType 是普通枚举，其值位于 OSImGui 命名空间内）
    Gui.Start(GetModuleHandle(nullptr), []() {
        if (GetAsyncKeyState(VK_INSERT) & 1) g_MenuVisible = !g_MenuVisible;
        ImGuiCallback();
        }, OSImGui::DX11);  // 使用DirectX 11 / Using DirectX 11

    // 启动主逻辑线程
    // Start main logic thread
    CreateThread(nullptr, 0, [](LPVOID)->DWORD { MainLoop(); return 0; }, nullptr, 0, nullptr);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_BaseAddress = (uintptr_t)GetModuleHandleA(NULL);
        LoadConfig();
        CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);
    }
    return TRUE;
}