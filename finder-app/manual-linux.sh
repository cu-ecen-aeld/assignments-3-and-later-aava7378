#!/bin/bash
# Script outline to install and build kernel.
# Author: Siddhant Jajoo.
# ChatGPT use: https://chatgpt.com/share/698019cd-444c-8004-b4af-6a478bf515a4
set -e
set -u

OUTDIR=/tmp/aeld
KERNEL_REPO=git://git.kernel.org/pub/scm/linux/kernel/git/stable/linux-stable.git
KERNEL_VERSION=v5.15.163
BUSYBOX_VERSION=1_33_1
FINDER_APP_DIR=$(realpath $(dirname $0))
ARCH=arm64
CROSS_COMPILE=aarch64-none-linux-gnu-

if [ $# -lt 1 ]
then
	echo "Using default directory ${OUTDIR} for output"
else
	OUTDIR=$1
	echo "Using passed directory ${OUTDIR} for output"
fi

mkdir -p ${OUTDIR}

cd "$OUTDIR"
if [ ! -d "${OUTDIR}/linux-stable" ]; then
    #Clone only if the repository does not exist.
	echo "CLONING GIT LINUX STABLE VERSION ${KERNEL_VERSION} IN ${OUTDIR}"
	git clone ${KERNEL_REPO} --depth 1 --single-branch --branch ${KERNEL_VERSION}
fi
if [ ! -e ${OUTDIR}/linux-stable/arch/${ARCH}/boot/Image ]; then
    cd linux-stable
    echo "Checking out version ${KERNEL_VERSION}"
    git checkout ${KERNEL_VERSION}

    # TODO: Add your kernel build steps here
    # Build the kernel (Image) for ARM64 using the cross compiler
    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} mrproper
    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} defconfig
    make -j$(nproc) ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} Image

    # Copy the Image to OUTDIR
    cp arch/${ARCH}/boot/Image ${OUTDIR}/Image
fi

echo "Adding the Image in outdir"

echo "Creating the staging directory for the root filesystem"
cd "$OUTDIR"
if [ -d "${OUTDIR}/rootfs" ]
then
	echo "Deleting rootfs directory at ${OUTDIR}/rootfs and starting over"
    sudo rm  -rf ${OUTDIR}/rootfs
fi

# TODO: Create necessary base directories
mkdir -p ${OUTDIR}/rootfs
mkdir -p ${OUTDIR}/rootfs/{bin,dev,etc,home,lib,lib64,proc,sbin,sys,tmp,usr,var}
mkdir -p ${OUTDIR}/rootfs/usr/{bin,lib,sbin}
mkdir -p ${OUTDIR}/rootfs/var/log

cd "$OUTDIR"
if [ ! -d "${OUTDIR}/busybox" ]
then
git clone git://busybox.net/busybox.git
    cd busybox
    git checkout ${BUSYBOX_VERSION}
    # TODO:  Configure busybox
    make distclean
    make defconfig

else
    cd busybox
fi

# TODO: Make and install busybox
make -j$(nproc) ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE}
make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} CONFIG_PREFIX=${OUTDIR}/rootfs install

echo "Library dependencies"
${CROSS_COMPILE}readelf -a ${OUTDIR}/rootfs/bin/busybox | grep "program interpreter"
${CROSS_COMPILE}readelf -a ${OUTDIR}/rootfs/bin/busybox | grep "Shared library"

# TODO: Add library dependencies to rootfs
SYSROOT=$(${CROSS_COMPILE}gcc -print-sysroot)

# Program interpreter (dynamic linker) path (e.g., /lib/ld-linux-aarch64.so.1)
INTERPRETER=$(${CROSS_COMPILE}readelf -a bin/busybox | awk -F'[][]' '/program interpreter/ {print $2}')
if [ -n "${INTERPRETER}" ]; then
    sudo cp -a ${SYSROOT}${INTERPRETER} ${OUTDIR}/rootfs${INTERPRETER}
fi

# Shared libraries needed by busybox
LIBS=$(${CROSS_COMPILE}readelf -a bin/busybox | awk '/Shared library/ {gsub(/\[|\]/,"",$NF); print $NF}')

for L in ${LIBS}; do
    # Find library in sysroot and copy into rootfs lib/lib64 based on where it lives
    SRC=$(find ${SYSROOT} -name ${L} 2>/dev/null | head -n 1)
    if echo "${SRC}" | grep -q "/lib64/"; then
        sudo cp -a ${SRC} ${OUTDIR}/rootfs/lib64/
    else
        sudo cp -a ${SRC} ${OUTDIR}/rootfs/lib/
    fi
done

# TODO: Make device nodes
sudo mknod -m 666 ${OUTDIR}/rootfs/dev/null c 1 3 || true
sudo mknod -m 600 ${OUTDIR}/rootfs/dev/console c 5 1 || true

# TODO: Clean and build the writer utility
# Build writer using the ARM64 cross compiler
${CROSS_COMPILE}gcc -Wall -Werror -O2 -o ${FINDER_APP_DIR}/writer ${FINDER_APP_DIR}/writer.c

# TODO: Copy the finder related scripts and executables to the /home directory
# on the target rootfs
# Copy scripts and config files to rootfs/home
sudo mkdir -p ${OUTDIR}/rootfs/home/conf

sudo cp -a ${FINDER_APP_DIR}/finder.sh ${OUTDIR}/rootfs/home/
sudo cp -a ${FINDER_APP_DIR}/finder-test.sh ${OUTDIR}/rootfs/home/
sudo cp -a ${FINDER_APP_DIR}/writer ${OUTDIR}/rootfs/home/

sudo cp -a ${FINDER_APP_DIR}/conf/username.txt ${OUTDIR}/rootfs/home/conf/
sudo cp -a ${FINDER_APP_DIR}/conf/assignment.txt ${OUTDIR}/rootfs/home/conf/

# Copy autorun-qemu.sh if it exists in your tree
if [ -e ${FINDER_APP_DIR}/autorun-qemu.sh ]; then
    sudo cp -a ${FINDER_APP_DIR}/autorun-qemu.sh ${OUTDIR}/rootfs/home/
fi

# Update finder-test.sh to reference conf/assignment.txt (not ../conf/assignment.txt)
sudo sed -i 's|\.\./conf/assignment\.txt|conf/assignment.txt|g' ${OUTDIR}/rootfs/home/finder-test.sh

sudo chmod +x ${OUTDIR}/rootfs/home/finder.sh
sudo chmod +x ${OUTDIR}/rootfs/home/finder-test.sh
if [ -e ${OUTDIR}/rootfs/home/autorun-qemu.sh ]; then
    sudo chmod +x ${OUTDIR}/rootfs/home/autorun-qemu.sh
fi

# TODO: Chown the root directory
sudo chown -R root:root ${OUTDIR}/rootfs


# TODO: Create initramfs.cpio.gz
cd ${OUTDIR}/rootfs
find . -print0 | cpio --null -ov --format=newc | gzip -9 > ${OUTDIR}/initramfs.cpio.gz
