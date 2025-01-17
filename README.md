# Phobos Copytool for Lustre

## Installation

### Requirements

The copytool needs [Phobos](https://github.com/phobos-storage/phobos) version
2.2 or later. It has been tested with Lustre 2.12.x and 2.15.0.

`rpmbuild` is also required to generate the RPMs.

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
lctl set_param mdt.<MTDNAME>.hsm_control=enabled
```

### Start the copytool

**Main options:**

```
lhsmtool_phobos --default-family tape \
                --fuid-xattr     "trusted.hsm_fuid" \
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
- `-x|--fuid-xattr`: name of the extended attribute which stores the mapping
  between filename and object ID. This attribute is set on the file during the
  archive process.
- `-A|--archive`: archive number handled by this copytool. HSM Archive requests
  can target specific archive IDs. Only copytools with the matching archive ID
  will be able to handle them. This option can be repeated to indicate several
  archive IDs. If not used, the copytool will accept any request.
- `-f|--event-fifo`: path to a Linux FIFO on which events will be pushed.
  Useful for debugging.
- `--daemon`: make the copytool run in the background.

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

## File striping

The file striping is stored in Phobos's `user_md` when the file is archived.
If the file has a plain layout the striping will be stored under the key
`layout`. For PFL layouts, each component will be stored under `layout_comp<id>`
where `<id>` is the component id (cf. `lfs getstripe --component-id <file>`).

The following values are stored:

- `stripe_count`: `-1` if the file is striped over every OST;
- `stripe_size`;
- `pattern`: either `raid0` or `mdt`;
- `pool_name`: only present if the pool name is not empty;
- `extent_start`, `extent_end`: only used for PFL layouts. `EOF` can be used
  for `extent_end`.

## Hints

The copytool supports hints though the HSM requests (e.g. `lfs hsm_remove
--data "hsm_fuid=<myoid>"`). The hints are provided through a list of coma
separated key value pairs (`k1=v1,k2=v2`). The supported hints are:

| Name          | Accepted Values                                    | HSM Action Type |
| ------------- | -------------------------------------------------- | --------------- |
| `hsm_fuid`    | A printable string of characters                   | Remove          |
| `family`      | Any valid Phobos family (e.g. `dir`, `tape`, etc.) | Archive         |
| `layout`      | Any valid Phobos layout (e.g. `raid1`)             | Archive         |
| `alias`       | Any valid profile defined in the configuration     | Archive         |
| `profile`     | Any valid profile defined in the configuration     | Archive         |
| `tag`         | A printable string of characters                   | Archive         |
| `grouping`    | A printable string of characters                   | Archive         |

**Note:** the alias hint is the old name of the profile hint. As we may remove
it in a future version of the copytool, please use profile instead.
**Note:** the tag hint can be specified several times to add more tags.
