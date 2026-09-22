// speedo - Split/Second speedometer setup (unified)
//
// One tool for both builds. Drop it next to SplitSecond.exe and run it:
//
//   - dc / retail build: hud data sits as loose files. The tool finds
//     Deferred\GameModeHUDs (or Data\GameModeHUDs), rewrites the camera
//     placement of the target player element, and installs DialsInfo.gfx.
//
//   - steam / ark build: hud data lives inside data\Resident.ark. The tool
//     finds that archive and pushes the same parameter set plus the movie
//     into it (append + retarget, original rows saved for undo).
//
// Detection is by directory layout, so the user never has to pick a build.
// Nothing is written until the patch button is pressed.

#include <windows.h>
#include <commdlg.h>
#include <d3d11.h>
#include <dwmapi.h>

#include <cfloat>
#include <cstdio>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui/backends/imgui_impl_dx11.h"
#include "imgui/backends/imgui_impl_win32.h"
#include "ark.h"
#include "dc_params.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam,
                                                             LPARAM lParam);

namespace {

// ---------------------------------------------------------------------------
// Window / D3D11
// ---------------------------------------------------------------------------
constexpr int kClientWidth = 620;
constexpr int kClientHeight = 470;
constexpr int kMinWindowWidth = 460;
constexpr int kMinWindowHeight = 320;

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_swapChain = nullptr;
ID3D11RenderTargetView* g_renderTarget = nullptr;
HWND g_hwnd = nullptr;

UINT g_pendingWidth = 0;
UINT g_pendingHeight = 0;

const ImVec4 kOk = ImVec4(0.30f, 0.90f, 0.35f, 1.0f);
const ImVec4 kWarn = ImVec4(0.95f, 0.75f, 0.30f, 1.0f);
const ImVec4 kError = ImVec4(0.95f, 0.35f, 0.35f, 1.0f);

// ---------------------------------------------------------------------------
// Build detection
// ---------------------------------------------------------------------------
enum class Build { Unknown, Dc, Ark };

struct BuildInfo {
    Build build = Build::Unknown;
    std::string arkPath;   // valid when build == Ark
};

BuildInfo detectBuild(const std::string& root) {
    BuildInfo info;

    // Dc first: loose hud parameters are unambiguous.
    {
        // Locate a GameModeHUDs folder without touching any file.
        std::vector<std::string> candidates;
        const char* leaves[] = { "\\Deferred\\GameModeHUDs", "\\Data\\GameModeHUDs" };
        std::string dir = root;
        for (int level = 0; level < 4 && !dir.empty(); ++level) {
            for (const char* leaf : leaves) {
                const std::string candidate = dir + leaf;
                const DWORD attrs = GetFileAttributesA(candidate.c_str());
                if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY))
                    candidates.push_back(candidate);
            }
            WIN32_FIND_DATAA fd;
            HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE) {
                do {
                    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.') {
                        for (const char* leaf : leaves) {
                            const std::string candidate = dir + "\\" + fd.cFileName + leaf;
                            const DWORD attrs = GetFileAttributesA(candidate.c_str());
                            if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY))
                                candidates.push_back(candidate);
                        }
                    }
                } while (FindNextFileA(h, &fd));
                FindClose(h);
            }
            const std::size_t slash = dir.find_last_of("\\/");
            if (slash == std::string::npos || slash < 3) break;
            dir = dir.substr(0, slash);
        }
        if (!candidates.empty()) {
            info.build = Build::Dc;
            return info;
        }
    }

    // Ark next.
    std::string arkPath;
    if (ark::findResidentArk(root, arkPath)) {
        info.build = Build::Ark;
        info.arkPath = arkPath;
    }
    return info;
}

// ---------------------------------------------------------------------------
// Application state
// ---------------------------------------------------------------------------
struct App {
    std::string treeRoot;                 // where the game is searched for
    BuildInfo build;

    // dc state
    bool dcDone = false;                  // parameters were applied this run
    bool dcGfx = true;                    // user choice: install DialsInfo.gfx too
    dc::Result dcResult;

    // ark state
    std::string arkPath;
    std::string arkInfo;
    bool opened = false;
    std::vector<ark::Item> items;
    bool fullBackup = false;

    std::string message;
    bool messageIsError = false;
};

App g_app;

std::string narrow(const wchar_t* wide) {
    if (!wide || !*wide) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(size > 0 ? size - 1 : 0), '\0');
    if (size > 1) WideCharToMultiByte(CP_UTF8, 0, wide, -1, &out[0], size, nullptr, nullptr);
    return out;
}

std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    std::wstring out(static_cast<std::size_t>(size > 0 ? size - 1 : 0), L'\0');
    if (size > 1) MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &out[0], size);
    return out;
}

std::string fileNameOf(const std::string& path) {
    const std::size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string exeDir() {
    char path[MAX_PATH];
    if (!GetModuleFileNameA(nullptr, path, MAX_PATH)) return ".";
    std::string p(path);
    const std::size_t slash = p.find_last_of("\\/");
    return slash == std::string::npos ? "." : p.substr(0, slash);
}

// ---------------------------------------------------------------------------
// Core actions
// ---------------------------------------------------------------------------
void setUpItems() {
    g_app.items.assign(static_cast<std::size_t>(ark::Target::Count), {});

    g_app.items[static_cast<std::size_t>(ark::Target::Params)].target = ark::Target::Params;
    g_app.items[static_cast<std::size_t>(ark::Target::Params)].label =
        "GameModeHUDs parameters (required)";
    g_app.items[static_cast<std::size_t>(ark::Target::Params)].mandatory = true;

    g_app.items[static_cast<std::size_t>(ark::Target::Gfx)].target = ark::Target::Gfx;
    g_app.items[static_cast<std::size_t>(ark::Target::Gfx)].label = "DialsInfo.gfx";
}

void openArk(const std::string& path) {
    g_app.message.clear();
    g_app.messageIsError = false;
    g_app.opened = false;
    g_app.arkInfo.clear();
    g_app.arkPath = path;

    ark::Archive archive;
    std::string error;
    if (!archive.open(path, error)) {
        g_app.message = error;
        g_app.messageIsError = true;
        return;
    }

    char info[160];
    std::snprintf(info, sizeof(info), "%u entries (%u stored, %u compressed)",
                  static_cast<unsigned>(archive.entryCount()), archive.rawCount(),
                  archive.compressedCount());
    g_app.arkInfo = info;

    setUpItems();
    ark::analyze(archive, g_app.items);
    g_app.opened = true;

    // The mandatory item decides whether this archive is even ours.
    for (const ark::Item& item : g_app.items) {
        if (item.mandatory && item.report.state == ark::State::Mismatch) {
            g_app.message = item.report.detail;
            g_app.messageIsError = true;
            break;
        }
    }
}

void autoDetect() {
    g_app.build = detectBuild(g_app.treeRoot);

    if (g_app.build.build == Build::Dc) {
        g_app.message = "Detected: dc build (loose hud files).";
        g_app.messageIsError = false;
    } else if (g_app.build.build == Build::Ark) {
        openArk(g_app.build.arkPath);
        if (g_app.message.empty()) {
            g_app.message = "Detected: ark build (data\\Resident.ark).";
            g_app.messageIsError = false;
        }
    } else {
        g_app.message = "Neither loose hud files nor data\\Resident.ark were found next to "
                        "this tool. Put it in the game folder (next to SplitSecond.exe), "
                        "or use Browse to point at the archive.";
        g_app.messageIsError = true;
    }
}

void browseForArk() {
    wchar_t buffer[MAX_PATH] = {};
    if (!g_app.arkPath.empty()) {
        const std::wstring wide = widen(g_app.arkPath);
        lstrcpynW(buffer, wide.c_str(), MAX_PATH);
    }

    OPENFILENAMEW dialog = {};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = g_hwnd;
    dialog.lpstrFilter = L"Game archives (*.ark)\0*.ark\0All files (*.*)\0*.*\0";
    dialog.lpstrFile = buffer;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrTitle = L"Select data\\Resident.ark";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameW(&dialog)) {
        g_app.build.build = Build::Ark;
        openArk(narrow(buffer));
    }
}

int pendingCount() {
    int pending = 0;
    for (const ark::Item& item : g_app.items)
        if (item.mandatory || item.enabled) pending += item.report.pending;
    return pending;
}

void runDcPatch() {
    g_app.dcResult = dc::applyParams(g_app.treeRoot);
    g_app.dcDone = true;

    if (!g_app.dcResult.found) {
        g_app.message = g_app.dcResult.error;
        g_app.messageIsError = true;
        return;
    }
    if (g_app.dcResult.failed > 0) {
        g_app.message = g_app.dcResult.error;
        g_app.messageIsError = true;
        return;
    }

    if (g_app.dcGfx) {
        std::string gfxError;
        if (!dc::installGfx(g_app.treeRoot, gfxError)) {
            g_app.message = "Parameters were applied, but the movie failed: " + gfxError;
            g_app.messageIsError = true;
            return;
        }
    }

    char text[256];
    std::snprintf(text, sizeof(text), "Done: %d updated, %d already correct, %d skipped.",
                  g_app.dcResult.updated, g_app.dcResult.unchanged, g_app.dcResult.skipped);
    g_app.message = text;
    g_app.messageIsError = false;
}

void runArkPatch() {
    if (!g_app.opened) return;
    if (pendingCount() == 0) {
        g_app.message = "Nothing to do - everything is already correct.";
        g_app.messageIsError = false;
        return;
    }

    ark::Archive archive;
    std::string error;
    if (!archive.open(g_app.arkPath, error)) {
        g_app.message = error;
        g_app.messageIsError = true;
        return;
    }

    std::string message;
    if (!ark::apply(archive, g_app.items, g_app.fullBackup, message, error)) {
        g_app.message = error;
        g_app.messageIsError = true;
        return;
    }
    archive.close();

    // Re-analyze so the rows reflect what is on disk now.
    const std::string path = g_app.arkPath;
    openArk(path);
    g_app.message = message;
    g_app.messageIsError = false;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------
const char* statusText(ark::State state) {
    switch (state) {
        case ark::State::Patchable: return "OK!";
        case ark::State::AlreadyPatched: return "already patched";
        default: return "mismatch";
    }
}

ImVec4 statusColor(ark::State state) {
    switch (state) {
        case ark::State::Patchable: return kOk;
        case ark::State::AlreadyPatched: return kWarn;
        default: return kError;
    }
}

void drawItemRow(std::size_t index) {
    ark::Item& item = g_app.items[index];

    bool checked = item.mandatory ? true : item.enabled;
    const bool locked = item.mandatory;
    const bool usable = g_app.opened && item.report.state != ark::State::Mismatch;

    ImGui::BeginDisabled(locked || !usable);
    if (ImGui::Checkbox(item.label, &checked) && !locked) item.enabled = checked;
    ImGui::EndDisabled();

    const char* status = statusText(item.report.state);
    const float statusWidth = ImGui::CalcTextSize(status).x;
    const float remaining = ImGui::GetContentRegionAvail().x;
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + remaining - statusWidth);
    ImGui::TextColored(statusColor(item.report.state), "%s", status);

    ImGui::TextDisabled("    %s", item.report.detail.c_str());
    ImGui::Spacing();
}

void drawDcContent() {
    ImGui::TextWrapped(
        "Detected the dc build: the hud data sits as loose files next to the game.");
    ImGui::Spacing();

    char pathBuffer[600];
    std::snprintf(pathBuffer, sizeof(pathBuffer), "%s", g_app.treeRoot.c_str());
    ImGui::TextUnformatted("Game folder");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputText("##game_folder", pathBuffer, sizeof(pathBuffer), ImGuiInputTextFlags_ReadOnly);

    ImGui::Spacing();
    ImGui::SeparatorText("Patches");
    ImGui::Spacing();

    ImGui::BeginDisabled(true);
    bool paramsLocked = true;
    ImGui::Checkbox("GameModeHUDs parameters (required)", &paramsLocked);
    ImGui::EndDisabled();

    // The dc build gets the movie from the loose ui folder; the tool ships it.
    ImGui::Checkbox("DialsInfo.gfx", &g_app.dcGfx);
    ImGui::Spacing();

    if (ImGui::Button("Ready to Patch", ImVec2(160.0f, 0.0f))) runDcPatch();

    if (g_app.dcDone && g_app.dcResult.found) {
        ImGui::Spacing();
        ImGui::TextDisabled("%s", g_app.dcResult.hudDir.c_str());
        char line[256];
        std::snprintf(line, sizeof(line), "%d updated, %d already correct, %d skipped, %d failed.",
                      g_app.dcResult.updated, g_app.dcResult.unchanged, g_app.dcResult.skipped,
                      g_app.dcResult.failed);
        ImGui::TextDisabled("%s", line);
    }
}

void drawArkContent() {
    ImGui::TextWrapped(
        "Detected the ark build: the hud data lives inside the game's archive.");
    ImGui::Spacing();

    char nameBuffer[260];
    char pathBuffer[600];
    std::snprintf(nameBuffer, sizeof(nameBuffer), "%s",
                  g_app.arkPath.empty() ? "" : fileNameOf(g_app.arkPath).c_str());
    std::snprintf(pathBuffer, sizeof(pathBuffer), "%s", g_app.arkPath.c_str());

    ImGui::TextUnformatted("File name");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputText("##file_name", nameBuffer, sizeof(nameBuffer), ImGuiInputTextFlags_ReadOnly);

    ImGui::TextUnformatted("Full path");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputText("##full_path", pathBuffer, sizeof(pathBuffer), ImGuiInputTextFlags_ReadOnly);

    if (!g_app.opened) {
        ImGui::Spacing();
        if (ImGui::Button("Browse...", ImVec2(120.0f, 0.0f))) browseForArk();
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Patches");
    ImGui::Spacing();

    if (g_app.opened) {
        for (std::size_t i = 0; i < g_app.items.size(); ++i) drawItemRow(i);
    } else {
        ImGui::TextDisabled("Point the tool at data\\Resident.ark to continue.");
        ImGui::Spacing();
        ImGui::Spacing();
    }

    ImGui::Checkbox("Create a full backup before patching  (.speedo-bak)", &g_app.fullBackup);
    ImGui::Spacing();

    ImGui::BeginDisabled(!g_app.opened || pendingCount() == 0);
    if (ImGui::Button("Ready to Patch", ImVec2(160.0f, 0.0f))) runArkPatch();
    ImGui::EndDisabled();

    if (!g_app.arkInfo.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("%s", g_app.arkInfo.c_str());
    }
}

void drawContent() {
    if (g_app.build.build == Build::Dc) drawDcContent();
    else if (g_app.build.build == Build::Ark) drawArkContent();
    else {
        ImGui::TextWrapped("No game installation was detected next to this tool.");
        ImGui::Spacing();
        if (ImGui::Button("Browse for data\\Resident.ark...", ImVec2(240.0f, 0.0f))) {
            browseForArk();
        }
        ImGui::Spacing();
        ImGui::TextWrapped(
            "If this is the dc build, put this tool in the game folder "
            "(next to SplitSecond.exe) and run it again.");
    }

    if (!g_app.message.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(g_app.messageIsError ? kError : kOk, "%s", g_app.message.c_str());
    }
}

// ---------------------------------------------------------------------------
// Win32 plumbing
// ---------------------------------------------------------------------------
LRESULT WINAPI windowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam)) return TRUE;

    switch (msg) {
        case WM_SIZE:
            if (wparam != SIZE_MINIMIZED) {
                g_pendingWidth = static_cast<UINT>(LOWORD(lparam));
                g_pendingHeight = static_cast<UINT>(HIWORD(lparam));
            }
            return 0;
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
            info->ptMinTrackSize.x = kMinWindowWidth;
            info->ptMinTrackSize.y = kMinWindowHeight;
            return 0;
        }
        case WM_SYSCOMMAND:
            if ((wparam & 0xfff0) == SC_KEYMENU) return 0;
            break;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void applyTitleBarTheme(HWND hwnd, bool dark) {
    const BOOL value = dark ? TRUE : FALSE;
    constexpr DWORD kImmersiveDarkMode = 20;
    constexpr DWORD kImmersiveDarkModeLegacy = 19;
    if (FAILED(DwmSetWindowAttribute(hwnd, kImmersiveDarkMode, &value, sizeof(value)))) {
        DwmSetWindowAttribute(hwnd, kImmersiveDarkModeLegacy, &value, sizeof(value));
    }
}

void createRenderTarget() {
    ID3D11Texture2D* backBuffer = nullptr;
    g_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (backBuffer) {
        g_device->CreateRenderTargetView(backBuffer, nullptr, &g_renderTarget);
        backBuffer->Release();
    }
}

void cleanupRenderTarget() {
    if (g_renderTarget) {
        g_renderTarget->Release();
        g_renderTarget = nullptr;
    }
}

bool createDeviceD3D(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC desc = {};
    desc.BufferCount = 2;
    desc.BufferDesc.Width = 0;
    desc.BufferDesc.Height = 0;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferDesc.RefreshRate.Numerator = 60;
    desc.BufferDesc.RefreshRate.Denominator = 1;
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = hwnd;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL obtained = {};
    const HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &desc,
        &g_swapChain, &g_device, &obtained, &g_context);
    if (FAILED(hr)) return false;

    createRenderTarget();
    return true;
}

void cleanupDeviceD3D() {
    cleanupRenderTarget();
    if (g_swapChain) {
        g_swapChain->Release();
        g_swapChain = nullptr;
    }
    if (g_context) {
        g_context->Release();
        g_context = nullptr;
    }
    if (g_device) {
        g_device->Release();
        g_device = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Headless helpers (used while bringing the tool up, not by the ui)
// ---------------------------------------------------------------------------
int runReport(const std::string& outPath) {
    FILE* fp = nullptr;
    if (fopen_s(&fp, outPath.c_str(), "wb") != 0 || !fp) return 1;

    g_app.build = detectBuild(g_app.treeRoot);
    std::fprintf(fp, "speedo setup report\n");
    std::fprintf(fp, "root: %s\n", g_app.treeRoot.c_str());
    std::fprintf(fp, "build: %s\n",
                 g_app.build.build == Build::Dc ? "dc"
                 : g_app.build.build == Build::Ark ? "ark" : "unknown");

    if (g_app.build.build == Build::Ark) {
        openArk(g_app.build.arkPath);
        std::fprintf(fp, "ark: %s\n", g_app.arkPath.c_str());
        std::fprintf(fp, "info: %s\n", g_app.arkInfo.c_str());
        std::fprintf(fp, "opened: %s\n", g_app.opened ? "yes" : "no");
        if (!g_app.message.empty()) std::fprintf(fp, "message: %s\n", g_app.message.c_str());
        for (const ark::Item& item : g_app.items) {
            std::fprintf(fp, "\nitem: %s\n", item.label);
            std::fprintf(fp, "  state  : %s\n", statusText(item.report.state));
            std::fprintf(fp, "  found  : %d/%d\n", item.report.found, item.report.total);
            std::fprintf(fp, "  pending: %d\n", item.report.pending);
            std::fprintf(fp, "  detail : %s\n", item.report.detail.c_str());
        }
    } else if (g_app.build.build == Build::Dc) {
        // Read-only: report mode must never touch the files.
        std::string hudDir;
        const bool found = dc::probe(g_app.treeRoot, hudDir);
        std::fprintf(fp, "params found: %s\n", found ? "yes" : "no");
        if (found) {
            std::fprintf(fp, "hud dir: %s\n", hudDir.c_str());
        } else {
            std::fprintf(fp, "error: Could not find the hud parameters.\n");
        }
    }
    std::fclose(fp);
    return 0;
}

int runRestore(const std::string& arkPath) {
    std::string message;
    std::string error;
    if (!ark::restoreFromUndo(arkPath, message, error)) return 1;
    return 0;
}

int runApply(const std::string& root) {
    // Headless apply, used for scripted testing.
    BuildInfo info = detectBuild(root);
    if (info.build == Build::Dc) {
        dc::Result result = dc::applyParams(root);
        if (!result.found || result.failed > 0) return 1;
        std::printf("dc: %d updated, %d already correct, %d skipped\n",
                    result.updated, result.unchanged, result.skipped);
        return 0;
    }
    if (info.build == Build::Ark) {
        std::vector<ark::Item> items;
        setUpItems();
        ark::Archive archive;
        std::string error;
        if (!archive.open(info.arkPath, error)) {
            std::printf("ark: %s\n", error.c_str());
            return 1;
        }
        ark::analyze(archive, items);
        std::string message;
        if (!ark::apply(archive, items, false, message, error)) {
            std::printf("ark: %s\n", error.c_str());
            return 1;
        }
        std::printf("ark: %s\n", message.c_str());
        return 0;
    }
    std::printf("no build detected under %s\n", root.c_str());
    return 1;
}

}  // namespace

int realMain(HINSTANCE instance);

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    // The tool may be dropped next to the game's exe, where proxy dlls
    // (version.dll, d3d9.dll, ...) shipped for the game live. Those get picked
    // up by the standard loader search when system components load them and
    // crash this process. Restricting the search to System32 keeps them out.
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    return realMain(instance);
}

int realMain(HINSTANCE instance) {
    // (loader lockdown already done in wWinMain)
    g_app.treeRoot = exeDir();

    // Hidden switches, used while bringing the tool up. All arguments are
    // collected first, then dispatched, so their order does not matter.
    std::string reportPath;
    std::string restorePath;
    std::string applyPath;
    for (int i = 1; i < __argc; ++i) {
        const std::wstring arg = __wargv[i];
        if (arg == L"--ark" && i + 1 < __argc) {
            const std::string path = narrow(__wargv[i + 1]);
            const DWORD attrs = GetFileAttributesA(path.c_str());
            if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY))
                g_app.treeRoot = path;                       // a folder was given
            else if (attrs != INVALID_FILE_ATTRIBUTES) {
                g_app.build.build = Build::Ark;
                g_app.build.arkPath = path;                  // a file was given
            }
        } else if (arg == L"--report" && i + 1 < __argc) {
            reportPath = narrow(__wargv[i + 1]);
        } else if (arg == L"--restore" && i + 1 < __argc) {
            restorePath = narrow(__wargv[i + 1]);
        } else if (arg == L"--apply" && i + 1 < __argc) {
            applyPath = narrow(__wargv[i + 1]);
        }
    }
    if (!restorePath.empty()) return runRestore(restorePath);
    if (!reportPath.empty()) return runReport(reportPath);
    if (!applyPath.empty()) return runApply(applyPath);

    ImGui_ImplWin32_EnableDpiAwareness();

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = windowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"SpeedoSetupWindow";
    RegisterClassExW(&wc);

    const DWORD style = WS_OVERLAPPEDWINDOW;
    RECT rect = { 0, 0, kClientWidth, kClientHeight };
    AdjustWindowRect(&rect, style, FALSE);

    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"Split/Second - speedometer setup", style,
                             CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left,
                             rect.bottom - rect.top, nullptr, nullptr, instance, nullptr);
    if (!g_hwnd) {
        UnregisterClassW(wc.lpszClassName, instance);
        return 1;
    }

    applyTitleBarTheme(g_hwnd, true);

    if (!createDeviceD3D(g_hwnd)) {
        cleanupDeviceD3D();
        DestroyWindow(g_hwnd);
        UnregisterClassW(wc.lpszClassName, instance);
        return 1;
    }

    ShowWindow(g_hwnd, SW_SHOWDEFAULT);
    UpdateWindow(g_hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    autoDetect();

    bool running = true;
    while (running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) running = false;
        }
        if (!running) break;

        if (g_pendingWidth != 0 && g_pendingHeight != 0) {
            cleanupRenderTarget();
            g_swapChain->ResizeBuffers(0, g_pendingWidth, g_pendingHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_pendingWidth = 0;
            g_pendingHeight = 0;
            createRenderTarget();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        const ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("main", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_NoBringToFrontOnFocus);
        drawContent();
        ImGui::End();

        ImGui::Render();
        const float clear[4] = { 0.06f, 0.06f, 0.06f, 1.0f };
        g_context->OMSetRenderTargets(1, &g_renderTarget, nullptr);
        g_context->ClearRenderTargetView(g_renderTarget, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swapChain->Present(1, 0);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    cleanupDeviceD3D();
    DestroyWindow(g_hwnd);
    UnregisterClassW(wc.lpszClassName, instance);
    return 0;
}
