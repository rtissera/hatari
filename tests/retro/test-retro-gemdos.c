/*
 * Mock-driven tests for the libretro GEMDOS host-directory content
 * adapter (src/retro/gemdos.c).
 *
 * Same rationale as test-retro-harddisk.c: never calls retro_init().
 * GemDOS_InitDrives()/UnInitDrives() only touch ConfigureParams and the
 * adapter's own emudrives[] global array - no CPU/TOS dependency - so
 * it's safe to exercise RetroGemDos_LoadGame()/UnloadGame() directly via
 * dlsym(), pre-init, the same way ACSI/IDE/SCSI hard-disk content is
 * tested.
 *
 * No VFS mock is needed here: this content type is host-filesystem-only
 * by design (a directory has no VFS equivalent to materialize), so both
 * the loader file and the target directory are always plain host paths.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <libretro.h>
#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool (*lr_gemdos_load_game)(const struct retro_game_info *game);
static void (*lr_gemdos_unload_game)(void);
static void (*lr_set_environment)(retro_environment_t cb);

static char scratch_dir[PATH_MAX];

static bool env_cb(unsigned cmd, void *data)
{
	(void)cmd; (void)data;
	return false;
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

static void write_loader_file(const char *loader_path, const char *target_directory)
{
	FILE *file = fopen(loader_path, "w");
	if (!file)
	{
		fprintf(stderr, "Failed to create '%s'\n", loader_path);
		exit(EXIT_FAILURE);
	}
	fprintf(file, "%s\n", target_directory);
	fclose(file);
}

static void check(bool condition, const char *label)
{
	printf("%-65s%s\n", label, condition ? "OK" : "ERROR");
	if (!condition)
		exit(EXIT_FAILURE);
}

int main(int argc, char *argv[])
{
	void *dlh;
	char loader_a[PATH_MAX + 64], loader_b[PATH_MAX + 64], loader_missing[PATH_MAX + 64];
	char target_a[PATH_MAX + 64], target_b[PATH_MAX + 64];
	char not_gemdos[PATH_MAX + 64];

	if (argc != 2)
	{
		printf("Usage: %s <path-to-libretro-hatari.so>\n", argv[0]);
		return 0;
	}

	snprintf(scratch_dir, sizeof(scratch_dir), "/tmp/hatari-retro-gemdos-test-XXXXXX");
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
	lr_gemdos_load_game = must_dlsym(dlh, "RetroGemDos_LoadGame");
	lr_gemdos_unload_game = must_dlsym(dlh, "RetroGemDos_UnloadGame");

	lr_set_environment(env_cb);

	/* --- Scenario A: extension gate rejects non-.gemdos content --- */
	snprintf(not_gemdos, sizeof(not_gemdos), "%s/not-a-loader.txt", scratch_dir);
	write_loader_file(not_gemdos, scratch_dir);
	{
		struct retro_game_info game = { not_gemdos, NULL, 0, NULL };
		check(!lr_gemdos_load_game(&game),
		      "Non-.gemdos extension: RetroGemDos_LoadGame rejects it");
	}

	/* --- Scenario B: loader pointing at a directory that doesn't exist
	   is rejected, without ever calling GemDOS_InitDrives() --- */
	snprintf(loader_missing, sizeof(loader_missing), "%s/missing.gemdos", scratch_dir);
	snprintf(target_a, sizeof(target_a), "%s/does-not-exist", scratch_dir);
	write_loader_file(loader_missing, target_a);
	{
		struct retro_game_info game = { loader_missing, NULL, 0, NULL };
		check(!lr_gemdos_load_game(&game),
		      "Loader pointing at a missing directory: rejected");
	}

	/* --- Scenario C: loader pointing at a real directory mounts --- */
	snprintf(target_a, sizeof(target_a), "%s/drive-a", scratch_dir);
	if (mkdir(target_a, 0755) != 0)
	{
		fprintf(stderr, "mkdir failed\n");
		return EXIT_FAILURE;
	}
	snprintf(loader_a, sizeof(loader_a), "%s/drive-a.gemdos", scratch_dir);
	write_loader_file(loader_a, target_a);
	{
		struct retro_game_info game = { loader_a, NULL, 0, NULL };
		check(lr_gemdos_load_game(&game),
		      "Loader pointing at a real directory: RetroGemDos_LoadGame mounts it");
	}

	/* --- Scenario D: loading a second directory without an explicit
	   unload works cleanly (RetroGemDos_LoadGame unmounts the first
	   itself) --- */
	snprintf(target_b, sizeof(target_b), "%s/drive-b", scratch_dir);
	if (mkdir(target_b, 0755) != 0)
	{
		fprintf(stderr, "mkdir failed\n");
		return EXIT_FAILURE;
	}
	snprintf(loader_b, sizeof(loader_b), "%s/drive-b.gemdos", scratch_dir);
	write_loader_file(loader_b, target_b);
	{
		struct retro_game_info game = { loader_b, NULL, 0, NULL };
		check(lr_gemdos_load_game(&game),
		      "Second directory without explicit unload: loads cleanly");
	}
	lr_gemdos_unload_game();
	/* Unloading twice must not crash. */
	lr_gemdos_unload_game();
	check(true, "Double unload does not crash");

	/* --- Scenario E: after unmounting, a fresh valid load still works
	   (config wasn't left in a bad state) --- */
	{
		struct retro_game_info game = { loader_a, NULL, 0, NULL };
		check(lr_gemdos_load_game(&game),
		      "After unload: a valid loader still mounts cleanly");
	}
	lr_gemdos_unload_game();

	dlclose(dlh);
	{
		char rm_command[PATH_MAX + 64];
		snprintf(rm_command, sizeof(rm_command), "rm -rf '%s'", scratch_dir);
		if (system(rm_command) != 0)
			fprintf(stderr, "Warning: failed to clean up '%s'\n", scratch_dir);
	}
	puts("All GEMDOS-directory tests finished successfully.");
	return 0;
}
