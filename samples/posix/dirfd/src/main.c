/*
 * Copyright (c) 2023 Meta
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/logging/log.h>
#include <stdio.h>
#include <sys/utsname.h>
// #include <sys/types.h>
#include <dirent.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <fcntl.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>
#include <unistd.h>

#define DIRECTORY_NAME "/lfs/cfs"

LOG_MODULE_REGISTER(dirfd_sample, 4);

static int littlefs_flash_erase(unsigned int id)
{
	const struct flash_area *pfa;
	int rc;

	rc = flash_area_open(id, &pfa);
	if (rc < 0) {
		LOG_ERR("FAIL: unable to find flash area %u: %d\n",
			id, rc);
		return rc;
	}

	LOG_PRINTK("Area %u at 0x%x on %s for %u bytes\n",
		   id, (unsigned int)pfa->fa_off, pfa->fa_dev->name,
		   (unsigned int)pfa->fa_size);

	/* Optional wipe flash contents */
	if (IS_ENABLED(CONFIG_APP_WIPE_STORAGE)) {
		rc = flash_area_flatten(pfa, 0, pfa->fa_size);
		LOG_ERR("Erasing flash area ... %d", rc);
	}

	flash_area_close(pfa);
	return rc;
}
#define PARTITION_NODE DT_NODELABEL(lfs1)

#if DT_NODE_EXISTS(PARTITION_NODE)
FS_FSTAB_DECLARE_ENTRY(PARTITION_NODE);
#else /* PARTITION_NODE */
FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(storage);
static struct fs_mount_t lfs_storage_mnt = {
	.type = FS_LITTLEFS,
	.fs_data = &storage,
	.storage_dev = (void *)FIXED_PARTITION_ID(storage_partition),
	.mnt_point = "/lfs",
};
#endif /* PARTITION_NODE */

	struct fs_mount_t *mountpoint =
#if DT_NODE_EXISTS(PARTITION_NODE)
		&FS_FSTAB_ENTRY(PARTITION_NODE)
#else
		&lfs_storage_mnt
#endif
		;

static int littlefs_mount(struct fs_mount_t *mp)
{
	int rc;

	rc = littlefs_flash_erase((uintptr_t)mp->storage_dev);
	if (rc < 0) {
		return rc;
	}

	/* Do not mount if auto-mount has been enabled */
#if !DT_NODE_EXISTS(PARTITION_NODE) ||						\
	!(FSTAB_ENTRY_DT_MOUNT_FLAGS(PARTITION_NODE) & FS_MOUNT_FLAG_AUTOMOUNT)
	rc = fs_mount(mp);
	if (rc < 0) {
		LOG_PRINTK("FAIL: mount id %" PRIuPTR " at %s: %d\n",
		       (uintptr_t)mp->storage_dev, mp->mnt_point, rc);
		return rc;
	}
	LOG_PRINTK("%s mount: %d\n", mp->mnt_point, rc);
#else
	LOG_PRINTK("%s automounted\n", mp->mnt_point);
#endif

	return 0;
}


int main(void)
{
	int rc;

	LOG_PRINTK("Mounting littlefs\n");

	rc = littlefs_mount(mountpoint);
	if (rc < 0) {
		LOG_PRINTK("FAIL: mount id %" PRIuPTR " at %s: %d\n",
		       (uintptr_t)mountpoint->storage_dev, mountpoint->mnt_point, rc);
		return rc;
	}
	LOG_PRINTK("%s mount: %d\n", mountpoint->mnt_point, rc);

	DIR *parent = opendir(DIRECTORY_NAME);
	if (!parent) {
		perror("opendir");
		return 1;
	}

	int dfd = dirfd(parent);
	if (dfd < 0) {
		perror("dirfd");
		return 1;
	}

	printf("got dirfd %d\n", dfd);

	int ffd = openat(dfd, "file.txt", O_RDWR | O_CREAT, 0644);
	if (ffd < 0) {
		perror("openat");
		return 1;
	}

	write(ffd, "Hello, World!", 13);
	close(ffd);

	close(dfd);


	// uname(&info);

	// printf("\nPrinting everything in utsname...\n");
	// printf("sysname[%zu]: %s\n", sizeof(info.sysname), info.sysname);
	// printf("nodename[%zu]: %s\n", sizeof(info.nodename), info.nodename);
	// printf("release[%zu]: %s\n", sizeof(info.release), info.release);
	// printf("version[%zu]: %s\n", sizeof(info.version), info.version);
	// printf("machine[%zu]: %s\n", sizeof(info.machine), info.machine);

	return 0;
}
