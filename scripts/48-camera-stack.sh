#!/bin/bash
# Builds and installs libcamera with the Pro1X camera sensors.
#
# Runs as root INSIDE the target (chroot or the running phone). Needs network.
#
#   NIXOS=/path/to/fxtec-pro1x-nixos ./48-camera-stack.sh
#
# The rear IMX582 and front S5K4H7 sit behind the CAMSS of the SM6115, which
# libcamera drives with its "simple" pipeline handler and software ISP (on the
# GPU via EGL/GLES since 0.6). Daedalus ships libcamera 0.0.3, without either,
# so v0.7.2 is built with the NixOS port's two sensor patches (camera sensor
# helpers and properties) and its tuning files. Verified on the phone: both
# cameras are listed by `cam -l` and deliver colour images, also through
# GStreamer's libcamerasrc.
set -euo pipefail
NIXOS=${NIXOS:?set NIXOS=<checkout of fxtec-pro1x-nixos>}
SRC=${SRC:-/root/src}
LIBCAMERA_TAG=v0.7.2
export DEBIAN_FRONTEND=noninteractive

apt-get install -y -q --no-install-recommends build-essential git pkg-config \
  ninja-build libyaml-dev python3-yaml python3-jinja2 python3-ply \
  libgnutls28-dev openssl libudev-dev libevent-dev libdrm-dev libjpeg-dev \
  libtiff-dev libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
  libegl-dev libgles-dev libepoxy-dev libdw-dev libunwind-dev \
  gstreamer1.0-tools gstreamer1.0-plugins-base gstreamer1.0-plugins-good v4l-utils
apt-get install -y -q -t daedalus-backports meson

mkdir -p "$SRC"
[ -d "$SRC/libcamera/.git" ] || git clone -q https://git.libcamera.org/libcamera/libcamera.git "$SRC/libcamera"
cd "$SRC/libcamera"
git fetch -q --tags origin
git checkout -q -f "$LIBCAMERA_TAG"
git clean -fdq
for p in "$NIXOS"/device/libcamera/*.patch; do
  git apply "$p"
done

rm -rf build
meson setup build -Dprefix=/usr/local -Dpipelines=simple -Dipas=simple \
  -Dcam=enabled -Dqcam=disabled -Dgstreamer=enabled -Dv4l2=enabled \
  -Ddocumentation=disabled -Dtest=false -Dlc-compliance=disabled -Dpycamera=disabled
ninja -C build install
ldconfig

# Tuning files for the software ISP (colour, black level, lens shading).
install -d /usr/local/share/libcamera/ipa/simple
install -m 644 "$NIXOS"/device/libcamera/*.yaml /usr/local/share/libcamera/ipa/simple/

# GStreamer only scans its own plugin directory.
ln -sfn /usr/local/lib/aarch64-linux-gnu/gstreamer-1.0/libgstlibcamera.so \
  /usr/lib/aarch64-linux-gnu/gstreamer-1.0/libgstlibcamera.so

# The software ISP allocates frame buffers from dma-buf heaps; let the video
# group (user "user" is in it) use them.
cat > /etc/udev/rules.d/84-pro1x-camera.rules <<'EOF'
# libcamera's software ISP allocates frame buffers from these dma-buf providers.
SUBSYSTEM=="dma_heap", GROUP="video", MODE="0660"
KERNEL=="udmabuf", GROUP="video", MODE="0660"
EOF
echo ">> done"
