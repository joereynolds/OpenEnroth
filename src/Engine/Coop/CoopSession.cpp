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
#include "Engine/Evt/EvtVariables.h"
#include "Engine/Graphics/Indoor.h"
#include "Engine/Objects/Actor.h"
#include "Engine/Objects/Chest.h"
#include "Engine/Objects/SpriteObject.h"
#include "Engine/Objects/Character.h"
#include "Engine/Objects/MonsterEnumFunctions.h"
#include "Engine/Objects/Monsters.h"
#include "Engine/Tables/MapTable.h"
#include "Engine/Tables/ObjectTable.h"
#include "Engine/Tables/QuestTable.h"

#include "GUI/GUIWindow.h"
#include "GUI/GUIMessageQueue.h"
#include "GUI/UI/UIGame.h"
#include "GUI/UI/UIRest.h"
#include "GUI/UI/UIStatusBar.h"

#include "Media/Audio/AudioPlayer.h"

#include "Utility/String/Format.h"

#include "CoopClient.h"
#include "CoopGuestSave.h"
#include "CoopServer.h"

static constexpr std::chrono::seconds POSE_HEARTBEAT{1}; // Makes up for an unreliable pose that got lost while the party stands still.
static constexpr std::chrono::seconds FULL_ACTOR_UPDATE_INTERVAL{1}; // Makes up for lost unreliable monster updates.
static constexpr Duration CLOCK_INTERVAL = Duration::fromSeconds(30); // A real second at normal speed.
static constexpr Duration CLOCK_TOLERANCE = Duration::fromMinutes(1); // About two real seconds, well above network delay.

std::unique_ptr<CoopSession> coopSession;

CoopSession::CoopSession() {
    engine->_coopSessionActive = true;
}

CoopSession::~CoopSession() {
    engine->_remoteParties.clear();
    engine->_actorsFollowHost = false;
    if (gameTimer)
        gameTimer->setKeepsRunning(false);
    engine->_actorHitsForHost.clear();
    engine->_standInHits.clear();
    engine->_lootRequestsForHost.clear();
    engine->_coopSessionActive = false;
    engine->_questBitChanges.clear();
    engine->_questRewards.clear();
    engine->_changedChests.clear();
    engine->_takenGroundItems.clear();
    engine->_droppedItems.clear();
    engine->_launchedSprites.clear();
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
        gameTimer->setKeepsRunning(_server->joinedClientCount() > 0); // The guests' monsters are the host's.
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
        applyActorBuffs();
        handOutLoot();
        sendStandInHits();
        updateStandIns();
        sendActorStates();
    } else {
        sendActorHits();
        sendActorBuffs();
        sendLootRequests();
        applyActorStates();
        applyPartyHits();
        applyLoot();
    }

    syncQuestBits();
    syncQuestRewards();
    syncChests();
    syncGroundItems();
    syncDroppedItems();
    syncLaunchedSprites();
    syncClock();

    std::optional<CoopVoteDecision> decision = _server ? _server->takeVoteDecision() : _client->takeVoteDecision();
    if (decision)
        handleVoteDecision(*decision);

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
        _server->requestVote(COOP_VOTE_TRAVEL, destination);
    } else {
        _client->requestVote(COOP_VOTE_TRAVEL, destination);
    }
    engine->_statusBar->setEvent(fmt::format("Asked the others to travel to {}", pMapTable->pInfos[map].name));
    return true;
}

const std::optional<CoopVote> &CoopSession::pendingVote() const {
    return _server ? _server->pendingVote() : _client->pendingVote();
}

void CoopSession::answerVote(bool accepted) {
    if (_server) {
        _server->answerVote(accepted);
    } else {
        _client->answerVote(accepted);
    }
}

bool CoopSession::interceptRest() {
    if (std::exchange(_restApproved, false))
        return false;

    if (_server) {
        _server->requestVote(COOP_VOTE_REST);
    } else {
        _client->requestVote(COOP_VOTE_REST);
    }
    engine->_statusBar->setEvent("Asked the others to rest");
    return true;
}

void CoopSession::handleVoteDecision(const CoopVoteDecision &decision) {
    if (decision.kind == COOP_VOTE_REST) {
        handleRestDecision(decision);
        return;
    }

    std::string mapName = pMapTable->pInfos[decision.destination.map()].name;
    if (decision.approved) {
        _travelApproved = true;
        startMapTransition(decision.destination);
    } else if (decision.voteId == -1) {
        engine->_statusBar->setEvent(fmt::format("Can't travel to {} while another vote is going on", mapName));
    } else {
        engine->_statusBar->setEvent(fmt::format("{} doesn't want to travel to {}", decision.declinedBy, mapName));
    }
}

void CoopSession::handleRestDecision(const CoopVoteDecision &decision) {
    if (!decision.approved) {
        if (decision.voteId == -1) {
            engine->_statusBar->setEvent("Can't rest while another vote is going on");
        } else {
            engine->_statusBar->setEvent(fmt::format("{} doesn't want to rest", decision.declinedBy));
        }
        return;
    }

    if (current_screen_type == SCREEN_GAME)
        pGUIWindow_CurrentMenu = std::make_unique<GUIWindow_Rest>();
    if (current_screen_type != SCREEN_REST) {
        engine->_statusBar->setEvent("The group rested while you were busy"); // In a shop or a dialogue, the rest window can't open over it.
        return;
    }
    _restApproved = true;
    engine->_messageQueue->addMessageNextFrame(UIMSG_Rest8Hour, 0, 0);
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
    for (ActorBuff buff : actor.buffs.indices())
        if (actor.buffs[buff].Active())
            result.activeBuffs |= 1u << std::to_underlying(buff);
    if (actor.buffs[ACTOR_BUFF_SHRINK].Active())
        result.shrinkPower = actor.buffs[ACTOR_BUFF_SHRINK].power;
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
           l.animation == r.animation && l.actionLength == r.actionLength && l.activeBuffs == r.activeBuffs && l.shrinkPower == r.shrinkPower;
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
            for (ActorBuff buff : actor.buffs.indices()) {
                bool active = state.activeBuffs & (1u << std::to_underlying(buff));
                if (active && !actor.buffs[buff].Active()) {
                    actor.buffs[buff].Apply(pParty->GetPlayingTime() + Duration::fromHours(1), MASTERY_NOVICE, 0, 0, -1); // Lasts until the host says otherwise.
                } else if (!active && actor.buffs[buff].Active()) {
                    actor.buffs[buff].Reset();
                }
            }
            if (actor.buffs[ACTOR_BUFF_SHRINK].Active())
                actor.buffs[ACTOR_BUFF_SHRINK].power = state.shrinkPower;
            if (state.index < std::ssize(_lastActorBuffs))
                _lastActorBuffs[state.index] = actor.buffs; // These changes are the host's, they mustn't go back to it.
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
        actor.monsterInfo.recoveryTime += damage.extraRecovery;
        if (damage.knockback != Vec3f() && actor.hp > 0)
            actor.velocity = damage.knockback;
        if (actor.hp <= 0) {
            Actor::Die(damage.index); // No experience, the guest's game gave it to the guest's party.
        } else if (auto standIn = _standIns.find(damage.playerId); standIn != _standIns.end()) {
            Actor::AI_Pain(damage.index, Pid(OBJECT_Actor, standIn->second), 0);
        }
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

/**
 * Collects loot for a co-op guest instead of giving it to the local party.
 */
class GuestLootReceiver : public LootReceiver {
 public:
    virtual void receiveGold(int gold) override {
        loot.gold += gold;
    }

    virtual void receiveFoundItem(int gold, const Item &item) override {
        loot.foundItem = item;
    }

    virtual bool receiveExtraItem(const Item &item) override {
        loot.extraItems.push_back(item);
        return true; // The guest's inventory is out of reach, the guest's game puts what doesn't fit on the cursor.
    }

    CoopLoot loot;
};

void CoopSession::handOutLoot() {
    for (const CoopLootRequest &request : _server->takeLootRequests()) {
        if (request.map != engine->_currentLoadedMapId || request.index < 0 || request.index >= std::ssize(pActors))
            continue;
        if (pActors[request.index].aiState != Dead)
            continue; // Someone else got here first.

        GuestLootReceiver receiver;
        pActors[request.index].lootInto(&receiver);
        _server->sendLoot(request.playerId, receiver.loot);
    }
}

void CoopSession::sendLootRequests() {
    for (int index : std::exchange(engine->_lootRequestsForHost, {}))
        _client->sendLootRequest(CoopLootRequest{.map = engine->_currentLoadedMapId, .index = index});
}

void CoopSession::applyLoot() {
    for (const CoopLoot &loot : _client->takeLoot()) {
        pParty->placeHeldItemInInventoryOrDrop();
        PartyLootReceiver receiver;
        if (loot.gold)
            receiver.receiveGold(loot.gold);
        if (loot.foundItem)
            receiver.receiveFoundItem(loot.gold, *loot.foundItem);
        for (const Item &item : loot.extraItems)
            receiver.receiveExtraItem(item);
    }
}

static void applyQuestBit(const CoopQuestBit &change) {
    if (change.value && !pParty->_questBits[change.bit] && !pQuestTable[change.bit].empty())
        bFlashQuestBook = true;
    pParty->_questBits.set(change.bit, change.value); // Directly, so that the change isn't recorded and sent back.
}

void CoopSession::syncQuestBits() {
    for (const QuestBitChange &local : std::exchange(engine->_questBitChanges, {})) {
        CoopQuestBit change{.bit = local.bit, .value = local.value};
        if (_server) {
            change.playerId = CoopServer::HOST_PLAYER_ID;
            _server->sendQuestBit(change);
        } else {
            _client->sendQuestBit(change);
        }
    }

    std::vector<CoopQuestBit> remote = _server ? _server->takeQuestBits() : _client->takeQuestBits();
    for (const CoopQuestBit &change : remote) {
        if (change.bit < QBIT_FIRST || change.bit > QBIT_LAST)
            continue;
        applyQuestBit(change);
        if (_server)
            _server->sendQuestBit(change);
    }
}

void CoopSession::syncQuestRewards() {
    std::vector<QuestReward> local = std::exchange(engine->_questRewards, {});
    if (!local.empty()) {
        CoopQuestRewards rewards{.rewards = std::move(local)};
        if (_server) {
            rewards.playerId = CoopServer::HOST_PLAYER_ID;
            _server->sendQuestRewards(rewards);
        } else {
            _client->sendQuestRewards(rewards);
        }
    }

    std::vector<CoopQuestRewards> remote = _server ? _server->takeQuestRewards() : _client->takeQuestRewards();
    for (const CoopQuestRewards &rewards : remote) {
        for (const QuestReward &reward : rewards.rewards)
            if (reward.characterIndex >= 0 && reward.characterIndex < std::ssize(pParty->pCharacters))
                addEvtVariable(pParty->pCharacters[reward.characterIndex], reward.variable, reward.value);
        if (_server)
            _server->sendQuestRewards(rewards);
    }
}

void CoopSession::syncChests() {
    std::vector<int> local = std::exchange(engine->_changedChests, {});
    std::ranges::sort(local);
    local.erase(std::unique(local.begin(), local.end()), local.end());
    for (int chestId : local) {
        CoopChest chest{.map = engine->_currentLoadedMapId, .chestId = chestId, .chest = vChests[chestId]};
        if (_server) {
            chest.playerId = CoopServer::HOST_PLAYER_ID;
            _server->sendChest(chest);
        } else {
            _client->sendChest(chest);
        }
    }

    std::vector<CoopChest> remote = _server ? _server->takeChests() : _client->takeChests();
    for (const CoopChest &chest : remote) {
        if (chest.map == engine->_currentLoadedMapId && chest.chestId >= 0 && chest.chestId < std::ssize(vChests)) {
            Chest &target = vChests[chest.chestId];
            target.chestTypeId = chest.chest.chestTypeId;
            target.flags = chest.chest.flags;
            target.inventory = chest.chest.inventory;
        }
        if (_server)
            _server->sendChest(chest); // Even from another map, a guest there may share it with the sender.
    }
}

static constexpr float GROUND_ITEM_MATCH_DISTANCE = 64; // Items lying this close with the same kind count as the same item.

static void removeGroundItem(ItemId item, const Vec3f &pos) {
    int closest = -1;
    float closestDistance = GROUND_ITEM_MATCH_DISTANCE;
    for (int i = 0; i < std::ssize(pSpriteObjects); i++) {
        const SpriteObject &object = pSpriteObjects[i];
        if (!object.uObjectDescID || object.containing_item.itemId != item)
            continue;
        float distance = (object.vPosition - pos).length();
        if (distance < closestDistance) {
            closest = i;
            closestDistance = distance;
        }
    }
    if (closest != -1)
        SpriteObject::Remove(closest);
}

void CoopSession::syncGroundItems() {
    for (const GroundItemTaken &local : std::exchange(engine->_takenGroundItems, {})) {
        CoopGroundItemTaken taken{.map = engine->_currentLoadedMapId, .item = local.item, .pos = local.pos};
        if (_server) {
            taken.playerId = CoopServer::HOST_PLAYER_ID;
            _server->sendGroundItemTaken(taken);
        } else {
            _client->sendGroundItemTaken(taken);
        }
    }

    std::vector<CoopGroundItemTaken> remote = _server ? _server->takeGroundItemsTaken() : _client->takeGroundItemsTaken();
    for (const CoopGroundItemTaken &taken : remote) {
        if (taken.map == engine->_currentLoadedMapId)
            removeGroundItem(taken.item, taken.pos);
        if (_server)
            _server->sendGroundItemTaken(taken);
    }
}

void CoopSession::syncClock() {
    if (_server) {
        Time now = pParty->GetPlayingTime();
        if (!_lastClockSendTime || now - *_lastClockSendTime >= CLOCK_INTERVAL || now < *_lastClockSendTime) {
            _server->sendClock(CoopClock{now});
            _lastClockSendTime = now;
        }
        return;
    }

    std::optional<CoopClock> clock = _client->takeClock();
    if (!clock || currentRestType != REST_NONE)
        return; // A rest moves the clock on its own, the next clock after it brings the guest back in line.

    Duration drift = clock->playingTime - pParty->GetPlayingTime();
    if (drift > CLOCK_TOLERANCE) {
        _494035_timed_effects__water_walking_damage__etc(drift); // Catching up goes through the day changes it skipped.
    } else if (drift < Duration() - CLOCK_TOLERANCE) {
        pParty->GetPlayingTime() = clock->playingTime;
        _494035_timed_effects__water_walking_damage__etc(Duration()); // Updates the calendar fields.
    }
}

void CoopSession::syncDroppedItems() {
    for (const DroppedItem &local : std::exchange(engine->_droppedItems, {})) {
        CoopItemDropped dropped{.map = engine->_currentLoadedMapId, .item = local.item, .pos = local.pos, .yaw = local.yaw};
        if (_server) {
            dropped.playerId = CoopServer::HOST_PLAYER_ID;
            _server->sendItemDropped(dropped);
        } else {
            _client->sendItemDropped(dropped);
        }
    }

    std::vector<CoopItemDropped> remote = _server ? _server->takeItemsDropped() : _client->takeItemsDropped();
    for (const CoopItemDropped &dropped : remote) {
        if (dropped.map == engine->_currentLoadedMapId) {
            int sectorId = uCurrentlyLoadedLevelType == LEVEL_INDOOR ? pIndoor->GetSector(dropped.pos) : 0;
            Party::dropItem(dropped.item, dropped.pos, dropped.yaw, sectorId);
        }
        if (_server)
            _server->sendItemDropped(dropped);
    }
}

void CoopSession::syncLaunchedSprites() {
    for (const SpriteObject &local : std::exchange(engine->_launchedSprites, {})) {
        CoopSpriteLaunched launched{.map = engine->_currentLoadedMapId, .sprite = local};
        if (_server) {
            launched.playerId = CoopServer::HOST_PLAYER_ID;
            _server->sendSpriteLaunched(launched);
        } else {
            _client->sendSpriteLaunched(launched);
        }
    }

    std::vector<SpellId> soundsPlayed; // A spray of sprites makes one sound in the caster's game too.
    std::vector<CoopSpriteLaunched> remote = _server ? _server->takeSpritesLaunched() : _client->takeSpritesLaunched();
    for (const CoopSpriteLaunched &launched : remote) {
        if (_server)
            _server->sendSpriteLaunched(launched);
        if (launched.map != engine->_currentLoadedMapId)
            continue;

        SpriteObject sprite = launched.sprite;
        sprite.uObjectDescID = pObjectTable->ObjectIDByItemID(sprite.spriteId);
        sprite.isCoopRemote = true;
        int slot = sprite.Create(0, 0, 0, 0);
        if (slot == -1)
            continue;
        pSpriteObjects[slot].vVelocity = launched.sprite.vVelocity; // Create only knows to launch at a yaw and pitch.

        if (!std::ranges::contains(soundsPlayed, sprite.uSpellID)) {
            pAudioPlayer->playSpellSound(sprite.uSpellID, false, SOUND_MODE_PID, Pid(OBJECT_Sprite, slot));
            soundsPlayed.push_back(sprite.uSpellID);
        }
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

static bool sameBuff(const SpellBuff &l, const SpellBuff &r) {
    return l.expireTime == r.expireTime && l.power == r.power && l.skillMastery == r.skillMastery && l.caster == r.caster;
}

void CoopSession::sendActorBuffs() {
    // Guests don't run monster AI and the host's updates leave buffs alone, so any change here is the local party's.
    if (_lastActorBuffsMap != engine->_currentLoadedMapId) {
        _lastActorBuffs.clear();
        _lastActorBuffsMap = engine->_currentLoadedMapId;
    }
    while (_lastActorBuffs.size() < pActors.size())
        _lastActorBuffs.push_back(pActors[_lastActorBuffs.size()].buffs);

    for (int i = 0; i < std::ssize(pActors); i++) {
        for (ActorBuff buff : pActors[i].buffs.indices()) {
            if (sameBuff(pActors[i].buffs[buff], _lastActorBuffs[i][buff]))
                continue;
            _client->sendActorBuff(CoopActorBuff{.map = engine->_currentLoadedMapId, .index = i, .buff = buff, .value = pActors[i].buffs[buff]});
            _lastActorBuffs[i][buff] = pActors[i].buffs[buff];
        }
    }
}

void CoopSession::applyActorBuffs() {
    for (const CoopActorBuff &buff : _server->takeActorBuffs())
        if (buff.map == engine->_currentLoadedMapId && buff.index >= 0 && buff.index < std::ssize(pActors))
            pActors[buff.index].buffs[buff.buff] = buff.value;
}

void CoopSession::sendActorHits() {
    for (const ActorHit &hit : std::exchange(engine->_actorHitsForHost, {}))
        _client->sendActorDamage(CoopActorDamage{.map = engine->_currentLoadedMapId, .index = hit.actorId, .damage = hit.damage,
                                                 .extraRecovery = hit.extraRecovery, .knockback = hit.knockback});
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
