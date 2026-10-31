#include "CoopClient.h"

#include <exception>
#include <string>
#include <utility>
#include <vector>

#include "Library/Logger/Logger.h"

#include "Utility/String/Format.h"

CoopClient::CoopClient(std::string_view address, int port, std::string_view playerName) :
    _net(NetHost::connect(address, port)),
    _playerName(playerName) {}

CoopClient::~CoopClient() = default;

void CoopClient::update() {
    while (_state != COOP_CLIENT_FAILED) {
        std::optional<NetEvent> event = _net->poll();
        if (!event)
            break;

        switch (event->type) {
        case NET_EVENT_CONNECTED: {
            _host = event->peer;
            CoopHello hello;
            hello.playerName = _playerName;
            _net->send(_host, NET_CHANNEL_RELIABLE, encodeCoopMessage(std::move(hello)));
            break;
        }
        case NET_EVENT_DISCONNECTED:
            _canRetry = _host == -1;
            fail(_host == -1 ? "Could not connect to the host" : "Disconnected from the host");
            break;
        case NET_EVENT_MESSAGE:
            try {
                handleMessage(decodeCoopMessage(event->data));
            } catch (const std::exception &e) {
                fail(fmt::format("Host sent a malformed message: {}", e.what()));
            }
            break;
        }
    }
}

void CoopClient::flush() {
    _net->flush();
}

Blob CoopClient::takeSaveGame() {
    return std::move(_saveGame);
}

void CoopClient::sendLocalPose(const CoopPartyPose &pose) {
    if (_state != COOP_CLIENT_JOINED)
        return;

    CoopPartyPose message = pose;
    message.playerId = -1;
    _net->send(_host, NET_CHANNEL_UNRELIABLE, encodeCoopMessage(message));
}

void CoopClient::requestTravel(const MapDestination &destination) {
    if (_state != COOP_CLIENT_JOINED)
        return;

    _net->send(_host, NET_CHANNEL_RELIABLE, encodeCoopMessage(CoopTravelRequest{destination}));
}

void CoopClient::answerTravel(bool accepted) {
    if (!_pendingVote)
        return;

    CoopTravelAnswer answer;
    answer.voteId = _pendingVote->voteId;
    answer.accepted = accepted;
    _net->send(_host, NET_CHANNEL_RELIABLE, encodeCoopMessage(answer));
    _pendingVote.reset();
}

std::optional<CoopTravelDecision> CoopClient::takeTravelDecision() {
    return std::exchange(_travelDecision, std::nullopt);
}

void CoopClient::sendActorDamage(const CoopActorDamage &damage) {
    if (_state != COOP_CLIENT_JOINED)
        return;

    _net->send(_host, NET_CHANNEL_RELIABLE, encodeCoopMessage(damage));
}

std::vector<CoopActorStates> CoopClient::takeActorStates() {
    return std::exchange(_actorStates, {});
}

void CoopClient::handleMessage(CoopMessage message) {
    if (CoopWelcome *welcome = std::get_if<CoopWelcome>(&message)) {
        if (_state != COOP_CLIENT_CONNECTING) {
            fail("Host sent a second welcome");
            return;
        }
        _state = COOP_CLIENT_JOINED;
        _playerId = welcome->playerId;
        _saveGame = std::move(welcome->saveGame);
        MM_INFO("Joined co-op game as player {}", _playerId);
    } else if (CoopReject *reject = std::get_if<CoopReject>(&message)) {
        _canRetry = reject->retryLater;
        fail(fmt::format("Host refused to let us in: {}", reject->reason));
    } else if (const CoopPartyPose *pose = std::get_if<CoopPartyPose>(&message)) {
        _remoteParties[pose->playerId] = *pose;
    } else if (const CoopPlayerLeft *left = std::get_if<CoopPlayerLeft>(&message)) {
        _remoteParties.erase(left->playerId);
    } else if (CoopTravelVote *vote = std::get_if<CoopTravelVote>(&message)) {
        _pendingVote = std::move(*vote);
    } else if (CoopActorStates *states = std::get_if<CoopActorStates>(&message)) {
        _actorStates.push_back(std::move(*states));
    } else if (CoopTravelDecision *decision = std::get_if<CoopTravelDecision>(&message)) {
        if (_pendingVote && _pendingVote->voteId == decision->voteId)
            _pendingVote.reset(); // Someone else said no before this player answered.
        _travelDecision = std::move(*decision);
    } else {
        fail("Host sent a client-only message");
    }
}

void CoopClient::fail(std::string reason) {
    MM_WARNING("Co-op session failed: {}", reason);
    _state = COOP_CLIENT_FAILED;
    _failureReason = std::move(reason);
    _remoteParties.clear();
    if (_host != -1)
        _net->disconnect(_host);
}
