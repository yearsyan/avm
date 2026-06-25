#!/bin/bash
# We forcefully use bash to make sure we can find the proper location of this script
# So we can add ninja to the path, and setup the ASAN_OPTIONS

PROGDIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null && pwd )"
export ASAN_OPTIONS=$(cat ${PROGDIR}/asan_overrides)

OS="linux-x86"
if [ "$(uname)" == "Darwin" ]; then
  OS="darwin-x86"
fi
LOCAL_NINJA_PATH="$PROGDIR/../../../prebuilts/ninja/$OS"

if [ $(which ninja) ] ; then
  LOCAL_VER=$("$LOCAL_NINJA_PATH"/ninja --version 2>/dev/null || echo "Error: Local Ninja not found")
  GLOBAL_VER=$(ninja --version 2>/dev/null || echo "Error: Global Ninja not found")
  if [ "$LOCAL_VER" = "$GLOBAL_VER" ]; then
    echo "Ninja is already on the path, no need to add it."
  else
    echo "Global ninja version is different, setting up PATH"
    export PATH=$LOCAL_NINJA_PATH:$PATH
  fi
else
  export PATH=$LOCAL_NINJA_PATH:$PATH
fi
