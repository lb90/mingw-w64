#!/usr/bin/env bash
set -e

if [[ -z "$WindowsSdkDir" ]]; then
  echo "This script must be run with an initialized MSVC Build Tools environment"
  exit 1
fi

SDKDIR=`cygpath -u "$WindowsSdkDir"`

diff -r --strip-trailing-cr mingw-w64-headers/include/winrt "${SDKDIR}/Include/$SDKVER/cppwinrt/winrt"
