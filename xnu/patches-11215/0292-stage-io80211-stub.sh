#!/usr/bin/env bash
# Stage the IO80211Interface stub (off unless wifi_iokit=1; see the .cpp header).
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
test -d iokit/Kernel/arm
test -f iokit/conf/files
install -m 0644 "$here/raphael_io80211_stub.cpp" iokit/Kernel/arm/RaphaelIO80211.cpp
grep -q 'RaphaelIO80211.cpp' iokit/conf/files || \
	echo 'iokit/Kernel/arm/RaphaelIO80211.cpp              optional iokitcpp' >> iokit/conf/files
