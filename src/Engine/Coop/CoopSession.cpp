#include "CoopSession.h"

#include <cassert>
#include <map>
#include <memory>
#include <string_view>
#include <utility>

#include "Engine/Engine.h"
#include "Engine/Party.h"
#include "Engine/SaveLoad.h"

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

    engine->_remoteParties.clear();
    for (const auto &[playerId, remote] : remoteParties())
        if (remote.map == engine->_currentLoadedMapId)
            engine->_remoteParties.push_back(RemoteParty{remote.pos, remote.yaw, remote.walking});
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
