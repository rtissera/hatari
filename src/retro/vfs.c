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

void RetroVfs_SetEnvironment(retro_environment_t cb)
{
	struct retro_vfs_interface_info info = { 1, NULL };
	vfs = NULL;
	if (cb(RETRO_ENVIRONMENT_GET_VFS_INTERFACE, &info))
		vfs = info.iface;
}

bool RetroVfs_Materialize(const char *source, char *template_path,
		int64_t size_limit, char *destination, size_t destination_size)
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
	fd = mkstemp(template_path);
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
