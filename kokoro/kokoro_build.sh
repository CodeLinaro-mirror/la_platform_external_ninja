#!/bin/bash
set -e

top=$(cd $(dirname $0)/../../.. && pwd)
ninja_src=$top/external/ninja

# On Linux, enter the Docker container and reinvoke this script.
if [ "$(uname)" == "Linux" -a "$SKIP_DOCKER" == "" ]; then
    docker build -t ndk-ninja $ninja_src/kokoro
    export SKIP_DOCKER=1
    docker run -v$top:$top -eKOKORO_BUILD_ID -eSKIP_DOCKER \
      --entrypoint $ninja_src/kokoro/kokoro_build.sh \
      ndk-ninja
    exit $?
fi

$ninja_src/kokoro/build.py
