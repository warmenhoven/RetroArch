#!/bin/bash

# Debug: Show environment
echo "PATH: $PATH"
echo "Running from: $(pwd)"
echo "Shell: $SHELL"
echo ""

DEVNAME="${1:-iPhone}"
SCHEME="RetroArch iOS Release"
WORKSPACE="RetroArch.xcworkspace"
BUNDLE_ID="org.warmenhoven.RetroArch"
CONFIG="Release"

# Resolve the device UDID by name
echo "Looking for device: $DEVNAME"
echo "Running: xcrun xctrace list devices"

# Check if xcrun is available
if ! command -v xcrun &> /dev/null; then
    echo "Error: xcrun not found in PATH"
    echo "Attempting to use full path /usr/bin/xcrun"
    # Use full path as fallback
    XCRUN="/usr/bin/xcrun"
    if [ ! -x "$XCRUN" ]; then
        echo "Error: xcrun not found at /usr/bin/xcrun either"
        exit 1
    fi
else
    XCRUN="xcrun"
fi

# Get raw output for debugging
RAW_OUTPUT=$($XCRUN xctrace list devices 2>&1)
echo "Raw xctrace output (first 5 lines):"
echo "$RAW_OUTPUT" | head -5
echo ""

# Try to find the device using xctrace (handle both space and dash versions of name)
# Also handle curly apostrophes (') vs straight apostrophes (') - Apple uses curly in device names
DEVNAME_SPACE=$(echo "$DEVNAME" | tr '-' ' ')
# Remove both straight (') and curly (') apostrophes - use sed for curly since tr can't handle multi-byte UTF-8
DEVNAME_NO_APOS=$(echo "$DEVNAME" | sed $'s/\xe2\x80\x99//g' | tr -d "'")
DEVNAME_SPACE_NO_APOS=$(echo "$DEVNAME_SPACE" | sed $'s/\xe2\x80\x99//g' | tr -d "'")
# Normalize curly apostrophes to straight in the output, then match against all variants
UDID=$(echo "$RAW_OUTPUT" | sed $'s/\xe2\x80\x99/\x27/g' | grep -iE "($DEVNAME|$DEVNAME_SPACE|$DEVNAME_NO_APOS|$DEVNAME_SPACE_NO_APOS)" | grep -v Simulator | sed -n 's/.*(\([^)]*\))$/\1/p' | head -1)

# If xctrace didn't work, try devicectl as backup
if [ -z "$UDID" ]; then
    echo "xctrace didn't find device, trying devicectl..."
    DEVICECTL_OUTPUT=$($XCRUN devicectl list devices 2>&1)
    echo "Devicectl output (first 5 lines):"
    echo "$DEVICECTL_OUTPUT" | head -5

    # Try to get UDID from devicectl (handle both space and dash versions, and apostrophe variants)
    DEVICE_LINE=$(echo "$DEVICECTL_OUTPUT" | sed $'s/\xe2\x80\x99/\x27/g' | grep -iE "($DEVNAME|$DEVNAME_SPACE|$DEVNAME_NO_APOS|$DEVNAME_SPACE_NO_APOS)" | grep -v Simulator | head -1)
    if [ -n "$DEVICE_LINE" ]; then
        # Extract identifier in UUID format (8-4-4-4-12 hex digits)
        UDID=$(echo "$DEVICE_LINE" | grep -oE '[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}')
        echo "Found via devicectl: $UDID"
    fi
fi

# Final fallback: use xcodebuild's available destinations to find the correct UDID
if [ -n "$UDID" ]; then
    # Check if the UDID we found works with xcodebuild
    if ! $XCRUN xcodebuild -showdestinations -workspace "$WORKSPACE" -scheme "$SCHEME" 2>&1 | grep -q "$UDID"; then
        echo "UDID from devicectl ($UDID) not compatible with xcodebuild, searching destinations..."
        
        # Try to find device in xcodebuild destinations (handle both space and dash versions, and apostrophe variants)
        XCODE_UDID=$($XCRUN xcodebuild -showdestinations -workspace "$WORKSPACE" -scheme "$SCHEME" 2>&1 | sed $'s/\xe2\x80\x99/\x27/g' | grep -iE "platform:iOS.*(name:$DEVNAME|name:$DEVNAME_SPACE|name:$DEVNAME_NO_APOS|name:$DEVNAME_SPACE_NO_APOS)" | grep -v Simulator | sed -n 's/.*id:\([^,]*\).*/\1/p' | head -1)

        if [ -n "$XCODE_UDID" ]; then
            echo "Found $DEVNAME UDID from xcodebuild destinations: $XCODE_UDID"
            UDID="$XCODE_UDID"
        else
            echo "Could not find $DEVNAME in xcodebuild destinations"
            echo "Available iOS devices:"
            $XCRUN xcodebuild -showdestinations -workspace "$WORKSPACE" -scheme "$SCHEME" 2>&1 | grep -E "platform:iOS.*arch:arm64" | grep -v Simulator
        fi
    fi
fi

[ -z "$UDID" ] && { echo "Device not found: $DEVNAME"; echo "Try running 'xcrun devicectl list devices' manually to see available devices"; exit 1; }

echo "Found device: $DEVNAME with UDID: $UDID"

# Check if device is available for wireless deployment
DEVICE_STATUS=$($XCRUN devicectl list devices 2>/dev/null | sed $'s/\xe2\x80\x99/\x27/g' | grep -iE "($DEVNAME|$DEVNAME_SPACE|$DEVNAME_NO_APOS|$DEVNAME_SPACE_NO_APOS)" | grep -v Simulator | head -1)
if echo "$DEVICE_STATUS" | grep -qE "(available \(paired\)|connected)"; then
    echo "✓ Device is connected and ready for deployment"
elif echo "$DEVICE_STATUS" | grep -q "unavailable"; then
    echo "Device is currently unavailable for wireless deployment."
    echo ""
    echo "To enable wireless deployment:"
    echo "  1. Connect your iPhone via USB cable"
    echo "  2. Unlock your iPhone and trust this computer"
    echo "  3. Ensure Developer Mode is enabled (Settings > Privacy & Security > Developer Mode)"
    echo "  4. Run: xcrun devicectl manage pair --device \"$UDID\""
    echo "  5. Once paired, disconnect USB and ensure both devices are on the same network"
    echo ""
    echo "Attempting to pair device now (requires USB connection)..."
    $XCRUN devicectl manage pair --device "$UDID" || {
        echo "Failed to pair. Please connect via USB and try again."
        exit 1
    }
fi

echo ""

# Get the default DerivedData path for incremental builds (shared with Xcode)
DERIVED_DATA_PATH=$(xcodebuild -showBuildSettings -workspace "$WORKSPACE" -scheme "$SCHEME" 2>/dev/null | awk -F= '/BUILD_ROOT/ {gsub(/^[ \t]+/, "", $2); gsub(/\/Build\/Products$/, "", $2); print $2; exit}')

if [ -z "$DERIVED_DATA_PATH" ]; then
    echo "Warning: Could not determine DerivedData path, using local ./Derived"
    DERIVED_DATA_PATH="./Derived"
    DERIVED_DATA_ARG="-derivedDataPath $DERIVED_DATA_PATH"
    APP_PATH="./Derived/Build/Products/${CONFIG}-iphoneos/RetroArch.app"
else
    echo "Using shared DerivedData path: $DERIVED_DATA_PATH"
    # Don't specify -derivedDataPath to use Xcode's default (for sharing with Xcode GUI)
    DERIVED_DATA_ARG=""
    APP_PATH="${DERIVED_DATA_PATH}/Build/Products/${CONFIG}-iphoneos/RetroArch.app"
fi

echo "Building for incremental builds (shared with Xcode)..."
echo "Command: xcodebuild -workspace $WORKSPACE -scheme \"$SCHEME\" -configuration $CONFIG -destination \"platform=iOS,id=$UDID\" $DERIVED_DATA_ARG build"

# Build the app
echo "Starting build at: $(date)"

# Double-check device is still available before building
echo "Verifying device connection before build..."
if ! $XCRUN xcodebuild -showdestinations -workspace "$WORKSPACE" -scheme "$SCHEME" 2>&1 | grep -q "$UDID"; then
    echo "❌ Device $UDID not found in available destinations"
    echo "Current available destinations:"
    $XCRUN xcodebuild -showdestinations -workspace "$WORKSPACE" -scheme "$SCHEME" 2>&1 | grep -E "(platform|name):" | head -10
    exit 1
fi
echo "✅ Device verified, proceeding with build"

# Build for that device with options to skip package resolution when possible
echo "Running xcodebuild (this may take a moment)..."

# Check if packages are already resolved, if not resolve them first
if [ ! -f "$WORKSPACE/xcshareddata/swiftpm/Package.resolved" ] || [ ! -d "${DERIVED_DATA_PATH:-./Derived}/SourcePackages" ]; then
    echo "Resolving Swift packages first..."
    xcodebuild \
        -workspace "$WORKSPACE" \
        -scheme "$SCHEME" \
        $DERIVED_DATA_ARG \
        -resolvePackageDependencies
fi

# Detect build output formatter
if command -v xcbeautify &> /dev/null; then
    FORMATTER="stdbuf -o0 xcbeautify"
    echo "Using xcbeautify to format build output"
elif command -v xcpretty &> /dev/null; then
    FORMATTER="stdbuf -o0 xcpretty"
    echo "Using xcpretty to format build output"
else
    FORMATTER="cat"
    echo "No formatter found (install xcbeautify or xcpretty for prettier output)"
fi

# Use stdbuf to force line buffering and tee to see output in real time
# Strip non-ASCII bytes (emoji) via tr - they break some terminal emulators
# Add flags to skip package resolution since we just did it
stdbuf -o0 xcodebuild \
    -workspace "$WORKSPACE" \
    -scheme "$SCHEME" \
    -configuration "$CONFIG" \
    -destination "platform=iOS,id=$UDID" \
    $DERIVED_DATA_ARG \
    -skipPackagePluginValidation \
    -skipMacroValidation \
    -onlyUsePackageVersionsFromResolvedFile \
    build 2>&1 | $FORMATTER | LC_ALL=C stdbuf -o0 tr -d '\200-\377' | stdbuf -o0 tee /tmp/xcodebuild.log

BUILD_EXIT_CODE=${PIPESTATUS[0]}
echo "Build finished at: $(date) with exit code: $BUILD_EXIT_CODE"

if [ $BUILD_EXIT_CODE -ne 0 ]; then
    echo "Build failed with exit code $BUILD_EXIT_CODE"
    
    # Check for recent xcresult bundles with more details
    LATEST_XCRESULT=$(find /var/folders -name "ResultBundle_*.xcresult" -newer /tmp -type d 2>/dev/null | head -1)
    if [ -n "$LATEST_XCRESULT" ]; then
        echo "Latest error result bundle: $LATEST_XCRESULT"
        echo "To analyze errors, run: xcrun xcresulttool get --path \"$LATEST_XCRESULT\""
    fi
    
    exit $BUILD_EXIT_CODE
fi

# Locate the .app we just built
[ ! -d "$APP_PATH" ] && { echo "App not found at: $APP_PATH"; exit 1; }

# Install to device
$XCRUN devicectl device install app \
      --device "$UDID" \
      "$APP_PATH" || exit $?

# Launch the app
$XCRUN devicectl device process launch \
      --device "$UDID" \
      "$BUNDLE_ID"
