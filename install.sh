#!/bin/sh

set -eu

REPOSITORY="Mare02/LoutreView"
INSTALL_DIR="${LOUTREVIEW_INSTALL_DIR:-$HOME/.local/bin}"
VERSION="${LOUTREVIEW_VERSION:-latest}"

say() { printf '%s\n' "$*"; }
fail() { printf 'loutre-view installer: %s\n' "$*" >&2; exit 1; }

command -v curl >/dev/null 2>&1 || fail "curl is required"
command -v tar >/dev/null 2>&1 || fail "tar is required"
command -v shasum >/dev/null 2>&1 || fail "shasum is required"
[ "$(uname -s)" = "Darwin" ] || fail "LoutreView currently supports macOS only"

case "$(uname -m)" in
  arm64) ASSET="loutre-view-darwin-arm64" ;;
  x86_64) ASSET="loutre-view-darwin-amd64" ;;
  *) fail "unsupported macOS architecture: $(uname -m)" ;;
esac

if [ "$VERSION" = "latest" ]; then
  RELEASE_URL="https://github.com/$REPOSITORY/releases/latest/download"
else
  RELEASE_URL="https://github.com/$REPOSITORY/releases/download/$VERSION"
fi

TEMP_DIR="$(mktemp -d 2>/dev/null || mktemp -d -t loutre-view)"
trap 'rm -rf "$TEMP_DIR"' EXIT INT TERM
ARCHIVE="$TEMP_DIR/$ASSET.tar.gz"
CHECKSUMS="$TEMP_DIR/checksums.txt"

say "Downloading LoutreView for $(uname -m)..."
curl --fail --silent --show-error --location "$RELEASE_URL/$ASSET.tar.gz" -o "$ARCHIVE"
curl --fail --silent --show-error --location "$RELEASE_URL/checksums.txt" -o "$CHECKSUMS"
EXPECTED="$(awk -v file="$ASSET.tar.gz" '$2 == file || $2 == "*" file { print $1; exit }' "$CHECKSUMS")"
[ -n "$EXPECTED" ] || fail "checksum for $ASSET.tar.gz was not found"
ACTUAL="$(shasum -a 256 "$ARCHIVE" | awk '{print $1}')"
[ "$EXPECTED" = "$ACTUAL" ] || fail "checksum verification failed"

mkdir -p "$INSTALL_DIR"
tar -xzf "$ARCHIVE" -C "$TEMP_DIR"
BINARY="$(find "$TEMP_DIR" -type f -name loutre-view -perm -111 -print -quit)"
[ -n "$BINARY" ] || fail "release archive did not contain loutre-view"
install -m 755 "$BINARY" "$INSTALL_DIR/loutre-view"
say "Installed loutre-view to $INSTALL_DIR/loutre-view"

case ":${PATH:-}:" in
  *:"$INSTALL_DIR":*) say "Run: loutre-view --help" ;;
  *) say "Add this directory to your PATH: export PATH=\"$INSTALL_DIR:\$PATH\"" ;;
esac
