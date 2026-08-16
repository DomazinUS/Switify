/*
    Copyright 2021 natinusala
    Copyright 2023 xfangfang

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
*/

#include <unistd.h>

#include <borealis/core/application.hpp>
#include <borealis/core/assets.hpp>
#include <borealis/platforms/desktop/desktop_font.hpp>

#define INTER_FONT_PATH BRLS_ASSET("font/switch_font.ttf")
#define INTER_ICON_PATH BRLS_ASSET("font/switch_icons.ttf")

namespace brls
{

bool DesktopFontLoader::loadFont(const std::string& name, const std::string& path) {
#ifdef USE_LIBROMFS
    if (path.empty()) return false;
    if (path.rfind("@res/", 0) == 0)
    {
        // font is inside the romfs
        try
        {
            auto& font = romfs::get(path.substr(5));
            if (font.valid() && Application::loadFontFromMemory(name, (void*)font.data(), font.size(), false))
                return true;
        }
        catch (...)
        {
        }
    } else
#endif
    if (access(path.c_str(), F_OK) != -1 && Application::loadFontFromFile(name, path)) {
        return true;
    }

    return false;
}

void DesktopFontLoader::loadFonts()
{
    NVGcontext* vg = brls::Application::getNVGContext();

    // Text font
    if (loadFont(FONT_REGULAR, USER_FONT_PATH))
    {
        // Using internal font as fallback
        if (loadFont("default", INTER_FONT_PATH))
        {
            nvgAddFallbackFontId(vg, Application::getFont(FONT_REGULAR), Application::getFont("default"));
        }
    } else {
        Logger::warning("Cannot find custom font, (Searched at: {})", USER_FONT_PATH);
        if (!loadFont(FONT_REGULAR, INTER_FONT_PATH))
        {
            Logger::warning("Failed to load internal font, text may not be displayed");
        }
        else
        {
            Logger::info("Trying to use internal font: {}", INTER_FONT_PATH);
        }
    }

    if (loadFont(FONT_CHINESE_SIMPLIFIED, BRLS_ASSET("font/chinese_fallback.ttf")))
    {
        nvgAddFallbackFontId(vg, Application::getFont(FONT_REGULAR), Application::getFont(FONT_CHINESE_SIMPLIFIED));
    }
    if (loadFont(FONT_KOREAN_REGULAR, BRLS_ASSET("font/korean_fallback.ttf")))
    {
        nvgAddFallbackFontId(vg, Application::getFont(FONT_REGULAR), Application::getFont(FONT_KOREAN_REGULAR));
    }

    // Load Emoji
    if (loadFont(FONT_EMOJI, USER_EMOJI_PATH))
    {
        nvgAddFallbackFontId(vg, Application::getFont(FONT_REGULAR), Application::getFont(FONT_EMOJI));
    }

    // bottom bar icons
    if (loadFont(FONT_SWITCH_ICONS, USER_ICON_PATH))
    {
        // User-provided icons
        nvgAddFallbackFontId(vg, Application::getFont(FONT_REGULAR), Application::getFont(FONT_SWITCH_ICONS));
    }
    else
    {
        brls::Logger::warning("Cannot find custom icon, (Searched at: {})", USER_ICON_PATH);
        brls::Logger::info("Trying to use internal icon: {}", INTER_ICON_PATH);
        if (loadFont(FONT_SWITCH_ICONS, INTER_ICON_PATH))
        {
            // Internal icons
            nvgAddFallbackFontId(vg, Application::getFont(FONT_REGULAR), Application::getFont(FONT_SWITCH_ICONS));
        }
        else
        {
            Logger::warning("Failed to load internal icons, bottom bar icons may not be displayed");
        }
    }

    // Material icons
    if (this->loadMaterialFromResources())
    {
        nvgAddFallbackFontId(vg, Application::getFont(FONT_REGULAR), Application::getFont(FONT_MATERIAL_ICONS));
    }
    else
    {
        Logger::error("switch: could not load Material icons font from resources");
    }
}

} // namespace brls
