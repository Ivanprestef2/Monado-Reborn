#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "settings.h"
#include "services/romfs_virtual.h"

Settings g_settings;
bool kbicons_wanted(const char *path) { (void)path; return false; }
bool kbicons_init(void) { return false; }
uint8_t *kbicons_make(const char *path, uint64_t *length) { (void)path; (void)length; return NULL; }
void plat_print_backtrace(void) {}
void plat_will_write(uint64_t address, uint64_t size) { (void)address; (void)size; }
void rt_logf(const char *format, ...) { (void)format; }
void settings_load(void) {}
bool settings_mod_enabled(const char *name) { (void)name; return false; }
int settings_mod_priority(const char *name) { (void)name; return 0; }
uint64_t vk_frame_counter(void) { return 0; }

int main(void) {
    char directory[] = "build/test-romfs-XXXXXX";
    assert(mkdtemp(directory));
    char asset[256], second[256], why[256];
    snprintf(asset, sizeof asset, "%s/asset", directory);
    snprintf(second, sizeof second, "%s/second", directory);
    FILE *file = fopen(asset, "wb");
    assert(file);
    assert(fputs("abcdefgh", file) >= 0 && fclose(file) == 0);
    file = fopen(second, "wb");
    assert(file);
    assert(fputs("second", file) >= 0 && fclose(file) == 0);
    assert(setenv("XCDE_NO_MODS", "1", 1) == 0);
    assert(rvfs_init(directory, why, sizeof why));

    // The first file starts at 0x200 and the next is aligned to 0x210.
    uint8_t data[16];
    assert(rvfs_read(data, sizeof data, 0x200) == sizeof data);
    assert(!memcmp(data, "abcdefgh\0\0\0\0\0\0\0\0", sizeof data));
    assert(truncate(asset, 3) == 0);
    assert(rvfs_read(data, 8, 0x200) == -1 && errno == EIO);
    assert(rvfs_read(data, 6, 0x210) == 6 && !memcmp(data, "second", 6));

    assert(unlink(asset) == 0 && unlink(second) == 0 && rmdir(directory) == 0);
    puts("virtual RomFS reads and padding: OK");
    return 0;
}
