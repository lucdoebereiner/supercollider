# Opt-in install of the experimental PipeWirePatch sclang extension
# (pure-sclang PipeWire graph control via pw-link). OFF by default so it
# only ships when explicitly enabled, matching the local-experiment scope of
# the PipeWire backend it pairs with.
#
# Enable with:
#   cmake -DSC_INSTALL_PIPEWIRE_PATCH=ON .
#   cmake --build . --target install
#
# This installs the .sc into the SYSTEM SuperCollider Extensions dir
# (<prefix>/share/SuperCollider/Extensions), so sclang compiles it on
# startup. For per-user development, prefer install.sh (symlink) instead.

option(SC_INSTALL_PIPEWIRE_PATCH
	"Install the experimental PipeWirePatch sclang extension (pw-link graph control)" OFF)

if(SC_INSTALL_PIPEWIRE_PATCH)
	if(APPLE OR WIN32)
		message(WARNING
			"SC_INSTALL_PIPEWIRE_PATCH is Linux-only (PipeWire backend experiment); "
			"skipping install on this platform.")
	else()
		install(FILES PipeWirePatch.sc
			DESTINATION share/SuperCollider/Extensions/PipeWirePatch)
		message(STATUS
			"PipeWirePatch: will install to "
			"${CMAKE_INSTALL_PREFIX}/share/SuperCollider/Extensions/PipeWirePatch")
	endif()
endif()
