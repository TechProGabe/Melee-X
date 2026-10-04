/* xhw_main.c - Xbox entry point.
 *
 * Boot: mount E:, create the save folder, open boot.log, find the user's disc
 * image next to default.xbe (D:\), show the title card, then hand over to the
 * Dolphin SDK side (xsdk_boot -> OSInit ... video mode ... melee_main), which
 * never returns. */
#include <hal/debug.h>
#include <hal/video.h>
#include <hal/xbox.h>
#include <nxdk/mount.h>
#include <pbkit/pbkit.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>
#include <stdio.h>
#include <string.h>

#include "xhw.h"
#include "xhw_internal.h"

static int ends_ci(const char* s, const char* suf) {
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && _stricmp(s + a - b, suf) == 0;
}

/* First .iso/.gcm/.ciso in a folder, any name. xsdk_boot checks that it
 * really is GALE01 (revision 2 is the target; 0 and 1 are accepted). */
static int find_image_in(const char* dir, char* out, size_t cap) {
    WIN32_FIND_DATAA fd;
    char pattern[MAX_PATH];
    HANDLE h;
    snprintf(pattern, sizeof pattern, "%s\\*", dir);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (ends_ci(fd.cFileName, ".iso") || ends_ci(fd.cFileName, ".gcm") || ends_ci(fd.cFileName, ".ciso")) {
            snprintf(out, cap, "%s\\%s", dir, fd.cFileName);
            FindClose(h);
            return 1;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return 0;
}

/* Next to the XBE. Test builds also take the release folder's image, so a
 * console round can put build variants in folders of their own
 * (F:\Applications\Melee-X-r2a\default.xbe, ...) without a copy of the disc
 * in each (docs/fps-plan.md, round 2). */
static int find_disc_image(char* out, size_t cap) {
    if (find_image_in("D:", out, cap)) return 1;
    if (!XHW_TEST_BUILD) return 0;
    if (!nxIsDriveMounted('F') && !nxMountDrive('F', "\\Device\\Harddisk0\\Partition6\\")) return 0;
    return find_image_in("F:\\Applications\\Melee-X", out, cap);
}

void xhw_error_screen(const char* title, const char* const* lines) {
    int i;
    xhw_watchdog_disable();
    pb_show_debug_screen();
    debugClearScreen();
    debugPrint("\n\n    Melee-X\n\n    %s\n\n", title);
    for (i = 0; lines && lines[i]; i++) debugPrint("    %s\n", lines[i]);
    debugPrint("\n\n    Press any button or wait to return to the dashboard.\n");
}

void xhw_fatal(const char* title, const char* msg) {
    const char* lines[2] = { msg, NULL };
    xhw_logf("[FATAL] %s: %s", title, msg);
    xhw_error_screen(title, lines);
    Sleep(15000);
    xhw_quit_to_dashboard();
}

static void fatal_no_disc(void) {
    static const char* const lines[] = {
        "Melee-X needs your own copy of Super Smash Bros. Melee",
        "for GameCube: NTSC-U 1.02 (GALE01, revision 2).",
        "",
        "Put the disc image in the SAME FOLDER as default.xbe:",
        "",
        "    Melee-X\\default.xbe",
        "    Melee-X\\Melee.iso",
        "",
        "Accepted: .iso  .gcm  .ciso   (any filename)",
        NULL,
    };
    xhw_logf("[BOOT] no disc image in D:\\");
    xhw_error_screen("No disc image found", lines);
    Sleep(30000);
    xhw_quit_to_dashboard();
}

/* XBE header: base address at +0x104, image size at +0x10C (the image is
 * mapped at 0x10000). The crash reporter lists stack words in this range. */
static void read_image_range(void) {
    const unsigned char* xbe = (const unsigned char*)0x00010000;
    xhw_image_base = *(const unsigned int*)(xbe + 0x104);
    xhw_image_end = xhw_image_base + *(const unsigned int*)(xbe + 0x10C);
}

static void main_body(void* arg) {
    char disc[MAX_PATH];
    (void)arg;

    xhw_logf("[BOOT] Melee-X");
    if (!nxIsDriveMounted('E') && !nxMountDrive('E', "\\Device\\Harddisk0\\Partition1\\"))
        xhw_logf("[BOOT] could not mount E: (saves disabled)");
    CreateDirectoryA("E:\\UDATA", NULL);
    CreateDirectoryA(XHW_UDATA_ROOT, NULL);
    xhw_log_open_file();
    read_image_range();
    xhw_logf("[BOOT] image %08x-%08x %.*s", xhw_image_base, xhw_image_end, (int)XeImageFileName[0].Length,
             XeImageFileName[0].Buffer);
    xhw_watchdog_start();
    xhw_prof_set_game_thread();
    xhw_prof_start();
    xhw_perf_calibrate();
    xhw_cpu_probe();
    xhw_mem_log("boot");
    xhw_splash_show();

    if (!find_disc_image(disc, sizeof disc)) fatal_no_disc();
    xhw_logf("[BOOT] disc image %s", disc);
    xhw_autopad_load();
    xhw_splash_progress(0.1f);

    xsdk_early();
    xsdk_boot(disc);   /* sets the video mode once the disc checks out */
}

unsigned xsdk_frame_count(void);
unsigned xhw_frame_count(void) { return xsdk_frame_count(); }

int main(void) {
    xhw_crash_guard(main_body, NULL);
    return 0;
}

void xhw_quit_to_dashboard(void) {
    xhw_led_shutdown();
    xhw_audio_shutdown();
    xhw_pad_shutdown();
    XLaunchXBE(NULL);
    for (;;) Sleep(1000);
}

/* Relaunch this XBE by the kernel's own path for it
 * (\Device\Harddisk0\Partition6\...\default.xbe, \Device\CdRom0\...):
 * XLaunchXBE takes \Device\ paths as they are, while a DOS path like
 * D:\default.xbe would become \??\D:;default.xbe in the launch data. It
 * quick-reboots and doesn't come back; it returns only when the path has
 * no folder in it, and then the console reboots. */
static void __attribute__((noreturn)) launch(char* path) {
    xhw_led_shutdown();
    xhw_audio_shutdown();
    xhw_pad_shutdown();
    XLaunchXBE(path);
    HalReturnToFirmware(HalRebootRoutine);
    for (;;) Sleep(1000);
}

void xhw_reboot_self(void) {
    char path[300];
    const ANSI_STRING* img = &XeImageFileName[0];
    snprintf(path, sizeof path, "%.*s", (int)img->Length, img->Buffer);
    xhw_logf("[BOOT] restart: %s", path);
    launch(path);
}

/* The folder this XBE was launched from (Melee-X-r3a for
 * ...\Applications\Melee-X-r3a\default.xbe); 0 if there is none */
int xhw_image_folder(char* out, int size) {
    const ANSI_STRING* img = &XeImageFileName[0];
    int n = (int)img->Length, end, start;
    for (end = n - 1; end > 0 && img->Buffer[end] != '\\'; end--) {}
    for (start = end - 1; start > 0 && img->Buffer[start - 1] != '\\'; start--) {}
    if (end <= start || start <= 0 || end - start >= size) return 0;
    memcpy(out, img->Buffer + start, (size_t)(end - start));
    out[end - start] = '\0';
    return 1;
}

/* F:\ and E:\ as the kernel names them (xhw_reboot_self: XLaunchXBE wants
 * \Device\ paths); "dashboard": back to the dashboard, as the in-game reset
 * does (a round's last build, so its logs can be pulled over FTP) */
void xhw_launch_xbe(const char* dos_path) {
    char path[300], line[340];
    const char* part = (dos_path[0] | 0x20) == 'f' ? "Partition6" : (dos_path[0] | 0x20) == 'e' ? "Partition1" : NULL;
    if (part && dos_path[1] == ':')
        snprintf(path, sizeof path, "\\Device\\Harddisk0\\%s%s", part, dos_path + 2);
    else
        snprintf(path, sizeof path, "%s", dos_path);
    snprintf(line, sizeof line, "[BOOT] next: %s", path);
    xhw_log_try(line);   /* on disk before the launch */
    {   /* kept as boot_<this build's folder>.log: the next boots rotate boot.log away */
        char folder[64];
        if (xhw_image_folder(folder, sizeof folder)) {
            snprintf(line, sizeof line, "boot_%s.log", folder);
            xhw_log_keep(line);
        }
    }
    if (strcmp(dos_path, "dashboard") == 0) xhw_quit_to_dashboard();
    launch(path);
}
