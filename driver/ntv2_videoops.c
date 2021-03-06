/*
 * NTV2 video stream channel ops
 *
 * Copyright 2016 AJA Video Systems Inc. All rights reserved.
 *
 * This program is free software; you may redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "ntv2_videoops.h"
#include "ntv2_channel.h"
#include "ntv2_konareg.h"
#include "ntv2_register.h"
#include "ntv2_features.h"

int ntv2_videoops_setup_capture(struct ntv2_channel_stream *stream)
{
	struct ntv2_channel *ntv2_chn = stream->ntv2_chn;
	struct ntv2_features *features = ntv2_chn->features;
	int index = ntv2_chn->index;
	int buf_index;
	int i;

	/* initialize video input stream data */
	INIT_LIST_HEAD(&stream->data_ready_list);
	INIT_LIST_HEAD(&stream->data_done_list);
	stream->queue_run = false;
	stream->queue_last = false;
	stream->video.frame_active	= NULL;
	stream->video.frame_next	= NULL;
	stream->video.total_frame_count = 0;
	stream->video.total_drop_count = 0;
	stream->video.stat_frame_count = 0;
	stream->video.stat_drop_count = 0;
	stream->video.last_display_time = 0;
	for (i = 0; i < NTV2_MAX_CHANNELS; i++)
		stream->video.hardware_enable[i] = false;

	/* get the video frame buffer frame range and size */
	ntv2_features_get_frame_range(features,
								  &stream->video_format,
								  &stream->pixel_format,
								  index,
								  &stream->video.frame_first,
								  &stream->video.frame_last,
								  &stream->video.frame_size);

	/* initialize video data buffers */
	buf_index = 0;
	for (i = stream->video.frame_first; i <= stream->video.frame_last; i++) {
		stream->data_array[buf_index].index = buf_index;
		stream->data_array[buf_index].type = stream->type;
		INIT_LIST_HEAD(&stream->data_array[buf_index].list);
		stream->data_array[buf_index].ntv2_str = stream;
		stream->data_array[buf_index].video.frame_number = i;
		stream->data_array[buf_index].video.address = i * stream->video.frame_size;
		stream->data_array[buf_index].video.data_size = 0;
		list_add_tail(&stream->data_array[buf_index].list, &stream->data_done_list);
		buf_index++;
	}

	/* initialize the frame store capture buffer */
	stream->video.frame_next = list_first_entry(&stream->data_done_list, struct ntv2_stream_data, list);
	list_del_init(&stream->video.frame_next->list);

	/* setup hardware */
	stream->ops.update_timing(stream);
	stream->ops.update_format(stream);
	stream->ops.update_route(stream);

	return 0;
}

int ntv2_videoops_update_mode(struct ntv2_channel_stream *stream)
{
	struct ntv2_channel *ntv2_chn = stream->ntv2_chn;
	u32 val = stream->queue_enable? 1 : 0;
	u32 mask = NTV2_FLD_MASK(ntv2_kona_fld_frame_capture_enable);
	int index_first = ntv2_chn->index;
	int index_last = ntv2_chn->index + stream->input_format.num_inputs - 1;
	int i;

	/* enable/disable frame store capture mode */
	for (i = 0; i < NTV2_MAX_CHANNELS; i++) {
		if ((i >= index_first) && (i <= index_last)) {
			ntv2_reg_rmw(ntv2_chn->vid_reg, ntv2_kona_reg_frame_control, i, val, mask);
			stream->video.hardware_enable[i] = stream->queue_enable;
//			NTV2_MSG_INFO("%s: write frame control[%d]  enable %d\n", ntv2_chn->name, i, enable);
		} else if (stream->video.hardware_enable[i]) {
			ntv2_reg_rmw(ntv2_chn->vid_reg, ntv2_kona_reg_frame_control, i, 0, mask);
			stream->video.hardware_enable[i] = false;
//			NTV2_MSG_INFO("%s: write frame control[%d]  disable\n", ntv2_chn->name, i);
		}
	}

	return 0;
}

int ntv2_videoops_update_format(struct ntv2_channel_stream *stream)
{
	struct ntv2_channel *ntv2_chn = stream->ntv2_chn;
	int index = ntv2_chn->index;
	u32 val;
	u32 mask;
	int i;

	val = NTV2_FLD_SET(ntv2_kona_fld_frame_buffer_format_b0123, stream->pixel_format.ntv2_pixel_format);
	mask = NTV2_FLD_MASK(ntv2_kona_fld_frame_buffer_format_b0123);

	/* set frame store pixel format */
	for (i = 0; i < stream->input_format.num_inputs; i++) {
		if (ntv2_chn->state == ntv2_channel_state_run) {
			ntv2_reg_rmw(ntv2_chn->vid_reg, ntv2_kona_reg_frame_control, index + i, val, mask);
		} else {
			ntv2_reg_write(ntv2_chn->vid_reg, ntv2_kona_reg_frame_control, index + i, val);
		}
//		NTV2_MSG_INFO("%s: write frame control[%d]  %08x\n", ntv2_chn->name, index + i, val);
	}

	/* update the video frame buffer frame size */
	ntv2_features_get_frame_range(ntv2_chn->features,
								  &stream->video_format,
								  &stream->pixel_format,
								  ntv2_chn->index,
								  &stream->video.frame_first,
								  &stream->video.frame_last,
								  &stream->video.frame_size);
	return 0;
}

int ntv2_videoops_update_timing(struct ntv2_channel_stream *stream)
{
	struct ntv2_channel *ntv2_chn = stream->ntv2_chn;
	int index = ntv2_chn->index;
	int mode_372;
	int mode_sync;
	int mode_quad;
	u32 standard;
	u32 rate;
	u32 val;

	mode_sync =
		((stream->video_format.frame_flags & ntv2_kona_frame_picture_interlaced) != 0)?
		ntv2_kona_reg_sync_frame : ntv2_kona_reg_sync_field;
	mode_quad =
		((stream->video_format.frame_flags & ntv2_kona_frame_square_division) != 0)? 1 : 0;

	/* kluge 372 mode */
	mode_372 =
		((stream->input_format.num_inputs == 2) &&
		 ((stream->input_format.frame_flags & ntv2_kona_frame_hd) != 0) &&
		 ((stream->input_format.frame_flags & ntv2_kona_frame_line_interleaved) != 0))? 1 : 0;

	standard = stream->video_format.video_standard;
	rate = stream->video_format.frame_rate;
	if (mode_372 == 1) {
		if (standard == ntv2_kona_video_standard_1080p)
			standard = ntv2_kona_video_standard_1080i;
		if (rate == ntv2_kona_frame_rate_5000) {
			rate = ntv2_kona_frame_rate_2500;
		} else if (rate == ntv2_kona_frame_rate_5994) {
			rate = ntv2_kona_frame_rate_2997;
		} else if (rate == ntv2_kona_frame_rate_6000) {
			rate = ntv2_kona_frame_rate_3000;
		}
	}

	/* setup video timing, device sync source and sync mode */
	val = NTV2_FLD_SET(ntv2_kona_fld_global_frame_rate_b012, rate & 0x7);
	val |= NTV2_FLD_SET(ntv2_kona_fld_global_frame_rate_b3, rate >> 3);
	val |= NTV2_FLD_SET(ntv2_kona_fld_global_frame_geometry, stream->video_format.frame_geometry);
	val |= NTV2_FLD_SET(ntv2_kona_fld_global_video_standard, standard);
	val |= NTV2_FLD_SET(ntv2_kona_fld_reference_source_b012, ntv2_kona_ref_source_sdiin1 & 0x7);
	val |= NTV2_FLD_SET(ntv2_kona_fld_linkb_p60_mode_ch2, mode_372);
	val |= NTV2_FLD_SET(ntv2_kona_fld_global_reg_sync, mode_sync);
	ntv2_reg_write(ntv2_chn->vid_reg, ntv2_kona_reg_global_control, index, val);
//	NTV2_MSG_INFO("%s: write global control  %08x\n", ntv2_chn->name, val);

	/* set quad bit for square division video */
	val = NTV2_FLD_SET(ntv2_kona_fld_fs1234_quad_mode, mode_quad);
	val |= NTV2_FLD_SET(ntv2_kona_fld_independent_channel_enable, 1);
	val |= NTV2_FLD_SET(ntv2_kona_fld_reference_source_b3, ntv2_kona_ref_source_sdiin1 >> 3);
	ntv2_reg_write(ntv2_chn->vid_reg, ntv2_kona_reg_global_control2, index, val);
//	NTV2_MSG_INFO("%s: write global control2 %08x\n", ntv2_chn->name, val);

	/* update the video frame buffer range */
	ntv2_features_get_frame_range(ntv2_chn->features,
								  &stream->video_format,
								  &stream->pixel_format,
								  ntv2_chn->index,
								  &stream->video.frame_first,
								  &stream->video.frame_last,
								  &stream->video.frame_size);
	return 0;
}

int ntv2_videoops_update_route(struct ntv2_channel_stream *stream)
{
	struct ntv2_channel *ntv2_chn = stream->ntv2_chn;
	bool convert3gb;
	bool in_rgb;
	bool fs_rgb;
	u32 val;
	int i;

	convert3gb =
		((stream->input_format.frame_flags &
		  ntv2_kona_frame_3gb) != 0) &&
		((stream->input_format.frame_flags &
		  ntv2_kona_frame_line_interleaved) != 0);

	in_rgb = (stream->input_format.pixel_flags & ntv2_kona_pixel_rgb) != 0;
	fs_rgb = (stream->pixel_format.pixel_flags & ntv2_kona_pixel_rgb) != 0;

	if (stream->input_format.type == ntv2_input_type_sdi) {
		for (i = 0; i < stream->input_format.num_inputs; i++) {
			/* set sdi to input mode */
			ntv2_sdi_output_transmit_enable(ntv2_chn->vid_reg,
											stream->input_format.input_index + i,
											false);
			/* convert 3gb to 3ga for line interleaved input */
			ntv2_sdi_input_convert_3g_enable(ntv2_chn->vid_reg,
											 stream->input_format.input_index + i,
											 convert3gb);

			/* route sdi to frame store */
			if ((in_rgb && fs_rgb) || (!in_rgb && !fs_rgb)) {
				ntv2_route_sdi_to_fs(ntv2_chn->vid_reg,
									 stream->input_format.input_index + i, 0, in_rgb,
									 ntv2_chn->index + i, 0);
			} else {
				ntv2_route_sdi_to_csc(ntv2_chn->vid_reg,
									  stream->input_format.input_index + i, 0, in_rgb,
									  ntv2_chn->index + i, 0);
				ntv2_route_csc_to_fs(ntv2_chn->vid_reg,
									 ntv2_chn->index + i, 0, !in_rgb,
									 ntv2_chn->index + i, 0);
			}
		}
	}

	if (stream->input_format.type == ntv2_input_type_hdmi) {
		/* configure hdmi rasterizer */
		if (stream->input_format.num_streams > 1) {
			val = NTV2_FLD_SET(ntv2_kona_fld_hdmi_raster_mode, ntv2_kona_hdmi_raster_mode_4k_input);
		} else {
			val = NTV2_FLD_SET(ntv2_kona_fld_hdmi_raster_mode, ntv2_kona_hdmi_raster_mode_hdsd_bidirect);
		}
		ntv2_reg_write(ntv2_chn->vid_reg, ntv2_kona_reg_hdmi_rasterizer_control, ntv2_chn->index, val);

		/* route hdmi to frame store */
		for (i = 0; i < stream->input_format.num_streams; i++) {
			if ((in_rgb && fs_rgb) || (!in_rgb && !fs_rgb)) {
				ntv2_route_hdmi_to_fs(ntv2_chn->vid_reg,
									  stream->input_format.input_index, i, in_rgb,
									  ntv2_chn->index + i, 0);
			} else {
				ntv2_route_hdmi_to_csc(ntv2_chn->vid_reg,
									   stream->input_format.input_index + i, 0, in_rgb,
									   ntv2_chn->index + i, 0);
				ntv2_route_csc_to_fs(ntv2_chn->vid_reg,
									 ntv2_chn->index + i, 0, !in_rgb,
									 ntv2_chn->index + i, 0);
			}
		}
	}

	return 0;
}

int ntv2_videoops_update_frame(struct ntv2_channel_stream *stream)
{
	struct ntv2_channel *ntv2_chn = stream->ntv2_chn;
	int index = ntv2_chn->index;

	/* set the frame store next frame buffer number */
	ntv2_reg_write(ntv2_chn->vid_reg,
				   ntv2_kona_reg_frame_input, index,
				   stream->video.frame_next->video.frame_number);

	return 0;
}

int ntv2_videoops_interrupt_capture(struct ntv2_channel_stream *stream)
{
	struct ntv2_channel *ntv2_chn = stream->ntv2_chn;
	struct ntv2_stream_data *data_ready;
	s64 stat_time = ntv2_chn->dpc_status.stat_time;
	s64 time_us;

	if (!stream->queue_enable)
		return 0;

	/* need an input interrupt */
	if(!ntv2_chn->dpc_status.interrupt_input)
		return 0;

	/* update time stamp */
	stream->timestamp = ntv2_chn->dpc_status.interrupt_time;

	/* this frame can be ready */
	data_ready = stream->video.frame_active;

	/* this is now the active frame */
	stream->video.frame_active = stream->video.frame_next;
	if (stream->video.frame_active != NULL)
		stream->video.frame_active->timestamp = stream->timestamp;

	if (stream->queue_run) {
		if (stream->queue_last) {
			stream->video.total_frame_count++;
			stream->video.stat_frame_count++;
		} else {
			stream->video.total_frame_count = 0;
			stream->video.total_drop_count = 0;
			stream->video.stat_frame_count = 0;
			stream->video.stat_drop_count = 0;
			stream->video.last_display_time = stat_time;
		}

		/* get the next data object */
		if (!list_empty(&stream->data_done_list)) {
			stream->video.frame_next = list_first_entry(&stream->data_done_list,
														struct ntv2_stream_data, list);
			list_del_init(&stream->video.frame_next->list);
		} else {
			stream->video.total_drop_count++;
			stream->video.stat_drop_count++;
		}

		/* add ready frame to queue */
		if ((stream->video.total_frame_count != 0) &&
			(data_ready != NULL) &&
			(data_ready->video.frame_number != stream->video.frame_active->video.frame_number)) {
			data_ready->video.address = data_ready->video.frame_number * stream->video.frame_size;
			data_ready->video.data_size = stream->video.frame_size;
			list_add_tail(&data_ready->list, &stream->data_ready_list);
			NTV2_MSG_CHANNEL_STREAM("%s: video capture data queue %d  buffer %d\n",
									ntv2_chn->name,
									data_ready->index,
									data_ready->video.frame_number);
		}
	}
			
	/* frame store fills frame next */
	stream->ops.update_frame(stream);

	/* cache last enable state */
	stream->queue_last = stream->queue_run;

	/* print statistics */
	if (stream->queue_run && (stream->video.stat_frame_count != 0)) {
		time_us = stat_time - stream->video.last_display_time;
		if (time_us > NTV2_CHANNEL_STATISTIC_INTERVAL)
		{
			NTV2_MSG_CHANNEL_STATISTICS("%s: video frames %4d  drops %4d  time %6d (us)   total frames %lld  drops %lld\n",
										ntv2_chn->name,
										(u32)(stream->video.stat_frame_count),
										(u32)(stream->video.stat_drop_count),
										(u32)(time_us / stream->video.stat_frame_count),
										stream->video.total_frame_count,
										stream->video.total_drop_count);
			
			stream->video.stat_frame_count = 0;
			stream->video.stat_drop_count = 0;
			stream->video.last_display_time = stat_time;
		}
	}

	return 0;
}

