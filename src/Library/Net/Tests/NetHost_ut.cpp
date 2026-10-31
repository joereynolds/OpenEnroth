#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Testing/Unit/UnitTest.h"

#include "Library/Net/NetHost.h"

#include "Utility/Exception.h"

static constexpr std::chrono::seconds TIMEOUT{5};

static std::string toString(const Blob &blob) {
    return std::string(blob.str());
}

static std::optional<NetEvent> waitForEvent(NetHost *host, NetHost *other) {
    // Polls both hosts until `host` reports an event, `other` must stay quiet meanwhile.
    auto deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (std::chrono::steady_clock::now() < deadline) {
        if (std::optional<NetEvent> event = host->poll(1))
            return event;
        if (other) {
            std::optional<NetEvent> otherEvent = other->poll(1);
            EXPECT_FALSE(otherEvent) << "Unexpected event on the other host.";
        }
    }
    return std::nullopt;
}

static void connectBoth(NetHost *server, NetHost *client, NetPeerId *clientOnServer, NetPeerId *serverOnClient) {
    // Waits for the connection events on both ends and returns the peer ids they report.
    std::optional<NetEvent> serverEvent;
    std::optional<NetEvent> clientEvent;
    auto deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while ((!serverEvent || !clientEvent) && std::chrono::steady_clock::now() < deadline) {
        if (std::optional<NetEvent> event = server->poll(1)) {
            ASSERT_FALSE(serverEvent) << "Unexpected second event on the server.";
            serverEvent = std::move(event);
        }
        if (std::optional<NetEvent> event = client->poll(1)) { // The client acks the handshake on its next poll, so it's polled even when done.
            ASSERT_FALSE(clientEvent) << "Unexpected second event on the client.";
            clientEvent = std::move(event);
        }
    }
    ASSERT_TRUE(serverEvent);
    ASSERT_TRUE(clientEvent);
    ASSERT_EQ(serverEvent->type, NET_EVENT_CONNECTED);
    ASSERT_EQ(clientEvent->type, NET_EVENT_CONNECTED);
    *clientOnServer = serverEvent->peer;
    *serverOnClient = clientEvent->peer;
}

UNIT_TEST(NetHost, ListenPicksFreePort) {
    // Listening on port zero binds to some free port and reports it.
    std::unique_ptr<NetHost> server = NetHost::listen(0, 4);
    EXPECT_GT(server->port(), 0);
}

UNIT_TEST(NetHost, ListenOnBusyPortThrows) {
    // Binding a port that's already taken is an error.
    std::unique_ptr<NetHost> server = NetHost::listen(0, 4);
    EXPECT_THROW((void) NetHost::listen(server->port(), 4), Exception);
}

UNIT_TEST(NetHost, ConnectAndExchangeMessages) {
    // A client connects, and messages travel both ways on both channels.
    std::unique_ptr<NetHost> server = NetHost::listen(0, 4);
    std::unique_ptr<NetHost> client = NetHost::connect("127.0.0.1", server->port());

    NetPeerId clientOnServer = -1;
    NetPeerId serverOnClient = -1;
    ASSERT_NO_FATAL_FAILURE(connectBoth(server.get(), client.get(), &clientOnServer, &serverOnClient));

    client->send(serverOnClient, NET_CHANNEL_RELIABLE, Blob::fromString("hello"));
    std::optional<NetEvent> event = waitForEvent(server.get(), client.get());
    ASSERT_TRUE(event);
    EXPECT_EQ(event->type, NET_EVENT_MESSAGE);
    EXPECT_EQ(event->peer, clientOnServer);
    EXPECT_EQ(toString(event->data), "hello");

    server->send(clientOnServer, NET_CHANNEL_UNRELIABLE, Blob::fromString("world"));
    event = waitForEvent(client.get(), server.get());
    ASSERT_TRUE(event);
    EXPECT_EQ(event->type, NET_EVENT_MESSAGE);
    EXPECT_EQ(event->peer, serverOnClient);
    EXPECT_EQ(toString(event->data), "world");
}

UNIT_TEST(NetHost, ReliableMessagesArriveInOrder) {
    // A burst of reliable messages arrives complete and in the order it was sent.
    std::unique_ptr<NetHost> server = NetHost::listen(0, 4);
    std::unique_ptr<NetHost> client = NetHost::connect("127.0.0.1", server->port());

    NetPeerId clientOnServer = -1;
    NetPeerId serverOnClient = -1;
    ASSERT_NO_FATAL_FAILURE(connectBoth(server.get(), client.get(), &clientOnServer, &serverOnClient));

    std::vector<std::string> sent;
    for (int i = 0; i < 100; i++) {
        sent.push_back(std::to_string(i));
        client->send(serverOnClient, NET_CHANNEL_RELIABLE, Blob::fromString(sent.back()));
    }

    std::vector<std::string> received;
    while (received.size() < sent.size()) {
        std::optional<NetEvent> event = waitForEvent(server.get(), client.get());
        ASSERT_TRUE(event);
        ASSERT_EQ(event->type, NET_EVENT_MESSAGE);
        received.push_back(toString(event->data));
    }
    EXPECT_EQ(received, sent);
}

UNIT_TEST(NetHost, BroadcastReachesAllClients) {
    // A broadcast is delivered to every connected client.
    std::unique_ptr<NetHost> server = NetHost::listen(0, 4);
    std::unique_ptr<NetHost> client1 = NetHost::connect("127.0.0.1", server->port());
    std::unique_ptr<NetHost> client2 = NetHost::connect("127.0.0.1", server->port());

    NetPeerId clientOnServer = -1;
    NetPeerId serverOnClient = -1;
    ASSERT_NO_FATAL_FAILURE(connectBoth(server.get(), client1.get(), &clientOnServer, &serverOnClient));
    ASSERT_NO_FATAL_FAILURE(connectBoth(server.get(), client2.get(), &clientOnServer, &serverOnClient));

    server->broadcast(NET_CHANNEL_RELIABLE, Blob::fromString("everyone"));
    server->flush();

    for (NetHost *client : {client1.get(), client2.get()}) {
        std::optional<NetEvent> event = waitForEvent(client, nullptr);
        ASSERT_TRUE(event);
        EXPECT_EQ(event->type, NET_EVENT_MESSAGE);
        EXPECT_EQ(toString(event->data), "everyone");
    }
}

UNIT_TEST(NetHost, GracefulDisconnect) {
    // Disconnecting a client is reported on both ends.
    std::unique_ptr<NetHost> server = NetHost::listen(0, 4);
    std::unique_ptr<NetHost> client = NetHost::connect("127.0.0.1", server->port());

    NetPeerId clientOnServer = -1;
    NetPeerId serverOnClient = -1;
    ASSERT_NO_FATAL_FAILURE(connectBoth(server.get(), client.get(), &clientOnServer, &serverOnClient));

    server->disconnect(clientOnServer);

    std::optional<NetEvent> clientEvent = waitForEvent(client.get(), server.get());
    ASSERT_TRUE(clientEvent);
    EXPECT_EQ(clientEvent->type, NET_EVENT_DISCONNECTED);

    std::optional<NetEvent> serverEvent = waitForEvent(server.get(), client.get());
    ASSERT_TRUE(serverEvent);
    EXPECT_EQ(serverEvent->type, NET_EVENT_DISCONNECTED);
    EXPECT_EQ(serverEvent->peer, clientOnServer);
}

UNIT_TEST(NetHost, DestroyedClientIsReportedRightAway) {
    // A client that shuts down is reported as disconnected without waiting for a timeout.
    std::unique_ptr<NetHost> server = NetHost::listen(0, 4);
    std::unique_ptr<NetHost> client = NetHost::connect("127.0.0.1", server->port());

    NetPeerId clientOnServer = -1;
    NetPeerId serverOnClient = -1;
    ASSERT_NO_FATAL_FAILURE(connectBoth(server.get(), client.get(), &clientOnServer, &serverOnClient));

    client.reset();

    auto start = std::chrono::steady_clock::now();
    std::optional<NetEvent> event = waitForEvent(server.get(), nullptr);
    ASSERT_TRUE(event);
    EXPECT_EQ(event->type, NET_EVENT_DISCONNECTED);
    EXPECT_EQ(event->peer, clientOnServer);
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(1));
}

