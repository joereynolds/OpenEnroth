#pragma once

#include "Utility/Memory/Blob.h"

#include "NetEnums.h"

using NetPeerId = int;

struct NetEvent {
    NetEventType type = NET_EVENT_MESSAGE;
    NetPeerId peer = -1;
    Blob data; // Message payload, empty for connection events.
};
