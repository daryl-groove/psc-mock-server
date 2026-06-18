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
# Compiled proto, shared by the core lib and its tests so the generated .cc are
# built once rather than per-target. The `sources` headers give every consumer a
# build-order edge to proto generation (gnmi.pb.h must exist before a TU compiles).
# ---------------------------------------------------------------------------

proto_lib = static_library('gnmi_pb',
  [gnmi_proto, gnmi_ext_proto],
  dependencies : [grpc_dep, proto_dep],
)

proto_dep_all = declare_dependency(
  link_with    : proto_lib,
  sources      : [gnmi_proto[1], gnmi_proto[3], gnmi_ext_proto[1], gnmi_ext_proto[3]],
  dependencies : [grpc_dep, proto_dep],
)

# ---------------------------------------------------------------------------
# gnmid core data layer (src/core) — the new path-keyed leaf store + groups.
# Replaces the old backend (LeafStore / DataProviderRegistry); the gNMI service
# is migrated onto it in a following slice.
# ---------------------------------------------------------------------------

gnmid_core_inc = include_directories('src/core')

gnmid_core_lib = static_library('gnmid_core',
  [
    'src/core/canonical_path.cpp',
    'src/core/notification_group.cpp',
    'src/core/leaf_registry.cpp',
  ],
  dependencies        : [proto_dep_all],
  include_directories : [gnmid_core_inc],
)

gnmid_core_dep = declare_dependency(
  link_with           : gnmid_core_lib,
  include_directories : [gnmid_core_inc],
  dependencies        : [proto_dep_all],
)

# ---------------------------------------------------------------------------
# Backend — device/schema layer over the core (routing, writability plane,
# provider wiring). The gNMI service is migrated onto it in a following slice.
# ---------------------------------------------------------------------------

gnmid_backend_lib = static_library('gnmid_backend',
  ['src/backend/backend.cpp'],
  dependencies        : [gnmid_core_dep],
  include_directories : [include_directories('src')],
)

gnmid_backend_dep = declare_dependency(
  link_with           : gnmid_backend_lib,
  include_directories : [include_directories('src')],
  dependencies        : [gnmid_core_dep],
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
  'src/gnmi/subscribe_emit.cpp',
  'src/gnmi/subscription.cpp',
  'src/backend/psc_power_sensor_provider.cpp',
  'src/backend/system_config_provider.cpp',
  'src/security/authentication.cpp',
  'src/utils/log.cpp',
]
# proto, core and Backend (incl. backend.cpp) come through gnmid_backend_dep.

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
    gnmid_backend_dep,   # Backend + core + compiled proto (headers + symbols)
  ],
  include_directories : [
    include_directories('src'),
  ],
  install : false
)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

gtest_dep = dependency('gtest', main: true)

test_utils = executable('test_utils',
  ['tests/test_utils.cpp', gnmi_ext_proto, gnmi_proto],
  dependencies : [proto_dep, grpc_dep, thread_dep, gtest_dep],
  include_directories : [include_directories('src')],
)
test('utils', test_utils)

# ---------------------------------------------------------------------------
# Core (src/core) unit tests
# ---------------------------------------------------------------------------

foreach t : [
    'canonical_path',
    'notification_group',
    'leaf_registry_registration',
    'leaf_registry_values',
    'leaf_registry_structural',
    'leaf_sink',
    'integration_scenario',
  ]
  exe = executable('test_core_' + t,
    ['tests/core/test_' + t + '.cpp'],
    dependencies : [gnmid_core_dep, gtest_dep, thread_dep],
  )
  test('core_' + t, exe)
endforeach

test_backend = executable('test_backend',
  ['tests/test_backend.cpp', 'src/backend/psc_power_sensor_provider.cpp'],
  dependencies        : [gnmid_backend_dep, gtest_dep, thread_dep],
  include_directories : [include_directories('src')],
)
test('backend', test_backend)

# Subscribe emit logic (atomic partition + changeSeq diff + target echo). Compiles
# subscribe_emit.cpp directly against the Backend/core/proto deps — a fast unit test
# for the View -> Notification(s) contract, without a gRPC stream or timing.
test_subscribe_emit = executable('test_subscribe_emit',
  ['tests/test_subscribe_emit.cpp', 'src/gnmi/subscribe_emit.cpp'],
  dependencies        : [gnmid_backend_dep, gtest_dep, thread_dep],
  include_directories : [include_directories('src')],
)
test('subscribe_emit', test_subscribe_emit)

# Push routing (P1/P2, S2): the SubscriptionHub change->stream matching, tested as a
# pure function against ChangeBatch inputs (no gRPC stream, no threads).
test_subscription_hub = executable('test_subscription_hub',
  ['tests/test_subscription_hub.cpp', 'src/gnmi/subscription.cpp'],
  dependencies        : [gnmid_core_dep, gtest_dep, thread_dep],
  include_directories : [include_directories('src/gnmi')],
)
test('subscription_hub', test_subscription_hub)
