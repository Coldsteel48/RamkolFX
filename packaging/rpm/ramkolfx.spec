%global debug_package %{nil}

Name:           ramkolfx
# upstream_version / Version / Release are maintained by packaging/version.sh
%global upstream_version 0.1.1
Version:        0.1.1
Release:        1
Summary:        3D spatial audio, EQ and bass enhancer for PipeWire
License:        GPL-3.0-only
URL:            https://github.com/Coldsteel48/RamkolFX
Source0:        %{url}/archive/v%{upstream_version}/RamkolFX-%{upstream_version}.tar.gz
Source1:        https://github.com/hoene/libmysofa/archive/refs/tags/v1.3.5.tar.gz#/libmysofa-1.3.5.tar.gz
Source2:        https://github.com/mborgerding/kissfft/archive/refs/tags/131.2.0.tar.gz#/kissfft-131.2.0.tar.gz
Source3:        https://github.com/ocornut/imgui/archive/refs/tags/v1.91.6.tar.gz#/imgui-1.91.6.tar.gz

BuildRequires:  gcc-c++
BuildRequires:  cmake >= 3.20
BuildRequires:  ninja-build
BuildRequires:  pkgconfig(libpipewire-0.3)
BuildRequires:  pkgconfig(sdl2) >= 2.0.18
BuildRequires:  pkgconfig(x11)
BuildRequires:  pkgconfig(fontconfig)
BuildRequires:  pkgconfig(dbus-1)
BuildRequires:  pkgconfig(gl)
BuildRequires:  zlib-devel
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
for s in %{SOURCE1} %{SOURCE2} %{SOURCE3}; do tar -xzf "$s" -C deps; done

%build
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=%{_prefix} \
  -DRAMKOLFX_HRTF_DATA_DIR=%{_datadir}/ramkolfx/hrtf \
  -DRAMKOLFX_BUILD_GUI=ON -DRAMKOLFX_BUILD_TESTS=OFF \
  $(sh packaging/ci/vendor-flags.sh "$PWD/deps")
cmake --build build

%install
DESTDIR=%{buildroot} cmake --install build --component ramkolfx

%files
%license LICENSE LICENSE-COMMERCIAL.md
%{_bindir}/ramkolfxd
%{_bindir}/ramkolfx-gui
%{_datadir}/ramkolfx/
%{_datadir}/applications/ramkolfx.desktop
%{_datadir}/icons/hicolor/scalable/apps/ramkolfx.svg
%{_prefix}/lib/systemd/user/ramkolfxd.service
%doc %{_docdir}/RamkolFX/

%changelog
* Thu Nov 19 2026 Roman Levin (Coldsteel48) <noreply@github.com> - 0.1.1-1
- Release 0.1.1

* Wed Nov 18 2026 Roman Levin (Coldsteel48) <noreply@github.com> - 0.1.0-1
- Initial packaging
