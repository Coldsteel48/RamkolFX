Name:           ramkolfx
# upstream_version / Version / Release are maintained by packaging/version.sh
%global upstream_version 0.4.1
Version:        0.4.1
Release:        1%{?dist}
Summary:        3D spatial audio, EQ and bass enhancer for PipeWire
# GPL-3.0-only: RamkolFX. Bundled code: MIT (Dear ImGui), BSD-3-Clause
# (KissFFT). Bundled data: MIT (SONICOM HRTFs, Auto-detect model),
# Apache-2.0 (SADIE II HRTFs), CC-BY-4.0 (HUTUBS HRTFs); the MIT KEMAR HRTF
# is public domain.
License:        GPL-3.0-only AND MIT AND BSD-3-Clause AND Apache-2.0 AND CC-BY-4.0 AND LicenseRef-Fedora-Public-Domain
URL:            https://github.com/Coldsteel48/RamkolFX
Source0:        %{url}/archive/v%{upstream_version}/RamkolFX-%{upstream_version}.tar.gz
# Compiled in: KissFFT isn't packaged for Fedora, and the GUI targets this
# exact Dear ImGui (it uses imgui_internal.h). libmysofa comes from Fedora.
Source1:        https://github.com/mborgerding/kissfft/archive/refs/tags/131.2.0.tar.gz#/kissfft-131.2.0.tar.gz
Source2:        https://github.com/ocornut/imgui/archive/refs/tags/v1.91.6.tar.gz#/imgui-1.91.6.tar.gz
Provides:       bundled(kissfft) = 131.2.0
Provides:       bundled(imgui) = 1.91.6

BuildRequires:  gcc-c++
BuildRequires:  cmake >= 3.20
BuildRequires:  pkgconfig(libpipewire-0.3)
BuildRequires:  pkgconfig(libmysofa)
BuildRequires:  pkgconfig(sdl2) >= 2.0.18
BuildRequires:  pkgconfig(x11)
BuildRequires:  pkgconfig(fontconfig)
BuildRequires:  pkgconfig(dbus-1)
BuildRequires:  pkgconfig(gl)
BuildRequires:  zlib-devel
# %%check validates the AppStream metadata and desktop entry.
BuildRequires:  appstream
BuildRequires:  desktop-file-utils
# Auto-detect's classifier; dlopened, so not found by the automatic deps.
Requires:       onnxruntime
Requires:       hicolor-icon-theme
Recommends:     pipewire
Recommends:     wireplumber

%description
RamkolFX creates a virtual PipeWire sink, processes whatever is played into it
(HRTF binaural rendering or ambisonics speaker virtualisation, a 10-band
graphic EQ and a psychoacoustic bass enhancer) and plays the result out
through a real output device. Includes the ramkolfxd daemon, the
ramkolfx-gui control app and a bundled set of redistributable HRTFs.

Enable the daemon with: systemctl --user enable --now ramkolfxd

%prep
%autosetup -n RamkolFX-%{upstream_version}
mkdir deps
for s in %{SOURCE1} %{SOURCE2}; do tar -xzf "$s" -C deps; done

%build
%cmake \
  -DRAMKOLFX_HRTF_DATA_DIR=%{_datadir}/ramkolfx/hrtf \
  -DRAMKOLFX_MODEL_DATA_DIR=%{_datadir}/ramkolfx/models \
  -DRAMKOLFX_SYSTEM_MYSOFA=ON \
  -DRAMKOLFX_BUILD_GUI=ON -DRAMKOLFX_BUILD_TESTS=ON \
  $(sh packaging/ci/vendor-flags.sh "$PWD/deps")
%cmake_build

%install
# Only RamkolFX's own files: the bundled KissFFT's install rules (headers,
# static library) are left out by installing just this component.
DESTDIR=%{buildroot} %{__cmake} --install %{__cmake_builddir} --component ramkolfx

%check
%ctest

%files
%license LICENSE LICENSE-COMMERCIAL.md
%license deps/imgui-1.91.6/LICENSE.txt
%license deps/kissfft-131.2.0/COPYING deps/kissfft-131.2.0/LICENSES/BSD-3-Clause
%{_bindir}/ramkolfxd
%{_bindir}/ramkolfx-gui
%{_datadir}/ramkolfx/
%{_datadir}/applications/ramkolfx.desktop
%{_metainfodir}/io.github.coldsteel48.RamkolFX.metainfo.xml
%{_mandir}/man1/ramkolfxd.1*
%{_mandir}/man1/ramkolfx-gui.1*
%{_datadir}/icons/hicolor/scalable/apps/ramkolfx.svg
%{_prefix}/lib/systemd/user/ramkolfxd.service
%doc %{_docdir}/RamkolFX/

%changelog
* Fri Nov 20 2026 Roman Levin (Coldsteel48) <noreply@github.com> - 0.4.1-1
- Release 0.4.1

* Fri Nov 20 2026 Roman Levin (Coldsteel48) <noreply@github.com> - 0.4.0-1
- Release 0.4.0

* Fri Nov 20 2026 Roman Levin (Coldsteel48) <noreply@github.com> - 0.3.0-1
- Release 0.3.0

* Fri Nov 20 2026 Roman Levin (Coldsteel48) <noreply@github.com> - 0.2.2-1
- Release 0.2.2

* Fri Nov 20 2026 Roman Levin (Coldsteel48) <noreply@github.com> - 0.2.1-1
- Release 0.2.1

* Fri Nov 20 2026 Roman Levin (Coldsteel48) <noreply@github.com> - 0.2.0-1
- Release 0.2.0

* Fri Nov 20 2026 Roman Levin (Coldsteel48) <noreply@github.com> - 0.1.3-1
- Release 0.1.3

* Fri Nov 20 2026 Roman Levin (Coldsteel48) <noreply@github.com> - 0.1.2-1
- Release 0.1.2

* Thu Nov 19 2026 Roman Levin (Coldsteel48) <noreply@github.com> - 0.1.1-1
- Release 0.1.1

* Wed Nov 18 2026 Roman Levin (Coldsteel48) <noreply@github.com> - 0.1.0-1
- Initial packaging
