#include "CoopServer.h"

#include <cassert>
#include <exception>
#include <string>
#include <utility>
#include <variant>

#include "Library/Logger/Logger.h"

#include "Utility/String/Format.h"

CoopServer::CoopServer(int port, int maxClients, std::function<Blob()> saveGameProvider, std::string hostName) :
    _net(NetHost::listen(port, maxClients)),
    _saveGameProvider(std::move(saveGameProvider)),
    _hostName(std::move(hostName)) {
    assert(_saveGameProvider);
}

CoopServer::~CoopServer() = default;

int CoopServer::port() const {
    return _net->port();
}

void CoopServer::update() {
    while (std::optional<NetEvent> event = _net->poll()) {
        switch (event->type) {
        case NET_EVENT_CONNECTED:
            _clients[event->peer] = Client();
            break;
        case NET_EVENT_DISCONNECTED:
            handleDisconnect(event->peer);
            break;
        case NET_EVENT_MESSAGE:
            try {
                handleMessage(event->peer, decodeCoopMessage(event->data));
            } catch (const std::exception &e) {
                MM_WARNING("Disconnecting co-op client that sent a malformed message: {}", e.what());
                _net->disconnect(event->peer);
            }
            break;
        }
    }
}

void CoopServer::flush() {
    _net->flush();
}

void CoopServer::sendLocalPose(const CoopPartyPose &pose) {
    _localPose = pose;
    _localPose->playerId = HOST_PLAYER_ID;
    sendToJoined(NET_CHANNEL_UNRELIABLE, *_localPose);
}

void CoopServer::sendActorStates(const CoopActorStates &states) {
    sendToJoined(NET_CHANNEL_UNRELIABLE, states);
}

void CoopServer::requestTravel(const MapDestination &destination) {
    startVote(HOST_PLAYER_ID, -1, destination);
}

void CoopServer::answerTravel(bool accepted) {
    if (_hostVote)
        recordAnswer(HOST_PLAYER_ID, accepted);
}

std::optional<CoopTravelDecision> CoopServer::takeTravelDecision() {
    return std::exchange(_hostDecision, std::nullopt);
}

void CoopServer::handleMessage(NetPeerId peer, CoopMessage message) {
    if (!std::holds_alternative<CoopHello>(message) && _clients[peer].playerId == -1) {
        MM_WARNING("Disconnecting co-op client that sent a message before its hello");
        _net->disconnect(peer);
        return;
    }

    if (const CoopHello *hello = std::get_if<CoopHello>(&message)) {
        handleHello(peer, *hello);
    } else if (CoopPartyPose *pose = std::get_if<CoopPartyPose>(&message)) {
        handlePartyPose(peer, std::move(*pose));
    } else if (const CoopTravelRequest *request = std::get_if<CoopTravelRequest>(&message)) {
        startVote(_clients[peer].playerId, peer, request->destination);
    } else if (const CoopTravelAnswer *answer = std::get_if<CoopTravelAnswer>(&message)) {
        if (_vote && _vote->id == answer->voteId) // Answers to a vote that has already ended are dropped.
            recordAnswer(_clients[peer].playerId, answer->accepted);
    } else {
        MM_WARNING("Disconnecting co-op client that sent a host-only message");
        _net->disconnect(peer);
    }
}

void CoopServer::handleHello(NetPeerId peer, const CoopHello &hello) {
    Client &client = _clients[peer];
    if (client.playerId != -1) {
        MM_WARNING("Disconnecting co-op client '{}' that sent a second hello", client.playerName);
        _net->disconnect(peer);
        return;
    }

    if (hello.protocolVersion != COOP_PROTOCOL_VERSION) {
        reject(peer, CoopReject{.reason = fmt::format("Protocol version mismatch, host has {} and client has {}", COOP_PROTOCOL_VERSION, hello.protocolVersion)});
        return;
    }

    Blob saveGame = _saveGameProvider();
    if (!saveGame) {
        reject(peer, CoopReject{.reason = "The host hasn't started a game yet", .retryLater = true});
        return;
    }

    client.playerId = _nextPlayerId++;
    client.playerName = hello.playerName;
    MM_INFO("Co-op player '{}' joined as player {}", client.playerName, client.playerId);

    CoopWelcome welcome;
    welcome.playerId = client.playerId;
    welcome.saveGame = std::move(saveGame);
    send(peer, NET_CHANNEL_RELIABLE, std::move(welcome));

    // Reliable, so that the newcomer sees everyone on its first frame in the game.
    if (_localPose)
        send(peer, NET_CHANNEL_RELIABLE, *_localPose);
    for (const auto &[playerId, pose] : _remoteParties)
        send(peer, NET_CHANNEL_RELIABLE, pose);
}

void CoopServer::handlePartyPose(NetPeerId peer, CoopPartyPose pose) {
    const Client &client = _clients[peer];
    pose.playerId = client.playerId;
    _remoteParties[client.playerId] = pose;
    sendToJoined(NET_CHANNEL_UNRELIABLE, pose, peer);
}

void CoopServer::handleDisconnect(NetPeerId peer) {
    auto pos = _clients.find(peer);
    if (pos == _clients.end())
        return;

    int playerId = pos->second.playerId;
    std::string playerName = std::move(pos->second.playerName);
    _clients.erase(pos);
    if (playerId == -1)
        return;

    MM_INFO("Co-op player '{}' left", playerName);
    _remoteParties.erase(playerId);

    CoopPlayerLeft left;
    left.playerId = playerId;
    sendToJoined(NET_CHANNEL_RELIABLE, left);

    if (_vote) {
        _vote->waitingFor.erase(playerId);
        if (_vote->waitingFor.empty())
            finishVote(true, {});
    }
}

void CoopServer::startVote(int proposerId, NetPeerId proposerPeer, const MapDestination &destination) {
    if (_vote) {
        CoopTravelDecision turnedDown;
        turnedDown.destination = destination;
        if (proposerPeer == -1) {
            _hostDecision = std::move(turnedDown);
        } else {
            send(proposerPeer, NET_CHANNEL_RELIABLE, std::move(turnedDown));
        }
        return;
    }

    Vote vote;
    vote.id = _nextVoteId++;
    vote.destination = destination;
    if (proposerId != HOST_PLAYER_ID)
        vote.waitingFor.insert(HOST_PLAYER_ID);
    for (const auto &[peer, client] : _clients)
        if (client.playerId != -1 && client.playerId != proposerId)
            vote.waitingFor.insert(client.playerId);
    _vote = vote;

    CoopTravelVote message;
    message.voteId = vote.id;
    message.proposerName = playerName(proposerId);
    message.destination = destination;
    if (vote.waitingFor.contains(HOST_PLAYER_ID))
        _hostVote = message;
    for (const auto &[peer, client] : _clients)
        if (vote.waitingFor.contains(client.playerId))
            send(peer, NET_CHANNEL_RELIABLE, message);

    if (vote.waitingFor.empty())
        finishVote(true, {});
}

void CoopServer::recordAnswer(int playerId, bool accepted) {
    assert(_vote);
    if (!_vote->waitingFor.contains(playerId))
        return;

    if (!accepted) {
        finishVote(false, playerName(playerId));
        return;
    }

    _vote->waitingFor.erase(playerId);
    if (playerId == HOST_PLAYER_ID)
        _hostVote.reset();
    if (_vote->waitingFor.empty())
        finishVote(true, {});
}

void CoopServer::finishVote(bool approved, std::string declinedBy) {
    assert(_vote);

    CoopTravelDecision decision;
    decision.voteId = _vote->id;
    decision.approved = approved;
    decision.declinedBy = std::move(declinedBy);
    decision.destination = _vote->destination;
    _vote.reset();
    _hostVote.reset();

    sendToJoined(NET_CHANNEL_RELIABLE, decision);
    _hostDecision = std::move(decision);
}

std::string CoopServer::playerName(int playerId) const {
    if (playerId == HOST_PLAYER_ID)
        return _hostName;
    for (const auto &[peer, client] : _clients)
        if (client.playerId == playerId)
            return client.playerName;
    return {};
}

void CoopServer::reject(NetPeerId peer, CoopReject message) {
    MM_INFO("Rejecting co-op client: {}", message.reason);

    send(peer, NET_CHANNEL_RELIABLE, std::move(message));
    _net->disconnect(peer);
}

void CoopServer::send(NetPeerId peer, NetChannel channel, CoopMessage message) {
    _net->send(peer, channel, encodeCoopMessage(message));
}

void CoopServer::sendToJoined(NetChannel channel, CoopMessage message, NetPeerId except) {
    Blob data = encodeCoopMessage(message);
    for (const auto &[peer, client] : _clients)
        if (client.playerId != -1 && peer != except)
            _net->send(peer, channel, data);
}
