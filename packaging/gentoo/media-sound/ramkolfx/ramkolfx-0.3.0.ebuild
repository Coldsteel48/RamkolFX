# Copyright 2026 Roman Levin (Coldsteel48)
# Distributed under the terms of the GNU General Public License v3

EAPI=8

inherit xdg cmake

# MY_PV (upstream SemVer tag) is maintained by packaging/version.sh from ../../../../VERSION.
MY_PV="0.3.0"

DESCRIPTION="3D spatial audio, EQ and bass enhancer for PipeWire"
HOMEPAGE="https://github.com/Coldsteel48/RamkolFX"
SRC_URI="
	https://github.com/Coldsteel48/RamkolFX/archive/v${MY_PV}.tar.gz -> RamkolFX-${MY_PV}.tar.gz
	gui? ( https://github.com/ocornut/imgui/archive/refs/tags/v1.91.6.tar.gz -> imgui-1.91.6.tar.gz )
"
S="${WORKDIR}/RamkolFX-${MY_PV}"

LICENSE="GPL-3 MIT Apache-2.0 CC-BY-4.0 public-domain"
SLOT="0"
KEYWORDS="~amd64 ~arm64"
IUSE="+gui test"
RESTRICT="!test? ( test )"

RDEPEND="
	media-libs/libmysofa
	media-video/pipewire:=
	sci-libs/kissfft
	sys-libs/zlib
	gui? (
		>=media-libs/libsdl2-2.0.18[opengl]
		media-libs/fontconfig
		sys-apps/dbus
		virtual/opengl
	)
"
DEPEND="${RDEPEND}
	gui? ( x11-libs/libX11 )
"
BDEPEND="virtual/pkgconfig"

src_configure() {
	local mycmakeargs=(
		-DRAMKOLFX_HRTF_DATA_DIR="${EPREFIX}/usr/share/ramkolfx/hrtf"
		-DRAMKOLFX_MODEL_DATA_DIR="${EPREFIX}/usr/share/ramkolfx/models"
		-DRAMKOLFX_BUILD_GUI=$(usex gui)
		-DRAMKOLFX_SYSTEM_MYSOFA=ON
		-DRAMKOLFX_SYSTEM_KISSFFT=ON
		-DRAMKOLFX_BUILD_TESTS=$(usex test)
		-DFETCHCONTENT_FULLY_DISCONNECTED=ON
	)
	# Dear ImGui is compiled into the GUI: it targets this exact version.
	use gui && mycmakeargs+=( -DFETCHCONTENT_SOURCE_DIR_IMGUI="${WORKDIR}/imgui-1.91.6" )
	cmake_src_configure
}

src_install() {
	DESTDIR="${D}" cmake --install "${BUILD_DIR}" --component ramkolfx || die
}

pkg_postinst() {
	xdg_pkg_postinst
	elog "Enable the daemon with: systemctl --user enable --now ramkolfxd"
	elog "Auto-detect (content-aware EQ presets) needs ONNX Runtime >= 1.17,"
	elog "which is not in ::gentoo; without it Auto-detect shows as unavailable."
}
