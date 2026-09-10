// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// ui_thread.h — handing work to the thread OBS runs its interface on.
//
// The docks drive the broadcast controller from OBS's UI thread, and the
// controller was written with that assumption. A request that arrives over the
// network is on neither that thread nor any thread OBS owns, so anything that
// would change what is on air is handed to the queue OBS already runs its UI
// work on. That serialises it with the dock rather than racing it, and the
// caller still gets the outcome inside the same HTTP response — the rule the
// whole remote surface follows: an operator pressing a button sees the answer
// at once, not at the next poll.
//
#include <functional>

namespace multisite_obs {

// Runs `fn` on OBS's UI thread and waits for it. Returns false if OBS did not
// get to it within `timeout_ms`: a front end that is busy or stuck must leave
// the page saying so, rather than leaving a request thread waiting for ever.
//
// `fn` may still run later even when this returns false, so anything it touches
// has to outlive the call — capture shared ownership, never a reference to a
// local in the handler that gave up waiting.
bool run_on_ui_thread(const std::function<void()>& fn, int timeout_ms = 15000);

} // namespace multisite_obs