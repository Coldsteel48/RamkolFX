# COPR "make_srpm" source method (https://docs.pagure.org/copr.copr/user_documentation.html#make-srpm).
# COPR clones this repo at the requested tag and runs, as root in a Fedora
# chroot with network access:
#     make -f .copr/Makefile srpm outdir=<dir> spec=<spec>
# The spec's sources (the release tarball of its version and the vendored
# libmysofa / KissFFT / Dear ImGui) are downloaded here, so the RPM build
# itself runs offline, as COPR requires.

spec ?= packaging/rpm/ramkolfx.spec
outdir ?= .

srpm:
	dnf -y install rpmdevtools rpm-build
	src=$$(mktemp -d) && \
	spectool -g -C "$$src" "$(spec)" && \
	rpmbuild -bs --define "_sourcedir $$src" --define "_srcrpmdir $(outdir)" "$(spec)"

.PHONY: srpm
