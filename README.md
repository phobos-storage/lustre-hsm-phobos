# Phobos Copytool for Lustre

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
- `extent_start`, `extent_end`: only used for PFL layouts. `EOF` is can be used
  for `extent_end`.
