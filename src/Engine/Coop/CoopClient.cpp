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

void CoopClient::setReady(bool ready) {
    if (_state != COOP_CLIENT_IN_LOBBY)
        return;

    _net->send(_host, NET_CHANNEL_RELIABLE, encodeCoopMessage(CoopLobbyReady{ready}));
}

void CoopClient::sendLocalPose(const CoopPartyPose &pose) {
    if (_state != COOP_CLIENT_JOINED)
        return;

    CoopPartyPose message = pose;
    message.playerId = -1;
    _net->send(_host, NET_CHANNEL_UNRELIABLE, encodeCoopMessage(message));
}

void CoopClient::requestVote(CoopVoteKind kind, const MapDestination &destination) {
    if (_state != COOP_CLIENT_JOINED)
        return;

    _net->send(_host, NET_CHANNEL_RELIABLE, encodeCoopMessage(CoopVoteRequest{kind, destination}));
}

void CoopClient::answerVote(bool accepted) {
    if (!_pendingVote)
        return;

    CoopVoteAnswer answer;
    answer.voteId = _pendingVote->voteId;
    answer.accepted = accepted;
    _net->send(_host, NET_CHANNEL_RELIABLE, encodeCoopMessage(answer));
    _pendingVote.reset();
}

std::optional<CoopVoteDecision> CoopClient::takeVoteDecision() {
    return std::exchange(_voteDecision, std::nullopt);
}

void CoopClient::sendActorDamage(const CoopActorDamage &damage) {
    if (_state != COOP_CLIENT_JOINED)
        return;

    CoopActorDamage message = damage;
    message.playerId = -1;
    _net->send(_host, NET_CHANNEL_RELIABLE, encodeCoopMessage(message));
}

void CoopClient::sendQuestBit(const CoopQuestBit &change) {
    if (_state != COOP_CLIENT_JOINED)
        return;

    CoopQuestBit message = change;
    message.playerId = -1;
    _net->send(_host, NET_CHANNEL_RELIABLE, encodeCoopMessage(message));
}

std::vector<CoopQuestBit> CoopClient::takeQuestBits() {
    return std::exchange(_questBits, {});
}

void CoopClient::sendQuestRewards(const CoopQuestRewards &rewards) {
    if (_state != COOP_CLIENT_JOINED)
        return;

    CoopQuestRewards message = rewards;
    message.playerId = -1;
    _net->send(_host, NET_CHANNEL_RELIABLE, encodeCoopMessage(message));
}

std::vector<CoopQuestRewards> CoopClient::takeQuestRewards() {
    return std::exchange(_questRewards, {});
}

void CoopClient::sendChest(const CoopChest &chest) {
    if (_state != COOP_CLIENT_JOINED)
        return;

    CoopChest message = chest;
    message.playerId = -1;
    _net->send(_host, NET_CHANNEL_RELIABLE, encodeCoopMessage(message));
}

std::vector<CoopChest> CoopClient::takeChests() {
    return std::exchange(_chests, {});
}

void CoopClient::sendGroundItemTaken(const CoopGroundItemTaken &taken) {
    if (_state != COOP_CLIENT_JOINED)
        return;

    CoopGroundItemTaken message = taken;
    message.playerId = -1;
    _net->send(_host, NET_CHANNEL_RELIABLE, encodeCoopMessage(message));
}

std::vector<CoopGroundItemTaken> CoopClient::takeGroundItemsTaken() {
    return std::exchange(_groundItemsTaken, {});
}

std::optional<CoopClock> CoopClient::takeClock() {
    return std::exchange(_clock, std::nullopt);
}

void CoopClient::sendActorBuff(const CoopActorBuff &buff) {
    if (_state != COOP_CLIENT_JOINED)
        return;

    CoopActorBuff message = buff;
    message.playerId = -1;
    _net->send(_host, NET_CHANNEL_RELIABLE, encodeCoopMessage(message));
}

void CoopClient::sendItemDropped(const CoopItemDropped &dropped) {
    if (_state != COOP_CLIENT_JOINED)
        return;

    CoopItemDropped message = dropped;
    message.playerId = -1;
    _net->send(_host, NET_CHANNEL_RELIABLE, encodeCoopMessage(message));
}

std::vector<CoopItemDropped> CoopClient::takeItemsDropped() {
    return std::exchange(_itemsDropped, {});
}

void CoopClient::sendSpriteLaunched(const CoopSpriteLaunched &launched) {
    if (_state != COOP_CLIENT_JOINED)
        return;

    CoopSpriteLaunched message = launched;
    message.playerId = -1;
    _net->send(_host, NET_CHANNEL_RELIABLE, encodeCoopMessage(message));
}

std::vector<CoopSpriteLaunched> CoopClient::takeSpritesLaunched() {
    return std::exchange(_spritesLaunched, {});
}

void CoopClient::sendLootRequest(const CoopLootRequest &request) {
    if (_state != COOP_CLIENT_JOINED)
        return;

    CoopLootRequest message = request;
    message.playerId = -1;
    _net->send(_host, NET_CHANNEL_RELIABLE, encodeCoopMessage(message));
}

std::vector<CoopLoot> CoopClient::takeLoot() {
    return std::exchange(_loot, {});
}

std::vector<CoopPartyHit> CoopClient::takePartyHits() {
    return std::exchange(_partyHits, {});
}

std::vector<CoopActorStates> CoopClient::takeActorStates() {
    return std::exchange(_actorStates, {});
}

void CoopClient::handleMessage(CoopMessage message) {
    if (CoopWelcome *welcome = std::get_if<CoopWelcome>(&message)) {
        if (_state != COOP_CLIENT_CONNECTING && _state != COOP_CLIENT_IN_LOBBY) {
            fail("Host sent a second welcome");
            return;
        }
        _state = COOP_CLIENT_JOINED;
        _playerId = welcome->playerId;
        _saveGame = std::move(welcome->saveGame);
        _lobbyPlayers.clear();
        MM_INFO("Joined co-op game as player {}", _playerId);
    } else if (const CoopLobbyJoined *joined = std::get_if<CoopLobbyJoined>(&message)) {
        if (_state != COOP_CLIENT_CONNECTING) {
            fail("Host let us into the lobby twice");
            return;
        }
        _state = COOP_CLIENT_IN_LOBBY;
        _playerId = joined->playerId;
        MM_INFO("Joined co-op lobby as player {}", _playerId);
    } else if (CoopLobbyStatus *status = std::get_if<CoopLobbyStatus>(&message)) {
        _lobbyPlayers = std::move(status->players);
    } else if (CoopReject *reject = std::get_if<CoopReject>(&message)) {
        _canRetry = reject->retryLater;
        fail(fmt::format("Host refused to let us in: {}", reject->reason));
    } else if (const CoopPartyPose *pose = std::get_if<CoopPartyPose>(&message)) {
        _remoteParties[pose->playerId] = *pose;
    } else if (const CoopPlayerLeft *left = std::get_if<CoopPlayerLeft>(&message)) {
        _remoteParties.erase(left->playerId);
    } else if (CoopVote *vote = std::get_if<CoopVote>(&message)) {
        _pendingVote = std::move(*vote);
    } else if (const CoopQuestBit *change = std::get_if<CoopQuestBit>(&message)) {
        _questBits.push_back(*change);
    } else if (CoopQuestRewards *rewards = std::get_if<CoopQuestRewards>(&message)) {
        _questRewards.push_back(std::move(*rewards));
    } else if (CoopChest *chest = std::get_if<CoopChest>(&message)) {
        _chests.push_back(std::move(*chest));
    } else if (const CoopGroundItemTaken *taken = std::get_if<CoopGroundItemTaken>(&message)) {
        _groundItemsTaken.push_back(*taken);
    } else if (const CoopClock *clock = std::get_if<CoopClock>(&message)) {
        _clock = *clock;
    } else if (CoopItemDropped *dropped = std::get_if<CoopItemDropped>(&message)) {
        _itemsDropped.push_back(std::move(*dropped));
    } else if (CoopSpriteLaunched *launched = std::get_if<CoopSpriteLaunched>(&message)) {
        _spritesLaunched.push_back(std::move(*launched));
    } else if (CoopLoot *loot = std::get_if<CoopLoot>(&message)) {
        _loot.push_back(std::move(*loot));
    } else if (const CoopPartyHit *hit = std::get_if<CoopPartyHit>(&message)) {
        _partyHits.push_back(*hit);
    } else if (CoopActorStates *states = std::get_if<CoopActorStates>(&message)) {
        _actorStates.push_back(std::move(*states));
    } else if (CoopVoteDecision *decision = std::get_if<CoopVoteDecision>(&message)) {
        if (_pendingVote && _pendingVote->voteId == decision->voteId)
            _pendingVote.reset(); // Someone else said no before this player answered.
        _voteDecision = std::move(*decision);
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
