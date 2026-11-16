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
 * Copyright 2026 the NoctOS Project.
 */

/*
 * memfd_create(2) emulation.
 *
 * A memfd is an anonymous, tmpfs-backed regular file reached only
 * through its file descriptor.  We emulate it with the same mechanism
 * Linux uses underneath: create a uniquely-named file on the zone's
 * /dev/shm tmpfs (which the lx brand guarantees is mounted), open it,
 * and immediately unlink it so that the descriptor is the only
 * remaining reference.
 *
 * File sealing (fcntl F_ADD_SEALS/F_GET_SEALS, see lx_fcntl.c) is
 * provided by tmpfs itself (tn_seals; see tmp_seal_add()), so seals
 * are enforced on every access path -- including native ones -- rather
 * than only on branded syscalls.  When MFD_ALLOW_SEALING is not
 * requested, Linux semantics call for the file to start with
 * F_SEAL_SEAL set (it can never be sealed); we do the same.
 */

#include <sys/systm.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/file.h>
#include <sys/vnode.h>
#include <sys/atomic.h>
#include <sys/cred.h>
#include <sys/proc.h>
#include <sys/kmem.h>
#include <sys/fs/tmp.h>
#include <sys/fs/tmpnode.h>
#include <sys/lx_brand.h>

/* memfd_create(2) flags, from the Linux uapi. */
#define	LX_MFD_CLOEXEC		0x0001
#define	LX_MFD_ALLOW_SEALING	0x0002
#define	LX_MFD_HUGETLB		0x0004	/* unsupported */

/* Linux limits the name to NAME_MAX - strlen("memfd:") characters. */
#define	LX_MFD_NAME_MAX		249

#define	LX_MFD_SHM_DIR		"/dev/shm"
#define	LX_MFD_RETRIES		3

static uint64_t lx_memfd_seq = 0;

long
lx_memfd_create(char *uname, int flags)
{
	char name[LX_MFD_NAME_MAX + 1];
	char *path = NULL;
	size_t len, i;
	vnode_t *vp = NULL;
	file_t *fp = NULL;
	uint_t seals;
	uint64_t seq;
	int fd, error, attempt;

	if ((flags & ~(LX_MFD_CLOEXEC | LX_MFD_ALLOW_SEALING)) != 0) {
		/* this includes LX_MFD_HUGETLB, which we do not support */
		return (set_errno(EINVAL));
	}

	error = copyinstr(uname, name, sizeof (name), &len);
	if (error == ENAMETOOLONG)
		return (set_errno(EINVAL));
	if (error != 0)
		return (set_errno(EFAULT));

	/*
	 * The name is only a debugging label on Linux and may contain
	 * any character, but since we embed it in a real (if transient)
	 * path, neuter '/' characters.
	 */
	for (i = 0; i < len; i++) {
		if (name[i] == '/')
			name[i] = '_';
	}

	path = kmem_alloc(MAXPATHLEN, KM_SLEEP);

	/*
	 * Create and open a uniquely-named file on the zone's /dev/shm.
	 * vn_openat() resolves the path against this process's root, so
	 * inside a zone this lands on the zone's own tmpfs mount.  The
	 * sequence number makes collisions essentially impossible, but
	 * retry a few times in case something in the zone is squatting
	 * on names.
	 */
	for (attempt = 0; attempt < LX_MFD_RETRIES; attempt++) {
		seq = atomic_inc_64_nv(&lx_memfd_seq);
		(void) snprintf(path, MAXPATHLEN, "%s/.memfd:%s:%u:%llu",
		    LX_MFD_SHM_DIR, name, (uint_t)curproc->p_pid,
		    (u_longlong_t)seq);
		error = vn_openat(path, UIO_SYSSPACE,
		    FREAD | FWRITE | FCREAT | FEXCL | FOFFMAX, 0700, &vp,
		    CRCREAT, 0, NULL, -1);
		if (error != EEXIST)
			break;
	}
	if (error != 0) {
		kmem_free(path, MAXPATHLEN);
		return (set_errno(error));
	}

	/*
	 * Unlink it immediately: the descriptor is the only reference.
	 * A failure here is not fatal to the semantics (the node would
	 * merely remain visible); the name is unpredictable enough that
	 * nothing should race us for it.
	 */
	(void) vn_removeat(NULL, path, UIO_SYSSPACE, RMFILE);
	kmem_free(path, MAXPATHLEN);
	path = NULL;

	/*
	 * Seals live in tmpfs.  If this probe fails, the zone's /dev/shm
	 * is not tmpfs and we cannot provide memfd semantics at all.
	 */
	if (tmp_seal_get(vp, &seals) != 0) {
		error = ENOSYS;
		goto fail;
	}
	if ((flags & LX_MFD_ALLOW_SEALING) == 0) {
		if ((error = tmp_seal_add(vp, TMP_SEAL_SEAL)) != 0)
			goto fail;
	}

	if (falloc(vp, FWRITE | FREAD, &fp, &fd) != 0) {
		error = EMFILE;
		goto fail;
	}
	mutex_exit(&fp->f_tlock);
	setf(fd, fp);
	if (flags & LX_MFD_CLOEXEC)
		f_setfd_or(fd, FD_CLOEXEC);

	return (fd);

fail:
	(void) VOP_CLOSE(vp, FREAD | FWRITE, 1, 0, CRED(), NULL);
	VN_RELE(vp);
	return (set_errno(error));
}
