#pragma once

#include <memory>

#include "Library/Fsm/FsmState.h"

class GUIWindow_CoopLobby;

/**
 * Where co-op players wait for a new game to start, with everyone's party listed as ready or still being made.
 *
 * The host starts the game from here once everyone is ready. A guest leaves for the host's game as soon as the host
 * welcomes it, which is right away when the host is already playing. A guest whose session failed is told why.
 */
class CoopLobbyState : public FsmState {
 public:
    CoopLobbyState();
    virtual ~CoopLobbyState();

    virtual FsmAction enter() override;
    virtual FsmAction update() override;
    virtual void exit() override;

 private:
    std::unique_ptr<GUIWindow_CoopLobby> _window;
};
