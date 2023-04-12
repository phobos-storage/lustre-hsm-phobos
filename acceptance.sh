#!/bin/env bash

VERBOSE=${VERBOSE:--vv}
QUICK=${QUICK:-false}
LOG_DIR=${LOG_DIR:-/tmp/lhsmtool_phobos}
SERVERNAME=${SERVERNAME:-$(hostname)}

mkdir -p "$LOG_DIR"

FIFO="$LOG_DIR/event_fifo"
EVENTS="$LOG_DIR/hsm_events"
PHOBOSD_LOG="$LOG_DIR/phobosd.log"
PHOBOSD_LOCK=/tmp/phobosd.lock
PHOBOSD_SOCKET=/tmp/lrs
LUSTRE_ROOT=/mnt/lustre
FSNAME=lustre
SKIP=77
COPYTOOL_PID=
declare -a dirs
if [ ! -z ${VALGRIND+x} ]
then
    VALGRIND=${VALGRIND:-valgrind --leak-check=full --show-leak-kinds=all \
--track-origins=yes --error-exitcode=1 \
--suppressions=tests/valgrind.supp \
--gen-suppressions=all}
fi

# Safe check QUICK variable
if [[ $QUICK != true && $QUICK != false ]]
then
    QUICK=false
fi

function error()
{
    echo "$*"
    exit 1
}

function skip()
{
    echo "$*"
    exit $SKIP
}

function start_phobosd()
{
    phobosd $VERBOSE &> "$PHOBOSD_LOG" ||
        error "Failed to start phobosd"
}

function stop_phobosd()
{
    kill $(pgrep phobosd)
}

function phobos_setup()
{
    sudo systemctl status postgresql >/dev/null 2>&1 ||
        error "postgresql service not started"
    HAVE_POSTGRES=y
    sudo -u postgres phobos_db setup_db -p phobos
    phobos_db setup_tables

    export PHOBOS_LRS_lock_file="$PHOBOSD_LOCK"
    export PHOBOS_LRS_server_socket="$PHOBOSD_SOCKET"
    rm -f "$PHOBOSD_LOCK"

    echo "Starting phobosd, log file: $PHOBOSD_LOG"
    start_phobosd

    for i in {0..2}
    do
        dirs+=($(mktemp -d))

        phobos dir add "${dirs[i]}"
        phobos dir format --fs posix --unlock "${dirs[i]}"
        phobos dir update --tags t$i "${dirs[i]}"
    done

    echo '' >> /etc/phobos.conf
    echo '[alias "test_alias"]' >> /etc/phobos.conf
    echo 'layout = raid1' >> /etc/phobos.conf
    echo 'lyt-params = repl_count=3' >> /etc/phobos.conf
}

function phobos_teardown()
{
    if [ "$HAVE_POSTGRES" != "y" ]
    then
        return
    fi

    stop_phobosd
    phobos_db drop_tables
    sudo -u postgres phobos_db drop_db

    rm -rf "${dirs[@]}"
}

function lustre_is_mounted()
{
    mount -t lustre | grep "$LUSTRE_ROOT" > /dev/null
    return $?
}

# Run a function on the server
function frun_server()
{
    local func=$1

    if [ "$SERVERNAME" = $(hostname) ]
    then
        $func
        return
    fi

    ssh "$SERVERNAME" "
export PATH="$PATH:/usr/lib64/lustre/tests"
# XXX variables are not passed through ssh. Global variables and new functions
# must be added if necessary.
FSNAME=$FSNAME
$(declare -f error)
$(declare -f $func)
$func"
}

function lustre_server_setup()
{
    llmount.sh
    lctl set_param mdt.*.hsm_control=enabled
    lctl set_param mdt.*.hsm.max_requests=50

    lctl pool_new "${FSNAME}.test_pool" ||
        error "Failed to create pool 'test_pool'"
    lctl pool_add "${FSNAME}.test_pool" OST[0] ||
        error "Failed to add 'OST0' to 'test_pool'"
}

function lustre_client_setup()
{
    if [ "$SERVERNAME" = "$(hostname)" ]
    then
        return
    fi

    mkdir -p /mnt/lustre
    mount -t lustre "${SERVERNAME}@tcp:/${FSNAME}" /mnt/lustre
}

function lustre_setup()
{
    frun_server lustre_server_setup
    lustre_client_setup
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
    touch "$PHOBOSD_LOG"
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

function args_to_filename()
{
    local args=$1

    if [ -z "$args" ]
    then
        echo ""
    else
        # Replace space with '_'
        echo "_${args// /_}"
    fi
}

# Traps are a bit tricky. Since the exit status of the subshell running the test
# can be overwritten by a trapped function, we cannot simply trap any command we
# want to execute at the end of the test. Otherwise a test that fails may be
# interpreted as successful. To work around this issue, a global 'traps' array
# will keep track of which commands should be executed at the end of the test.
# They will be executed by 'at_exit' which is trapped in the same subshell that
# runs the test. The main process executing the whole test suite will only trap
# global_teardown.
function run_test()
{
    local test_name="$1"
    local test_func="test_$test_name"
    local tmp=$(mktemp)

    shift

    local args="$*"
    local log_file="$LOG_DIR/${test_name}$(args_to_filename "$args").log"

    (
     test_dir=$(mktemp -d -p "$LUSTRE_ROOT")
     trap_add "rm -r $test_dir"

     local begin=`date +%s.%N`
     (trap at_exit EXIT; $test_func "$args" &> "$log_file")
     local rc=$?
     local end=`date +%s.%N`

     echo "$end - $begin" | bc -l > "$tmp"
     exit $rc
    )
    local rc=$?
    local runtime=$(cat "$tmp")

    rm "$tmp"

    if [ ! -z "$args" ]
    then
        args=" $args"
    fi

    if [[ $rc == 0 ]]
    then
        printf ' \033[0;30;42m PASS   \033[0;0;0m %s%s (%s s)\n' \
            "$test_name" "$args" "$runtime"
    elif [[ $rc == $SKIP ]]
    then
        printf ' \033[0;30;47m SKIP   \033[0;0;0m %s%s (%s s)\n' \
            "$test_name" "$args" "$runtime"
    else
        printf ' \033[0;30;41m FAILED \033[0;0;0m %s%s (%s s)\n' \
                "$test_name" "$args" "$runtime"
        ((FAILURES++))
        echo "Log file: $log_file"
    fi
}

function randint()
{
    local min=$1
    local max=$2

    echo $(( $RANDOM % max + min ))
}

tests=()
test_args=()

function add_test()
{
    tests+=($1)
    shift
    test_args+=("$*")
}

function run_tests()
{
    echo ""
    echo "Starting tests. Log dir: $LOG_DIR"

    FAILURES=0
    if [ ! -z "$TESTS" ]
    then
        for t in $TESTS
        do
            run_test "$t"
        done
    else
        for i in $(seq 0 $((${#tests[@]} - 1)))
        do
            run_test "${tests[$i]}" "${test_args[$i]}"
        done
    fi

    echo ""
}

function create_file()
{
    local file="$1"

    touch "$file"
    dd if=/dev/urandom of="$file" bs=4096 count=$(randint 5 20)

    fid_nobrace "$file"
}

function add_event_watch()
{
    echo "Registering events from $FIFO in $EVENTS"
    truncate -s 0 "$EVENTS"
    (cat "$FIFO" > "$EVENTS") &
}

function wait_for_event()
{
    local fid=$(fid_nobrace "$2")

    wait_for_event_fid "$1" "$fid"
}

function wait_for_event_fid()
{
    local event="$1"
    local fid="$2"
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
    local key="$2"
    local value="$3"

    phobos -q getmd "$oid" | grep -Fe "$key" -e "$value"
}

function fid_nobrace()
{
    lfs path2fid "$1" | sed "s/\[\(.*\)\]/\1/"
}

function get_oid_from_path()
{
    echo "$FSNAME:$(fid_nobrace "$1")"
}

function get_file_user_md()
{
    phobos -q getmd "$(get_oid_from_path "$1")"
}

# Executes all the traps at the end of each test
function at_exit()
{
    local rc=$?

    if (( rc != 0 && rc != SKIP ))
    then
        echo "Cleanup test..."
    fi

    for (( i = 0; i < ${#traps[@]}; i++ ))
    do
        if (( rc != 0 && rc != SKIP ))
        then
            echo "${traps[i]}"
        fi
        ${traps[i]}
    done
    exit $rc
}

function trap_add()
{
    traps+=("$1")
}

__lhsmtool_phobos=$(PATH="$PWD:$PWD/build:$PATH" which lhsmtool_phobos)
function ct_phobos()
{
    $VALGRIND "$__lhsmtool_phobos" "$@" &
    COPYTOOL_PID=$!
}

__hsm_import=$(PATH="$PWD:$PWD/build:$PATH" which hsm-import)
function hsm_import()
{
    "$__hsm_import" "$@"
}

function kill_copytool()
{
    kill $COPYTOOL_PID || error "Daemon was not running"
    wait $COPYTOOL_PID

    COPYTOOL_PID=0
}

function start_copytool()
{
    ct_phobos "$@" "$VERBOSE"   \
        --default-family dir    \
        --event-fifo "$FIFO"    \
        "$LUSTRE_ROOT"

    trap_add kill_copytool
}

function get_xattr_value()
{
    local file="$1"
    local key="$2"

    getfattr -n "trusted.$key" "$file" |
        grep -v "file:" |
        sed "s/trusted.$key=\"\(.*\)\"$/\1/"
}

trap global_teardown EXIT
global_setup

###########
## Tests ##
###########

function test_no_daemon()
{
    local file="$test_dir/file"

    create_file "$file"

    add_event_watch
    start_copytool

    lfs hsm_archive "$file"
    wait_for_event ARCHIVE_FINISH "$file"

    trap_add "start_phobosd"
    stop_phobosd

    lfs hsm_release "$file"
    # This request will fail because phobosd is not here but the copytool should
    # stay up.
    lfs hsm_restore "$file"
    wait_for_event RESTORE_ERROR "$file"

    ps -p $COPYTOOL_PID ||
        error "Copytool crashed in the absence of phobosd"
}
add_test no_daemon

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
    local pool=${1:-""}

    create_file "$file"

    add_event_watch
    start_copytool

    lfs migrate -p "$pool" "$file" ||
        error "Could not migrate '$file' to '$pool'"

    lfs hsm_archive "$file"
    wait_for_event ARCHIVE_FINISH "$file"

    check_layout "$file"
}
add_test layout_in_user_md test_pool
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

function test_hints_fuid_xattr()
{
    local file="$test_dir/file"
    local fid=$(create_file "$file")

    add_event_watch
    start_copytool

    lfs hsm_archive --data "hsm_fuid=$file" "$file"
    wait_for_event ARCHIVE_FINISH "$file"

    local fuid=$(get_xattr_value "$file" hsm_fuid)

    if [[ $fuid = $file ]]
    then
        error "hsm_fuid hint was not ignored during archive"
    elif [[ $fuid != "$(get_oid_from_path "$file")" ]]
    then
        error "xattr trusted.hsm_fuid should be " \
              "'$(get_oid_from_path "$file")', not '$fuid'."
    fi

    if [[ $(phobos object list "$file" | wc -l) > 0 ]]
    then
        error "File '$file' was stored in phobos as '$file', hsm_fuid hint" \
              "was not ignored. It should have been '$fuid'."
    fi

    lfs hsm_release "$file"

    lfs hsm_restore --data "hsm_fuid=$file" "$file"
    # Restore should succeed as it ignores this hint
    wait_for_event RESTORE_FINISH "$file"
}
add_test hints_fuid_xattr

function test_hsm_import()
{
    local file="$test_dir/file"
    local fid=$(create_file "$file")
    local stat_file=$(mktemp)

    add_event_watch
    start_copytool

    lfs hsm_archive "$file"
    wait_for_event ARCHIVE_FINISH "$file"

    hsm_import -s "$stat_file" -p "${file}"  --stat
    local newfid=$(hsm_import -s "$stat_file" -p "${file}2" --undelete)

    lfs hsm_restore "${file}2"
    wait_for_event RESTORE_FINISH "${file}2"

    rm "${file}" "${file}2"

    # the files are now removed, we can't use the xattr to get the object ID
    lfs hsm_remove --mntpath "$LUSTRE_ROOT" "$newfid"
    wait_for_event_fid REMOVE_ERROR "$newfid"

    if [[ $(phobos object list "lustre:$fid" | wc -l) == 0 ]]
    then
        error "Removing from archive a removed file should fail"
    fi

    lfs hsm_remove --mntpath "$LUSTRE_ROOT" \
        --data "hsm_fuid=lustre:$fid" "$newfid"
    wait_for_event_fid REMOVE_FINISH "$newfid"

    if [[ $(phobos object list "lustre:$fid" | wc -l) != 0 ]]
    then
        error "Removing from archive a removed file with hint should succeed"
    fi
}
add_test hsm_import

function test_fuid_xattr()
{
    local file="$test_dir/file"
    local attr=test_xattr
    local arg=${1:--x}

    add_event_watch
    start_copytool "$arg" "trusted.$attr"

    create_file "$file"

    lfs hsm_archive "$file"
    wait_for_event ARCHIVE_FINISH "$file"

    get_xattr_value "$file" "$attr" | grep -F "$(get_oid_from_path "$file")" ||
        error "Object ID should be stored in 'trusted.$attr'"
}
add_test fuid_xattr --fuid-xattr
add_test fuid_xattr -x

function test_phobos_alias()
{
    local file="$test_dir/file"

    create_file "$file"

    local oid=$(get_oid_from_path "$file")

    add_event_watch
    start_copytool

    lfs hsm_archive --data "alias=test_alias" "$file"
    wait_for_event ARCHIVE_FINISH "$file"

    local repl_count=$(phobos extent list -o ext_count "$oid")
    if [[ $repl_count != 3 ]]
    then
        error "Invalid repl_count for alias 'test_alias'." \
              "Expected '3', got '$repl_count'"
    fi
}
add_test phobos_alias

# XXX /!\ unfortunatly, valgrind returns the exit code of the process if its
# exit status is non zero. Which means that we cannot easily automate memory
# testing for this test. Currently, this test will pass even if valgrind detects
# errors. Therefore, one should check the logs of this test when using valgrind.
function test_invalid_archive_id()
{
    local arg=${1:--1}
    local ERANGE=34
    local EINVAL=22
    local rc

    timeout --preserve-status 1 \
        $VALGRIND "$__lhsmtool_phobos" -A 1 -A "$arg" "$LUSTRE_ROOT"
    rc=$?
    echo $rc
    (( rc == ERANGE || rc == EINVAL )) &&
        exit 0
}
add_test invalid_archive_id -1
add_test invalid_archive_id invalid
add_test invalid_archive_id 2147483648 # INT_MAX + 1

function test_all_archive_ids()
{
    local file="$test_dir/file"
    local copy="$test_dir/copy"

    create_file "$file"

    local defaultstripe=$(lfs getstripe -cSp "$file")

    lfs migrate -c 2 -S 4096K "$file" ||
        error "Could not migrate '$file'"

    add_event_watch
    start_copytool -A 0

    lfs hsm_archive --archive 17 "$file"
    wait_for_event ARCHIVE_FINISH "$file"
}
add_test all_archive_ids

function test_rpm()
{
    cd build

    meson compile rpm
}
add_test rpm

function test_tag_hint()
{
    local file="$test_dir/file"

    create_file "$file"

    add_event_watch
    start_copytool

    # Lock the directory with tag t0
    phobos dir lock "${dirs[0]}"
    trap_add "phobos dir unlock ${dirs[0]}"

    local phobosd_pid=$(pgrep phobosd)
    stop_phobosd
    tail --pid $phobosd_pid -f /dev/null
    start_phobosd

    lfs hsm_archive --data "tag=t0" "$file"

    # The archive should fail since no unlocked directory has the tag t0
    wait_for_event ARCHIVE_ERROR "$file"
}
add_test tag_hint

function test_invalid_tag_hint()
{
    local file="$test_dir/file"

    create_file "$file"

    add_event_watch
    start_copytool

    lfs hsm_archive --data "tag=t0,tag=invalid" "$file"

    # The archive should fail since no directory has the tag invalid
    wait_for_event ARCHIVE_ERROR "$file"
}
add_test invalid_tag_hint

run_tests

exit $FAILURES
