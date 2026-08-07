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
static bool ejected[2] = { true, true };
static unsigned selected_drive;
static unsigned initial_image = RETRO_DISK_MAX;
static char initial_image_path[FILENAME_MAX];

static bool has_extension(const char *path, const char *extension)
{
	const char *dot = strrchr(path, '.');
	return dot && !strcasecmp(dot + 1, extension);
}

/* Full compound extension of the basename (e.g. ".st", ".st.gz"), or "" if
   none. Hatari's floppy format detectors (ST_FileNameIsST() etc.) key off
   the file extension, not content, so a materialized temp file that drops
   it would never be recognized as any known image format. */
static const char *extension_suffix(const char *path)
{
	const char *slash = strrchr(path, '/');
	const char *backslash = strrchr(path, '\\');
	const char *base;
	const char *dot;

	if (!slash || (backslash && backslash > slash))
		slash = backslash;
	base = slash ? slash + 1 : path;
	dot = strchr(base, '.');
	return dot ? dot : "";
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
	const char *suffix;

	if (index >= image_count || materialized_paths[index][0])
		return false;
	suffix = extension_suffix(image_paths[index]);
	if (!RetroVfs_MakeTemplate("hatari-libretro-disk-XXXXXX", suffix, temporary,
	                          sizeof(temporary)))
		return false;
	return RetroVfs_Materialize(image_paths[index], temporary,
	                            sizeof(temporary), 0, (int)strlen(suffix),
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

	if (!RetroVfs_MakeTemplate("hatari-libretro-playlist-XXXXXX", "", temporary,
	                           sizeof(temporary)))
		return false;
	if (RetroVfs_Materialize(playlist, temporary, sizeof(temporary), 0, 0,
                         materialized, sizeof(materialized)))
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

static retro_environment_t disk_environment_cb;

static void notify(const char *message)
{
	struct retro_message_ext notification;

	if (!disk_environment_cb)
		return;
	memset(&notification, 0, sizeof(notification));
	notification.msg = message;
	notification.duration = 3000;
	notification.priority = 1;
	notification.level = RETRO_LOG_INFO;
	notification.target = RETRO_MESSAGE_TARGET_OSD;
	notification.type = RETRO_MESSAGE_TYPE_NOTIFICATION;
	notification.progress = -1;
	disk_environment_cb(RETRO_ENVIRONMENT_SET_MESSAGE_EXT, &notification);
}

static bool insert_drive(unsigned drive)
{
	const char *path;
	unsigned index = image_index[drive];

	if (index >= image_count)
		return true;
	path = image_paths[index];
	if (!Floppy_SetDiskFileName((int)drive, path, NULL))
	{
		if (!materialize_image(index))
			return false;
		path = materialized_paths[index];
		if (!Floppy_SetDiskFileName((int)drive, path, NULL))
		{
			release_materialized(index, false);
			return false;
		}
	}
	if (Floppy_InsertDiskIntoDrive((int)drive))
		return true;
	/* Don't leave szDiskFileName[drive] pointing at an image that failed
	   to actually mount: it would block that same image from being
	   inserted into the other drive afterwards. */
	Floppy_SetDiskFileNameNone((int)drive);
	return false;
}

static void eject_drive(unsigned drive)
{
	if (ejected[drive])
		return;
	Floppy_EjectDiskFromDrive((int)drive);
	/* Floppy_EjectDiskFromDrive() only clears Hatari's internal
	   EmulationDrives[] record, not ConfigureParams.DiskImage.szDiskFileName[].
	   Leaving the latter set makes Floppy_SetDiskFileName() refuse to insert
	   the same image into the other drive ("Cannot insert same floppy to
	   multiple drives!"), which breaks the mutual-exclusion eviction below. */
	Floppy_SetDiskFileNameNone((int)drive);
	release_materialized(image_index[drive], true);
	ejected[drive] = true;
}

static bool set_drive_eject_state(bool value, unsigned drive)
{
	unsigned other = drive ^ 1;

	if (value)
	{
		eject_drive(drive);
		return true;
	}
	if (!ejected[drive])
		return true;
	/* The same physical image cannot be mounted in both drives at once. */
	bool evicted_other = !ejected[other] && image_index[other] < image_count &&
	                      image_index[other] == image_index[drive];
	if (evicted_other)
		eject_drive(other);
	if (!insert_drive(drive))
	{
		/* Best-effort: don't leave the evicted drive empty on failure. */
		if (evicted_other && insert_drive(other))
			ejected[other] = false;
		return false;
	}
	ejected[drive] = false;
	if (evicted_other)
		notify(other == 0 ? "Hatari: drive A ejected (disk moved to B)" :
		                     "Hatari: drive B ejected (disk moved to A)");
	return true;
}

static bool RETRO_CALLCONV disk_set_eject_state(bool value)
{
	return set_drive_eject_state(value, selected_drive);
}

static bool RETRO_CALLCONV disk_get_eject_state(void)
{
	return ejected[selected_drive];
}

static unsigned RETRO_CALLCONV disk_get_image_index(void)
{
	return image_index[selected_drive];
}

static bool RETRO_CALLCONV disk_set_image_index(unsigned index)
{
	if (!ejected[selected_drive])
		return false;
	image_index[selected_drive] = index;
	return true;
}

static bool RETRO_CALLCONV disk_set_initial_image(unsigned index,
		const char *path)
{
	if (!path)
		return false;

	/* Confirmed against real RetroArch 1.18.0: this is called *before*
	   retro_load_game() (image_count == 0 at this point), matching
	   libretro.h's documented contract. Just stash the preference for
	   RetroDisk_LoadGame()/LoadGameSpecial() to consult once the swap
	   list is actually populated. */
	if (index >= RETRO_DISK_MAX)
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
	if (index >= image_count)
		return false;
	/* Refuse to disturb an image that either drive currently has
	   inserted: shifting the backing array out from under it would
	   make eject-time writeback target the wrong file. */
	if ((!ejected[0] && image_index[0] == index) ||
	    (!ejected[1] && image_index[1] == index))
		return false;
	if (!game)
	{
		release_materialized(index, false);
		memmove(&image_paths[index], &image_paths[index + 1],
		        (image_count - index - 1) * sizeof(image_paths[0]));
		memmove(&materialized_paths[index], &materialized_paths[index + 1],
		        (image_count - index - 1) * sizeof(materialized_paths[0]));
		--image_count;
		/* The old tail slot is now a stale duplicate of the entry that
		   was shifted down into image_count - 1; clear it so a later
		   disk_add_image_index() doesn't inherit a live materialized
		   path under a blank source path. */
		image_paths[image_count][0] = '\0';
		materialized_paths[image_count][0] = '\0';
		if (image_index[0] != RETRO_DISK_MAX && image_index[0] > index)
			--image_index[0];
		if (image_index[1] != RETRO_DISK_MAX && image_index[1] > index)
			--image_index[1];
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

	disk_environment_cb = cb;
	cb(RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE, (void *)&callbacks);
	cb(RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE, (void *)&extended);
}

void RetroDisk_SetActiveDrive(unsigned drive)
{
	selected_drive = drive == 1 ? 1 : 0;
}

void RetroDisk_SetDriveBEnabled(bool enabled)
{
	if (!enabled)
		eject_drive(1);
}

bool RetroDisk_LoadGame(const struct retro_game_info *game)
{
	eject_drive(0);
	eject_drive(1);
	clear_images();
	image_index[0] = image_index[1] = RETRO_DISK_MAX;
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
	/* Drive B starts pointed at the same swap-list entry as drive A, but
	   stays ejected until the user switches the active drive to B and
	   inserts it explicitly; the two drives can never hold the same
	   image at once (see set_drive_eject_state()'s mutual exclusion). Drive
	   A always gets the boot disk on load, independent of whichever drive
	   the "Disk Control target drive" option currently points at. */
	image_index[1] = image_index[0];
	return set_drive_eject_state(false, 0);
}

bool RetroDisk_LoadGameSpecial(const struct retro_game_info *info,
		size_t num_info)
{
	size_t index;

	if (!info || !num_info)
		return false;
	eject_drive(0);
	eject_drive(1);
	clear_images();
	image_index[0] = image_index[1] = RETRO_DISK_MAX;
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
	image_index[1] = image_index[0];
	return set_drive_eject_state(false, 0);
}

void RetroDisk_UnloadGame(void)
{
	eject_drive(0);
	eject_drive(1);
	clear_images();
	image_index[0] = image_index[1] = RETRO_DISK_MAX;
	initial_image = RETRO_DISK_MAX;
	initial_image_path[0] = '\0';
}
