#pragma once

#include <array>
#include <memory>
#include <string>

#include "Library/Fsm/FsmState.h"

class GUIWindow_CoopMenu;
enum class CoopMenuPage;

/**
 * Main menu screen for hosting or joining a co-op game, an alternative to the command line options.
 *
 * It first asks whether to host or join, then shows only the fields that one needs. Hosting starts listening and goes
 * back to the main menu, where the host starts or loads a game. Joining starts a client session and goes back to the
 * main menu too, which hands over to the join screen right away.
 */
class CoopMenuState : public FsmState {
 public:
    CoopMenuState();
    virtual ~CoopMenuState();

    virtual FsmAction enter() override;
    virtual FsmAction update() override;
    virtual void exit() override;

 private:
    void showPage(CoopMenuPage page);
    void selectField(int field);
    [[nodiscard]] bool host();
    [[nodiscard]] bool join();

 private:
    std::unique_ptr<GUIWindow_CoopMenu> _window;
};
