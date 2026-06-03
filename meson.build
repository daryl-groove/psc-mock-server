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
# Pre-generated proto sources
#
# gnmi.proto and gnmi_ext.proto are stable OpenConfig spec files.
# Generated files live in proto/generated/ and are committed to the repo.
#
# To regenerate (only needed when proto/ files change):
#   python3 proto/gen_proto.py <protoc> <grpc_cpp_plugin> <outdir> <srcdir>
# or directly:
#   cd proto && protoc --proto_path=. --cpp_out=generated --grpc_out=generated \
#     --plugin=protoc-gen-grpc=$(which grpc_cpp_plugin) gnmi_ext.proto gnmi.proto
# ---------------------------------------------------------------------------

proto_sources = files(
  'proto/generated/gnmi.pb.cc',
  'proto/generated/gnmi.grpc.pb.cc',
  'proto/generated/gnmi_ext.pb.cc',
  'proto/generated/gnmi_ext.grpc.pb.cc',
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
  'src/backend/psc_power_sensor_provider.cpp',
  'src/security/authentication.cpp',
  'src/utils/log.cpp',
  proto_sources,
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
    include_directories('proto/generated'),  # for #include <gnmi.grpc.pb.h>
  ],
  install : false
)
