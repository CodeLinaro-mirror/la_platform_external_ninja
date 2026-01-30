#!/usr/bin/env python3
#
# Copyright 2021 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import os
from pathlib import Path
import shutil
import sys
from typing import List, Union

NINJA_SRC = Path(__file__).parent.parent
TOP = NINJA_SRC.parent.parent

sys.path.append(str(TOP / 'toolchain/ndk-kokoro'))
from build_utils import Host, get_default_host, create_new_dir, run_cmd, zip_dir, LinuxArm64Musl


def main() -> None:
    host = get_default_host()
    exe = '.exe' if host == Host.Windows else ''

    out = TOP / 'out/ninja'
    create_new_dir(out)
    create_new_dir(out / 'build')
    create_new_dir(out / 'artifact')

    prebuilt_cmake = TOP / f'prebuilts/cmake/{host.value}/bin/cmake{exe}'
    prebuilt_ninja = TOP / f'prebuilts/ninja/{host.value}/ninja{exe}'

    cmake_conf_args: List[Union[str, Path]] = [
        prebuilt_cmake,
        NINJA_SRC,
        '-B', out / 'build',
    ]
    cmake_build_args: List[Union[str, Path]] = [
        prebuilt_cmake,
        '--build', out / 'build',
        '--parallel',
        '--config', 'Release',
    ]

    if host == Host.Windows:
        cmake_conf_args += ['-G', 'Visual Studio 17 2022', '-A', 'x64']
    else:
        # Use the Multi-Config generator for consistency with MSVC, which is also multi-config.
        # (e.g. "--config Release" replaces CMAKE_BUILD_TYPE, and there is a "Release" subdir.)
        cmake_conf_args += [
            '-G', 'Ninja Multi-Config',
            f'-DCMAKE_MAKE_PROGRAM={prebuilt_ninja}',
        ]
        if host == Host.Linux:
            ldflags = '-static-libstdc++'
            cmake_conf_args += [
                f'-DCMAKE_EXE_LINKER_FLAGS={ldflags}',
                f'-DCMAKE_SHARED_LINKER_FLAGS={ldflags}',
            ]
        elif host == Host.LinuxArm64:
            ldflags = LinuxArm64Musl.LDFLAGS + ' -static-libstdc++'
            cflags = LinuxArm64Musl.CFLAGS
            cmake_conf_args += [
                f'-DCMAKE_CXX_COMPILER={LinuxArm64Musl.CXX}',
                f'-DCMAKE_C_COMPILER={LinuxArm64Musl.CC}',
                f'-DCMAKE_EXE_LINKER_FLAGS={ldflags}',
                f'-DCMAKE_SHARED_LINKER_FLAGS={ldflags}',
                f'-DCMAKE_CXX_FLAGS={cflags}',
            ]
        elif host == Host.Darwin:
            cmake_conf_args += [
                '-DCMAKE_OSX_DEPLOYMENT_TARGET=10.9',
                '-DCMAKE_OSX_ARCHITECTURES=x86_64;arm64',
            ]


    run_cmd(cmake_conf_args)
    run_cmd(cmake_build_args)

    ninja_bin = out / f'build/Release/ninja{exe}'
    if host != Host.Windows:
        run_cmd(['strip', ninja_bin])

    create_new_dir(out / 'install')
    shutil.copy2(ninja_bin, out / 'install')
    # The LICENSE file is a symlink to COPYING. Avoid copying it directly because Kokoro uses Cygwin
    # git, which produces a Cygwin symlink that non-Cygwin Python treats as a small binary file.
    shutil.copy2(NINJA_SRC / 'COPYING', out / 'install/LICENSE')

    if host == Host.LinuxArm64:
        shutil.copy2(LinuxArm64Musl.LIBC_MUSL, out / 'install/libc_musl.so')
        for notice in LinuxArm64Musl.LIBC_MUSL_NOTICES:
            shutil.copy2(notice, out / 'install' / notice.name)

    build_id = os.getenv('KOKORO_BUILD_ID', 'dev')
    zip_dir(out / 'install', out / f'artifact/ninja-{host.value}-{build_id}.zip')
    run_cmd([sys.executable, TOP / 'toolchain/ndk-kokoro/gen_manifest.py',
        '--root', TOP, '-o', out / f'artifact/manifest-{build_id}.xml'])

    # The ninja tests complete in just a few seconds, so run them during the build. Wait until the
    # end so we can collect artifacts if a test fails.
    env = os.environ.copy()
    if host == Host.LinuxArm64:
        env['LD_LIBRARY_PATH'] = out / 'install'
    run_cmd([out / f'build/Release/ninja_test{exe}'], cwd=out / 'build', env=env)


if __name__ == '__main__':
    main()
