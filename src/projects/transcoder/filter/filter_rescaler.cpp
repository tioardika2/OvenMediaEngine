//==============================================================================
//
//  Transcode
//
//  Created by Kwon Keuk Han
//  Copyright (c) 2018 AirenSoft. All rights reserved.
//
//==============================================================================

#include "filter_rescaler.h"

#include <base/ovlibrary/ovlibrary.h>

#include "../transcoder_gpu.h"
#include "../transcoder_private.h"

#define MAX_QUEUE_SIZE 500

FilterRescaler::FilterRescaler()
{
	_frame = ::av_frame_alloc();

	_outputs = ::avfilter_inout_alloc();
	_inputs = ::avfilter_inout_alloc();

	_input_buffer.SetThreshold(MAX_QUEUE_SIZE);

	OV_ASSERT2(_frame != nullptr);
	OV_ASSERT2(_inputs != nullptr);
	OV_ASSERT2(_outputs != nullptr);
}

FilterRescaler::~FilterRescaler()
{
	Stop();

	OV_SAFE_FUNC(_frame, nullptr, ::av_frame_free, &);

	OV_SAFE_FUNC(_inputs, nullptr, ::avfilter_inout_free, &);
	OV_SAFE_FUNC(_outputs, nullptr, ::avfilter_inout_free, &);

	OV_SAFE_FUNC(_filter_graph, nullptr, ::avfilter_graph_free, &);

	_input_buffer.Clear();
}

bool FilterRescaler::Configure(const std::shared_ptr<MediaTrack> &input_track, const std::shared_ptr<MediaTrack> &output_track)
{
	_input_track = input_track;
	_output_track = output_track;

	const AVFilter *buffersrc = ::avfilter_get_by_name("buffer");
	const AVFilter *buffersink = ::avfilter_get_by_name("buffersink");
	int ret;
	_filter_graph = ::avfilter_graph_alloc();

	if ((_filter_graph == nullptr) || (_inputs == nullptr) || (_outputs == nullptr))
	{
		logte("Could not allocate variables for filter graph: %p, %p, %p", _filter_graph, _inputs, _outputs);
		return false;
	}

	// Limit the number of filter threads to 4. I think 4 thread is usually enough for video filtering processing.
	_filter_graph->nb_threads = 4;

	AVRational input_timebase = ffmpeg::Conv::TimebaseToAVRational(input_track->GetTimeBase());
	AVRational output_timebase = ffmpeg::Conv::TimebaseToAVRational(output_track->GetTimeBase());

	_scale = ::av_q2d(::av_div_q(input_timebase, output_timebase));

	if (::isnan(_scale))
	{
		logte("Invalid timebase: input: %d/%d, output: %d/%d",
			  input_timebase.num, input_timebase.den,
			  output_timebase.num, output_timebase.den);

		return false;
	}

	// Prepare filters
	//
	// Filter graph:
	//     [buffer] -> [fps] -> [scale] -> [settb] -> [buffersink]

	// Prepare the input parameters
	std::vector<ov::String> src_params = {
		ov::String::FormatString("video_size=%dx%d", input_track->GetWidth(), input_track->GetHeight()),
		ov::String::FormatString("pix_fmt=%d", input_track->GetColorspace()),
		ov::String::FormatString("time_base=%s", input_track->GetTimeBase().GetStringExpr().CStr()),
		ov::String::FormatString("pixel_aspect=%d/%d", 1, 1)};


	ov::String src_args = ov::String::Join(src_params, ":");

	ret = ::avfilter_graph_create_filter(&_buffersrc_ctx, buffersrc, "in", src_args, nullptr, _filter_graph);
	if (ret < 0)
	{
		logte("Could not create video buffer source filter for rescaling: %d", ret);
		return false;
	}

	// Prepare output filters
	ret = ::avfilter_graph_create_filter(&_buffersink_ctx, buffersink, "out", nullptr, nullptr, _filter_graph);
	if (ret < 0)
	{
		logte("Could not create video buffer sink filter for rescaling: %d", ret);
		return false;
	}

	enum AVPixelFormat pix_fmts[] = {(AVPixelFormat)output_track->GetColorspace(), AV_PIX_FMT_NONE};
	ret = av_opt_set_int_list(_buffersink_ctx, "pix_fmts", pix_fmts, AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN);
	if (ret < 0)
	{
		logte("Could not set output pixel format for rescaling: %d", ret);
		return false;
	}

	std::vector<ov::String> filters;

	if (output_track->GetFrameRateByConfig() > 0.0f)
	{
		filters.push_back(ov::String::FormatString("fps=fps=%.2f:round=near", output_track->GetFrameRateByConfig()));
	}

	if (output_track->GetHardwareAccel() == true &&
		TranscodeGPU::GetInstance()->IsSupportedNV() == true &&
		input_track->GetColorspace() == AV_PIX_FMT_NV12 &&
		output_track->GetColorspace() == AV_PIX_FMT_NV12)
	{
		filters.push_back(ov::String::FormatString("hwupload_cuda,scale_cuda=%d:%d,hwdownload",
												   output_track->GetWidth(), output_track->GetHeight()));
	}
	else
	{
		filters.push_back(ov::String::FormatString("scale=%dx%d:flags=bilinear",
												   output_track->GetWidth(), output_track->GetHeight()));
	}

	filters.push_back(ov::String::FormatString("settb=%s", output_track->GetTimeBase().GetStringExpr().CStr()));

	ov::String output_filters = ov::String::Join(filters, ",");

	_outputs->name = ::av_strdup("in");
	_outputs->filter_ctx = _buffersrc_ctx;
	_outputs->pad_idx = 0;
	_outputs->next = nullptr;

	_inputs->name = ::av_strdup("out");
	_inputs->filter_ctx = _buffersink_ctx;
	_inputs->pad_idx = 0;
	_inputs->next = nullptr;

	if ((ret = ::avfilter_graph_parse_ptr(_filter_graph, output_filters, &_inputs, &_outputs, nullptr)) < 0)
	{
		logte("Could not parse filter string for rescaling: %d (%s)", ret, output_filters.CStr());
		return false;
	}

	if ((ret = ::avfilter_graph_config(_filter_graph, nullptr)) < 0)
	{
		logte("Could not validate filter graph for rescaling: %d", ret);
		return false;
	}

	logti("Rescaler is enabled for track #%u using parameters. input: %s / outputs: %s", input_track->GetId(), src_args.CStr(), output_filters.CStr());

	_input_width = input_track->GetWidth();
	_input_height = input_track->GetHeight();


	return true;
}

int32_t FilterRescaler::SendBuffer(std::shared_ptr<MediaFrame> buffer)
{
	_input_buffer.Enqueue(std::move(buffer));

	return 0;
}

bool FilterRescaler::Start()
{
	// Generates a thread that reads and encodes frames in the input_buffer queue and places them in the output queue.
	try
	{
		_kill_flag = false;

		_thread_work = std::thread(&FilterRescaler::FilterThread, this);
		pthread_setname_np(_thread_work.native_handle(), "Rescaler");
	}
	catch (const std::system_error &e)
	{
		_kill_flag = true;

		logte("Failed to start rescaling filter thread");
		return false;
	}

	return true;
}

void FilterRescaler::Stop()
{
	_kill_flag = true;

	_input_buffer.Stop();

	if (_thread_work.joinable())
	{
		_thread_work.join();
		logtd("rescaling filter thread has ended");
	}
}

void FilterRescaler::FilterThread()
{
	logtd("Start rescaling filter thread");

	while (!_kill_flag)
	{
		auto obj = _input_buffer.Dequeue();
		if (obj.has_value() == false)
			continue;

		auto media_frame = std::move(obj.value());

		auto av_frame = ffmpeg::Conv::ToAVFrame(cmn::MediaType::Video, media_frame);
		if (!av_frame)
		{
			logte("Could not allocate the video frame data");
			break;
		}

		int ret = ::av_buffersrc_add_frame_flags(_buffersrc_ctx, av_frame, AV_BUFFERSRC_FLAG_KEEP_REF);
		if (ret < 0)
		{
			logte("An error occurred while feeding the audio filtergraph: format: %d, pts: %lld, linesize: %d, size: %d", _frame->format, _frame->pts, _frame->linesize[0], _input_buffer.Size());

			continue;
		}

		while (true)
		{
			int ret = ::av_buffersink_get_frame(_buffersink_ctx, _frame);
			if (ret == AVERROR(EAGAIN))
			{
				// Need more data
				break;
			}
			else if (ret == AVERROR_EOF)
			{
				logte("End of file error(%d)", ret);
				break;
			}
			else if (ret < 0)
			{
				logte("Unknown error is occurred while get frame. error(%d)", ret);
				break;
			}
			else
			{
				_frame->pict_type = AV_PICTURE_TYPE_NONE;
				auto output_frame = ffmpeg::Conv::ToMediaFrame(cmn::MediaType::Video, _frame);
				::av_frame_unref(_frame);
				if (output_frame == nullptr)
				{
					continue;
				}

				if (_complete_handler != nullptr && _kill_flag == false)
				{
					_complete_handler(std::move(output_frame));
				}
			}
		}
	}
}
