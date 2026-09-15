#!/bin/sh

set -eu

REPOSITORY="Mare02/LoutreView"
INSTALL_DIR="${LOUTREVIEW_INSTALL_DIR:-$HOME/.local/bin}"
VERSION="${LOUTREVIEW_VERSION:-latest}"

say() { printf '%s\n' "$*"; }
fail() { printf 'loutre-view installer: %s\n' "$*" >&2; exit 1; }

command -v curl >/dev/null 2>&1 || fail "curl is required"
command -v tar >/dev/null 2>&1 || fail "tar is required"
if command -v sha256sum >/dev/null 2>&1; then
  CHECKSUM_TOOL=sha256sum
elif command -v shasum >/dev/null 2>&1; then
  CHECKSUM_TOOL=shasum
else
  fail "sha256sum or shasum is required"
fi

case "$(uname -s)" in
  Darwin) PLATFORM=darwin ;;
  Linux) PLATFORM=linux ;;
  *) fail "unsupported operating system: $(uname -s)" ;;
esac

case "$(uname -m)" in
  arm64|aarch64) ARCH=arm64 ;;
  x86_64|amd64) ARCH=amd64 ;;
  *) fail "unsupported architecture: $(uname -m)" ;;
esac
ASSET="loutre-view-$PLATFORM-$ARCH"

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
if [ "$CHECKSUM_TOOL" = sha256sum ]; then
  ACTUAL="$(sha256sum "$ARCHIVE")"
else
  ACTUAL="$(shasum -a 256 "$ARCHIVE")"
fi
ACTUAL="${ACTUAL%% *}"
[ "$EXPECTED" = "$ACTUAL" ] || fail "checksum verification failed"

mkdir -p "$INSTALL_DIR"
tar -xzf "$ARCHIVE" -C "$TEMP_DIR" loutre-view
BINARY="$TEMP_DIR/loutre-view"
[ -f "$BINARY" ] && [ ! -L "$BINARY" ] || fail "release archive did not contain a regular loutre-view binary at its root"
install -m 755 "$BINARY" "$INSTALL_DIR/loutre-view"
say "Installed loutre-view to $INSTALL_DIR/loutre-view"

case ":${PATH:-}:" in
  *:"$INSTALL_DIR":*) say "Run: loutre-view --help" ;;
  *) say "Add this directory to your PATH: export PATH=\"$INSTALL_DIR:\$PATH\"" ;;
esac
