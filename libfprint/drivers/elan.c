/*
 * Elan driver for libfprint
 *
 * Copyright (C) 2017 Igor Filatov <ia.filatov@gmail.com>
 * Copyright (C) 2018 Sébastien Béchet <sebastien.bechet@osinix.com >
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

/*
 * The algorithm which libfprint uses to match fingerprints doesn't like small
 * images like the ones these drivers produce. There's just not enough minutiae
 * (recognizable print-specific points) on them for a reliable match. This means
 * that unless another matching algo is found/implemented, these readers will
 * not work as good with libfprint as they do with vendor drivers.
 *
 * To get bigger images the driver expects you to swipe the finger over the
 * reader. This works quite well for readers with a rectangular 144x64 sensor.
 * Worse than real swipe readers but good enough for day-to-day use. It needs
 * a steady and relatively slow swipe. There are also square 96x96 sensors and
 * I don't know whether they are in fact usable or not because I don't have one.
 * I imagine they'd be less reliable because the resulting image is even
 * smaller. If they can't be made usable with libfprint, I might end up dropping
 * them because it's better than saying they work when they don't.
 */

#define FP_COMPONENT "elan"

#include "drivers_api.h"
#include "elan.h"
#include "elanpress-match.h"

#include <math.h>
#include <stdlib.h>

static unsigned char
elan_get_pixel (struct fpi_frame_asmbl_ctx *ctx,
                struct fpi_frame *frame, unsigned int x,
                unsigned int y)
{
  return frame->data[x + y * ctx->frame_width];
}

static struct fpi_frame_asmbl_ctx assembling_ctx = {
  .frame_width = 0,
  .frame_height = 0,
  .image_width = 0,
  .get_pixel = elan_get_pixel,
};

struct _FpiDeviceElan
{
  FpImageDevice parent;

  /* device config */
  unsigned short dev_type;
  unsigned short fw_ver;
  void           (*process_frame) (unsigned short *raw_frame,
                                   GSList       ** frames);
  /* end device config */

  /* commands */
  const struct elan_cmd *cmd;
  int                    cmd_timeout;
  /* end commands */

  /* state */
  gboolean        active;
  gboolean        deactivating;
  unsigned char  *last_read;
  gsize           last_read_len;
  unsigned char   calib_atts_left;
  unsigned char   calib_rounds_left;
  unsigned char   calib_status;
  unsigned int    calib_delta;
  unsigned char   finger_waits_left;
  unsigned short *background;
  unsigned char   frame_width;
  unsigned char   frame_height;
  unsigned char   raw_frame_height;
  int             num_frames;
  gboolean        press_submitted;
  GSList         *frames;
  /* end state */
};
G_DEFINE_TYPE (FpiDeviceElan, fpi_device_elan, FP_TYPE_IMAGE_DEVICE);

static int
cmp_short (const void *a, const void *b)
{
  return (int) (*(short *) a - *(short *) b);
}

static void
elan_dev_reset_state (FpiDeviceElan *elandev)
{
  G_DEBUG_HERE ();

  elandev->cmd = NULL;
  elandev->cmd_timeout = ELAN_CMD_TIMEOUT;

  elandev->calib_status = 0;

  g_free (elandev->last_read);
  elandev->last_read = NULL;
  elandev->last_read_len = 0;

  g_slist_free_full (elandev->frames, g_free);
  elandev->frames = NULL;
  elandev->num_frames = 0;
  elandev->press_submitted = FALSE;
}

static void
elan_save_frame (FpiDeviceElan *self, unsigned short *frame)
{
  G_DEBUG_HERE ();

  /* so far 3 types of readers by sensor dimensions and orientation have been
   * seen in the wild:
   * 1. 144x64. Raw images are in portrait orientation while readers themselves
   *    are placed (e.g. built into a touchpad) in landscape orientation. These
   *    need to be rotated before assembling.
   * 2. 96x96 rotated. Like the first type but square. Likewise, need to be
   *    rotated before assembling.
   * 3. 96x96 normal. Square and need NOT be rotated. So far there's only been
   *    1 report of a 0c03 of this type. Hopefully this type can be identified
   *    by device id (and manufacturers don't just install the readers as they
   *    please).
   * we also discard stripes of 'frame_margin' from bottom and top because
   * assembling works bad for tall frames */

  unsigned char frame_width = self->frame_width;
  unsigned char frame_height = self->frame_height;
  unsigned char raw_height = self->raw_frame_height;
  unsigned char frame_margin = (raw_height - self->frame_height) / 2;
  int frame_idx, raw_idx;

  for (int y = 0; y < frame_height; y++)
    for (int x = 0; x < frame_width; x++)
      {
        if (self->dev_type & ELAN_NOT_ROTATED)
          raw_idx = x + (y + frame_margin) * frame_width;
        else
          raw_idx = frame_margin + y + x * raw_height;
        frame_idx = x + y * frame_width;
        frame[frame_idx] =
          ((unsigned short *) self->last_read)[raw_idx];
      }
}

static void
elan_save_background (FpiDeviceElan *elandev)
{
  G_DEBUG_HERE ();

  g_free (elandev->background);
  elandev->background =
    g_malloc (elandev->frame_width * elandev->frame_height *
              sizeof (short));
  elan_save_frame (elandev, elandev->background);
}

/* save a frame as part of the fingerprint image
 * background needs to have been captured for this routine to work
 * Elantech recommends 2-step non-linear normalization in order to reduce
 * 2^14 ADC resolution to 2^8 image:
 *
 * 1. background is subtracted (done here)
 *
 * 2. pixels are grouped in 3 groups by intensity and each group is mapped
 *    separately onto the normalized frame (done in elan_process_frame_*)
 *    ==== 16383     ____> ======== 255
 *                  /
 *    ----- lvl3 __/
 *                   35% pixels
 *
 *    ----- lvl2 --------> ======== 156
 *
 *                   30% pixels
 *    ----- lvl1 --------> ======== 99
 *
 *                   35% pixels
 *    ----- lvl0 __
 *                 \
 *    ======== 0    \____> ======== 0
 *
 * For some devices we don't do 2. but instead do a simple linear mapping
 * because it seems to produce better results (or at least as good):
 *    ==== 16383      ___> ======== 255
 *                   /
 *    ------ max  __/
 *
 *
 *    ------ min  __
 *                  \
 *    ======== 0     \___> ======== 0
 */
static int
elan_save_img_frame (FpiDeviceElan *elandev)
{
  G_DEBUG_HERE ();

  unsigned int frame_size = elandev->frame_width * elandev->frame_height;
  unsigned short *frame = g_malloc (frame_size * sizeof (short));

  elan_save_frame (elandev, frame);
  unsigned int sum = 0;

  for (int i = 0; i < frame_size; i++)
    {
      if (elandev->background[i] > frame[i])
        frame[i] = 0;
      else
        frame[i] -= elandev->background[i];
      sum += frame[i];
    }

  if (sum == 0)
    {
      fp_dbg
        ("frame darker than background; finger present during calibration?");
      g_free (frame);
      return -1;
    }

  elandev->frames = g_slist_prepend (elandev->frames, frame);
  elandev->num_frames += 1;
  return 0;
}

static void
elan_process_frame_linear (unsigned short *raw_frame,
                           GSList       ** frames)
{
  unsigned int frame_size =
    assembling_ctx.frame_width * assembling_ctx.frame_height;
  struct fpi_frame *frame =
    g_malloc (frame_size + sizeof (struct fpi_frame));

  G_DEBUG_HERE ();

  unsigned short min = 0xffff, max = 0;

  for (int i = 0; i < frame_size; i++)
    {
      if (raw_frame[i] < min)
        min = raw_frame[i];
      if (raw_frame[i] > max)
        max = raw_frame[i];
    }

  g_assert (max != min);

  unsigned short px;

  for (int i = 0; i < frame_size; i++)
    {
      px = raw_frame[i];
      px = (px - min) * 0xff / (max - min);
      frame->data[i] = (unsigned char) px;
    }

  *frames = g_slist_prepend (*frames, frame);
}

static void
elan_process_frame_thirds (unsigned short *raw_frame,
                           GSList       ** frames)
{
  G_DEBUG_HERE ();

  unsigned int frame_size =
    assembling_ctx.frame_width * assembling_ctx.frame_height;
  struct fpi_frame *frame =
    g_malloc (frame_size + sizeof (struct fpi_frame));

  unsigned short lvl0, lvl1, lvl2, lvl3;
  unsigned short *sorted = g_malloc (frame_size * sizeof (short));

  memcpy (sorted, raw_frame, frame_size * sizeof (short));
  qsort (sorted, frame_size, sizeof (short), cmp_short);
  lvl0 = sorted[0];
  lvl1 = sorted[frame_size * 3 / 10];
  lvl2 = sorted[frame_size * 65 / 100];
  lvl3 = sorted[frame_size - 1];
  g_free (sorted);

  unsigned short px;

  for (int i = 0; i < frame_size; i++)
    {
      px = raw_frame[i];
      if (lvl0 <= px && px < lvl1)
        px = (px - lvl0) * 99 / (lvl1 - lvl0);
      else if (lvl1 <= px && px < lvl2)
        px = 99 + ((px - lvl1) * 56 / (lvl2 - lvl1));
      else                      // (lvl2 <= px && px <= lvl3)
        px = 155 + ((px - lvl2) * 100 / (lvl3 - lvl2));
      frame->data[i] = (unsigned char) px;
    }

  *frames = g_slist_prepend (*frames, frame);
}

/* sum of absolute neighbour differences: higher means sharper ridges */
static guint64
elan_frame_sharpness (FpiDeviceElan *self, const unsigned short *f)
{
  guint64 s = 0;

  for (int y = 0; y < self->frame_height - 1; y++)
    for (int x = 0; x < self->frame_width - 1; x++)
      {
        int i = x + y * self->frame_width;
        s += ABS (f[i + 1] - f[i]) + ABS (f[i + self->frame_width] - f[i]);
      }
  return s;
}

/* std dev of column means over std dev of all pixels: close to 1 for the
 * vertical stripe noise some empty captures produce, well below for ridges */
static double
elan_stripe_ratio (const guint8 *px, int w, int h)
{
  double mean = 0, var = 0, cvar = 0;

  for (int i = 0; i < w * h; i++)
    mean += px[i];
  mean /= w * h;
  for (int i = 0; i < w * h; i++)
    var += (px[i] - mean) * (px[i] - mean);

  for (int x = 0; x < w; x++)
    {
      double c = 0;
      for (int y = 0; y < h; y++)
        c += px[x + y * w];
      c = c / h - mean;
      cvar += c * c;
    }

  if (var == 0)
    return 1;
  return sqrt ((cvar / w) / (var / (w * h)));
}

/* Press mode: the finger rests still on the sensor, so stitching frames by
 * estimated motion only produces garbage. Submit the sharpest settled frame. */
static void
elan_submit_press_image (FpImageDevice *dev)
{
  FpiDeviceElan *self = FPI_DEVICE_ELAN (dev);
  unsigned int frame_size = self->frame_width * self->frame_height;
  g_autoptr(FpImage) img = NULL;
  unsigned short *best = NULL;
  guint64 best_sharpness = 0;
  GSList *frames = NULL;
  struct fpi_frame *frame;
  double stripes;
  int n = 0;

  G_DEBUG_HERE ();

  /* runs while the finger is still down; self->frames is newest first, so
   * the skipped landing frames are at the tail */
  for (GSList *l = self->frames;
       l && n < self->num_frames - ELAN_PRESS_SKIP_FRAMES;
       l = l->next, n++)
    {
      guint64 s = elan_frame_sharpness (self, l->data);

      if (s > best_sharpness)
        {
          best_sharpness = s;
          best = l->data;
        }
    }

  assembling_ctx.frame_width = self->frame_width;
  assembling_ctx.frame_height = self->frame_height;
  self->process_frame (best, &frames);
  frame = frames->data;

  img = fp_image_new (self->frame_width, self->frame_height);
  memcpy (img->data, frame->data, frame_size);
  g_slist_free_full (frames, g_free);

  stripes = elan_stripe_ratio (img->data, img->width, img->height);
  fp_dbg ("press image: sharpest of %d frames (%" G_GUINT64_FORMAT "), stripe ratio %.2f",
          n, best_sharpness, stripes);
  if (stripes > ELAN_PRESS_MAX_STRIPE_RATIO)
    {
      fpi_image_device_retry_scan (dev, FP_DEVICE_RETRY_CENTER_FINGER);
      return;
    }

  img->flags |= FPI_IMAGE_PARTIAL;
  fpi_image_device_image_captured (dev, g_steal_pointer (&img));
}

/* SIFT features for one capture, stored as a one-element "aay" RAW print so
 * fpi_print_add_print() can collect every enroll capture.
 *
 * Blob format:
 *   u32 magic, u32 version, u32 count,
 *   count * (float x, float y, float angle, float scale, float response,
 *            u8 descriptor[128])
 */
#define ELAN_SIFT_MAGIC 0x53465431u
#define ELAN_SIFT_VERSION 1u
#define ELAN_SIFT_HEADER 12
#define ELAN_SIFT_KP_BYTES (5 * sizeof (float) + ELANPRESS_SIFT_DESC_LEN)

static unsigned char *
elan_sift_serialize (const ElanpressFeatures *features, gsize *outlen)
{
  guint32 magic = GUINT32_TO_LE (ELAN_SIFT_MAGIC);
  guint32 version = GUINT32_TO_LE (ELAN_SIFT_VERSION);
  guint32 count;
  gsize len, off;
  unsigned char *buf;

  if (!features || features->n == 0 ||
      features->n > ELANPRESS_MAX_KEYPOINTS)
    return NULL;

  count = GUINT32_TO_LE (features->n);
  len = ELAN_SIFT_HEADER + (gsize) features->n * ELAN_SIFT_KP_BYTES;
  buf = g_malloc (len);

  memcpy (buf, &magic, sizeof (magic));
  memcpy (buf + 4, &version, sizeof (version));
  memcpy (buf + 8, &count, sizeof (count));

  off = ELAN_SIFT_HEADER;
  for (guint i = 0; i < features->n; i++)
    {
      const ElanpressKeypoint *kp = &features->kp[i];
      float values[5] = { kp->x, kp->y, kp->angle, kp->scale, kp->response };

      memcpy (buf + off, values, sizeof (values));
      off += sizeof (values);
      memcpy (buf + off, kp->desc, ELANPRESS_SIFT_DESC_LEN);
      off += ELANPRESS_SIFT_DESC_LEN;
    }

  *outlen = len;
  return buf;
}

static ElanpressFeatures *
elan_sift_deserialize (const guchar *blob, gsize len)
{
  guint32 magic, version, count;
  ElanpressFeatures *features;
  gsize off;

  if (!blob || len < ELAN_SIFT_HEADER)
    return NULL;

  memcpy (&magic, blob, sizeof (magic));
  memcpy (&version, blob + 4, sizeof (version));
  memcpy (&count, blob + 8, sizeof (count));
  magic = GUINT32_FROM_LE (magic);
  version = GUINT32_FROM_LE (version);
  count = GUINT32_FROM_LE (count);

  if (magic != ELAN_SIFT_MAGIC || version != ELAN_SIFT_VERSION ||
      count == 0 || count > ELANPRESS_MAX_KEYPOINTS)
    return NULL;
  if (len != ELAN_SIFT_HEADER + (gsize) count * ELAN_SIFT_KP_BYTES)
    return NULL;

  features = g_new0 (ElanpressFeatures, 1);
  features->kp = g_new0 (ElanpressKeypoint, count);
  features->n = count;

  off = ELAN_SIFT_HEADER;
  for (guint i = 0; i < count; i++)
    {
      ElanpressKeypoint *kp = &features->kp[i];
      float values[5];

      memcpy (values, blob + off, sizeof (values));
      off += sizeof (values);
      kp->x = values[0];
      kp->y = values[1];
      kp->angle = values[2];
      kp->scale = values[3];
      kp->response = values[4];
      memcpy (kp->desc, blob + off, ELANPRESS_SIFT_DESC_LEN);
      off += ELANPRESS_SIFT_DESC_LEN;
    }

  return features;
}

static gboolean
elan_sift_img_to_print (FpImageDevice *dev, FpImage *image,
                        FpPrint *print, GError **error)
{
  g_autoptr(ElanpressFeatures) features = NULL;
  g_autofree unsigned char *blob = NULL;
  GVariantBuilder builder;
  gdouble quality;
  gsize len;

  quality = elanpress_image_quality (image->data,
                                     image->width * image->height);
  fp_dbg ("sift image quality: %.1f", quality);
  if (quality < ELANPRESS_MIN_QUALITY_STDDEV)
    {
      *error = fpi_device_retry_new (FP_DEVICE_RETRY_CENTER_FINGER);
      return FALSE;
    }

  features = elanpress_image_features (image->data, image->width,
                                       image->height);
  if (!features || features->n < ELAN_SIFT_MIN_KEYPOINTS)
    {
      fp_dbg ("sift keypoints: %u (too few)",
              features ? features->n : 0);
      *error = fpi_device_retry_new (FP_DEVICE_RETRY_CENTER_FINGER);
      return FALSE;
    }
  fp_dbg ("sift keypoints: %u", features->n);

  blob = elan_sift_serialize (features, &len);
  if (!blob)
    {
      *error = fpi_device_retry_new_msg (FP_DEVICE_RETRY_GENERAL,
                                         "SIFT serialization failed");
      return FALSE;
    }

  fpi_print_set_type (print, FPI_PRINT_RAW);
  g_variant_builder_init (&builder, G_VARIANT_TYPE ("aay"));
  g_variant_builder_add_value (&builder,
                               g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE,
                                                          blob, len, 1));
  g_object_set (print, "fpi-data", g_variant_builder_end (&builder), NULL);
  return TRUE;
}

static ElanpressFeatures *
elan_sift_nth (GVariant *data, gsize i)
{
  g_autoptr(GVariant) child = NULL;
  const guchar *blob;
  gsize len;

  if (i >= g_variant_n_children (data))
    return NULL;
  child = g_variant_get_child_value (data, i);
  blob = g_variant_get_fixed_array (child, &len, 1);
  return elan_sift_deserialize (blob, len);
}

static FpiMatchResult
elan_sift_match (FpImageDevice *dev, FpPrint *template,
                 FpPrint *print, GError **error)
{
  g_autoptr(GVariant) tdata = NULL;
  g_autoptr(GVariant) pdata = NULL;
  g_autoptr(ElanpressFeatures) probe = NULL;
  int best = 0;
  int usable = 0;
  gsize n_templates;

  g_object_get (template, "fpi-data", &tdata, NULL);
  g_object_get (print, "fpi-data", &pdata, NULL);

  if (!tdata || !g_variant_is_of_type (tdata, G_VARIANT_TYPE ("aay")))
    {
      fp_warn ("stored print has no SIFT data, re-enroll this finger");
      return FPI_MATCH_FAIL;
    }

  if (!pdata || !g_variant_is_of_type (pdata, G_VARIANT_TYPE ("aay")))
    {
      *error = fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_INVALID,
                                         "Bad SIFT probe");
      return FPI_MATCH_ERROR;
    }

  probe = elan_sift_nth (pdata, 0);
  if (!probe)
    {
      *error = fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_INVALID,
                                         "Bad SIFT probe");
      return FPI_MATCH_ERROR;
    }

  n_templates = g_variant_n_children (tdata);

  for (gsize i = 0; i < n_templates; i++)
    {
      g_autoptr(ElanpressFeatures) enrolled = elan_sift_nth (tdata, i);
      guint pairs;

      if (!enrolled)
        continue;
      usable++;
      pairs = elanpress_match_features (probe, enrolled);
      best = MAX (best, (int) pairs);
      if (best >= ELANPRESS_MATCH_MIN_PAIRS)
        break;
    }

  if (!usable)
    fp_warn ("stored print has incompatible data, re-enroll this finger");

  fp_dbg ("sift best pairs %d/%d", best, ELANPRESS_MATCH_MIN_PAIRS);
  return best >= ELANPRESS_MATCH_MIN_PAIRS ? FPI_MATCH_SUCCESS : FPI_MATCH_FAIL;
}

static void
elan_submit_image (FpImageDevice *dev)
{
  FpiDeviceElan *self = FPI_DEVICE_ELAN (dev);
  GSList *raw_frames;
  GSList *frames = NULL;
  FpImage *img;

  G_DEBUG_HERE ();

  if (self->dev_type & ELAN_PRESS)
    {
      elan_submit_press_image (dev);
      return;
    }

  raw_frames = g_slist_nth (self->frames, ELAN_SKIP_LAST_FRAMES);

  fp_dbg ("submitting %d frame(s) for assembly",
          self->num_frames - ELAN_SKIP_LAST_FRAMES);

  assembling_ctx.frame_width = self->frame_width;
  assembling_ctx.frame_height = self->frame_height;
  assembling_ctx.image_width = self->frame_width * 3 / 2;
  g_slist_foreach (raw_frames, (GFunc) self->process_frame, &frames);
  fpi_do_movement_estimation (&assembling_ctx, frames);
  img = fpi_assemble_frames (&assembling_ctx, frames);
  img->flags |= FPI_IMAGE_PARTIAL;

  fp_dbg ("assembled image %ux%u", img->width, img->height);

  g_slist_free_full (frames, g_free);

  fpi_image_device_image_captured (dev, img);
}

static void
elan_cmd_done (FpiSsm *ssm)
{
  G_DEBUG_HERE ();
  fpi_ssm_next_state (ssm);
}

static void
elan_cmd_cb (FpiUsbTransfer *transfer, FpDevice *dev,
             gpointer user_data, GError *error)
{
  FpiSsm *ssm = transfer->ssm;
  FpiDeviceElan *self = FPI_DEVICE_ELAN (dev);

  G_DEBUG_HERE ();

  if (error)
    {
      /* XXX: In the cancellation case we used to not
       *      mark the SSM as failed?! */
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  /* XXX: We used to reset the device in error cases! */
  if (transfer->endpoint & FPI_USB_ENDPOINT_IN)
    {
      /* just finished receiving */
      self->last_read = g_memdup2 (transfer->buffer, transfer->actual_length);
      self->last_read_len = transfer->actual_length;
      elan_cmd_done (ssm);
    }
  else
    {
      /* just finished sending */
      G_DEBUG_HERE ();
      elan_cmd_read (ssm, dev);
    }
}

static void
elan_cmd_read (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceElan *self = FPI_DEVICE_ELAN (dev);
  FpiUsbTransfer *transfer;
  GCancellable *cancellable = NULL;
  int response_len = self->cmd->response_len;

  G_DEBUG_HERE ();

  if (self->cmd->response_len == ELAN_CMD_SKIP_READ)
    {
      fp_dbg ("skipping read, not expecting anything");
      elan_cmd_done (ssm);
      return;
    }

  if (self->dev_type == ELAN_0C42)
    {
      /* ELAN_0C42 sends an extra byte in one byte responses */
      if (self->cmd->response_len == 1)
        response_len = 2;
    }

  if (self->cmd->cmd == get_image_cmd.cmd)
    /* raw data has 2-byte "pixels" and the frame is vertical */
    response_len =
      self->raw_frame_height * self->frame_width * 2;

  g_clear_pointer (&self->last_read, g_free);
  self->last_read_len = 0;

  transfer = fpi_usb_transfer_new (dev);
  transfer->ssm = ssm;
  transfer->short_is_error = TRUE;

  fpi_usb_transfer_fill_bulk (transfer,
                              self->cmd->response_in,
                              response_len);

  if (!self->cmd->never_cancel)
    cancellable = fpi_device_get_cancellable (dev);

  fpi_usb_transfer_submit (transfer, self->cmd_timeout, cancellable, elan_cmd_cb, NULL);
}

static void
elan_run_cmd (FpiSsm                *ssm,
              FpDevice              *dev,
              const struct elan_cmd *cmd,
              int                    cmd_timeout)
{
  FpiDeviceElan *self = FPI_DEVICE_ELAN (dev);
  FpiUsbTransfer *transfer;
  GCancellable *cancellable = NULL;

  self->cmd = cmd;
  if (cmd_timeout != -1)
    self->cmd_timeout = cmd_timeout;

  if (cmd->devices != ELAN_ALL_DEV && !(cmd->devices & self->dev_type))
    {
      fp_dbg ("skipping command 0x%x 0x%x for this device (for devices 0x%x but device is 0x%x)",
              cmd->cmd[0], cmd->cmd[1], cmd->devices, self->dev_type);
      elan_cmd_done (ssm);
      return;
    }

  transfer = fpi_usb_transfer_new (dev);
  transfer->ssm = ssm;
  transfer->short_is_error = TRUE;

  fpi_usb_transfer_fill_bulk_full (transfer,
                                   ELAN_EP_CMD_OUT,
                                   (guint8 *) cmd->cmd,
                                   ELAN_CMD_LEN,
                                   NULL);

  if (!self->cmd->never_cancel)
    cancellable = fpi_device_get_cancellable (dev);

  fpi_usb_transfer_submit (transfer,
                           self->cmd_timeout,
                           cancellable,
                           elan_cmd_cb,
                           NULL);
}

enum stop_capture_states {
  STOP_CAPTURE,
  STOP_CAPTURE_NUM_STATES,
};

static void
stop_capture_run_state (FpiSsm *ssm, FpDevice *dev)
{
  G_DEBUG_HERE ();

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case STOP_CAPTURE:
      elan_run_cmd (ssm, dev, &stop_cmd,
                    ELAN_CMD_TIMEOUT);
      break;
    }
}

static void
stop_capture_complete (FpiSsm *ssm, FpDevice *_dev, GError *error)
{
  FpImageDevice *dev = FP_IMAGE_DEVICE (_dev);
  FpiDeviceElan *self = FPI_DEVICE_ELAN (dev);

  G_DEBUG_HERE ();


  /* The device is inactive at this point. */
  self->active = FALSE;

  if (self->deactivating)
    {
      /* Simply complete the pending deactivation. */
      self->deactivating = FALSE;
      fpi_image_device_deactivate_complete (dev, error);
      return;
    }

  if (!error)
    fpi_image_device_report_finger_status (dev, FALSE);
  else
    /* NOTE: We cannot get a cancellation error here. */
    fpi_image_device_session_error (dev, error);
}

static void
elan_stop_capture (FpiDeviceElan *self)
{
  G_DEBUG_HERE ();

  elan_dev_reset_state (self);

  FpiSsm *ssm =
    fpi_ssm_new (FP_DEVICE (self), stop_capture_run_state, STOP_CAPTURE_NUM_STATES);

  fpi_ssm_start (ssm, stop_capture_complete);
}

enum capture_states {
  CAPTURE_LED_ON,
  CAPTURE_WAIT_FINGER,
  CAPTURE_READ_DATA,
  CAPTURE_CHECK_ENOUGH_FRAMES,
  CAPTURE_NUM_STATES,
};

static void
capture_run_state (FpiSsm *ssm, FpDevice *dev)
{
  FpImageDevice *idev = FP_IMAGE_DEVICE (dev);
  FpiDeviceElan *self = FPI_DEVICE_ELAN (dev);
  int r;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case CAPTURE_LED_ON:
      elan_run_cmd (ssm, dev, &led_on_cmd, ELAN_CMD_TIMEOUT);
      break;

    case CAPTURE_WAIT_FINGER:
      /* Press mode waits for the first touch as long as it takes (0 = no
       * timeout); a timeout here would be reported as a too-short touch. */
      if ((self->dev_type & ELAN_PRESS) && self->num_frames == 0 &&
          !self->press_submitted)
        elan_run_cmd (ssm, dev, &pre_scan_cmd, 0);
      else
        elan_run_cmd (ssm, dev, &pre_scan_cmd, -1);
      break;

    case CAPTURE_READ_DATA:
      /* 0x55 - finger present
       * 0xff - device not calibrated (probably) */
      if (self->last_read && self->last_read[0] == 0x55)
        {
          fpi_image_device_report_finger_status (idev, TRUE);
          elan_run_cmd (ssm, dev, &get_image_cmd, ELAN_CMD_TIMEOUT);
        }
      else if (self->dev_type & ELAN_0C1A)
        {
          /* 0c1a firmware answers the pre-scan command immediately instead
           * of blocking until a finger is placed. Poll it until the finger
           * arrives; otherwise a capture started before the touch (e.g. at
           * the login screen) aborts with a protocol error. */
          fp_dbg ("pre-scan returned 0x%02x (%u byte(s)), %d frame(s) captured",
                  self->last_read ? self->last_read[0] : 0,
                  (unsigned int) self->last_read_len,
                  self->num_frames);

          if (self->num_frames > 0)
            {
              /* The finger was already being captured; the sensor reports
               * the lift with a non-0x55 answer instead of a timeout. */
              fpi_ssm_mark_completed (ssm);
            }
          else if (g_cancellable_is_cancelled (fpi_device_get_cancellable (dev)))
            {
              fpi_ssm_mark_failed (ssm,
                                   g_error_new_literal (G_IO_ERROR,
                                                        G_IO_ERROR_CANCELLED,
                                                        "Cancelled"));
            }
          else
            {
              fpi_ssm_jump_to_state_delayed (ssm, CAPTURE_WAIT_FINGER, 100);
            }
        }
      else
        {
          /* XXX: The timeout is emulated incorrectly, resulting in a zero byte read. */
          if (g_strcmp0 (g_getenv ("FP_DEVICE_EMULATION"), "1") == 0)
            fpi_ssm_mark_completed (ssm);
          else
            fpi_ssm_mark_failed (ssm, fpi_device_error_new (FP_DEVICE_ERROR_PROTO));
        }
      break;

    case CAPTURE_CHECK_ENOUGH_FRAMES:
      r = elan_save_img_frame (self);
      if (r < 0 && (self->dev_type & ELAN_PRESS))
        {
          /* The finger landed during calibration, ask for a new touch. The
           * next calibration waits for the finger to lift. */
          self->press_submitted = TRUE;
          fpi_image_device_retry_scan (idev, FP_DEVICE_RETRY_REMOVE_FINGER);
          fpi_ssm_mark_completed (ssm);
          break;
        }
      if (r < 0)
        {
          fpi_ssm_mark_failed (ssm, fpi_device_error_new (FP_DEVICE_ERROR_GENERAL));
          break;
        }

      /* Report the touch and end it right away, without waiting for the
       * sensor to notice the lift (it can take seconds), so verify and the
       * enroll duplicate check answer at once. The next calibration waits
       * for the finger to lift. */
      if ((self->dev_type & ELAN_PRESS) &&
          self->num_frames >= ELAN_PRESS_FRAMES)
        {
          self->press_submitted = TRUE;
          elan_submit_press_image (idev);
          fpi_ssm_mark_completed (ssm);
          break;
        }

      if (self->num_frames < ELAN_MAX_FRAMES || (self->dev_type & ELAN_PRESS))
        {
          /* quickly stop if finger is removed */
          self->cmd_timeout = ELAN_FINGER_TIMEOUT;
          fpi_ssm_jump_to_state (ssm, CAPTURE_WAIT_FINGER);
        }
      else
        {
          fpi_ssm_next_state (ssm);
        }
      break;
    }
}

static void
capture_complete (FpiSsm *ssm, FpDevice *_dev, GError *error)
{
  FpImageDevice *dev = FP_IMAGE_DEVICE (_dev);
  FpiDeviceElan *self = FPI_DEVICE_ELAN (_dev);
  gboolean press = self->dev_type & ELAN_PRESS;
  int min_frames = press ? ELAN_PRESS_FRAMES : ELAN_MIN_FRAMES;

  G_DEBUG_HERE ();

  /* either max frames captured or timed out waiting for the next frame */
  if (!error ||
      (g_error_matches (error, G_USB_DEVICE_ERROR, G_USB_DEVICE_ERROR_TIMED_OUT) &&
       fpi_ssm_get_cur_state (ssm) == CAPTURE_WAIT_FINGER))
    {
      /* press mode reports the touch from the capture loop instead */
      if (!press && self->num_frames >= min_frames)
        elan_submit_image (dev);

      if (!self->press_submitted && (press || self->num_frames < min_frames))
        {
          fp_dbg ("%s too short: want >= %d frames, got %d",
                  press ? "press" : "swipe", min_frames, self->num_frames);
          fpi_image_device_retry_scan (dev, FP_DEVICE_RETRY_TOO_SHORT);
        }
      g_clear_error (&error);
    }
  else
    {
      fpi_image_device_session_error (dev, error);
    }

  /* Note: We always stop capturing even if that may not be needed always.
   * Doing this between captures appears to make it at least less likely for
   * devices to end up in a bad state.
   */
  elan_stop_capture (self);
}

static void
elan_capture (FpiDeviceElan *self)
{
  G_DEBUG_HERE ();

  elan_dev_reset_state (self);
  FpiSsm *ssm =
    fpi_ssm_new (FP_DEVICE (self), capture_run_state, CAPTURE_NUM_STATES);

  fpi_ssm_start (ssm, capture_complete);
}

/* this function needs to have elandev->background and elandev->last_read to be
 * the calibration mean */
static int
elan_need_calibration (FpiDeviceElan *elandev)
{
  G_DEBUG_HERE ();

  unsigned short calib_mean =
    elandev->last_read[0] * 0xff + elandev->last_read[1];
  unsigned int bg_mean = 0, delta;
  unsigned int frame_size = elandev->frame_width * elandev->frame_height;

  g_assert (frame_size != 0);

  if (elandev->dev_type == ELAN_0C42)
    {
      if (calib_mean > 5500 ||
          calib_mean < 2500)
        {
          fp_dbg ("Forcing needed recalibration");
          return 1;
        }
    }

  for (int i = 0; i < frame_size; i++)
    bg_mean += elandev->background[i];
  bg_mean /= frame_size;

  delta =
    bg_mean > calib_mean ? bg_mean - calib_mean : calib_mean - bg_mean;

  fp_dbg ("calibration mean: %d, bg mean: %d, delta: %d", calib_mean,
          bg_mean, delta);
  elandev->calib_delta = delta;

  return delta > ELAN_CALIBRATION_MAX_DELTA ? 1 : 0;
}

enum calibrate_states {
  CALIBRATE_GET_BACKGROUND,
  CALIBRATE_SAVE_BACKGROUND,
  CALIBRATE_GET_MEAN,
  CALIBRATE_CHECK_NEEDED,
  CALIBRATE_GET_STATUS,
  CALIBRATE_CHECK_STATUS,
  CALIBRATE_REPEAT_STATUS,
  CALIBRATE_NUM_STATES,
};

static gboolean
elan_supports_calibration (FpiDeviceElan *elandev)
{
  if (elandev->dev_type == ELAN_0C42)
    return TRUE;

  return elandev->fw_ver >= ELAN_MIN_CALIBRATION_FW;
}

static void
calibrate_run_state (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceElan *self = FPI_DEVICE_ELAN (dev);

  G_DEBUG_HERE ();

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case CALIBRATE_GET_BACKGROUND:
      elan_run_cmd (ssm, dev, &get_image_cmd, ELAN_CMD_TIMEOUT);
      break;

    case CALIBRATE_SAVE_BACKGROUND:
      elan_save_background (self);
      if (!elan_supports_calibration (self))
        {
          fp_dbg ("FW does not support calibration");
          fpi_ssm_mark_completed (ssm);
        }
      else
        {
          fpi_ssm_next_state (ssm);
        }
      break;

    case CALIBRATE_GET_MEAN:
      elan_run_cmd (ssm, dev, &get_calib_mean_cmd, ELAN_CMD_TIMEOUT);
      break;

    case CALIBRATE_CHECK_NEEDED:
      if (g_cancellable_is_cancelled (fpi_device_get_cancellable (dev)))
        {
          fpi_ssm_mark_failed (ssm,
                               g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                                    "Cancelled"));
          break;
        }

      if (elan_need_calibration (self))
        {
          if ((self->dev_type & ELAN_0C1A) &&
              self->calib_delta > ELAN_0C1A_FINGER_DELTA &&
              self->finger_waits_left)
            {
              /* A finger is still on the sensor. Calibrating now would bake
               * it into the sensor baseline, so take the background again
               * after waiting for the finger to lift. */
              self->finger_waits_left -= 1;
              fp_dbg ("finger on sensor during calibration (delta %u), waiting (%u tries left)",
                      self->calib_delta, self->finger_waits_left);
              fpi_ssm_jump_to_state_delayed (ssm, CALIBRATE_GET_BACKGROUND,
                                             ELAN_0C1A_FINGER_WAIT);
            }
          else
            {
              self->calib_status = 0;
              fpi_ssm_next_state (ssm);
            }
        }
      else
        {
          fpi_ssm_mark_completed (ssm);
        }
      break;

    case CALIBRATE_GET_STATUS:
      self->calib_atts_left -= 1;
      if (self->calib_atts_left)
        {
          elan_run_cmd (ssm, dev, &get_calib_status_cmd,
                        ELAN_CMD_TIMEOUT);
        }
      else if ((self->dev_type & ELAN_PRESS) && self->calib_rounds_left)
        {
          /* most likely a finger on the sensor, wait for it to lift */
          fp_dbg ("calibration failed, starting over (%d rounds left)",
                  self->calib_rounds_left);
          self->calib_rounds_left -= 1;
          self->calib_atts_left = ELAN_CALIBRATION_ATTEMPTS;
          fpi_ssm_jump_to_state_delayed (ssm, CALIBRATE_GET_BACKGROUND,
                                         ELAN_PRESS_CALIBRATION_DELAY);
        }
      else
        {
          fp_dbg ("calibration failed");
          fpi_ssm_mark_failed (ssm,
                               fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                                         "Calibration failed!"));
        }
      break;

    case CALIBRATE_CHECK_STATUS:
      /* 0x01 - retry, 0x03 - ok
       * It appears that when reading the response soon after 0x4023 the device
       * can return 0x03, and only after some time (up to 100 ms) the response
       * changes to 0x01. It stays that way for some time and then changes back
       * to 0x03. Because of this we don't just expect 0x03, we want to see 0x01
       * first. This is to make sure that a full calibration loop has completed */
      fp_dbg ("calibration status: 0x%02x", self->last_read[0]);
      if (self->calib_status == 0x01 &&
          self->last_read[0] == 0x03)
        {
          self->calib_status = 0x03;
          fpi_ssm_jump_to_state (ssm, CALIBRATE_GET_BACKGROUND);
        }
      else
        {
          if (self->calib_status == 0x00 &&
              self->last_read[0] == 0x01)
            self->calib_status = 0x01;
          fpi_ssm_next_state_delayed (ssm, 50);
        }
      break;

    case CALIBRATE_REPEAT_STATUS:
      fpi_ssm_jump_to_state (ssm, CALIBRATE_GET_STATUS);
      break;
    }
}

static void
calibrate_complete (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  G_DEBUG_HERE ();

  if (error)
    {
      fpi_image_device_session_error (FP_IMAGE_DEVICE (dev), error);
      elan_stop_capture (FPI_DEVICE_ELAN (dev));
    }
  else
    {
      elan_capture (FPI_DEVICE_ELAN (dev));
    }
}

static void
elan_calibrate (FpiDeviceElan *self)
{
  G_DEBUG_HERE ();

  elan_dev_reset_state (self);

  g_return_if_fail (!self->active);
  self->active = TRUE;
  self->calib_atts_left = ELAN_CALIBRATION_ATTEMPTS;
  self->calib_rounds_left = ELAN_PRESS_CALIBRATION_ROUNDS;
  self->finger_waits_left = ELAN_0C1A_FINGER_WAITS;

  FpiSsm *ssm = fpi_ssm_new (FP_DEVICE (self), calibrate_run_state,
                             CALIBRATE_NUM_STATES);

  fpi_ssm_start (ssm, calibrate_complete);
}

enum activate_states {
  ACTIVATE_GET_FW_VER,
  ACTIVATE_SET_FW_VER,
  ACTIVATE_GET_SENSOR_DIM,
  ACTIVATE_SET_SENSOR_DIM,
  ACTIVATE_CMD_1,
  ACTIVATE_NUM_STATES,
};

static void
activate_run_state (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceElan *self = FPI_DEVICE_ELAN (dev);

  G_DEBUG_HERE ();

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case ACTIVATE_GET_FW_VER:
      elan_run_cmd (ssm, dev, &get_fw_ver_cmd, ELAN_CMD_TIMEOUT);
      break;

    case ACTIVATE_SET_FW_VER:
      self->fw_ver =
        (self->last_read[0] << 8 | self->last_read[1]);
      fp_dbg ("FW ver 0x%04hx", self->fw_ver);
      fpi_ssm_next_state (ssm);
      break;

    case ACTIVATE_GET_SENSOR_DIM:
      elan_run_cmd (ssm, dev, &get_sensor_dim_cmd, ELAN_CMD_TIMEOUT);
      break;

    case ACTIVATE_SET_SENSOR_DIM:
      /* see elan_save_frame for details */
      if (self->dev_type & ELAN_NOT_ROTATED)
        {
          self->frame_width = self->last_read[0];
          self->frame_height = self->raw_frame_height =
            self->last_read[2];
        }
      else
        {
          self->frame_width = self->last_read[2];
          self->frame_height = self->raw_frame_height =
            self->last_read[0];
        }
      /* Work-around sensors returning the sizes as zero-based index
       * rather than the number of pixels. */
      if ((self->frame_width % 2 == 1) &&
          (self->frame_height % 2 == 1))
        {
          self->frame_width++;
          self->frame_height++;
          self->raw_frame_height = self->frame_height;
        }
      if (self->frame_height > ELAN_MAX_FRAME_HEIGHT &&
          !(self->dev_type & ELAN_PRESS))
        self->frame_height = ELAN_MAX_FRAME_HEIGHT;
      fp_dbg ("sensor dimensions, WxH: %dx%d", self->frame_width,
              self->raw_frame_height);
      fpi_ssm_next_state (ssm);
      break;

    case ACTIVATE_CMD_1:
      /* TODO: find out what this does, if we need it */
      elan_run_cmd (ssm, dev, &activate_cmd_1, ELAN_CMD_TIMEOUT);
      break;
    }
}

static void
activate_complete (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpImageDevice *idev = FP_IMAGE_DEVICE (dev);

  G_DEBUG_HERE ();

  fpi_image_device_activate_complete (idev, error);

}

static void
elan_activate (FpImageDevice *dev)
{
  FpiDeviceElan *self = FPI_DEVICE_ELAN (dev);

  G_DEBUG_HERE ();
  elan_dev_reset_state (self);

  FpiSsm *ssm =
    fpi_ssm_new (FP_DEVICE (dev), activate_run_state,
                 ACTIVATE_NUM_STATES);

  fpi_ssm_start (ssm, activate_complete);
}

static void
dev_init (FpImageDevice *dev)
{
  GError *error = NULL;
  FpiDeviceElan *self;

  G_DEBUG_HERE ();

  if (!g_usb_device_claim_interface (fpi_device_get_usb_device (FP_DEVICE (dev)), 0, 0, &error))
    {
      fpi_image_device_open_complete (dev, error);
      return;
    }

  self = FPI_DEVICE_ELAN (dev);

  /* common params */
  self->dev_type = fpi_device_get_driver_data (FP_DEVICE (dev));
  self->background = NULL;
  self->process_frame = elan_process_frame_thirds;

  switch (self->dev_type)
    {
    case ELAN_0907:
      self->process_frame = elan_process_frame_linear;
      break;
    }

  if (self->dev_type & ELAN_PRESS)
    {
      fpi_device_set_scan_type (FP_DEVICE (dev), FP_SCAN_TYPE_PRESS);
      fpi_device_set_nr_enroll_stages (FP_DEVICE (dev), ELAN_PRESS_ENROLL_STAGES);
      /* 144x64 is too small for NBIS minutiae; match ridge texture instead */
      fpi_image_device_set_matcher (dev, elan_sift_img_to_print, elan_sift_match);
    }

  fpi_image_device_open_complete (dev, NULL);
}

static void
dev_deinit (FpImageDevice *dev)
{
  GError *error = NULL;
  FpiDeviceElan *self = FPI_DEVICE_ELAN (dev);

  G_DEBUG_HERE ();

  elan_dev_reset_state (self);
  g_free (self->background);
  g_usb_device_release_interface (fpi_device_get_usb_device (FP_DEVICE (dev)),
                                  0, 0, &error);
  fpi_image_device_close_complete (dev, error);
}

static void
dev_activate (FpImageDevice *dev)
{
  G_DEBUG_HERE ();
  elan_activate (dev);
}

static void
dev_change_state (FpImageDevice *dev, FpiImageDeviceState state)
{
  FpiDeviceElan *self = FPI_DEVICE_ELAN (dev);

  G_DEBUG_HERE ();

  /* Note: We always calibrate even if that may not be needed always.
   * Doing this for each capture appears to make it at least less likely for
   * devices to end up in a bad state.
   */
  if (state == FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_ON)
    elan_calibrate (self);
}

static void
dev_deactivate (FpImageDevice *dev)
{
  FpiDeviceElan *self = FPI_DEVICE_ELAN (dev);

  G_DEBUG_HERE ();

  if (!self->active)
    /* The device is inactive already, complete the operation immediately. */
    fpi_image_device_deactivate_complete (dev, NULL);
  else
    /* The device is not yet inactive, flag that we are deactivating (and
     * need to signal back deactivation).
     * Note that any running capture will be cancelled already if needed. */
    self->deactivating = TRUE;
}

static void
fpi_device_elan_init (FpiDeviceElan *self)
{
}
static void
fpi_device_elan_class_init (FpiDeviceElanClass *klass)
{
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);
  FpImageDeviceClass *img_class = FP_IMAGE_DEVICE_CLASS (klass);

  dev_class->id = "elan";
  dev_class->full_name = "ElanTech Fingerprint Sensor";
  dev_class->type = FP_DEVICE_TYPE_USB;
  dev_class->id_table = elan_id_table;
  dev_class->scan_type = FP_SCAN_TYPE_SWIPE;

  img_class->img_open = dev_init;
  img_class->img_close = dev_deinit;
  img_class->activate = dev_activate;
  img_class->deactivate = dev_deactivate;
  img_class->change_state = dev_change_state;

  img_class->bz3_threshold = 24;
}
