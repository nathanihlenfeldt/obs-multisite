#!/bin/sh
# The core must remain free of OBS and Qt: it is shared with the planned
# headless appliance, which links neither. A convention in a README erodes;
# this fails the build instead.
set -e
core_dir="${1:-src/core}"
bad=0

for pattern in "obs-module.h" "obs-frontend-api.h" "obs.h" "QWidget" "QObject" "Qt6"; do
  if grep -rl "$pattern" "$core_dir" 2>/dev/null | grep -v vendor/ > /tmp/hits.txt; then
    if [ -s /tmp/hits.txt ]; then
      echo "FAIL: core includes '$pattern':"
      sed 's/^/    /' /tmp/hits.txt
      bad=1
    fi
  fi
done

if [ "$bad" -eq 0 ]; then
  echo "core is free of OBS and Qt — safe to reuse in the headless appliance"
fi
exit "$bad"
