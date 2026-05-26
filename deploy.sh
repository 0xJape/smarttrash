#!/usr/bin/env bash
# Sync the FastAPI server to the Jetson over SSH.
set -euo pipefail

HOST="${HOST:-kllew@kllewai.local}"
KEY="${KEY:-ubuntu_server.pem}"
DEST="${DEST:-~/smartrash}"

ssh -i "$KEY" -o StrictHostKeyChecking=no "$HOST" \
    "mkdir -p $DEST/server/templates $DEST/server/deploy"

scp -i "$KEY" -o StrictHostKeyChecking=no \
    server/app.py server/requirements.txt server/README.md \
    "$HOST:$DEST/server/"

scp -i "$KEY" -o StrictHostKeyChecking=no \
    server/templates/dashboard.html \
    "$HOST:$DEST/server/templates/"

scp -i "$KEY" -o StrictHostKeyChecking=no \
    server/deploy/smartrash-api.service \
    server/deploy/cloudflared.service \
    server/deploy/cloudflared-config.example.yml \
    server/deploy/smartrash.env.example \
    "$HOST:$DEST/server/deploy/"

echo "Server synced to $HOST:$DEST/server"
echo "Next: ssh in and follow server/README.md from step 2 onward."
