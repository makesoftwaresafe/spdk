#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2020 Intel Corporation
#  All rights reserved.
#  Copyright (c) 2022 Dell Inc, or its subsidiaries.
#
# Minimal install
zypper install -y gcc gcc-c++ make cunit-devel libaio-devel libopenssl-devel \
	libuuid-devel python3-base ncurses-devel libjson-c-devel libcmocka-devel \
	ninja meson python3-devel python3-pyelftools fuse3-devel unzip

# use python3.11 for SLES <= 15 that ships with python3.6
if [[ ${VERSION_ID:0:2} -le "15" ]]; then
	zypper install -y python311-base python311-devel python311-Jinja2 python311-tabulate python311-pyelftools
	update-alternatives --install /usr/bin/python3 python3 /usr/bin/python3.11 1
	update-alternatives --set python3 /usr/bin/python3.11
fi

# per PEP668 work inside virtual env
virtdir=${PIP_VIRTDIR:-/var/spdk/dependencies/pip}
python3 -m venv --system-site-packages "$virtdir"
source "$virtdir/bin/activate"
python -m pip install -U "pip<26" setuptools wheel pip-tools
pip-compile --extra dev --strip-extras -o "$rootdir/scripts/pkgdep/requirements.txt" "${rootdir}/python/pyproject.toml"
pip3 install -r "$rootdir/scripts/pkgdep/requirements.txt"

# Fixes issue: #3721
pkgdep_toolpath meson "${virtdir}/bin"

# Additional dependencies for DPDK
zypper install -y libnuma-devel nasm
# Additional dependencies for ISA-L used in compression
zypper install -y autoconf automake libtool help2man
if [[ $INSTALL_DEV_TOOLS == "true" ]]; then
	# Tools for developers
	zypper install -y git-core lcov python3-pycodestyle sg3_utils \
		pciutils ShellCheck bash-completion
fi
if [[ $INSTALL_RBD == "true" ]]; then
	# Additional dependencies for RBD bdev in NVMe over Fabrics
	zypper install -y librados-devel librbd-devel
fi
if [[ $INSTALL_RDMA == "true" ]]; then
	# Additional dependencies for RDMA transport in NVMe over Fabrics
	zypper install -y rdma-core-devel
fi
if [[ $INSTALL_DOCS == "true" ]]; then
	# Additional dependencies for building docs
	zypper install -y doxygen graphviz
	[[ $VERSION != "16.0" ]] && zypper install -y mscgen
fi
if [[ $INSTALL_DAOS == "true" ]]; then
	if ! zypper lr daos_packages &> /dev/null; then
		zypper ar https://packages.daos.io/v2.0/Leap15/packages/x86_64/ daos_packages
	fi
	rpm --import https://packages.daos.io/RPM-GPG-KEY
	zypper --non-interactive refresh
	zypper install -y daos-client daos-devel
fi
if [[ $INSTALL_AVAHI == "true" ]]; then
	# Additional dependencies for Avahi
	zypper install -y avahi-devel
fi
if [[ $INSTALL_LZ4 == "true" ]]; then
	zypper install -y liblz4-devel
fi
