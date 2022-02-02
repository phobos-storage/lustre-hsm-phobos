#!/bin/bash

VERBOSE=${VERBOSE:--vv}
QUICK=${QUICK:-false}

ARCHIVE_PATH=$(mktemp -d)
LOG_DIR=$(mktemp -d)
FIFO="$LOG_DIR/event_fifo"
EVENTS="$LOG_DIR/hsm_events"
PHOBOSD_LOG=$(mktemp -t phobosd.log.XXXXX -p "$LOG_DIR")
PHOBOSD_LOCK=/tmp/phobosd.lock
PHOBOSD_SOCKET=/tmp/lrs
LUSTRE_ROOT=/mnt/lustre

# Safe check QUICK variable
if [[ $QUICK != true && $QUICK != false ]]
then
    QUICK=false
fi

function error()
{
    echo $*
    exit 1
}

function phobos_setup()
{
    sudo -u postgres phobos_db setup_db -p phobos
    phobos_db setup_tables

    export PHOBOS_LRS_lock_file="$PHOBOSD_LOCK"
    export PHOBOS_LRS_server_socket="$PHOBOSD_SOCKET"
    rm -f "$PHOBOSD_LOCK"

    echo "Starting phobosd, log file: $PHOBOSD_LOG"
    phobosd $VERBOSE &> "$PHOBOSD_LOG" ||
        error "Failed to start phobosd"
    PHOBOSD_PID=$(pgrep phobosd)

    phobos dir add "$ARCHIVE_PATH" ||
        error "Could not add $ARCHIVE_PATH"
    phobos dir format --fs posix --unlock "$ARCHIVE_PATH" ||
        error "Could not format $ARCHIVE_PATH"
}

function phobos_teardown()
{
    kill "$PHOBOSD_PID"
    phobos_db drop_tables
    sudo -u postgres phobos_db drop_db
}

function lustre_is_mounted()
{
    mount -t lustre | grep "$LUSTRE_ROOT" > /dev/null
    return $?
}

function lustre_setup()
{
    llmount.sh
    lctl set_param mdt.*.hsm_control=enabled
    lctl set_param mdt.*.hsm.max_requests=50
}

function global_setup()
{
    # Add Lustre test binaries to PATH
    export PATH="$PATH:/usr/lib64/lustre/tests"

    if ! $QUICK
    then
        lustre_setup
    elif ! lustre_is_mounted
    then
        lustre_setup
    fi

    phobos_setup

    mkfifo -m 644 "$FIFO"
    touch "$EVENTS"
}

function global_teardown()
{
    echo "Cleanup..."

    phobos_teardown
    if ! $QUICK
    then
        llmountcleanup.sh
    fi

    rm -f "$FIFO" "$EVENTS"
}

function run_test()
{
    local test_name="$1"
    local test_func="test_$test_name"
    local log_file="$LOG_DIR/$test_name.log"

    (
     test_dir=$(mktemp -d -p "$LUSTRE_ROOT")
     trap "rm -rf $test_dir" EXIT
     $test_func &> "$log_file"
    )

    if [[ $? == 0 ]]
    then
        echo "$test_name: PASS"
    else
        echo "$test_name: FAILED"
        echo "Log file: $log_file"
        cat "$log_file"
    fi
}

function randint()
{
    local min=$1
    local max=$2

    echo $(( $RANDOM % max + min ))
}

tests=()

function add_test()
{
    tests+=($1)
}

function run_tests()
{
    echo ""
    echo "Starting tests. Log dir: $LOG_DIR"

    for t in ${tests[@]}
    do
        run_test "$t"
    done

    echo ""
}

function create_file()
{
    local file="$1"

    touch "$file"
    dd if=/dev/urandom of="$file" bs=4096 count=$(randint 5 20)
}

function wait_for_state()
{
    local file="$1"
    local expected="$2"
    local Max=10
    local N=0

    while ! lfs hsm_state "$file" | grep -e "$expected"
    do
        if [[ $N == $Max ]]
        then
            local state=$(lfs hsm_state "$file" | cut -d: -f2)

            error "After 5s, $file was still not in state $expected but $state"
        fi

        sleep 0.5
        ((N++))
    done
}

function add_event_watch()
{
    echo "Registering events from $FIFO in $EVENTS"
    truncate -s 0 "$EVENTS"
    (cat "$FIFO" > "$EVENTS") &
}

function wait_for_event()
{
    local event="$1"
    local fid=$(lfs path2fid "$2" | sed 's/\[\(.*\)\]/\1/')
    local count=0

    while ! grep -B 1 -a "$fid" "$EVENTS" | grep -a "$event"
    do
        sleep 0.1
        ((count++))
        if ((count > 50))
        then
            cat "$EVENTS"
            error "Event $event not found after 5s"
        fi
    done
}

function check_valid_restore()
{
    local orig="$1"
    local restored="$2"

    diff "$orig" "$restored" >/dev/null ||
        error "File $restored changed after restore"
}

function get_trap()
{
    echo $(trap -p EXIT | sed "s/trap -- '\(.*\)' EXIT/\1/")
}

function trap_add()
{
    eval "trap '$(get_trap); $1' EXIT"
}

__lhsmtool_phobos=$(PATH="$PWD/build:$PATH" which lhsmtool_phobos)
function ct_phobos()
{
    "$__lhsmtool_phobos" "$@"
}

function start_copytool()
{
    ct_phobos "$@" "$VERBOSE"   \
        --default-family dir    \
        --event-fifo "$FIFO"    \
        --daemon "$LUSTRE_ROOT"

    trap_add "kill -9 $(pgrep lhsmtool_phobos)"
}

trap global_teardown EXIT
global_setup

###########
## Tests ##
###########

function test_archive_release_restore()
{
    local file="$test_dir/file"
    local copy="$test_dir/copy"

    create_file "$file"
    cp "$file" "$copy"

    add_event_watch
    start_copytool

    lfs hsm_archive "$file"
    wait_for_event ARCHIVE_FINISH "$file"

    lfs hsm_release "$file"
    wait_for_state "$file" "released"

    lfs hsm_restore "$file"
    wait_for_event RESTORE_FINISH "$file"

    check_valid_restore "$copy" "$file"
}
add_test archive_release_restore

run_tests
