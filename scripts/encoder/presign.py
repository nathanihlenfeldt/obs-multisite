#!/usr/bin/env python3
"""presign.py — one pre-signed PUT URL, for probe.sh to send its report to.

The board the probe runs on is in someone else's lab, so probe.sh can upload
its own tarball rather than relying on somebody finding it and emailing it.
This makes the URL it uploads to.

A pre-signed URL is permission to write ONE object, under ONE name, until it
expires. It contains no usable credential: the secret key signs it and is never
part of it. That is what lets it be handed to a third party.

Standard library only, deliberately. There is no aws CLI on the machines this
gets run from, and a bring-up task should not start with installing a toolchain.

  export MULTISITE_S3_KEY=...  MULTISITE_S3_SECRET=...
  ./presign.py --endpoint https://<account>.r2.cloudflarestorage.com \
               --bucket my-bucket --key encoder-probe/rk3588-a.tar.gz

Prints the URL, and the exact command to send the vendor.

DO NOT commit the output, or paste it anywhere public. Until it expires,
anyone holding it can write that one object.
"""
import argparse, datetime, hashlib, hmac, os, sys
from urllib.parse import quote

ALGO = "AWS4-HMAC-SHA256"


def _sign(key: bytes, msg: str) -> bytes:
    return hmac.new(key, msg.encode("utf-8"), hashlib.sha256).digest()


def signing_key(secret: str, datestamp: str, region: str, service: str) -> bytes:
    k = _sign(("AWS4" + secret).encode("utf-8"), datestamp)
    k = _sign(k, region)
    k = _sign(k, service)
    return _sign(k, "aws4_request")


def presign(endpoint, bucket, key, access_key, secret, region="auto",
            expires=604800, method="PUT", now=None, service="s3",
            host_style=False):
    """A SigV4 query-string-signed URL. Mirrors what S3 itself recomputes."""
    now = now or datetime.datetime.now(datetime.timezone.utc)
    amzdate = now.strftime("%Y%m%dT%H%M%SZ")
    datestamp = now.strftime("%Y%m%d")

    scheme, _, rest = endpoint.partition("://")
    host = rest.rstrip("/")
    # Path style (endpoint/bucket/key) is what R2, MinIO and most S3-compatible
    # endpoints take. Virtual-host style is only needed for real AWS buckets
    # whose name is part of the hostname.
    if host_style:
        host = f"{bucket}.{host}"
        canonical_uri = "/" + quote(key, safe="/~")
    else:
        canonical_uri = "/" + quote(f"{bucket}/{key}", safe="/~")

    credential = f"{access_key}/{datestamp}/{region}/{service}/aws4_request"
    params = {
        "X-Amz-Algorithm": ALGO,
        "X-Amz-Credential": credential,
        "X-Amz-Date": amzdate,
        "X-Amz-Expires": str(expires),
        "X-Amz-SignedHeaders": "host",
    }
    # Sorted, and each component encoded — the order and the encoding are both
    # part of what is signed, so getting either wrong gives a 403 that says
    # nothing useful.
    canonical_qs = "&".join(
        f"{quote(k, safe='-_.~')}={quote(v, safe='-_.~')}"
        for k, v in sorted(params.items()))

    canonical_request = "\n".join([
        method, canonical_uri, canonical_qs,
        f"host:{host}\n", "host", "UNSIGNED-PAYLOAD",
    ])
    string_to_sign = "\n".join([
        ALGO, amzdate, f"{datestamp}/{region}/{service}/aws4_request",
        hashlib.sha256(canonical_request.encode("utf-8")).hexdigest(),
    ])
    signature = hmac.new(signing_key(secret, datestamp, region, service),
                         string_to_sign.encode("utf-8"),
                         hashlib.sha256).hexdigest()
    return f"{scheme}://{host}{canonical_uri}?{canonical_qs}&X-Amz-Signature={signature}"


def main():
    p = argparse.ArgumentParser(description="A pre-signed PUT URL for probe.sh.")
    p.add_argument("--endpoint", required=True,
                   help="https://<account>.r2.cloudflarestorage.com, or your S3 endpoint")
    p.add_argument("--bucket", required=True)
    p.add_argument("--key", required=True,
                   help="object name, e.g. encoder-probe/rk3588-a-2026-09-11.tar.gz")
    p.add_argument("--region", default="auto", help="R2 uses 'auto' (default)")
    p.add_argument("--expires", type=int, default=604800,
                   help="seconds, max 604800 (7 days, the SigV4 limit)")
    p.add_argument("--host-style", action="store_true",
                   help="bucket as a subdomain; needed for real AWS, not R2")
    p.add_argument("--get", action="store_true",
                   help="sign a GET instead, to fetch the uploaded report back")
    a = p.parse_args()

    access_key = os.environ.get("MULTISITE_S3_KEY")
    secret = os.environ.get("MULTISITE_S3_SECRET")
    if not access_key or not secret:
        sys.exit("set MULTISITE_S3_KEY and MULTISITE_S3_SECRET first "
                 "(kept out of the arguments so they stay out of shell history)")
    if not 1 <= a.expires <= 604800:
        sys.exit("--expires must be between 1 and 604800 seconds")

    method = "GET" if a.get else "PUT"
    url = presign(a.endpoint, a.bucket, a.key, access_key, secret,
                  region=a.region, expires=a.expires, method=method,
                  host_style=a.host_style)
    print(url)
    print()
    if a.get:
        print("Fetch the report back with:")
        print()
        print(f"  curl -fsSL -o {a.key.split('/')[-1]} '{url}'")
    else:
        raw = "https://raw.githubusercontent.com/stageaudioworks/obs-multisite/main"
        print("Send the vendor this, as one line:")
        print()
        print(f"  curl -fsSL {raw}/scripts/encoder/probe.sh \\")
        print(f"    | sudo bash -s -- --upload-url '{url}'")
    print()
    print(f"Valid for {a.expires // 3600}h. One board per URL — a second run "
          "against it overwrites the first.")
    print("Do not paste it anywhere public.")


if __name__ == "__main__":
    main()
