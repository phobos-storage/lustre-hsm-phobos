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
FSNAME=lustre
SKIP=77

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

function skip()
{
    echo $*
    exit $SKIP
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

    lctl pool_new "${FSNAME}.test_pool" ||
        error "Failed to create pool 'test_pool'"
    lctl pool_add "${FSNAME}.test_pool" OST[0] ||
        error "Failed to add 'OST0' to 'test_pool'"
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
    local rc=$?

    if [[ $rc == 0 ]]
    then
        echo "$test_name: PASS"
    elif [[ $rc == $SKIP ]]
    then
        echo "$test_name: SKIP"
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
            error "Event $event not found after 5s for $fid"
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

function user_md_contains()
{
    local oid="$1"
    local component="$2"
    local value="$3"

    phobos -q getmd "$oid" | grep "$component" | grep "$value"
    return $?
}

function get_oid_from_fid()
{
    echo "$FSNAME:$1"
}

function get_oid_from_path()
{
    echo "$FSNAME:$(lfs path2fid "$1")"
}

function get_file_user_md()
{
    phobos -q getmd "$(get_oid_from_path "$1")"
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

    local defaultstripe=$(lfs getstripe -cSp "$file")

    lfs migrate -c 2 -S 4096K "$file" ||
        error "Could not migrate '$file'"
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

    local newstripe=$(lfs getstripe -cSp "$file")

    if [[ $defaultstripe != $newstripe ]]
    then
        error "Stripping not set to default after restore: " \
              "$defaultstripe != $newstripe"
    fi
}
add_test archive_release_restore

function test_archive_release_restore_with_lov()
{
    local file="$test_dir/file"

    if ! ct_phobos --help | grep restore-lov
    then
        skip "Option '--restore-lov' required for this test"
    fi

    create_file "$file"
    lfs migrate -c 2 -S 4096K "$file" ||
        error "Could not migrate '$file'"

    local oldstripe=$(lfs getstripe -cSp "$file")

    add_event_watch
    start_copytool --restore-lov

    lfs hsm_archive "$file"
    wait_for_event ARCHIVE_FINISH "$file"

    lfs hsm_release "$file"
    wait_for_state "$file" "released"

    lfs hsm_restore "$file"
    wait_for_event RESTORE_FINISH "$file"

    local newstripe=$(lfs getstripe -cSp "$file")

    if [[ $oldstripe != $newstripe ]]
    then
        error "Stripping changed after restore: $oldstripe != $newstripe"
    fi
}
add_test archive_release_restore_with_lov

function invalid_layout_error()
{
    local file="$1"

    error -e "Invalid user_md for $file:\n" \
             "got: $(get_file_user_md "$file")\n" \
             "expected: $(lfs getstripe -cSLp "$file")"
}

function check_layout()
{
    local file="$1"
    local oid="$(get_oid_from_path "$file")"

    user_md_contains "$oid" "layout" \
        "stripe_count=$(lfs getstripe -c "$file")" ||
        invalid_layout_error "$file"

    user_md_contains "$oid" "layout" \
        "stripe_size=$(lfs getstripe -S "$file")" ||
        invalid_layout_error "$file"

    user_md_contains "$oid" "layout" \
        "pattern=$(lfs getstripe -L "$file")" ||
        invalid_layout_error "$file"

    if [[ $(lfs getstripe -p "$file") != "" ]]
    then
        user_md_contains "$oid" "layout" \
            "pool_name=$(lfs getstripe -p "$file")" ||
            invalid_layout_error "$file"
    fi
}

function test_layout_in_user_md()
{
    local file="$test_dir/file"

    create_file "$file"

    add_event_watch
    start_copytool

    lfs hsm_archive "$file"
    wait_for_event ARCHIVE_FINISH "$file"

    check_layout "$file"

    lfs migrate -p test_pool "$file" ||
        error "Could not migrate '$file' to 'test_pool'"
    lfs hsm_set --dirty "$file"
    lfs hsm_archive "$file"
    wait_for_event ARCHIVE_FINISH "$file"

    check_layout "$file"
}
add_test layout_in_user_md

function invalid_component_error()
{
    local file="$1"
    local id="$2"

    error -e "Invalid user_md for $file in component $id:\n" \
             "got: $(get_file_user_md "$file")\n" \
             "expected: $(lfs getstripe --component-id="$id" -cSLp "$file")"
}

function check_layout_component()
{
    local file="$1"
    local oid="$(get_oid_from_path "$file")"
    local id="$2"

    user_md_contains "$oid" "layout_comp$id" \
        "stripe_count=$(lfs getstripe --component-id="$id" -c "$file")" ||
        invalid_component_error "$file" "$id"

    user_md_contains "$oid" "layout_comp$id" \
        "stripe_size=$(lfs getstripe --component-id="$id" -S "$file")" ||
        invalid_component_error "$file" "$id"

    user_md_contains "$oid" "layout_comp$id" \
        "pattern=$(lfs getstripe --component-id="$id" -L "$file")" ||
        invalid_component_error "$file" "$id"

    if [[ $(lfs getstripe -p "$file") != "" ]]
    then
        user_md_contains "$oid" "layout_comp$id" \
            "pool_name=$(lfs getstripe --component-id="$id" -p "$file")" ||
            invalid_component_error "$file" "$id"
    fi

    local comp_start=$(lfs getstripe --component-id="$id" \
                       --component-start "$file")
    local comp_end=$(lfs getstripe --component-id="$id" --component-end "$file")

    user_md_contains "$oid" "layout_comp$id" \
        "extent_start=$comp_start" ||
        invalid_component_error "$file" "$id"

    user_md_contains "$oid" "layout_comp$id" \
        "extent_end=$comp_end" ||
        invalid_component_error "$file" "$id"
}

function check_layout_pfl()
{
    local file="$1"
    local oid="$(get_oid_from_path "$file")"

    for i in $(seq $(lfs getstripe --component-count "$file"))
    do
        user_md_contains "$oid" "layout_comp$i" "stripe_count" ||
            error "'layout_comp$i' not found in user_md of '$oid'"

        check_layout_component "$file" $i
    done
}

function test_layout_pfl_in_user_md()
{
    local file="$test_dir/file"

    create_file "$file"

    add_event_watch
    start_copytool

    lfs migrate \
        -E   1M -c  1 -S 256k -p test_pool \
        -E 512M -c  2 -S 512k -p "" \
        -E  -1  -c -1 -S 1024k \
        "$file" || error "Could not migrate '$file'"

    lfs hsm_archive "$file"
    wait_for_event ARCHIVE_FINISH "$file"

    check_layout_pfl "$file"
}
add_test layout_pfl_in_user_md

run_tests
