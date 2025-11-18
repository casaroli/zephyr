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

// #include "../fs_impl.h"
#include "tmpfs.h"
#include "tmpfs_impl.h"

LOG_MODULE_DECLARE(tmpfs);

// static char *strndup(const char *s, size_t n)
// {
// 	size_t len = strnlen(s, n);
// 	char *new = k_malloc(len + 1);

// 	if (new == NULL) {
// 		return NULL;
// 	}

// 	new[len] = '\0';
// 	return memcpy(new, s, len);
// }

static void *my_memrchr(const void *m, int c, size_t n)
{
	const unsigned char *s = m;
	c = (unsigned char)c;
	while (n--) {
		if (s[n] == c) {
			return (void *)(s + n);
		}
	}
	return 0;
}

/****************************************************************************
 * Name: tmpfs_realloc_directory
 ****************************************************************************/

int tmpfs_realloc_directory(struct tmpfs_directory_s *tdo, unsigned int nentries)
{
	struct tmpfs_dirent_s *newentry;
	size_t objsize;
	int ret = tdo->tdo_nentries;

	/* Get the new object size */

	objsize = SIZEOF_TMPFS_DIRECTORY(nentries);
	if (objsize <= tdo->tdo_alloc) {
		/* Already big enough.
		 * REVISIT: Missing logic to shrink directory objects.
		 */

		tdo->tdo_nentries = nentries;
		return ret;
	}

	/* Added some additional amount to the new size to account frequent
	 * reallocations.
	 */

	objsize += CONFIG_FILE_SYSTEM_TMPFS_DIRECTORY_ALLOCGUARD;

	/* Realloc the directory object */

	newentry = k_realloc(tdo->tdo_entry, objsize);
	if (newentry == NULL) {
		return -ENOMEM;
	}

	/* Return the new address of the reallocated directory object */

	tdo->tdo_alloc = objsize;
	tdo->tdo_nentries = nentries;
	tdo->tdo_entry = newentry;

	/* Return the index to the first, newly allocated directory entry */

	return ret;
}

/****************************************************************************
 * Name: tmpfs_realloc_file
 ****************************************************************************/

int tmpfs_realloc_file(struct tmpfs_file_s *tfo, size_t newsize)
{
	uint8_t *newdata;
	size_t allocsize;
	size_t delta;

	LOG_DBG("tfo: %p, new size: %d", tfo, newsize);

	/* Are we growing or shrinking the object? */

	if (newsize <= tfo->tfo_alloc) {
		/* Shrinking ... Shrink unconditionally if the size is shrinking to
		 * zero.
		 */

		if (newsize == 0) {
			/* Free the file object */

			k_free(tfo->tfo_data);
			tfo->tfo_data = NULL;
			tfo->tfo_alloc = 0;
			tfo->tfo_size = 0;
			return 0;
		} else if (newsize > 0) {
			/* Otherwise, don't realloc unless the object has shrunk by a
			 * lot.
			 */

			delta = tfo->tfo_alloc - newsize;

			/* We should make sure the shrunked memory be zero */

			memset(tfo->tfo_data + newsize, 0, delta);
			if (delta <= CONFIG_FILE_SYSTEM_TMPFS_FILE_FREEGUARD) {
				/* Hasn't shrunk enough.. Return doing nothing for now */

				tfo->tfo_size = newsize;
				return 0;
			}
		}
	}

	/* Added some additional amount to the new size to account frequent
	 * reallocations.
	 */

	allocsize = newsize + CONFIG_FILE_SYSTEM_TMPFS_FILE_ALLOCGUARD;
	if (allocsize < newsize) {
		/* There must have been an integer overflow */
		LOG_ERR("Integer overflow occurred");
		return -ENOMEM;
	}

	/* Realloc the file object */

	newdata = k_realloc(tfo->tfo_data, allocsize);
	if (newdata == NULL) {
		LOG_ERR("Failed to reallocate memory size: %d", allocsize);
		return -ENOMEM;
	}

	/* Return the new address of the reallocated file object */

	tfo->tfo_alloc = allocsize;
	tfo->tfo_size = newsize;
	tfo->tfo_data = newdata;
	return 0;
}

/****************************************************************************
 * Name: tmpfs_release_lockedobject
 ****************************************************************************/

void tmpfs_release_lockedobject(struct tmpfs_object_s *to)
{
	__ASSERT_NO_MSG(to && to->to_refs > 0);

	/* Is this a file object? */

	if (to->to_type == TMPFS_REGULAR) {
		tmpfs_release_lockedfile((struct tmpfs_file_s *)to);
	} else {
		to->to_refs--;
		tmpfs_unlock_object(to);
	}
}

/****************************************************************************
 * Name: tmpfs_release_lockedfile
 ****************************************************************************/

void tmpfs_release_lockedfile(struct tmpfs_file_s *tfo)
{
	__ASSERT_NO_MSG(tfo && tfo->tfo_refs > 0);

	/* If there are no longer any references to the file and the file has been
	 * unlinked from its parent directory, then free the file object now.
	 */

	if (tfo->tfo_refs == 1 && (tfo->tfo_flags & TFO_FLAG_UNLINKED) != 0) {
		tmpfs_unlock_file(tfo);
		// nxrmutex_destroy(&tfo->tfo_lock);
		k_free(tfo->tfo_data);
		k_free(tfo);
	}

	/* Otherwise, just decrement the reference count on the file object */

	else {
		tfo->tfo_refs--;
		tmpfs_unlock_file(tfo);
	}
}

/****************************************************************************
 * Name: tmpfs_release_file
 ****************************************************************************/

int tmpfs_release_file(struct tmpfs_file_s *tfo)
{
	int ret;

	__ASSERT_NO_MSG(tfo);

	/* Get exclusive access to the file */

	ret = tmpfs_lock_file(tfo);
	if (ret < 0) {
		return ret;
	}

	tmpfs_release_lockedfile(tfo);
	return 0;
}

/****************************************************************************
 * Name: tmpfs_find_dirent
 ****************************************************************************/

int tmpfs_find_dirent(struct tmpfs_directory_s *tdo, const char *name, size_t len)
{
	int i;

	if (len == 0) {
		return -EINVAL;
	} else if (name[len - 1] == '/') {
		/* Ignore the tail '/' */

		if (--len == 0) {
			return -EINVAL;
		}
	}

	/* Search the list of directory entries for a match */

	for (i = 0; i < tdo->tdo_nentries && (strncmp(tdo->tdo_entry[i].tde_name, name, len) != 0 ||
					      tdo->tdo_entry[i].tde_name[len] != 0);
	     i++)
		;

	/* Return what we found, if anything */

	return i < tdo->tdo_nentries ? i : -ENOENT;
}

/****************************************************************************
 * Name: tmpfs_remove_dirent
 ****************************************************************************/

int tmpfs_remove_dirent(struct tmpfs_directory_s *tdo, const char *name)
{
	int index;
	int last;

	/* Search the list of directory entries for a match */

	index = tmpfs_find_dirent(tdo, name, strlen(name));
	if (index < 0) {
		return index;
	}

	/* Free the object name */

	if (tdo->tdo_entry[index].tde_name != NULL) {
		k_free(tdo->tdo_entry[index].tde_name);
	}

	/* Remove by replacing this entry with the final directory entry */

	last = tdo->tdo_nentries - 1;
	if (index != last) {
		tdo->tdo_entry[index] = tdo->tdo_entry[last];
	}

	/* And decrement the count of directory entries */

	tdo->tdo_nentries = last;
	return 0;
}

/****************************************************************************
 * Name: tmpfs_add_dirent
 ****************************************************************************/

int tmpfs_add_dirent(struct tmpfs_directory_s *tdo, struct tmpfs_object_s *to, const char *name)
{
	struct tmpfs_dirent_s *tde;
	char *newname;
	unsigned int nentries;
	size_t namelen;
	int index;

	/* Copy the name string so that it will persist as long as the
	 * directory entry.
	 */

	namelen = strlen(name);
	if (namelen == 0) {
		return -EINVAL;
	} else if (name[namelen - 1] == '/') {
		/* Don't copy the tail '/' */

		if (--namelen == 0) {
			return -EINVAL;
		}
	}

	newname = strndup(name, namelen);
	if (newname == NULL) {
		return -ENOMEM;
	}

	/* Get the new number of entries */

	nentries = tdo->tdo_nentries + 1;

	/* Reallocate the directory object (if necessary) */

	index = tmpfs_realloc_directory(tdo, nentries);
	if (index < 0) {
		k_free(newname);
		return index;
	}

	/* Save the new object info in the new directory entry */

	to->to_parent = tdo;
	tde = &tdo->tdo_entry[index];
	tde->tde_object = to;
	tde->tde_name = newname;

	return 0;
}

/****************************************************************************
 * Name: tmpfs_alloc_file
 ****************************************************************************/

struct tmpfs_file_s *tmpfs_alloc_file(struct tmpfs_directory_s *parent)
{
	struct tmpfs_file_s *tfo;

	/* Create a new zero length file object */

	tfo = k_malloc(sizeof(*tfo));
	if (tfo == NULL) {
		return NULL;
	}

	memset(tfo, 0, sizeof(*tfo));

	/* Initialize the new file object.  NOTE that the initial state is
	 * locked with one reference count.
	 */

	tfo->tfo_alloc = 0;
	tfo->tfo_type = TMPFS_REGULAR;
	tfo->tfo_refs = 1;
	tfo->tfo_parent = parent;
	tfo->tfo_flags = 0;
	tfo->tfo_size = 0;
	tfo->tfo_data = NULL;

	k_mutex_init(&tfo->tfo_lock);
	tmpfs_lock_file(tfo);

	return tfo;
}

/****************************************************************************
 * Name: tmpfs_create_file
 ****************************************************************************/

int tmpfs_create_file(struct tmpfs_s *fs, const char *relpath, struct tmpfs_file_s **tfo)
{
	struct tmpfs_directory_s *parent;
	struct tmpfs_file_s *newtfo;
	const char *name;
	int ret;

	/* Separate the path into the file name and the path to the parent
	 * directory.
	 */

	name = strrchr(relpath, '/');
	if (name == NULL) {
		/* No subdirectories... use the root directory */

		name = relpath;
		parent = (struct tmpfs_directory_s *)fs->tfs_root.tde_object;

		/* Lock the root directory to emulate the behavior of
		 * tmpfs_find_directory()
		 */

		ret = tmpfs_lock_directory(parent);
		if (ret < 0) {
			return ret;
		}

		parent->tdo_refs++;
	} else if (name[1] != '\0') {
		/* Locate the parent directory that should contain this name.
		 * On success, tmpfs_find_directory() will lock the parent
		 * directory and increment the reference count.
		 */

		ret = tmpfs_find_directory(fs, relpath, name - relpath, &parent, NULL);
		if (ret < 0) {
			return ret;
		}

		/* Skip the '/' path separator */

		name++;
	} else {
		return -EISDIR;
	}

	/* Verify that no object of this name already exists in the directory */

	ret = tmpfs_find_dirent(parent, name, strlen(name));
	if (ret != -ENOENT) {
		/* Something with this name already exists in the directory.
		 * OR perhaps some fatal error occurred.
		 */

		if (ret >= 0) {
			ret = -EEXIST;
		}

		goto errout_with_parent;
	}

	/* Allocate an empty file.  The initial state of the file is locked with
	 * one reference count.
	 */

	newtfo = tmpfs_alloc_file(parent);
	if (newtfo == NULL) {
		ret = -ENOMEM;
		goto errout_with_parent;
	}

	/* Then add the new, empty file to the directory */

	ret = tmpfs_add_dirent(parent, (struct tmpfs_object_s *)newtfo, name);
	if (ret < 0) {
		goto errout_with_file;
	}

	/* Release the reference and lock on the parent directory */

	parent->tdo_refs--;
	tmpfs_unlock_directory(parent);

	/* Return success */

	*tfo = newtfo;
	return 0;

	/* Error exits */

errout_with_file:
	// nxrmutex_destroy(&newtfo->tfo_lock);
	k_free(newtfo);

errout_with_parent:
	parent->tdo_refs--;
	tmpfs_unlock_directory(parent);
	return ret;
}

/****************************************************************************
 * Name: tmpfs_alloc_directory
 ****************************************************************************/

struct tmpfs_directory_s *tmpfs_alloc_directory(struct tmpfs_directory_s *parent)
{
	struct tmpfs_directory_s *tdo;

	/* Create a new zero length directory object */

	tdo = k_malloc(sizeof(*tdo));
	if (tdo == NULL) {
		return NULL;
	}

	memset(tdo, 0, sizeof(*tdo));

	/* Initialize the new directory object */

	tdo->tdo_alloc = 0;
	tdo->tdo_type = TMPFS_DIRECTORY;
	tdo->tdo_refs = 0;
	tdo->tdo_parent = parent;
	tdo->tdo_nentries = 0;
	tdo->tdo_entry = NULL;

	k_mutex_init(&tdo->tdo_lock);

	return tdo;
}

/****************************************************************************
 * Name: tmpfs_create_directory
 ****************************************************************************/

int tmpfs_create_directory(struct tmpfs_s *fs, const char *relpath, struct tmpfs_directory_s **tdo)
{
	struct tmpfs_directory_s *parent;
	struct tmpfs_directory_s *newtdo;
	const char *name;
	int ret;

	/* Separate the path into the file name and the path to the parent
	 * directory.
	 */

	name = strrchr(relpath, '/');
	if (name && name[1] == '\0') {
		/* Ignore the tail '/' */

		name = memrchr(relpath, '/', name - relpath);
	}

	if (name == NULL) {
		/* No subdirectories... use the root directory */

		name = relpath;
		parent = (struct tmpfs_directory_s *)fs->tfs_root.tde_object;

		ret = tmpfs_lock_directory(parent);
		if (ret < 0) {
			return ret;
		}

		parent->tdo_refs++;
	} else {
		/* Locate the parent directory that should contain this name.
		 * On success, tmpfs_find_directory() will lockthe parent
		 * directory and increment the reference count.
		 */

		ret = tmpfs_find_directory(fs, relpath, name - relpath, &parent, NULL);
		if (ret < 0) {
			return ret;
		}

		/* Skip the '/' path separator */

		name++;
	}

	/* Verify that no object of this name already exists in the directory */

	ret = tmpfs_find_dirent(parent, name, strlen(name));
	if (ret != -ENOENT) {
		/* Something with this name already exists in the directory.
		 * OR perhaps some fatal error occurred.
		 */

		if (ret >= 0) {
			ret = -EEXIST;
		}

		goto errout_with_parent;
	}

	/* Allocate an empty directory object.  NOTE that there is no reference on
	 * the new directory and the object is not locked.
	 */

	newtdo = tmpfs_alloc_directory(parent);
	if (newtdo == NULL) {
		ret = -ENOMEM;
		goto errout_with_parent;
	}

	/* Then add the new, empty file to the directory */

	ret = tmpfs_add_dirent(parent, (struct tmpfs_object_s *)newtdo, name);
	if (ret < 0) {
		goto errout_with_directory;
	}

	/* Free the copy of the relpath, release our reference to the parent
	 * directory, and return success
	 */

	parent->tdo_refs--;
	tmpfs_unlock_directory(parent);

	/* Return the (unlocked, unreferenced) directory object to the caller */

	if (tdo != NULL) {
		*tdo = newtdo;
	}

	return 0;

	/* Error exits */

errout_with_directory:
	// nxrmutex_destroy(&newtdo->tdo_lock);
	k_free(newtdo);

errout_with_parent:
	parent->tdo_refs--;
	tmpfs_unlock_directory(parent);
	return ret;
}

/****************************************************************************
 * Name: tmpfs_find_object
 ****************************************************************************/

int tmpfs_find_object(struct tmpfs_s *fs, const char *relpath, size_t len,
		      struct tmpfs_object_s **object, struct tmpfs_directory_s **parent)
{
	struct tmpfs_object_s *to = NULL;
	struct tmpfs_directory_s *tdo = NULL;
	struct tmpfs_directory_s *next_tdo;
	const char *segment;
	const char *next_segment;
	int index;
	int ret;

	/* Traverse the file system for any object with the matching name */

	to = fs->tfs_root.tde_object;
	next_tdo = (struct tmpfs_directory_s *)fs->tfs_root.tde_object;

	for (segment = relpath; len != 0; segment = next_segment + 1) {
		/* Get the next segment after the one we are currently working on.
		 * This will be NULL is we are working on the final segment of the
		 * relpath.
		 */

		/* Skip any slash. */

		while (*segment == '/') {
			segment++;
			len--;
		}

		next_segment = my_memrchr(segment, '/', len);
		if (next_segment) {
			len -= next_segment + 1 - segment;
		} else {
			next_segment = segment + len;
			len = 0;
		}

		/* Search the next directory. */

		tdo = next_tdo;

		/* Find the TMPFS object with the next segment name in the current
		 * directory.
		 */

		index = tmpfs_find_dirent(tdo, segment, next_segment - segment);
		if (index < 0) {
			/* No object with this name exists in the directory. */

			return index;
		}

		to = tdo->tdo_entry[index].tde_object;

		/* Is this object another directory? */

		if (to->to_type != TMPFS_DIRECTORY) {
			/* No.  Was this the final segment in the path? */

			if (len == 0 && *next_segment != '/') {
				/* Then we can break out of the loop now */

				break;
			}

			/* No, this was not the final segment of the relpath.
			 * We cannot continue the search if any of the intermediate
			 * segments do no correspond to directories.
			 */

			return -ENOTDIR;
		}

		/* Search this directory for the next segment.  If we
		 * exit the loop, tdo will still refer to the parent
		 * directory of to.
		 */

		next_tdo = (struct tmpfs_directory_s *)to;
	}

	/* When we exit this loop (successfully), to will point to the TMPFS
	 * object associated with the terminal segment of the relpath.
	 * Increment the reference count on the located object.
	 */

	/* Return what we found */

	if (parent) {
		if (tdo != NULL) {
			/* Get exclusive access to the parent and increment the reference
			 * count on the object.
			 */

			ret = tmpfs_lock_directory(tdo);
			if (ret < 0) {
				return ret;
			}

			tdo->tdo_refs++;
		}

		*parent = tdo;
	}

	if (object) {
		if (to != NULL) {
			/* Get exclusive access to the object and increment the reference
			 * count on the object.
			 */

			ret = tmpfs_lock_object(to);
			if (ret < 0) {
				return ret;
			}

			to->to_refs++;
		}

		*object = to;
	}

	return 0;
}

/****************************************************************************
 * Name: tmpfs_find_file
 ****************************************************************************/

int tmpfs_find_file(struct tmpfs_s *fs, const char *relpath, struct tmpfs_file_s **tfo,
		    struct tmpfs_directory_s **parent)
{
	struct tmpfs_object_s *to;
	size_t len;
	int ret;

	len = strlen(relpath);
	if (len == 0) {
		return -EINVAL;
	} else if (relpath[len - 1] == '/') {
		return -EISDIR;
	}

	/* Find the object at this path.  If successful, tmpfs_find_object() will
	 * lock both the object and the parent directory and will increment the
	 * reference count on both.
	 */

	ret = tmpfs_find_object(fs, relpath, len, &to, parent);
	if (ret >= 0) {
		/* We found it... but is it a regular file? */

		if (to->to_type != TMPFS_REGULAR) {
			/* No... unlock the object and its parent and return an error */

			tmpfs_release_lockedobject(to);

			if (parent) {
				struct tmpfs_directory_s *tdo = *parent;

				tdo->tdo_refs--;
				tmpfs_unlock_directory(tdo);
			}

			ret = -EISDIR;
		}

		/* Return the verified file object */

		*tfo = (struct tmpfs_file_s *)to;
	}

	return ret;
}

/****************************************************************************
 * Name: tmpfs_find_directory
 ****************************************************************************/

int tmpfs_find_directory(struct tmpfs_s *fs, const char *relpath, size_t len,
			 struct tmpfs_directory_s **tdo, struct tmpfs_directory_s **parent)
{
	struct tmpfs_object_s *to;
	int ret;

	/* Find the object at this path */

	ret = tmpfs_find_object(fs, relpath, len, &to, parent);
	if (ret >= 0) {
		/* We found it... but is it a regular file? */

		if (to->to_type != TMPFS_DIRECTORY) {
			/* No... unlock the object and its parent and return an error */

			tmpfs_release_lockedobject(to);

			if (parent) {
				struct tmpfs_directory_s *tmptdo = *parent;

				tmptdo->tdo_refs--;
				tmpfs_unlock_directory(tmptdo);
			}

			ret = -ENOTDIR;
		}

		/* Return the verified file object */

		*tdo = (struct tmpfs_directory_s *)to;
	}

	return ret;
}

/****************************************************************************
 * Name: tmpfs_free_callout
 ****************************************************************************/

int tmpfs_free_callout(struct tmpfs_directory_s *tdo, unsigned int index, void *arg)
{
	struct tmpfs_dirent_s *tde;
	struct tmpfs_object_s *to;
	struct tmpfs_file_s *tfo;
	unsigned int last;

	/* Free the object name */

	if (tdo->tdo_entry[index].tde_name != NULL) {
		k_free(tdo->tdo_entry[index].tde_name);
	}

	/* Remove by replacing this entry with the final directory entry */

	tde = &tdo->tdo_entry[index];
	to = tde->tde_object;
	last = tdo->tdo_nentries - 1;

	if (index != last) {
		/* Move the directory entry */

		*tde = tdo->tdo_entry[last];
	}

	/* And decrement the count of directory entries */

	tdo->tdo_nentries = last;

	/* Is this directory entry a file object? */

	if (to->to_type == TMPFS_REGULAR) {
		tfo = (struct tmpfs_file_s *)to;

		/* Are there references to the file? */

		if (tfo->tfo_refs > 0) {
			/* Yes.. We cannot delete the file now. Just mark it as unlinked. */

			tfo->tfo_flags |= TFO_FLAG_UNLINKED;
			return TMPFS_UNLINKED;
		}

		k_free(tfo->tfo_data);
	} else /* if (to->to_type == TMPFS_DIRECTORY) */
	{
		tdo = (struct tmpfs_directory_s *)to;

		k_free(tdo->tdo_entry);
	}

	/* Free the object now */

	// nxrmutex_destroy(&to->to_lock);
	k_free(to);
	return TMPFS_DELETED;
}

/****************************************************************************
 * Name: tmpfs_foreach
 ****************************************************************************/

int tmpfs_foreach(struct tmpfs_directory_s *tdo, tmpfs_foreach_t callout, void *arg)
{
	struct tmpfs_object_s *to;
	unsigned int index;
	int ret;

	/* Visit each directory entry */

	for (index = 0; index < tdo->tdo_nentries;) {
		/* Lock the object and take a reference */

		to = tdo->tdo_entry[index].tde_object;
		ret = tmpfs_lock_object(to);
		if (ret < 0) {
			return ret;
		}

		to->to_refs++;

		/* Is the next entry a directory? */

		if (to->to_type == TMPFS_DIRECTORY) {
			struct tmpfs_directory_s *next = (struct tmpfs_directory_s *)to;

			/* Yes.. traverse its children first in the case the final
			 * action will be to delete the directory.
			 */

			ret = tmpfs_foreach(next, callout, arg);
			if (ret < 0) {
				return -ECANCELED;
			}
		}

		/* Perform the callout */

		ret = callout(tdo, index, arg);
		switch (ret) {
		case TMPFS_CONTINUE: /* Continue enumeration */

			/* Release the object and index to the next entry */

			tmpfs_release_lockedobject(to);
			index++;
			break;

		case TMPFS_HALT: /* Stop enumeration */

			/* Release the object and cancel the traversal */

			tmpfs_release_lockedobject(to);
			return -ECANCELED;

		case TMPFS_UNLINKED: /* Only the directory entry was deleted */

			/* Release the object and continue with the same index */

			tmpfs_release_lockedobject(to);

		case TMPFS_DELETED: /* Object and directory entry deleted */
			break;      /* Continue with the same index */
		}
	}

	return 0;
}
