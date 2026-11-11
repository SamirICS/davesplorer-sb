// Font loading: Open Sans for the interface, Fira Code for anything
// column-aligned, Material Icons merged into the text faces.
//
// With ImGui 1.92 a font is a face, not a face-at-a-size: glyphs rasterise on
// demand, so there is one face per style and the size is a PushFont argument.
#pragma once

#include "imgui.h"

#include <string>
#include <vector>

namespace ui::fonts
{

// Loads from <assetsDir>/fonts. Falls back to ImGui's built-in font when a
// file is missing, so the app still runs; the error says which file.
bool Load(const std::string& assetsDir, float sizePx, std::string& error);

ImFont* Ui();
ImFont* Bold();
ImFont* Italic();
ImFont* Mono();
float   Size();

// The interface faces the Settings window offers: Open Sans, which ships
// with the app, then whichever common Linux families are installed, all
// loaded at start (glyphs are rasterised only when drawn).
inline constexpr const char* kDefaultFamily = "Open Sans";
inline constexpr float kDefaultSize = 18.0f;
inline constexpr float kMinSize = 12.0f;
inline constexpr float kMaxSize = 30.0f;
const std::vector<std::string>& Families();

// Between frames: the face and base text size every window starts with.
// An unknown family falls back to Open Sans.
void Apply(const std::string& family, float sizePx);

} // namespace ui::fonts
