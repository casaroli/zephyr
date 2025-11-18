/*
 * Copyright (c) 2025 Marco Casaroli <marco.casaroli@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/fs_sys.h>
#include <zephyr/logging/log.h>

#include "../fs_impl.h"
#include "tmpfs.h"
#include "tmpfs_impl.h"

LOG_MODULE_REGISTER(tmpfs, CONFIG_TMPFS_LOG_LEVEL);
// LOG_MODULE_DECLARE(tmpfs);

#define TMPFS_MAGIC 0x01021994

#if CONFIG_FILE_SYSTEM_TMPFS_DIRECTORY_FREEGUARD <= CONFIG_FILE_SYSTEM_TMPFS_DIRECTORY_ALLOCGUARD
#warning CONFIG_FILE_SYSTEM_TMPFS_DIRECTORY_FREEGUARD needs to be > ALLOCGUARD
#endif

#if CONFIG_FILE_SYSTEM_TMPFS_FILE_FREEGUARD <= CONFIG_FILE_SYSTEM_TMPFS_FILE_ALLOCGUARD
#warning CONFIG_FILE_SYSTEM_TMPFS_FILE_FREEGUARD needs to be > ALLOCGUARD
#endif

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct tmpfs_dir_s {
	struct fs_dirent tf_base;         /* Vfs directory structure */
	struct tmpfs_directory_s *tf_tdo; /* Directory being enumerated */
	unsigned int tf_index;            /* Directory index */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/
/* File system operations */

static int tmpfs_open(struct fs_file_t *filep, const char *relpath, fs_mode_t mode);
static int tmpfs_close(struct fs_file_t *filep);
static ssize_t tmpfs_read(struct fs_file_t *filep, void *buffer, size_t buflen);
static ssize_t tmpfs_write(struct fs_file_t *filep, const void *buffer, size_t buflen);
static int tmpfs_lseek(struct fs_file_t *filep, off_t offset, int whence);
static int tmpfs_sync(struct fs_file_t *filep);
static off_t tmpfs_tell(struct fs_file_t *filp);
static int tmpfs_truncate(struct fs_file_t *filep, off_t length);
static int tmpfs_opendir(struct fs_dir_t *mountpt, const char *fs_path);
static int tmpfs_closedir(struct fs_dir_t *mountpt);
static int tmpfs_readdir(struct fs_dir_t *mountpt, struct fs_dirent *dir);
static int tmpfs_bind(struct fs_mount_t *handle);
static int tmpfs_unbind(struct fs_mount_t *handle);
static int tmpfs_unlink(struct fs_mount_t *mountpt, const char *relpath);
static int tmpfs_mkdir(struct fs_mount_t *mountpt, const char *relpath);
static int tmpfs_rename(struct fs_mount_t *mountpt, const char *oldrelpath, const char *newrelpath);
static void tmpfs_stat_common(struct tmpfs_object_s *to, struct fs_dirent *buf);
static int tmpfs_stat(struct fs_mount_t *mountpt, const char *relpath, struct fs_dirent *buf);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/*
 * Copy src to string dst of size siz.  At most siz-1 characters
 * will be copied.  Always NUL terminates (unless siz == 0).
 * Returns strlen(src); if retval >= siz, truncation occurred.
 */
static size_t strlcpy(char *dst, const char *src, size_t siz)
{
	char *d = dst;
	const char *s = src;
	size_t n = siz;
	/* Copy as many bytes as will fit */
	if (n != 0) {
		while (--n != 0) {
			if ((*d++ = *s++) == '\0') {
				break;
			}
		}
	}
	/* Not enough room in dst, add NUL and traverse rest of src */
	if (n == 0) {
		if (siz != 0) {
			*d = '\0'; /* NUL-terminate dst */
		}
		while (*s++)
			;
	}
	return (s - src - 1); /* count does not include NUL */
}

/****************************************************************************
 * Name: tmpfs_open
 ****************************************************************************/

static int tmpfs_open(struct fs_file_t *filep, const char *relpath, fs_mode_t mode)
{
	struct tmpfs_s *fs;
	struct tmpfs_file_s *tfo;
	off_t offset;
	int ret;

	const char *path = fs_impl_strip_prefix(relpath, filep->mp);

	LOG_DBG("Open '%s' (%s) mode: Rd:%d Wr:%d App:%d Creat:%d", relpath, path,
		(mode & FS_O_READ) != 0, (mode & FS_O_WRITE) != 0, (mode & FS_O_APPEND) != 0,
		(mode & FS_O_CREATE) != 0);

	__ASSERT_NO_MSG(filep->filep == NULL);

	/* Get the mountpoint inode reference from the file structure and the
	 * mountpoint private data from the inode structure
	 */

	fs = filep->mp->fs_data;

	__ASSERT_NO_MSG(fs != NULL && fs->tfs_root.tde_object != NULL);

	LOG_DBG("filep: %p, fs: %p", filep, fs);

	/* Get exclusive access to the file system */

	ret = tmpfs_lock(fs);
	if (ret < 0) {
		return ret;
	}

	/* Skip over any leading directory separators (shouldn't be any) */

	for (; *path == '/'; path++)
		;

	/* Find the file object associated with this relative path.
	 * If successful, this action will lock both the parent directory and
	 * the file object, adding one to the reference count of both.
	 * In the event that -ENOENT, there will still be a reference and
	 * lock on the returned directory.
	 */

	ret = tmpfs_find_file(fs, path, &tfo, NULL);
	if (ret >= 0) {
		/* The file exists.  We hold the lock and one reference count
		 * on the file object.
		 *
		 * It would be an error if we are asked to create it exclusively
		 */

		// if ((oflags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL))
		//   {
		//     /* Already exists -- can't create it exclusively */

		//     ret = -EEXIST;
		//     goto errout_with_filelock;
		//   }

		/* Check if the caller has sufficient privileges to open the file.
		 * REVISIT: No file protection implemented
		 */

		/* If O_TRUNC is specified and the file is opened for writing,
		 * then truncate the file.  This operation requires that the file is
		 * writeable, but we have already checked that. O_TRUNC without write
		 * access is ignored.
		 */

		if ((mode & (FS_O_TRUNC | FS_O_WRITE)) == (FS_O_TRUNC | FS_O_WRITE)) {
			/* Truncate the file to zero length (if it is not already
			 * zero length)
			 */

			if (tfo->tfo_size > 0) {
				ret = tmpfs_realloc_file(tfo, 0);
				if (ret < 0) {
					goto errout_with_filelock;
				}
			}
		}
	}

	/* ENOENT would be returned by tmpfs_find_file() if the full directory
	 * path was found, but the file was not found in the final directory.
	 */

	else if (ret == -ENOENT) {
		/* The file does not exist.  Were we asked to create it? */
		LOG_DBG("File '%s' does not exist, (mode %x)", path, mode);
		if ((mode & FS_O_CREATE) == 0) {
			/* No.. then we fail with -ENOENT */

			LOG_ERR("No creation requested!, %x", mode);

			ret = -ENOENT;
			goto errout_with_fslock;
		}

		/* Yes.. create the file object.  There will be a reference and a lock
		 * on the new file object.
		 */

		ret = tmpfs_create_file(fs, path, &tfo);
		LOG_DBG("tmpfs_create_file %d", ret);

		if (ret < 0) {
			goto errout_with_fslock;
		}
	}

	/* Some other error occurred */

	else {
		goto errout_with_fslock;
	}

	/* Save the struct tmpfs_file_s instance as the file private data */

	filep->filep = tfo;

	/* In write/append mode, we need to set the file pointer to the end of the
	 * file.
	 */

	offset = 0;
	if ((mode & (FS_O_APPEND | FS_O_WRITE)) == (FS_O_APPEND | FS_O_WRITE)) {
		offset = tfo->tfo_size;
	}

	tfo->f_pos = offset;

	/* Unlock the file file object, but retain the reference count */

	tmpfs_unlock_file(tfo);
	tmpfs_unlock(fs);
	return 0;

	/* Error exits */

errout_with_filelock:
	tmpfs_release_lockedfile(tfo);

errout_with_fslock:
	tmpfs_unlock(fs);
	return ret;
}

/****************************************************************************
 * Name: tmpfs_close
 ****************************************************************************/

static int tmpfs_close(struct fs_file_t *filep)
{
	struct tmpfs_file_s *tfo;
	int ret;

	__ASSERT_NO_MSG(filep->filep != NULL);

	tfo = filep->filep;

	ret = tmpfs_release_file(tfo);
	if (ret >= 0) {
		filep->filep = NULL;
	}

	return ret;
}

/****************************************************************************
 * Name: tmpfs_read
 ****************************************************************************/

static ssize_t tmpfs_read(struct fs_file_t *filep, void *buffer, size_t buflen)
{
	struct tmpfs_file_s *tfo;
	ssize_t nread;
	off_t startpos;
	off_t endpos;
	int ret;

	__ASSERT_NO_MSG(filep->filep != NULL);

	/* Recover our private data from the struct fs_file_t instance */

	tfo = filep->filep;

	/* Directly return when the f_pos bigger then tfo_size */

	if (tfo->f_pos > tfo->tfo_size) {
		return 0;
	}

	/* Get exclusive access to the file */

	ret = tmpfs_lock_file(tfo);
	if (ret < 0) {
		return ret;
	}

	/* Handle attempts to read beyond the end of the file. */

	startpos = tfo->f_pos;
	nread = buflen;
	endpos = startpos + buflen;

	if (endpos > tfo->tfo_size) {
		endpos = tfo->tfo_size;
		nread = endpos - startpos;
	}

	/* Copy data from the memory object to the user buffer */

	if (tfo->tfo_data != NULL) {
		memcpy(buffer, &tfo->tfo_data[startpos], nread);
		tfo->f_pos += nread;
	} else {
		__ASSERT_NO_MSG(tfo->tfo_size == 0 && nread == 0);
	}

	/* Release the lock on the file */

	tmpfs_unlock_file(tfo);
	return nread;
}

/****************************************************************************
 * Name: tmpfs_write
 ****************************************************************************/

static ssize_t tmpfs_write(struct fs_file_t *filep, const void *buffer, size_t buflen)
{
	struct tmpfs_file_s *tfo;
	ssize_t nwritten;
	off_t startpos;
	off_t endpos;
	int ret;

	__ASSERT_NO_MSG(filep->filep != NULL);

	/* Recover our private data from the struct fs_file_t instance */

	tfo = filep->filep;

	/* Get exclusive access to the file */

	ret = tmpfs_lock_file(tfo);
	if (ret < 0) {
		return ret;
	}

	/* Handle attempts to write beyond the end of the file */

	if ((filep->flags & FS_O_APPEND) != 0) {
		startpos = tfo->tfo_size;
	} else {
		startpos = tfo->f_pos;
	}

	nwritten = buflen;
	endpos = startpos + buflen;

	LOG_DBG("nwritten: %jd", (intmax_t)nwritten);

	if (endpos > tfo->tfo_size) {
		/* Reallocate the file to handle the write past the end of the file. */

		ret = tmpfs_realloc_file(tfo, (size_t)endpos);
		if (ret < 0) {
			LOG_ERR("tmpfs_realloc_file failed with %d", ret);
			goto errout_with_lock;
		}
	}

	/* Copy data from the memory object to the user buffer */

	if (tfo->tfo_data != NULL) {
		memcpy(&tfo->tfo_data[startpos], buffer, nwritten);
	} else {
		__ASSERT_NO_MSG(tfo->tfo_size == 0 && nwritten == 0);
	}

	tfo->f_pos = endpos;

	/* Release the lock on the file */

	tmpfs_unlock_file(tfo);
	LOG_DBG("will return %d", nwritten);

	return nwritten;

errout_with_lock:
	tmpfs_unlock_file(tfo);
	return (ssize_t)ret;
}

/****************************************************************************
 * Name: tmpfs_lseek
 ****************************************************************************/

static int tmpfs_lseek(struct fs_file_t *filep, off_t offset, int whence)
{
	struct tmpfs_file_s *tfo;
	off_t position;

	__ASSERT_NO_MSG(filep->filep != NULL);

	/* Recover our private data from the struct fs_file_t instance */

	tfo = filep->filep;

	/* Map the offset according to the whence option */

	switch (whence) {
	case SEEK_SET: /* The offset is set to offset bytes. */
		position = offset;
		break;

	case SEEK_CUR: /* The offset is set to its current location plus
			* offset bytes. */
		position = offset + tfo->f_pos;
		break;

	case SEEK_END: /* The offset is set to the size of the file plus
			* offset bytes. */
		position = offset + tfo->tfo_size;
		break;

	default:
		return -EINVAL;
	}

	/* Save the new file position */

	tfo->f_pos = position;
	return 0;
}

/****************************************************************************
 * Name: tmpfs_sync
 ****************************************************************************/

static int tmpfs_sync(struct fs_file_t *filep)
{
	return 0;
}

/****************************************************************************
 * Name: tmpfs_fstat
 *
 * Description:
 *   Obtain information about an open file associated with the file
 *   descriptor 'fd', and will write it to the area pointed to by 'buf'.
 *
 ****************************************************************************/

// static int tmpfs_fstat(const struct fs_file_t *filep, struct fs_dirent *buf)
// {
//   struct tmpfs_file_s *tfo;
//   int ret;

//   LOG_INF("Fstat %p\n", buf);
//   __ASSERT_NO_MSG(buf != NULL);

//   /* Recover our private data from the struct fs_file_t instance */

//   __ASSERT_NO_MSG(filep->filep != NULL);
//   tfo = filep->filep;

//   /* Get exclusive access to the file */

//   ret = tmpfs_lock_file(tfo);
//   if (ret < 0)
//     {
//       return ret;
//     }

//   /* Return information about the file in the stat buffer. */

//   tmpfs_stat_common((struct tmpfs_object_s *)tfo, buf);

//   /* Release the lock on the file and return success. */

//   tmpfs_unlock_file(tfo);
//   return 0;
// }

static off_t tmpfs_tell(struct fs_file_t *filp)
{
	struct tmpfs_file_s *tfo = filp->filep;
	return tfo->f_pos;
}

/****************************************************************************
 * Name: tmpfs_truncate
 ****************************************************************************/

static int tmpfs_truncate(struct fs_file_t *filep, off_t length)
{
	struct tmpfs_file_s *tfo;
	size_t oldsize;
	int ret;

	__ASSERT_NO_MSG(length >= 0);

	/* Recover our private data from the struct fs_file_t instance */

	tfo = filep->filep;

	/* Get exclusive access to the file */

	ret = tmpfs_lock_file(tfo);
	if (ret < 0) {
		return ret;
	}

	/* Get the old size of the file.  Do nothing if the file size is not
	 * changing.
	 */

	oldsize = tfo->tfo_size;
	if (oldsize != length) {
		/* The size is changing.. up or down.  Reallocate the file memory. */

		ret = tmpfs_realloc_file(tfo, (size_t)length);
		if (ret < 0) {
			goto errout_with_lock;
		}

		/* If the size has increased, then we need to zero the newly added
		 * memory.
		 */

		if (length > oldsize) {
			memset(&tfo->tfo_data[oldsize], 0, length - oldsize);
		}

		ret = 0;
	}

	/* Release the lock on the file */

errout_with_lock:
	tmpfs_unlock_file(tfo);
	return ret;
}

/****************************************************************************
 * Name: tmpfs_opendir
 ****************************************************************************/

static int tmpfs_opendir(struct fs_dir_t *mountpt, const char *fs_path)
{
	struct tmpfs_s *fs;
	struct tmpfs_dir_s *tdir;
	struct tmpfs_directory_s *tdo;
	int ret;

	const char *path = fs_impl_strip_prefix(fs_path, mountpt->mp);

	__ASSERT_NO_MSG(mountpt != NULL && path != NULL);

	/* Get the mountpoint private data from the inode structure */

	fs = mountpt->mp->fs_data;
	__ASSERT_NO_MSG(fs != NULL && fs->tfs_root.tde_object != NULL);

	tdir = k_malloc(sizeof(*tdir));
	if (tdir == NULL) {
		LOG_ERR("Failed to allocate memory for directory");
		return -ENOMEM;
	}

	memset(tdir, 0, sizeof(*tdir));

	/* Get exclusive access to the file system */

	ret = tmpfs_lock(fs);
	if (ret < 0) {
		k_free(tdir);
		return ret;
	}

	/* Skip over any leading directory separators (shouldn't be any) */

	for (; *path == '/'; path++)
		;

	/* Find the directory object associated with this relative path.
	 * If successful, this action will lock both the parent directory and
	 * the file object, adding one to the reference count of both.
	 * In the event that -ENOENT, there will still be a reference and
	 * lock on the returned directory.
	 */

	ret = tmpfs_find_directory(fs, path, strlen(path), &tdo, NULL);
	if (ret >= 0) {
		LOG_DBG("Directory found");
		tdir->tf_tdo = tdo;
		tdir->tf_index = tdo->tdo_nentries;

		tmpfs_unlock_directory(tdo);
	} else {
		LOG_DBG("Directory NOT found");
	}
	/* Release the lock on the file system and return the result */

	tmpfs_unlock(fs);
	mountpt->dirp = tdir;

	return ret;
}

/****************************************************************************
 * Name: tmpfs_closedir
 ****************************************************************************/

static int tmpfs_closedir(struct fs_dir_t *mountpt)
{
	struct tmpfs_directory_s *tdo;

	__ASSERT_NO_MSG(mountpt != NULL && dir != NULL);

	/* Get the directory structure from the dir argument */

	tdo = mountpt->dirp;
	__ASSERT_NO_MSG(tdo != NULL);

	/* Decrement the reference count on the directory object */

	tmpfs_lock_directory(tdo);
	tdo->tdo_refs--;
	tmpfs_unlock_directory(tdo);
	// k_free(dir);
	return 0;
}

/****************************************************************************
 * Name: tmpfs_readdir
 ****************************************************************************/

static int tmpfs_readdir(struct fs_dir_t *mountpt, struct fs_dirent *dir)
{
	struct tmpfs_directory_s *tdo;
	struct tmpfs_dir_s *tdir;
	unsigned int index;
	int ret;

	__ASSERT_NO_MSG(mountpt != NULL && dir != NULL);

	/* Get the directory structure from the dir argument and lock it */

	tdir = (struct tmpfs_dir_s *)mountpt->dirp;
	tdo = tdir->tf_tdo;
	__ASSERT_NO_MSG(tdo != NULL);

	tmpfs_lock_directory(tdo);

	/* Have we reached the end of the directory? */

	index = tdir->tf_index;

	if (index-- == 0) {
		LOG_DBG("End of directory");
		dir->name[0] = 0;
		ret = 0;
	} else {
		struct tmpfs_dirent_s *tde;
		struct tmpfs_object_s *to;

		/* Does this entry refer to a file or a directory object? */

		tde = &tdo->tdo_entry[index];
		to = tde->tde_object;
		__ASSERT_NO_MSG(to != NULL);

		if (to->to_type == TMPFS_DIRECTORY) {
			dir->type = FS_DIR_ENTRY_DIR;
		} else /* to->to_type == TMPFS_REGULAR) */
		{
			dir->type = FS_DIR_ENTRY_FILE;
		}

		/* Copy the entry name */

		strlcpy(dir->name, tde->tde_name, sizeof(dir->name));

		/* Save the index for next time */

		tdir->tf_index = index;
		ret = 0;
	}

	tmpfs_unlock_directory(tdo);

	return ret;
}

/****************************************************************************
 * Name: tmpfs_bind
 ****************************************************************************/

static int tmpfs_bind(struct fs_mount_t *handle)
{
	struct tmpfs_directory_s *tdo;
	struct tmpfs_s *fs;

	LOG_INF("MOUNT! handle: %p", handle);
	__ASSERT_NO_MSG(handle != NULL);

	/* Create an instance of the tmpfs file system */

	fs = k_malloc(sizeof(struct tmpfs_s));
	if (fs == NULL) {
		return -ENOMEM;
	}

	LOG_INF("MOUNT! fs: %p", fs);

	memset(fs, 0, sizeof(struct tmpfs_s));

	/* Create a root file system.  This is like a single directory entry in
	 * the file system structure.
	 */

	tdo = tmpfs_alloc_directory(NULL);
	if (tdo == NULL) {
		k_free(fs);
		return -ENOMEM;
	}

	fs->tfs_root.tde_object = (struct tmpfs_object_s *)tdo;
	fs->tfs_root.tde_name = "";

	/* Initialize the file system state */

	k_mutex_init(&fs->tfs_lock);

	/* Return the new file system handle */

	handle->fs_data = fs;
	return 0;
}

/****************************************************************************
 * Name: tmpfs_unbind
 ****************************************************************************/

static int tmpfs_unbind(struct fs_mount_t *handle)
{
	struct tmpfs_s *fs = (struct tmpfs_s *)handle;
	struct tmpfs_directory_s *tdo;
	int ret;

	LOG_INF("UMOUNT! handle: %p", handle);
	__ASSERT_NO_MSG(fs != NULL && fs->tfs_root.tde_object != NULL);

	/* Lock the file system */

	ret = tmpfs_lock(fs);
	if (ret < 0) {
		return ret;
	}

	/* Traverse all directory entries (recursively), freeing all resources. */

	tdo = (struct tmpfs_directory_s *)fs->tfs_root.tde_object;
	ret = tmpfs_foreach(tdo, tmpfs_free_callout, NULL);

	/* Now we can destroy the root file system and the file system itself. */

	// nxrmutex_destroy(&tdo->tdo_lock);
	k_free(tdo->tdo_entry);
	k_free(tdo);

	// nxrmutex_destroy(&fs->tfs_lock);
	k_free(fs);
	return ret;
}

/****************************************************************************
 * Name: tmpfs_statfs
 ****************************************************************************/

// static int tmpfs_statfs(struct fs_dir_t *mountpt, struct fs_statvfs *buf)
// {
//   struct tmpfs_s *fs;
//   struct tmpfs_directory_s *tdo;
//   struct tmpfs_statfs_s tmpbuf;
//   size_t avail;
//   off_t blkalloc;
//   off_t blkavail;
//   int ret;

//   LOG_INF("mountpt: %p buf: %p\n", mountpt, buf);
//   __ASSERT_NO_MSG(mountpt != NULL && buf != NULL);

//   /* Get the file system structure from the inode reference. */

//   fs = mountpt->dirp;
//   __ASSERT_NO_MSG(fs != NULL && fs->tfs_root.tde_object != NULL);

//   /* Get exclusive access to the file system */

//   ret = tmpfs_lock(fs);
//   if (ret < 0)
//     {
//       return ret;
//     }

//   /* Set up the memory use for the file system and root directory object */

//   tdo              = (struct tmpfs_directory_s *)fs->tfs_root.tde_object;
//   avail            = tdo->tdo_alloc -
//                      SIZEOF_TMPFS_DIRECTORY(tdo->tdo_nentries);

//   tmpbuf.tsf_alloc = sizeof(struct tmpfs_s) +
//                      sizeof(struct tmpfs_directory_s) +
//                      tdo->tdo_alloc;
//   tmpbuf.tsf_avail = avail;
//   tmpbuf.tsf_files = 0;
//   tmpbuf.tsf_ffree = avail / sizeof(struct tmpfs_dirent_s);

//   /* Traverse the file system to accurmulate statistics */

//   ret = tmpfs_foreach(tdo, tmpfs_statfs_callout, (void *)&tmpbuf);
//   if (ret < 0)
//     {
//       return -ECANCELED;
//     }

//   /* Return something for the file system description */

//   blkalloc        = (tmpbuf.tsf_alloc + CONFIG_FILE_SYSTEM_TMPFS_BLOCKSIZE - 1) /
//                      CONFIG_FILE_SYSTEM_TMPFS_BLOCKSIZE;
//   blkavail        = (tmpbuf.tsf_avail + CONFIG_FILE_SYSTEM_TMPFS_BLOCKSIZE - 1) /
//                      CONFIG_FILE_SYSTEM_TMPFS_BLOCKSIZE;

//   // buf->f_type     = TMPFS_MAGIC;
//   // buf->f_namelen  = NAME_MAX;
//   buf->f_bsize    = CONFIG_FILE_SYSTEM_TMPFS_BLOCKSIZE;
//   buf->f_frsize    = CONFIG_FILE_SYSTEM_TMPFS_BLOCKSIZE;
//   buf->f_blocks   = blkalloc;
//   buf->f_bfree    = blkavail;
//   // buf->f_bavail   = blkavail;
//   // buf->f_files    = tmpbuf.tsf_files;
//   // buf->f_ffree    = tmpbuf.tsf_ffree;

//   /* Release the lock on the file system */

//   tmpfs_unlock(fs);
//   return 0;
// }

/****************************************************************************
 * Name: tmpfs_unlink
 ****************************************************************************/

static int tmpfs_unlink(struct fs_mount_t *mountpt, const char *relpath)
{
	struct tmpfs_s *fs;
	struct tmpfs_directory_s *tdo;
	struct tmpfs_object_s *to = NULL;
	const char *name;
	int ret;

	const char *path = fs_impl_strip_prefix(relpath, mountpt);

	for (; *path == '/'; path++)
		;

	// LOG_INF("mountpt: %p relpath: %s, path: %s\n", mountpt, relpath, path);
	__ASSERT_NO_MSG(mountpt != NULL && relpath != NULL);

	/* Get the file system structure from the inode reference. */

	fs = mountpt->fs_data;
	__ASSERT_NO_MSG(fs != NULL && fs->tfs_root.tde_object != NULL);

	/* Get exclusive access to the file system */

	ret = tmpfs_lock(fs);
	if (ret < 0) {
		return ret;
	}

	/* Find the file object and parent directory associated with this relative
	 * path.  If successful, tmpfs_find_file will lock both the file object
	 * and the parent directory and take one reference count on each.
	 */

	ret = tmpfs_find_object(fs, path, strlen(path), &to, &tdo);
	if (ret < 0) {
		goto errout_with_lock;
	}

	__ASSERT_NO_MSG(tfo != NULL);

	/* If the reference count is not one, then just mark the file as
	 * unlinked
	 */

	if (to->to_type == TMPFS_REGULAR) {
		struct tmpfs_file_s *tfo = (struct tmpfs_file_s *)to;

		/* Get the file name from the relative path */
		name = strrchr(relpath, '/');
		if (name != NULL) {
			LOG_DBG("Got name file name: %s", name);
			/* Skip over the file '/' character */

			name++;
		} else {
			/* The name must lie in the root directory */
			name = relpath;
		}

		/* Remove the file from parent directory */

		ret = tmpfs_remove_dirent(tdo, name);
		if (ret < 0) {
			goto errout_with_objects;
		}

		if (tfo->tfo_refs > 1) {
			/* Make the file object as unlinked */

			tfo->tfo_flags |= TFO_FLAG_UNLINKED;

			/* Release the reference count on the file object */

			tfo->tfo_refs--;
			tmpfs_unlock_file(tfo);
		}

		/* Otherwise we can free the object now */

		else {
			// nxrmutex_destroy(&tfo->tfo_lock);
			k_free(tfo->tfo_data);
			k_free(tfo);
		}
	} else /* if (to->to_type == TMPFS_DIRECTORY) */ {
		struct tmpfs_directory_s *child_tdo = (struct tmpfs_directory_s *)to;

		if (child_tdo->tdo_nentries > 0 || child_tdo->tdo_refs > 1) {
			ret = -EBUSY;
			goto errout_with_objects;
		}

		/* Get the directory name from the relative path */

		name = strrchr(relpath, '/');
		if (name && name[1] == '\0') {
			/* Ignore the tail '/' */
			name = memrchr(relpath, '/', name - relpath);
		}

		if (name != NULL) {
			/* Skip over the fidirectoryle '/' character */
			name++;
		} else {
			/* The name must lie in the root directory */
			name = relpath;
		}
		LOG_DBG("REMOVE DIR name is %s", name);

		/* Remove the directory from parent directory */

		ret = tmpfs_remove_dirent(tdo, name);
		if (ret < 0) {
			LOG_ERR("Failed to remove directory %s", name);
			goto errout_with_objects;
		}

		/* Free the directory object */
		k_free(child_tdo->tdo_entry);
		k_free(child_tdo);
	}

	/* Release the reference and lock on the parent directory */

	tdo->tdo_refs--;
	LOG_DBG("parent ref count tdo->tdo_refs is %d", tdo->tdo_refs);
	tmpfs_unlock_directory(tdo);
	tmpfs_unlock(fs);

	return 0;

errout_with_objects:
	tmpfs_release_lockedobject(to);

	tdo->tdo_refs--;
	tmpfs_unlock_directory(tdo);

errout_with_lock:
	tmpfs_unlock(fs);
	return ret;
}

/****************************************************************************
 * Name: tmpfs_mkdir
 ****************************************************************************/

static int tmpfs_mkdir(struct fs_mount_t *mountpt, const char *path)
{
	struct tmpfs_s *fs;
	int ret;

	const char *relpath = fs_impl_strip_prefix(path, mountpt);
	for (; *relpath == '/'; relpath++)
		;

	__ASSERT_NO_MSG(mountpt != NULL && relpath != NULL);

	/* Get the file system structure from the inode reference. */

	fs = mountpt->fs_data;
	__ASSERT_NO_MSG(fs != NULL && fs->tfs_root.tde_object != NULL);

	/* Get exclusive access to the file system */

	ret = tmpfs_lock(fs);
	if (ret < 0) {
		return ret;
	}

	/* Create the directory. */

	ret = tmpfs_create_directory(fs, relpath, NULL);
	tmpfs_unlock(fs);
	return ret;
}

/****************************************************************************
 * Name: tmpfs_rename
 ****************************************************************************/

static int tmpfs_rename(struct fs_mount_t *mountpt, const char *oldrelpath, const char *newrelpath)
{
	struct tmpfs_directory_s *oldparent;
	struct tmpfs_directory_s *newparent;
	struct tmpfs_object_s *to;
	struct tmpfs_s *fs;
	const char *oldname;
	const char *newname;
	int ret;

	LOG_INF("mountpt: %p oldrelpath: %s newrelpath: %s\n", mountpt, oldrelpath, newrelpath);
	__ASSERT_NO_MSG(mountpt != NULL && oldrelpath != NULL && newrelpath != NULL);

	/* Get the file system structure from the inode reference. */

	fs = mountpt->fs_data;
	__ASSERT_NO_MSG(fs != NULL && fs->tfs_root.tde_object != NULL);

	/* Get exclusive access to the file system */

	ret = tmpfs_lock(fs);
	if (ret < 0) {
		return ret;
	}

	/* Separate the new path into the new file name and the path to the new
	 * parent directory.
	 */

	newname = strrchr(newrelpath, '/');
	if (newname && newname[1] == '\0') {
		/* Ignore the tail '/' */

		newname = memrchr(newrelpath, '/', newname - newrelpath);
	}

	if (newname == NULL) {
		/* No subdirectories... use the root directory */

		newname = newrelpath;
		newparent = (struct tmpfs_directory_s *)fs->tfs_root.tde_object;

		tmpfs_lock_directory(newparent);
		newparent->tdo_refs++;
	} else {
		/* Locate the parent directory that should contain this name.
		 * On success, tmpfs_find_directory() will lockthe parent
		 * directory and increment the reference count.
		 */

		ret = tmpfs_find_directory(fs, newrelpath, newname - newrelpath, &newparent, NULL);
		if (ret < 0) {
			goto errout_with_lock;
		}

		/* Skip the '/' path separator */

		newname++;
	}

	/* Verify that no object of this name already exists in the destination
	 * directory.
	 */

	ret = tmpfs_find_dirent(newparent, newname, strlen(newname));
	if (ret != -ENOENT) {
		/* Something with this name already exists in the directory.
		 * OR perhaps some fatal error occurred.
		 */

		if (ret >= 0) {
			ret = -EEXIST;
		}

		goto errout_with_newparent;
	}

	/* Find the old object at oldpath.  If successful, tmpfs_find_object()
	 * will lock both the object and the parent directory and will increment
	 * the reference count on both.
	 */

	ret = tmpfs_find_object(fs, oldrelpath, strlen(oldrelpath), &to, &oldparent);
	if (ret < 0) {
		goto errout_with_newparent;
	}

	/* Get the old file name from the relative path */

	oldname = strrchr(oldrelpath, '/');
	if (oldname && oldname[1] == '\0') {
		/* Ignore the tail '/' */

		oldname = memrchr(oldrelpath, '/', oldname - oldrelpath);
	}

	if (oldname != NULL) {
		/* Skip over the file '/' character */

		oldname++;
	} else {
		/* The name must lie in the root directory */

		oldname = oldrelpath;
	}

	/* Remove the entry from the parent directory */

	ret = tmpfs_remove_dirent(oldparent, oldname);
	if (ret < 0) {
		goto errout_with_oldparent;
	}

	/* Add an entry to the new parent directory. */

	ret = tmpfs_add_dirent(newparent, to, newname);

errout_with_oldparent:
	oldparent->tdo_refs--;
	tmpfs_unlock_directory(oldparent);

	tmpfs_release_lockedobject(to);

errout_with_newparent:
	newparent->tdo_refs--;
	tmpfs_unlock_directory(newparent);

errout_with_lock:
	tmpfs_unlock(fs);
	return ret;
}

/****************************************************************************
 * Name: tmpfs_stat_common
 ****************************************************************************/

static void tmpfs_stat_common(struct tmpfs_object_s *to, struct fs_dirent *ent)
{
	size_t objsize;

	/* Is the tmpfs object a regular file? */

	LOG_DBG("to: %p", to);

	memset(ent, 0, sizeof(struct fs_dirent));

	if (to->to_type == TMPFS_REGULAR) {
		struct tmpfs_file_s *tfo = (struct tmpfs_file_s *)to;

		LOG_DBG("regular file: %p", tfo);

		/* Get the size of the object */

		objsize = tfo->tfo_size;

		ent->type = FS_DIR_ENTRY_FILE;

	} else /* if (to->to_type == TMPFS_DIRECTORY) */
	{
		struct tmpfs_directory_s *tdo = (struct tmpfs_directory_s *)to;

		/* Get the size of the object */
		objsize = SIZEOF_TMPFS_DIRECTORY(tdo->tdo_nentries);

		ent->type = FS_DIR_ENTRY_DIR;
	}

	/* Fake the rest of the information */

	ent->size = objsize;
}

/****************************************************************************
 * Name: tmpfs_stat
 ****************************************************************************/

static int tmpfs_stat(struct fs_mount_t *mountpt, const char *path, struct fs_dirent *ent)
{
	struct tmpfs_s *fs;
	struct tmpfs_object_s *to;
	int ret;

	const char *relpath = fs_impl_strip_prefix(path, mountpt);
	for (; *relpath == '/'; relpath++)
		;

	__ASSERT_NO_MSG(mountpt != NULL && relpath != NULL && buf != NULL);

	/* Get the file system structure from the inode reference. */

	fs = mountpt->fs_data;
	__ASSERT_NO_MSG(fs != NULL && fs->tfs_root.tde_object != NULL);

	/* Get exclusive access to the file system */

	ret = tmpfs_lock(fs);
	if (ret < 0) {
		return ret;
	}

	/* Find the tmpfs object at the relpath.  If successful,
	 * tmpfs_find_object() will lock the object and increment the
	 * reference count on the object.
	 */

	ret = tmpfs_find_object(fs, relpath, strlen(relpath), &to, NULL);
	if (ret < 0) {
		LOG_ERR("Failed to find object at relpath %s", relpath);
		goto errout_with_fslock;
	}

	/* We found it... Return information about the file object in the stat
	 * buffer.
	 */

	__ASSERT_NO_MSG(to != NULL);
	tmpfs_stat_common(to, ent);

	ent->name[0] = '/';
	memcpy(ent->name + 1, relpath, strlen(relpath));
	ent->name[strlen(relpath) + 1] = '\0';

	/* Unlock the object and return success */

	tmpfs_release_lockedobject(to);
	ret = 0;

errout_with_fslock:
	tmpfs_unlock(fs);
	return ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

static const struct fs_file_system_t tmpfs_fs = {
	.open = tmpfs_open,
	.close = tmpfs_close,
	.read = tmpfs_read,
	.write = tmpfs_write,
	.lseek = tmpfs_lseek,
	.tell = tmpfs_tell,
	.truncate = tmpfs_truncate,
	.sync = tmpfs_sync,
	.mkdir = tmpfs_mkdir,
	.opendir = tmpfs_opendir,
	.readdir = tmpfs_readdir,
	.closedir = tmpfs_closedir,
	.mount = tmpfs_bind,
	.unmount = tmpfs_unbind,
	.unlink = tmpfs_unlink,
	.rename = tmpfs_rename,
	.stat = tmpfs_stat,
	// .statvfs = tmpfs_statvfs,
	// #if defined(CONFIG_FILE_SYSTEM_MKFS)
	// 	.mkfs = tmpfs_mkfs,
	// #endif
};

#define DT_DRV_COMPAT zephyr_fstab_tmpfs

#define DEFINE_FS(inst)                                                                            \
	struct fs_mount_t FS_FSTAB_ENTRY(DT_DRV_INST(inst)) = {                                    \
		.type = FS_TMPFS,                                                                  \
		.mnt_point = DT_INST_PROP(inst, mount_point),                                      \
		.storage_dev = NULL,                                                               \
		.flags = FSTAB_ENTRY_DT_MOUNT_FLAGS(DT_DRV_INST(inst)),                            \
	};

DT_INST_FOREACH_STATUS_OKAY(DEFINE_FS);

#ifdef CONFIG_TMPFS_FSTAB_AUTOMOUNT
#define REFERENCE_MOUNT(inst) (&FS_FSTAB_ENTRY(DT_DRV_INST(inst))),

static void automount_if_enabled(struct fs_mount_t *mountp)
{
	int ret = 0;

	if ((mountp->flags & FS_MOUNT_FLAG_AUTOMOUNT) != 0) {
		ret = fs_mount(mountp);
		if (ret < 0) {
			LOG_ERR("Error mounting filesystem: at %s: %d", mountp->mnt_point, ret);
		} else {
			LOG_DBG("TMPFS Filesystem \"%s\" initialized", mountp->mnt_point);
		}
	}
}
#endif /* CONFIG_EXT2_FSTAB_AUTOMOUNT */

#ifdef CONFIG_TMPFS_TMP_AUTOMOUNT
static struct fs_mount_t tmpfs_mount = {
   	.type = FS_TMPFS,
   	.mnt_point = "/tmp",
    .flags = FS_MOUNT_FLAG_AUTOMOUNT,
};
#endif /* CONFIG_TMPFS_TMP_AUTOMOUNT */

static int tmpfs_init(void)
{
	int rc = fs_register(FS_TMPFS, &tmpfs_fs);

	if (rc < 0) {
		LOG_WRN("tmpfs register error (%d)\n", rc);
	} else {
		LOG_DBG("tmpfs registered\n");
	}

	if (rc == 0) {
#ifdef CONFIG_TMPFS_TMP_AUTOMOUNT
		automount_if_enabled(&tmpfs_mount);
#endif /* CONFIG_TMPFS_TMP_AUTOMOUNT */

#ifdef CONFIG_TMPFS_FSTAB_AUTOMOUNT
		struct fs_mount_t *partitions[] = {DT_INST_FOREACH_STATUS_OKAY(REFERENCE_MOUNT)};

		for (size_t i = 0; i < ARRAY_SIZE(partitions); i++) {
			LOG_DBG("auto mounting %d\n", i);

			struct fs_mount_t *mpi = partitions[i];

			automount_if_enabled(mpi);
		}
#endif /* CONFIG_TMPFS_FSTAB_AUTOMOUNT */
	}

	return rc;
}

SYS_INIT(tmpfs_init, POST_KERNEL, CONFIG_FILE_SYSTEM_INIT_PRIORITY);
