#!/bin/bash
# Wrapper script for slim2upnp systemd/OpenRC service.
# Reads configuration and builds the command line.

# Source config file
if [ -f /etc/default/slim2upnp ]; then
    . /etc/default/slim2upnp
elif [ -f /etc/conf.d/slim2upnp ]; then
    . /etc/conf.d/slim2upnp
fi

# Build command-line arguments (array preserves spaces in values)
ARGS=()

if [ -n "$RENDERER_URL" ]; then
    ARGS+=(--renderer-url "$RENDERER_URL")
elif [ -n "$RENDERER" ]; then
    ARGS+=(-r "$RENDERER")
fi

[ -n "$LMS_SERVER" ]   && ARGS+=(-s "$LMS_SERVER")
[ -n "$PLAYER_NAME" ]  && ARGS+=(-n "$PLAYER_NAME")
[ "$NO_DSD" = "yes" ]  && ARGS+=(--no-dsd)
[ -n "$DECODER" ]      && ARGS+=(--decoder "$DECODER")
[ -n "$INTERFACE" ]    && ARGS+=(--interface "$INTERFACE")
[ -n "$HTTP_PORT" ]    && ARGS+=(--http-port "$HTTP_PORT")
[ "$VERBOSE" = "yes" ] && ARGS+=(-v)

exec /usr/local/bin/slim2upnp "${ARGS[@]}"
