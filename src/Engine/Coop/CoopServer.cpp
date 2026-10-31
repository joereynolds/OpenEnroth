#include "CoopServer.h"

#include <cassert>
#include <exception>
#include <string>
#include <utility>

#include "Library/Logger/Logger.h"

#include "Utility/String/Format.h"

CoopServer::CoopServer(int port, int maxClients, std::function<Blob()> saveGameProvider) :
    _net(NetHost::listen(port, maxClients)),
    _saveGameProvider(std::move(saveGameProvider)) {
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

void CoopServer::sendLocalPose(const CoopPartyPose &pose) {
    _localPose = pose;
    _localPose->playerId = HOST_PLAYER_ID;
    sendToJoined(NET_CHANNEL_UNRELIABLE, *_localPose);
}

void CoopServer::handleMessage(NetPeerId peer, CoopMessage message) {
    if (const CoopHello *hello = std::get_if<CoopHello>(&message)) {
        handleHello(peer, *hello);
    } else if (CoopPartyPose *pose = std::get_if<CoopPartyPose>(&message)) {
        handlePartyPose(peer, std::move(*pose));
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
        reject(peer, fmt::format("Protocol version mismatch, host has {} and client has {}", COOP_PROTOCOL_VERSION, hello.protocolVersion));
        return;
    }

    client.playerId = _nextPlayerId++;
    client.playerName = hello.playerName;
    MM_INFO("Co-op player '{}' joined as player {}", client.playerName, client.playerId);

    CoopWelcome welcome;
    welcome.playerId = client.playerId;
    welcome.saveGame = _saveGameProvider();
    send(peer, NET_CHANNEL_RELIABLE, std::move(welcome));

    // Reliable, so that the newcomer sees everyone on its first frame in the game.
    if (_localPose)
        send(peer, NET_CHANNEL_RELIABLE, *_localPose);
    for (const auto &[playerId, pose] : _remoteParties)
        send(peer, NET_CHANNEL_RELIABLE, pose);
}

void CoopServer::handlePartyPose(NetPeerId peer, CoopPartyPose pose) {
    const Client &client = _clients[peer];
    if (client.playerId == -1) {
        MM_WARNING("Disconnecting co-op client that sent a pose before its hello");
        _net->disconnect(peer);
        return;
    }

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
}

void CoopServer::reject(NetPeerId peer, std::string reason) {
    MM_INFO("Rejecting co-op client: {}", reason);

    CoopReject message;
    message.reason = std::move(reason);
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
