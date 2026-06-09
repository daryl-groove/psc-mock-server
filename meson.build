project('psc-mock-server', 'cpp',
  version : '0.1.0',
  default_options : [
    'cpp_std=c++20',
    'warning_level=2',
    'buildtype=debugoptimized',
  ]
)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------

grpc_dep   = dependency('grpc++')
proto_dep  = dependency('protobuf')
boost_dep  = dependency('boost',
  modules : ['log', 'log_setup', 'system', 'thread'],
  required : true
)
thread_dep = dependency('threads')

# ---------------------------------------------------------------------------
# Proto code generation
#
# gnmi.proto imports "github.com/.../gnmi_ext/gnmi_ext.proto".
# protoc places gnmi_ext outputs in a matching subdirectory of @OUTDIR@.
# gnmi.pb.h will #include "github.com/.../gnmi_ext.pb.h" — resolved because
# meson adds the custom_target output dir to the executable's include path.
# We also copy gnmi_ext files flat to @OUTDIR@ so they compile as sources.
# ---------------------------------------------------------------------------

protoc          = find_program('protoc')
grpc_cpp_plugin = find_program('grpc_cpp_plugin')

gnmi_ext_proto = custom_target('gnmi_ext_proto',
  input  : 'proto/github.com/openconfig/gnmi/proto/gnmi_ext/gnmi_ext.proto',
  output : ['gnmi_ext.pb.cc', 'gnmi_ext.pb.h',
             'gnmi_ext.grpc.pb.cc', 'gnmi_ext.grpc.pb.h'],
  command: [
    'bash', '-c',
    'set -e'
    + '; "$1" --proto_path="$4/proto" --cpp_out="$2" --grpc_out="$2"'
    + ' --plugin=protoc-gen-grpc="$3"'
    + ' github.com/openconfig/gnmi/proto/gnmi_ext/gnmi_ext.proto'
    + '; EXT="$2/github.com/openconfig/gnmi/proto/gnmi_ext"'
    + '; for f in pb.cc pb.h grpc.pb.cc grpc.pb.h;'
    + ' do cp "$EXT/gnmi_ext.$f" "$2/gnmi_ext.$f"; done',
    '_',                            # $0 (dummy for bash -c positional args)
    protoc.full_path(),             # $1
    '@OUTDIR@',                     # $2
    grpc_cpp_plugin.full_path(),    # $3
    meson.project_source_root(),    # $4
  ]
)

gnmi_proto = custom_target('gnmi_proto',
  input  : 'proto/gnmi.proto',
  output : ['gnmi.pb.cc', 'gnmi.pb.h', 'gnmi.grpc.pb.cc', 'gnmi.grpc.pb.h'],
  depends: gnmi_ext_proto,
  command: [
    protoc,
    '--proto_path=@CURRENT_SOURCE_DIR@/proto',
    '--cpp_out=@OUTDIR@',
    '--grpc_out=@OUTDIR@',
    '--plugin=protoc-gen-grpc=' + grpc_cpp_plugin.full_path(),
    '@CURRENT_SOURCE_DIR@/proto/gnmi.proto',
  ]
)

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------

sources = [
  'src/main.cpp',
  'src/gnmi/gnmi.cpp',
  'src/gnmi/capabilities.cpp',
  'src/gnmi/get.cpp',
  'src/gnmi/set.cpp',
  'src/gnmi/subscribe.cpp',
  'src/backend/leaf_store.cpp',
  'src/backend/psc_power_sensor_provider.cpp',
  'src/security/authentication.cpp',
  'src/utils/log.cpp',
  gnmi_ext_proto,
  gnmi_proto,
]

# ---------------------------------------------------------------------------
# Build target
# ---------------------------------------------------------------------------

executable('psc-mock-server',
  sources,
  dependencies : [
    grpc_dep,
    proto_dep,
    boost_dep,
    thread_dep,
  ],
  include_directories : [
    include_directories('src'),
    # Generated proto headers live in the build dir; meson auto-adds that
    # directory to the include path when custom_target appears in sources.
  ],
  install : false
)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

gtest_dep = dependency('gtest', main: true)

test_data_provider = executable('test_data_provider',
  ['tests/test_data_provider.cpp', gnmi_ext_proto, gnmi_proto],
  dependencies : [proto_dep, grpc_dep, thread_dep, gtest_dep],
  include_directories : [include_directories('src')],
)

test_psc_provider = executable('test_psc_provider',
  ['tests/test_psc_provider.cpp',
   'src/backend/leaf_store.cpp',
   'src/backend/psc_power_sensor_provider.cpp',
   gnmi_ext_proto, gnmi_proto],
  dependencies : [proto_dep, grpc_dep, thread_dep, boost_dep, gtest_dep],
  include_directories : [include_directories('src')],
)

test_leaf_store = executable('test_leaf_store',
  ['tests/test_leaf_store.cpp',
   'src/backend/leaf_store.cpp',
   gnmi_ext_proto, gnmi_proto],
  dependencies : [proto_dep, grpc_dep, thread_dep, gtest_dep],
  include_directories : [include_directories('src')],
)

test_utils = executable('test_utils',
  ['tests/test_utils.cpp', gnmi_ext_proto, gnmi_proto],
  dependencies : [proto_dep, grpc_dep, thread_dep, gtest_dep],
  include_directories : [include_directories('src')],
)

test('data_provider', test_data_provider)
test('psc_provider',  test_psc_provider)
test('leaf_store',    test_leaf_store)
test('utils',         test_utils)
