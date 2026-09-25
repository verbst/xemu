# Groovy MiSTer streaming client.
#
# Vendored from the Groovy_MiSTer repository's api/ directory at commit
# 55860b2. The C wrapper (groovymister_wrapper.*) is deliberately not vendored;
# xemu links the C++ class directly. Re-sync by copying api/ over this
# directory.
#
# Local patches: preserve line-time precision in CmdSwitchres and provide
# optional packet/transport observations for buffered diagnostics. Neither
# changes the wire format. Reapply these when refreshing the vendor copy.
#
# Full provenance, including how to verify the copy against upstream, is kept
# with the project notes rather than in-tree.
#
# The bundled LZ4 is used rather than GM_SYSTEM_LZ4, because xemu links no
# other copy. Warnings are silenced per-target so the vendored sources need no
# edits of ours to build clean.

groovymister_c_args = cc.get_supported_arguments([
  '-Wno-missing-prototypes',
  '-Wno-redundant-decls',
])

groovymister_cpp_args = cxx.get_supported_arguments([
  '-Wno-missing-format-attribute',
  '-Wno-suggest-attribute=format',
])

# nlc_codec encodes on std::thread workers; the client itself needs sockets
# (ws2_32 on Windows, where the send path uses Registered I/O).
groovymister_deps = [threads, socket, libm]

libgroovymister = static_library('groovymister',
                                 sources: files(
                                   'groovymister.cpp',
                                   'nlc_codec.cpp',
                                   'lz4/lz4.c',
                                   'lz4/lz4hc.c',
                                 ),
                                 c_args: groovymister_c_args,
                                 cpp_args: groovymister_cpp_args,
                                 include_directories: ['.', 'lz4'],
                                 dependencies: groovymister_deps)

groovymister = declare_dependency(link_with: libgroovymister,
                                  include_directories: ['.'],
                                  dependencies: groovymister_deps)
