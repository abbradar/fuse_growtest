# fuse_growtest

Demonstrates a possible FUSE race condition where readers see zero-filled bytes
when a FUSE-served file grows externally, with no userspace writes. The issue
was originally encountered while running
[JuiceFS](https://github.com/juicedata/juicefs/issues/5038) in production.

## LLM disclosure

LLMs were used to generate the code of these tests, which was then manually
reviewed. LLMs were also used to research the Linux kernel source code to
(hopefully) understand and fix the underlying issue. Any issues found here are
mine to blame on; I have no prior experience with the kernel-side FUSE code, so
the kernel patch reflects my current understanding and has a good chance of
being hot garbage.

## Building

```
$ nix develop
# Or another build environment with gcc, libfuse3, pkg-config and python
$ make
```

## Running

```
$ mkdir -p /tmp/test
$ sudo ./fuse_growtest /tmp/test        # or _inval / _direct variant
# in another terminal:
$ python3 check_stale.py /tmp/test/testfile
```

To exercise the `mmap()` path instead of `read()`:

```
$ ./check_stale_mmap /tmp/test/testfile
```

## The daemon

All three variants serve a single read-only file `/testfile` filled
entirely with `0xAA` bytes. The file starts at 4996 bytes (intentionally
straddling a page boundary) and grows at 128 KB/s.

## The `read()` checker

`check_stale.py` opens the file and reads it sequentially, checking every byte
for zeros. Since the daemon fills every byte with `0xAA`, any zero byte is
stale page cache data. Background threads hammer `stat()` to force frequent
`i_size` updates via FUSE `getattr`.

### the `mmap()` checker

`check_stale_mmap.c` does the same, but reads via a single long-lived `mmap()`
instead of `read()`, exercising the page-fault path (`filemap_fault`)

## Variants

### `fuse_growtest` — baseline

No cache management on the daemon side. The kernel invalidates cached pages on
its own based on `mtime` (which always grows).

### `fuse_growtest_inval` — invalidate in getattr

Before replying to `getattr` with a new size, the daemon calls
`fuse_lowlevel_notify_inval_inode()` to evict pages in the growth region.

### `fuse_growtest_direct` — direct I/O

Sets `fi->direct_io = 1` in the `open` callback, bypassing the page cache
entirely.

## Test results

All results were obtained on Linux 6.19.7, x86_64.

### `fuse_growtest`

```
reading /tmp/test/testfile, checking for stale zeros ...
  STALE ZERO at offset 311329 (page 76, +33) t=1.070s
  STALE ZERO at offset 373793 (page 91, +1057) t=1.547s
  STALE ZERO at offset 383290 (page 93, +2362) t=1.619s
  STALE ZERO at offset 442539 (page 108, +171) t=2.072s
  STALE ZERO at offset 452426 (page 110, +1866) t=2.147s
  STALE ZERO at offset 490233 (page 119, +2809) t=2.435s
  STALE ZERO at offset 505449 (page 123, +1641) t=2.551s
  STALE ZERO at offset 534734 (page 130, +2254) t=2.775s
  STALE ZERO at offset 536660 (page 131, +84) t=2.790s
  STALE ZERO at offset 537449 (page 131, +873) t=2.796s
  STALE ZERO at offset 545211 (page 133, +443) t=2.855s
  STALE ZERO at offset 565193 (page 137, +4041) t=3.007s
  STALE ZERO at offset 591458 (page 144, +1634) t=3.208s
  STALE ZERO at offset 597481 (page 145, +3561) t=3.254s
  STALE ZERO at offset 608768 (page 148, +2560) t=3.340s
  STALE ZERO at offset 651181 (page 158, +4013) t=3.663s
  STALE ZERO at offset 667886 (page 163, +238) t=3.791s
  STALE ZERO at offset 705815 (page 172, +1303) t=4.080s
  STALE ZERO at offset 708758 (page 173, +150) t=4.103s
  STALE ZERO at offset 708982 (page 173, +374) t=4.104s

Bug reproduced: 20 stale-zero region(s) in 4.1s (708985 bytes read).
```

### `fuse_growtest_inval`

```
reading /tmp/test/testfile, checking for stale zeros ...
  STALE ZERO at offset 654951 (page 159, +3687) t=1.091s
  STALE ZERO at offset 1307671 (page 319, +1047) t=6.071s
  STALE ZERO at offset 1599122 (page 390, +1682) t=8.294s
  STALE ZERO at offset 1913163 (page 467, +331) t=10.690s
  STALE ZERO at offset 2045906 (page 499, +2002) t=11.703s
  STALE ZERO at offset 2140250 (page 522, +2138) t=12.423s
  STALE ZERO at offset 2340911 (page 571, +2095) t=13.954s
  STALE ZERO at offset 2401705 (page 586, +1449) t=14.418s
  STALE ZERO at offset 2598190 (page 634, +1326) t=15.917s
  STALE ZERO at offset 2619645 (page 639, +2301) t=16.080s
  STALE ZERO at offset 3552286 (page 867, +1054) t=23.196s
  STALE ZERO at offset 3594686 (page 877, +2494) t=23.519s
  STALE ZERO at offset 3841503 (page 937, +3551) t=25.402s
  STALE ZERO at offset 5195667 (page 1268, +1939) t=35.734s
  STALE ZERO at offset 5221008 (page 1274, +2704) t=35.927s
  STALE ZERO at offset 5223710 (page 1275, +1310) t=35.948s
  STALE ZERO at offset 5341624 (page 1304, +440) t=36.847s
  STALE ZERO at offset 5933509 (page 1448, +2501) t=41.363s
  STALE ZERO at offset 5969383 (page 1457, +1511) t=41.637s
  STALE ZERO at offset 5970075 (page 1457, +2203) t=41.642s

Bug reproduced: 20 stale-zero region(s) in 41.6s (5970077 bytes read).
```

Per my understanding, the race remains: between the invalidation and the reply,
a concurrent read can re-populate the cache page with fresh zero-fill from a
short read, and the reply then widens `i_size` to expose those zeros.

### `fuse_growtest_direct`

```
reading /tmp/test/testfile, checking for stale zeros ...
^CNo corruption after 9558185 bytes.
```

Disabling the cache makes the issue go away.

## Kernel patch

I tried to understand and fix the issue; see [the kernel patch](./0001-fuse-fix-stale-page-cache-data-race-on-file-growth.patch). This patch is for the 6.6 kernel (the production machines we have use it). I also have a version for the curent master where FUSE was slightly refactored, but I didn't test it yet. The patch does *not* fully fix the issue, but it brings the number of corrupted data encountered down a notch. As far as I understand, there is no way to completely avoid this race without changes in the VFS kernel layer.

Results (`fuse_growtest`, a different machine, kernel 6.6.85 with the patch, x86_64), without the patch:
```
reading /tmp/test/testfile, checking for stale zeros ...
  STALE ZERO at offset 1963073 (page 479, +1089) t=0.100s
  STALE ZERO at offset 1994138 (page 486, +3482) t=0.337s
  STALE ZERO at offset 2047090 (page 499, +3186) t=0.741s
  STALE ZERO at offset 2194546 (page 535, +3186) t=1.866s
  STALE ZERO at offset 2292588 (page 559, +2924) t=2.614s
  STALE ZERO at offset 2417110 (page 590, +470) t=3.564s
  STALE ZERO at offset 2462982 (page 601, +1286) t=3.914s
  STALE ZERO at offset 2464947 (page 601, +3251) t=3.929s
  STALE ZERO at offset 2713329 (page 662, +1777) t=5.824s
  STALE ZERO at offset 2716999 (page 663, +1351) t=5.852s
  STALE ZERO at offset 2766151 (page 675, +1351) t=6.227s
  STALE ZERO at offset 2825134 (page 689, +2990) t=6.677s
  STALE ZERO at offset 2873237 (page 701, +1941) t=7.044s
  STALE ZERO at offset 2923176 (page 713, +2728) t=7.425s
  STALE ZERO at offset 2998149 (page 731, +3973) t=7.997s
  STALE ZERO at offset 3042058 (page 742, +2826) t=8.332s
  STALE ZERO at offset 3049528 (page 744, +2104) t=8.389s
  STALE ZERO at offset 3082821 (page 752, +2629) t=8.643s
  STALE ZERO at offset 3242074 (page 791, +2138) t=9.858s
  STALE ZERO at offset 3291488 (page 803, +2400) t=10.235s

Bug reproduced: 20 stale-zero region(s) in 10.2s (3291491 bytes read).
```

With the patch:

```
reading /tmp/test/testfile, checking for stale zeros ...
  STALE ZERO at offset 997305 (page 243, +1977) t=6.659s
  STALE ZERO at offset 3922831 (page 957, +2959) t=28.979s
  STALE ZERO at offset 5547601 (page 1354, +1617) t=41.375s
  STALE ZERO at offset 10956810 (page 2675, +10) t=82.644s
  STALE ZERO at offset 11055377 (page 2699, +273) t=83.396s
  STALE ZERO at offset 11837356 (page 2889, +4012) t=89.362s
  STALE ZERO at offset 13095774 (page 3197, +862) t=98.963s
  STALE ZERO at offset 15747624 (page 3844, +2600) t=119.195s
  STALE ZERO at offset 18610761 (page 4543, +2633) t=141.039s
  STALE ZERO at offset 18842102 (page 4600, +502) t=142.804s
  STALE ZERO at offset 19575057 (page 4779, +273) t=148.396s
  STALE ZERO at offset 19596421 (page 4784, +1157) t=148.559s
  STALE ZERO at offset 20219535 (page 4936, +1679) t=153.313s
  STALE ZERO at offset 20556393 (page 5018, +2665) t=155.883s
  STALE ZERO at offset 21270607 (page 5193, +79) t=161.332s
  STALE ZERO at offset 22470045 (page 5485, +3485) t=170.483s
  STALE ZERO at offset 23473400 (page 5730, +3320) t=178.138s
  STALE ZERO at offset 23517179 (page 5741, +2043) t=178.472s
  STALE ZERO at offset 23844070 (page 5821, +1254) t=180.966s
  STALE ZERO at offset 24747944 (page 6041, +4008) t=187.862s

Bug reproduced: 20 stale-zero region(s) in 187.9s (24747949 bytes read).
```

## The VFS issue

This is copied from my [NFS MRE](https://github.com/abbradar/nfs_stale_cache_test) which reproduces a similar bug and is my current understanding of the underlying VFS issue:

When a file grows remotely, the page before the old EOF in the read cache contains zero-fill beyond the old size. Those zeroes are valid while new size <= old size (they are beyond EOF), but become stale once the new size is updated to reflect the remote growth: the remote host wrote real data there, but the local cache still has the old zero-fill.

In filemap_read() (mm/filemap.c) we have:

```
do {
    ...
    error = filemap_get_pages(iocb, ...);  // (1) get cached folios
    ...
    isize = i_size_read(inode);            // (2) get file size
    ...
    // (3) copy from folio to user, capped at isize
} while (...);
```

If we grow the inode size in-between (1) and (2), the race happens; the old page gets capped at the new size, so the userspace reads zeroes where there should be actual data.

To trigger this bug, something must change the inode size in parallel with a read and not come from a user's `write()` since writes are coherent with reads via the cache layer. In a network FS this may happen on getattr when we discover that the remote file has grown, and update the inode's size. When this happens we need to mark the cache pages as stale, but there is no way to "lock" the page and the inode size simultaneously, so the race cannot be fixed just by stalling the cache in getattr.
