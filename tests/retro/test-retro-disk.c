/*
 * Mock-driven tests for the libretro disk-control and core-options adapter
 * (src/retro/disk.c, src/retro/options.c).
 *
 * Unlike test-retro.c, this deliberately never calls retro_init(): with no
 * TOS image available, Hatari's cold-reset path opens a GUI dialog or calls
 * exit() (see Main_InitSubsystems() in src/main.c). Disk-control and option
 * logic only touch ConfigureParams/EmulationDrives globals and the adapter's
 * own static state, so it is safe to exercise via retro_load_game() and the
 * exported RetroOptions_Apply()/disk-control callback pointers alone.
 *
 * This rests on Log_AlertDlg() (used by e.g. Floppy_SetDiskFileName() on a
 * rejected insert) not opening a GUI dialog or blocking when no display/GUI
 * subsystem has been initialized: observed to just print and return here,
 * but re-check this comment if these tests ever start hanging.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <libretro.h>
#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void (*lr_set_environment)(retro_environment_t cb);
static bool (*lr_load_game)(const struct retro_game_info *game);
static void (*lr_unload_game)(void);
static void (*lr_options_apply)(void);

static const struct retro_disk_control_ext_callback *disk_ext;

static char scratch_dir[PATH_MAX];
static char last_message[512];

/* --- mock core-option key/value store, set per scenario --- */
#define MAX_MOCK_VARS 8
static struct { const char *key; const char *value; } mock_vars[MAX_MOCK_VARS];

static void mock_set_var(const char *key, const char *value)
{
	int i;
	for (i = 0; i < MAX_MOCK_VARS; ++i)
	{
		if (!mock_vars[i].key || !strcmp(mock_vars[i].key, key))
		{
			mock_vars[i].key = key;
			mock_vars[i].value = value;
			return;
		}
	}
	abort(); /* table too small, grow MAX_MOCK_VARS */
}

/* --- mock VFS: serves "vfsmock:<name>" by opening <scratch_dir>/<name> --- */
struct mock_vfs_handle { FILE *fp; };

static struct retro_vfs_file_handle *mock_vfs_open(const char *path,
		unsigned mode, unsigned hints)
{
	struct mock_vfs_handle *handle;
	const char *prefix = "vfsmock:";
	char real_path[PATH_MAX + 64];
	(void)hints;

	if (strncmp(path, prefix, strlen(prefix)) != 0)
		return NULL;
	snprintf(real_path, sizeof(real_path), "%s/%s", scratch_dir,
	         path + strlen(prefix));
	handle = malloc(sizeof(*handle));
	if (!handle)
		return NULL;
	handle->fp = fopen(real_path, (mode & RETRO_VFS_FILE_ACCESS_WRITE) ?
	                    "wb" : "rb");
	if (!handle->fp)
	{
		free(handle);
		return NULL;
	}
	return (struct retro_vfs_file_handle *)handle;
}

static int mock_vfs_close(struct retro_vfs_file_handle *stream)
{
	struct mock_vfs_handle *handle = (struct mock_vfs_handle *)stream;
	fclose(handle->fp);
	free(handle);
	return 0;
}

static int64_t mock_vfs_size(struct retro_vfs_file_handle *stream)
{
	struct mock_vfs_handle *handle = (struct mock_vfs_handle *)stream;
	long current = ftell(handle->fp);
	long size;
	fseek(handle->fp, 0, SEEK_END);
	size = ftell(handle->fp);
	fseek(handle->fp, current, SEEK_SET);
	return size;
}

static int64_t mock_vfs_read(struct retro_vfs_file_handle *stream, void *s,
		uint64_t len)
{
	struct mock_vfs_handle *handle = (struct mock_vfs_handle *)stream;
	return (int64_t)fread(s, 1, (size_t)len, handle->fp);
}

static int64_t mock_vfs_write(struct retro_vfs_file_handle *stream,
		const void *s, uint64_t len)
{
	struct mock_vfs_handle *handle = (struct mock_vfs_handle *)stream;
	return (int64_t)fwrite(s, 1, (size_t)len, handle->fp);
}

static struct retro_vfs_interface mock_vfs = {
	.open = mock_vfs_open,
	.close = mock_vfs_close,
	.size = mock_vfs_size,
	.read = mock_vfs_read,
	.write = mock_vfs_write,
};

static bool env_cb(unsigned cmd, void *data)
{
	switch (cmd)
	{
	 case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
	 case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
	 case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
	 case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
	 case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
	 case RETRO_ENVIRONMENT_SET_VARIABLES:
	 case RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK:
	 case RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO:
		return true;
	 case RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE:
		return true;
	 case RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE:
		disk_ext = (const struct retro_disk_control_ext_callback *)data;
		return true;
	 case RETRO_ENVIRONMENT_GET_VARIABLE:
	 {
		struct retro_variable *variable = (struct retro_variable *)data;
		int i;
		variable->value = NULL;
		for (i = 0; i < MAX_MOCK_VARS; ++i)
			if (mock_vars[i].key && !strcmp(mock_vars[i].key, variable->key))
			{
				variable->value = mock_vars[i].value;
				break;
			}
		return true;
	 }
	 case RETRO_ENVIRONMENT_GET_VFS_INTERFACE:
	 {
		struct retro_vfs_interface_info *info =
			(struct retro_vfs_interface_info *)data;
		if (info->required_interface_version > 3)
			return false;
		info->iface = &mock_vfs;
		return true;
	 }
	 case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
		*(const char **)data = scratch_dir;
		return true;
	 case RETRO_ENVIRONMENT_SET_MESSAGE_EXT:
	 {
		const struct retro_message_ext *msg =
			(const struct retro_message_ext *)data;
		snprintf(last_message, sizeof(last_message), "%s", msg->msg);
		return true;
	 }
	 case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
		return false;
	 default:
		return false;
	}
}

static void *must_dlsym(void *dlh, const char *name)
{
	void *fn = dlsym(dlh, name);
	if (!fn)
	{
		fprintf(stderr, "Failed to get symbol '%s': %s\n", name, dlerror());
		exit(EXIT_FAILURE);
	}
	return fn;
}

static void write_zero_file(const char *path, size_t size)
{
	FILE *file = fopen(path, "wb");
	static const char zeros[64 * 1024] = { 0 };
	size_t remaining = size;

	if (!file)
	{
		fprintf(stderr, "Failed to create '%s'\n", path);
		exit(EXIT_FAILURE);
	}
	while (remaining)
	{
		size_t chunk = remaining < sizeof(zeros) ? remaining : sizeof(zeros);
		fwrite(zeros, 1, chunk, file);
		remaining -= chunk;
	}
	fclose(file);
}

static void check(bool condition, const char *label)
{
	printf("%-55s%s\n", label, condition ? "OK" : "ERROR");
	if (!condition)
		exit(EXIT_FAILURE);
}

/* A blank, correctly-sized 720K double-sided 9-sector ST image. Hatari's
   ST reader only validates geometry from file size, not content. */
#define ST_IMAGE_SIZE (720 * 1024)

int main(int argc, char *argv[])
{
	void *dlh;
	char path_a[PATH_MAX + 64], path_b[PATH_MAX + 64];
	char playlist_path[PATH_MAX + 64], vfs_backing[PATH_MAX + 64];

	if (argc != 2)
	{
		printf("Usage: %s <path-to-libretro-hatari.so>\n", argv[0]);
		return 0;
	}

	snprintf(scratch_dir, sizeof(scratch_dir), "/tmp/hatari-retro-disk-test-XXXXXX");
	if (!mkdtemp(scratch_dir))
	{
		fprintf(stderr, "mkdtemp failed\n");
		return EXIT_FAILURE;
	}

	dlh = dlopen(argv[1], RTLD_NOW);
	if (!dlh)
	{
		fprintf(stderr, "dlopen: %s\n", dlerror());
		return EXIT_FAILURE;
	}
	lr_set_environment = must_dlsym(dlh, "retro_set_environment");
	lr_load_game = must_dlsym(dlh, "retro_load_game");
	lr_unload_game = must_dlsym(dlh, "retro_unload_game");
	lr_options_apply = must_dlsym(dlh, "RetroOptions_Apply");

	lr_set_environment(env_cb);
	check(disk_ext != NULL, "Disk control ext interface captured");

	/* --- Scenario A: single-file load lands on drive A, inserted --- */
	snprintf(path_a, sizeof(path_a), "%s/disk-a.st", scratch_dir);
	write_zero_file(path_a, ST_IMAGE_SIZE);
	{
		struct retro_game_info game = { path_a, NULL, 0, NULL };
		check(lr_load_game(&game), "Single .st file: retro_load_game succeeds");
	}
	check(disk_ext->get_num_images() == 1, "Single file: one swap-list entry");
	check(disk_ext->get_image_index() == 0, "Single file: drive A on index 0");
	check(!disk_ext->get_eject_state(), "Single file: drive A auto-inserted");
	lr_unload_game();

	/* --- Scenario B: M3U playlist with two entries --- */
	snprintf(path_a, sizeof(path_a), "%s/disk1.st", scratch_dir);
	snprintf(path_b, sizeof(path_b), "%s/disk2.st", scratch_dir);
	write_zero_file(path_a, ST_IMAGE_SIZE);
	write_zero_file(path_b, ST_IMAGE_SIZE);
	snprintf(playlist_path, sizeof(playlist_path), "%s/set.m3u", scratch_dir);
	{
		FILE *m3u = fopen(playlist_path, "w");
		fprintf(m3u, "disk1.st\ndisk2.st\n");
		fclose(m3u);
	}
	{
		struct retro_game_info game = { playlist_path, NULL, 0, NULL };
		check(lr_load_game(&game), "M3U playlist: retro_load_game succeeds");
	}
	check(disk_ext->get_num_images() == 2, "M3U playlist: two swap-list entries");
	check(!disk_ext->get_eject_state(), "M3U playlist: drive A auto-inserted");

	/* --- Scenario C: active-drive option + A/B mutual exclusion --- */
	mock_set_var("hatari_disk_active_drive", "b");
	mock_set_var("hatari_drive_b", "enabled");
	lr_options_apply();
	check(disk_ext->get_eject_state(),
	      "Active drive B: starts ejected (only A was auto-inserted)");
	check(disk_ext->set_image_index(0),
	      "Active drive B: set_image_index(0) while ejected");
	last_message[0] = '\0';
	check(disk_ext->set_eject_state(false),
	      "Active drive B: insert index 0 (same as drive A's)");
	check(!disk_ext->get_eject_state(), "Active drive B: now inserted");
	check(last_message[0] != '\0',
	      "Active drive B: eviction notification fired");
	mock_set_var("hatari_disk_active_drive", "a");
	lr_options_apply();
	check(disk_ext->get_eject_state(),
	      "Active drive A: ejected after being evicted by B's insert");

	/* --- Scenario D: disabling drive B ejects it, even while inaccessible --- */
	mock_set_var("hatari_drive_b", "disabled");
	mock_set_var("hatari_disk_active_drive", "b");
	lr_options_apply();
	check(disk_ext->get_eject_state(),
	      "Drive B disabled: active-drive option ignored, targets A (ejected)");
	mock_set_var("hatari_drive_b", "enabled");
	lr_options_apply();
	check(disk_ext->get_eject_state(),
	      "Drive B re-enabled: was ejected while disabled, stays ejected");

	/* --- Scenario E: can't delete/replace an image a drive still holds --- */
	check(disk_ext->set_eject_state(false),
	      "Drive B: re-insert index 0 for the replace-guard check");
	check(!disk_ext->replace_image_index(0, NULL),
	      "Replace guard: delete of an inserted index is rejected");
	check(disk_ext->get_num_images() == 2,
	      "Replace guard: rejected delete left the swap list untouched");
	check(disk_ext->set_eject_state(true), "Drive B: eject before delete");
	check(disk_ext->replace_image_index(0, NULL),
	      "Replace guard: delete of an ejected, unheld index succeeds");
	check(disk_ext->get_num_images() == 1,
	      "Replace guard: swap list shrank by one");
	lr_unload_game();

	/* --- Scenario F: VFS-only content is materialized through the mock VFS --- */
	/* The boot disk always lands on drive A regardless of the active-drive
	   option (see set_drive_eject_state(false, 0) in RetroDisk_LoadGame()),
	   but querying eject state below still goes through whichever drive
	   the option last pointed the disk-control tray at (still B, from
	   Scenario E) - point it back at A first so the check below means
	   what its label says. */
	mock_set_var("hatari_disk_active_drive", "a");
	lr_options_apply();
	snprintf(vfs_backing, sizeof(vfs_backing), "%s/vfs-disk.st", scratch_dir);
	write_zero_file(vfs_backing, ST_IMAGE_SIZE);
	{
		struct retro_game_info game = { "vfsmock:vfs-disk.st", NULL, 0, NULL };
		check(lr_load_game(&game),
		      "VFS-only content: retro_load_game materializes and inserts");
	}
	check(!disk_ext->get_eject_state(), "VFS-only content: drive A inserted");
	lr_unload_game();

	dlclose(dlh);
	puts("All disk-control tests finished successfully.");
	return 0;
}
