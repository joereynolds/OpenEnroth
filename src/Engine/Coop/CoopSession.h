#pragma once

#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Core/Time/Time.h"

#include "Engine/Objects/ActorEnums.h"
#include "Engine/Spells/SpellBuff.h"

#include "Library/Geometry/Vec.h"

#include "Utility/IndexedArray.h"

#include "Utility/Memory/Blob.h"

#include "CoopProtocol.h"

class CoopClient;
class CoopServer;

/**
 * Connects a co-op session to the running game. Sends the local party's pose to the other players, and keeps
 * `Engine::_remoteParties` in sync with theirs.
 */
class CoopSession {
 public:
    ~CoopSession();

    /**
     * @param port                      Port to listen on, zero picks a free one.
     * @param maxClients                Maximal number of simultaneously connected clients.
     * @return                          Host session. Clients that join get the current game as a save, and are asked to
     *                                  come back later while the host isn't in a game.
     * @throws Exception                If the port couldn't be bound.
     */
    [[nodiscard]] static std::unique_ptr<CoopSession> host(int port, int maxClients);

    /**
     * @param address                   Host name or IP address of the host.
     * @param port                      Port the host is listening on.
     * @param playerName                Name to show to the other players.
     * @param guestSave                 One of the guest's own saves, its party joins the host's world. Empty to
     *                                  play with a copy of the host's party.
     * @return                          Client session that has started connecting. Drive `client()` until it has
     *                                  joined, then load the save it received before calling `update`.
     * @throws Exception                If the address couldn't be resolved.
     */
    [[nodiscard]] static std::unique_ptr<CoopSession> join(std::string_view address, int port, std::string_view playerName,
                                                           Blob guestSave);

    /**
     * @return                          Server of a host session, `nullptr` for a client session.
     */
    [[nodiscard]] CoopServer *server() const {
        return _server.get();
    }

    /**
     * @return                          Client of a client session, `nullptr` for a host session.
     */
    [[nodiscard]] CoopClient *client() const {
        return _client.get();
    }

    /**
     * Drops a client session's failed attempt and starts joining the same host again.
     */
    void retryJoin();

    /**
     * @return                          Save to load on the guest's side once joined, the host's world with the guest's
     *                                  party if there is one. Empty until joined.
     */
    [[nodiscard]] Blob takeJoinedSave();

    /**
     * Processes network events, sends the local party's pose if needed, and refreshes `Engine::_remoteParties`.
     * Should be called once per frame while a game is running.
     */
    void update();

    /**
     * Should be called when the game is about to change maps, with the destination in `Engine::_pendingTransition`.
     * A trip to another map that the group hasn't agreed on yet is put to a vote instead. The session starts the
     * map change itself once the vote passes.
     *
     * @return                          Whether the session took over the map change, the caller must then go back to
     *                                  playing instead of loading the destination.
     */
    [[nodiscard]] bool interceptMapChange();

    /**
     * Should be called when the local player asks to rest. Resting moves the world's clock, so the whole group has to
     * agree first, and the session starts the rest on every player's side once the vote passes.
     *
     * @return                          Whether the session took over the rest, the caller must then not rest now.
     */
    [[nodiscard]] bool interceptRest();

    /**
     * @return                          Trip that's waiting for the local player's answer, if any.
     */
    [[nodiscard]] const std::optional<CoopVote> &pendingVote() const;

    /**
     * Answers the vote returned by `pendingVote` on behalf of the local player.
     *
     * @param accepted                  Whether the local player agrees to travel.
     */
    void answerVote(bool accepted);

 private:
    CoopSession();

    [[nodiscard]] CoopPartyPose localPose();
    [[nodiscard]] const std::map<int, CoopPartyPose> &remoteParties() const;
    void sendLocalPose(const CoopPartyPose &pose);
    void handleVoteDecision(const CoopVoteDecision &decision);
    void handleRestDecision(const CoopVoteDecision &decision);
    void sendActorStates();
    void applyActorStates();
    void applyActorDamage();
    void updateStandIns();
    void sendStandInHits();
    void applyPartyHits();
    void handOutLoot();
    void sendLootRequests();
    void applyLoot();
    void syncQuestBits();
    void syncQuestRewards();
    void syncChests();
    void syncGroundItems();
    void syncDroppedItems();
    void syncLaunchedSprites();
    void syncClock();
    void sendActorHits();
    void sendActorBuffs();
    void applyActorBuffs();

 private:
    std::unique_ptr<CoopServer> _server;
    std::unique_ptr<CoopClient> _client;
    std::string _joinAddress;
    int _joinPort = 0;
    std::string _playerName;
    Blob _guestSave;
    std::optional<Vec3f> _lastFramePos;
    std::optional<CoopPartyPose> _lastSentPose;
    std::chrono::steady_clock::time_point _lastSendTime;
    bool _travelApproved = false;
    bool _restApproved = false;
    std::vector<CoopActorState> _lastSentActors;
    MapId _lastSentActorsMap = MAP_INVALID;
    std::chrono::steady_clock::time_point _lastFullActorUpdate;
    std::map<int, int> _standIns; // Player id to the index of the actor that stands in for that player's party.
    MapId _standInsMap = MAP_INVALID;
    std::optional<Time> _lastClockSendTime;
    std::vector<IndexedArray<SpellBuff, ACTOR_BUFF_FIRST, ACTOR_BUFF_LAST>> _lastActorBuffs; // Guest, buffs as of the last frame.
    MapId _lastActorBuffsMap = MAP_INVALID; // Game time, so that the clock goes out as often in tests as in play.
};

extern std::unique_ptr<CoopSession> coopSession;
