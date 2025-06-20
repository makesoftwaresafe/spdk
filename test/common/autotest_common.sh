#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation
#  All rights reserved.
#  Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#
rpc_py=rpc_cmd

function xtrace_disable() {
	set +x
	X_STACK+=("${FUNCNAME[*]}") # push
}

function xtrace_restore() {
	# unset'ing foo[-1] under older Bash (4.2 -> Centos7) won't work, hence the dance
	unset -v "X_STACK[${#X_STACK[@]} - 1 < 0 ? 0 : ${#X_STACK[@]} - 1]" # pop
	if ((${#X_STACK[@]} == 0)); then
		set -x
	fi
}

function xtrace_disable_per_cmd() { eval "$* ${BASH_XTRACEFD}> /dev/null"; }

function xtrace_fd() {
	if [[ -n ${BASH_XTRACEFD:-} && -e /proc/self/fd/$BASH_XTRACEFD ]]; then
		# Close it first to make sure it's sane
		exec {BASH_XTRACEFD}>&-
	fi
	exec {BASH_XTRACEFD}>&2

	xtrace_restore
}

set -e
shopt -s nullglob
shopt -s extglob
shopt -s inherit_errexit

if [ -z "${output_dir:-}" ]; then
	mkdir -p "$rootdir/../output"
	export output_dir="$rootdir/../output"
fi

if [[ -e $rootdir/test/common/build_config.sh ]]; then
	source "$rootdir/test/common/build_config.sh"
elif [[ -e $rootdir/mk/config.mk ]]; then
	build_config=$(< "$rootdir/mk/config.mk")
	source <(echo "${build_config//\?=/=}")
else
	source "$rootdir/CONFIG"
fi

# Source scripts after the config so that the definitions are available.
source "$rootdir/test/common/applications.sh"
source "$rootdir/scripts/common.sh"
source "$rootdir/scripts/perf/pm/common"

: ${RUN_NIGHTLY:=0}
export RUN_NIGHTLY

# Set defaults for missing test config options
: ${SPDK_AUTOTEST_DEBUG_APPS:=0}
export SPDK_AUTOTEST_DEBUG_APPS
: ${SPDK_RUN_VALGRIND=0}
export SPDK_RUN_VALGRIND
: ${SPDK_RUN_FUNCTIONAL_TEST=0}
export SPDK_RUN_FUNCTIONAL_TEST
: ${SPDK_TEST_UNITTEST=0}
export SPDK_TEST_UNITTEST
: ${SPDK_TEST_AUTOBUILD=""}
export SPDK_TEST_AUTOBUILD
: ${SPDK_TEST_RELEASE_BUILD=0}
export SPDK_TEST_RELEASE_BUILD
: ${SPDK_TEST_ISAL=0}
export SPDK_TEST_ISAL
: ${SPDK_TEST_ISCSI=0}
export SPDK_TEST_ISCSI
: ${SPDK_TEST_ISCSI_INITIATOR=0}
export SPDK_TEST_ISCSI_INITIATOR
: ${SPDK_TEST_NVME=0}
export SPDK_TEST_NVME
: ${SPDK_TEST_NVME_PMR=0}
export SPDK_TEST_NVME_PMR
: ${SPDK_TEST_NVME_BP=0}
export SPDK_TEST_NVME_BP
: ${SPDK_TEST_NVME_CLI=0}
export SPDK_TEST_NVME_CLI
: ${SPDK_TEST_NVME_CUSE=0}
export SPDK_TEST_NVME_CUSE
: ${SPDK_TEST_NVME_FDP=0}
export SPDK_TEST_NVME_FDP
: ${SPDK_TEST_NVMF=0}
export SPDK_TEST_NVMF
: ${SPDK_TEST_VFIOUSER=0}
export SPDK_TEST_VFIOUSER
: ${SPDK_TEST_VFIOUSER_QEMU=0}
export SPDK_TEST_VFIOUSER_QEMU
: ${SPDK_TEST_FUZZER=0}
export SPDK_TEST_FUZZER
: ${SPDK_TEST_FUZZER_SHORT=0}
export SPDK_TEST_FUZZER_SHORT
: ${SPDK_TEST_NVMF_TRANSPORT="rdma"}
export SPDK_TEST_NVMF_TRANSPORT
: ${SPDK_TEST_RBD=0}
export SPDK_TEST_RBD
: ${SPDK_TEST_VHOST=0}
export SPDK_TEST_VHOST
: ${SPDK_TEST_BLOCKDEV=0}
export SPDK_TEST_BLOCKDEV
: ${SPDK_TEST_RAID=0}
export SPDK_TEST_RAID
: ${SPDK_TEST_IOAT=0}
export SPDK_TEST_IOAT
: ${SPDK_TEST_BLOBFS=0}
export SPDK_TEST_BLOBFS
: ${SPDK_TEST_VHOST_INIT=0}
export SPDK_TEST_VHOST_INIT
: ${SPDK_TEST_LVOL=0}
export SPDK_TEST_LVOL
: ${SPDK_TEST_VBDEV_COMPRESS=0}
export SPDK_TEST_VBDEV_COMPRESS
: ${SPDK_RUN_ASAN=0}
export SPDK_RUN_ASAN
: ${SPDK_RUN_UBSAN=0}
export SPDK_RUN_UBSAN
: ${SPDK_RUN_EXTERNAL_DPDK=""}
export SPDK_RUN_EXTERNAL_DPDK
: ${SPDK_RUN_NON_ROOT=0}
export SPDK_RUN_NON_ROOT
: ${SPDK_TEST_CRYPTO=0}
export SPDK_TEST_CRYPTO
: ${SPDK_TEST_FTL=0}
export SPDK_TEST_FTL
: ${SPDK_TEST_OCF=0}
export SPDK_TEST_OCF
: ${SPDK_TEST_VMD=0}
export SPDK_TEST_VMD
: ${SPDK_TEST_OPAL=0}
export SPDK_TEST_OPAL
: ${SPDK_TEST_NATIVE_DPDK}
export SPDK_TEST_NATIVE_DPDK
: ${SPDK_AUTOTEST_X=true}
export SPDK_AUTOTEST_X
: ${SPDK_TEST_URING=0}
export SPDK_TEST_URING
: ${SPDK_TEST_USDT=0}
export SPDK_TEST_USDT
: ${SPDK_TEST_USE_IGB_UIO:=0}
export SPDK_TEST_USE_IGB_UIO
: ${SPDK_TEST_SCHEDULER:=0}
export SPDK_TEST_SCHEDULER
: ${SPDK_TEST_SCANBUILD:=0}
export SPDK_TEST_SCANBUILD
: ${SPDK_TEST_NVMF_NICS:=}
export SPDK_TEST_NVMF_NICS
: ${SPDK_TEST_SMA=0}
export SPDK_TEST_SMA
: ${SPDK_TEST_DAOS=0}
export SPDK_TEST_DAOS
: ${SPDK_TEST_XNVME:=0}
export SPDK_TEST_XNVME
: ${SPDK_TEST_ACCEL:=0}
export SPDK_TEST_ACCEL
: ${SPDK_TEST_ACCEL_DSA=0}
export SPDK_TEST_ACCEL_DSA
: ${SPDK_TEST_ACCEL_IAA=0}
export SPDK_TEST_ACCEL_IAA
# Comma-separated list of fuzzer targets matching test/fuzz/llvm/$target
: ${SPDK_TEST_FUZZER_TARGET:=}
export SPDK_TEST_FUZZER_TARGET
: ${SPDK_TEST_NVMF_MDNS=0}
export SPDK_TEST_NVMF_MDNS
: ${SPDK_JSONRPC_GO_CLIENT=0}
export SPDK_JSONRPC_GO_CLIENT
: ${SPDK_TEST_SETUP=0}
export SPDK_TEST_SETUP
: ${SPDK_TEST_NVME_INTERRUPT=0}
export SPDK_TEST_NVME_INTERRUPT

# always test with SPDK shared objects.
export SPDK_LIB_DIR="$rootdir/build/lib"
export DPDK_LIB_DIR="${SPDK_RUN_EXTERNAL_DPDK:-$rootdir/dpdk/build}/lib"
export VFIO_LIB_DIR="$rootdir/build/libvfio-user/usr/local/lib"
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:$SPDK_LIB_DIR:$DPDK_LIB_DIR:$VFIO_LIB_DIR

# Tell setup.sh to wait for block devices upon each reset
export PCI_BLOCK_SYNC_ON_RESET=yes

# Export PYTHONPATH with addition of RPC framework. New scripts can be created
# specific use cases for tests.
export PYTHONPATH=$PYTHONPATH:$rootdir/python

# Don't create Python .pyc files. When running with sudo these will be
# created with root ownership and can cause problems when cleaning the repository.
export PYTHONDONTWRITEBYTECODE=1

# Export new_delete_type_mismatch to skip the known bug that exists in librados
# https://tracker.ceph.com/issues/24078
export ASAN_OPTIONS=new_delete_type_mismatch=0:disable_coredump=0:abort_on_error=1:use_sigaltstack=0
export UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1:abort_on_error=1:disable_coredump=0:exitcode=134'

# Export LeakSanitizer option to use suppression file in order to prevent false positives
# and known leaks in external executables or libraries from showing up.
asan_suppression_file="/var/tmp/asan_suppression_file"
rm -rf "$asan_suppression_file" 2> /dev/null || sudo rm -rf "$asan_suppression_file"
cat << EOL >> "$asan_suppression_file"
# ASAN has some bugs around thread_local variables.  We have a destructor in place
# to free the thread contexts, but ASAN complains about the leak before those
# destructors have a chance to run.  So suppress this one specific leak using
# LSAN_OPTIONS.
leak:spdk_fs_alloc_thread_ctx

# Suppress known leaks in fio project
leak:$CONFIG_FIO_SOURCE_DIR/parse.c
leak:$CONFIG_FIO_SOURCE_DIR/iolog.c
leak:$CONFIG_FIO_SOURCE_DIR/init.c
leak:$CONFIG_FIO_SOURCE_DIR/filesetup.c
leak:fio_memalign
leak:spdk_fio_io_u_init
# Suppress leaks in gperftools-libs from fio
leak:libtcmalloc_minimal.so

# Suppress leaks in libiscsi
leak:libiscsi.so

# Suppress leaks in libcrypto
# Below is caused by openssl 3.0.8 leaks
leak:libcrypto.so

# Suppress leaks in accel-config
# Versions with unresolved leaks:
# v3.4.6.4 [Fedora 37]
leak:add_wq
leak:add_group
# v3.5.2 [Fedora 38]
leak:accfg_get_param_str
# v4.0 [Fedora 39]
leak:__scandir64_tail
EOL

# Suppress leaks in libfuse3
echo "leak:libfuse3.so" >> "$asan_suppression_file"

export LSAN_OPTIONS=suppressions="$asan_suppression_file"

export DEFAULT_RPC_ADDR="/var/tmp/spdk.sock"

if [ -z "${DEPENDENCY_DIR:-}" ]; then
	export DEPENDENCY_DIR=/var/spdk/dependencies
else
	export DEPENDENCY_DIR
fi

# Export location of where all the SPDK binaries are
export SPDK_BIN_DIR="$rootdir/build/bin"
export SPDK_EXAMPLE_DIR="$rootdir/build/examples"

# for vhost, vfio-user tests
export QEMU_BIN=${QEMU_BIN:-}
export VFIO_QEMU_BIN=${VFIO_QEMU_BIN:-}

export AR_TOOL=$rootdir/scripts/ar-xnvme-fixer

# For testing nvmes which are attached to some sort of a fanout switch in the CI pool
export UNBIND_ENTIRE_IOMMU_GROUP=${UNBIND_ENTIRE_IOMMU_GROUP:-no}

_LCOV_MAIN=0
_LCOV_LLVM=1
_LCOV=$LCOV_MAIN
[[ $CC == *clang* || $SPDK_TEST_FUZZER -eq 1 ]] && _LCOV=$_LCOV_LLVM

_lcov_opt[_LCOV_LLVM]="--gcov-tool $rootdir/test/fuzz/llvm/llvm-gcov.sh"
_lcov_opt[_LCOV_MAIN]=""

lcov_opt=${_lcov_opt[_LCOV]}

# pass our valgrind desire on to unittest.sh
if [ $SPDK_RUN_VALGRIND -eq 0 ]; then
	export valgrind=''
else
	# unset all DEBUGINFOD_* vars that may affect our valgrind instance
	unset -v "${!DEBUGINFOD_@}"
fi

if [ "$(uname -s)" = "Linux" ]; then
	HUGEMEM=${HUGEMEM:-4096}
	export CLEAR_HUGE=yes

	MAKE="make"
	MAKEFLAGS=${MAKEFLAGS:--j$(nproc)}
elif [ "$(uname -s)" = "FreeBSD" ]; then
	MAKE="gmake"
	MAKEFLAGS=${MAKEFLAGS:--j$(sysctl -a | grep -E -i 'hw.ncpu' | awk '{print $2}')}
	# FreeBSD runs a much more limited set of tests, so keep the default 2GB.
	HUGEMEM=${HUGEMEM:-2048}
elif [ "$(uname -s)" = "Windows" ]; then
	MAKE="make"
	MAKEFLAGS=${MAKEFLAGS:--j$(nproc)}
	# Keep the default 2GB for Windows.
	HUGEMEM=${HUGEMEM:-2048}
else
	echo "Unknown OS \"$(uname -s)\""
	exit 1
fi

export HUGEMEM=$HUGEMEM

NO_HUGE=()
TEST_MODE=
for i in "$@"; do
	case "$i" in
		--iso)
			TEST_MODE=iso
			;;
		--transport=*)
			TEST_TRANSPORT="${i#*=}"
			;;
		--no-hugepages)
			NO_HUGE=(--no-huge -s 1024)
			;;
		--interrupt-mode)
			TEST_INTERRUPT_MODE=1
			;;
	esac
done

# start rpc.py coprocess if it's not started yet
if [[ -z ${RPC_PIPE_PID:-} ]] || ! kill -0 "$RPC_PIPE_PID" &> /dev/null; then
	# Include list to all known plugins we use in the tests
	PYTHONPATH+=":$rootdir/test/rpc_plugins"
	coproc RPC_PIPE { PYTHONPATH="$PYTHONPATH" "$rootdir/scripts/rpc.py" --server; }
	exec {RPC_PIPE_OUTPUT}<&${RPC_PIPE[0]} {RPC_PIPE_INPUT}>&${RPC_PIPE[1]}
	# all descriptors will automatically close together with this bash
	# process, this will make rpc.py stop reading and exit gracefully
fi

function set_test_storage() {
	[[ -v testdir ]] || return 0

	local requested_size=$1 # bytes
	local mount target_dir

	local -A mounts fss sizes avails uses
	local source fs size avail mount use

	local storage_fallback storage_candidates

	storage_fallback=$(mktemp -udt spdk.XXXXXX)
	storage_candidates=(
		"$testdir"
		"$storage_fallback/tests/${testdir##*/}"
		"$storage_fallback"
	)

	if [[ -n ${ADD_TEST_STORAGE:-} ]]; then
		# List of dirs|mounts separated by whitespaces
		storage_candidates+=($ADD_TEST_STORAGE)
	fi

	if [[ -n ${DEDICATED_TEST_STORAGE:-} ]]; then
		# Single, dedicated dir|mount
		storage_candidates=("$DEDICATED_TEST_STORAGE")
	fi

	mkdir -p "${storage_candidates[@]}"

	# add some headroom - 64M
	requested_size=$((requested_size + (64 << 20)))

	while read -r source fs size use avail _ mount; do
		mounts["$mount"]=$source fss["$mount"]=$fs
		avails["$mount"]=$((avail * 1024)) sizes["$mount"]=$((size * 1024))
		uses["$mount"]=$((use * 1024))
	done < <(df -T | grep -v Filesystem)

	printf '* Looking for test storage...\n' >&2

	local target_space new_size
	for target_dir in "${storage_candidates[@]}"; do
		# FreeBSD's df is lacking the --output arg
		# mount=$(df --output=target "$target_dir" | grep -v "Mounted on")
		mount=$(df "$target_dir" | awk '$1 !~ /Filesystem/{print $6}')

		target_space=${avails["$mount"]}
		if ((target_space == 0 || target_space < requested_size)); then
			continue
		fi
		if ((target_space >= requested_size)); then
			# For in-memory fs, and / make sure our requested size won't fill most of the space.
			if [[ ${fss["$mount"]} == tmpfs ]] || [[ ${fss["$mount"]} == ramfs ]] || [[ $mount == / ]]; then
				new_size=$((uses["$mount"] + requested_size))
				if ((new_size * 100 / sizes["$mount"] > 95)); then
					continue
				fi
			fi
		fi
		export SPDK_TEST_STORAGE=$target_dir
		printf '* Found test storage at %s\n' "$SPDK_TEST_STORAGE" >&2
		return 0
	done
	printf '* Test storage is not available\n'
	return 1
}

function get_config_params() {
	xtrace_disable
	config_params='--enable-debug --enable-werror'

	# for options with dependencies but no test flag, set them here
	if [ -f /usr/include/infiniband/verbs.h ]; then
		config_params+=' --with-rdma'
	fi

	if [ $SPDK_TEST_USDT -eq 1 ]; then
		config_params+=" --with-usdt"
	fi

	case "$(uname -s)" in
		FreeBSD) [[ $(sysctl -n hw.model) == Intel* ]] ;;
		Linux) [[ $(< /proc/cpuinfo) == *GenuineIntel* ]] ;;
		*) false ;;
	esac && config_params+=" --with-idxd" || config_params+=" --without-idxd"

	if [[ -d $CONFIG_FIO_SOURCE_DIR ]]; then
		config_params+=" --with-fio=$CONFIG_FIO_SOURCE_DIR"
	fi

	if [ -d ${DEPENDENCY_DIR}/vtune_codes ]; then
		config_params+=' --with-vtune='${DEPENDENCY_DIR}'/vtune_codes'
	fi

	if [ -d /usr/include/iscsi ]; then
		[[ $(< /usr/include/iscsi/iscsi.h) =~ "define LIBISCSI_API_VERSION ("([0-9]+)")" ]] \
			&& libiscsi_version=${BASH_REMATCH[1]}
		if ((libiscsi_version >= 20150621)); then
			config_params+=' --with-iscsi-initiator'
		fi
	fi

	if [[ $SPDK_TEST_UNITTEST -eq 0 &&
		$SPDK_TEST_SCANBUILD -eq 0 && -z ${SPDK_TEST_AUTOBUILD:-} ]]; then
		config_params+=' --disable-unit-tests'
	fi

	if [ -f /usr/include/libpmem.h ] && [ $SPDK_TEST_VBDEV_COMPRESS -eq 1 ]; then
		if ge "$(nasm --version | awk '{print $3}')" 2.14 && [[ $SPDK_TEST_ISAL -eq 1 ]]; then
			config_params+=' --with-vbdev-compress --with-dpdk-compressdev'
		fi
	fi

	if [ -d /usr/include/rbd ] && [ -d /usr/include/rados ] && [ $SPDK_TEST_RBD -eq 1 ]; then
		config_params+=' --with-rbd'
	fi

	# for options with no required dependencies, just test flags, set them here
	if [ $SPDK_TEST_CRYPTO -eq 1 ]; then
		config_params+=' --with-crypto'
	fi

	if [ $SPDK_TEST_OCF -eq 1 ]; then
		config_params+=" --with-ocf"
	fi

	if [ $SPDK_RUN_UBSAN -eq 1 ]; then
		config_params+=' --enable-ubsan'
	fi

	if [ $SPDK_RUN_ASAN -eq 1 ]; then
		config_params+=' --enable-asan'
	fi

	config_params+=' --enable-coverage'

	if [ $SPDK_TEST_BLOBFS -eq 1 ]; then
		if [[ -d /usr/include/fuse3 ]] || [[ -d /usr/local/include/fuse3 ]]; then
			config_params+=' --with-fuse'
		fi
	fi

	if [[ -f /usr/include/liburing/io_uring.h && -f /usr/include/linux/ublk_cmd.h ]]; then
		config_params+=' --with-ublk'
	fi

	if [ $SPDK_TEST_RAID -eq 1 ]; then
		config_params+=' --with-raid5f'
	fi

	if [ $SPDK_TEST_VFIOUSER -eq 1 ] || [ $SPDK_TEST_VFIOUSER_QEMU -eq 1 ] || [ $SPDK_TEST_SMA -eq 1 ]; then
		config_params+=' --with-vfio-user'
	fi

	# Check whether liburing library header exists
	if [ -f /usr/include/liburing/io_uring.h ] && [ $SPDK_TEST_URING -eq 1 ]; then
		config_params+=' --with-uring'
	fi

	if [ -n "${SPDK_RUN_EXTERNAL_DPDK:-}" ]; then
		config_params+=" --with-dpdk=$SPDK_RUN_EXTERNAL_DPDK"
	fi

	if [[ $SPDK_TEST_SMA -eq 1 ]]; then
		config_params+=' --with-sma'
		config_params+=' --with-crypto'
	fi

	if [ -f /usr/include/daos.h ] && [ $SPDK_TEST_DAOS -eq 1 ]; then
		config_params+=' --with-daos'
	fi

	# Make the xnvme module available for the tests
	if [[ $SPDK_TEST_XNVME -eq 1 ]]; then
		config_params+=' --with-xnvme'
	fi

	if [[ $SPDK_TEST_FUZZER -eq 1 ]]; then
		config_params+=" $(get_fuzzer_target_config)"
	fi

	if [[ $SPDK_TEST_NVMF_MDNS -eq 1 ]]; then
		config_params+=' --with-avahi'
	fi

	if [[ $SPDK_JSONRPC_GO_CLIENT -eq 1 ]]; then
		config_params+=' --with-golang'
	fi

	echo "$config_params"
	xtrace_restore
}

function get_fuzzer_target_config() {
	local -A fuzzer_targets_to_config=()
	local config target

	fuzzer_targets_to_config["vfio"]="--with-vfio-user"
	for target in $(get_fuzzer_targets); do
		[[ -n ${fuzzer_targets_to_config["$target"]:-} ]] || continue
		config+=("${fuzzer_targets_to_config["$target"]}")
	done

	if ((${#config[@]} > 0)); then
		echo "${config[*]}"
	fi
}

function get_fuzzer_targets() {
	local fuzzers=()

	if [[ -n ${SPDK_TEST_FUZZER_TARGET:-} ]]; then
		IFS="," read -ra fuzzers <<< "$SPDK_TEST_FUZZER_TARGET"
	else
		fuzzers=("$rootdir/test/fuzz/llvm/"*)
		fuzzers=("${fuzzers[@]##*/}")
	fi

	echo "${fuzzers[*]}"
}

function rpc_cmd() {
	xtrace_disable
	local rsp rc=1
	local stdin cmd cmds_number=0 status_number=0 status

	if (($#)); then
		cmds_number=1
		echo "$@" >&$RPC_PIPE_INPUT
	elif [[ ! -t 0 ]]; then
		mapfile -t stdin <&0
		cmds_number=${#stdin[@]}
		printf '%s\n' "${stdin[@]}" >&$RPC_PIPE_INPUT
	else
		return 0
	fi

	while read -t "${RPC_PIPE_TIMEOUT:-15}" -ru $RPC_PIPE_OUTPUT rsp; do
		if [[ $rsp == "**STATUS="* ]]; then
			status[${rsp#*=}]=$rsp
			if ((++status_number == cmds_number)); then
				break
			fi
			continue
		fi
		echo "$rsp"
	done

	rc=${!status[*]}
	xtrace_restore
	[[ $rc == 0 ]]
}

function rpc_cmd_simple_data_json() {

	local elems="$1[@]" elem
	local -gA jq_out=()
	local jq val

	local lvs=(
		"uuid"
		"name"
		"base_bdev"
		"total_data_clusters"
		"free_clusters"
		"block_size"
		"cluster_size"
	)

	local bdev=(
		"name"
		"aliases[0]"
		"block_size"
		"num_blocks"
		"uuid"
		"product_name"
		"supported_io_types.read"
		"supported_io_types.write"
		"driver_specific.lvol.clone"
		"driver_specific.lvol.base_snapshot"
		"driver_specific.lvol.esnap_clone"
		"driver_specific.lvol.external_snapshot_name"
	)

	[[ -v $elems ]] || return 1

	for elem in "${!elems}"; do
		jq="${jq:+$jq,\"\\n\",}\"$elem\",\" \",.[0].$elem"
	done
	jq+=',"\n"'

	shift
	while read -r elem val; do
		jq_out["$elem"]=$val
	done < <(rpc_cmd "$@" | jq -jr "$jq")
	((${#jq_out[@]} > 0)) || return 1
}

function valid_exec_arg() {
	local arg=$1
	# First argument must be the executable so do some basic sanity checks first. For bash, this
	# covers two basic cases where es == 126 || es == 127 so catch them early on and fail hard
	# if needed.
	case "$(type -t "$arg")" in
		builtin | function) ;;
		file) arg=$(type -P "$arg") && [[ -x $arg ]] ;;
		*) return 1 ;;
	esac
}

function NOT() {
	local es=0

	valid_exec_arg "$@" || return 1
	"$@" || es=$?

	# Logic looks like so:
	#  - return false if command exit successfully
	#  - return false if command exit after receiving a core signal (FIXME: or any signal?)
	#  - return true if command exit with an error

	# This naively assumes that the process doesn't exit with > 128 on its own.
	if ((es > 128)); then
		es=$((es & ~128))
		case "$es" in
			3) ;&       # SIGQUIT
			4) ;&       # SIGILL
			6) ;&       # SIGABRT
			8) ;&       # SIGFPE
			9) ;&       # SIGKILL
			11) es=0 ;; # SIGSEGV
			*) es=1 ;;
		esac
	elif [[ -n ${EXIT_STATUS:-} ]] && ((es != EXIT_STATUS)); then
		es=0
	fi

	# invert error code of any command and also trigger ERR on 0 (unlike bash ! prefix)
	((!es == 0))
}

function timing() {
	direction="$1"
	testname="$2"

	now=$(date +%s)

	if [ "$direction" = "enter" ]; then
		export timing_stack="${timing_stack:-};${now}"
		export test_stack="${test_stack:-};${testname}"
	else
		touch "$output_dir/timing.txt"
		child_time=$(grep "^${test_stack:1};" $output_dir/timing.txt | awk '{s+=$2} END {print s}')

		start_time=$(echo "$timing_stack" | sed -e 's@^.*;@@')
		timing_stack=$(echo "$timing_stack" | sed -e 's@;[^;]*$@@')

		elapsed=$((now - start_time - child_time))
		echo "${test_stack:1} $elapsed" >> $output_dir/timing.txt

		test_stack=$(echo "$test_stack" | sed -e 's@;[^;]*$@@')
	fi
}

function timing_cmd() (
	# The use-case here is this: ts=$(timing_cmd echo bar). Since stdout is always redirected
	# to a pipe handling the $(), lookup the stdin's device and determine if it's sane to send
	# cmd's output to it. If not, just null it.
	local cmd_es=$?

	[[ -t 0 ]] && exec {cmd_out}>&0 || exec {cmd_out}> /dev/null

	local time=0 TIMEFORMAT=%2R # seconds

	# We redirect cmd's std{out,err} to a separate fd dup'ed to stdin's device (or /dev/null) to
	# catch only output from the time builtin - output from the actual cmd would be still visible,
	# but $() will return just the time's data, hence making it possible to just do:
	#  time_of_super_verbose_cmd=$(timing_cmd super_verbose_cmd)
	time=$({ time "$@" >&"$cmd_out" 2>&1; } 2>&1) || cmd_es=$?
	echo "$time"

	return "$cmd_es"
)

function timing_enter() {
	xtrace_disable
	timing "enter" "$1"
	xtrace_restore
}

function timing_exit() {
	xtrace_disable
	timing "exit" "$1"
	xtrace_restore
}

function timing_finish() {
	[[ -e $output_dir/timing.txt ]] || return 0

	flamegraph='/usr/local/FlameGraph/flamegraph.pl'
	[[ -x "$flamegraph" ]] || return 1

	"$flamegraph" \
		--title 'Build Timing' \
		--nametype 'Step:' \
		--countname seconds \
		"$output_dir/timing.txt" \
		> "$output_dir/timing.svg"
}

function create_test_list() {
	xtrace_disable
	# First search all scripts in main SPDK directory.
	completion=$(grep -shI -d skip --include="*.sh" -e "run_test " $rootdir/*)
	# Follow up with search in test directory recursively.
	completion+=$'\n'$(grep -rshI --include="*.sh" --exclude="*autotest_common.sh" -e "run_test " $rootdir/test)
	printf "%s" "$completion" | grep -v "#" \
		| sed 's/^.*run_test/run_test/' | awk '{print $2}' \
		| sed 's/\"//g' | sort > $output_dir/all_tests.txt || true
	xtrace_restore
}

function gdb_attach() {
	gdb -q --batch \
		-ex 'handle SIGHUP nostop pass' \
		-ex 'handle SIGQUIT nostop pass' \
		-ex 'handle SIGPIPE nostop pass' \
		-ex 'handle SIGALRM nostop pass' \
		-ex 'handle SIGTERM nostop pass' \
		-ex 'handle SIGUSR1 nostop pass' \
		-ex 'handle SIGUSR2 nostop pass' \
		-ex 'handle SIGCHLD nostop pass' \
		-ex 'set print thread-events off' \
		-ex 'cont' \
		-ex 'thread apply all bt' \
		-ex 'quit' \
		--tty=/dev/stdout \
		-p $1
}

function process_core() {
	# Note that this always was racy as we can't really sync with the kernel
	# to see if there's any core queued up for writing. Assume that if we are
	# being called via trap, as in, when some error has occurred, wait up to
	# 10s for any potential cores. If we are called just for cleanup at the
	# very end, don't wait since all the tests ended successfully, hence
	# having any critical cores lying around is unlikely.
	((autotest_es != 0)) && sleep 10

	local coredumps core

	"$rootdir/scripts/core-collector.sh" "$output_dir/coredumps"

	coredumps=("$output_dir/coredumps/"*.bt.txt)

	((${#coredumps[@]} > 0)) || return 0
	chmod -R a+r "$output_dir/coredumps"

	for core in "${coredumps[@]}"; do
		cat <<- BT
			##### CORE BT ${core##*/} #####

			$(< "$core")

			--
		BT
	done
	return 1
}

function process_shm() {
	type=$1
	id=$2
	if [ "$type" = "--pid" ]; then
		id="pid${id}"
	fi

	shm_files=$(find /dev/shm -name "*.${id}" -printf "%f\n")

	if [[ -z ${shm_files:-} ]]; then
		echo "SHM File for specified PID or shared memory id: ${id} not found!"
		return 1
	fi
	for n in $shm_files; do
		tar -C /dev/shm/ -cvzf $output_dir/${n}_shm.tar.gz ${n}
	done
	return 0
}

# Parameters:
# $1 - process pid
# $2 - rpc address (optional)
# $3 - max retries (optional)
function waitforlisten() {
	if [ -z "${1:-}" ]; then
		exit 1
	fi

	local rpc_addr="${2:-$DEFAULT_RPC_ADDR}"
	local max_retries=${3:-100}

	echo "Waiting for process to start up and listen on UNIX domain socket $rpc_addr..."
	# turn off trace for this loop
	xtrace_disable
	local ret=0
	local i
	for ((i = max_retries; i != 0; i--)); do
		# if the process is no longer running, then exit the script
		#  since it means the application crashed
		if ! kill -s 0 $1; then
			echo "ERROR: process (pid: $1) is no longer running"
			ret=1
			break
		fi

		if $rootdir/scripts/rpc.py -t 1 -s "$rpc_addr" rpc_get_methods &> /dev/null; then
			break
		fi

		sleep 0.5
	done

	xtrace_restore
	if ((i == 0)); then
		echo "ERROR: timeout while waiting for process (pid: $1) to start listening on '$rpc_addr'"
		ret=1
	fi
	return $ret
}

function waitfornbd() {
	local nbd_name=$1
	local i

	for ((i = 1; i <= 20; i++)); do
		if grep -q -w $nbd_name /proc/partitions; then
			break
		else
			sleep 0.1
		fi
	done

	# The nbd device is now recognized as a block device, but there can be
	#  a small delay before we can start I/O to that block device.  So loop
	#  here trying to read the first block of the nbd block device to a temp
	#  file.  Note that dd returns success when reading an empty file, so we
	#  need to check the size of the output file instead.
	for ((i = 1; i <= 20; i++)); do
		dd if=/dev/$nbd_name of="$SPDK_TEST_STORAGE/nbdtest" bs=4096 count=1 iflag=direct
		size=$(stat -c %s "$SPDK_TEST_STORAGE/nbdtest")
		rm -f "$SPDK_TEST_STORAGE/nbdtest"
		if [ "$size" != "0" ]; then
			return 0
		else
			sleep 0.1
		fi
	done

	return 1
}

function waitforbdev() {
	local bdev_name=$1
	local bdev_timeout=$2
	local i
	[[ -z ${bdev_timeout:-} ]] && bdev_timeout=2000 # ms

	$rpc_py bdev_wait_for_examine

	if $rpc_py bdev_get_bdevs -b $bdev_name -t $bdev_timeout; then
		return 0
	fi

	return 1
}

function waitforcondition() {
	local cond=$1
	local max=${2:-10}
	while ((max--)); do
		if eval $cond; then
			return 0
		fi
		sleep 1
	done
	return 1
}

function make_filesystem() {
	local fstype=$1
	local dev_name=$2
	local i=0
	local force

	if [ $fstype = ext4 ]; then
		force=-F
	else
		force=-f
	fi

	while ! mkfs.${fstype} $force ${dev_name}; do
		if [ $i -ge 15 ]; then
			return 1
		fi
		i=$((i + 1))
		sleep 1
	done

	return 0
}

function killprocess() {
	# $1 = process pid
	if [ -z "${1:-}" ]; then
		return 1
	fi

	if kill -0 $1; then
		if [ $(uname) = Linux ]; then
			process_name=$(ps --no-headers -o comm= $1)
		else
			process_name=$(ps -c -o command $1 | tail -1)
		fi
		if [ "$process_name" = "sudo" ]; then
			# kill the child process, which is the actual app
			# (assume $1 has just one child)
			local child
			child="$(pgrep -P $1)"
			echo "killing process with pid $child"
			kill $child
		else
			echo "killing process with pid $1"
			kill $1
		fi

		# wait for the process regardless if its the dummy sudo one
		# or the actual app - it should terminate anyway
		wait $1
	else
		# the process is not there anymore
		echo "Process with pid $1 is not found"
	fi
}

function iscsicleanup() {
	echo "Cleaning up iSCSI connection"
	iscsiadm -m node --logout || true
	iscsiadm -m node -o delete || true
	rm -rf /var/lib/iscsi/nodes/*
}

function stop_iscsi_service() {
	if cat /etc/*-release | grep Ubuntu; then
		service open-iscsi stop
	else
		service iscsid stop
	fi
}

function start_iscsi_service() {
	if cat /etc/*-release | grep Ubuntu; then
		service open-iscsi start
	else
		service iscsid start
	fi
}

function rbd_setup() {
	# $1 = monitor ip address
	# $2 = name of the namespace
	if [ -z "${1:-}" ]; then
		echo "No monitor IP address provided for ceph"
		exit 1
	fi
	if [ -n "${2:-}" ]; then
		if ip netns list | grep "$2"; then
			NS_CMD="ip netns exec $2"
		else
			echo "No namespace $2 exists"
			exit 1
		fi
	fi

	if hash ceph; then
		export PG_NUM=128
		export RBD_POOL=rbd
		export RBD_NAME=foo
		$NS_CMD $rootdir/scripts/ceph/stop.sh || true
		$NS_CMD $rootdir/scripts/ceph/start.sh $1

		$NS_CMD ceph osd pool create $RBD_POOL $PG_NUM || true
		$NS_CMD rbd create $RBD_NAME --size 1000
	fi
}

function rbd_cleanup() {
	if hash ceph; then
		$rootdir/scripts/ceph/stop.sh || true
		rm -f /var/tmp/ceph_raw.img
	fi
}

function daos_setup() {
	# $1 = pool name
	# $2 = cont name
	if [ -z "${1:-}" ]; then
		echo "No pool name provided"
		exit 1
	fi
	if [ -z "${2:-}" ]; then
		echo "No cont name provided"
		exit 1
	fi

	dmg pool create --size=10G $1 || true
	daos container create --type=POSIX --label=$2 $1 || true
}

function daos_cleanup() {
	local pool=${1:-testpool}
	local cont=${2:-testcont}

	daos container destroy -f $pool $cont || true
	sudo dmg pool destroy -f $pool || true
}

function _start_stub() {
	# Disable ASLR for multi-process testing.  SPDK does support using DPDK multi-process,
	# but ASLR can still be unreliable in some cases.
	# We will re-enable it again after multi-process testing is complete in kill_stub().
	# Save current setting so it can be restored upon calling kill_stub().
	_randomize_va_space=$(< /proc/sys/kernel/randomize_va_space)
	echo 0 > /proc/sys/kernel/randomize_va_space
	$rootdir/test/app/stub/stub $1 &
	stubpid=$!
	echo Waiting for stub to ready for secondary processes...
	while ! [ -e /var/run/spdk_stub0 ]; do
		# If stub dies while we wait, bail
		[[ -e /proc/$stubpid ]] || return 1
		sleep 1s
	done
	echo done.
}

function start_stub() {
	if ! _start_stub "$@"; then
		echo "stub failed" >&2
		return 1
	fi
}

function kill_stub() {
	if [[ -e /proc/$stubpid ]]; then
		kill $1 $stubpid
		wait $stubpid
	fi 2> /dev/null || :
	rm -f /var/run/spdk_stub0
	# Re-enable ASLR now that we are done with multi-process testing
	# Note: "1" enables ASLR w/o randomizing data segments, "2" adds data segment
	#  randomizing and is the default on all recent Linux kernels
	echo "${_randomize_va_space:-2}" > /proc/sys/kernel/randomize_va_space
}

function run_test() {
	if [ $# -le 1 ]; then
		echo "Not enough parameters"
		echo "usage: run_test test_name test_script [script_params]"
		exit 1
	fi

	xtrace_disable
	local test_name="$1" pid
	shift

	if [ -n "${test_domain:-}" ]; then
		export test_domain="${test_domain}.${test_name}"
	else
		export test_domain="$test_name"
	fi

	# Signal our daemons to update the test tag
	update_tag_monitor_resources "$test_domain"

	timing_enter $test_name
	echo "************************************"
	echo "START TEST $test_name"
	echo "************************************"
	xtrace_restore
	time "$@"
	xtrace_disable
	echo "************************************"
	echo "END TEST $test_name"
	echo "************************************"
	timing_exit $test_name

	export test_domain=${test_domain%"$test_name"}
	if [ -n "$test_domain" ]; then
		export test_domain=${test_domain%?}
	fi

	if [ -z "${test_domain:-}" ]; then
		echo "top_level $test_name" >> $output_dir/test_completions.txt
	else
		echo "$test_domain $test_name" >> $output_dir/test_completions.txt
	fi
	xtrace_restore
}

function skip_run_test_with_warning() {
	echo "WARNING: $1"
	echo "Test run may fail if run with autorun.sh"
	echo "Please check your $rootdir/test/common/skipped_tests.txt"
}

function print_backtrace() {
	# if errexit is not enabled, don't print a backtrace
	[[ "$-" =~ e ]] || return 0

	local args=("${BASH_ARGV[@]}")

	xtrace_disable
	# Reset IFS in case we were called from an environment where it was modified
	IFS=" "$'\t'$'\n'
	echo "========== Backtrace start: =========="
	echo ""
	for ((i = 1; i < ${#FUNCNAME[@]}; i++)); do
		local func="${FUNCNAME[$i]}"
		local line_nr="${BASH_LINENO[$((i - 1))]}"
		local src="${BASH_SOURCE[$i]}"
		local bt="" cmdline=()

		if [[ -f $src ]]; then
			bt=$(nl -w 4 -ba -nln $src | grep -B 5 -A 5 "^${line_nr}[^0-9]" \
				| sed "s/^/   /g" | sed "s/^   $line_nr /=> $line_nr /g")
		fi

		# If extdebug set the BASH_ARGC[i], try to fetch all the args
		if ((BASH_ARGC[i] > 0)); then
			# Use argc as index to reverse the stack
			local argc=${BASH_ARGC[i]} arg
			for arg in "${args[@]::BASH_ARGC[i]}"; do
				cmdline[argc--]="[\"$arg\"]"
			done
			args=("${args[@]:BASH_ARGC[i]}")
		fi

		echo "in $src:$line_nr -> $func($(
			IFS=","
			printf '%s\n' "${cmdline[*]:-[]}"
		))"
		echo "     ..."
		echo "${bt:-backtrace unavailable}"
		echo "     ..."
	done
	echo ""
	echo "========== Backtrace end =========="
	xtrace_restore
	return 0
}

function waitforserial() {
	local i=0
	local nvme_device_counter=1 nvme_devices=0
	if [[ -n "${2:-}" ]]; then
		nvme_device_counter=$2
	fi

	# Wait initially for min 2s to make sure all devices are ready for use.
	sleep 2
	while ((i++ <= 15)); do
		nvme_devices=$(lsblk -l -o NAME,SERIAL | grep -c "$1")
		((nvme_devices == nvme_device_counter)) && return 0
		if ((nvme_devices > nvme_device_counter)); then
			echo "$nvme_device_counter device(s) expected, found $nvme_devices" >&2
		fi
		echo "Waiting for devices"
		sleep 1
	done
	return 1
}

function waitforserial_disconnect() {
	local i=0
	while lsblk -o NAME,SERIAL | grep -q -w $1; do
		[ $i -lt 15 ] || break
		i=$((i + 1))
		echo "Waiting for disconnect devices"
		sleep 1
	done

	if lsblk -l -o NAME,SERIAL | grep -q -w $1; then
		return 1
	fi

	return 0
}

function waitforblk() {
	local i=0
	while ! lsblk -l -o NAME | grep -q -w $1; do
		[ $i -lt 15 ] || break
		i=$((i + 1))
		sleep 1
	done

	if ! lsblk -l -o NAME | grep -q -w $1; then
		return 1
	fi

	return 0
}

function waitforblk_disconnect() {
	local i=0
	while lsblk -l -o NAME | grep -q -w $1; do
		[ $i -lt 15 ] || break
		i=$((i + 1))
		sleep 1
	done

	if lsblk -l -o NAME | grep -q -w $1; then
		return 1
	fi

	return 0
}

function waitforfile() {
	local i=0
	while [ ! -e $1 ]; do
		[ $i -lt 200 ] || break
		i=$((i + 1))
		sleep 0.1
	done

	if [ ! -e $1 ]; then
		return 1
	fi

	return 0
}

function fio_config_gen() {
	local config_file=$1
	local workload=$2
	local bdev_type=$3
	local env_context=$4
	local fio_dir=$CONFIG_FIO_SOURCE_DIR

	if [ -e "$config_file" ]; then
		echo "Configuration File Already Exists!: $config_file"
		return 1
	fi

	if [ -z "${workload:-}" ]; then
		workload=randrw
	fi

	if [ -n "$env_context" ]; then
		env_context="env_context=$env_context"
	fi

	touch $1

	cat > $1 << EOL
[global]
thread=1
$env_context
group_reporting=1
direct=1
norandommap=1
percentile_list=50:99:99.9:99.99:99.999
time_based=1
ramp_time=0
EOL

	if [ "$workload" == "verify" ]; then
		cat <<- EOL >> $config_file
			verify=sha1
			verify_backlog=1024
			rw=randwrite
		EOL

		# To avoid potential data race issue due to the AIO device
		# flush mechanism, add the flag to serialize the writes.
		# This is to fix the intermittent IO failure issue of #935
		if [ "$bdev_type" == "AIO" ]; then
			if [[ $($fio_dir/fio --version) == *"fio-3"* ]]; then
				echo "serialize_overlap=1" >> $config_file
			fi
		fi
	elif [ "$workload" == "trim" ]; then
		echo "rw=trimwrite" >> $config_file
	else
		echo "rw=$workload" >> $config_file
	fi
}

function fio_plugin() {
	# Setup fio binary cmd line
	local fio_dir=$CONFIG_FIO_SOURCE_DIR
	# gcc and clang uses different sanitizer libraries
	local sanitizers=(libasan libclang_rt.asan)
	local plugin=$1
	shift

	local asan_lib=
	for sanitizer in "${sanitizers[@]}"; do
		asan_lib=$(ldd $plugin | grep $sanitizer | awk '{print $3}')
		if [[ -n "${asan_lib:-}" ]]; then
			break
		fi
	done

	# Preload the sanitizer library to fio if fio_plugin was compiled with it
	LD_PRELOAD="$asan_lib $plugin" "$fio_dir"/fio "$@"
}

function fio_bdev() {
	fio_plugin "$rootdir/build/fio/spdk_bdev" "$@"
}

function fio_nvme() {
	fio_plugin "$rootdir/build/fio/spdk_nvme" "$@"
}

function get_lvs_free_mb() {
	local lvs_uuid=$1
	local lvs_info
	local fc
	local cs
	lvs_info=$($rpc_py bdev_lvol_get_lvstores)
	fc=$(jq ".[] | select(.uuid==\"$lvs_uuid\") .free_clusters" <<< "$lvs_info")
	cs=$(jq ".[] | select(.uuid==\"$lvs_uuid\") .cluster_size" <<< "$lvs_info")

	# Change to MB's
	free_mb=$((fc * cs / 1024 / 1024))
	echo "$free_mb"
}

function get_bdev_size() {
	local bdev_name=$1
	local bdev_info
	local bs
	local nb
	bdev_info=$($rpc_py bdev_get_bdevs -b $bdev_name)
	bs=$(jq ".[] .block_size" <<< "$bdev_info")
	nb=$(jq ".[] .num_blocks" <<< "$bdev_info")

	# Change to MB's
	bdev_size=$((bs * nb / 1024 / 1024))
	echo "$bdev_size"
}

function autotest_cleanup() {
	local autotest_es=$?
	xtrace_disable

	# Slurp at_app_exit() so we can kill all lingering vhost and qemu processes
	# in one swing. We do this in a subshell as vhost/common.sh is too eager to
	# do some extra work which we don't care about in this context.
	# shellcheck source=/dev/null
	vhost_reap() (source "$rootdir/test/vhost/common.sh" &> /dev/null || return 0 && at_app_exit)

	# catch any stray core files and kill all remaining SPDK processes. Update
	# autotest_es in case autotest reported success but cores and/or processes
	# were left behind regardless.

	process_core || autotest_es=1
	reap_spdk_processes || autotest_es=1
	vhost_reap || autotest_es=1

	$rootdir/scripts/setup.sh reset
	$rootdir/scripts/setup.sh cleanup
	if [ $(uname -s) = "Linux" ]; then
		modprobe -r uio_pci_generic
	fi
	rm -rf "$asan_suppression_file"
	if [[ -n ${old_core_pattern:-} ]]; then
		echo "$old_core_pattern" > /proc/sys/kernel/core_pattern
	fi
	if [[ -e /proc/$udevadm_pid/status ]]; then
		kill "$udevadm_pid" || :
	fi

	local storage_fallback_purge=("${TMPDIR:-/tmp}/spdk."??????)

	if ((${#storage_fallback_purge[@]} > 0)); then
		rm -rf "${storage_fallback_purge[@]}"
	fi

	if ((autotest_es)); then
		if [[ $(uname) == FreeBSD ]]; then
			ps aux
		elif [[ $(uname) == Linux ]]; then
			# Get more detailed view
			grep . /proc/[0-9]*/status
			# Dump some extra info into kernel log
			echo "######## Autotest Cleanup Dump ########" > /dev/kmsg
			# Show cpus backtraces
			echo l > /proc/sysrq-trigger
			# Show mem usage
			echo m > /proc/sysrq-trigger
			# show blocked tasks
			echo w > /proc/sysrq-trigger

		fi > "$output_dir/proc_list.txt" 2>&1 || :
	fi

	stop_monitor_resources

	xtrace_restore
	return $autotest_es
}

function freebsd_update_contigmem_mod() {
	if [ $(uname) = FreeBSD ]; then
		kldunload contigmem.ko || true
		if [ -n "${SPDK_RUN_EXTERNAL_DPDK:-}" ]; then
			cp -f "$SPDK_RUN_EXTERNAL_DPDK/kmod/contigmem.ko" /boot/modules/
			cp -f "$SPDK_RUN_EXTERNAL_DPDK/kmod/contigmem.ko" /boot/kernel/
			cp -f "$SPDK_RUN_EXTERNAL_DPDK/kmod/nic_uio.ko" /boot/modules/
			cp -f "$SPDK_RUN_EXTERNAL_DPDK/kmod/nic_uio.ko" /boot/kernel/
		else
			cp -f "$rootdir/dpdk/build/kmod/contigmem.ko" /boot/modules/
			cp -f "$rootdir/dpdk/build/kmod/contigmem.ko" /boot/kernel/
			cp -f "$rootdir/dpdk/build/kmod/nic_uio.ko" /boot/modules/
			cp -f "$rootdir/dpdk/build/kmod/nic_uio.ko" /boot/kernel/
		fi
	fi
}

function freebsd_set_maxsock_buf() {
	# FreeBSD needs 4MB maxsockbuf size to pass socket unit tests.
	# Otherwise tests fail due to ENOBUFS when trying to do setsockopt(SO_RCVBUF|SO_SNDBUF).
	# See https://github.com/spdk/spdk/issues/2943
	if [[ $(uname) = FreeBSD ]] && (($(sysctl -n kern.ipc.maxsockbuf) < 4194304)); then
		sysctl kern.ipc.maxsockbuf=4194304
	fi
}

function get_nvme_name_from_bdf() {
	get_block_dev_from_nvme "$@"
}

function get_nvme_ctrlr_from_bdf() {
	bdf_sysfs_path=$(readlink -f /sys/class/nvme/nvme* | grep "$1/nvme/nvme")
	if [[ -z "${bdf_sysfs_path:-}" ]]; then
		return
	fi

	printf '%s\n' "$(basename $bdf_sysfs_path)"
}

# Get BDF addresses of all NVMe drives currently attached to
# uio-pci-generic or vfio-pci
function get_nvme_bdfs() {
	local bdfs=()
	bdfs=($("$rootdir/scripts/gen_nvme.sh" | jq -r '.config[].params.traddr'))
	if ((${#bdfs[@]} == 0)); then
		echo "No bdevs found" >&2
		return 1
	fi
	printf '%s\n' "${bdfs[@]}"
}

# Same as function above, but just get the first disks BDF address
function get_first_nvme_bdf() {
	local bdfs=()
	bdfs=($(get_nvme_bdfs))

	echo "${bdfs[0]}"
}

function nvme_namespace_revert() {
	$rootdir/scripts/setup.sh
	sleep 1
	local bdfs=()
	# If there are no nvme bdfs, just return immediately
	bdfs=($(get_nvme_bdfs)) || return 0

	$rootdir/scripts/setup.sh reset

	for bdf in "${bdfs[@]}"; do
		nvme_ctrlr=/dev/$(get_nvme_ctrlr_from_bdf ${bdf})
		if [[ -z "${nvme_ctrlr:-}" ]]; then
			continue
		fi

		# Check Optional Admin Command Support for Namespace Management
		oacs=$(nvme id-ctrl ${nvme_ctrlr} | grep oacs | cut -d: -f2)
		oacs_ns_manage=$((oacs & 0x8))

		if [[ "$oacs_ns_manage" -ne 0 ]]; then
			# This assumes every NVMe controller contains single namespace,
			# encompassing Total NVM Capacity and formatted as 512 block size.
			# 512 block size is needed for test/vhost/vhost_boot.sh to
			# successfully run.

			unvmcap=$(nvme id-ctrl ${nvme_ctrlr} | grep unvmcap | cut -d: -f2)
			if [[ "$unvmcap" -eq 0 ]]; then
				# All available space already used
				continue
			fi
			tnvmcap=$(nvme id-ctrl ${nvme_ctrlr} | grep tnvmcap | cut -d: -f2)
			cntlid=$(nvme id-ctrl ${nvme_ctrlr} | grep cntlid | cut -d: -f2)
			blksize=512

			size=$((tnvmcap / blksize))

			nvme detach-ns ${nvme_ctrlr} -n 0xffffffff -c $cntlid || true
			nvme delete-ns ${nvme_ctrlr} -n 0xffffffff || true
			nvme create-ns ${nvme_ctrlr} -s ${size} -c ${size} -b ${blksize}
			nvme attach-ns ${nvme_ctrlr} -n 1 -c $cntlid
			nvme reset ${nvme_ctrlr}
			waitforfile "${nvme_ctrlr}n1"
		fi
	done
}

# Get BDFs based on device ID, such as 0x0a54
function get_nvme_bdfs_by_id() {
	local bdfs=() _bdfs=()
	_bdfs=($(get_nvme_bdfs)) || return 0
	for bdf in "${_bdfs[@]}"; do
		device=$(cat /sys/bus/pci/devices/$bdf/device) || true
		if [[ "$device" == "$1" ]]; then
			bdfs+=($bdf)
		fi
	done

	((${#bdfs[@]} > 0)) || return 0
	printf '%s\n' "${bdfs[@]}"
}

function opal_revert_cleanup() {
	# The OPAL CI tests is only used for P4510 devices.
	mapfile -t bdfs < <(get_nvme_bdfs_by_id 0x0a54)
	if [[ -z ${bdfs[0]:-} ]]; then
		return 0
	fi

	$SPDK_BIN_DIR/spdk_tgt &
	spdk_tgt_pid=$!
	waitforlisten $spdk_tgt_pid

	bdf_id=0
	for bdf in "${bdfs[@]}"; do
		$rootdir/scripts/rpc.py bdev_nvme_attach_controller -b "nvme"${bdf_id} -t "pcie" -a ${bdf}
		# Ignore if this fails.
		$rootdir/scripts/rpc.py bdev_nvme_opal_revert -b "nvme"${bdf_id} -p test || true
		((++bdf_id))
	done

	killprocess $spdk_tgt_pid
}

function pap() {
	while read -r file; do
		cat <<- FILE
			--- $file ---
			$(< "$file")
			--- $file ---
		FILE
		rm -f "$file"
	done < <(find "$@" -type f | sort -u)
}

function get_proc_paths() {
	case "$(uname -s)" in
		Linux) # ps -e -opid,exe <- not supported under {centos7,rocky8}'s procps-ng
			local pid exe
			for pid in /proc/[0-9]*; do
				exe=$(readlink "$pid/exe") || continue
				exe=${exe/ (deleted)/}
				echo "${pid##*/} $exe"
			done
			;;
		FreeeBSD) procstat -ab | awk '{print $1, $4}' ;;
	esac
}

exec_files() { file "$@" | awk -F: '/ELF.+executable/{print $1}'; }

function reap_spdk_processes() {
	local bins test_bins procs
	local spdk_procs spdk_pids

	mapfile -t test_bins < <(find "$rootdir"/test/{app,env,event,nvme} -type f)
	mapfile -t bins < <(
		exec_files "${test_bins[@]}"
		readlink -f "$SPDK_BIN_DIR/"* "$SPDK_EXAMPLE_DIR/"*
	)
	((${#bins[@]} > 0)) || return 0

	mapfile -t spdk_procs < <(get_proc_paths | grep -E "$(
		IFS="|"
		echo "${bins[*]#$rootdir/}"
	)" || true)
	((${#spdk_procs[@]} > 0)) || return 0

	printf '%s is still up, killing\n' "${spdk_procs[@]}" >&2
	mapfile -t spdk_pids < <(printf '%s\n' "${spdk_procs[@]}" | awk '{print $1}')

	kill -SIGKILL "${spdk_pids[@]}" 2> /dev/null || :
	return 1
}

function is_block_zoned() {
	local device=$1

	[[ -e /sys/block/$device/queue/zoned ]] || return 1
	[[ $(< "/sys/block/$device/queue/zoned") != none ]]
}

function get_zoned_devs() {
	local -gA zoned_devs=()
	local -A zoned_ctrls=()
	local nvme bdf ns

	# When given ctrl has > 1 namespaces attached, we need to make
	# sure we pick up ALL of them, even if only one of them is zoned.
	# This is because the zoned_devs[] is mainly used for PCI_BLOCKED
	# which passed to setup.sh will skip entire ctrl, not a single
	# ns. FIXME: this should not be necessary. We need to find a way
	# to handle zoned devices more gracefully instead of hiding them
	# like that from all the other non-zns test suites.
	for nvme in /sys/class/nvme/nvme*; do
		bdf=$(< "$nvme/address")
		for ns in "$nvme/"nvme*n*; do
			if is_block_zoned "${ns##*/}"; then
				zoned_ctrls["$nvme"]=$bdf
				continue 2
			fi
		done
	done

	for nvme in "${!zoned_ctrls[@]}"; do
		for ns in "$nvme/"nvme*n*; do
			zoned_devs["${ns##*/}"]=${zoned_ctrls["$nvme"]}
		done
	done
}

function is_pid_child() {
	local pid=$1 _pid

	while read -r _pid; do
		((pid == _pid)) && return 0
	done < <(jobs -pr)

	return 1
}

function enable_coverage() {
	[[ $CONFIG_COVERAGE == y ]] || return 0
	if lt "$(lcov --version | awk '{print $NF}')" 2; then
		lcov_rc_opt="--rc lcov_branch_coverage=1 --rc lcov_function_coverage=1"
	else
		lcov_rc_opt="--rc branch_coverage=1 --rc function_coverage=1"
	fi
	export LCOV_OPTS="
		$lcov_rc_opt
		--rc genhtml_branch_coverage=1
		--rc genhtml_function_coverage=1
		--rc genhtml_legend=1
		--rc geninfo_all_blocks=1
		--rc geninfo_unexecuted_blocks=1
		$lcov_opt
		"
	export LCOV="lcov $LCOV_OPTS"
}

function gather_coverage() {
	[[ $CONFIG_COVERAGE == y ]] || return 0
	# generate coverage data and combine with baseline
	$LCOV -q -c --no-external -d "$rootdir" -t "$(hostname)" -o "$output_dir/cov_test.info"
	$LCOV -q -a "$output_dir/cov_base.info" -a "$output_dir/cov_test.info" -o "$output_dir/cov_total.info"
	$LCOV -q -r "$output_dir/cov_total.info" '*/dpdk/*' -o "$output_dir/cov_total.info"
	# C++ headers in /usr can sometimes generate data even when specifying
	# --no-external, so remove them. But we need to add an ignore-errors
	# flag to squash warnings on systems where they don't generate data.
	$LCOV -q -r "$output_dir/cov_total.info" --ignore-errors unused,unused '/usr/*' -o "$output_dir/cov_total.info"
	$LCOV -q -r "$output_dir/cov_total.info" '*/examples/vmd/*' -o "$output_dir/cov_total.info"
	$LCOV -q -r "$output_dir/cov_total.info" '*/app/spdk_lspci/*' -o "$output_dir/cov_total.info"
	$LCOV -q -r "$output_dir/cov_total.info" '*/app/spdk_top/*' -o "$output_dir/cov_total.info"
	rm -f "$output_dir/cov_base.info" "$output_dir/cov_test.info"
}

# Define temp storage for all the tests. Look for 2GB at minimum
set_test_storage "${TEST_MIN_STORAGE_SIZE:-$((1 << 31))}"

set -o errtrace
shopt -s extdebug
trap "trap - ERR; print_backtrace >&2" ERR

PS4=' \t ${test_domain:-} -- ${BASH_SOURCE#${BASH_SOURCE%/*/*}/}@${LINENO} -- \$ '
if $SPDK_AUTOTEST_X; then
	# explicitly enable xtraces, overriding any tracking information.
	xtrace_fd
else
	xtrace_disable
fi

enable_coverage
