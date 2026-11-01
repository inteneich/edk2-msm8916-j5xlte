#!/bin/bash
# usage: ./build_rel.sh [device]
exec "$(dirname "$0")/build.sh" "${1:-j5nlte}" RELEASE
