#pragma once
//
// s3_list_xml.h — extraction of the handful of fields we need from a
// ListObjectsV2 response.
//
// Deliberately not a general XML parser. We read one well-known response shape
// from a service we control the requests to, and need exactly four things from
// it: the common prefixes, the keys, whether the result was truncated, and the
// continuation token. A dozen lines of tag scanning is a smaller liability than
// a parser dependency in the reliability core.
//
// Lives in the core (not the curl-linked S3 transport) so it can be tested
// against captured responses with no network and no libcurl.
//
#include "transport.h"
#include <string>

namespace multisite {

// Parse a ListObjectsV2 body into `out`, populating common_prefixes, keys,
// truncated and next_continuation_token. Returns false if the body is not a
// listing — including the case where it is an S3 <Error> document, whose Code
// and Message are then reported in out.error.
//
// Does not set out.success or out.http_status; the transport owns those.
bool parse_list_objects_v2(const std::string& xml, ListResult& out);

// Decode the XML entities S3 uses in keys (&amp; &lt; &gt; &quot; &apos; and
// numeric character references). Exposed for testing.
std::string xml_decode(const std::string& s);

} // namespace multisite
