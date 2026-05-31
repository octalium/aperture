#!/usr/bin/env python3
"""Print the AppStream release date.

Honors SOURCE_DATE_EPOCH (UTC, formatted YYYY-MM-DD) when set in the
environment; otherwise prints the hardcoded fallback passed as argv[1].
"""

import os
import sys
import time


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: release-date.py FALLBACK_YYYY_MM_DD", file=sys.stderr)
        return 2

    fallback = sys.argv[1]
    epoch = os.environ.get("SOURCE_DATE_EPOCH", "").strip()
    if epoch:
        try:
            print(time.strftime("%Y-%m-%d", time.gmtime(int(epoch))))
            return 0
        except ValueError:
            print(f"invalid SOURCE_DATE_EPOCH: {epoch!r}", file=sys.stderr)
            return 1

    print(fallback)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
