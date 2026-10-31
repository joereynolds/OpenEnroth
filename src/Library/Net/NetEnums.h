#pragma once

enum class NetChannel {
    NET_CHANNEL_RELIABLE, // Delivered exactly once, in order.
    NET_CHANNEL_UNRELIABLE, // May be dropped, and a packet that arrives after a newer one is dropped too.
};
using enum NetChannel;

enum class NetEventType {
    NET_EVENT_CONNECTED,
    NET_EVENT_DISCONNECTED,
    NET_EVENT_MESSAGE,
};
using enum NetEventType;
