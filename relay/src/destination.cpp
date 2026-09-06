#include "destination.h"

#include <algorithm>

namespace multisite_relay {

std::string validate(const Destination& d) {
    if (d.name.empty())
        return "Give this destination a name, so you can tell it apart from "
               "the others.";
    if (d.room_id.empty())
        return "Choose which feed this destination should send.";

    std::string u = d.url;
    std::transform(u.begin(), u.end(), u.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    if (u.empty())
        return "Paste the server address from the streaming site — it starts "
               "with rtmp://";
    if (u.rfind("rtmp://", 0) != 0 && u.rfind("rtmps://", 0) != 0)
        return "That does not look like a streaming address. It should start "
               "with rtmp:// or rtmps://";
    // A stream key pasted into the address field is the commonest setup
    // mistake, and it fails at the destination with nothing useful said.
    if (d.stream_key.empty())
        return "Paste the stream key from the streaming site as well. It is "
               "usually shown next to the server address.";

    // Bounds rather than opinions: below about ten seconds there is nothing to
    // absorb a hiccup with, and beyond an hour the operator has almost
    // certainly typed minutes into a seconds box.
    if (d.delay_s != 0 && (d.delay_s < 10 || d.delay_s > 3600))
        return "The delay should be between 10 seconds and an hour.";

    return {};
}

} // namespace multisite_relay
