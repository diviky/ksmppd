#!/bin/bash
set -e

# Ensure library path is set for Kannel dependencies
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-/usr/local/kannel/lib:/usr/local/lib}"

# Execute the command (default: ksmppd with config)
exec "$@"
