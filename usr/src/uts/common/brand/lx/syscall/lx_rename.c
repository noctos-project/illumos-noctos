/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 */

/*
 * Copyright 2016 Joyent, Inc.
 * Copyright 2026 the NoctOS Project.
 */

#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/systm.h>
#include <sys/lx_fcntl.h>

/* From uts/common/syscall/rename.c */
extern int rename(char *, char *);
extern int renameat(int, char *, int, char *);

long
lx_rename(char *p1, char *p2)
{
	return (rename(p1, p2));
}

long
lx_renameat(int atfd1, char *p1, int atfd2, char *p2)
{
	if (atfd1 == LX_AT_FDCWD)
		atfd1 = AT_FDCWD;

	if (atfd2 == LX_AT_FDCWD)
		atfd2 = AT_FDCWD;

	return (renameat(atfd1, p1, atfd2, p2));
}

/*
 * renameat2(2) flags, from the Linux uapi.
 */
#define	LX_RENAME_NOREPLACE	(1 << 0)
#define	LX_RENAME_EXCHANGE	(1 << 1)
#define	LX_RENAME_WHITEOUT	(1 << 2)

long
lx_renameat2(int atfd1, char *p1, int atfd2, char *p2, int flags)
{
	/*
	 * When no flags are present, renameat2() is exactly renameat().
	 *
	 * None of the flags can currently be honored with correct semantics:
	 * RENAME_NOREPLACE and RENAME_EXCHANGE require atomicity that the
	 * illumos VFS does not provide (emulating them with a lookup would
	 * silently weaken a guarantee callers may rely upon), and
	 * RENAME_WHITEOUT is specific to overlay-style filesystems.  Linux
	 * returns EINVAL when the underlying filesystem does not support a
	 * requested flag, so EINVAL is the correct (and fallback-friendly)
	 * answer here; glibc and coreutils handle it as "unsupported" and
	 * fall back, just as they do on filesystems like NFS.
	 */
	if (flags != 0)
		return (set_errno(EINVAL));

	return (lx_renameat(atfd1, p1, atfd2, p2));
}
