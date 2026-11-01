#include "CoopServer.h"

#include <algorithm>
#include <cassert>
#include <exception>
#include <string>
#include <utility>
#include <variant>
#include <vector>

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

int CoopServer::joinedClientCount() const {
    return std::ranges::count_if(_clients, [](const auto &entry) { return entry.second.playerId != -1; });
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

void CoopServer::sendPartyHit(int playerId, const CoopPartyHit &hit) {
    for (const auto &[peer, client] : _clients)
        if (client.playerId == playerId)
            send(peer, NET_CHANNEL_RELIABLE, hit);
}

void CoopServer::sendQuestBit(const CoopQuestBit &change) {
    Blob data = encodeCoopMessage(change);
    for (const auto &[peer, client] : _clients)
        if (client.playerId != -1 && client.playerId != change.playerId)
            _net->send(peer, NET_CHANNEL_RELIABLE, data);
}

std::vector<CoopQuestBit> CoopServer::takeQuestBits() {
    return std::exchange(_questBits, {});
}

void CoopServer::sendQuestRewards(const CoopQuestRewards &rewards) {
    Blob data = encodeCoopMessage(rewards);
    for (const auto &[peer, client] : _clients)
        if (client.playerId != -1 && client.playerId != rewards.playerId)
            _net->send(peer, NET_CHANNEL_RELIABLE, data);
}

std::vector<CoopQuestRewards> CoopServer::takeQuestRewards() {
    return std::exchange(_questRewards, {});
}

void CoopServer::sendChest(const CoopChest &chest) {
    Blob data = encodeCoopMessage(chest);
    for (const auto &[peer, client] : _clients)
        if (client.playerId != -1 && client.playerId != chest.playerId)
            _net->send(peer, NET_CHANNEL_RELIABLE, data);
}

std::vector<CoopChest> CoopServer::takeChests() {
    return std::exchange(_chests, {});
}

void CoopServer::sendGroundItemTaken(const CoopGroundItemTaken &taken) {
    Blob data = encodeCoopMessage(taken);
    for (const auto &[peer, client] : _clients)
        if (client.playerId != -1 && client.playerId != taken.playerId)
            _net->send(peer, NET_CHANNEL_RELIABLE, data);
}

std::vector<CoopGroundItemTaken> CoopServer::takeGroundItemsTaken() {
    return std::exchange(_groundItemsTaken, {});
}

void CoopServer::sendClock(const CoopClock &clock) {
    sendToJoined(NET_CHANNEL_UNRELIABLE, clock);
}

std::vector<CoopActorBuff> CoopServer::takeActorBuffs() {
    return std::exchange(_actorBuffs, {});
}

void CoopServer::sendItemDropped(const CoopItemDropped &dropped) {
    Blob data = encodeCoopMessage(dropped);
    for (const auto &[peer, client] : _clients)
        if (client.playerId != -1 && client.playerId != dropped.playerId)
            _net->send(peer, NET_CHANNEL_RELIABLE, data);
}

std::vector<CoopItemDropped> CoopServer::takeItemsDropped() {
    return std::exchange(_itemsDropped, {});
}

void CoopServer::sendLoot(int playerId, const CoopLoot &loot) {
    for (const auto &[peer, client] : _clients)
        if (client.playerId == playerId)
            send(peer, NET_CHANNEL_RELIABLE, loot);
}

std::vector<CoopLootRequest> CoopServer::takeLootRequests() {
    return std::exchange(_lootRequests, {});
}

std::vector<CoopActorDamage> CoopServer::takeActorDamage() {
    return std::exchange(_actorDamage, {});
}

void CoopServer::requestVote(CoopVoteKind kind, const MapDestination &destination) {
    startVote(HOST_PLAYER_ID, -1, kind, destination);
}

void CoopServer::answerVote(bool accepted) {
    if (_hostVote)
        recordAnswer(HOST_PLAYER_ID, accepted);
}

std::optional<CoopVoteDecision> CoopServer::takeVoteDecision() {
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
    } else if (const CoopVoteRequest *request = std::get_if<CoopVoteRequest>(&message)) {
        startVote(_clients[peer].playerId, peer, request->kind, request->destination);
    } else if (CoopActorDamage *damage = std::get_if<CoopActorDamage>(&message)) {
        damage->playerId = _clients[peer].playerId;
        _actorDamage.push_back(std::move(*damage));
    } else if (CoopQuestBit *change = std::get_if<CoopQuestBit>(&message)) {
        change->playerId = _clients[peer].playerId;
        _questBits.push_back(*change);
    } else if (CoopQuestRewards *rewards = std::get_if<CoopQuestRewards>(&message)) {
        rewards->playerId = _clients[peer].playerId;
        _questRewards.push_back(std::move(*rewards));
    } else if (CoopChest *chest = std::get_if<CoopChest>(&message)) {
        chest->playerId = _clients[peer].playerId;
        _chests.push_back(std::move(*chest));
    } else if (CoopGroundItemTaken *taken = std::get_if<CoopGroundItemTaken>(&message)) {
        taken->playerId = _clients[peer].playerId;
        _groundItemsTaken.push_back(*taken);
    } else if (CoopActorBuff *buff = std::get_if<CoopActorBuff>(&message)) {
        buff->playerId = _clients[peer].playerId;
        _actorBuffs.push_back(*buff);
    } else if (CoopItemDropped *dropped = std::get_if<CoopItemDropped>(&message)) {
        dropped->playerId = _clients[peer].playerId;
        _itemsDropped.push_back(std::move(*dropped));
    } else if (CoopLootRequest *request = std::get_if<CoopLootRequest>(&message)) {
        request->playerId = _clients[peer].playerId;
        _lootRequests.push_back(std::move(*request));
    } else if (const CoopVoteAnswer *answer = std::get_if<CoopVoteAnswer>(&message)) {
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

void CoopServer::startVote(int proposerId, NetPeerId proposerPeer, CoopVoteKind kind, const MapDestination &destination) {
    if (_vote) {
        CoopVoteDecision turnedDown;
        turnedDown.kind = kind;
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
    vote.kind = kind;
    vote.destination = destination;
    if (proposerId != HOST_PLAYER_ID)
        vote.waitingFor.insert(HOST_PLAYER_ID);
    for (const auto &[peer, client] : _clients)
        if (client.playerId != -1 && client.playerId != proposerId)
            vote.waitingFor.insert(client.playerId);
    _vote = vote;

    CoopVote message;
    message.voteId = vote.id;
    message.kind = kind;
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

    CoopVoteDecision decision;
    decision.voteId = _vote->id;
    decision.kind = _vote->kind;
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
