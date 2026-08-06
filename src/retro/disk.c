/*
  Hatari - libretro disk control

  SPDX-License-Identifier: GPL-2.0-or-later

  This layer owns the libretro disk list and delegates actual image parsing,
  writes, and emulation to Hatari's native floppy implementation.
*/

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <libretro.h>

#include "floppy.h"
#include "retro_disk.h"
#include "vfs.h"

#define RETRO_DISK_MAX 32

static char image_paths[RETRO_DISK_MAX][FILENAME_MAX];
static char materialized_paths[RETRO_DISK_MAX][FILENAME_MAX];
static unsigned image_count;
static unsigned image_index[2];
static unsigned selected_drive;
static bool ejected = true;
static unsigned initial_image = RETRO_DISK_MAX;
static char initial_image_path[FILENAME_MAX];

static bool has_extension(const char *path, const char *extension)
{
	const char *dot = strrchr(path, '.');
	return dot && !strcasecmp(dot + 1, extension);
}

static bool add_path(const char *path)
{
	if (!path || !*path || image_count >= RETRO_DISK_MAX)
		return false;
	snprintf(image_paths[image_count], sizeof(image_paths[0]), "%s", path);
	++image_count;
	return true;
}

static void release_materialized(unsigned index, bool writeback)
{
	if (index >= image_count || !materialized_paths[index][0])
		return;
	RetroVfs_Release(image_paths[index], materialized_paths[index], writeback);
}

static bool materialize_image(unsigned index)
{
	char temporary[PATH_MAX];

	if (index >= image_count || materialized_paths[index][0])
		return false;
	if (!RetroVfs_MakeTemplate("hatari-libretro-disk-XXXXXX", temporary,
	                          sizeof(temporary)))
		return false;
	return RetroVfs_Materialize(image_paths[index], temporary, 0,
	                            materialized_paths[index],
	                            sizeof(materialized_paths[index]));
}

static void clear_images(void)
{
	unsigned index;
	for (index = 0; index < image_count; ++index)
		release_materialized(index, false);
	image_count = 0;
}

static void playlist_directory(const char *playlist, char *directory,
	                              size_t size)
{
	const char *slash = strrchr(playlist, '/');
#ifdef _WIN32
	const char *backslash = strrchr(playlist, '\\');
	if (!slash || (backslash && backslash > slash))
		slash = backslash;
#endif
	if (!slash)
	{
		directory[0] = '\0';
		return;
	}
	if ((size_t)(slash - playlist + 1) >= size)
	{
		directory[0] = '\0';
		return;
	}
	memcpy(directory, playlist, (size_t)(slash - playlist + 1));
	directory[slash - playlist + 1] = '\0';
}

static bool load_playlist(const char *playlist)
{
	FILE *file;
	char line[FILENAME_MAX];
	char directory[FILENAME_MAX];
	char temporary[PATH_MAX];
	char materialized[FILENAME_MAX];
	unsigned old_count = image_count;

	if (!RetroVfs_MakeTemplate("hatari-libretro-playlist-XXXXXX", temporary,
	                           sizeof(temporary)))
		return false;
	if (RetroVfs_Materialize(playlist, temporary, 0, materialized,
                         sizeof(materialized)))
	{
		file = fopen(temporary, "rb");
		if (file)
		{
			while (fgets(line, sizeof(line), file))
			{
				char *entry = line;
				char path[FILENAME_MAX];
				size_t length;

				while (isspace((unsigned char)*entry))
					++entry;
				length = strlen(entry);
				while (length && isspace((unsigned char)entry[length - 1]))
					entry[--length] = '\0';
				if (!*entry || *entry == '#')
					continue;
				playlist_directory(playlist, directory, sizeof(directory));
				if (entry[0] == '/' ||
				    (isalpha((unsigned char)entry[0]) && entry[1] == ':'))
					snprintf(path, sizeof(path), "%s", entry);
				else
					snprintf(path, sizeof(path), "%s%s", directory, entry);
				if (!add_path(path))
					break;
			}
			fclose(file);
		}
		RetroVfs_Release(NULL, materialized, false);
		return image_count > old_count;
	}

	file = fopen(playlist, "rb");
	if (!file)
		return false;
	playlist_directory(playlist, directory, sizeof(directory));
	while (fgets(line, sizeof(line), file))
	{
		char *entry = line;
		char path[FILENAME_MAX];
		size_t length;

		while (isspace((unsigned char)*entry))
			++entry;
		length = strlen(entry);
		while (length && isspace((unsigned char)entry[length - 1]))
			entry[--length] = '\0';
		if (!*entry || *entry == '#')
			continue;
		if (entry[0] == '/' ||
		    (isalpha((unsigned char)entry[0]) && entry[1] == ':'))
			snprintf(path, sizeof(path), "%s", entry);
		else
			snprintf(path, sizeof(path), "%s%s", directory, entry);
		if (!add_path(path))
			break;
	}
	fclose(file);
	return image_count > old_count;
}

static bool insert_selected(void)
{
	const char *path;
	unsigned index = image_index[selected_drive];

	if (index >= image_count)
		return true;
	path = image_paths[index];
	if (!Floppy_SetDiskFileName((int)selected_drive, path, NULL))
	{
		if (!materialize_image(index))
			return false;
		path = materialized_paths[index];
		if (!Floppy_SetDiskFileName((int)selected_drive, path, NULL))
		{
			release_materialized(index, false);
			return false;
		}
	}
	return Floppy_InsertDiskIntoDrive((int)selected_drive);
}

static bool RETRO_CALLCONV disk_set_eject_state(bool value)
{
	if (value)
	{
		if (!ejected)
		{
			Floppy_EjectDiskFromDrive((int)selected_drive);
			release_materialized(image_index[selected_drive], true);
		}
		ejected = true;
		return true;
	}
	if (!ejected)
		return true;
	if (!insert_selected())
		return false;
	ejected = false;
	return true;
}

static bool RETRO_CALLCONV disk_get_eject_state(void)
{
	return ejected;
}

static unsigned RETRO_CALLCONV disk_get_image_index(void)
{
	return image_index[selected_drive];
}

static bool RETRO_CALLCONV disk_set_image_index(unsigned index)
{
	if (!ejected)
		return false;
	image_index[selected_drive] = index;
	return true;
}

static bool RETRO_CALLCONV disk_set_initial_image(unsigned index,
		const char *path)
{
	if (index >= RETRO_DISK_MAX || !path)
		return false;
	initial_image = index;
	if (snprintf(initial_image_path, sizeof(initial_image_path), "%s", path) >=
	    (int)sizeof(initial_image_path))
	{
		initial_image = RETRO_DISK_MAX;
		initial_image_path[0] = '\0';
		return false;
	}
	return true;
}

static unsigned RETRO_CALLCONV disk_get_num_images(void)
{
	return image_count;
}

static bool RETRO_CALLCONV disk_replace_image_index(unsigned index,
		const struct retro_game_info *game)
{
	if (!ejected || index >= image_count)
		return false;
	if (!game)
	{
		release_materialized(index, false);
		memmove(&image_paths[index], &image_paths[index + 1],
		        (image_count - index - 1) * sizeof(image_paths[0]));
		memmove(&materialized_paths[index], &materialized_paths[index + 1],
		        (image_count - index - 1) * sizeof(materialized_paths[0]));
		--image_count;
		return true;
	}
	if (!game->path)
		return false;
	release_materialized(index, false);
	snprintf(image_paths[index], sizeof(image_paths[0]), "%s", game->path);
	return true;
}

static bool RETRO_CALLCONV disk_add_image_index(void)
{
	if (image_count >= RETRO_DISK_MAX)
		return false;
	image_paths[image_count][0] = '\0';
	++image_count;
	return true;
}

static bool RETRO_CALLCONV disk_get_image_path(unsigned index, char *path,
		size_t length)
{
	if (index >= image_count || !path || !length)
		return false;
	strncpy(path, image_paths[index], length - 1);
	path[length - 1] = '\0';
	return true;
}

static bool RETRO_CALLCONV disk_get_image_label(unsigned index, char *label,
		size_t length)
{
	const char *slash;
	const char *backslash;
	if (!disk_get_image_path(index, label, length))
		return false;
	slash = strrchr(label, '/');
	backslash = strrchr(label, '\\');
	if (!slash || (backslash && backslash > slash))
		slash = backslash;
	if (slash)
		memmove(label, slash + 1, strlen(slash + 1) + 1);
	return true;
}

void RetroDisk_SetEnvironment(retro_environment_t cb)
{
	static struct retro_disk_control_callback callbacks = {
		disk_set_eject_state, disk_get_eject_state,
		disk_get_image_index, disk_set_image_index, disk_get_num_images,
		disk_replace_image_index, disk_add_image_index
	};
	static struct retro_disk_control_ext_callback extended = {
		disk_set_eject_state, disk_get_eject_state,
		disk_get_image_index, disk_set_image_index, disk_get_num_images,
		disk_replace_image_index, disk_add_image_index, disk_set_initial_image,
		disk_get_image_path, disk_get_image_label
	};

	cb(RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE, (void *)&callbacks);
	cb(RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE, (void *)&extended);
}

bool RetroDisk_LoadGame(const struct retro_game_info *game)
{
	if (!ejected)
		disk_set_eject_state(true);
	clear_images();
	image_index[0] = image_index[1] = RETRO_DISK_MAX;
	selected_drive = 0;
	ejected = true;
	Floppy_SetDiskFileNameNone(0);
	Floppy_SetDiskFileNameNone(1);

	if (!game)
		return true;
	if (!game->path)
		return false;
	if (has_extension(game->path, "m3u") || has_extension(game->path, "m3u8"))
	{
		if (!load_playlist(game->path))
			return false;
	}
	else if (!add_path(game->path))
		return false;

	image_index[0] = 0;
	if (initial_image < image_count &&
	    (!initial_image_path[0] ||
	     !strcmp(initial_image_path, image_paths[initial_image])))
		image_index[0] = initial_image;
	initial_image = RETRO_DISK_MAX;
	initial_image_path[0] = '\0';
	return disk_set_eject_state(false);
}

bool RetroDisk_LoadGameSpecial(const struct retro_game_info *info,
		size_t num_info)
{
	size_t index;

	if (!info || !num_info)
		return false;
	if (!ejected)
		disk_set_eject_state(true);
	clear_images();
	image_index[0] = image_index[1] = RETRO_DISK_MAX;
	selected_drive = 0;
	ejected = true;
	Floppy_SetDiskFileNameNone(0);
	Floppy_SetDiskFileNameNone(1);
	for (index = 0; index < num_info; ++index)
	{
		if (!info[index].path || !add_path(info[index].path))
		{
			clear_images();
			return false;
		}
	}
	image_index[0] = 0;
	if (initial_image < image_count &&
	    (!initial_image_path[0] ||
	     !strcmp(initial_image_path, image_paths[initial_image])))
		image_index[0] = initial_image;
	initial_image = RETRO_DISK_MAX;
	initial_image_path[0] = '\0';
	return disk_set_eject_state(false);
}

void RetroDisk_UnloadGame(void)
{
	if (!ejected)
		disk_set_eject_state(true);
	clear_images();
	image_index[0] = image_index[1] = RETRO_DISK_MAX;
	selected_drive = 0;
	initial_image = RETRO_DISK_MAX;
	initial_image_path[0] = '\0';
}
