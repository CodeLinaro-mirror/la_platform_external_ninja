#!/bin/bash
set -e
set -x

kokoro_dir=$(cd $(dirname $0) && pwd)
top=$(cd $kokoro_dir/../../.. && pwd)
$top/toolchain/ndk-kokoro/kokoro_build.sh $kokoro_dir $kokoro_dir/build.py
