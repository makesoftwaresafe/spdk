#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2017 Intel Corporation
#  All rights reserved.
#

# The purpose of this script is to provide a simple procedure for spinning up a new
# test environment capable of running our whole test suite. This script will install
# all of the necessary dependencies to run almost the complete test suite.

sudo() {
	"$(type -P sudo)" -E "$@"
}

set -e
shopt -s extglob nullglob

UPGRADE=false
INSTALL=false
CONF="rocksdb,fio,flamegraph,tsocks,qemu,libiscsi,nvmecli,qat,spdk,refspdk,vagrant,igb_uio,ice"
package_manager=

function pre_install() { :; }
function install() { :; }
function upgrade() { :; }

function usage() {
	echo "This script is intended to automate the environment setup for a linux virtual machine."
	echo "Please run this script as your regular user. The script will make calls to sudo as needed."
	echo ""
	echo "${0##*/}"
	echo "  -h --help"
	echo "  -u --upgrade Run $package_manager upgrade"
	echo "  -i --install-deps Install $package_manager based dependencies"
	echo "  -t --test-conf List of test configurations to enable (${CONF},irdma,lcov,bpftrace,doxygen)"
	echo "  -c --conf-path Path to configuration file"
	echo "  -d --dir-git Path to where git sources should be saved"
	echo "  -s --disable-tsocks Disable use of tsocks"
}

function error() {
	printf "%s\n\n" "$1" >&2
	usage
	return 1
}

function set_os_id_version() {
	if [[ -f /etc/os-release ]]; then
		source /etc/os-release
	elif [[ -f /usr/local/etc/os-release ]]; then
		# On FreeBSD file is located under /usr/local if etc_os-release package is installed
		source /usr/local/etc/os-release
	elif [[ $(uname -s) == FreeBSD ]]; then
		ID=freebsd
		VERSION_ID=$(freebsd-version)
		VERSION_ID=${VERSION_ID//.*/}
	else
		echo "File os-release not found" >&2
		return 1
	fi

	OSID=$ID
	OSVERSION=$VERSION_ID

	echo "OS-ID: $OSID | OS-Version: $OSVERSION"
}

function detect_package_manager() {
	local manager_scripts
	manager_scripts=("$vmsetupdir/pkgdep/"!(git))

	local package_manager_lib
	for package_manager_lib in "${manager_scripts[@]}"; do
		package_manager=${package_manager_lib##*/}
		if hash "${package_manager}" &> /dev/null; then
			source "${package_manager_lib}"
			return
		fi
	done
}

vmsetupdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$vmsetupdir/../../../")
source "$rootdir/scripts/common.sh"

set_os_id_version
detect_package_manager

if [[ -e $vmsetupdir/pkgdep/os/$OSID ]]; then
	source "$vmsetupdir/pkgdep/os/$OSID"
fi

# Parse input arguments #
while getopts 'd:siuht:c:-:' optchar; do
	case "$optchar" in
		-)
			case "$OPTARG" in
				help)
					usage
					exit 0
					;;
				upgrade) UPGRADE=true ;;
				install-deps) INSTALL=true ;;
				test-conf=*) CONF="${OPTARG#*=}" ;;
				conf-path=*) CONF_PATH="${OPTARG#*=}" ;;
				dir-git=*) GIT_REPOS="${OPTARG#*=}" ;;
				disable-tsocks) NO_TSOCKS=true ;;
				*) error "Invalid argument '$OPTARG'" ;;
			esac
			;;
		h)
			usage
			exit 0
			;;
		u) UPGRADE=true ;;
		i) INSTALL=true ;;
		t) CONF="$OPTARG" ;;
		c) CONF_PATH="$OPTARG" ;;
		d) GIT_REPOS="$OPTARG" ;;
		s) NO_TSOCKS=true ;;
		*) error "Invalid argument '$OPTARG'" ;;
	esac
done

if [[ -z $package_manager ]]; then
	echo "Supported package manager not found. Script supports:"
	printf " * %s\n" "${manager_scripts[@]##*/}"
	exit 1
fi

if [[ -n $CONF_PATH ]]; then
	if [[ ! -f $CONF_PATH ]]; then
		error "Configuration file does not exist: '$CONF_PATH'"
	fi
	source "$CONF_PATH"
fi

if $UPGRADE; then
	upgrade
fi

if $INSTALL; then
	sudo "$rootdir/scripts/pkgdep.sh" --all
	pre_install
	install "${packages[@]}"
fi

if [[ -n $CONF ]]; then
	source "$vmsetupdir/pkgdep/git"
	install_sources
fi
