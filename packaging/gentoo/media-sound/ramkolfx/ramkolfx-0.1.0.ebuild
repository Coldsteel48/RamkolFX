# Copyright 2026 Roman Levin (Coldsteel48)
# Distributed under the terms of the GNU General Public License v3

EAPI=8

inherit cmake

DESCRIPTION="3D spatial audio, EQ and bass enhancer for PipeWire"
HOMEPAGE="https://github.com/Coldsteel48/RamkolFX"
SRC_URI="
	https://github.com/Coldsteel48/RamkolFX/archive/v${PV}.tar.gz -> RamkolFX-${PV}.tar.gz
	https://github.com/hoene/libmysofa/archive/refs/tags/v1.3.5.tar.gz -> libmysofa-1.3.5.tar.gz
	https://github.com/mborgerding/kissfft/archive/refs/tags/131.2.0.tar.gz -> kissfft-131.2.0.tar.gz
	gui? ( https://github.com/ocornut/imgui/archive/refs/tags/v1.91.6.tar.gz -> imgui-1.91.6.tar.gz )
"
S="${WORKDIR}/RamkolFX-${PV}"

LICENSE="GPL-3 MIT Apache-2.0 CC-BY-4.0 BSD"
SLOT="0"
KEYWORDS="~amd64 ~arm64"
IUSE="+gui"

RDEPEND="
	media-video/pipewire:=
	sys-libs/zlib
	gui? (
		media-libs/libsdl2[opengl]
		media-libs/fontconfig
		virtual/opengl
	)
"
DEPEND="${RDEPEND}"
BDEPEND="virtual/pkgconfig"

src_configure() {
	local mycmakeargs=(
		-DRAMKOLFX_HRTF_DATA_DIR="${EPREFIX}/usr/share/ramkolfx/hrtf"
		-DRAMKOLFX_BUILD_GUI=$(usex gui)
		-DRAMKOLFX_BUILD_TESTS=OFF
		-DFETCHCONTENT_FULLY_DISCONNECTED=ON
		-DFETCHCONTENT_SOURCE_DIR_MYSOFA="${WORKDIR}/libmysofa-1.3.5"
		-DFETCHCONTENT_SOURCE_DIR_KISSFFT="${WORKDIR}/kissfft-131.2.0"
		-DFETCHCONTENT_SOURCE_DIR_IMGUI="${WORKDIR}/imgui-1.91.6"
	)
	cmake_src_configure
}

src_install() {
	DESTDIR="${D}" cmake --install "${BUILD_DIR}" --component ramkolfx || die
}

pkg_postinst() {
	elog "Enable the daemon with: systemctl --user enable --now ramkolfxd"
}
