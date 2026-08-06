/*
 * Mock-driven tests for the libretro ACSI hard-disk content adapter
 * (src/retro/harddisk.c).
 *
 * Same rationale as test-retro-disk.c: never calls retro_init(). HDC_Init()/
 * HDC_InitDevice() only open the image file, check its size, and touch
 * ConfigureParams/global ACSI bus state - no CPU/TOS dependency - so it's
 * safe to exercise RetroHardDisk_LoadGame()/UnloadGame() directly via
 * dlsym(), pre-init, the same way disk-control was found to be testable.
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

static bool (*lr_hd_load_game)(const struct retro_game_info *game);
static void (*lr_hd_unload_game)(void);
static void (*lr_set_environment)(retro_environment_t cb);

static char scratch_dir[PATH_MAX];

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

static void write_pattern_file(const char *path, size_t size)
{
	FILE *file = fopen(path, "wb");
	unsigned char buffer[64 * 1024];
	size_t remaining = size;
	size_t offset = 0;

	if (!file)
	{
		fprintf(stderr, "Failed to create '%s'\n", path);
		exit(EXIT_FAILURE);
	}
	while (remaining)
	{
		size_t chunk = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
		size_t i;
		for (i = 0; i < chunk; ++i)
			buffer[i] = (unsigned char)((offset + i) % 251);
		fwrite(buffer, 1, chunk, file);
		remaining -= chunk;
		offset += chunk;
	}
	fclose(file);
}

static bool file_matches_pattern(const char *path, size_t size)
{
	FILE *file = fopen(path, "rb");
	unsigned char buffer[64 * 1024];
	size_t remaining = size;
	size_t offset = 0;
	bool ok = true;

	if (!file)
		return false;
	while (remaining && ok)
	{
		size_t chunk = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
		size_t i;
		if (fread(buffer, 1, chunk, file) != chunk)
		{
			ok = false;
			break;
		}
		for (i = 0; i < chunk; ++i)
			if (buffer[i] != (unsigned char)((offset + i) % 251))
			{
				ok = false;
				break;
			}
		remaining -= chunk;
		offset += chunk;
	}
	if (ok && fgetc(file) != EOF)
		ok = false; /* file must be exactly `size` bytes, no more */
	fclose(file);
	return ok;
}

static void write_sparse_file(const char *path, int64_t size)
{
	FILE *file = fopen(path, "wb");
	if (!file || fseek(file, size - 1, SEEK_SET) != 0 ||
	    fputc(0, file) == EOF || fclose(file) != 0)
	{
		fprintf(stderr, "Failed to create sparse file '%s'\n", path);
		exit(EXIT_FAILURE);
	}
}

static void check(bool condition, const char *label)
{
	printf("%-55s%s\n", label, condition ? "OK" : "ERROR");
	if (!condition)
		exit(EXIT_FAILURE);
}

/* HDC_CheckAndGetSize() only requires non-empty and a multiple of the
   512-byte block size; content is irrelevant to mounting. */
#define HD_IMAGE_SIZE (512 * 100)
#define MATERIALIZE_LIMIT (256 * 1024 * 1024)

int main(int argc, char *argv[])
{
	void *dlh;
	char path_a[PATH_MAX + 64], path_b[PATH_MAX + 64];
	char vfs_backing[PATH_MAX + 64], huge_backing[PATH_MAX + 64];

	if (argc != 2)
	{
		printf("Usage: %s <path-to-libretro-hatari.so>\n", argv[0]);
		return 0;
	}

	snprintf(scratch_dir, sizeof(scratch_dir), "/tmp/hatari-retro-hd-test-XXXXXX");
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
	lr_hd_load_game = must_dlsym(dlh, "RetroHardDisk_LoadGame");
	lr_hd_unload_game = must_dlsym(dlh, "RetroHardDisk_UnloadGame");

	/* RetroDisk_SetEnvironment()/RetroOptions_SetEnvironment() also run as
	   part of retro_set_environment(); harmless here, we just don't
	   capture their callbacks since this test doesn't need them. */
	lr_set_environment(env_cb);

	/* --- Scenario A: extension gate rejects non-HD content --- */
	snprintf(path_a, sizeof(path_a), "%s/not-a-disk.txt", scratch_dir);
	write_zero_file(path_a, HD_IMAGE_SIZE);
	{
		struct retro_game_info game = { path_a, NULL, 0, NULL };
		check(!lr_hd_load_game(&game),
		      "Non-HD extension: RetroHardDisk_LoadGame rejects it");
	}

	/* --- Scenario B: plain host .hdf file mounts directly --- */
	snprintf(path_a, sizeof(path_a), "%s/disk-a.hdf", scratch_dir);
	write_zero_file(path_a, HD_IMAGE_SIZE);
	{
		struct retro_game_info game = { path_a, NULL, 0, NULL };
		check(lr_hd_load_game(&game),
		      "Host .hdf file: RetroHardDisk_LoadGame mounts it");
	}

	/* --- Scenario C: loading a second image without an explicit unload
	   works (RetroHardDisk_LoadGame() unmounts the previous one itself) --- */
	snprintf(path_b, sizeof(path_b), "%s/disk-b.hdf", scratch_dir);
	write_zero_file(path_b, HD_IMAGE_SIZE);
	{
		struct retro_game_info game = { path_b, NULL, 0, NULL };
		check(lr_hd_load_game(&game),
		      "Second .hdf without explicit unload: loads cleanly");
	}
	lr_hd_unload_game();
	/* Unloading twice must not crash. */
	lr_hd_unload_game();
	check(true, "Double unload does not crash");

	/* --- Scenario D: VFS-only content is materialized through the mock VFS,
	   and the whole-image writeback on unload round-trips byte-for-byte --- */
	snprintf(vfs_backing, sizeof(vfs_backing), "%s/vfs-disk.hdf", scratch_dir);
	write_pattern_file(vfs_backing, HD_IMAGE_SIZE);
	{
		struct retro_game_info game = { "vfsmock:vfs-disk.hdf", NULL, 0, NULL };
		check(lr_hd_load_game(&game),
		      "VFS-only .hdf content: materializes and mounts");
	}
	lr_hd_unload_game();
	check(file_matches_pattern(vfs_backing, HD_IMAGE_SIZE),
	      "VFS-only .hdf content: writeback round-trips unchanged");

	/* --- Scenario E: content over the materialize size limit is rejected,
	   without ever reading its (sparse, mostly-absent) body, and doesn't
	   corrupt ConfigureParams.Acsi[0] for the next, valid load --- */
	snprintf(huge_backing, sizeof(huge_backing), "%s/huge.hdf", scratch_dir);
	write_sparse_file(huge_backing, (int64_t)MATERIALIZE_LIMIT + 512);
	{
		struct retro_game_info game = { "vfsmock:huge.hdf", NULL, 0, NULL };
		check(!lr_hd_load_game(&game),
		      "Oversized VFS-only content: rejected at the size check");
	}
	lr_hd_unload_game();
	{
		struct retro_game_info game = { path_a, NULL, 0, NULL };
		check(lr_hd_load_game(&game),
		      "After oversized rejection: a valid host file still mounts");
	}
	lr_hd_unload_game();

	dlclose(dlh);
	{
		char rm_command[PATH_MAX + 64];
		snprintf(rm_command, sizeof(rm_command), "rm -rf '%s'", scratch_dir);
		if (system(rm_command) != 0)
			fprintf(stderr, "Warning: failed to clean up '%s'\n", scratch_dir);
	}
	puts("All hard-disk tests finished successfully.");
	return 0;
}
