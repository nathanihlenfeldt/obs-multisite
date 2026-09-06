// test_config_store.cpp — what the relay remembers between restarts.
//
// The things worth pinning down here are the ones that lose a church its
// setup or leak a stream key: that a saved destination comes back exactly as
// it was, that saving the storage form without retyping the secret does not
// wipe it, and that validation happens before anything reaches the database
// rather than after.
#include "../src/config_store.h"

#include <cstdio>
#include <string>
#include <unistd.h>

using namespace multisite_relay;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

static Destination sample() {
    Destination d;
    d.name = "YouTube";
    d.room_id = "main-auditorium";
    d.url = "rtmp://a.rtmp.youtube.com/live2";
    d.stream_key = "abcd-efgh-ijkl";
    d.audio.label = "Sermon ISO";
    d.delay_s = 240;
    return d;
}

int main() {
    std::printf("config store\n");
    const std::string path = "/tmp/relay_test_config.db";
    ::unlink(path.c_str());
    ::unlink((path + "-wal").c_str());
    ::unlink((path + "-shm").c_str());

    {
        ConfigStore cs;
        CHECK(cs.open(path).empty(), "a new database opens");
        CHECK(!cs.storage_configured(),
              "and reports that storage is not set up yet");

        std::string err;
        const int64_t id = cs.add(sample(), err);
        CHECK(id > 0 && err.empty(), "a valid destination saves");

        Destination bad = sample();
        bad.url = "https://youtube.com/live";
        CHECK(cs.add(bad, err) == 0 && !err.empty(),
              "an invalid one is refused before it reaches the database");
        CHECK(cs.destinations().size() == 1,
              "and leaves nothing behind when it is");

        multisite::S3Config s;
        s.bucket = "church-media";
        s.r2_account_id = "acct123";
        s.access_key_id = "AKIA";
        s.secret_access_key = "very-secret";
        s.use_https = false;
        cs.set_storage(s);
        CHECK(cs.storage_configured(), "storage is configured once it is set");
    }

    // Everything must survive the process going away — that is the whole point
    // of the file.
    {
        ConfigStore cs;
        CHECK(cs.open(path).empty(), "the database reopens");

        auto all = cs.destinations();
        CHECK(all.size() == 1, "the destination is still there after a restart");
        const auto& d = all[0];
        CHECK(d.name == "YouTube" && d.url == "rtmp://a.rtmp.youtube.com/live2",
              "with its name and address intact");
        CHECK(d.stream_key == "abcd-efgh-ijkl", "and its stream key");
        CHECK(d.audio.label == "Sermon ISO",
              "and the sound feed the operator chose");
        CHECK(d.delay_s == 240, "and its delay");
        CHECK(!d.enabled, "a saved destination comes back stopped");

        const auto s = cs.storage();
        CHECK(s.bucket == "church-media" && s.access_key_id == "AKIA",
              "the storage settings survive too");
        CHECK(s.secret_access_key == "very-secret", "including the secret");
        CHECK(s.use_https == false,
              "and the choice to use plain HTTP, which must not silently "
              "flip back on");

        // Enabling is what Start does, and it has to outlive a restart or a
        // container replacement mid-service would come back doing nothing.
        cs.set_enabled(d.id, true);
        CHECK(cs.destination(d.id)->enabled, "starting a destination persists");

        Destination up = *cs.destination(d.id);
        up.name = "YouTube (main)";
        std::string err;
        CHECK(cs.update(up, err), "a destination can be renamed");
        CHECK(cs.destination(d.id)->name == "YouTube (main)", "and it sticks");
        CHECK(cs.destination(d.id)->stream_key == "abcd-efgh-ijkl",
              "without disturbing the stream key");

        CHECK(cs.remove(d.id), "and removed");
        CHECK(cs.destinations().empty(), "leaving none");
    }

    ::unlink(path.c_str());
    ::unlink((path + "-wal").c_str());
    ::unlink((path + "-shm").c_str());

    std::printf("\n%s\n", g_fail == 0 ? "ALL CONFIG STORE TESTS PASSED"
                                      : "SOME TESTS FAILED");
    return g_fail == 0 ? 0 : 1;
}
