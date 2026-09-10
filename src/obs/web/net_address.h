// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// net_address.h — the address this machine is reached at, on the local network.
//
// The remote-control pages are useless unless somebody can be told what to type
// into a phone, and "localhost" is never the answer: the phone is not this
// machine. Kept in its own file because finding out means opening a socket, and
// a translation unit that includes Winsock must not also include the Windows
// headers OBS arrives with.
//
#include <string>

namespace multisite_obs {

// The IPv4 address a packet to the internet would leave from, which is the one
// a device on the same network reaches this machine at. Empty when there is no
// network at all — a machine with no connection has no address to offer, and
// guessing one would only send somebody to a page that does not answer.
std::string local_ipv4();

} // namespace multisite_obs