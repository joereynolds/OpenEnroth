#include "CoopJoinState.h"

#include <string>

#include "Engine/AssetsManager.h"
#include "Engine/Engine.h"
#include "Engine/Coop/CoopClient.h"
#include "Engine/Coop/CoopSession.h"
#include "Engine/Graphics/Renderer/Renderer.h"
#include "Engine/Resources/EngineFileSystem.h"

#include "GUI/GUIWindow.h"

#include "Utility/String/Format.h"

static constexpr char COOP_SAVE_FILE_NAME[] = "coop.mm7";
static constexpr std::chrono::seconds FAILURE_MESSAGE_DURATION{5};
static constexpr std::chrono::seconds RETRY_INTERVAL{2};
static constexpr std::chrono::seconds RETRY_DURATION{120};
static constexpr char WAITING_MESSAGE[] = "Waiting for the host to start a game...";

FsmAction CoopJoinState::enter() {
    _background = assets->getIcon("mm6title.pcx");
    _enteredAt = _attemptedAt = std::chrono::steady_clock::now();
    _failedAt.reset();
    _retrying = false;
    return FsmAction::none();
}

FsmAction CoopJoinState::update() {
    CoopClient *client = coopSession->client();
    client->update();

    switch (client->state()) {
    case COOP_CLIENT_CONNECTING:
        drawMessage(_retrying ? WAITING_MESSAGE : "Joining co-op game...");
        return FsmAction::none();
    case COOP_CLIENT_JOINED: {
        std::string fileName = COOP_SAVE_FILE_NAME;
        ufs->write(fmt::format("saves/{}", fileName), coopSession->takeJoinedSave());
        engine->_pendingLoadFileName = fileName;
        SetCurrentMenuID(MENU_LoadingProcInMainMenu);
        return FsmAction::transition("joined");
    }
    case COOP_CLIENT_FAILED:
    default: {
        auto now = std::chrono::steady_clock::now();
        if (client->canRetry() && now - _enteredAt < RETRY_DURATION) {
            drawMessage(WAITING_MESSAGE);
            if (now - _attemptedAt >= RETRY_INTERVAL) {
                coopSession->retryJoin();
                _attemptedAt = now;
                _retrying = true;
            }
            return FsmAction::none();
        }

        if (!_failedAt)
            _failedAt = now;
        drawMessage(fmt::format("Could not join co-op game. {}.", client->failureReason()));
        if (now - *_failedAt < FAILURE_MESSAGE_DURATION)
            return FsmAction::none();
        coopSession.reset();
        return FsmAction::transition("failed");
    }
    }
}

void CoopJoinState::exit() {
    assets->releaseImage(_background);
    _background = nullptr;
}

bool CoopJoinState::isJoinPending() {
    return coopSession && coopSession->client() && coopSession->client()->state() == COOP_CLIENT_CONNECTING;
}

void CoopJoinState::drawMessage(std::string_view message) {
    render->DrawQuad2D(_background, {0, 0});
    Recti window(120, 200, 400, 0);
    GUIWindow::DrawMessageBox(window, std::string(message));
}
