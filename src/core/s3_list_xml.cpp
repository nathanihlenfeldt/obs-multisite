#include "s3_list_xml.h"

#include <cstdlib>

namespace multisite {

namespace {

// Find the text between <tag> and </tag>, starting at `from`. Returns false if
// the tag is absent. `end_pos` receives the offset just past </tag> so callers
// can scan repeated elements without rescanning from the start.
bool tag_text(const std::string& xml, const std::string& tag, size_t from,
              std::string& text, size_t* end_pos = nullptr) {
    const std::string open = "<" + tag;
    const std::string close = "</" + tag + ">";

    size_t a = xml.find(open, from);
    if (a == std::string::npos) return false;
    // Skip the rest of the opening tag, which may carry attributes. A
    // self-closing element has no text at all.
    size_t gt = xml.find('>', a + open.size());
    if (gt == std::string::npos) return false;
    if (gt > a && xml[gt - 1] == '/') {
        text.clear();
        if (end_pos) *end_pos = gt + 1;
        return true;
    }
    // Guard against matching "<KeyCount>" when looking for "<Key>": the
    // character after the tag name must end it.
    if (a + open.size() >= xml.size()) return false;
    const char after = xml[a + open.size()];
    if (after != '>' && after != ' ' && after != '/' && after != '\t' &&
        after != '\n' && after != '\r') {
        return tag_text(xml, tag, a + open.size(), text, end_pos);
    }

    size_t b = xml.find(close, gt + 1);
    if (b == std::string::npos) return false;
    text = xml.substr(gt + 1, b - (gt + 1));
    if (end_pos) *end_pos = b + close.size();
    return true;
}

// Iterate <container>…</container> blocks, handing each block's body out.
template <typename Fn>
void for_each_block(const std::string& xml, const std::string& tag, Fn fn) {
    size_t pos = 0;
    std::string body;
    size_t end = 0;
    while (tag_text(xml, tag, pos, body, &end)) {
        fn(body);
        pos = end;
    }
}

bool starts_with(const std::string& s, const char* p) {
    return s.compare(0, std::string(p).size(), p) == 0;
}

} // namespace

std::string xml_decode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') { out.push_back(s[i]); continue; }
        size_t semi = s.find(';', i + 1);
        // A bare '&' is not an entity; pass it through rather than losing the
        // rest of the key. Cap the search so a stray '&' cannot swallow a line.
        if (semi == std::string::npos || semi - i > 10) { out.push_back('&'); continue; }
        const std::string ent = s.substr(i + 1, semi - i - 1);
        if      (ent == "amp")  out.push_back('&');
        else if (ent == "lt")   out.push_back('<');
        else if (ent == "gt")   out.push_back('>');
        else if (ent == "quot") out.push_back('"');
        else if (ent == "apos") out.push_back('\'');
        else if (!ent.empty() && ent[0] == '#') {
            const bool hex = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X');
            const long cp = std::strtol(ent.c_str() + (hex ? 2 : 1), nullptr,
                                        hex ? 16 : 10);
            // Only Latin-1 is emitted directly; anything wider would need UTF-8
            // encoding, and object keys we generate never reach there.
            if (cp > 0 && cp < 0x100) out.push_back((char)cp);
            else { out.push_back('&'); continue; }
        } else {
            out.push_back('&');
            continue;                      // unknown entity: leave it literal
        }
        i = semi;
    }
    return out;
}

bool parse_list_objects_v2(const std::string& xml, ListResult& out) {
    // An error is XML too, and a 403 body explains itself far better than the
    // status code does — particularly the missing-ListBucket case.
    if (xml.find("<Error") != std::string::npos) {
        std::string code, msg;
        tag_text(xml, "Code", 0, code);
        tag_text(xml, "Message", 0, msg);
        out.error = code.empty() ? "S3 error response" : xml_decode(code);
        if (!msg.empty()) out.error += ": " + xml_decode(msg);
        return false;
    }

    if (xml.find("<ListBucketResult") == std::string::npos) {
        out.error = "not a ListObjectsV2 response";
        return false;
    }

    for_each_block(xml, "CommonPrefixes", [&](const std::string& block) {
        std::string p;
        if (tag_text(block, "Prefix", 0, p) && !p.empty())
            out.common_prefixes.push_back(xml_decode(p));
    });

    for_each_block(xml, "Contents", [&](const std::string& block) {
        ListEntry e;
        if (!tag_text(block, "Key", 0, e.key) || e.key.empty()) return;
        e.key = xml_decode(e.key);
        std::string sz;
        if (tag_text(block, "Size", 0, sz) && !sz.empty())
            e.size = std::strtoll(sz.c_str(), nullptr, 10);
        tag_text(block, "LastModified", 0, e.last_modified);
        out.keys.push_back(std::move(e));
    });

    std::string trunc;
    if (tag_text(xml, "IsTruncated", 0, trunc))
        out.truncated = starts_with(trunc, "true") || starts_with(trunc, "TRUE");

    std::string token;
    if (tag_text(xml, "NextContinuationToken", 0, token))
        out.next_continuation_token = xml_decode(token);

    // Truncated with no token would page forever on the same request. Treat a
    // store that does that as simply finished rather than looping.
    if (out.truncated && out.next_continuation_token.empty())
        out.truncated = false;

    return true;
}

} // namespace multisite
