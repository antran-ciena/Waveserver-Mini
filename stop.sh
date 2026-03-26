#!/bin/bash
set -euo pipefail

# Stop all Waveserver Mini backend services if they are running.
pkill -f '/port_manager$' 2>/dev/null || true
pkill -f '/conn_manager$' 2>/dev/null || true
pkill -f '/traffic_manager$' 2>/dev/null || true

echo "Stopped Waveserver Mini services (if running)."
