# switchres (https://github.com/antonioginer/switchres), GPL-2.0-or-later.
#
# Three local patches are carried against upstream, all worth submitting there:
#   custom_video.cpp  gate the Windows ATI/ADL/PowerStrip backends behind
#                     SR_WITH_ATI / SR_WITH_PSTRIP, matching how the Linux
#                     backends are already gated, so a modeline-only build does
#                     not have to compile or link them
#   display.cpp       set_preset() strncpy'd a buffer onto itself, which is
#                     undefined and aborts under a fortified libc
#   switchres.cpp     the constructor's crt_range loop ran 1..MAX_RANGES,
#                     leaving entry 0 unset and writing one past the array
#
# Full provenance and the reasoning behind each patch are kept with the project
# notes rather than in-tree.
#
# switchres, built as a modeline calculator only. It is driven with the "dummy"
# display backend and never touches a host display, so none of the GPU-vendor or
# windowing backends are compiled. See PROVENANCE.md for the two local patches.
#
# Leaving SR_WITH_ATI, SR_WITH_PSTRIP, SR_WITH_XRANDR, SR_WITH_KMSDRM and
# SR_WITH_SDL2 undefined is what keeps this dependency-free.

switchres_src = files(
  'custom_video.cpp',
  'display.cpp',
  'edid.cpp',
  'log.cpp',
  'modeline.cpp',
  'monitor.cpp',
  'switchres.cpp',
  'switchres_wrapper.cpp',
)

switchres_cpp_args = []

if host_os == 'windows'
  switchres_src += files('display_windows.cpp')
  # Build the wrapper's entry points for static linkage rather than as DLL exports.
  switchres_cpp_args += '-DSR_WIN32_STATIC'
elif host_os == 'linux'
  switchres_src += files('display_linux.cpp')
endif

libswitchres = static_library('switchres',
                              sources: switchres_src,
                              cpp_args: switchres_cpp_args)

switchres = declare_dependency(link_with: libswitchres,
                               include_directories: ['.'])
