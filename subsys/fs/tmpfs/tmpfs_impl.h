/*
 * Copyright (c) 2025 Marco Casaroli <marco.casaroli@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "tmpfs.h"

void *memrchr(const void *m, int c, size_t n);

int tmpfs_realloc_directory(struct tmpfs_directory_s *tdo, unsigned int nentries);
int tmpfs_realloc_file(struct tmpfs_file_s *tfo, size_t newsize);
void tmpfs_release_lockedobject(struct tmpfs_object_s *to);
void tmpfs_release_lockedfile(struct tmpfs_file_s *tfo);
int tmpfs_release_file(struct tmpfs_file_s *tfo);
int tmpfs_find_dirent(struct tmpfs_directory_s *tdo, const char *name, size_t len);
int tmpfs_remove_dirent(struct tmpfs_directory_s *tdo, const char *name);
int tmpfs_add_dirent(struct tmpfs_directory_s *tdo, struct tmpfs_object_s *to, const char *name);
struct tmpfs_file_s *tmpfs_alloc_file(struct tmpfs_directory_s *parent);
int tmpfs_create_file(struct tmpfs_s *fs, const char *relpath, struct tmpfs_file_s **tfo);
struct tmpfs_directory_s *tmpfs_alloc_directory(struct tmpfs_directory_s *parent);
int tmpfs_create_directory(struct tmpfs_s *fs, const char *relpath, struct tmpfs_directory_s **tdo);
int tmpfs_find_object(struct tmpfs_s *fs, const char *relpath, size_t len,
		      struct tmpfs_object_s **object, struct tmpfs_directory_s **parent);
int tmpfs_find_file(struct tmpfs_s *fs, const char *relpath, struct tmpfs_file_s **tfo,
		    struct tmpfs_directory_s **parent);
int tmpfs_find_directory(struct tmpfs_s *fs, const char *relpath, size_t len,
			 struct tmpfs_directory_s **tdo, struct tmpfs_directory_s **parent);
int tmpfs_free_callout(struct tmpfs_directory_s *tdo, unsigned int index, void *arg);
int tmpfs_foreach(struct tmpfs_directory_s *tdo, tmpfs_foreach_t callout, void *arg);
