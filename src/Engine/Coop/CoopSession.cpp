#include "CoopSession.h"

#include <cassert>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "Engine/Engine.h"
#include "Engine/Party.h"
#include "Engine/SaveLoad.h"
#include "Engine/Timer.h"
#include "Engine/Tables/MapTable.h"

#include "GUI/UI/UIStatusBar.h"

#include "Utility/String/Format.h"

#include "CoopClient.h"
#include "CoopGuestSave.h"
#include "CoopServer.h"

static constexpr std::chrono::seconds POSE_HEARTBEAT{1}; // Makes up for an unreliable pose that got lost while the party stands still.

std::unique_ptr<CoopSession> coopSession;

CoopSession::CoopSession() = default;

CoopSession::~CoopSession() {
    engine->_remoteParties.clear();
}

std::unique_ptr<CoopSession> CoopSession::host(int port, int maxClients) {
    std::unique_ptr<CoopSession> result(new CoopSession());
    result->_server = std::make_unique<CoopServer>(port, maxClients, [] { return createSaveData(false, "Co-op").second; });
    return result;
}

std::unique_ptr<CoopSession> CoopSession::join(std::string_view address, int port, std::string_view playerName, Blob guestSave) {
    std::unique_ptr<CoopSession> result(new CoopSession());
    result->_client = std::make_unique<CoopClient>(address, port, playerName);
    result->_guestSave = std::move(guestSave);
    return result;
}

Blob CoopSession::takeJoinedSave() {
    assert(_client);
    Blob hostSave = _client->takeSaveGame();
    if (!hostSave || !_guestSave)
        return hostSave;
    return makeCoopGuestSave(hostSave, std::exchange(_guestSave, Blob()));
}

void CoopSession::update() {
    if (_server) {
        _server->update();
    } else {
        _client->update();
    }

    CoopPartyPose pose = localPose();
    auto now = std::chrono::steady_clock::now();
    if (pose != _lastSentPose || now - _lastSendTime >= POSE_HEARTBEAT) {
        sendLocalPose(pose);
        _lastSentPose = pose;
        _lastSendTime = now;
    }

    std::optional<CoopTravelDecision> decision = _server ? _server->takeTravelDecision() : _client->takeTravelDecision();
    if (decision)
        handleTravelDecision(*decision);

    engine->_remoteParties.clear();
    for (const auto &[playerId, remote] : remoteParties())
        if (remote.map == engine->_currentLoadedMapId)
            engine->_remoteParties.push_back(RemoteParty{remote.pos, remote.yaw, remote.walking});
}

bool CoopSession::interceptMapChange() {
    if (std::exchange(_travelApproved, false))
        return false;

    MapId map = engine->_pendingTransition ? engine->_pendingTransition->map() : MAP_INVALID;
    if (map == MAP_INVALID || map == engine->_currentLoadedMapId)
        return false; // Moving around on the current map doesn't split the group.

    MapDestination destination = *std::exchange(engine->_pendingTransition, std::nullopt);
    gameTimer->setPaused(false); // Travel UIs pause the game, and the party now stays where it is.
    if (_server) {
        _server->requestTravel(destination);
    } else {
        _client->requestTravel(destination);
    }
    engine->_statusBar->setEvent(fmt::format("Asked the others to travel to {}", pMapTable->pInfos[map].name));
    return true;
}

const std::optional<CoopTravelVote> &CoopSession::pendingVote() const {
    return _server ? _server->pendingVote() : _client->pendingVote();
}

void CoopSession::answerTravel(bool accepted) {
    if (_server) {
        _server->answerTravel(accepted);
    } else {
        _client->answerTravel(accepted);
    }
}

void CoopSession::handleTravelDecision(const CoopTravelDecision &decision) {
    std::string mapName = pMapTable->pInfos[decision.destination.map()].name;
    if (decision.approved) {
        _travelApproved = true;
        startMapTransition(decision.destination);
    } else if (decision.voteId == -1) {
        engine->_statusBar->setEvent(fmt::format("Can't travel to {} while another trip is being voted on", mapName));
    } else {
        engine->_statusBar->setEvent(fmt::format("{} doesn't want to travel to {}", decision.declinedBy, mapName));
    }
}

CoopPartyPose CoopSession::localPose() {
    CoopPartyPose result;
    result.map = engine->_currentLoadedMapId;
    result.pos = pParty->pos;
    result.yaw = pParty->_viewYaw;
    result.pitch = pParty->_viewPitch;
    result.walking = _lastFramePos && _lastFramePos->xy() != pParty->pos.xy();
    _lastFramePos = pParty->pos;
    return result;
}

const std::map<int, CoopPartyPose> &CoopSession::remoteParties() const {
    return _server ? _server->remoteParties() : _client->remoteParties();
}

void CoopSession::sendLocalPose(const CoopPartyPose &pose) {
    if (_server) {
        _server->sendLocalPose(pose);
    } else {
        _client->sendLocalPose(pose);
    }
}
