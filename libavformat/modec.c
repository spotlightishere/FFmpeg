/*
 * MO demuxer
 * Copyright (c) 2022 Spotlight Deveaux
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "libavutil/intreadwrite.h"

#include "avformat.h"
#include "demux.h"
#include "internal.h"

#define MO_TAG MKTAG('M', 'O', 'C', '5')

#define FORMAT_MARKER(a,b) ((a) | ((b) << 8))

// TODO: can this become a better formatted enum?
// What C specification are we using?

// Metadata types
#define FORMAT_LENGTH           FORMAT_MARKER('T', 'L')
#define FORMAT_VIDEO            FORMAT_MARKER('V', '2')
#define FORMAT_RSA              FORMAT_MARKER('p', 'c')

// Unknown
#define FORMAT_UNKNOWN_AUDIO    FORMAT_MARKER('P', 0xc6)

// Audio types
#define FORMAT_FASTAUDIO        FORMAT_MARKER('A', '2')
#define FORMAT_FASTAUDIO_STEREO FORMAT_MARKER('A', '3')
#define FORMAT_PCM              FORMAT_MARKER('A', 'P')
#define FORMAT_ADPCM            FORMAT_MARKER('A', '8')
#define FORMAT_ADPCM_STEREO     FORMAT_MARKER('A', '9')
#define FORMAT_MULTITRACK       FORMAT_MARKER('A', 'M')
#define FORMAT_VORBIS           FORMAT_MARKER('A', 'V')

// Video metadata
#define FORMAT_KEYINDEX         FORMAT_MARKER('K', 'I')
#define FORMAT_POSSIBLY_CAPTIONS  FORMAT_MARKER('c', 'c')
#define FORMAT_HEADER_DONE      FORMAT_MARKER('H', 'E')

typedef struct MoDemuxContext {
    int handle_audio_packet;
    uint32_t audio_size;
    uint32_t unknown_size;
} MoDemuxContext;

static int mo_probe(const AVProbeData *p)
{
    if (AV_RB32(p->buf) != MO_TAG)
        return 0;
    if (AV_RB32(p->buf + 4) < 0x28) // Rough minimum size
        return 0;
    if (AV_RB16(p->buf + 8) != FORMAT_LENGTH) // Typically first
        return 0;
    return AVPROBE_SCORE_EXTENSION + 10;
}

static int mo_handle_audio(AVStream *ast, uint16_t marker, AVIOContext* pb) {
    ast->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    ast->codecpar->codec_tag = 0;
    
    uint32_t sample_rate = avio_rl32(pb);
    ast->codecpar->sample_rate = sample_rate;
    avpriv_set_pts_info(ast, 1, 64, ast->codecpar->sample_rate);

    // The container format also specifies a channel count.
    // However, we are not going to use it - the channel count should
    // always match stereo or mono, per format marker.
    avio_skip(pb, 4);

    switch(marker) {
    case FORMAT_FASTAUDIO:
        ast->codecpar->codec_id = AV_CODEC_ID_FASTAUDIO;
        ast->codecpar->ch_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_MONO;
        break;
    case FORMAT_FASTAUDIO_STEREO:
        ast->codecpar->codec_id = AV_CODEC_ID_FASTAUDIO;
        ast->codecpar->ch_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;
        break;
    case FORMAT_PCM:
        ast->codecpar->codec_id = AV_CODEC_ID_PCM_S16LE;
        ast->codecpar->ch_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;
        break;
    case FORMAT_ADPCM:
        ast->codecpar->codec_id = AV_CODEC_ID_ADPCM_IMA_MOFLEX;
        ast->codecpar->ch_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_MONO;
        break;
    case FORMAT_ADPCM_STEREO:
        ast->codecpar->codec_id = AV_CODEC_ID_ADPCM_IMA_MOFLEX;
        ast->codecpar->ch_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;
        break;
    default:
        // Unknown audio type.
        return -1;
    }

    return 0;
};

static int mo_read_header(AVFormatContext *s)
{
    AVIOContext *pb = s->pb;
    AVRational fps;

    // Wii Mobiclips must have audio and video.
    // Though the format appears to support an audioless type
    // on some platforms, the library for the Wii does not.
    AVStream *vst = avformat_new_stream(s, NULL);
    if (!vst)
        return AVERROR(ENOMEM);
    
    vst->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    vst->codecpar->codec_id   = AV_CODEC_ID_MOBICLIP;

    AVStream *ast = avformat_new_stream(s, NULL);
    if (!ast)
        return AVERROR(ENOMEM);

    avio_skip(pb, 4);
    // Add 8 to account for magic and header length
    uint32_t header_length = avio_rl32(pb) + 8;

    int has_read_header = 0;
    while (has_read_header == 0) {
        if (avio_tell(pb) > header_length) {
            // Exhausted header
            break;
        }

        uint16_t format_marker = avio_rl16(pb);
        av_log(s, AV_LOG_TRACE, "Handling '%c%c'...\n",
            (uint8_t)format_marker, (uint8_t)(format_marker >> 8));

        // Length in file is amount of u32s available within format segment.
        uint16_t format_length = avio_rl16(pb) * 4;
        if ((avio_tell(pb) + format_length) > header_length) {
            // Will exhaust header length
            break;
        }

        // Used for stream helper functions.
        int result;

        switch (format_marker) {
        case FORMAT_LENGTH:
            // 256.0 / fps for our time base
            // exact fps is that flipped, fps / 256.0
            fps.num = 256;
            fps.den = avio_rl32(pb);
            avpriv_set_pts_info(vst, 1, fps.num, fps.den);

            // TODO: can we use chunk count?
            unsigned int frame_count = avio_rl32(pb);
            vst->duration = frame_count;
            ast->duration = frame_count;

            // TODO: what is this?
            avio_skip(pb, 4);
            break;
        case FORMAT_VIDEO:
            // TODO: properly register video stream
            vst->codecpar->width  = avio_rl32(pb);
            vst->codecpar->height = avio_rl32(pb);
            break;
        case FORMAT_RSA:
            // We can not - and will not - handle validating RSA signatures.
            avio_skip(pb, format_length);
            break;
        case FORMAT_UNKNOWN_AUDIO:
            // TODO: Should we rightfully ignore this chunk?
            // This existing may imply a stereo track.
            avio_skip(pb, format_length);
            break;
        case FORMAT_FASTAUDIO:
        case FORMAT_FASTAUDIO_STEREO:
        case FORMAT_PCM:
        case FORMAT_ADPCM:
        case FORMAT_ADPCM_STEREO:
            result = mo_handle_audio(ast, format_marker, pb);
            if (result == -1) { // Unknown audio type
                return AVERROR_PATCHWELCOME;
            }
            break;
        case FORMAT_MULTITRACK:
            return AVERROR_PATCHWELCOME;
        case FORMAT_VORBIS:
            // TODO: This is something horrifying. Why?
            return AVERROR_PATCHWELCOME;
        case FORMAT_KEYINDEX:
            // TODO: We may need keyframe data as part of FFmpeg.
            avio_skip(pb, format_length);
            break;
        case FORMAT_HEADER_DONE:
            // We should be finished!
            has_read_header = 1;
            break;
        default:
            av_log(s, AV_LOG_INFO, "Encountered unknown chunk '%c%c' - ignoring.\n",
                (uint8_t)format_marker, (uint8_t)(format_marker >> 8));
            avio_skip(pb, format_length);
            break;
        }
    }

    if (!has_read_header) {
        return AVERROR_EOF;
    }

    return 0;
}

static int mo_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    MoDemuxContext *mo = s->priv_data;
    AVIOContext *pb = s->pb;
    int ret;

    // Determine whether audio or video.
    if (mo->handle_audio_packet) {
        // We now need to read the audio packet within this chunk.
        ret = av_get_packet(pb, pkt, mo->audio_size);
        if (ret < 0) {
            return ret;
        }

        // Skip over the following data.
        avio_skip(pb, mo->unknown_size);

        // Stream 1 is always audio.
        // TODO: adjust for multistream, if applicable
        pkt->stream_index = 1;
        mo->handle_audio_packet = 0;
    } else {
        // Dissect the current packet's header.
        uint32_t chunk_size = avio_rl32(pb);
        uint32_t video_size = avio_rl32(pb);
        uint32_t audio_size = chunk_size - video_size - 8;

        // TODO: what is this?
        uint32_t pos = avio_tell(pb) + video_size + audio_size;
        uint32_t unknown_size = (pos + 4 - (pos % 4)) - pos;

        mo->audio_size = audio_size;
        mo->unknown_size = unknown_size;

        ret = av_get_packet(pb, pkt, video_size);
        if (ret < 0) {
            return ret;
        }

        // Stream 0 is always video.
        pkt->stream_index = 0;
        mo->handle_audio_packet = 1;
    }

    if (avio_feof(pb))
        return AVERROR_EOF;

    return ret;
}

const FFInputFormat ff_mo_demuxer = {
    .p.name         = "mobiclip_mo",
    .p.long_name    = NULL_IF_CONFIG_SMALL("MobiClip MO"),
    .p.extensions   = "mo",
    .p.flags        = AVFMT_GENERIC_INDEX,
    .read_probe     = mo_probe,
    .read_header    = mo_read_header,
    .read_packet    = mo_read_packet,
    .priv_data_size = sizeof(MoDemuxContext),
};
