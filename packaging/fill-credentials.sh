#!/usr/bin/env bash
#
# Injects the Dropbox app key into the packaging files.
#
# Only the app key, never a client secret: sign-in uses OAuth with PKCE, which
# exists precisely so that a native application -- a "public client" under
# RFC 8252 -- needs no secret. There is nothing here that stamping a secret in
# would make work, and plenty it would make worse.
#
# The key is not a secret in any meaningful sense either, but it is also not
# something to commit to this repository by accident, so it lives in .env
# (gitignored) and is stamped into the packaging files only when you are
# cutting a release.
#
# Usage:
#     packaging/fill-credentials.sh            # read from .env
#     packaging/fill-credentials.sh --check    # report status, change nothing
#     packaging/fill-credentials.sh --clear    # put the placeholders back
#
# The key may also come from the environment, which is what CI should do:
#     DROPBOX_APP_KEY=... packaging/fill-credentials.sh

set -euo pipefail

cd "$(dirname "$0")/.."

PKGBUILD='packaging/arch/kio-dropbox/PKGBUILD'
PKGBUILD_GIT='packaging/arch/kio-dropbox-git/PKGBUILD'

ALL_FILES=("$PKGBUILD" "$PKGBUILD_GIT")

mode="${1:-fill}"

if [[ "$mode" == "--check" ]]; then
    for f in "${ALL_FILES[@]}"; do
        # An unfilled line ends right after the "=", or holds an empty ''.
        if grep -qE "^_dropbox_app_key=('')?$" "$f"; then
            echo "empty:  $f"
        else
            echo "filled: $f"
        fi
    done
    exit 0
fi

if [[ "$mode" == "--clear" ]]; then
    sed -i -E "s|^(_dropbox_app_key=).*|\1''|" "${ALL_FILES[@]}"
    echo "Placeholders restored. Remember to do this before committing."
    exit 0
fi

# Environment wins over .env, so CI can supply it without a file on disk.
if [[ -z "${DROPBOX_APP_KEY:-}" ]]; then
    if [[ -f .env ]]; then
        # shellcheck disable=SC1091
        set -a; . ./.env; set +a
    fi
fi

: "${DROPBOX_APP_KEY:?set DROPBOX_APP_KEY, or put it in .env}"

# The value is inserted into sed replacement text below. Escape its replacement
# metacharacters so a valid key containing &, |, or a backslash is kept
# byte-for-byte rather than changing the package recipe.
escape_sed_replacement() {
    printf '%s' "$1" | sed -e 's/[\\&|]/\\&/g'
}

app_key_escaped="$(escape_sed_replacement "$DROPBOX_APP_KEY")"

# DROPBOX_APP_SECRET is deliberately ignored even when .env defines it. It has
# no role in the PKCE flow, and a package is the last place a real secret
# should end up.
sed -i -E "s|^(_dropbox_app_key=).*|\1'${app_key_escaped}'|" "${ALL_FILES[@]}"

echo "Stamped the app key into:"
printf '  %s\n' "${ALL_FILES[@]}"
echo
echo "Run --clear before committing to this repo."
