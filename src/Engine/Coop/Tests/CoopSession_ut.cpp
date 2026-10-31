#include <chrono>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>

#include "Testing/Unit/UnitTest.h"

#include "Engine/Coop/CoopClient.h"
#include "Engine/Coop/CoopServer.h"

#include "Library/Net/NetHost.h"

static constexpr std::chrono::seconds TIMEOUT{5};

static Blob testSaveGame() {
    return Blob::fromString("the world");
}

static CoopPartyPose makePose(MapId map, Vec3f pos, int yaw) {
    CoopPartyPose result;
    result.map = map;
    result.pos = pos;
    result.yaw = yaw;
    return result;
}

static bool pumpUntil(CoopServer *server, std::initializer_list<CoopClient *> clients, const std::function<bool()> &done) {
    // Updates the server and all clients until `done` returns true, returns false on timeout.
    auto deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (std::chrono::steady_clock::now() < deadline) {
        server->update();
        for (CoopClient *client : clients)
            client->update();
        if (done())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

static std::unique_ptr<CoopClient> joinedClient(CoopServer *server, std::string_view name) {
    // Connects a client and waits until the server has welcomed it.
    auto client = std::make_unique<CoopClient>("127.0.0.1", server->port(), name);
    EXPECT_TRUE(pumpUntil(server, {client.get()}, [&] { return client->state() != COOP_CLIENT_CONNECTING; }));
    EXPECT_EQ(client->state(), COOP_CLIENT_JOINED);
    return client;
}

UNIT_TEST(CoopSession, ClientJoinsAndReceivesSaveGame) {
    // A client that connects is welcomed with a player id and the host's save game.
    CoopServer server(0, 4, testSaveGame);
    std::unique_ptr<CoopClient> client = joinedClient(&server, "Zoltan");

    EXPECT_EQ(client->playerId(), CoopServer::HOST_PLAYER_ID + 1);
    EXPECT_EQ(client->failureReason(), "");
    EXPECT_EQ(client->takeSaveGame().str(), "the world");
    EXPECT_EQ(client->takeSaveGame().str(), "");
}

UNIT_TEST(CoopSession, PlayerIdsAreNotReused) {
    // Every client gets a fresh player id, even after an earlier client has left.
    CoopServer server(0, 4, testSaveGame);
    std::unique_ptr<CoopClient> client1 = joinedClient(&server, "One");
    int firstId = client1->playerId();
    client1.reset();

    std::unique_ptr<CoopClient> client2 = joinedClient(&server, "Two");
    EXPECT_NE(client2->playerId(), firstId);
    EXPECT_NE(client2->playerId(), CoopServer::HOST_PLAYER_ID);
}

UNIT_TEST(CoopSession, WrongProtocolVersionIsRejected) {
    // A client with a different protocol version is told why it's turned away, then disconnected.
    CoopServer server(0, 4, testSaveGame);
    std::unique_ptr<NetHost> raw = NetHost::connect("127.0.0.1", server.port());

    std::optional<CoopMessage> reply;
    bool disconnected = false;
    auto deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (!disconnected && std::chrono::steady_clock::now() < deadline) {
        server.update();
        while (std::optional<NetEvent> event = raw->poll(1)) {
            if (event->type == NET_EVENT_CONNECTED) {
                CoopHello hello;
                hello.protocolVersion = COOP_PROTOCOL_VERSION + 1;
                raw->send(event->peer, NET_CHANNEL_RELIABLE, encodeCoopMessage(std::move(hello)));
            } else if (event->type == NET_EVENT_MESSAGE) {
                reply = decodeCoopMessage(event->data);
            } else {
                disconnected = true;
            }
        }
    }

    ASSERT_TRUE(reply);
    ASSERT_TRUE(std::holds_alternative<CoopReject>(*reply));
    EXPECT_TRUE(std::get<CoopReject>(*reply).reason.contains("version"));
    EXPECT_TRUE(disconnected);
}

UNIT_TEST(CoopSession, PosesAreRelayedToEveryone) {
    // A client's pose reaches the host and the other client, and the host's pose reaches both clients.
    CoopServer server(0, 4, testSaveGame);
    std::unique_ptr<CoopClient> client1 = joinedClient(&server, "One");
    std::unique_ptr<CoopClient> client2 = joinedClient(&server, "Two");
    int id1 = client1->playerId();

    client1->sendLocalPose(makePose(MAP_HARMONDALE, Vec3f(100, 200, 300), 512));
    server.sendLocalPose(makePose(MAP_HARMONDALE, Vec3f(-5, -6, -7), 1024));
    ASSERT_TRUE(pumpUntil(&server, {client1.get(), client2.get()}, [&] {
        return server.remoteParties().contains(id1) && client2->remoteParties().size() == 2 && client1->remoteParties().size() == 1;
    }));

    EXPECT_EQ(server.remoteParties().at(id1).pos, Vec3f(100, 200, 300));
    EXPECT_EQ(server.remoteParties().at(id1).playerId, id1);

    EXPECT_EQ(client2->remoteParties().at(id1).pos, Vec3f(100, 200, 300));
    EXPECT_EQ(client2->remoteParties().at(id1).yaw, 512);
    EXPECT_EQ(client2->remoteParties().at(CoopServer::HOST_PLAYER_ID).pos, Vec3f(-5, -6, -7));
    EXPECT_EQ(client2->remoteParties().at(CoopServer::HOST_PLAYER_ID).map, MAP_HARMONDALE);

    EXPECT_FALSE(client1->remoteParties().contains(id1)); // Own pose is not echoed back.
    EXPECT_EQ(client1->remoteParties().at(CoopServer::HOST_PLAYER_ID).yaw, 1024);
}

UNIT_TEST(CoopSession, LateJoinerSeesExistingParties) {
    // A client that joins after the others have moved sees them right away, without waiting for them to move again.
    CoopServer server(0, 4, testSaveGame);
    std::unique_ptr<CoopClient> client1 = joinedClient(&server, "One");
    int id1 = client1->playerId();

    client1->sendLocalPose(makePose(MAP_HARMONDALE, Vec3f(1, 2, 3), 0));
    server.sendLocalPose(makePose(MAP_HARMONDALE, Vec3f(4, 5, 6), 0));
    ASSERT_TRUE(pumpUntil(&server, {client1.get()}, [&] { return server.remoteParties().contains(id1); }));

    std::unique_ptr<CoopClient> client2 = joinedClient(&server, "Two");
    ASSERT_TRUE(pumpUntil(&server, {client1.get(), client2.get()}, [&] { return client2->remoteParties().size() == 2; }));
    EXPECT_EQ(client2->remoteParties().at(id1).pos, Vec3f(1, 2, 3));
    EXPECT_EQ(client2->remoteParties().at(CoopServer::HOST_PLAYER_ID).pos, Vec3f(4, 5, 6));
}

UNIT_TEST(CoopSession, LeavingPlayerIsRemovedEverywhere) {
    // When a client leaves, its party disappears for the host and for the remaining clients.
    CoopServer server(0, 4, testSaveGame);
    std::unique_ptr<CoopClient> client1 = joinedClient(&server, "One");
    std::unique_ptr<CoopClient> client2 = joinedClient(&server, "Two");
    int id1 = client1->playerId();

    client1->sendLocalPose(makePose(MAP_HARMONDALE, Vec3f(1, 2, 3), 0));
    ASSERT_TRUE(pumpUntil(&server, {client1.get(), client2.get()}, [&] { return client2->remoteParties().contains(id1); }));

    client1.reset();
    ASSERT_TRUE(pumpUntil(&server, {client2.get()}, [&] { return !client2->remoteParties().contains(id1); }));
    EXPECT_FALSE(server.remoteParties().contains(id1));
}

UNIT_TEST(CoopSession, ClientFailsWhenHostShutsDown) {
    // A client whose host goes away ends up failed, with a reason, and forgets the other parties.
    auto server = std::make_unique<CoopServer>(0, 4, testSaveGame);
    std::unique_ptr<CoopClient> client = joinedClient(server.get(), "One");
    server->sendLocalPose(makePose(MAP_HARMONDALE, Vec3f(1, 2, 3), 0));
    ASSERT_TRUE(pumpUntil(server.get(), {client.get()}, [&] { return !client->remoteParties().empty(); }));

    server.reset();
    auto deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (client->state() != COOP_CLIENT_FAILED && std::chrono::steady_clock::now() < deadline)
        client->update();

    EXPECT_EQ(client->state(), COOP_CLIENT_FAILED);
    EXPECT_EQ(client->failureReason(), "Disconnected from the host");
    EXPECT_TRUE(client->remoteParties().empty());
}

UNIT_TEST(CoopSession, MalformedMessageGetsClientDisconnected) {
    // A client that sends garbage is dropped by the host.
    CoopServer server(0, 4, testSaveGame);
    std::unique_ptr<NetHost> raw = NetHost::connect("127.0.0.1", server.port());

    bool disconnected = false;
    auto deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (!disconnected && std::chrono::steady_clock::now() < deadline) {
        server.update();
        while (std::optional<NetEvent> event = raw->poll(1)) {
            if (event->type == NET_EVENT_CONNECTED)
                raw->send(event->peer, NET_CHANNEL_RELIABLE, Blob::fromString("\x7Fgarbage"));
            else if (event->type == NET_EVENT_DISCONNECTED)
                disconnected = true;
        }
    }
    EXPECT_TRUE(disconnected);
}
