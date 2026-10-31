#pragma once

#include <chrono>
#include <optional>
#include <string_view>

#include "Library/Fsm/FsmState.h"

class GraphicsImage;

/**
 * Waits for the co-op client in `coopSession` to join, then exits into the game with the host's save. Shows the
 * reason and goes back to the main menu if joining fails.
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
    std::optional<std::chrono::steady_clock::time_point> _failedAt;
};
