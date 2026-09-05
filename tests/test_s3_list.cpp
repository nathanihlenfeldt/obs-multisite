// test_s3_list.cpp — the two mechanisms event listing rests on.
//
// 1. Reading a ListObjectsV2 response. Events are discovered as CommonPrefixes,
//    so the parser must tell those apart from the request's own <Prefix> echo,
//    and must follow pagination rather than silently showing the first page as
//    if it were everything.
// 2. Signing a query string. Every other request in the protocol is a plain
//    PUT/GET with no query at all, so listing is the first thing that exercises
//    canonicalisation — and a signature computed over a double-encoded value is
//    rejected in a way that reads like bad credentials.
#include "../src/core/s3_list_xml.h"
#include "../src/core/aws_sigv4.h"

#include <cstdio>
#include <string>

using namespace multisite;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

int main() {
    // ── Listing events under a room prefix ───────────────────────────────────
    std::printf("Parsing a listing\n");
    {
        // Note the top-level <Prefix> echo, and <KeyCount>: both are traps for
        // a scanner that just looks for the next matching tag.
        const std::string xml = R"(<?xml version="1.0" encoding="UTF-8"?>
<ListBucketResult xmlns="http://s3.amazonaws.com/doc/2006-03-01/">
  <Name>broadcast</Name>
  <Prefix>rooms/main/events/</Prefix>
  <KeyCount>2</KeyCount>
  <MaxKeys>1000</MaxKeys>
  <Delimiter>/</Delimiter>
  <IsTruncated>false</IsTruncated>
  <CommonPrefixes><Prefix>rooms/main/events/01J8ZKQ0000000000000000001/</Prefix></CommonPrefixes>
  <CommonPrefixes><Prefix>rooms/main/events/01J8ZKQ0000000000000000002/</Prefix></CommonPrefixes>
</ListBucketResult>)";

        ListResult r;
        CHECK(parse_list_objects_v2(xml, r), "a listing body parses");
        CHECK(r.common_prefixes.size() == 2, "both events found");
        CHECK(r.common_prefixes.size() == 2 &&
              r.common_prefixes[0] == "rooms/main/events/01J8ZKQ0000000000000000001/",
              "the event prefix keeps its trailing delimiter");
        for (const auto& p : r.common_prefixes)
            CHECK(p != "rooms/main/events/",
                  "the request's own <Prefix> echo is not mistaken for an event");
        CHECK(!r.truncated, "a complete listing is not marked truncated");
    }

    // ── Objects, not just prefixes ───────────────────────────────────────────
    std::printf("Parsing object entries\n");
    {
        const std::string xml = R"(<ListBucketResult>
  <KeyCount>1</KeyCount>
  <IsTruncated>false</IsTruncated>
  <Contents>
    <Key>rooms/main/events/01J8ZKQ.json</Key>
    <LastModified>2026-09-01T10:42:00.000Z</LastModified>
    <Size>184</Size>
  </Contents>
</ListBucketResult>)";
        ListResult r;
        CHECK(parse_list_objects_v2(xml, r), "an object listing parses");
        CHECK(r.keys.size() == 1, "one object listed");
        CHECK(!r.keys.empty() && r.keys[0].key == "rooms/main/events/01J8ZKQ.json",
              "<KeyCount> is not read as a <Key>");
        CHECK(!r.keys.empty() && r.keys[0].size == 184, "size parsed");
        CHECK(!r.keys.empty() &&
              r.keys[0].last_modified == "2026-09-01T10:42:00.000Z",
              "last-modified parsed");
    }

    // ── Pagination ───────────────────────────────────────────────────────────
    // 7-day retention keeps this small today, but a page is a page: the store
    // decides how much it returns, not us.
    std::printf("Pagination\n");
    {
        const std::string xml = R"(<ListBucketResult>
  <IsTruncated>true</IsTruncated>
  <NextContinuationToken>1ueGcxLPRx1Tr/XYExHnhbYLgveDs2J/wm36Hy4vbOwM=</NextContinuationToken>
  <CommonPrefixes><Prefix>events/a/</Prefix></CommonPrefixes>
</ListBucketResult>)";
        ListResult r;
        CHECK(parse_list_objects_v2(xml, r), "a truncated page parses");
        CHECK(r.truncated, "truncation is reported");
        CHECK(r.next_continuation_token == "1ueGcxLPRx1Tr/XYExHnhbYLgveDs2J/wm36Hy4vbOwM=",
              "the continuation token survives verbatim");

        // Truncated but tokenless would page over the same request forever.
        const std::string bad = "<ListBucketResult><IsTruncated>true</IsTruncated>"
                                "</ListBucketResult>";
        ListResult r2;
        parse_list_objects_v2(bad, r2);
        CHECK(!r2.truncated, "truncated-with-no-token does not become an infinite loop");
    }

    // ── Error documents ──────────────────────────────────────────────────────
    // The one an operator will actually hit: a Cloudflare token scoped to
    // objects rather than the bucket.
    std::printf("Error responses\n");
    {
        const std::string xml = R"(<?xml version="1.0" encoding="UTF-8"?>
<Error><Code>AccessDenied</Code><Message>Access Denied</Message></Error>)";
        ListResult r;
        CHECK(!parse_list_objects_v2(xml, r), "an error document is not a listing");
        CHECK(r.error.find("AccessDenied") != std::string::npos,
              "the error code is reported rather than an empty list");
        CHECK(r.common_prefixes.empty(), "no events invented from an error");

        ListResult r2;
        CHECK(!parse_list_objects_v2("not xml at all", r2),
              "junk is rejected");
    }

    // ── Entity decoding ──────────────────────────────────────────────────────
    std::printf("XML entities\n");
    {
        CHECK(xml_decode("a&amp;b") == "a&b", "&amp; decodes");
        CHECK(xml_decode("&lt;x&gt;") == "<x>", "&lt;/&gt; decode");
        CHECK(xml_decode("caf&#233;") == "caf\xe9", "numeric references decode");
        CHECK(xml_decode("100% & rising") == "100% & rising",
              "a bare ampersand is left alone rather than eating the key");

        const std::string xml = "<ListBucketResult><CommonPrefixes>"
            "<Prefix>rooms/main &amp; hall/events/</Prefix>"
            "</CommonPrefixes></ListBucketResult>";
        ListResult r;
        parse_list_objects_v2(xml, r);
        CHECK(r.common_prefixes.size() == 1 &&
              r.common_prefixes[0] == "rooms/main & hall/events/",
              "entities in a room name decode");
    }

    // ── Query-string canonicalisation ────────────────────────────────────────
    std::printf("Signing a query string\n");
    {
        // Sorted by key, regardless of the order the URL was built in.
        CHECK(SigV4Signer::canonical_query("prefix=events/&list-type=2") ==
              "list-type=2&prefix=events%2F",
              "parameters sort by name and values encode");

        // The one that matters: an already-encoded URL must not be encoded
        // twice. S3 decodes before canonicalising, so we must too.
        const std::string raw = SigV4Signer::canonical_query("prefix=rooms/main/events/");
        const std::string enc = SigV4Signer::canonical_query("prefix=rooms%2Fmain%2Fevents%2F");
        CHECK(raw == enc, "an encoded value signs the same as its raw form");
        CHECK(enc == "prefix=rooms%2Fmain%2Fevents%2F", "and neither double-encodes");

        // Continuation tokens are base64: '+', '/' and '=' all appear.
        const std::string tok = "1ueGcxLPRx1Tr/XYExHnhbYLgveDs2J+wm36Hy4vbOwM=";
        CHECK(SigV4Signer::canonical_query(
                  "continuation-token=" + SigV4Signer::uri_encode(tok, true)) ==
              "continuation-token=" + SigV4Signer::uri_encode(tok, true),
              "a base64 continuation token round-trips through canonicalisation");

        // A space in an operator-typed room name must not split the query.
        CHECK(SigV4Signer::canonical_query("prefix=" +
                  SigV4Signer::uri_encode("rooms/main hall/", true)) ==
              "prefix=rooms%2Fmain%20hall%2F",
              "a space in a room name encodes rather than breaking the URL");

        CHECK(SigV4Signer::canonical_query("") == "",
              "no query canonicalises to nothing");
    }

    std::printf("\n%s\n", g_fail == 0 ? "ALL LISTING TESTS PASSED"
                                      : "SOME TESTS FAILED");
    return g_fail == 0 ? 0 : 1;
}
