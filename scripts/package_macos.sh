#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-"$ROOT_DIR/build"}"
DIST_DIR="${DIST_DIR:-"$BUILD_DIR/dist"}"
APP_NAME="${APP_NAME:-MeshRepair}"
BUNDLE_ID="${BUNDLE_ID:-com.meshrepair.meshrepair}"
SIGN_IDENTITY="${SIGN_IDENTITY:-}"
NOTARY_PROFILE="${NOTARY_PROFILE:-}"
SKIP_NOTARY="${SKIP_NOTARY:-0}"
SKIP_DMG="${SKIP_DMG:-0}"
INCLUDE_ADDON_ZIP="${INCLUDE_ADDON_ZIP:-1}"
INCLUDE_CLI_ZIP="${INCLUDE_CLI_ZIP:-1}"

VERSION="$(
    sed -nE 's/^[[:space:]]*VERSION[[:space:]]+([0-9]+(\.[0-9]+){1,3}).*/\1/p' "$ROOT_DIR/CMakeLists.txt" | head -n 1
)"
if [[ -z "$VERSION" ]]; then
    echo "Could not read MeshRepair project version from CMakeLists.txt" >&2
    exit 1
fi

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "macOS packaging must run on macOS." >&2
    exit 1
fi

if [[ -z "$SIGN_IDENTITY" ]]; then
    SIGN_IDENTITY="-"
    SKIP_NOTARY=1
    echo "SIGN_IDENTITY is not set; using ad-hoc signing and skipping notarization." >&2
fi
if [[ "$SKIP_DMG" == "1" ]]; then
    SKIP_NOTARY=1
fi
if [[ "$SKIP_NOTARY" != "1" && -z "$NOTARY_PROFILE" ]]; then
    echo "NOTARY_PROFILE is required unless SKIP_NOTARY=1." >&2
    echo "Create one with: xcrun notarytool store-credentials <profile-name> --apple-id <email> --team-id <team-id>" >&2
    exit 1
fi
if [[ "$SIGN_IDENTITY" == "-" ]]; then
    SIGN_TIMESTAMP_ARGS=(--timestamp=none)
else
    SIGN_TIMESTAMP_ARGS=(--timestamp)
fi

GUI_EXECUTABLE="$BUILD_DIR/meshrepair-gui/meshrepair_gui"
CLI_EXECUTABLE="$BUILD_DIR/meshrepair"
PLIST_TEMPLATE="$ROOT_DIR/packaging/macos/Info.plist.in"
ENTITLEMENTS="$ROOT_DIR/packaging/macos/MeshRepair.entitlements"
APP_ICON="$ROOT_DIR/packaging/macos/MeshRepair.icns"
APP="$DIST_DIR/$APP_NAME.app"
DMG="$DIST_DIR/${APP_NAME}-${VERSION}-macos.dmg"
STAGE="$DIST_DIR/dmg-stage"
ADDON_STAGE="$DIST_DIR/addon-stage"
ADDON_ZIP="$DIST_DIR/meshrepair_blender-${VERSION}-macos.zip"
CLI_STAGE="$DIST_DIR/cli-stage"
CLI_ZIP="$DIST_DIR/meshrepair-cli-${VERSION}-macos.zip"
FRAMEWORKS_DIR="$APP/Contents/Frameworks"
APP_EXECUTABLE="$APP/Contents/MacOS/$APP_NAME"
HELPERS_DIR="$APP/Contents/Helpers"
APP_CLI="$HELPERS_DIR/meshrepair"

cleanup()
{
    rm -rf "$STAGE" "$ADDON_STAGE" "$CLI_STAGE" "$DIST_DIR/dylib-queue.txt" "$DIST_DIR/dylib-processed.txt"
}
trap cleanup EXIT

is_bundled_dependency()
{
    case "$1" in
        /opt/homebrew/*|/usr/local/*|"$BUILD_DIR"/*|"$ROOT_DIR"/build/*)
            return 0
            ;;
        *)
            return 1
            ;;
    esac
}

dependency_basename()
{
    basename "$1"
}

framework_dependency_path()
{
    printf '%s/%s\n' "$FRAMEWORKS_DIR" "$(dependency_basename "$1")"
}

collect_bundled_dependencies()
{
    otool -L "$1" | awk 'NR > 1 { print $1 }' | while IFS= read -r dep; do
        if is_bundled_dependency "$dep"; then
            printf '%s\n' "$dep"
        fi
    done
}

queue_dependency()
{
    if ! grep -Fxq "$1" "$DIST_DIR/dylib-queue.txt"; then
        printf '%s\n' "$1" >> "$DIST_DIR/dylib-queue.txt"
    fi
}

rewrite_dependency_references()
{
    local binary="$1"
    local loader_prefix="$2"
    collect_bundled_dependencies "$binary" | while IFS= read -r dep; do
        install_name_tool -change "$dep" "$loader_prefix/$(dependency_basename "$dep")" "$binary"
    done
}

bundle_dylibs()
{
    : > "$DIST_DIR/dylib-queue.txt"
    : > "$DIST_DIR/dylib-processed.txt"

    collect_bundled_dependencies "$APP_EXECUTABLE" | while IFS= read -r dep; do queue_dependency "$dep"; done
    collect_bundled_dependencies "$APP_CLI" | while IFS= read -r dep; do queue_dependency "$dep"; done

    while IFS= read -r dep; do
        if grep -Fxq "$dep" "$DIST_DIR/dylib-processed.txt"; then
            continue
        fi

        local dest
        dest="$(framework_dependency_path "$dep")"
        echo "Bundling dylib: $dep"
        ditto --noextattr --noacl "$dep" "$dest"
        chmod u+w "$dest"
        install_name_tool -id "@executable_path/../Frameworks/$(dependency_basename "$dep")" "$dest"

        collect_bundled_dependencies "$dest" | while IFS= read -r nested_dep; do
            queue_dependency "$nested_dep"
        done

        printf '%s\n' "$dep" >> "$DIST_DIR/dylib-processed.txt"
    done < "$DIST_DIR/dylib-queue.txt"

    rewrite_dependency_references "$APP_EXECUTABLE" "@executable_path/../Frameworks"
    rewrite_dependency_references "$APP_CLI" "@executable_path/../Frameworks"
    find "$FRAMEWORKS_DIR" -type f -name '*.dylib' -print | while IFS= read -r dylib; do
        rewrite_dependency_references "$dylib" "@loader_path"
    done
}

sign_path()
{
    local path="$1"
    codesign --force \
        --sign "$SIGN_IDENTITY" \
        --options runtime \
        "${SIGN_TIMESTAMP_ARGS[@]}" \
        "$path"
}

sign_app_path()
{
    local app_path="$1"
    find "$app_path/Contents/Frameworks" -type f -name '*.dylib' -print 2>/dev/null | while IFS= read -r dylib; do
        sign_path "$dylib"
    done

    sign_path "$app_path/Contents/Helpers/meshrepair"

    codesign --force \
        --sign "$SIGN_IDENTITY" \
        --options runtime \
        "${SIGN_TIMESTAMP_ARGS[@]}" \
        --entitlements "$ENTITLEMENTS" \
        "$app_path"
}

if [[ ! -x "$GUI_EXECUTABLE" ]]; then
    echo "Missing GUI executable: $GUI_EXECUTABLE" >&2
    echo "Run: cmake --build \"$BUILD_DIR\" --config Release --target meshrepair_gui" >&2
    exit 1
fi
if [[ ! -x "$CLI_EXECUTABLE" ]]; then
    echo "Missing CLI executable: $CLI_EXECUTABLE" >&2
    echo "Run: cmake --build \"$BUILD_DIR\" --config Release --target meshrepair" >&2
    exit 1
fi
if [[ ! -f "$APP_ICON" ]]; then
    echo "Missing app icon: $APP_ICON" >&2
    echo "Run: scripts/generate_icons.py" >&2
    exit 1
fi

rm -rf "$APP" "$STAGE" "$ADDON_STAGE"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources" "$FRAMEWORKS_DIR" "$HELPERS_DIR" "$DIST_DIR" "$STAGE"

sed \
    -e "s|@APP_NAME@|$APP_NAME|g" \
    -e "s|@APP_EXECUTABLE_NAME@|$APP_NAME|g" \
    -e "s|@BUNDLE_ID@|$BUNDLE_ID|g" \
    -e "s|@VERSION@|$VERSION|g" \
    "$PLIST_TEMPLATE" > "$APP/Contents/Info.plist"

ditto --noextattr --noacl "$GUI_EXECUTABLE" "$APP_EXECUTABLE"
ditto --noextattr --noacl "$CLI_EXECUTABLE" "$APP_CLI"
ditto --noextattr --noacl "$APP_ICON" "$APP/Contents/Resources/MeshRepair.icns"
chmod 755 "$APP_EXECUTABLE" "$APP_CLI"

bundle_dylibs
sign_app_path "$APP"
codesign --verify --deep --strict --verbose=4 "$APP"

if [[ "$INCLUDE_ADDON_ZIP" == "1" ]]; then
    mkdir -p "$ADDON_STAGE/meshrepair_blender/bin/macos/universal" "$ADDON_STAGE/meshrepair_blender/bin/macos/Frameworks"
    ditto --noextattr --noacl "$ROOT_DIR/meshrepair_blender" "$ADDON_STAGE/meshrepair_blender"
    ditto --noextattr --noacl "$APP_CLI" "$ADDON_STAGE/meshrepair_blender/bin/macos/universal/meshrepair"
    if [[ -d "$FRAMEWORKS_DIR" ]]; then
        ditto --noextattr --noacl "$FRAMEWORKS_DIR" "$ADDON_STAGE/meshrepair_blender/bin/macos/Frameworks"
    fi
    chmod 755 "$ADDON_STAGE/meshrepair_blender/bin/macos/universal/meshrepair"
    find "$ADDON_STAGE/meshrepair_blender/bin/macos/Frameworks" -type f -name '*.dylib' -print | while IFS= read -r dylib; do
        sign_path "$dylib"
    done
    codesign --force \
        --sign "$SIGN_IDENTITY" \
        --options runtime \
        "${SIGN_TIMESTAMP_ARGS[@]}" \
        "$ADDON_STAGE/meshrepair_blender/bin/macos/universal/meshrepair"
    rm -f "$ADDON_ZIP"
    (cd "$ADDON_STAGE" && ditto -c -k --keepParent meshrepair_blender "$ADDON_ZIP")
fi

if [[ "$INCLUDE_CLI_ZIP" == "1" ]]; then
    rm -rf "$CLI_STAGE"
    mkdir -p "$CLI_STAGE/meshrepair-cli-${VERSION}"
    ditto --noextattr --noacl "$APP_CLI" "$CLI_STAGE/meshrepair-cli-${VERSION}/meshrepair"
    chmod 755 "$CLI_STAGE/meshrepair-cli-${VERSION}/meshrepair"
    cat > "$CLI_STAGE/meshrepair-cli-${VERSION}/README-cli.txt" <<EOF
MeshRepair CLI ${VERSION}

Run from this folder:
  ./meshrepair --help

Example:
  ./meshrepair input.obj repaired.obj --validate

Optional install for terminal use:
  sudo install -m 755 meshrepair /usr/local/bin/meshrepair
EOF
    ditto --noextattr --noacl "$ROOT_DIR/LICENSE" "$CLI_STAGE/meshrepair-cli-${VERSION}/LICENSE"
    codesign --force \
        --sign "$SIGN_IDENTITY" \
        --options runtime \
        "${SIGN_TIMESTAMP_ARGS[@]}" \
        "$CLI_STAGE/meshrepair-cli-${VERSION}/meshrepair"
    rm -f "$CLI_ZIP"
    (cd "$CLI_STAGE" && ditto -c -k --keepParent "meshrepair-cli-${VERSION}" "$CLI_ZIP")
fi

rm -rf "$STAGE"
mkdir -p "$STAGE"
ditto --noextattr --noacl "$APP" "$STAGE/$APP_NAME.app"
ln -s /Applications "$STAGE/Applications"
if [[ "$INCLUDE_ADDON_ZIP" == "1" ]]; then
    ditto --noextattr --noacl "$ADDON_ZIP" "$STAGE/$(basename "$ADDON_ZIP")"
fi
if [[ "$INCLUDE_CLI_ZIP" == "1" ]]; then
    ditto --noextattr --noacl "$CLI_ZIP" "$STAGE/$(basename "$CLI_ZIP")"
fi

if [[ "$SKIP_DMG" == "1" ]]; then
    echo "Skipping DMG creation because SKIP_DMG=1."
    echo "Created app bundle: $APP"
    if [[ "$INCLUDE_ADDON_ZIP" == "1" ]]; then
        echo "Created: $ADDON_ZIP"
    fi
    if [[ "$INCLUDE_CLI_ZIP" == "1" ]]; then
        echo "Created: $CLI_ZIP"
    fi
    exit 0
fi

hdiutil create \
    -volname "$APP_NAME Installer" \
    -srcfolder "$STAGE" \
    -ov \
    -format UDZO \
    "$DMG"

codesign --force --sign "$SIGN_IDENTITY" "${SIGN_TIMESTAMP_ARGS[@]}" "$DMG"
codesign --verify --verbose=4 "$DMG"

if [[ "$SKIP_NOTARY" != "1" ]]; then
    xcrun notarytool submit "$DMG" \
        --keychain-profile "$NOTARY_PROFILE" \
        --wait

    xcrun stapler staple "$DMG"
    xcrun stapler validate "$DMG"
else
    echo "Skipping notarization because SKIP_NOTARY=1."
fi

echo "Created: $DMG"
if [[ "$INCLUDE_ADDON_ZIP" == "1" ]]; then
    echo "Created: $ADDON_ZIP"
fi
if [[ "$INCLUDE_CLI_ZIP" == "1" ]]; then
    echo "Created: $CLI_ZIP"
fi
