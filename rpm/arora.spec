#
# arora.spec — RPM build for openSUSE (PKG03)
#
# Artifact policy is identical to the Debian package (PKG01,
# .devin/pkg-deb.sh): the rpm ships the self-contained payload produced
# by BuildProcess/bundle-linux.sh mapped onto the FHS — a private
# Qt + QtWebEngine runtime under /usr/lib/arora behind a /usr/bin/arora
# launcher — because this tree builds against a newer Qt than the
# distro series ships. The optional Rust crates (arora-rustdl,
# arora-rustcore) link STATICALLY into the binary via their .a archives
# (src/rustdl/rustdl.pri, src/rustcore/rustcore.pri), so there are no
# rust .so artifacts to ship; libservo_embed does not exist in this
# tree yet (ENG03 gates it).
#
# Because the runtime is private, AutoReqProv is OFF: a dependency scan
# would publish the bundled Qt SONAMEs as Provides and emit Requires on
# them that no system package can satisfy. The explicit Requires: block
# below is the payload's real NEEDED-minus-bundled SONAME surface in
# rpm capability form — the same set the deb's Depends computes via
# dpkg-shlibdeps (version floors are deb-specific; capabilities are
# what AutoReq would emit). `.devin/pkg-rpm.sh verify` recomputes the
# set from the staged tree and fails on drift, so this list cannot
# silently rot. The capabilities carry the 64-bit ELF marker — build on
# a 64-bit arch (the deb is amd64-only today as well).
#
# The bundled-runtime payload needs a relocatable single-prefix Qt —
# the dev-loop ~/Qt/6.12.0/gcc_64 or any `aqt install` tree. The
# %{qtdir} macro picks up $QTDIR, then ~/Qt/6.12.0/gcc_64; override on
# the rpmbuild line:
#   rpmbuild -bb --define 'qtdir /path/to/Qt/6.x.y/gcc_64' rpm/arora.spec
# or just run ./.devin/pkg-rpm.sh (build + stage + verify + rpmbuild
# when present). On a host without rpm tooling, `pkg-rpm.sh container`
# prints the openSUSE container command.
#
# The BuildRequires are the openSUSE devel names — the same module set
# as the deb Build-Depends. They document a system-Qt build's needs and
# gate the toolchain in clean chroots (OBS/mock); the bundled payload
# itself is staged against %{qtdir}.
#

%{!?qtdir:%global qtdir %(echo "${QTDIR:-$HOME/Qt/6.12.0/gcc_64}")}

Name:           arora
Version:        0.2
# openSUSE convention: Release 0, the build service increments it.
Release:        0
Summary:        Experimental Qt 6 / QtWebEngine web browser
License:        GPL-2.0-or-later
Group:          Productivity/Networking/Web/Browsers
URL:            https://github.com/janos-szenfner/arora
Source0:        %{name}-%{version}.tar.gz
AutoReqProv:    no

BuildRequires:  gcc-c++
BuildRequires:  make
BuildRequires:  qt6-base-devel
BuildRequires:  qt6-webengine-devel
BuildRequires:  qt6-tools-devel
BuildRequires:  qt6-qt5compat-devel
# lrelease for src/locale TRANSLATIONS
BuildRequires:  qt6-linguist
# Rust toolchain builds the statically-linked crates (cargo build
# --release --locked needs crates.io access; an OBS build would vendor
# via cargo vendor instead).
BuildRequires:  cargo
BuildRequires:  rust
# staging tools: readelf/file/ldd for the bundled-lib scan
BuildRequires:  binutils
BuildRequires:  file
%if 0%{?suse_version}
BuildRequires:  update-desktop-files
%endif

# tor daemon + geoip are bundled under %{_libdir}/arora/lib/tor —
# no external tor dependency needed.

# --- NEEDED-minus-bundled SONAME surface of the staged payload -------
# Regenerate/diff: ./.devin/pkg-rpm.sh requires <staged-root>
# C/C++ runtime
Requires:       libc.so.6()(64bit)
Requires:       libdl.so.2()(64bit)
Requires:       libgcc_s.so.1()(64bit)
Requires:       libm.so.6()(64bit)
Requires:       libpthread.so.0()(64bit)
Requires:       libresolv.so.2()(64bit)
Requires:       libstdc++.so.6()(64bit)
Requires:       libz.so.1()(64bit)
Requires:       libzstd.so.1()(64bit)
# GLib stack (gtk platform theme plugin, glib event loops)
Requires:       libatk-1.0.so.0()(64bit)
Requires:       libgdk-3.so.0()(64bit)
Requires:       libgdk_pixbuf-2.0.so.0()(64bit)
Requires:       libgio-2.0.so.0()(64bit)
Requires:       libglib-2.0.so.0()(64bit)
Requires:       libgobject-2.0.so.0()(64bit)
Requires:       libgthread-2.0.so.0()(64bit)
Requires:       libgtk-3.so.0()(64bit)
Requires:       libcairo.so.2()(64bit)
Requires:       libcairo-gobject.so.2()(64bit)
Requires:       libpango-1.0.so.0()(64bit)
Requires:       libpangocairo-1.0.so.0()(64bit)
Requires:       libharfbuzz.so.0()(64bit)
# GL/GPU plumbing (QtWebEngine/Chromium)
Requires:       libEGL.so.1()(64bit)
Requires:       libGL.so.1()(64bit)
Requires:       libdrm.so.2()(64bit)
Requires:       libgbm.so.1()(64bit)
Requires:       libexpat.so.1()(64bit)
Requires:       libfontconfig.so.1()(64bit)
Requires:       libfreetype.so.6()(64bit)
# NSS/NSPR (Chromium crypto)
Requires:       libnspr4.so()(64bit)
Requires:       libnss3.so()(64bit)
Requires:       libnssutil3.so()(64bit)
Requires:       libsmime3.so()(64bit)
# misc system services
Requires:       libasound.so.2()(64bit)
Requires:       libbrotlicommon.so.1()(64bit)
Requires:       libbrotlidec.so.1()(64bit)
Requires:       libcups.so.2()(64bit)
Requires:       libdbus-1.so.3()(64bit)
Requires:       libgssapi_krb5.so.2()(64bit)
Requires:       libudev.so.1()(64bit)
# Wayland client stack
Requires:       libwayland-client.so.0()(64bit)
Requires:       libwayland-cursor.so.0()(64bit)
Requires:       libwayland-egl.so.1()(64bit)
# X11 / xcb stack
Requires:       libX11.so.6()(64bit)
Requires:       libX11-xcb.so.1()(64bit)
Requires:       libXcomposite.so.1()(64bit)
Requires:       libXdamage.so.1()(64bit)
Requires:       libXext.so.6()(64bit)
Requires:       libXfixes.so.3()(64bit)
Requires:       libXrandr.so.2()(64bit)
Requires:       libXtst.so.6()(64bit)
Requires:       libxcb.so.1()(64bit)
Requires:       libxcb-cursor.so.0()(64bit)
Requires:       libxcb-dri3.so.0()(64bit)
Requires:       libxcb-glx.so.0()(64bit)
Requires:       libxcb-icccm.so.4()(64bit)
Requires:       libxcb-image.so.0()(64bit)
Requires:       libxcb-keysyms.so.1()(64bit)
Requires:       libxcb-randr.so.0()(64bit)
Requires:       libxcb-render.so.0()(64bit)
Requires:       libxcb-render-util.so.0()(64bit)
Requires:       libxcb-shape.so.0()(64bit)
Requires:       libxcb-shm.so.0()(64bit)
Requires:       libxcb-sync.so.1()(64bit)
Requires:       libxcb-util.so.1()(64bit)
Requires:       libxcb-xfixes.so.0()(64bit)
Requires:       libxcb-xkb.so.1()(64bit)
Requires:       libxkbcommon.so.0()(64bit)
Requires:       libxkbcommon-x11.so.0()(64bit)
Requires:       libxkbfile.so.1()(64bit)

%description
Arora is a Qt 6 / QtWebEngine (Chromium) web browser — a maintained
continuation of the 2009 Qt/WebKit Arora. The shell has been rebuilt
around privacy hardening, per-site containers, optional Tor windows
and a Vivaldi-style UI: icon-rail sidebar, two-level container tab
strip, reader mode, rich download cards and a command palette.

This package bundles the Qt 6.12 runtime and QtWebEngine process
under /usr/lib/arora (the upstream build targets a newer Qt than the
distro series ships), and statically links the optional Rust
components (arora-rustdl download engine, arora-rustcore when built)
into the binary — there are no separate rust .so artifacts to ship.
Arora is experimental software provided as is.

%prep
%setup -q

%build
export QTDIR="%{qtdir}"
export ARORA_PKG_JOBS="%{?_smp_build_ncpus}"
./.devin/pkg-rpm.sh build

%install
export QTDIR="%{qtdir}"
./.devin/pkg-rpm.sh stage "%{buildroot}"
%if 0%{?suse_version}
%suse_update_desktop_file arora
%endif

%clean
rm -rf "%{buildroot}"

%files
%defattr(-,root,root)
/usr/bin/arora
/usr/lib/arora/
/usr/share/arora/
/usr/share/applications/arora.desktop
/usr/share/icons/hicolor/16x16/apps/arora.png
/usr/share/icons/hicolor/32x32/apps/arora.png
/usr/share/icons/hicolor/128x128/apps/arora.png
/usr/share/icons/hicolor/512x512/apps/arora.png
/usr/share/icons/hicolor/scalable/apps/arora.svg
/usr/share/pixmaps/arora.xpm
%{_mandir}/man1/arora.1*
%dir /usr/share/doc/arora
%doc /usr/share/doc/arora/copyright
%doc /usr/share/doc/arora/changelog.gz
%doc /usr/share/doc/arora/THIRD-PARTY-NOTICES
%license /usr/share/doc/arora/LICENSE.GPL2
%license /usr/share/doc/arora/LICENSE.GPL3

%changelog
* Wed Oct 14 2026 Arora Developers <devin@cognition.ai>
- Initial RPM packaging of the Qt 6 port: bundled Qt/WebEngine
  runtime under /usr/lib/arora, statically-linked rust crates, FHS
  /usr/bin + /usr/share layout — same payload policy as the .deb.
