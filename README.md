# Phobos Copytool for Lustre

## Installation

### Requirements

The copytool needs [Phobos](https://github.com/phobos-storage/phobos) version
1.93 or later. It has been tested with Lustre 2.12.7.

### Build and install

```
make

# You can set the install path explicitly
make PREFIX=... DESTDIR=... install

# Or alternativly build the rpm
make rpm
```

## How to use

For Phobos setup see [phobos-storage/phobos](https://github.com/phobos-storage/phobos).

### Lustre setup

On the MDT:
```
lfs set_param mdt.<MTDNAME>.hsm_control=enabled
```

### Start the copytool

**Main options:**

```
lhsmtool_phobos --default-family tape \
                --hsm_fsuid      "trusted.hsm_fuid" \
                --event-fifo     /tmp/fifo \
                --archive        1 \
                --archive        2 \
                --daemon \
                <lustre_mount_point>
```

- `-F|--default-family`: tape or dir
  The Phobos family used by this copytool to store objects. phobosd's
  configuration has to be compatible with this family. Otherwise, phobosd will
  not handle any request.
- `-t|--hsm_fsuid`: name of the extended attribute which stores the mapping
  between filename and object ID. This attribute is set on the file during the
  archive process.
- `-A|--archive`: archive number handled by this copytool. HSM Archive requests
  can target specific archive IDs. Only copytools with the matching archive ID
  will be able to handle them. This option can be repeated to indicate several
  archive IDs. If not used, the copytool will accept any request.
- `-f|--event-fifo`: path to a Linux FIFO on which events will be pushed.
  Useful for debugging.
- `--daemon`: make the copytool run on the background.

See `lhsmtool_phobos --help` for a complete list of options.

### Test the setup

Once the copytool and phobosd have started, they should be able to handle
requests. Try:

```
lfs hsm_archive <file>
lfs hsm_release <file>
lfs hsm_restore <file>
lfs hsm_remove  <file>
```
