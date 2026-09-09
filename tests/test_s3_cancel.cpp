// SPDX-License-Identifier: GPL-3.0-or-later
// test_s3_cancel.cpp — proves cancel_pending() actually aborts an in-flight
// request quickly, rather than trusting that wiring curl's progress callback
// was enough.
//
// The incident this exists for: tearing a Multisite source down while
// poll_loop happened to be mid-request blocked whichever thread called
// stop_workers() — usually OBS's own UI thread, during Quit — for as long as
// that one request had left to run, up to its full timeout. Long enough that
// an operator saw OBS stop responding and force-quit it, which OBS then
// reports as a crash on the next launch. Confirmed from real logs: three
// separate OBS sessions never printed "poll loop exiting" before the process
// ended, the one log line that final teardown step depends on.
//
// A real S3 endpoint can't be made to stall on demand, so this stands up a
// bare TCP listener that accepts a connection and then simply never answers
// it — the exact shape of "connected, then the far end went silent" that a
// stalled link or a link R2 has stopped serving looks like from libcurl's
// side. POSIX sockets only; this is not built on Windows (see CMakeLists.txt).
#include "s3_transport.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

using namespace multisite;
using Clock = std::chrono::steady_clock;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

int main() {
    // A listener that accepts one connection and then goes quiet forever —
    // never reads the request, never writes a response. Whatever curl call
    // lands here gets exactly as far as "connected", then waits.
    int srv = ::socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { std::printf("socket() failed\n"); return 1; }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;                          // let the OS pick a free port
    if (::bind(srv, (sockaddr*)&addr, sizeof(addr)) != 0) {
        std::printf("bind() failed\n"); return 1;
    }
    socklen_t alen = sizeof(addr);
    ::getsockname(srv, (sockaddr*)&addr, &alen);
    const int port = ntohs(addr.sin_port);
    ::listen(srv, 1);

    std::atomic<bool> accepted{false};
    std::thread listener([&] {
        int c = ::accept(srv, nullptr, nullptr);
        accepted = true;
        if (c >= 0) {
            // Hold the connection open without reading or writing anything,
            // long enough that the test's own timeouts fire first.
            std::this_thread::sleep_for(std::chrono::seconds(20));
            ::close(c);
        }
    });

    S3Config cfg;
    cfg.endpoint_host = "127.0.0.1:" + std::to_string(port);
    cfg.use_https = false;
    cfg.bucket = "test-bucket";
    cfg.access_key_id = "k";
    cfg.secret_access_key = "s";
    // Generous on purpose: the test proves cancellation finishes well before
    // this, not that a short timeout was doing the work.
    cfg.connect_timeout_ms = 8000;
    cfg.request_timeout_ms = 8000;

    S3Transport transport(cfg);

    std::printf("== cancel_pending() aborts a request stalled on the server's silence ==\n");
    {
        GetResult result;
        std::atomic<bool> done{false};
        auto t0 = Clock::now();
        std::thread worker([&] {
            result = transport.get("some/key");
            done = true;
        });

        // Give curl time to connect and start waiting on a response — and the
        // progress callback time to fire at least once.
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        CHECK(accepted.load(), "the server actually accepted the connection "
                               "before cancellation (test is exercising the "
                               "right state)");

        transport.cancel_pending();
        worker.join();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - t0).count();

        std::printf("      elapsed: %lldms (timeout configured: %dms)\n",
                    (long long)elapsed, cfg.request_timeout_ms);
        CHECK(elapsed < 3000,
              "the request returned in well under its configured timeout");
        CHECK(!result.success, "a cancelled request is reported as failed, "
                               "not silently as an empty success");
    }

    ::shutdown(srv, SHUT_RDWR);
    ::close(srv);
    listener.join();

    std::printf("\n%s\n", g_fail == 0 ? "ALL S3 CANCEL TESTS PASSED"
                                      : "SOME TESTS FAILED");
    return g_fail == 0 ? 0 : 1;
}
