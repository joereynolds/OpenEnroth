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
 * It first asks whether to host a new game, host a saved one, or join, then shows only the fields that one needs.
 * Hosting a new game opens a lobby and goes on to party creation, see `CoopLobbyState`. Hosting a saved game goes on to
 * the load menu. Joining starts a client session and goes back to the main menu, which hands over to the join screen
 * right away, and the guest picks a party once connected, see `CoopPartyState`.
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
