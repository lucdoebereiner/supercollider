# Opt-in install of the experimental PipeWirePatch sclang extension
# (pure-sclang PipeWire graph control via pw-link). OFF by default so it
# only ships when explicitly enabled, matching the local-experiment scope of
# the PipeWire backend it pairs with.
#
# Enable with:
#   cmake -DSC_INSTALL_PIPEWIRE_PATCH=ON .
#   cmake --build . --target install
#
# This installs the .sc into the PER-USER SuperCollider Extensions dir
# (${XDG_DATA_HOME:-$HOME/.local/share}/SuperCollider/Extensions), so the
# install needs no sudo and sclang compiles it on startup. The destination is
# an absolute path resolved at configure time, so it ignores
# CMAKE_INSTALL_PREFIX. For a live-editable symlink instead, use install.sh.

option(SC_INSTALL_PIPEWIRE_PATCH
	"Install the experimental PipeWirePatch sclang extension (pw-link graph control)" OFF)

if(SC_INSTALL_PIPEWIRE_PATCH)
	if(APPLE OR WIN32)
		message(WARNING
			"SC_INSTALL_PIPEWIRE_PATCH is Linux-only (PipeWire backend experiment); "
			"skipping install on this platform.")
	else()
		if(DEFINED ENV{XDG_DATA_HOME})
			set(_sc_user_data "$ENV{XDG_DATA_HOME}")
		else()
			set(_sc_user_data "$ENV{HOME}/.local/share")
		endif()
		set(_sc_ext_dir "${_sc_user_data}/SuperCollider/Extensions/PipeWirePatch")
		install(FILES PipeWirePatch.sc DESTINATION "${_sc_ext_dir}")
		install(DIRECTORY HelpSource DESTINATION "${_sc_ext_dir}")
		message(STATUS "PipeWirePatch: will install to ${_sc_ext_dir} (with HelpSource)")
	endif()
endif()
