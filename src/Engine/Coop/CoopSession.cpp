#include "CoopSession.h"

#include <algorithm>
#include <cassert>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Engine/Engine.h"
#include "Engine/Party.h"
#include "Engine/SaveLoad.h"
#include "Engine/Timer.h"
#include "Engine/Graphics/Indoor.h"
#include "Engine/Objects/Actor.h"
#include "Engine/Objects/Character.h"
#include "Engine/Objects/MonsterEnumFunctions.h"
#include "Engine/Objects/Monsters.h"
#include "Engine/Tables/MapTable.h"

#include "GUI/GUIWindow.h"
#include "GUI/UI/UIStatusBar.h"

#include "Utility/String/Format.h"

#include "CoopClient.h"
#include "CoopGuestSave.h"
#include "CoopServer.h"

static constexpr std::chrono::seconds POSE_HEARTBEAT{1}; // Makes up for an unreliable pose that got lost while the party stands still.
static constexpr std::chrono::seconds FULL_ACTOR_UPDATE_INTERVAL{1}; // Makes up for lost unreliable monster updates.

std::unique_ptr<CoopSession> coopSession;

CoopSession::CoopSession() = default;

CoopSession::~CoopSession() {
    engine->_remoteParties.clear();
    engine->_actorsFollowHost = false;
    engine->_actorHitsForHost.clear();
    engine->_standInHits.clear();
}

std::unique_ptr<CoopSession> CoopSession::host(int port, int maxClients) {
    std::unique_ptr<CoopSession> result(new CoopSession());
    result->_server = std::make_unique<CoopServer>(port, maxClients, [] {
        if (GetCurrentMenuID() != MENU_NONE)
            return Blob(); // Main menu or party creation, there's no world to hand out.
        return createSaveData(false, "Co-op").second;
    });
    return result;
}

std::unique_ptr<CoopSession> CoopSession::join(std::string_view address, int port, std::string_view playerName, Blob guestSave) {
    std::unique_ptr<CoopSession> result(new CoopSession());
    result->_client = std::make_unique<CoopClient>(address, port, playerName);
    result->_joinAddress = address;
    result->_joinPort = port;
    result->_playerName = playerName;
    result->_guestSave = std::move(guestSave);
    return result;
}

void CoopSession::retryJoin() {
    assert(_client);
    _client = std::make_unique<CoopClient>(_joinAddress, _joinPort, _playerName);
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

    if (_server) {
        applyActorDamage();
        sendStandInHits();
        updateStandIns();
        sendActorStates();
    } else {
        sendActorHits();
        applyActorStates();
        applyPartyHits();
    }

    std::optional<CoopTravelDecision> decision = _server ? _server->takeTravelDecision() : _client->takeTravelDecision();
    if (decision)
        handleTravelDecision(*decision);

    engine->_remoteParties.clear();
    for (const auto &[playerId, remote] : remoteParties())
        if (remote.map == engine->_currentLoadedMapId)
            engine->_remoteParties.push_back(RemoteParty{remote.pos, remote.yaw, remote.walking});

    if (_server) {
        _server->flush();
    } else {
        _client->flush();
    }
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

static CoopActorState actorState(int index, const Actor &actor) {
    CoopActorState result;
    result.index = index;
    result.monster = actor.monsterInfo.id;
    result.pos = actor.pos;
    result.yaw = actor.yawAngle;
    result.sectorId = actor.sectorId;
    result.hp = actor.hp;
    result.aiState = (actor.attributes & ACTOR_COOP_STAND_IN) ? Removed : actor.aiState; // Clients draw the parties themselves.
    result.animation = actor.currentActionAnimation;
    result.actionTime = actor.currentActionTime;
    result.actionLength = actor.currentActionLength;
    return result;
}

static void becomeMonster(Actor *actor, MonsterId monster) {
    const MonsterDesc &desc = pMonsterList->monsters[monster];
    actor->monsterInfo = pMonsterStats->infos[monster];
    actor->monsterId = monster;
    actor->radius = desc.monsterRadius;
    actor->height = desc.monsterHeight;
    actor->moveSpeed = desc.movementSpeed;
    actor->PrepareSprites(0);
}

static bool sameActionAndPlace(const CoopActorState &l, const CoopActorState &r) {
    // Action time ticks every frame, clients advance it on their own.
    return l.monster == r.monster && l.pos == r.pos && l.yaw == r.yaw && l.sectorId == r.sectorId && l.hp == r.hp && l.aiState == r.aiState &&
           l.animation == r.animation && l.actionLength == r.actionLength;
}

void CoopSession::sendActorStates() {
    auto now = std::chrono::steady_clock::now();
    bool full = now - _lastFullActorUpdate >= FULL_ACTOR_UPDATE_INTERVAL || _lastSentActorsMap != engine->_currentLoadedMapId ||
                _lastSentActors.size() != pActors.size();

    CoopActorStates states;
    states.map = engine->_currentLoadedMapId;
    states.actorCount = pActors.size();
    std::vector<CoopActorState> current;
    current.reserve(pActors.size());
    for (int i = 0; i < std::ssize(pActors); i++) {
        current.push_back(actorState(i, pActors[i]));
        if (full || !sameActionAndPlace(current.back(), _lastSentActors[i]))
            states.actors.push_back(current.back());
    }

    if (full)
        _lastFullActorUpdate = now;
    _lastSentActors = std::move(current);
    _lastSentActorsMap = engine->_currentLoadedMapId;
    if (!states.actors.empty())
        _server->sendActorStates(states);
}

void CoopSession::applyActorStates() {
    engine->_actorsFollowHost = _client->state() == COOP_CLIENT_JOINED;
    if (!engine->_actorsFollowHost)
        return;

    if (!gameTimer->isPaused())
        for (Actor &actor : pActors)
            actor.currentActionTime += gameTimer->dt();

    for (const CoopActorStates &states : _client->takeActorStates()) {
        if (states.map != engine->_currentLoadedMapId)
            continue;

        while (std::ssize(pActors) < states.actorCount) {
            Actor &actor = pActors.emplace_back(Actor(pActors.size()));
            actor.aiState = Removed; // Until the host's state for it arrives.
        }
        for (int i = states.actorCount; i < std::ssize(pActors); i++)
            pActors[i].aiState = Removed; // Spawned by a local encounter the host didn't roll.

        for (const CoopActorState &state : states.actors) {
            if (state.index < 0 || state.index >= states.actorCount || !allMonsters().contains(state.monster))
                continue;
            Actor &actor = pActors[state.index];
            if (actor.monsterInfo.id != state.monster)
                becomeMonster(&actor, state.monster);
            actor.pos = state.pos;
            actor.yawAngle = state.yaw;
            actor.sectorId = state.sectorId;
            actor.hp = state.hp;
            actor.aiState = state.aiState;
            actor.currentActionAnimation = state.animation;
            actor.currentActionTime = state.actionTime;
            actor.currentActionLength = state.actionLength;
        }
    }
}

void CoopSession::applyActorDamage() {
    for (const CoopActorDamage &damage : _server->takeActorDamage()) {
        if (damage.map != engine->_currentLoadedMapId || damage.index < 0 || damage.index >= std::ssize(pActors))
            continue; // The host has moved on to another map since.

        Actor &actor = pActors[damage.index];
        if (!actor.CanBeDamaged())
            continue;
        actor.hp -= damage.damage;
        if (actor.hp <= 0)
            Actor::Die(damage.index); // No experience, the guest's game gave it to the guest's party.
    }
}

void CoopSession::updateStandIns() {
    if (_standInsMap != engine->_currentLoadedMapId) {
        _standIns.clear(); // Loading a map replaced the whole actor list.
        _standInsMap = engine->_currentLoadedMapId;
    }

    const std::map<int, CoopPartyPose> &parties = _server->remoteParties();
    std::erase_if(_standIns, [&](const auto &entry) {
        auto pos = parties.find(entry.first);
        if (pos != parties.end() && pos->second.map == engine->_currentLoadedMapId)
            return false;
        pActors[entry.second].aiState = Removed;
        pActors[entry.second].attributes &= ~ACTOR_COOP_STAND_IN;
        return true;
    });

    for (const auto &[playerId, pose] : parties) {
        if (pose.map != engine->_currentLoadedMapId)
            continue;

        if (!_standIns.contains(playerId)) {
            Actor *actor = AllocateActor();
            if (!actor)
                continue;
            becomeMonster(actor, MONSTER_FIGHTER_CHAIN_C);
            actor->attributes = ACTOR_COOP_STAND_IN;
            _standIns[playerId] = actor->id;
        }

        Actor &actor = pActors[_standIns[playerId]];
        actor.pos = pose.pos;
        actor.yawAngle = pose.yaw;
        actor.velocity = Vec3f();
        actor.sectorId = uCurrentlyLoadedLevelType == LEVEL_INDOOR ? pIndoor->GetSector(pose.pos) : 0;
        actor.aiState = Standing;
        actor.hp = actor.monsterInfo.hp; // Stand-ins never die, what they're hit with is the guest's to take.
    }
}

void CoopSession::sendStandInHits() {
    for (const StandInHit &hit : std::exchange(engine->_standInHits, {})) {
        auto pos = std::ranges::find_if(_standIns, [&](const auto &entry) { return entry.second == hit.standInId; });
        if (pos != _standIns.end())
            _server->sendPartyHit(pos->first, CoopPartyHit{engine->_currentLoadedMapId, hit.attackerId, hit.ability});
    }
}

void CoopSession::applyPartyHits() {
    for (const CoopPartyHit &hit : _client->takePartyHits()) {
        if (hit.map != engine->_currentLoadedMapId || hit.attackerIndex < 0 || hit.attackerIndex >= std::ssize(pActors))
            continue;

        int target = stru_50C198.which_player_to_attack(&pActors[hit.attackerIndex]);
        if (target != -1)
            DamageCharacterFromMonster(Pid(OBJECT_Actor, hit.attackerIndex), hit.ability, target);
    }
}

void CoopSession::sendActorHits() {
    for (const ActorHit &hit : std::exchange(engine->_actorHitsForHost, {}))
        _client->sendActorDamage(CoopActorDamage{engine->_currentLoadedMapId, hit.actorId, hit.damage});
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
