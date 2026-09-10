// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_thread.h"

#include <obs-module.h>

#include <chrono>
#include <future>
#include <memory>
#include <utility>

namespace multisite_obs {

namespace {

// Heap-owned, because the task may outlive the caller that queued it: if OBS
// never gets to it inside the timeout, the request thread walks away and this
// is the only thing keeping the work — and whatever it writes into — alive.
struct UiTask {
    std::function<void()> fn;
    std::promise<void>    done;
};

void run_ui_task(void* param) {
    std::unique_ptr<UiTask> task(static_cast<UiTask*>(param));
    try {
        if (task->fn) task->fn();
    } catch (...) {
        // Nothing useful to do here: the caller reads the outcome from the
        // state it passed in, and an exception escaping an OBS task callback
        // would take the front end with it.
    }
    task->done.set_value();
}

} // namespace

bool run_on_ui_thread(const std::function<void()>& fn, int timeout_ms) {
    if (!fn) return false;

    auto task = std::make_unique<UiTask>();
    task->fn = fn;
    std::future<void> done = task->done.get_future();

    obs_queue_task(OBS_TASK_UI, run_ui_task, task.release());

    return done.wait_for(std::chrono::milliseconds(timeout_ms)) ==
           std::future_status::ready;
}

} // namespace multisite_obs