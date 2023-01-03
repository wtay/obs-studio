/*
Copyright (C) 2020 by Morten Bøgeskov <source@kosmisk.dk>

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

#include <spa/param/format.h>

#include <obs-module.h>


#include "pipewire-decoder.h"

#define blog(level, msg, ...) \
	blog(level, "pipewire-source: decoder: " msg, ##__VA_ARGS__)

int pipewire_init_decoder(struct pipewire_decoder *decoder, uint32_t subtype)
{
	switch (subtype) {
	case SPA_MEDIA_SUBTYPE_mjpg:
		decoder->codec = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
		if (decoder->codec == NULL) {
			blog(LOG_ERROR, "failed to find MJPEG decoder");
			return -1;
		}
		break;
	case SPA_MEDIA_SUBTYPE_h264:
		decoder->codec = avcodec_find_decoder(AV_CODEC_ID_H264);
		if (decoder->codec == NULL) {
			blog(LOG_ERROR, "failed to find H264 decoder");
			return -1;
		}
		break;
	default:
		return -1;
	}

	decoder->context = avcodec_alloc_context3(decoder->codec);
	if (!decoder->context) {
		return -1;
	}

	decoder->packet = av_packet_alloc();
	if (!decoder->packet) {
		return -1;
	}

	decoder->frame = av_frame_alloc();
	if (!decoder->frame) {
		return -1;
	}

	decoder->context->flags2 |= AV_CODEC_FLAG2_FAST;

	if (avcodec_open2(decoder->context, decoder->codec, NULL) < 0) {
		blog(LOG_ERROR, "failed to open codec");
		return -1;
	}

	blog(LOG_DEBUG, "initialized avcodec");

	return 0;
}

void pipewire_destroy_decoder(struct pipewire_decoder *decoder)
{
	blog(LOG_DEBUG, "destroying avcodec");
	if (decoder->frame) {
		av_frame_free(&decoder->frame);
	}

	if (decoder->packet) {
		av_packet_free(&decoder->packet);
	}

	if (decoder->context) {
		avcodec_close(decoder->context);
		avcodec_free_context(&decoder->context);
	}
}

int pipewire_decode_frame(struct obs_source_frame *out, uint8_t *data,
		      size_t length, struct pipewire_decoder *decoder)
{
	decoder->packet->data = data;
	decoder->packet->size = length;
	if (avcodec_send_packet(decoder->context, decoder->packet) < 0) {
		blog(LOG_ERROR, "failed to send frame to codec");
		return -1;
	}

	if (avcodec_receive_frame(decoder->context, decoder->frame) < 0) {
		blog(LOG_ERROR, "failed to receive frame from codec");
		return -1;
	}

	out->width = decoder->frame->width;
        out->height = decoder->frame->height;

	video_format_get_parameters(VIDEO_CS_DEFAULT,
				VIDEO_RANGE_FULL,
				out->color_matrix,
				out->color_range_min,
				out->color_range_max);

	for (uint_fast32_t i = 0; i < MAX_AV_PLANES; ++i) {
		out->data[i] = decoder->frame->data[i];
		out->linesize[i] = decoder->frame->linesize[i];
	}

	switch (decoder->context->pix_fmt) {
	case AV_PIX_FMT_YUVJ422P:
	case AV_PIX_FMT_YUV422P:
		out->format = VIDEO_FORMAT_I422;
		break;
	case AV_PIX_FMT_YUVJ420P:
	case AV_PIX_FMT_YUV420P:
		out->format = VIDEO_FORMAT_I420;
		break;
	case AV_PIX_FMT_YUVJ444P:
	case AV_PIX_FMT_YUV444P:
		out->format = VIDEO_FORMAT_I444;
		break;
	}

	return 0;
}
