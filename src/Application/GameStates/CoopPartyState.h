#pragma once

#include <memory>

#include "Library/Fsm/FsmState.h"

class GUIWindow_CoopParty;

/**
 * Asks a guest that has just connected which party to play: a new one, or one from the guest's saves.
 *
 * A save is handed to `coopSession` and the guest moves on to the lobby, which joins the host's game as soon as it can.
 * A new party is made in the usual party creation screen first, see `Game::loop`.
 */
class CoopPartyState : public FsmState {
 public:
    CoopPartyState();
    virtual ~CoopPartyState();

    virtual FsmAction enter() override;
    virtual FsmAction update() override;
    virtual void exit() override;

 private:
    std::unique_ptr<GUIWindow_CoopParty> _window;
};
