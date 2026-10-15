#!/bin/sh
# Exercise the update command without network access or changing the real install.
set -eu

BINARY=${1:?usage: test_update.sh BINARY}
TEST_DIR=$(mktemp -d)
trap 'rm -rf "$TEST_DIR"' EXIT INT TERM
INSTALL_DIR="$TEST_DIR/install"
MOCK_BIN="$TEST_DIR/mock-bin"
CURRENT_VERSION=$("$BINARY" --version | awk '{print $2}')
NEXT_VERSION="${CURRENT_VERSION%.*}.$((${CURRENT_VERSION##*.} + 1))"
mkdir -p "$INSTALL_DIR" "$MOCK_BIN"
cp "$BINARY" "$INSTALL_DIR/loutre-view"

cat > "$TEST_DIR/installer.sh" <<'SH'
#!/bin/sh
set -eu
cat > "$LOUTREVIEW_INSTALL_DIR/loutre-view" <<'BIN'
#!/bin/sh
printf 'updated fixture\n'
BIN
chmod 755 "$LOUTREVIEW_INSTALL_DIR/loutre-view"
SH

cat > "$MOCK_BIN/curl" <<'SH'
#!/bin/sh
set -eu
while [ "$#" -gt 0 ]; do
    case "$1" in
        --output) output=$2; shift 2 ;;
        https://api.github.com/repos/Mare02/LoutreView/releases/latest) latest=true; shift ;;
        *) shift ;;
    esac
done
[ -n "${output:-}" ]
if [ "${latest:-false}" = true ]; then
    printf '{"tag_name":"v%s"}\n' "$LOUTREVIEW_TEST_LATEST_VERSION" > "$output"
else
    cp "$LOUTREVIEW_TEST_INSTALLER" "$output"
fi
SH
chmod 755 "$MOCK_BIN/curl"

PATH="$INSTALL_DIR:$MOCK_BIN:$PATH" \
    LOUTREVIEW_TEST_LATEST_VERSION="$CURRENT_VERSION" \
    LOUTREVIEW_TEST_INSTALLER="$TEST_DIR/installer.sh" \
    loutre-view update > "$TEST_DIR/current-output" 2>&1
grep -F "loutre-view $CURRENT_VERSION is already up to date" "$TEST_DIR/current-output" >/dev/null
[ "$("$INSTALL_DIR/loutre-view" --version)" = "loutre-view $CURRENT_VERSION" ]

PATH="$INSTALL_DIR:$MOCK_BIN:$PATH" \
    LOUTREVIEW_TEST_LATEST_VERSION="$NEXT_VERSION" \
    LOUTREVIEW_TEST_INSTALLER="$TEST_DIR/installer.sh" \
    loutre-view update > "$TEST_DIR/update-output" 2>&1
[ "$("$INSTALL_DIR/loutre-view")" = "updated fixture" ]
printf 'Update command current-version and update fixtures passed\n'
