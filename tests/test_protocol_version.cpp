// SPDX-License-Identifier: GPL-3.0-or-later
// test_protocol_version.cpp — the storage protocol's own version number.
//
// Two properties matter here and they pull in opposite directions, which is why
// they are pinned together.
//
// Backward: every bucket written before this field existed must keep working,
// untouched, for ever. Those documents have no `protocol_version`, and an
// absent field must read as 1 rather than as 0, as an error, or as a throw. A
// church's recordings from last year are exactly what someone wants to play
// back, and a version field that broke them would be worse than no version
// field at all.
//
// Forward: a document from a protocol this build does not understand must be
// refused in a way that says so. The failure this exists to prevent is a
// satellite that half-reads a newer bucket and shows a stutter, a failed
// checksum, or a wrong clock time — symptoms that send an operator looking at
// the network while the actual answer is that the plugin is too old.
#include "../src/core/model.h"

#include <cstdio>
#include <string>

using namespace multisite;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

int main() {
    std::printf("== a document with no version is version 1 ==\n");
    {
        // Exactly what an encoder wrote before this field existed. Every one of
        // these must parse, and parse as 1.
        CHECK(LivePointer::from_json(
                  R"({"room_id":"main","event_id":"01J8","status":"live"})")
                  .protocol_version == 1,
              "live.json without the field is 1");
        CHECK(EventInfo::from_json(
                  R"({"event_id":"01J8","room_id":"main","started_at_ms":1000})")
                  .protocol_version == 1,
              "event.json without the field is 1");
        CHECK(RoomEventEntry::from_json(
                  R"({"event_id":"01J8","room_id":"main"})")
                  .protocol_version == 1,
              "the room index entry without the field is 1");
        CHECK(Manifest::from_json(
                  R"({"event_id":"01J8","status":"live","latest_seq":42})")
                  .protocol_version == 1,
              "manifest.json without the field is 1");
        CHECK(MarkerList::from_json(R"({"markers":[]})").protocol_version == 1,
              "markers.json without the field is 1");

        // And the rest of the document still parses, which is the point — the
        // version must be additive, not a gate on old content.
        const Manifest m = Manifest::from_json(
            R"({"event_id":"01J8","status":"live","latest_seq":42})");
        CHECK(m.event_id == "01J8" && m.latest_seq == 42,
              "an unversioned manifest still reads its own fields");
    }

    std::printf("== what this build writes, it can read ==\n");
    {
        LivePointer p; p.room_id = "main"; p.event_id = "01J8";
        CHECK(LivePointer::from_json(p.to_json()).protocol_version == kProtocolVersion,
              "live.json round-trips the current version");

        EventInfo e; e.event_id = "01J8"; e.room_id = "main";
        CHECK(EventInfo::from_json(e.to_json()).protocol_version == kProtocolVersion,
              "event.json round-trips the current version");

        RoomEventEntry r; r.event_id = "01J8"; r.room_id = "main";
        CHECK(RoomEventEntry::from_json(r.to_json()).protocol_version == kProtocolVersion,
              "the room index entry round-trips the current version");

        Manifest mf; mf.event_id = "01J8";
        CHECK(Manifest::from_json(mf.to_json()).protocol_version == kProtocolVersion,
              "manifest.json round-trips the current version");

        MarkerList ml;
        CHECK(MarkerList::from_json(ml.to_json()).protocol_version == kProtocolVersion,
              "markers.json round-trips the current version");

        // The field is actually on the wire, not merely surviving as a default.
        CHECK(mf.to_json().find("\"protocol_version\"") != std::string::npos,
              "the version is written into the document, not assumed");
    }

    std::printf("== an unclear version reads as the oldest protocol ==\n");
    {
        // This arrives from a bucket that any encoder version — or anything
        // else with write access — may have written. None of it may throw, and
        // none of it may be taken as a version we would then refuse to read.
        const char* junk[] = {
            R"({"protocol_version":"2"})",      // a string
            R"({"protocol_version":null})",
            R"({"protocol_version":0})",
            R"({"protocol_version":-5})",
            R"({"protocol_version":1.5})",      // not an integer
            R"({"protocol_version":[2]})",
            R"({"protocol_version":{"v":2}})",
            R"({"protocol_version":true})",
        };
        bool all_one = true;
        for (const char* s : junk) {
            int got = 0;
            try { got = Manifest::from_json(s).protocol_version; }
            catch (...) { all_one = false;
                          std::printf("        (threw on %s)\n", s); continue; }
            if (got != 1) { all_one = false;
                            std::printf("        (read %s as %d)\n", s, got); }
        }
        CHECK(all_one, "every unclear version reads as 1 and none throws");
    }

    std::printf("== older is readable, newer is not ==\n");
    {
        CHECK(protocol_readable(1), "version 1 is readable");
        CHECK(protocol_readable(kProtocolVersion),
              "what this build writes is readable by it");
        CHECK(!protocol_readable(kProtocolVersion + 1),
              "one version newer is refused");
        CHECK(!protocol_readable(kProtocolVersion + 99),
              "far newer is refused");
        // Older must never be refused: we go on reading what we once wrote.
        for (int v = 1; v <= kProtocolVersion; ++v)
            CHECK(protocol_readable(v),
                  ("every version up to the current one stays readable (" +
                   std::to_string(v) + ")").c_str());
    }

    std::printf("== a newer manifest is refused, not half-read ==\n");
    {
        // The decoder's guard reads the version off a parsed manifest, so the
        // parse itself must still succeed — it is the caller that refuses.
        // Confirming this here means the guard in decoder_session.cpp is
        // checking something that actually arrives.
        const Manifest m = Manifest::from_json(
            R"({"protocol_version":999,"event_id":"01J8","latest_seq":7})");
        CHECK(m.protocol_version == 999, "a newer version parses as itself");
        CHECK(!protocol_readable(m.protocol_version),
              "and is then refused by the readability rule");
    }

    std::printf(g_fail ? "\nFAILED: %d\n" : "\nAll passed\n", g_fail);
    return g_fail ? 1 : 0;
}
