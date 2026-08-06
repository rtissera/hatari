/*
  Hatari - libretro VFS helper

  SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "sysdeps.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <libretro.h>

#include "vfs.h"

static struct retro_vfs_interface *vfs;
static char temporary_directory[PATH_MAX];

void RetroVfs_SetEnvironment(retro_environment_t cb)
{
	struct retro_vfs_interface_info info = { 1, NULL };
	const char *directory = NULL;
	vfs = NULL;
	temporary_directory[0] = '\0';
	if (cb(RETRO_ENVIRONMENT_GET_VFS_INTERFACE, &info))
		vfs = info.iface;
	if (cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &directory) && directory &&
	    *directory && snprintf(temporary_directory, sizeof(temporary_directory),
                            "%s", directory) < (int)sizeof(temporary_directory))
		return;
	snprintf(temporary_directory, sizeof(temporary_directory), "%s", "/tmp");
}

bool RetroVfs_MakeTemplate(const char *name, const char *suffix, char *path,
		size_t path_size)
{
	size_t length;

	if (!name || !suffix || !path || !path_size || !temporary_directory[0])
		return false;
	length = strlen(temporary_directory);
	return snprintf(path, path_size, "%s%s%s%s", temporary_directory,
	                length && temporary_directory[length - 1] == '/' ? "" : "/",
	                name, suffix) < (int)path_size;
}

/* Append a suffix (e.g. a file extension) to a mkstemp() path without
   relying on the non-portable GNU/BSD mkstemps() extension: rename() a
   freshly mkstemp()'d file, keeping the already-open descriptor valid
   (POSIX guarantees an open fd survives a rename of its file). Writes the
   renamed path back into template_path so the caller sees the real name.
   Assumes template_path's last suffix_length characters are the suffix
   and the characters immediately before that are "XXXXXX", i.e. the name
   passed to RetroVfs_MakeTemplate() ends in "XXXXXX" with nothing between
   it and the suffix; mkstemp() below simply fails (-1) otherwise. */
static int materialize_with_suffix(char *template_path,
		size_t template_path_size, int suffix_length)
{
	size_t total_len = strlen(template_path);
	size_t base_len = total_len > (size_t)suffix_length ?
			total_len - (size_t)suffix_length : 0;
	char base_template[PATH_MAX];
	char final_path[PATH_MAX];
	int fd;

	if (!base_len || base_len >= sizeof(base_template))
		return -1;
	memcpy(base_template, template_path, base_len);
	base_template[base_len] = '\0';
	fd = mkstemp(base_template);
	if (fd < 0)
		return -1;
	if (snprintf(final_path, sizeof(final_path), "%s%s", base_template,
	             template_path + base_len) >= (int)sizeof(final_path) ||
	    rename(base_template, final_path) != 0)
	{
		close(fd);
		unlink(base_template);
		return -1;
	}
	if (snprintf(template_path, template_path_size, "%s", final_path) >=
	    (int)template_path_size)
	{
		close(fd);
		unlink(final_path);
		return -1;
	}
	return fd;
}

bool RetroVfs_Materialize(const char *source, char *template_path,
		size_t template_path_size, int64_t size_limit, int suffix_length,
		char *destination, size_t destination_size)
{
	struct retro_vfs_file_handle *stream;
	char buffer[64 * 1024];
	FILE *file;
	int64_t size;
	int64_t bytes;
	int fd;

	if (!source || !template_path || !destination || !destination_size ||
	    !vfs || !vfs->open || !vfs->read || !vfs->close)
		return false;
	stream = vfs->open(source, RETRO_VFS_FILE_ACCESS_READ,
	                  RETRO_VFS_FILE_ACCESS_HINT_NONE);
	if (!stream)
		return false;
	size = vfs->size ? vfs->size(stream) : -1;
	if (size_limit > 0 && (size < 0 || size > size_limit))
	{
		vfs->close(stream);
		return false;
	}
	fd = suffix_length > 0 ?
			materialize_with_suffix(template_path, template_path_size,
			                        suffix_length) :
			mkstemp(template_path);
	if (fd < 0)
	{
		vfs->close(stream);
		return false;
	}
	file = fdopen(fd, "wb");
	if (!file)
	{
		close(fd);
		unlink(template_path);
		vfs->close(stream);
		return false;
	}
	while ((bytes = vfs->read(stream, buffer, sizeof(buffer))) > 0)
	{
		if (fwrite(buffer, 1, (size_t)bytes, file) != (size_t)bytes)
		{
			fclose(file);
			unlink(template_path);
			vfs->close(stream);
			return false;
		}
	}
	vfs->close(stream);
	if (bytes < 0 || fclose(file) != 0)
	{
		unlink(template_path);
		return false;
	}
	if (snprintf(destination, destination_size, "%s", template_path) >=
	    (int)destination_size)
	{
		unlink(template_path);
		destination[0] = '\0';
		return false;
	}
	return true;
}

void RetroVfs_Writeback(const char *source, const char *materialized)
{
	struct retro_vfs_file_handle *stream;
	char buffer[64 * 1024];
	FILE *file;
	int64_t bytes;

	if (!source || !materialized || !*source || !*materialized || !vfs ||
	    !vfs->open || !vfs->write || !vfs->close)
		return;
	file = fopen(materialized, "rb");
	if (!file)
		return;
	stream = vfs->open(source,
	                  RETRO_VFS_FILE_ACCESS_READ_WRITE |
	                  RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING,
	                  RETRO_VFS_FILE_ACCESS_HINT_NONE);
	if (!stream)
	{
		fclose(file);
		return;
	}
	while ((bytes = (int64_t)fread(buffer, 1, sizeof(buffer), file)) > 0)
	{
		if (vfs->write(stream, buffer, (uint64_t)bytes) != bytes)
			break;
	}
	fclose(file);
	vfs->close(stream);
}

void RetroVfs_Release(const char *source, char *materialized, bool writeback)
{
	if (!materialized || !materialized[0])
		return;
	if (writeback)
		RetroVfs_Writeback(source, materialized);
	unlink(materialized);
	materialized[0] = '\0';
}
