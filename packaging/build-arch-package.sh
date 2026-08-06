#!/usr/bin/env bash
#
# Builds a pacman package (*.pkg.tar.zst) from the current checkout using the
# same PKGBUILD used for release packages.
#
#   packaging/build-arch-package.sh            # -> dist/*.pkg.tar.zst
#   packaging/build-arch-package.sh --outdir X # write somewhere else
#
# The release PKGBUILD downloads a release tarball, which does not exist yet
# while a release is being cut. So the source array is repointed at a tarball
# made from this checkout with `git archive` and the checksum recomputed.
#
# Note that `git archive HEAD` packages the last commit, not the working tree:
# uncommitted changes are invisible to this script, exactly as they are to a
# real release. Commit before wondering why an edit had no effect.
# Everything else -- the dependency list, the build flags, the check() step --
# is exactly what an Arch user will run, which is the point of building it this
# way rather than calling cmake directly.
#
# makepkg refuses to run as root, so under CI (which runs as root in the Arch
# container) the build is re-executed as an unprivileged user.

set -euo pipefail

readonly SELF="$(realpath "${BASH_SOURCE[0]}")"
readonly REPO_ROOT="$(cd "$(dirname "$SELF")/.." && pwd)"
readonly PKGNAME="kio-dropbox"

outdir="$REPO_ROOT/dist"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --outdir)
            outdir="$(realpath -m "$2")"
            shift 2
            ;;
        *)
            echo "unknown argument: $1" >&2
            exit 2
            ;;
    esac
done

# ---------------------------------------------------------------------------
# Running as root: set up a build user and hand the whole script over to it.
# ---------------------------------------------------------------------------
if [[ "$(id -u)" -eq 0 ]]; then
    if ! id builder &>/dev/null; then
        useradd -m builder
    fi
    # The packaging lint job may have created this user already. Provision the
    # narrowly-scoped sudo rule independently so makepkg can install declared
    # dependencies in either order.
    echo 'builder ALL=(ALL) NOPASSWD: /usr/bin/pacman' > /etc/sudoers.d/builder
    chmod 0440 /etc/sudoers.d/builder
    mkdir -p "$outdir"
    chown -R builder "$REPO_ROOT" "$outdir"
    exec sudo -u builder --preserve-env=GITHUB_REF_NAME \
        "$SELF" --outdir "$outdir"
fi

# ---------------------------------------------------------------------------
# Version and source tarball
# ---------------------------------------------------------------------------
cd "$REPO_ROOT"
git config --global --add safe.directory "$REPO_ROOT" 2>/dev/null || true

version="$(grep -oP "project\($PKGNAME VERSION \K[0-9]+\.[0-9]+\.[0-9]+" CMakeLists.txt)"
if [[ -z "$version" ]]; then
    echo "could not read the project version from CMakeLists.txt" >&2
    exit 1
fi
echo "==> building $PKGNAME $version"

workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT

tarball="$PKGNAME-$version.tar.gz"
git archive --format=tar.gz --prefix="$PKGNAME-$version/" \
    -o "$workdir/$tarball" HEAD

cp "packaging/arch/$PKGNAME/PKGBUILD" "$workdir/PKGBUILD"

# Point the source array at the local tarball instead of the release URL, and
# recompute the checksum to match it.
cd "$workdir"
sed -i "s|^source=(.*|source=(\"$tarball\")|" PKGBUILD
updpkgsums

# Suppress the separate -debug package. The Arch container's makepkg.conf
# enables `debug` by default, and a debug package attached to a release is just
# noise. This is set in the PKGBUILD rather than via the OPTIONS environment
# variable, because makepkg.conf is sourced after the environment and would
# override it.
printf "\noptions=('!debug')\n" >> PKGBUILD

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
# -s installs missing dependencies, -f overwrites a previous build of the same
# version.
makepkg -sf --noconfirm --noprogressbar

mkdir -p "$outdir"
mv ./*.pkg.tar.zst "$outdir/"

cd "$outdir"
for pkg in *.pkg.tar.zst; do
    sha256sum "$pkg" > "$pkg.sha256"
    echo "==> $outdir/$pkg"
done
