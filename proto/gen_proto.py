#!/usr/bin/env python3
"""
Generate gNMI proto files for C++.

gnmi.proto imports "github.com/.../gnmi_ext/gnmi_ext.proto".
protoc preserves the import path in the output, generating:
    <outdir>/github.com/.../gnmi_ext.pb.{h,cc}

We also copy those files to <outdir>/gnmi_ext.pb.{h,cc} so Meson can
track them as flat-named build outputs and include them in compilation.
gnmi.pb.h will still find them via the subdirectory path through -I<outdir>.
"""

import subprocess
import shutil
import os
import sys


def main():
    protoc      = sys.argv[1]
    grpc_plugin = sys.argv[2]
    outdir      = sys.argv[3]
    srcdir      = sys.argv[4]
    proto_path  = os.path.join(srcdir, 'proto')

    def run_protoc(proto_file):
        cmd = [
            protoc,
            f'--proto_path={proto_path}',
            f'--cpp_out={outdir}',
            f'--grpc_out={outdir}',
            f'--plugin=protoc-gen-grpc={grpc_plugin}',
            proto_file,
        ]
        subprocess.run(cmd, check=True)

    # 1. Generate gnmi_ext — lands in outdir/github.com/.../gnmi_ext/
    run_protoc('github.com/openconfig/gnmi/proto/gnmi_ext/gnmi_ext.proto')

    # 2. Copy generated gnmi_ext files to flat location for Meson tracking.
    #    gnmi.pb.h will still find them via the subdirectory with -I<outdir>.
    gnmi_ext_subdir = os.path.join(
        outdir, 'github.com', 'openconfig', 'gnmi', 'proto', 'gnmi_ext')
    for name in ['gnmi_ext.pb.cc', 'gnmi_ext.pb.h',
                 'gnmi_ext.grpc.pb.cc', 'gnmi_ext.grpc.pb.h']:
        shutil.copy(os.path.join(gnmi_ext_subdir, name),
                    os.path.join(outdir, name))

    # 3. Generate gnmi — gnmi.pb.h includes "github.com/.../gnmi_ext.pb.h"
    #    which exists in outdir/github.com/... from step 1.
    run_protoc('gnmi.proto')


if __name__ == '__main__':
    main()
