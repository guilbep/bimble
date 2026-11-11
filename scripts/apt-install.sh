#!/usr/bin/env bash
# Install Ubuntu packages on a CI runner, through mirror trouble: apt retries each download; if
# the attempt still fails, the runner's Azure mirror is replaced by Ubuntu's own archive (on
# 11 Nov 2026 the runners couldn't resolve azure.archive.ubuntu.com for 10+ minutes), and the
# whole update + install is tried again, three times in all, 30 s apart.
#   scripts/apt-install.sh pkg1 pkg2 …
set -u
for attempt in 1 2 3; do
  if sudo apt-get -o Acquire::Retries=5 -o Acquire::http::Timeout=30 update -q &&
     sudo apt-get -o Acquire::Retries=5 install -y -q --no-install-recommends "$@"; then
    exit 0
  fi
  echo "apt failed (attempt $attempt of 3); using archive.ubuntu.com, again in 30 s" >&2
  if [ -f /etc/apt/apt-mirrors.txt ]; then
    echo "http://archive.ubuntu.com/ubuntu/" | sudo tee /etc/apt/apt-mirrors.txt >/dev/null
  fi
  sudo sed -i 's|azure\.archive\.ubuntu\.com|archive.ubuntu.com|g' \
    /etc/apt/sources.list /etc/apt/sources.list.d/*.sources 2>/dev/null || true
  sleep 30
done
exit 1
