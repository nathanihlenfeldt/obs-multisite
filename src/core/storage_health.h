// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// storage_health.h — what the object store tells us about itself, and how
// fast it is answering.
//
// Deliberately free of libcurl. The transport does the talking; the parsing
// and the arithmetic live here, where they build on every platform and are
// covered by the ordinary test suite. A campus with a picture that stutters is
// nearly always a question about the link, and the answers to that question
// should not be the one part of the code nothing can test.
//
#include <cstdint>
#include <string>

namespace multisite {

// The Cloudflare edge that served a request, taken from the `cf-ray` response
// header. R2 sends `cf-ray: <id>-JNB`, where the suffix is the IATA-style code
// of the colo — JNB for Johannesburg, CPT for Cape Town, AMS for Amsterdam.
//
// This is the single most useful figure for a church in Africa on an S3
// endpoint that resolves wherever Cloudflare feels like: a box in Johannesburg
// being served from AMS explains an otherwise baffling latency, and no amount
// of local diagnosis finds it. Returns "" for anything that is not a colo
// code, including a header from a store that is not Cloudflare.
std::string cloudflare_colo(const std::string& cf_ray);

// Case-insensitive "does this header line start with this name" plus the value
// after the colon, trimmed. Header lines arrive from libcurl complete with
// their trailing CRLF.
bool header_is(const std::string& line, const char* name, std::string& value);

// Observed transfer rate, smoothed.
//
// A single segment's rate is a poor guide — it includes connection setup and
// whatever else the venue's link was doing that second — and a plain average
// over the session hides the link getting worse, which is the thing worth
// noticing during a service. So an exponentially-weighted mean: recent
// requests dominate, one slow fetch does not panic the display.
class RateMeter {
public:
    explicit RateMeter(double smoothing = 0.3) : m_alpha(smoothing) {}

    // One completed transfer. Ignored if it moved too little or took too
    // little time to mean anything: a 200-byte live.json fetched in 4 ms says
    // more about latency than throughput, and letting it in made the figure
    // swing wildly between segments.
    void add(uint64_t bytes, double seconds);

    // Smoothed bytes per second, or 0 if nothing has been measured.
    double bytes_per_s() const { return m_rate; }
    bool   has_measurement() const { return m_samples > 0; }
    uint64_t samples() const { return m_samples; }

private:
    double   m_alpha;
    double   m_rate = 0.0;
    uint64_t m_samples = 0;
};

} // namespace multisite
