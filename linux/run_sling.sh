#!/bin/bash

fatal_error() {
  >&2 echo $1
  exit 1
}

if [ -z "$SLING_BACKEND" ]; then
  fatal_error "Please specify the URL to the backend in SLING_BACKEND"
fi

SLING_PATH="${SLING_PATH:-./sling}"

if [ ! -x "$SLING_PATH" ]; then
  fatal_error "Could not find Sling at $SLING_PATH"
fi

SLING_DIR="$(dirname "$SLING_PATH")"
export SLING_KEY="$SLING_DIR/key.pem"
export SLING_CERT="$SLING_DIR/cert.pem"

export SLING_HOST="$(curl -s "$SLING_BACKEND"/mqtt_endpoint)"
export SLING_DEVICE_ID="$(curl -s "$SLING_BACKEND"/client_id)"

echo "Device ID: $SLING_DEVICE_ID"
echo "Endpoint: $SLING_HOST"

curl -s "$SLING_BACKEND"/key > $SLING_KEY || fatal_error "Failed to retrieve client key"
curl -s "$SLING_BACKEND"/cert > $SLING_CERT || fatal_error "Failed to retrieve client certificate"

exec "$SLING_PATH" "$@"
