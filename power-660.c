/*
 * Copyright (c) 2017, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * *    * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name of The Linux Foundation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define LOG_NIDEBUG 0

#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#define LOG_TAG "QTI PowerHAL"
#include <utils/Log.h>
#include <hardware/hardware.h>
#include <hardware/power.h>

#include "utils.h"
#include "metadata-defs.h"
#include "hint-data.h"
#include "performance.h"
#include "power-common.h"

#define MIN_VAL(X,Y) ((X>Y)?(Y):(X))

static int saved_interactive_mode = -1;
static int display_hint_sent;
static int video_encode_hint_sent;
static int cam_preview_hint_sent;

pthread_mutex_t camera_hint_mutex = PTHREAD_MUTEX_INITIALIZER;
static int camera_hint_ref_count;
static void process_video_encode_hint(void *metadata);
//static void process_cam_preview_hint(void *metadata);

const int kMinInteractiveDuration = 400;  /* ms */
const int kMaxInteractiveDuration = 5000; /* ms */
const int kMaxLaunchDuration = 5000;      /* ms */

/* Returns true is target is SDM630/SDM455 else false*/
static bool is_target_SDM630()
{
    int fd;
    bool is_target_SDM630=false;
    char buf[10] = {0};
    fd = open("/sys/devices/soc0/soc_id", O_RDONLY);
    if (fd >= 0) {
        if (read(fd, buf, sizeof(buf) - 1) == -1) {
            ALOGW("Unable to read soc_id");
            is_target_SDM630 = false;
        } else {
            int soc_id = atoi(buf);
            if (soc_id == 318 || soc_id == 327 || soc_id == 385) {
                is_target_SDM630 = true; /* Above SOCID for SDM630/SDM455 */
            }
        }
    }
    close(fd);
    return is_target_SDM630;
}

static void process_interaction_hint(void* data) {
    static struct timespec s_previous_boost_timespec;
    static int s_previous_duration = 0;

    struct timespec cur_boost_timespec;
    long long elapsed_time;
    int duration = kMinInteractiveDuration;

    if (data) {
        int input_duration = *((int*)data);
        if (input_duration > duration) {
            duration = (input_duration > kMaxInteractiveDuration) ? kMaxInteractiveDuration
                                                                  : input_duration;
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &cur_boost_timespec);

    elapsed_time = calc_timespan_us(s_previous_boost_timespec, cur_boost_timespec);
    // don't hint if it's been less than 250ms since last boost
    // also detect if we're doing anything resembling a fling
    // support additional boosting in case of flings
    if (elapsed_time < 250000 && duration <= 750) {
        return;
    }
    s_previous_boost_timespec = cur_boost_timespec;
    s_previous_duration = duration;

    perf_hint_enable_with_type(VENDOR_HINT_SCROLL_BOOST, duration, SCROLL_VERTICAL);
}

static int process_activity_launch_hint(void* data) {
    static int launch_handle = -1;
    static int launch_mode = 0;

    // release lock early if launch has finished
    if (!data) {
        if (CHECK_HANDLE(launch_handle)) {
            release_request(launch_handle);
            launch_handle = -1;
        }
        launch_mode = 0;
        return HINT_HANDLED;
    }

    if (!launch_mode) {
        launch_handle = perf_hint_enable_with_type(VENDOR_HINT_FIRST_LAUNCH_BOOST,
                                                   kMaxLaunchDuration, LAUNCH_BOOST_V1);
        if (!CHECK_HANDLE(launch_handle)) {
            ALOGE("Failed to perform launch boost");
            return HINT_NONE;
        }
        launch_mode = 1;
    }
    return HINT_HANDLED;
}

int power_hint_override(struct power_module *module, power_hint_t hint,
        void *data)
{

    int ret_val = HINT_NONE;

    switch(hint) {
    	case POWER_HINT_INTERACTION:
            process_interaction_hint(data);
            ret_val = HINT_HANDLED;
            break;
        case POWER_HINT_LAUNCH:
            ret_val = process_activity_launch_hint(data);
            break;
        case POWER_HINT_VSYNC:
            break;
        case POWER_HINT_VIDEO_ENCODE:
        {
            process_video_encode_hint(data);
            return HINT_HANDLED;
        }
        break;
        default:
	    break;
    }
    return HINT_NONE;
}

int  set_interactive_override(struct power_module *module, int on)
{
    char governor[80];
    char tmp_str[NODE_MAX];
    int resource_values[20];
    int num_resources;
    struct video_encode_metadata_t video_encode_metadata;
    int rc;

    ALOGI("Got set_interactive hint");

    if (get_scaling_governor(governor, sizeof(governor)) == -1) {
        ALOGE("Can't obtain scaling governor.");
        return HINT_HANDLED;
    }

    if (!on) {
        /* Display off. */
             if (is_interactive_governor(governor)) {
             /*
                 1. CPUfreq params
                        - hispeed freq for big - 1113Mhz
                        - go hispeed load for big - 95
                        - above_hispeed_delay for big - 40ms
                2. BusDCVS V2 params
                        - Sample_ms of 10ms
            */
            if(is_target_SDM630()){
                int res[] = { 0x41414000, 0x459,
                              0x41410000, 0x5F,
                              0x41400000, 0x4,
                              0x41820000, 0xA };
                memcpy(resource_values, res, MIN_VAL(sizeof(resource_values), sizeof(res)));
                num_resources = sizeof(res)/sizeof(res[0]);
            }
             /*
                 1. CPUfreq params
                        - hispeed freq for little - 902Mhz
                        - go hispeed load for little - 95
                        - above_hispeed_delay for little - 40ms
                 2. BusDCVS V2 params
                        - Sample_ms of 10ms
                 3. Sched group upmigrate - 500
            */
            else{
                int res[] =  { 0x41414100, 0x386,
                               0x41410100, 0x5F,
                               0x41400100, 0x4,
                               0x41820000, 0xA,
                               0x40C54000, 0x1F4};
                memcpy(resource_values, res, MIN_VAL(sizeof(resource_values), sizeof(res)));
                num_resources = sizeof(res)/sizeof(res[0]);

            }
               if (!display_hint_sent) {
                   perform_hint_action(DISPLAY_STATE_HINT_ID,
                   resource_values, num_resources);
                  display_hint_sent = 1;
                }
             }

    } else {
        /* Display on. */
          if (is_interactive_governor(governor)) {

             undo_hint_action(DISPLAY_STATE_HINT_ID);
             display_hint_sent = 0;
          }
   }
    saved_interactive_mode = !!on;
    return HINT_HANDLED;
}


/* Video Encode Hint */
static void process_video_encode_hint(void *metadata)
{
    char governor[80];
    int resource_values[20];
    int num_resources;
    struct video_encode_metadata_t video_encode_metadata;

    ALOGI("Got process_video_encode_hint");

    if (get_scaling_governor(governor, sizeof(governor)) == -1) {
        ALOGE("Can't obtain scaling governor.");
        //return HINT_HANDLED;
    }

    /* Initialize encode metadata struct fields. */
    memset(&video_encode_metadata, 0, sizeof(struct video_encode_metadata_t));
    video_encode_metadata.state = -1;
    video_encode_metadata.hint_id = DEFAULT_VIDEO_ENCODE_HINT_ID;

    if (metadata) {
        if (parse_video_encode_metadata((char *)metadata,
            &video_encode_metadata) == -1) {
            ALOGE("Error occurred while parsing metadata.");
            return;
        }
    } else {
        return;
    }

    if (video_encode_metadata.state == 1) {
        if (is_interactive_governor(governor)) {
             /*
                 1. CPUfreq params
                        - hispeed freq for big - 1113Mhz
                        - go hispeed load for big - 95
                        - above_hispeed_delay for big - 40ms
                        - target loads - 95
                        - nr_run - 5
                 2. BusDCVS V2 params
                        - Sample_ms of 10ms
            */
            if(is_target_SDM630()){
                int res[] = { 0x41414000, 0x459,
                              0x41410000, 0x5F,
                              0x41400000, 0x4,
                              0x41420000, 0x5F,
                              0x40C2C000, 0X5,
                              0x41820000, 0xA};
                memcpy(resource_values, res, MIN_VAL(sizeof(resource_values), sizeof(res)));
                num_resources = sizeof(res)/sizeof(res[0]);

            }
            /*
                 1. CPUfreq params
                        - hispeed freq for little - 902Mhz
                        - go hispeed load for little - 95
                        - above_hispeed_delay for little - 40ms
                 2. BusDCVS V2 params
                        - Sample_ms of 10ms
            */
            else{
                int res[] = { 0x41414100, 0x386,
                              0x41410100, 0x5F,
                              0x41400100, 0x4,
                              0x41820000, 0xA};
                memcpy(resource_values, res, MIN_VAL(sizeof(resource_values), sizeof(res)));
                num_resources = sizeof(res)/sizeof(res[0]);
            }
            pthread_mutex_lock(&camera_hint_mutex);
            camera_hint_ref_count++;
            if (camera_hint_ref_count == 1) {
                if (!video_encode_hint_sent) {
                    perform_hint_action(video_encode_metadata.hint_id,
                    resource_values, num_resources);
                    video_encode_hint_sent = 1;
                }
           }
           pthread_mutex_unlock(&camera_hint_mutex);
        }
    } else if (video_encode_metadata.state == 0) {
        if (is_interactive_governor(governor)) {
            pthread_mutex_lock(&camera_hint_mutex);
            camera_hint_ref_count--;
            if (!camera_hint_ref_count) {
                undo_hint_action(video_encode_metadata.hint_id);
                video_encode_hint_sent = 0;
            }
            pthread_mutex_unlock(&camera_hint_mutex);
            return ;
        }
    }
    return;
}


