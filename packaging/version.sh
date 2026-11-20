#!/usr/bin/env bash
# RamkolFX version management. The single source of truth is ./VERSION, a
# SemVer 2.0 string: MAJOR.MINOR.PATCH or MAJOR.MINOR.PATCH-<stage>.<N> with
# stage one of alpha | beta | rc  (e.g. 0.1.0-alpha.1, 0.1.0-rc.2, 0.1.0).
#
#   packaging/version.sh show            print every distro-specific rendering
#   packaging/version.sh sync            rewrite the recipes (and flake.nix) from VERSION
#   packaging/version.sh check [TAG]     fail if recipes are out of sync, or if
#                                        TAG (e.g. v0.1.0-alpha.1) != VERSION
#
# Per-format spelling of a pre-release (each sorts *before* the final release):
#   git tag / SemVer   0.1.0-alpha.1
#   Debian             0.1.0~alpha.1-1
#   RPM                Version 0.1.0, Release 0.1.alpha1   (final: Release 1)
#   Arch / Alpine /
#   Gentoo             0.1.0_alpha1
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

V=$(tr -d '[:space:]' < VERSION)
re='^([0-9]+\.[0-9]+\.[0-9]+)(-(alpha|beta|rc)\.([0-9]+))?$'
if [[ ! $V =~ $re ]]; then
    echo "error: VERSION '$V' is not MAJOR.MINOR.PATCH[-(alpha|beta|rc).N]" >&2
    exit 1
fi
BASE=${BASH_REMATCH[1]}
STAGE=${BASH_REMATCH[3]:-}
N=${BASH_REMATCH[4]:-}

if [[ -n $STAGE ]]; then
    DEB="$BASE~$STAGE.$N-1"
    RPM_REL="0.1.$STAGE$N"
    UNDERSCORE="${BASE}_$STAGE$N"
else
    DEB="$BASE-1"
    RPM_REL="1"
    UNDERSCORE="$BASE"
fi

EBUILD_DIR=packaging/gentoo/media-sound/ramkolfx

show() {
    cat <<EOF
semver (tag v$V)  $V
debian            $DEB
rpm               Version $BASE, Release $RPM_REL
arch/alpine/gentoo $UNDERSCORE
EOF
}

sync() {
    # Arch
    sed -i -E "s/^_semver=.*/_semver=$V/; s/^pkgver=.*/pkgver=$UNDERSCORE/" packaging/arch/PKGBUILD
    # Alpine
    sed -i -E "s/^_semver=.*/_semver=$V/; s/^pkgver=.*/pkgver=$UNDERSCORE/; s/^SKIP  RamkolFX-.*\.tar\.gz/SKIP  RamkolFX-$V.tar.gz/" \
        packaging/alpine/APKBUILD
    # RPM
    sed -i -E "s/^%global upstream_version .*/%global upstream_version $V/; s/^Version:( +).*/Version:\1$BASE/; s/^Release:( +).*/Release:\1$RPM_REL%{?dist}/" \
        packaging/rpm/ramkolfx.spec
    # Nix
    sed -i -E "s/^( +version = \")[^\"]*(\";)/\1$V\2/" flake.nix
    # Gentoo: the version lives in the file name; MY_PV is the upstream tag
    local target="$EBUILD_DIR/ramkolfx-$UNDERSCORE.ebuild" f
    for f in "$EBUILD_DIR"/ramkolfx-*.ebuild; do
        [[ $f == "$target" ]] || git mv "$f" "$target" 2>/dev/null || mv "$f" "$target"
    done
    sed -i -E "s/^MY_PV=.*/MY_PV=\"$V\"/" "$target"

    # Changelogs: add an entry the first time a new version is synced.
    local maint="Roman Levin (Coldsteel48) <noreply@github.com>"
    if ! head -1 packaging/debian/changelog | grep -qF "($DEB)"; then
        {
            printf 'ramkolfx (%s) unstable; urgency=medium\n\n  * Release %s.\n\n -- %s  %s\n\n' \
                "$DEB" "$V" "$maint" "$(date -R)"
            cat packaging/debian/changelog
        } > packaging/debian/changelog.new
        mv packaging/debian/changelog.new packaging/debian/changelog
    fi
    if ! grep -qF -- "- $BASE-$RPM_REL" packaging/rpm/ramkolfx.spec; then
        local entry
        entry=$(printf '* %s %s - %s-%s\n- Release %s\n' "$(date '+%a %b %d %Y')" "$maint" "$BASE" "$RPM_REL" "$V")
        awk -v e="$entry" '{print} /^%changelog/ {print e; print ""}' packaging/rpm/ramkolfx.spec \
            > packaging/rpm/ramkolfx.spec.new
        mv packaging/rpm/ramkolfx.spec.new packaging/rpm/ramkolfx.spec
    fi
    # AppStream: newest release first; alpha/beta/rc marked as development.
    local metainfo=packaging/common/io.github.coldsteel48.RamkolFX.metainfo.xml
    if ! grep -qF "<release version=\"$V\"" "$metainfo"; then
        local type=""
        [[ -n $STAGE ]] && type=' type="development"'
        sed -i "s|^\(  *\)<releases>\$|&\n\1  <release version=\"$V\" date=\"$(date +%F)\"$type/>|" "$metainfo"
    fi
}

# semver_key 0.1.0-rc.2 -> "0 1 0 3 2": stage alpha=1 beta=2 rc=3, final=4.
semver_key() {
    [[ $1 =~ $re ]] || return 1
    local rank
    case ${BASH_REMATCH[3]:-} in
        alpha) rank=1 ;; beta) rank=2 ;; rc) rank=3 ;; *) rank=4 ;;
    esac
    echo "${BASH_REMATCH[1]//./ } $rank ${BASH_REMATCH[4]:-0}"
}

# semver_gt A B: true if A sorts after B.
semver_gt() {
    local -a a b
    read -ra a <<< "$(semver_key "$1")"
    read -ra b <<< "$(semver_key "$2")"
    local i
    for i in 0 1 2 3 4; do
        (( a[i] > b[i] )) && return 0
        (( a[i] < b[i] )) && return 1
    done
    return 1
}

# VERSION must be newer than every release tag in this branch's history, and
# CHANGELOG.md must have a section for it and only link to tags that exist
# (v$V itself may not be tagged yet when this runs before tagging).
check_history() {
    if [[ $(git rev-parse --is-shallow-repository) == true ]]; then
        echo "error: shallow clone; fetch full history and tags (actions/checkout fetch-depth: 0)" >&2
        exit 1
    fi
    local t err=0
    while read -r t; do
        [[ $t == "v$V" || ! ${t#v} =~ $re ]] && continue
        if ! semver_gt "$V" "${t#v}"; then
            echo "error: VERSION '$V' is not newer than existing release tag '$t'" >&2
            err=1
        fi
    done < <(git tag --merged HEAD)

    local vre=${V//./\\.}
    if ! grep -qE "^## \[$vre\]( |$)" CHANGELOG.md; then
        echo "error: CHANGELOG.md has no '## [$V]' section" >&2
        err=1
    fi
    if ! grep -qE "^\[$vre\]: " CHANGELOG.md; then
        echo "error: CHANGELOG.md has no '[$V]: ' link reference" >&2
        err=1
    fi
    while read -r t; do
        [[ $t == "v$V" ]] && continue
        if ! git rev-parse -q --verify "refs/tags/$t" > /dev/null; then
            echo "error: CHANGELOG.md links to tag '$t', which does not exist" >&2
            err=1
        fi
    done < <(grep -E '^\[[^]]+\]: ' CHANGELOG.md | sed -E 's#\.\.\.|/# #g' \
        | grep -oE '(^| )v[0-9][0-9A-Za-z.-]*' | tr -d ' ' | sort -u)
    (( err == 0 )) || exit 1
}

check() {
    local tag="${1:-}"
    if [[ -n $tag && $tag != "v$V" ]]; then
        echo "error: tag '$tag' does not match VERSION '$V' (expected v$V)" >&2
        exit 1
    fi
    check_history
    local before after
    before=$(git diff HEAD -- packaging flake.nix | sha256sum)
    sync
    after=$(git diff HEAD -- packaging flake.nix | sha256sum)
    if [[ $before != "$after" ]]; then
        echo "error: packaging recipes are out of sync with VERSION; run packaging/version.sh sync" >&2
        git diff HEAD --stat -- packaging flake.nix >&2
        exit 1
    fi
    echo "version $V: recipes in sync"
}

case "${1:-show}" in
    show) show ;;
    sync) sync; show ;;
    check) check "${2:-}" ;;
    *) echo "usage: $0 show|sync|check [TAG]" >&2; exit 1 ;;
esac
