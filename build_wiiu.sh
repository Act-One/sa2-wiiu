#!/bin/bash
set -e
export DEVKITPRO=/opt/devkitpro
export DEVKITPPC=/opt/devkitpro/devkitPPC
export WUT_ROOT=/opt/devkitpro/wut
export PATH="$DEVKITPPC/bin:$DEVKITPRO/tools/bin:$PATH"
cd "$(dirname "$0")"

if [[ -f "Makefile" ]]; then
	make -f Makefile -I . PLATFORM=wiiu CPU_ARCH=ppc "$@"
elif [[ -f "sa2-wiiu/Makefile" ]]; then
	make -f sa2-wiiu/Makefile -I sa2-wiiu/ PLATFORM=wiiu CPU_ARCH=ppc "$@"
else
	echo "Error: could not find Makefile or sa2-wiiu/Makefile" >&2
	exit 1
fi

should_package=1
for arg in "$@"; do
	case "$arg" in
	clean | tidy | clean-tools)
		should_package=0
		;;
	esac
done

if [[ "$should_package" -eq 1 && -f "sa2.rpx" ]]; then
	WIIU_ASSET_DIR="${WIIU_ASSET_DIR:-assets/wiiu}"
	WUHB_OUTPUT="${WUHB_OUTPUT:-sa2.wuhb}"
	WIIU_ICON="${WIIU_ICON:-$WIIU_ASSET_DIR/icon.png}"
	WIIU_TV_IMAGE="${WIIU_TV_IMAGE:-$WIIU_ASSET_DIR/banner_tv.png}"
	WIIU_DRC_IMAGE="${WIIU_DRC_IMAGE:-$WIIU_ASSET_DIR/banner_drc.png}"

	[[ -f "$WIIU_TV_IMAGE" ]] || WIIU_TV_IMAGE="$WIIU_ASSET_DIR/banner.png"
	[[ -f "$WIIU_DRC_IMAGE" ]] || WIIU_DRC_IMAGE="$WIIU_ASSET_DIR/banner.png"

	if command -v wuhbtool >/dev/null 2>&1; then
		wuhbtool "sa2.rpx" "$WUHB_OUTPUT" \
			--name="Sonic Advance 2" \
			--short-name="SA2" \
			--author="Sonic Team / SA2 Decomp" \
			--icon="$WIIU_ICON" \
			--tv-image="$WIIU_TV_IMAGE" \
			--drc-image="$WIIU_DRC_IMAGE"
	else
		echo "Warning: wuhbtool not found; skipping WUHB package." >&2
	fi
else
	if [[ "$should_package" -eq 1 ]]; then
		echo "Warning: sa2.rpx was not produced; skipping WUHB package." >&2
	fi
fi
# Simple build script for the Wii U port. Feel free to modify or remove as needed.
# Was used for ease of testing and building.
