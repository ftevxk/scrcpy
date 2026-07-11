#include "common.h"

#include <stdbool.h>
#include <stdio.h>
#ifdef HAVE_V4L2
# include <libavdevice/avdevice.h>
#endif
#include <SDL3/SDL.h>

#include "cli.h"
#include "events.h"
#include "options.h"
#include "scrcpy.h"
#ifdef HAVE_USB
# include "usb/scrcpy_otg.h"
#endif
#include "util/log.h"
#include "util/net.h"
#include "version.h"

#ifdef _WIN32
#include <windows.h>
#include "util/str.h"
#endif

static int
main_scrcpy(int argc, char *argv[]) {
#ifdef _WIN32
    // disable buffering, we want logs immediately
    // even line buffering (setvbuf() with mode _IOLBF) is not sufficient
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);
#endif

    printf("scrcpy " SCRCPY_VERSION
           " <https://github.com/Genymobile/scrcpy>\n");

    struct scrcpy_cli_args args = {
        .opts = scrcpy_options_default,
        .help = false,
        .version = false,
        .pause_on_exit = SC_PAUSE_ON_EXIT_UNDEFINED,
    };

#if defined(SCRCPYW) && defined(_WIN32)
    // scrcpyw is a standalone GUI binary. Killing the adb daemon on close
    // prevents residual device-side scrcpy-server processes and stale
    // forward/reverse tunnels from accumulating across open/close cycles,
    // which otherwise makes the device unreachable after a few restarts.
    // CLI users can still override via explicit options in scrcpy.exe.
    args.opts.kill_adb_on_close = true;
#endif

#ifndef NDEBUG
    args.opts.log_level = SC_LOG_LEVEL_DEBUG;
#endif

    enum scrcpy_exit_code ret;

    if (!scrcpy_parse_args(&args, argc, argv)) {
        ret = SCRCPY_EXIT_FAILURE;
        goto end;
    }

    sc_set_log_level(args.opts.log_level);

    if (args.help) {
        scrcpy_print_usage(argv[0]);
        ret = SCRCPY_EXIT_SUCCESS;
        goto end;
    }

    if (args.version) {
        scrcpy_print_version();
        ret = SCRCPY_EXIT_SUCCESS;
        goto end;
    }

#ifdef SCRCPY_LAVF_REQUIRES_REGISTER_ALL
    av_register_all();
#endif

#ifdef HAVE_V4L2
    if (args.opts.v4l2_device) {
        avdevice_register_all();
    }
#endif

    if (!net_init()) {
        ret = SCRCPY_EXIT_FAILURE;
        goto end;
    }

    sc_log_configure();

    if (!sc_main_thread_init()) {
        ret = SCRCPY_EXIT_FAILURE;
        goto net_cleanup;
    }

#ifdef HAVE_USB
    ret = args.opts.otg ? scrcpy_otg(&args.opts) : scrcpy(&args.opts);
#else
    ret = scrcpy(&args.opts);
#endif

    sc_main_thread_destroy();

net_cleanup:
    net_cleanup();

end:
    if (args.pause_on_exit == SC_PAUSE_ON_EXIT_TRUE ||
            (args.pause_on_exit == SC_PAUSE_ON_EXIT_IF_ERROR &&
                ret != SCRCPY_EXIT_SUCCESS)) {
        printf("Press Enter to continue...\n");
        getchar();
    }

    return ret;
}

int
main(int argc, char *argv[]) {
#ifndef _WIN32
    return main_scrcpy(argc, argv);
#else
    (void) argc;
    (void) argv;
    int wargc;
    wchar_t **wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (!wargv) {
        LOG_OOM();
        return SCRCPY_EXIT_FAILURE;
    }

    char **argv_utf8 = malloc((wargc + 1) * sizeof(*argv_utf8));
    if (!argv_utf8) {
        LOG_OOM();
        LocalFree(wargv);
        return SCRCPY_EXIT_FAILURE;
    }

    argv_utf8[wargc] = NULL;

    for (int i = 0; i < wargc; ++i) {
        argv_utf8[i] = sc_str_from_wchars(wargv[i]);
        if (!argv_utf8[i]) {
            LOG_OOM();
            for (int j = 0; j < i; ++j) {
                free(argv_utf8[j]);
            }
            LocalFree(wargv);
            free(argv_utf8);
            return SCRCPY_EXIT_FAILURE;
        }
    }

    LocalFree(wargv);

    int ret = main_scrcpy(wargc, argv_utf8);

    for (int i = 0; i < wargc; ++i) {
        free(argv_utf8[i]);
    }
    free(argv_utf8);

    return ret;
#endif
}

#if defined(SCRCPYW) && defined(_WIN32)
#include <windows.h>
#include <stdlib.h>
#include <string.h>

// Path of the %TEMP%\scrcpy-<pid> directory holding extracted binaries.
// Kept globally so the atexit handler can delete it on exit.
static char *g_extracted_dir = NULL;

static bool
extract_resource_to_file(const wchar_t *res_name, const char *dest_path) {
    HRSRC hRes = FindResourceW(NULL, res_name, RT_RCDATA);
    if (!hRes) {
        return false;
    }
    HGLOBAL hMem = LoadResource(NULL, hRes);
    if (!hMem) {
        return false;
    }
    DWORD size = SizeofResource(NULL, hRes);
    const void *data = LockResource(hMem);
    if (!data || !size) {
        return false;
    }

    HANDLE hFile = CreateFileA(dest_path, GENERIC_WRITE, 0, NULL,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        return false;
    }

    DWORD written;
    BOOL ok = WriteFile(hFile, data, size, &written, NULL);
    CloseHandle(hFile);

    return ok && written == size;
}

static void
cleanup_extracted_files(void) {
    if (!g_extracted_dir) {
        return;
    }

    char pattern[MAX_PATH];
    int n = snprintf(pattern, sizeof(pattern), "%s\\*", g_extracted_dir);
    if (n <= 0 || (size_t) n >= sizeof(pattern)) {
        goto done;
    }

    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(pattern, &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (fd.cFileName[0] == '.') {
                continue;
            }
            char filepath[MAX_PATH];
            n = snprintf(filepath, sizeof(filepath), "%s\\%s",
                         g_extracted_dir, fd.cFileName);
            if (n <= 0 || (size_t) n >= sizeof(filepath)) {
                continue;
            }
            DeleteFileA(filepath);
        } while (FindNextFileA(hFind, &fd));
        FindClose(hFind);
    }

    RemoveDirectoryA(g_extracted_dir);

done:
    free(g_extracted_dir);
    g_extracted_dir = NULL;
}

// Extract adb.exe, AdbWinApi.dll, AdbWinUsbApi.dll and scrcpy-server from the
// EXE resource section into %TEMP%\scrcpy-<pid>\, then set ADB and
// SCRCPY_SERVER_PATH environment variables so that adb.c and server.c pick
// them up. An atexit handler deletes the directory on exit.
//
// Returns true on success, false on failure (with a MessageBox shown).
static bool
scrcpyw_extract_embedded_binaries(void) {
    char temp_path[MAX_PATH];
    DWORD len = GetTempPathA(MAX_PATH, temp_path);
    if (len == 0 || len >= MAX_PATH) {
        MessageBoxA(NULL, "Could not resolve %TEMP%.", "scrcpy", MB_ICONERROR);
        return false;
    }

    char dir_path[MAX_PATH];
    int n = snprintf(dir_path, sizeof(dir_path), "%sscrcpy-%lu",
                     temp_path, GetCurrentProcessId());
    if (n <= 0 || (size_t) n >= sizeof(dir_path)) {
        MessageBoxA(NULL, "Temporary path too long.", "scrcpy", MB_ICONERROR);
        return false;
    }

    // CreateDirectory fails with ERROR_ALREADY_EXISTS if the dir is there;
    // that's fine (a previous run may have left it).
    if (!CreateDirectoryA(dir_path, NULL)) {
        DWORD err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS) {
            MessageBoxA(NULL, "Could not create temp directory.",
                        "scrcpy", MB_ICONERROR);
            return false;
        }
    }

    g_extracted_dir = strdup(dir_path);
    if (!g_extracted_dir) {
        MessageBoxA(NULL, "Out of memory.", "scrcpy", MB_ICONERROR);
        return false;
    }

    struct {
        WORD res_id;
        const char *name;
    } items[] = {
        { SC_RES_ID_ADB_EXE,         "adb.exe" },
        { SC_RES_ID_ADB_WIN_API_DLL, "AdbWinApi.dll" },
        { SC_RES_ID_ADB_WIN_USB_DLL, "AdbWinUsbApi.dll" },
        { SC_RES_ID_SCRCPY_SERVER,   "scrcpy-server" },
    };

    char dest[MAX_PATH];
    for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); ++i) {
        n = snprintf(dest, sizeof(dest), "%s\\%s", dir_path, items[i].name);
        if (n <= 0 || (size_t) n >= sizeof(dest)) {
            MessageBoxA(NULL, "Path too long.", "scrcpy", MB_ICONERROR);
            return false;
        }
        if (!extract_resource_to_file(items[i].res_id, dest)) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Could not extract %s.", items[i].name);
            MessageBoxA(NULL, msg, "scrcpy", MB_ICONERROR);
            return false;
        }
    }

    // sc_adb_init() reads $ADB; get_server_path() reads $SCRCPY_SERVER_PATH.
    char adb_path[MAX_PATH];
    snprintf(adb_path, sizeof(adb_path), "%s\\adb.exe", dir_path);
    SetEnvironmentVariableA("ADB", adb_path);

    char server_path[MAX_PATH];
    snprintf(server_path, sizeof(server_path), "%s\\scrcpy-server", dir_path);
    SetEnvironmentVariableA("SCRCPY_SERVER_PATH", server_path);

    atexit(cleanup_extracted_files);
    return true;
}

int WINAPI
WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void) hInstance;
    (void) hPrevInstance;
    (void) lpCmdLine;
    (void) nCmdShow;

    FreeConsole();

    // Extract embedded adb.exe + scrcpy-server before anything else so that
    // sc_adb_init() and get_server_path() find them via $ADB and
    // $SCRCPY_SERVER_PATH. If extraction fails, bail out early.
    if (!scrcpyw_extract_embedded_binaries()) {
        return SCRCPY_EXIT_FAILURE;
    }

    int wargc;
    wchar_t **wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (!wargv) {
        LOG_OOM();
        return SCRCPY_EXIT_FAILURE;
    }

    char **argv_utf8 = malloc((wargc + 1) * sizeof(*argv_utf8));
    if (!argv_utf8) {
        LOG_OOM();
        LocalFree(wargv);
        return SCRCPY_EXIT_FAILURE;
    }

    argv_utf8[wargc] = NULL;
    for (int i = 0; i < wargc; ++i) {
        argv_utf8[i] = sc_str_from_wchars(wargv[i]);
        if (!argv_utf8[i]) {
            LOG_OOM();
            for (int j = 0; j < i; ++j) {
                free(argv_utf8[j]);
            }
            LocalFree(wargv);
            free(argv_utf8);
            return SCRCPY_EXIT_FAILURE;
        }
    }
    LocalFree(wargv);

    int ret = main_scrcpy(wargc, argv_utf8);

    for (int i = 0; i < wargc; ++i) {
        free(argv_utf8[i]);
    }
    free(argv_utf8);
    return ret;
}
#endif
