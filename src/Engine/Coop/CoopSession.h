#pragma once

#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <string_view>

#include "Library/Geometry/Vec.h"

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
     * @return                          Host session. Clients that join get the current game as a save.
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
     * @return                          Trip that's waiting for the local player's answer, if any.
     */
    [[nodiscard]] const std::optional<CoopTravelVote> &pendingVote() const;

    /**
     * Answers the vote returned by `pendingVote` on behalf of the local player.
     *
     * @param accepted                  Whether the local player agrees to travel.
     */
    void answerTravel(bool accepted);

 private:
    CoopSession();

    [[nodiscard]] CoopPartyPose localPose();
    [[nodiscard]] const std::map<int, CoopPartyPose> &remoteParties() const;
    void sendLocalPose(const CoopPartyPose &pose);
    void handleTravelDecision(const CoopTravelDecision &decision);

 private:
    std::unique_ptr<CoopServer> _server;
    std::unique_ptr<CoopClient> _client;
    Blob _guestSave;
    std::optional<Vec3f> _lastFramePos;
    std::optional<CoopPartyPose> _lastSentPose;
    std::chrono::steady_clock::time_point _lastSendTime;
    bool _travelApproved = false;
};

extern std::unique_ptr<CoopSession> coopSession;
