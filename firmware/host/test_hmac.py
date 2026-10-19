#!/usr/bin/env python3
"""Lock the JARVIS1 HMAC vector. The same bytes are checked in worker/test."""

import hashlib
import hmac
import sys

BODY = b'{"text":"hello"}'
DIGEST = "cbbbdcd27692344de5dbab3abcaba413fb0f45307267de7081401576df1cb176"
SIGNATURE = "bf4779d5fb191b1473cfdc44e099a9c5716e647b6aed119584d784eee1e795c6"


def main() -> int:
    digest = hashlib.sha256(BODY).hexdigest()
    if digest != DIGEST:
        print("body hash drifted", file=sys.stderr)
        return 1
    message = "JARVIS1\nPOST\n/testprefix/ask\ndevice-1\n1760000000\nnonce-1\n" + digest
    signature = hmac.new(b"test-hmac-key", message.encode(), hashlib.sha256).hexdigest()
    if signature != SIGNATURE:
        print("signature drifted", file=sys.stderr)
        return 1
    print("hmac ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
