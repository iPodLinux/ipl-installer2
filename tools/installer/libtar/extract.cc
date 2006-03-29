/*
**  Copyright 1998-2003 University of Illinois Board of Trustees
**  Copyright 1998-2003 Mark D. Roth
**  All rights reserved.
**
**  extract.c - libtar code to extract a file from a tar archive
**
**  Mark D. Roth <roth@uiuc.edu>
**  Campus Information Technologies and Educational Services
**  University of Illinois at Urbana-Champaign
*/

#include "libtar.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>


struct linkname
{
	char ln_save[4096];
	char ln_real[4096];
};
typedef struct linkname linkname_t;

#ifdef WIN32
int my_errno;
#define errno my_errno
#endif

static int
tar_set_file_perms(TAR *t, VFS::Filesystem *fs, const char *realname)
{
	mode_t mode;
	uid_t uid;
	gid_t gid;
	const char *filename;

	filename = (realname ? realname : th_get_pathname(t));
	mode = th_get_mode(t);
	uid = th_get_uid(t);
	gid = th_get_gid(t);

        fs->chown (filename, uid, gid);
        fs->chmod (filename, mode & 07777);

	return 0;
}


/* switchboard */
int
tar_extract_file(TAR *t, VFS::Filesystem *fs, const char *realname)
{
	int i;
	linkname_t *lnp;

	if (t->options & TAR_NOOVERWRITE)
	{
		struct my_stat s;
                int ret;

		if ((ret = fs->lstat(realname, &s)) >= 0 || ret != -ENOENT)
		{
			errno = EEXIST;
			return -1;
		}
	}

	if (TH_ISDIR(t))
	{
		i = tar_extract_dir(t, fs, realname);
		if (i == 1)
			i = 0;
	}
	else if (TH_ISLNK(t))
		i = tar_extract_hardlink(t, fs, realname);
	else if (TH_ISSYM(t))
		i = tar_extract_symlink(t, fs, realname);
	else if (TH_ISCHR(t))
		i = tar_extract_chardev(t, fs, realname);
	else if (TH_ISBLK(t))
		i = tar_extract_blockdev(t, fs, realname);
	else if (TH_ISFIFO(t))
		i = tar_extract_fifo(t, fs, realname);
	else /* if (TH_ISREG(t)) */
		i = tar_extract_regfile(t, fs, realname);

	if (i != 0)
		return i;

	i = tar_set_file_perms(t, fs, realname);
	if (i != 0)
		return i;

	lnp = (linkname_t *)calloc(1, sizeof(linkname_t));
	if (lnp == NULL)
		return -1;
	strncpy(lnp->ln_save, th_get_pathname(t), sizeof(lnp->ln_save) - 1);
	strncpy(lnp->ln_real, realname, sizeof(lnp->ln_real) - 1);
        lnp->ln_save[sizeof(lnp->ln_save) - 1] = 0;
        lnp->ln_real[sizeof(lnp->ln_real) - 1] = 0;
#ifdef DEBUG
	printf("tar_extract_file(): calling libtar_hash_add(): key=\"%s\", "
	       "value=\"%s\"\n", th_get_pathname(t), realname);
#endif
	if (libtar_hash_add(t->h, lnp) != 0)
		return -1;

	return 0;
}


/* extract regular file */
int
tar_extract_regfile(TAR *t, VFS::Filesystem *fs, const char *realname)
{
	mode_t mode;
	size_t size;
	uid_t uid;
	gid_t gid;
	VFS::File *fhout;
	int i, k;
	char buf[T_BLOCKSIZE];
	const char *filename;

#ifdef DEBUG
	printf("==> tar_extract_regfile(t=0x%lx, realname=\"%s\")\n", t,
	       realname);
#endif

	if (!TH_ISREG(t))
	{
		errno = EINVAL;
		return -1;
	}

	filename = (realname ? realname : th_get_pathname(t));
	mode = th_get_mode(t);
	size = th_get_size(t);
	uid = th_get_uid(t);
	gid = th_get_gid(t);

	if (mkdirhier(fs, filename) == -1)
		return -1;

#ifdef DEBUG
	printf("  ==> extracting: %s (mode %04o, uid %d, gid %d, %d bytes)\n",
	       filename, mode, uid, gid, size);
#endif
	fhout = fs->open (filename, O_WRONLY | O_CREAT | O_TRUNC);

	if (!fhout || fhout->error())
	{
#ifdef DEBUG
		fprintf(stderr, "open(): %s", fhout? strerror (fhout->error()) : "unknown error");
#endif
                delete fhout;
		return -1;
	}

	/* extract the file */
	for (i = size; i > 0; i -= T_BLOCKSIZE)
	{
		k = tar_block_read(t, buf);
		if (k != T_BLOCKSIZE)
		{
			if (k != -1)
				errno = EINVAL;
                        delete fhout;
			return -1;
		}

		/* write block to output file */
		if (fhout->write(buf,
                                 ((i > T_BLOCKSIZE) ? T_BLOCKSIZE : i)) < 0) {
                    delete fhout;
                    return -1;
                }
	}

	/* close output file */
	if (fhout->close() < 0)
		return -1;
        delete fhout;

#ifdef DEBUG
	printf("### done extracting %s\n", filename);
#endif

	return 0;
}


/* skip regfile */
int
tar_skip_regfile(TAR *t)
{
	int i, k;
	size_t size;
	char buf[T_BLOCKSIZE];

	if (!TH_ISREG(t))
	{
		errno = EINVAL;
		return -1;
	}

	size = th_get_size(t);
	for (i = size; i > 0; i -= T_BLOCKSIZE)
	{
		k = tar_block_read(t, buf);
		if (k != T_BLOCKSIZE)
		{
			if (k != -1)
				errno = EINVAL;
			return -1;
		}
	}

	return 0;
}


/* hardlink */
int
tar_extract_hardlink(TAR * t, VFS::Filesystem *fs, const char *realname)
{
	const char *filename;
	const char *linktgt = NULL;
	linkname_t *lnp;
	libtar_hashptr_t hp;

	if (!TH_ISLNK(t))
	{
		errno = EINVAL;
		return -1;
	}

	filename = (realname ? realname : th_get_pathname(t));
	if (mkdirhier(fs, filename) == -1)
		return -1;
	libtar_hashptr_reset(&hp);
	if (libtar_hash_getkey(t->h, &hp, th_get_linkname(t),
			       (libtar_matchfunc_t)libtar_str_match) != 0)
	{
		lnp = (linkname_t *)libtar_hashptr_data(&hp);
		linktgt = lnp->ln_real;
	}
	else
		linktgt = th_get_linkname(t);

#ifdef DEBUG
	printf("  ==> extracting: %s (link to %s)\n", filename, linktgt);
#endif
	if (fs->link (linktgt, filename) < 0)
	{
#ifdef DEBUG
		perror("link()");
#endif
		return -1;
	}

	return 0;
}


/* symlink */
int
tar_extract_symlink(TAR *t, VFS::Filesystem *fs, const char *realname)
{
	const char *filename;

	if (!TH_ISSYM(t))
	{
		errno = EINVAL;
		return -1;
	}

	filename = (realname ? realname : th_get_pathname(t));
	if (mkdirhier(fs, filename) == -1)
		return -1;

        int ret;
	if ((ret = fs->unlink(filename)) < 0 && ret != -ENOENT)
		return -1;

#ifdef DEBUG
	printf("  ==> extracting: %s (symlink to %s)\n",
	       filename, th_get_linkname(t));
#endif
	if (fs->symlink (th_get_linkname(t), filename) == -1)
	{
#ifdef DEBUG
		perror("symlink()");
#endif
		return -1;
	}

	return 0;
}


/* character device */
int
tar_extract_dev(TAR *t, VFS::Filesystem *fs, const char *realname, int modeflag)
{
    fprintf (stderr, "Warning! Device extraction skipped - not supported\n");
    return -1;

#if 0
	mode_t mode;
	unsigned long devmaj, devmin;
	const char *filename;

	if (!TH_ISCHR(t))
	{
		errno = EINVAL;
		return -1;
	}

	filename = (realname ? realname : th_get_pathname(t));
	mode = th_get_mode(t);
	devmaj = th_get_devmajor(t);
	devmin = th_get_devminor(t);

	if (mkdirhier(fs, filename) == -1)
		return -1;

#ifdef DEBUG
	printf("  ==> extracting: %s (character device %ld,%ld)\n",
	       filename, devmaj, devmin);
#endif
	if (fs->mknod (filename, mode | modeflag, (devmaj << 8) | devmin) < 0)
	{
#ifdef DEBUG
		perror("mknod()");
#endif
		return -1;
	}

	return 0;
#endif
}

int
tar_extract_chardev (TAR *t, VFS::Filesystem *fs, const char *realname) 
{
    return tar_extract_dev (t, fs, realname, S_IFCHR);
}

int
tar_extract_blockdev (TAR *t, VFS::Filesystem *fs, const char *realname) 
{
    return tar_extract_dev (t, fs, realname, S_IFBLK);
}


/* directory */
int
tar_extract_dir(TAR *t, VFS::Filesystem *fs, const char *realname)
{
	mode_t mode;
	const char *filename;

	if (!TH_ISDIR(t))
	{
		errno = EINVAL;
		return -1;
	}

	filename = (realname ? realname : th_get_pathname(t));
	mode = th_get_mode(t);

	if (mkdirhier(fs, filename) == -1)
		return -1;

#ifdef DEBUG
	printf("  ==> extracting: %s (mode %04o, directory)\n", filename,
	       mode);
#endif
        int ret;
	if ((ret = fs->mkdir (filename)) < 0)
	{
		if (ret == -EEXIST)
		{
			if (fs->chmod (filename, mode) < 0)
			{
#ifdef DEBUG
				perror("chmod()");
#endif
				return -1;
			}
			else
			{
#ifdef DEBUG
				puts("  *** using existing directory");
#endif
				return 1;
			}
		}
		else
		{
#ifdef DEBUG
			perror("mkdir()");
#endif
			return -1;
		}
	}

	return 0;
}


/* FIFO */
int
tar_extract_fifo(TAR *t, VFS::Filesystem *fs, const char *realname)
{
    fprintf (stderr, "Warning! FIFO extraction skipped - not supported\n");
}

