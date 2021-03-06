/* GStreamer
 * Copyright (C) <2018> Edward Hervey <edward@centricular.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifndef __GST_VIDEO_ANC_H__
#define __GST_VIDEO_ANC_H__

#include <gst/gst.h>
#include <gst/video/video-format.h>
#include <gst/video/video-info.h>

G_BEGIN_DECLS

typedef struct _GstVideoAncillary GstVideoAncillary;

/**
 * GstVideoAncillary:
 * @DID: The Data Identifier
 * @SDID_block_number: The Secondary Data Identifier (if type 2) or the Data
 *                     Block Number (if type 1)
 * @data_count: The amount of data (in bytes) in @data (max 255 bytes)
 * @data: (array length=data_count): The user data content of the Ancillary packet.
 *    Does not contain the ADF, DID, SDID nor CS.
 *
 * Video Ancillary data, according to SMPTE-291M specification.
 *
 * Note that the contents of the data are always stored as 8bit data (i.e. do not contain
 * the parity check bits).
 *
 * Since: 1.16
 */
struct _GstVideoAncillary {
  guint8 DID;
  guint8 SDID_block_number;
  guint8 data_count;
  guint8 data[256];

  /*< private >*/
  /* Padding for future extension */
  gpointer _gst_reserved[GST_PADDING];
};

typedef enum {
  GST_VIDEO_ANCILLARY_DID_UNDEFINED = 0x00,
  GST_VIDEO_ANCILLARY_DID_DELETION  = 0x80,
  GST_VIDEO_ANCILLARY_DID_HANC_3G_AUDIO_DATA_FIRST = 0xa0,
  GST_VIDEO_ANCILLARY_DID_HANC_3G_AUDIO_DATA_LAST = 0xa7,
  GST_VIDEO_ANCILLARY_DID_HANC_HDTV_AUDIO_DATA_FIRST = 0xe0,
  GST_VIDEO_ANCILLARY_DID_HANC_HDTV_AUDIO_DATA_LAST = 0xe7,
  GST_VIDEO_ANCILLARY_DID_HANC_SDTV_AUDIO_DATA_1_FIRST = 0xec,
  GST_VIDEO_ANCILLARY_DID_HANC_SDTV_AUDIO_DATA_1_LAST = 0xef,
  GST_VIDEO_ANCILLARY_DID_CAMERA_POSITION = 0xf0,
  GST_VIDEO_ANCILLARY_DID_HANC_ERROR_DETECTION = 0xf4,
  GST_VIDEO_ANCILLARY_DID_HANC_SDTV_AUDIO_DATA_2_FIRST = 0xf8,
  GST_VIDEO_ANCILLARY_DID_HANC_SDTV_AUDIO_DATA_2_LAST = 0xff,
} GstVideoAncillaryDID;

/**
 * GST_VIDEO_ANCILLARY_DID16:
 * @anc: a #GstVideoAncillary
 *
 * Returns the #GstVideoAncillaryDID16 of the ancillary data.
 *
 * Since: 1.16
 *
 * Returns: a #GstVideoAncillaryDID16 identifier
 */
#define GST_VIDEO_ANCILLARY_DID16(anc) ((guint16)((anc)->DID) << 8 | (guint16)((anc)->SDID_block_number))

/**
 * GstVideoAncillaryDID16:
 * @GST_VIDEO_ANCILLARY_DID16_S334_EIA_708: CEA 708 Ancillary data according to SMPTE 334
 * @GST_VIDEO_ANCILLARY_DID16_S334_EIA_608: CEA 608 Ancillary data according to SMPTE 334
 *
 * Some know types of Ancillary Data identifiers.
 *
 * Since: 1.16
 */
typedef enum {
  GST_VIDEO_ANCILLARY_DID16_S334_EIA_708	= 0x6101,
  GST_VIDEO_ANCILLARY_DID16_S334_EIA_608	= 0x6102,
} GstVideoAncillaryDID16;

/* Closed Caption support */
/**
 * GstVideoCaptionType:
 * @GST_VIDEO_CAPTION_TYPE_UNKNOWN: Unknown type of CC
 * @GST_VIDEO_CAPTION_TYPE_CEA608_RAW: CEA-608 as byte pairs. Note that
 *      this format is not recommended since is does not specify to
 *      which field the caption comes from and therefore assumes
 *      it comes from the first field (and that there is no information
 *      on the second field). Use @@GST_VIDEO_CAPTION_TYPE_CEA608_IN_CEA708_RAW
 *      if you wish to store CEA-608 from two fields.
 * @GST_VIDEO_CAPTION_TYPE_CEA608_IN_CEA708_RAW: CEA-608 as cc_data byte triplets.
 *      The first byte of each triplet shall specify the field as in CEA-708
 *      (i.e: 0xFC for the first field or 0xFD for the second field.). The 2nd
 *      and 3rd byte of each triplet are the cc1 and cc2 bytes. Use this if
 *      there is *only* CEA-608 caption. If there is also CEA-708 caption,
 *      use @GST_VIDEO_CAPTION_TYPE_CEA708_RAW.
 * @GST_VIDEO_CAPTION_TYPE_CEA708_RAW: CEA-708 as cc_data byte triplets. They
 *      can also contain 608-in-708.
 * @GST_VIDEO_CAPTION_TYPE_CEA708_CDP: CEA-708 (and optionally CEA-608) in
 *      a CDP (Caption Distribution Packet) defined by SMPTE S-334-2.
 *      Contains the whole CDP (starting with 0x9669).
 *
 * The various known types of Closed Caption (CC).
 *
 * Since: 1.16
 */
typedef enum {
  GST_VIDEO_CAPTION_TYPE_UNKNOWN                = 0,
  GST_VIDEO_CAPTION_TYPE_CEA608_RAW		= 1,
  GST_VIDEO_CAPTION_TYPE_CEA608_IN_CEA708_RAW   = 2,
  GST_VIDEO_CAPTION_TYPE_CEA708_RAW		= 3,
  GST_VIDEO_CAPTION_TYPE_CEA708_CDP		= 4
} GstVideoCaptionType;

/**
 * GstVideoCaptionMeta:
 * @meta: parent #GstMeta
 * @caption_type: The type of Closed Caption contained in the meta.
 * @data: (array length=size): The Closed Caption data.
 * @size: The size in bytes of @data
 *
 * Extra buffer metadata providing Closed Caption.
 *
 * Since: 1.16
 */
typedef struct {
  GstMeta meta;

  GstVideoCaptionType caption_type;
  guint8 *data;
  gsize size;
} GstVideoCaptionMeta;

GST_VIDEO_API
GType	gst_video_caption_meta_api_get_type (void);
#define GST_VIDEO_CAPTION_META_API_TYPE (gst_video_caption_meta_api_get_type())

GST_VIDEO_API
const GstMetaInfo *gst_video_caption_meta_get_info (void);
#define GST_VIDEO_CAPTION_META_INFO (gst_video_caption_meta_get_info())

/**
 * gst_buffer_get_video_caption_meta:
 * @b: A #GstBuffer
 *
 * Gets the #GstVideoCaptionMeta that might be present on @b.
 *
 * Since: 1.16
 *
 * Returns: The first #GstVideoCaptionMeta present on @b, or %NULL if
 * no #GstVideoCaptionMeta are present
 */
#define gst_buffer_get_video_caption_meta(b) \
        ((GstVideoCaptionMeta*)gst_buffer_get_meta((b),GST_VIDEO_CAPTION_META_API_TYPE))

GST_VIDEO_API
GstVideoCaptionMeta *gst_buffer_add_video_caption_meta    (GstBuffer   * buffer,
							   GstVideoCaptionType caption_type,
							   const guint8 *data,
							   gsize size);

/**
 * GstVideoVBIParser:
 *
 * A parser for detecting and extracting @GstVideoAncillary data from
 * Vertical Blanking Interval lines of component signals.
 *
 * Since: 1.16
 */

typedef struct _GstVideoVBIParser GstVideoVBIParser;

GST_VIDEO_API
GType gst_video_vbi_parser_get_type (void);

/**
 * GstVideoVBIParserResult:
 * @GST_VIDEO_VBI_PARSER_RESULT_DONE: No line were provided, or no more Ancillary data was found.
 * @GST_VIDEO_VBI_PARSER_RESULT_OK: A #GstVideoAncillary was found.
 * @GST_VIDEO_VBI_PARSER_RESULT_ERROR: An error occured
 *
 * Return values for #GstVideoVBIParser
 *
 * Since: 1.16
 */
typedef enum {
  GST_VIDEO_VBI_PARSER_RESULT_DONE  = 0,
  GST_VIDEO_VBI_PARSER_RESULT_OK    = 1,
  GST_VIDEO_VBI_PARSER_RESULT_ERROR = 2
} GstVideoVBIParserResult;

GST_VIDEO_API
GstVideoVBIParserResult gst_video_vbi_parser_get_ancillary(GstVideoVBIParser *parser,
							   GstVideoAncillary *anc);

GST_VIDEO_API
GstVideoVBIParser *gst_video_vbi_parser_new (GstVideoFormat format, guint32 pixel_width);

GST_VIDEO_API
GstVideoVBIParser *gst_video_vbi_parser_copy (const GstVideoVBIParser *parser);

GST_VIDEO_API
void               gst_video_vbi_parser_free (GstVideoVBIParser *parser);

GST_VIDEO_API
void		   gst_video_vbi_parser_add_line (GstVideoVBIParser *parser, const guint8 *data);

/**
 * GstVideoVBIEncoder:
 *
 * An encoder for writing ancillary data to the
 * Vertical Blanking Interval lines of component signals.
 *
 * Since: 1.16
 */

typedef struct _GstVideoVBIEncoder GstVideoVBIEncoder;

GST_VIDEO_API
GType gst_video_vbi_encoder_get_type (void);

GST_VIDEO_API
GstVideoVBIEncoder *gst_video_vbi_encoder_new  (GstVideoFormat format, guint32 pixel_width);

GST_VIDEO_API
GstVideoVBIEncoder *gst_video_vbi_encoder_copy (const GstVideoVBIEncoder *encoder);

GST_VIDEO_API
void               gst_video_vbi_encoder_free  (GstVideoVBIEncoder *encoder);

GST_VIDEO_API
gboolean gst_video_vbi_encoder_add_ancillary   (GstVideoVBIEncoder *encoder,
                                                gboolean            composite,
                                                guint8              DID,
                                                guint8              SDID_block_number,
                                                const guint8       *data,
                                                guint               data_count);

GST_VIDEO_API
void gst_video_vbi_encoder_write_line (GstVideoVBIEncoder *encoder, guint8 *data);

G_END_DECLS

#endif /* __GST_VIDEO_ANC_H__ */
