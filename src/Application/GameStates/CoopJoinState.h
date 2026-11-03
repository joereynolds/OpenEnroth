#pragma once

#include <chrono>
#include <optional>
#include <string_view>

#include "Library/Fsm/FsmState.h"

class GraphicsImage;

constexpr std::string_view COOP_JOINED_SAVE_FILE_NAME = "coop.mm7"; // The host's world as a guest joined it, replaced on every join.

/**
 * Waits for the co-op client in `coopSession` to join, then exits into the game with the host's save. Keeps trying
 * for a while if the host isn't reachable or isn't in a game yet. Shows the reason and goes back to the main menu if
 * joining fails for good.
 */
class CoopJoinState : public FsmState {
 public:
    virtual FsmAction enter() override;
    virtual FsmAction update() override;
    virtual void exit() override;

    /**
     * @return                          Whether `coopSession` holds a client that's still waiting to join.
     */
    [[nodiscard]] static bool isJoinPending();

 private:
    void drawMessage(std::string_view message);

 private:
    GraphicsImage *_background = nullptr;
    std::chrono::steady_clock::time_point _enteredAt;
    std::chrono::steady_clock::time_point _attemptedAt;
    std::optional<std::chrono::steady_clock::time_point> _failedAt;
    bool _retrying = false;
};
