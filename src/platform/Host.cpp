#include "platform/Host.h"
#include "platform/DragDrop.h"
#include "platform/Paths.h"
#include "platform/Strings.h"

#include "app/AppState.h"
#include "app/TreeScan.h"
#include "ui/Fonts.h"
#include "ui/MainWindow.h"
#include "ui/Textures.h"
#include "ui/Theme.h"
#include "ui/Thumbnails.h"
#include "ui/UiState.h"

#include <SDL3/SDL.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include "app/Layout.h"
#include "app/PathUtil.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>

namespace platform
{

namespace
{

constexpr float kBaseFontPx = 18.0f;

std::string BuildInfo()
{
    const int v = SDL_GetVersion();
    char buf[128];
    std::snprintf(buf, sizeof(buf), "SDL %d.%d.%d, built %s", SDL_VERSIONNUM_MAJOR(v),
                  SDL_VERSIONNUM_MINOR(v), SDL_VERSIONNUM_MICRO(v), __DATE__);
    return buf;
}

void ShowFatal(const char* what)
{
    // Stderr goes nowhere for a WIN32-subsystem exe; the message box is the
    // only channel that reaches the user.
    std::fprintf(stderr, "%s\n", what);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Davesplorer", what, nullptr);
}

// The layout file dialogs. SDL may answer on another thread, so the answer
// waits here until the main loop hands it to the interface.
struct LayoutDialogResult
{
    std::mutex       mutex;
    bool             done = false;
    ui::FileDialog   kind = ui::FileDialog::None;
    std::string      path;    // empty when the user cancelled
    std::string      error;
};
LayoutDialogResult g_layoutDialog;

void SDLCALL OnLayoutDialog(void* userdata, const char* const* files, int)
{
    std::lock_guard<std::mutex> lock(g_layoutDialog.mutex);
    g_layoutDialog.done = true;
    g_layoutDialog.kind = (ui::FileDialog)(intptr_t)userdata;
    g_layoutDialog.path.clear();
    g_layoutDialog.error.clear();
    if (!files) g_layoutDialog.error = SDL_GetError();
    else if (files[0]) g_layoutDialog.path = files[0];
}

void ShowLayoutDialog(SDL_Window* window, ui::FileDialog kind, const app::AppState& state)
{
    static const SDL_DialogFileFilter filters[] = { { "Davesplorer layouts", app::kLayoutExtension } };
    // Layouts are suggested a home of their own beside the settings.
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(reinterpret_cast<const char8_t*>(state.layoutsDir.c_str())), ec);
    void* tag = (void*)(intptr_t)kind;
    if (kind == ui::FileDialog::OpenLayout)
        {
        SDL_ShowOpenFileDialog(OnLayoutDialog, tag, window, filters, 1, state.layoutsDir.c_str(), false);
        }
    else
        {
        const std::string suggested = !state.currentLayout.empty()
            ? state.currentLayout
            : app::JoinPath(state.layoutsDir, std::string("layout.") + app::kLayoutExtension);
        SDL_ShowSaveFileDialog(OnLayoutDialog, tag, window, filters, 1, suggested.c_str());
        }
}

// A finished dialog becomes a layout to open or to save to.
void PumpLayoutDialog(ui::UiState& ui)
{
    std::lock_guard<std::mutex> lock(g_layoutDialog.mutex);
    if (!g_layoutDialog.done) return;
    g_layoutDialog.done = false;
    ui.fileDialogOpen = false;
    std::string path = std::move(g_layoutDialog.path);
    if (!g_layoutDialog.error.empty())
        {
        ui::ShowError(ui, "Could not show the file dialog: " + g_layoutDialog.error);
        return;
        }
    if (path.empty()) return;   // cancelled
    if (g_layoutDialog.kind == ui::FileDialog::OpenLayout)
        {
        ui.layoutToOpen = path;
        return;
        }
    // Not every desktop's save dialog adds the extension the filter names.
    const std::string ext = std::string(".") + app::kLayoutExtension;
    if (path.size() < ext.size() || ToLowerAscii(path.substr(path.size() - ext.size())) != ext) path += ext;
    ui.layoutToSave = path;
}

} // namespace

int RunApplication(int argc, char** argv)
{
    SDL_SetAppMetadata("Davesplorer", "0.1.0", "com.davesplorer.Davesplorer");
    // The window class takes its icon from resource 101 in app.rc, which
    // is the same red folder Explorer shows on the exe; no pixel data has
    // to be shipped or decoded for it.
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "101");
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON_SMALL, "101");
    if (!SDL_Init(SDL_INIT_VIDEO))
        {
        ShowFatal(SDL_GetError());
        return 1;
        }

    app::AppState state;
#ifdef _WIN32
    app::InitAppState(state, ExecutableDir());
#else
    const std::string configDir = app::JoinPath(UserConfigDir(), "davesplorer");
    std::error_code configError;
    std::filesystem::create_directories(configDir, configError);
    if (configError)
        {
        ShowFatal(("Could not create settings directory: " + configError.message()).c_str());
        SDL_Quit();
        return 1;
        }
    app::InitAppState(state, ExecutableDir(), configDir);
#endif
    // "davesplorer.exe <folder-or-file>": what the shell passes when a
    // folder is opened with this program. SDL's main shim already
    // converted the arguments to UTF-8.
    if (argc > 1 && argv[1] && argv[1][0] != '\0') app::ApplyStartArgument(state, argv[1]);
    state.buildInfo = BuildInfo();
    const app::Settings& s = state.settings;

    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN;
    if (s.windowMaximized) flags |= SDL_WINDOW_MAXIMIZED;
    SDL_Window* window = SDL_CreateWindow("Davesplorer", s.windowWidth, s.windowHeight, flags);
    if (!window)
        {
        ShowFatal(SDL_GetError());
        SDL_Quit();
        return 1;
        }
#ifndef _WIN32
    const std::string iconPath = app::JoinPath(state.exeDir, "assets/icon/davesplorer.png");
    if (SDL_Surface* icon = SDL_LoadPNG(iconPath.c_str()))
        {
        SDL_SetWindowIcon(window, icon);
        SDL_DestroySurface(icon);
        }
#endif
    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer)
        {
        ShowFatal(SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
        }
    SDL_SetRenderVSync(renderer, 1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = state.layoutFile.c_str();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigDpiScaleFonts = true;
    // Explorer's Ctrl+Tab switches tabs; ImGui's default would open its
    // window-switching overlay on the same chord.
    ImGui::GetCurrentContext()->ConfigNavWindowingKeyNext = 0;
    ImGui::GetCurrentContext()->ConfigNavWindowingKeyPrev = 0;

    std::string fontError;
    ui::fonts::Load(app::JoinPath(state.exeDir, "assets"), kBaseFontPx, fontError);

    ui::UiState uiState;
    uiState.dpiScale = SDL_GetWindowDisplayScale(window);
    ui::ApplyAppearanceNow(s.themeIndex, s.uiScale, uiState.dpiScale);
    if (!fontError.empty()) state.statusMessage = fontError;

    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);
    SDL_ShowWindow(window);
    InitDragOut(window);

    bool running = true;
    while (running && !state.quitRequested)
        {
        SDL_Event event;
        // Block until something happens rather than spinning: a file
        // explorer sitting idle should cost nothing. The timeout keeps
        // tooltips and blinking cursors alive; an animation asks for
        // frames at display rate.
        if (SDL_WaitEventTimeout(&event, uiState.animating ? 16 : 100))
            {
            do
                {
                ImGui_ImplSDL3_ProcessEvent(&event);
                switch (event.type)
                    {
                    case SDL_EVENT_QUIT:
                        running = false;
                        break;
                    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                        if (event.window.windowID == SDL_GetWindowID(window)) running = false;
                        break;
                    // A drag from another program. OLE owns the mouse while
                    // it lasts, so the drop position is fed to ImGui by hand
                    // for the panes to find the folder under the cursor.
                    case SDL_EVENT_DROP_BEGIN:
                        uiState.externalDrop = ui::UiState::ExternalDrop{};
                        uiState.externalDrop.active = true;
                        break;
                    case SDL_EVENT_DROP_POSITION:
                        io.AddMousePosEvent(event.drop.x, event.drop.y);
                        break;
                    case SDL_EVENT_DROP_FILE:
                        io.AddMousePosEvent(event.drop.x, event.drop.y);
                        if (event.drop.data) uiState.externalDrop.paths.push_back(event.drop.data);
                        break;
                    case SDL_EVENT_DROP_COMPLETE:
                        uiState.externalDrop.active = false;
                        uiState.externalDrop.completed = true;
#ifdef _WIN32
                        uiState.externalDrop.ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
                        uiState.externalDrop.shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
#else
                        uiState.externalDrop.ctrl = (SDL_GetModState() & SDL_KMOD_CTRL) != 0;
                        uiState.externalDrop.shift = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
#endif
                        break;
                    default:
                        break;
                    }
                }
            while (SDL_PollEvent(&event));
            }
        PumpLayoutDialog(uiState);
        // A drag handed to another program has ended; a move took items
        // out of whatever folder is showing.
        DragOutcome dragOutcome;
        if (PollDragOut(dragOutcome)) app::RefreshAll(state);
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED)
            {
            SDL_Delay(50);
            continue;
            }

        uiState.dpiScale = SDL_GetWindowDisplayScale(window);
        // Between frames only: ScaleAllSizes is not idempotent and a
        // mid-frame metric change leaves the rest of the frame inconsistent.
        ui::PumpAppearance(state.settings.themeIndex, state.settings.uiScale, uiState.dpiScale);

        app::TickAppState(state);

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        ui::DrawFrame(state, uiState);
        // While a drag is out over other programs the window's cursor is
        // the drag's cursor (X11 shows it for the whole implicit grab), so
        // it says whether the program under it would take the drop.
        bool dragAccepted = false;
        if (DragOutInProgress(dragAccepted))
            ImGui::SetMouseCursor(dragAccepted ? ImGuiMouseCursor_Hand : ImGuiMouseCursor_NotAllowed);
        ImGui::Render();

        const ImVec4& bg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
        SDL_SetRenderScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
        SDL_SetRenderDrawColorFloat(renderer, bg.x, bg.y, bg.z, 1.0f);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);

        if (uiState.fileDialogRequest != ui::FileDialog::None)
            {
            if (!uiState.fileDialogOpen)
                {
                uiState.fileDialogOpen = true;
                ShowLayoutDialog(window, uiState.fileDialogRequest, state);
                }
            uiState.fileDialogRequest = ui::FileDialog::None;
            }

        // An in-app drag crossed the window edge: hand it to the desktop so
        // Explorer, a file manager, a browser or any other program can take
        // it. From here the button-up belongs to the desktop's drag (OLE
        // blocks until it; X11 and Wayland run alongside this loop), so
        // ImGui is told now or it would keep dragging.
        if (uiState.externalDragRequested)
            {
            uiState.externalDragRequested = false;
            const std::vector<std::string> paths = uiState.dragPaths;
            uiState.dragPaths.clear();
            ImGui::ClearDragDrop();
            io.AddMouseButtonEvent(0, false);
            std::string error;
            if (!StartDragOut(paths, error)) state.statusMessage = error;
            }
        }

    // Window geometry goes back into settings so the next launch opens the
    // same way.
    const SDL_WindowFlags finalFlags = SDL_GetWindowFlags(window);
    state.settings.windowMaximized = (finalFlags & SDL_WINDOW_MAXIMIZED) != 0;
    if (!state.settings.windowMaximized)
        SDL_GetWindowSize(window, &state.settings.windowWidth, &state.settings.windowHeight);
    app::SaveAppState(state);
    ShutdownDragOut();

    app::treescan::Shutdown();           // joins the folder-walking thread
    app::programscan::Shutdown();        // and the program-finding one
    ui::thumbnails::Shutdown();          // joins the decoder thread first
    ImGui_ImplSDLRenderer3_Shutdown();   // destroys the GPU side of every texture, ours included
    ui::textures::ReleaseAll();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

} // namespace platform
