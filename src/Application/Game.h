#pragma once

#include <memory>
#include <string_view>

#include "Application/Startup/GameStarterOptions.h"

#include "Io/KeyboardInputHandler.h"
#include "Io/Mouse.h"

using Io::Mouse;

class Engine;
struct DecalBuilder;
class PlatformApplication;
class Platform;
class Menu;

class Game {
 public:
    Game(PlatformApplication *application, std::shared_ptr<GameConfig> config, CoopOptions coopOptions);
    ~Game();

    int run();

 private:
    bool loop();
    void processQueuedMessages();
    void gameLoop();
    void closeTargetedSpellWindow();
    void onEscape();
    void onPressSpace();
    void showCoopVote();

 private:
    PlatformApplication *_application = nullptr;
    std::shared_ptr<GameConfig> _config;
    std::unique_ptr<Menu> _menu;
    DecalBuilder *_decalBuilder = nullptr;
    CoopOptions _coopOptions;
};

extern std::unique_ptr<GraphicsImage> gamma_preview_image;  // 506E40
