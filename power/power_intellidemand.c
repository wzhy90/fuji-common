/*
 * Copyright (C) 2012 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Power HAL for use with the 'Intellidemand' governor
 * Originally written by Paul Reioux (faux123)
 * Adapted for msm8660 chipsets by Christopher Hesse (RaymanFX)
 * 
 */
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <dlfcn.h>

#define LOG_TAG "PowerHAL"
#include <utils/Log.h>

#include <hardware/hardware.h>
#include <hardware/power.h>

#define SCALINGMAXFREQ_PATH "/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq"
#define SCALING_GOVERNOR_PATH "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"
#define BOOSTPULSE_INTERACTIVE "/sys/devices/system/cpu/cpufreq/interactive/boostpulse"
#define BOOSTPULSE_INTELLIDEMAND "/sys/devices/system/cpu/cpufreq/intellidemand/boostpulse"
#define SAMPLING_RATE_INTELLIDEMAND "/sys/devices/system/cpu/cpufreq/intellidemand/sampling_rate"
#define SAMPLING_RATE_SCREEN_ON "50000"
#define SAMPLING_RATE_SCREEN_OFF "500000"

#define MAX_BUF_SZ  10

/* initialize to something safe */
static char scaling_max_freq[MAX_BUF_SZ] = "1512000";

struct msm8660_power_module {
    struct power_module base;
    pthread_mutex_t lock;
    int boostpulse_fd;
    int boostpulse_warned;
};

static void sysfs_write(char *path, char *s)
{
    char buf[80];
    int len;
    int fd = open(path, O_WRONLY);

    if (fd < 0) {
        strerror_r(errno, buf, sizeof(buf));
        ALOGE("Error opening %s: %s\n", path, buf);
        return;
    }

    len = write(fd, s, strlen(s));
    if (len < 0) {
        strerror_r(errno, buf, sizeof(buf));
        ALOGE("Error writing to %s: %s\n", path, buf);
    }

    close(fd);
}

int sysfs_read(const char *path, char *buf, size_t size)
{
    int fd, len;

    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;

    do {
        len = read(fd, buf, size);
    } while (len < 0 && errno == EINTR);

    close(fd);

    return len;
}

static int get_scaling_governor(char governor[], int size)
{
    if (sysfs_read(SCALING_GOVERNOR_PATH, governor, size) < 0) {
        // Can't obtain the scaling governor. Return.
        return -1;
    } else {
        // Strip newline at the end.
        int len = strlen(governor);

        len--;

        while (len >= 0 && (governor[len] == '\n' || governor[len] == '\r'))
            governor[len--] = '\0';
    }

    return 0;
}

static void msm8660_power_init(struct power_module *module)
{
    sysfs_write(SAMPLING_RATE_INTELLIDEMAND, SAMPLING_RATE_SCREEN_ON);

}

static int boostpulse_open(struct msm8660_power_module *msm8660)
{
    char buf[80];
    char governor[80];

    pthread_mutex_lock(&msm8660->lock);

    if (msm8660->boostpulse_fd < 0) {
        if (get_scaling_governor(governor, sizeof(governor)) < 0) {
            ALOGE("Can't read scaling governor.");
            msm8660->boostpulse_warned = 1;
        } else {
            if (strncmp(governor, "interactive", 11) == 0)
                msm8660->boostpulse_fd = open(BOOSTPULSE_INTERACTIVE, O_WRONLY);
            else if (strncmp(governor, "intellidemand", 13) == 0)
                msm8660->boostpulse_fd = open(BOOSTPULSE_INTELLIDEMAND, O_WRONLY);

            if (msm8660->boostpulse_fd < 0 && !msm8660->boostpulse_warned) {
                strerror_r(errno, buf, sizeof(buf));
                ALOGE("Error opening boostpulse: %s\n", buf);
                msm8660->boostpulse_warned = 1;
            }
        }
    }

    pthread_mutex_unlock(&msm8660->lock);
    return msm8660->boostpulse_fd;
}

static void msm8660_power_set_interactive(struct power_module *module, int on)
{
    sysfs_write(SAMPLING_RATE_INTELLIDEMAND,
            on ? SAMPLING_RATE_SCREEN_ON : SAMPLING_RATE_SCREEN_OFF);
}

static void msm8660_power_hint(struct power_module *module, power_hint_t hint,
                       void *data) {
    struct msm8660_power_module *msm8660 = (struct msm8660_power_module *) module;
    char buf[80];
    int len;
    int duration = 1;

    switch (hint) {
        case POWER_HINT_INTERACTION:
            if (boostpulse_open(msm8660) >= 0) {
                if (data != NULL)
                    duration = (int)data;

                snprintf(buf, sizeof(buf), "%d", duration);
                len = write(msm8660->boostpulse_fd, buf, strlen(buf));

                if (len < 0) {
                    strerror_r(errno, buf, sizeof(buf));
                    ALOGE("Error writing to boostpulse: %s\n", buf);

                    pthread_mutex_lock(&msm8660->lock);
                    close(msm8660->boostpulse_fd);
                    msm8660->boostpulse_fd = -1;
                    msm8660->boostpulse_warned = 0;
                    pthread_mutex_unlock(&msm8660->lock);
                }
            }
            break;

        case POWER_HINT_VSYNC:
            ALOGV("POWER_HINT_VSYNC %s", (data ? "ON" : "OFF"));
            break;

        default:
             break;
    }
}

static struct hw_module_methods_t power_module_methods = {
    .open = NULL,
};

struct msm8660_power_module HAL_MODULE_INFO_SYM = {
    base: {
        common: {
            tag: HARDWARE_MODULE_TAG,
            module_api_version: POWER_MODULE_API_VERSION_0_2,
            hal_api_version: HARDWARE_HAL_API_VERSION,
            id: POWER_HARDWARE_MODULE_ID,
            name: "Qualcomm Power HAL (by RaymanFX)",
            author: "The Android Open Source Project",
            methods: &power_module_methods,
        },

        init: msm8660_power_init,
        setInteractive: msm8660_power_set_interactive,
        powerHint: msm8660_power_hint,
    },
    lock: PTHREAD_MUTEX_INITIALIZER,
    boostpulse_fd: -1,
    boostpulse_warned: 0,
};