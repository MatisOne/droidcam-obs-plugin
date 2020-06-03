/*
Copyright (C) 2020 github.com/aramg

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net.h"
#include "command.h"
#include "usb_util.h"

bool process_check_success(process_t proc, const char *name) {
    if (proc == PROCESS_NONE) {
        elog("Could not execute \"%s\"", name);
        return false;
    }
    exit_code_t exit_code;
    if (!cmd_simple_wait(proc, &exit_code)) {
        if (exit_code != NO_EXIT_CODE) {
            elog("\"%s\" exit value %" PRIexitcode, name, exit_code);
        } else {
            elog("\"%s\" exited unexpectedly with %d", name, (int) exit_code);
        }
        return false;
    }
    return true;
}

// serialize argv to string "[arg1], [arg2], [arg3]"
bool
argv_to_string(const char *const *argv, char *buf, size_t bufsize) {
    bool truncated = false;
    size_t idx = 0;
    while (*argv && !truncated) {
        const char *arg = *argv;
        size_t len = strlen(arg);

        if (idx + len + 1 >= bufsize) {
            truncated = true;
            len = bufsize - idx - 2;
        }
        memcpy(&buf[idx], arg, len);
        idx += len;
        buf[idx++] = ' ';
        argv++;
    }
    if (idx > 0) idx--; // overwrite the last space
    buf[idx] = '\0';

    return truncated;
}

void
process_print_error(enum process_result err, const char *const argv[]) {
    char buf[256];
    switch (err) {
        case PROCESS_ERROR_GENERIC:
            argv_to_string(argv, buf, sizeof(buf));
            elog("failed to exec: %s", buf);
            break;
        case PROCESS_ERROR_MISSING_BINARY:
            argv_to_string(argv, buf, sizeof(buf));
            elog("command not found: %s", buf);
            break;
        case PROCESS_SUCCESS:
            break;
    }
}

// adb commands
static const char *adb_exe =
#ifdef _WIN32
#ifdef TEST
    ".\\build\\adbz.exe";
#else
    ".\\adb\\adb.exe";
#endif /* TEST */
#else
#ifdef TEST
    "/tmp/adbz";
#else
    "adb";
#endif /* TEST */
#endif

process_t
adb_execute(const char *serial, const char *const adb_cmd[], size_t len, char *output, size_t out_size) {
    const char *cmd[32];
    int i;
    process_t process;
    if (len > sizeof(cmd)-4) {
        elog("max 32 command args allowed");
        return PROCESS_NONE;
    }
    cmd[0] = adb_exe;
    if (serial) {
        cmd[1] = "-s";
        cmd[2] = serial;
        i = 3;
    } else {
        i = 1;
    }

    memcpy(&cmd[i], adb_cmd, len * sizeof(const char *));
    cmd[len + i] = NULL;

    enum process_result r = cmd_execute(cmd[0], cmd, &process, output, out_size);
    if (r != PROCESS_SUCCESS) {
        process_print_error(r, cmd);
        return PROCESS_NONE;
    }
    return process;
}

AdbMgr::AdbMgr() {
    int i = 0;
    for (; i < DEVICES_LIMIT; i++) deviceList[i] = NULL;

    const char *ss[] = {"start-server"};
    process_t proc = adb_execute(NULL, ss, ARRAY_LEN(ss), NULL, 0);
    process_check_success(proc, "adb start-server");
}

// FIXME what if adb is not installed
// FIXME cross check adb code with the windows client
AdbMgr::~AdbMgr() {
    int i = 0;
    for (; i < DEVICES_LIMIT; i++) if(deviceList[i]) delete deviceList[i];
}

bool AdbMgr::Reload(void) {
    char buf[1024];
    AdbDevice dev;
    process_t proc;

    const char *ro[] = {"reconnect", "offline"};
    proc = adb_execute(NULL, ro, ARRAY_LEN(ro), NULL, 0);
    if (!process_check_success(proc, "adb r.o.")) {
        return false;
    }

    const char *dd[] = {"devices"};
    proc = adb_execute(NULL, dd, ARRAY_LEN(dd), buf, sizeof(buf));
    if (!process_check_success(proc, "adb devices")) {
        return false;
    }

    size_t i = 0, len;
    char *n, *sep;
    char *p = strtok_r(buf, "\n", &n);
    do {
        dlog(": %s", p);
        if (strstr(p, "List of") != NULL){
            continue;
        }

        memset(dev.serial, 0, sizeof(dev.serial));
        memset(dev.state, 0, sizeof(dev.state));

        // eg. 00a3a5185d8ac3b1  device
        sep = strchr(p, ' ');
        if (!sep) {
            sep = strchr(p, '\t');
            if (!sep) break;
        }
        len = sep - p;
        if (len <= 0) continue;
        if (len > (sizeof(dev.serial)-1)) len = sizeof(dev.serial)-1;
        p[len] = 0;
        memcpy(dev.serial, p, len);

        p = sep + 1;
        while (*p != '\r' && *p != '\0' && (*p == ' ' || *p == '\t')) { p++; }
        sep = strchr(p, '\0');
        if (!sep) break;
        len = sep - p;
        if (len <= 0) continue;
        if (len > (sizeof(dev.state)-1)) len = sizeof(dev.state)-1;
        memcpy(dev.state, p, len);

        if (deviceList[i] == NULL)
            deviceList[i] = new AdbDevice();

        memcpy(deviceList[i]->serial, dev.serial, sizeof(dev.serial));
        memcpy(deviceList[i]->state, dev.state, sizeof(dev.state));
        memset(deviceList[i]->model, 0, sizeof(dev.model));
        if (++i == DEVICES_LIMIT) break;
    } while ((p = strtok_r(NULL, "\n", &n)) != NULL);

    return true;
}

static void GetModel(AdbDevice *dev) {
    char buf[1024] = {0};
    process_t proc;
    const char *ro[] = {"shell", "getprop", "ro.product.model"};
    proc = adb_execute(dev->serial, ro, ARRAY_LEN(ro), buf, sizeof(buf));
    if (process_check_success(proc, "adb get model")) {
        char *p = buf;
        char *end = buf + sizeof(dev->model) - 2;
        while (p < end && (isalnum(*p) || *p == ' ' || *p == '-' || *p == '_')) p++;
        memcpy(dev->model, buf, p - buf);
        dlog("model: %s", dev->model);
    }
}

AdbDevice* AdbMgr::NextDevice(int *is_offline) {
    const char* offline = "offline";

    if (iter >= DEVICES_LIMIT) iter = 0;

    if (deviceList[iter]) {
        if (memcmp(offline, deviceList[iter]->state, sizeof(offline)-1) == 0) {
            dlog("device %s is offline", deviceList[iter]->serial);
            *is_offline = 1;
        } else {
            *is_offline = 0;
        }

        AdbDevice* dev = deviceList[iter];
        iter++;
        if (!*is_offline && dev->model[0] == 0)
           GetModel(dev);
        return dev;
    }
    return 0;
}

bool
adb_forward(const char *serial, int local_port, int remote_port) {
    char local[32];
    char remote[32];
    snprintf(local, 32, "tcp:%d", local_port);
    snprintf(remote, 32, "tcp:%d", remote_port);

    const char *const cmd[] = {"forward", local, remote};
    process_t proc = adb_execute(serial, cmd, ARRAY_LEN(cmd), NULL, 0);
    return process_check_success(proc, "adb fwd");
}

void
adb_forward_remove_all(const char *serial) {
    const char *const cmd[] = {"forward", "--remove-all"};
    process_t proc = adb_execute(serial, cmd, ARRAY_LEN(cmd), NULL, 0);
    process_check_success(proc, "adb fwd clear");
    return;
}

// MARK: USBMUX

USBMux::USBMux() {
#ifdef _WIN32
    hModule = LoadLibrary("usbmuxd.dll");
    if (!hModule) {
        elog("Error loading usbmuxd.dll");
        return;
    }
    usbmuxd_set_debug_level  = (libusbmuxd_set_debug_level_t) GetProcAddress(hModule, "libusbmuxd_set_debug_level");
    usbmuxd_get_device_list  = (usbmuxd_get_device_list_t   ) GetProcAddress(hModule, "usbmuxd_get_device_list");
    usbmuxd_device_list_free = (usbmuxd_device_list_free_t  ) GetProcAddress(hModule, "usbmuxd_device_list_free");
    usbmuxd_connect          = (usbmuxd_connect_t           ) GetProcAddress(hModule, "usbmuxd_connect");
    usbmuxd_disconnect       = (usbmuxd_disconnect_t        ) GetProcAddress(hModule, "usbmuxd_disconnect");
#endif
    // usbmuxd_set_debug_level(9);
}

USBMux::~USBMux() {
#ifdef _WIN32
    if (hModule) {
        usbmuxd_device_list_free(&deviceList);
        FreeLibrary(hModule);
    }
#endif
}

int USBMux::Reload(void) {
#ifdef __APPLE__
    deviceCount = 0;
    return 0;
#endif
#ifdef _WIN32
    if (!hModule) {
        deviceCount = 0;
        return 0;
    }
#endif

    usbmuxd_device_list_free(&deviceList);
    deviceCount = usbmuxd_get_device_list(&deviceList);
    dlog("USBMux: Reload: %d devices", deviceCount);
    if (deviceCount < 0) {
        dlog("Could not get iOS device list, usbmuxd not running?");
        deviceCount = 0;
        return 0;
    }
    return 1;
}

usbmuxd_device_info_t* USBMux::NextDevice(void) {
    if (iter >= deviceCount) return 0;
    return &deviceList[iter++];
}

int USBMux::Connect(int device, int port) {
#ifdef __APPLE__
    return INVALID_SOCKET;
#else
    int rc;
    dlog("USBMUX Connect: dev=%d/%d, port=%d", device, deviceCount, port);

#ifdef _WIN32
    if (!hModule) {
        elog("USBMUX dll not loaded");
        goto out;
    }
#endif

    if (device < deviceCount) {
        rc = usbmuxd_connect(deviceList[device].handle, (short) port);
        if (rc <= 0) {
            elog("usbmuxd_connect failed: %d", rc);
            goto out;
        }
        return rc;
    }
out:
    return INVALID_SOCKET;
#endif // __APPLE__
}
