/*
 * Elan driver for libfprint
 *
 * Copyright (C) 2017 Igor Filatov <ia.filatov@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#pragma once

#include <glib.h>

#define ELAN_VEND_ID 0x04f3

/* a default device type */
#define ELAN_ALL_DEV 0

/* devices with quirks */
#define ELAN_0907 (1 << 0)
#define ELAN_0C03 (1 << 1)
#define ELAN_0C42 (1 << 2)
/* 04f3:0c1a firmware 0x0142 answers the pre-scan command (0x40 0x3f)
 * immediately instead of blocking until a finger is placed. The driver has
 * to poll it, otherwise a capture started before the finger touches the
 * sensor fails with a protocol error. */
#define ELAN_0C1A (1 << 3)

/* Press-and-hold sensors: capture one settled frame instead of stitching. */
#define ELAN_PRESS (1 << 4)

/* 0c1a: a finger resting on the sensor while the background is captured is
 * baked into the sensor calibration baseline, after which the sensor stops
 * reporting touches. If the background/calibration mean differ by more than
 * this, wait for the finger to lift and take the background again. */
#define ELAN_0C1A_FINGER_DELTA 1000
#define ELAN_0C1A_FINGER_WAIT 100
#define ELAN_0C1A_FINGER_WAITS 100

/* press mode: frames read before the touch is reported (the finger is still
 * down, so clients can prompt to lift it), and oldest frames to drop (finger
 * still landing) */
#define ELAN_PRESS_FRAMES 4
#define ELAN_PRESS_SKIP_FRAMES 1
/* reject captures whose column means carry most of the variance (stripe noise) */
#define ELAN_PRESS_MAX_STRIPE_RATIO 0.75
/* a small sensor sees a different patch each touch: enroll more of them */
#define ELAN_PRESS_ENROLL_STAGES 15
/* a finger landing during calibration makes it fail, start it over */
#define ELAN_PRESS_CALIBRATION_ROUNDS 20
#define ELAN_PRESS_CALIBRATION_DELAY 300
/* SIFT: minimum keypoints per capture; the matcher itself asks for at least
 * ELANPRESS_MATCH_MIN_PAIRS consistent pairs */
#define ELAN_SIFT_MIN_KEYPOINTS 20

/* devices which don't require frame rotation before assembling */
#define ELAN_NOT_ROTATED ELAN_0C03

/* min FW version that supports calibration */
#define ELAN_MIN_CALIBRATION_FW 0x0138

/* max difference between background image mean and calibration mean
 * (the response value of get_calib_mean_cmd)*/
#define ELAN_CALIBRATION_MAX_DELTA 500

/* times to retry reading calibration status during one session
 * generally prevents calibration from looping indefinitely */
#define ELAN_CALIBRATION_ATTEMPTS 10

/* min and max frames in a capture */
#define ELAN_MIN_FRAMES 7
#define ELAN_MAX_FRAMES 30

/* crop frames to this height to improve stitching */
#define ELAN_MAX_FRAME_HEIGHT 50

/* number of frames to drop at the end of capture because frames captured
 * while the finger is being lifted can be bad */
#define ELAN_SKIP_LAST_FRAMES 2

#define ELAN_CMD_LEN 0x2
#define ELAN_EP_CMD_OUT (0x1 | FPI_USB_ENDPOINT_OUT)
#define ELAN_EP_CMD_IN (0x3 | FPI_USB_ENDPOINT_IN)
#define ELAN_EP_IMG_IN (0x2 | FPI_USB_ENDPOINT_IN)

/* used as response length to tell the driver to skip reading response */
#define ELAN_CMD_SKIP_READ 0

/* usual command timeout and timeout for when we need to check if the finger is
 * still on the device */
#define ELAN_CMD_TIMEOUT 10000
#define ELAN_FINGER_TIMEOUT 200

G_DECLARE_FINAL_TYPE (FpiDeviceElan, fpi_device_elan, FPI, DEVICE_ELAN,
                      FpImageDevice);

struct elan_cmd
{
  unsigned char  cmd[ELAN_CMD_LEN];
  int            response_len;
  int            response_in;
  unsigned short devices;
  gboolean       never_cancel;
};

static const struct elan_cmd get_sensor_dim_cmd = {
  .cmd = {0x00, 0x0c},
  .response_len = 0x4,
  .response_in = ELAN_EP_CMD_IN,
  .devices = ELAN_ALL_DEV,
};

static const struct elan_cmd get_fw_ver_cmd = {
  .cmd = {0x40, 0x19},
  .response_len = 0x2,
  .response_in = ELAN_EP_CMD_IN,
  .devices = ELAN_ALL_DEV,
};

/* unknown, returns 0x0 0x1 on 0907 */
static const struct elan_cmd activate_cmd_1 = {
  .cmd = {0x40, 0x2a},
  .response_len = 0x2,
  .response_in = ELAN_EP_CMD_IN,
  .devices = ELAN_0907,
};

static const struct elan_cmd get_image_cmd = {
  .cmd = {0x00, 0x09},
  /* raw frame sizes are calculated from image dimensions reported by the
   * device */
  .response_len = -1,
  .response_in = ELAN_EP_IMG_IN,
  .devices = ELAN_ALL_DEV,
};

static const struct elan_cmd read_sensor_status_cmd = {
  .cmd = {0x40, 0x13},
  .response_len = 0x1,
  .response_in = ELAN_EP_CMD_IN,
  .devices = ELAN_ALL_DEV,
};

static const struct elan_cmd get_calib_status_cmd = {
  .cmd = {0x40, 0x23},
  .response_len = 0x1,
  .response_in = ELAN_EP_CMD_IN,
  .devices = ELAN_ALL_DEV,
};

static const struct elan_cmd get_calib_mean_cmd = {
  .cmd = {0x40, 0x24},
  .response_len = 0x2,
  .response_in = ELAN_EP_CMD_IN,
  .devices = ELAN_ALL_DEV,
};

static const struct elan_cmd led_on_cmd = {
  .cmd = {0x40, 0x31},
  .response_len = ELAN_CMD_SKIP_READ,
  .response_in = ELAN_EP_CMD_IN,
  .devices = ELAN_ALL_DEV,
};

/* wait for finger
 * subsequent read will not complete until finger is placed on the reader */
static const struct elan_cmd pre_scan_cmd = {
  .cmd = {0x40, 0x3f},
  .response_len = 0x1,
  .response_in = ELAN_EP_CMD_IN,
  .devices = ELAN_ALL_DEV,
};

/* led off, stop waiting for finger */
static const struct elan_cmd stop_cmd = {
  .cmd = {0x00, 0x0b},
  .response_len = ELAN_CMD_SKIP_READ,
  .response_in = ELAN_EP_CMD_IN,
  .devices = ELAN_ALL_DEV,
  .never_cancel = TRUE,
};

static const FpIdEntry elan_id_table[] = {
  {.vid = ELAN_VEND_ID,  .pid = 0x0903, .driver_data = ELAN_ALL_DEV},
  {.vid = ELAN_VEND_ID,  .pid = 0x0907, .driver_data = ELAN_0907},
  {.vid = ELAN_VEND_ID,  .pid = 0x0c01, .driver_data = ELAN_ALL_DEV},
  {.vid = ELAN_VEND_ID,  .pid = 0x0c02, .driver_data = ELAN_ALL_DEV},
  {.vid = ELAN_VEND_ID,  .pid = 0x0c03, .driver_data = ELAN_0C03},
  {.vid = ELAN_VEND_ID,  .pid = 0x0c04, .driver_data = ELAN_ALL_DEV},
  {.vid = ELAN_VEND_ID,  .pid = 0x0c05, .driver_data = ELAN_ALL_DEV},
  {.vid = ELAN_VEND_ID,  .pid = 0x0c06, .driver_data = ELAN_ALL_DEV},
  {.vid = ELAN_VEND_ID,  .pid = 0x0c07, .driver_data = ELAN_ALL_DEV},
  {.vid = ELAN_VEND_ID,  .pid = 0x0c08, .driver_data = ELAN_ALL_DEV},
  {.vid = ELAN_VEND_ID,  .pid = 0x0c09, .driver_data = ELAN_ALL_DEV},
  {.vid = ELAN_VEND_ID,  .pid = 0x0c0a, .driver_data = ELAN_ALL_DEV},
  {.vid = ELAN_VEND_ID,  .pid = 0x0c0b, .driver_data = ELAN_ALL_DEV},
  {.vid = ELAN_VEND_ID,  .pid = 0x0c0c, .driver_data = ELAN_ALL_DEV},
  {.vid = ELAN_VEND_ID,  .pid = 0x0c0d, .driver_data = ELAN_ALL_DEV},
  {.vid = ELAN_VEND_ID,  .pid = 0x0c0e, .driver_data = ELAN_ALL_DEV},
  {.vid = ELAN_VEND_ID,  .pid = 0x0c0f, .driver_data = ELAN_ALL_DEV},
  {.vid = ELAN_VEND_ID,  .pid = 0x0c10, .driver_data = ELAN_ALL_DEV},
  {.vid = ELAN_VEND_ID,  .pid = 0x0c11, .driver_data = ELAN_ALL_DEV},
  {.vid = ELAN_VEND_ID,  .pid = 0x0c12, .driver_data = ELAN_ALL_DEV},
  {.vid = ELAN_VEND_ID,  .pid = 0x0c13, .driver_data = ELAN_ALL_DEV},
  {.vid = ELAN_VEND_ID,  .pid = 0x0c14, .driver_data = ELAN_ALL_DEV},
  {.vid = ELAN_VEND_ID,  .pid = 0x0c15, .driver_data = ELAN_ALL_DEV},
  {.vid = ELAN_VEND_ID,  .pid = 0x0c16, .driver_data = ELAN_ALL_DEV},
  {.vid = ELAN_VEND_ID,  .pid = 0x0c17, .driver_data = ELAN_ALL_DEV},
  {.vid = ELAN_VEND_ID,  .pid = 0x0c18, .driver_data = ELAN_ALL_DEV},
  {.vid = ELAN_VEND_ID,  .pid = 0x0c19, .driver_data = ELAN_ALL_DEV},
  {.vid = ELAN_VEND_ID,  .pid = 0x0c1a, .driver_data = ELAN_0C1A | ELAN_PRESS},
  {.vid = ELAN_VEND_ID,  .pid = 0x0c1b, .driver_data = ELAN_ALL_DEV},
  {.vid = ELAN_VEND_ID,  .pid = 0x0c1c, .driver_data = ELAN_ALL_DEV},
  {.vid = ELAN_VEND_ID,  .pid = 0x0c1d, .driver_data = ELAN_ALL_DEV},
  {.vid = ELAN_VEND_ID,  .pid = 0x0c1e, .driver_data = ELAN_ALL_DEV},
  {.vid = ELAN_VEND_ID,  .pid = 0x0c1f, .driver_data = ELAN_ALL_DEV},
  {.vid = ELAN_VEND_ID,  .pid = 0x0c20, .driver_data = ELAN_ALL_DEV},
  {.vid = ELAN_VEND_ID,  .pid = 0x0c21, .driver_data = ELAN_ALL_DEV},
  {.vid = ELAN_VEND_ID,  .pid = 0x0c22, .driver_data = ELAN_ALL_DEV},
  {.vid = ELAN_VEND_ID,  .pid = 0x0c23, .driver_data = ELAN_ALL_DEV},
  {.vid = ELAN_VEND_ID,  .pid = 0x0c24, .driver_data = ELAN_ALL_DEV},
  {.vid = ELAN_VEND_ID,  .pid = 0x0c25, .driver_data = ELAN_ALL_DEV},
  {.vid = ELAN_VEND_ID,  .pid = 0x0c26, .driver_data = ELAN_ALL_DEV},
  {.vid = ELAN_VEND_ID,  .pid = 0x0c27, .driver_data = ELAN_ALL_DEV},
  {.vid = ELAN_VEND_ID,  .pid = 0x0c28, .driver_data = ELAN_ALL_DEV},
  {.vid = ELAN_VEND_ID,  .pid = 0x0c29, .driver_data = ELAN_ALL_DEV},
  {.vid = ELAN_VEND_ID,  .pid = 0x0c2a, .driver_data = ELAN_ALL_DEV},
  {.vid = ELAN_VEND_ID,  .pid = 0x0c2b, .driver_data = ELAN_ALL_DEV},
  {.vid = ELAN_VEND_ID,  .pid = 0x0c2c, .driver_data = ELAN_ALL_DEV},
  {.vid = ELAN_VEND_ID,  .pid = 0x0c2d, .driver_data = ELAN_ALL_DEV},
  {.vid = ELAN_VEND_ID,  .pid = 0x0c2e, .driver_data = ELAN_ALL_DEV},
  {.vid = ELAN_VEND_ID,  .pid = 0x0c2f, .driver_data = ELAN_ALL_DEV},
  {.vid = ELAN_VEND_ID,  .pid = 0x0c30, .driver_data = ELAN_ALL_DEV},
  {.vid = ELAN_VEND_ID,  .pid = 0x0c31, .driver_data = ELAN_ALL_DEV},
  {.vid = ELAN_VEND_ID,  .pid = 0x0c32, .driver_data = ELAN_ALL_DEV},
  {.vid = ELAN_VEND_ID,  .pid = 0x0c33, .driver_data = ELAN_ALL_DEV},
  {.vid = ELAN_VEND_ID,  .pid = 0x0c3d, .driver_data = ELAN_ALL_DEV},
  {.vid = ELAN_VEND_ID,  .pid = 0x0c42, .driver_data = ELAN_0C42},
  {.vid = ELAN_VEND_ID,  .pid = 0x0c4b, .driver_data = ELAN_ALL_DEV},
  {.vid = ELAN_VEND_ID,  .pid = 0x0c4d, .driver_data = ELAN_ALL_DEV},
  {.vid = ELAN_VEND_ID,  .pid = 0x0c4f, .driver_data = ELAN_ALL_DEV},
  {.vid = ELAN_VEND_ID,  .pid = 0x0c63, .driver_data = ELAN_ALL_DEV},
  {.vid = ELAN_VEND_ID,  .pid = 0x0c6e, .driver_data = ELAN_ALL_DEV},
  {.vid = ELAN_VEND_ID,  .pid = 0x0c58, .driver_data = ELAN_ALL_DEV},
  {.vid = 0,  .pid = 0,  .driver_data = 0},
};

static void elan_cmd_done (FpiSsm *ssm);
static void elan_cmd_read (FpiSsm   *ssm,
                           FpDevice *dev);

static void elan_calibrate (FpiDeviceElan *self);
static void elan_capture (FpiDeviceElan *self);

static void dev_change_state (FpImageDevice      *dev,
                              FpiImageDeviceState state);
