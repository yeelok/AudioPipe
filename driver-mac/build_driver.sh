#!/usr/bin/env bash
# Build the AudioPipe virtual audio device — a customized fork of BlackHole
# (https://github.com/ExistentialAudio/BlackHole, GPL-3.0). Customization is
# done entirely via build-time constants so the source stays mergeable with
# upstream. Produces build/AudioPipe.driver.
set -euo pipefail
cd "$(dirname "$0")"

DRIVER_NAME="AudioPipe"
BUNDLE_ID="audio.audiopipe.AudioPipe"
BUILD_DIR="$PWD/build"

# AudioPipe transports a fixed 48 kHz / 2ch / float32 stream (spec §4), so the
# device advertises exactly that — no extra sample rates to negotiate.
# Ad-hoc code signing (identity "-") with no development team: enough for a
# locally-built HAL plug-in on a personal machine, no Apple Developer account.
xcodebuild \
  -project BlackHole.xcodeproj \
  -target BlackHole \
  -configuration Release \
  CONFIGURATION_BUILD_DIR="$BUILD_DIR" \
  MACOSX_DEPLOYMENT_TARGET=12.3 \
  CODE_SIGN_STYLE=Manual \
  CODE_SIGN_IDENTITY="-" \
  DEVELOPMENT_TEAM="" \
  PROVISIONING_PROFILE_SPECIFIER="" \
  PRODUCT_NAME="$DRIVER_NAME" \
  PRODUCT_BUNDLE_IDENTIFIER="$BUNDLE_ID" \
  GCC_PREPROCESSOR_DEFINITIONS='$GCC_PREPROCESSOR_DEFINITIONS
  kDriver_Name=\"'"$DRIVER_NAME"'\"
  kPlugIn_BundleID=\"'"$BUNDLE_ID"'\"
  kDevice_Name=\"AudioPipe\"
  kManufacturer_Name=\"AudioPipe\"
  kNumber_Of_Channels=2
  kSampleRates=48000
  kLatency_Frame_Size=0' \
  build

echo
echo "Built: $BUILD_DIR/$DRIVER_NAME.driver"
echo "Install with: sudo ./install.sh"
