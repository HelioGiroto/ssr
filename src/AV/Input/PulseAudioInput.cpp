/*
Copyright (c) 2012-2013 Maarten Baert <maarten-baert@hotmail.com>

This file is part of SimpleScreenRecorder.

SimpleScreenRecorder is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

SimpleScreenRecorder is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with SimpleScreenRecorder.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "Global.h"
#include "PulseAudioInput.h"

#include "Logger.h"

// Artificial delay after the first samples have been received (in microseconds). Any samples received during this time will be dropped.
// This is needed because the first samples sometimes have weird timestamps, especially when PulseAudio is active
// (I've seen one situation where PulseAudio instantly 'captures' 2 seconds of silence when the recording is started).
// It also eliminates the clicking sound when the microphone is started for the first time.
const int64_t PulseAudioInput::START_DELAY = 100000;

static void PulseAudioIterate(pa_mainloop* mainloop) {
	if(pa_mainloop_prepare(mainloop, 1000) < 0) {
		Logger::LogError("[PulseAudioInput::Init] Error: Main loop prepare failed!");
		throw PulseAudioException();
	}
	if(pa_mainloop_poll(mainloop) < 0) {
		Logger::LogError("[PulseAudioInput::Init] Error: Main loop poll failed!");
		throw PulseAudioException();
	}
	if(pa_mainloop_dispatch(mainloop) < 0) {
		Logger::LogError("[PulseAudioInput::Init] Error: Main loop dispatch failed!");
		throw PulseAudioException();
	}
}

static void PulseAudioConnect(pa_mainloop** mainloop, pa_context** context) {

	// create PulseAudio main loop
	*mainloop = pa_mainloop_new();
	if(*mainloop == NULL) {
		Logger::LogError("[PulseAudioInput::Init] Error: Could not create main loop!");
		throw PulseAudioException();
	}

	// connect to PulseAudio
	*context = pa_context_new(pa_mainloop_get_api(*mainloop), "SimpleScreenRecorder");
	if(*context == NULL) {
		Logger::LogError("[PulseAudioInput::Init] Error: Could not create context!");
		throw PulseAudioException();
	}
	if(pa_context_connect(*context, NULL, PA_CONTEXT_NOAUTOSPAWN , NULL) < 0) {
		Logger::LogError(QString("[PulseAudioInput::Init] Error: Could not connect! Reason: ") + pa_strerror(pa_context_errno(*context)) + "\n"
						 "It is possible that your system doesn't use PulseAudio. Try using the ALSA backend instead.");
		throw PulseAudioException();
	}

	// wait until the connection is ready
	for( ; ; ) {
		PulseAudioIterate(*mainloop);
		pa_context_state_t state = pa_context_get_state(*context);
		if(state == PA_CONTEXT_READY)
			break;
		if(!PA_CONTEXT_IS_GOOD(state)) {
			Logger::LogError(QString("[PulseAudioInput::Init] Error: Connection attempt failed! Reason: ") + pa_strerror(pa_context_errno(*context)));
			throw PulseAudioException();
		}
	}

}

static void PulseAudioDisconnect(pa_mainloop** mainloop, pa_context** context) {
	if(*context != NULL) {
		pa_context_disconnect(*context);
		pa_context_unref(*context);
		*context = NULL;
	}
	if(*mainloop != NULL) {
		pa_mainloop_free(*mainloop);
		*mainloop = NULL;
	}
}

static void PulseAudioConnectStream(pa_mainloop* mainloop, pa_context* context, pa_stream** stream, const QString& source_name,
									unsigned int sample_rate, unsigned int channels, unsigned int period_size) {

	pa_sample_spec sample_spec;
	sample_spec.format = PA_SAMPLE_S16LE;
	sample_spec.rate = sample_rate;
	sample_spec.channels = channels;

	pa_buffer_attr buffer_attr;
	buffer_attr.fragsize = period_size * channels * 2;
	buffer_attr.maxlength = (uint32_t) -1;
	buffer_attr.minreq = (uint32_t) -1;
	buffer_attr.prebuf = (uint32_t) -1;
	buffer_attr.tlength = (uint32_t) -1;

	// create a stream
	*stream = pa_stream_new(context, "SimpleScreenRecorder Audio Input", &sample_spec, NULL);
	if(*stream == NULL) {
		Logger::LogError(QString("[PulseAudioInput::Init] Error: Could not create stream! Reason: ") + pa_strerror(pa_context_errno(context)));
		throw PulseAudioException();
	}

	// connect the stream
	if(pa_stream_connect_record(*stream, qPrintable(source_name), &buffer_attr,
								(pa_stream_flags_t) (PA_STREAM_INTERPOLATE_TIMING | PA_STREAM_AUTO_TIMING_UPDATE | PA_STREAM_ADJUST_LATENCY)) < 0) {
		Logger::LogError(QString("[PulseAudioInput::Init] Error: Could not connect stream! Reason: ") + pa_strerror(pa_context_errno(context)));
		throw PulseAudioException();
	}

	// wait until the stream is ready
	for( ; ; ) {
		PulseAudioIterate(mainloop);
		pa_stream_state_t state = pa_stream_get_state(*stream);
		if(state == PA_STREAM_READY)
			break;
		if(!PA_STREAM_IS_GOOD(state)) {
			Logger::LogError(QString("[PulseAudioInput::Init] Error: Stream connection attempt failed! Reason: ") + pa_strerror(pa_context_errno(context)));
			throw PulseAudioException();
		}
	}

}

static void PulseAudioDisconnectStream(pa_stream** stream) {
	if(*stream != NULL) {
		pa_stream_unref(*stream);
		*stream = NULL;
	}
}

static void PulseAudioCompleteOperation(pa_mainloop* mainloop, pa_operation** operation) {
	if(operation == NULL)
		return;

	// wait until the operation is done
	for( ; ; ) {
		PulseAudioIterate(mainloop);
		pa_operation_state_t state = pa_operation_get_state(*operation);
		if(state == PA_OPERATION_DONE || state == PA_OPERATION_CANCELLED)
			break;
	}

	// delete it
	pa_operation_unref(*operation);
	operation = NULL;

}

static void PulseAudioCancelOperation(pa_mainloop* mainloop, pa_operation** operation) {
	if(operation == NULL)
		return;

	// cancel it
	pa_operation_cancel(*operation);

	// wait until the operation is done
	for( ; ; ) {
		PulseAudioIterate(mainloop);
		pa_operation_state_t state = pa_operation_get_state(*operation);
		if(state == PA_OPERATION_DONE || state == PA_OPERATION_CANCELLED)
			break;
	}

	// delete it
	pa_operation_unref(*operation);
	operation = NULL;

}

PulseAudioInput::PulseAudioInput(const QString& source_name, unsigned int sample_rate) {

	m_source_name = source_name;
	m_sample_rate = sample_rate;
	m_channels = 2; // always 2 channels because the synchronizer and encoder don't support anything else at this point

	m_pa_mainloop = NULL;
	m_pa_context = NULL;
	m_pa_stream = NULL;
	m_pa_period_size = 1024; // number of samples per period

	try {
		Init();
	} catch(...) {
		Free();
		throw;
	}

}

PulseAudioInput::~PulseAudioInput() {

	// tell the thread to stop
	if(m_thread.joinable()) {
		Logger::LogInfo("[PulseAudioInput::~PulseAudioInput] Telling input thread to stop ...");
		m_should_stop = true;
		m_thread.join();
	}

	// free everything
	Free();

}

static void SourceNamesCallback(pa_context* context, const pa_source_info* info, int eol, void* userdata) {
	Q_UNUSED(context);
	if(!eol) {
		std::vector<PulseAudioInput::Source> &list = *((std::vector<PulseAudioInput::Source>*) userdata);
		PulseAudioInput::Source source;
		source.name = info->name;
		source.description = info->description;
		list.push_back(source);
	}
}

std::vector<PulseAudioInput::Source> PulseAudioInput::GetSourceList() {
	std::vector<Source> list;

	pa_mainloop *mainloop = NULL;
	pa_context *context = NULL;
	pa_operation *operation = NULL;

	try {

		PulseAudioConnect(&mainloop, &context);

		operation = pa_context_get_source_info_list(context, SourceNamesCallback, &list);
		if(operation == NULL) {
			Logger::LogError(QString("[PulseAudioInput::Init] Error: Could not get names of sources! Reason: ") + pa_strerror(pa_context_errno(context)));
			throw PulseAudioException();
		}
		PulseAudioCompleteOperation(mainloop, &operation);

		PulseAudioDisconnect(&mainloop, &context);

	} catch(...) {
		PulseAudioCancelOperation(mainloop, &operation);
		PulseAudioDisconnect(&mainloop, &context);
	}

	return list;
}

void PulseAudioInput::Init() {

	PulseAudioConnect(&m_pa_mainloop, &m_pa_context);
	PulseAudioConnectStream(m_pa_mainloop, m_pa_context, &m_pa_stream, m_source_name,
							m_sample_rate, m_channels, m_pa_period_size);

	// start input thread
	m_should_stop = false;
	m_error_occurred = false;
	m_thread = std::thread(&PulseAudioInput::InputThread, this);

}

void PulseAudioInput::Free() {
	PulseAudioDisconnectStream(&m_pa_stream);
	PulseAudioDisconnect(&m_pa_mainloop, &m_pa_context);
}

void PulseAudioInput::InputThread() {
	try {

		Logger::LogInfo("[PulseAudioInput::run] Input thread started.");

		std::vector<uint8_t> buffer;
		bool has_first_samples = false;
		int64_t first_timestamp = 0; // value won't be used, but GCC gives a warning otherwise

		while(!m_should_stop) {

			PulseAudioIterate(m_pa_mainloop);

			const void *data;
			size_t bytes;
			if(pa_stream_peek(m_pa_stream, &data, &bytes) < 0) {
				Logger::LogError("[PulseAudioInput::InputThread] Error: Stream peek failed!");
				throw PulseAudioException();
			}
			if(data == NULL) {
				if(bytes > 0) {
					// skip hole
					PushAudioHole();
					pa_stream_drop(m_pa_stream);
				}
				continue;
			}

			// deal with half samples from the last peek
			unsigned int samples = (buffer.size() + bytes) / (m_channels * 2);
			unsigned int bytes_left = (buffer.size() + bytes) % (m_channels * 2);
			uint8_t *push_data;
			if(buffer.size() > 0) {
				size_t p = buffer.size();
				buffer.resize(p + bytes - bytes_left);
				memcpy(buffer.data() + p, data, bytes - bytes_left);
				push_data = buffer.data();
			} else {
				push_data = (uint8_t*) data;
			}

			int64_t timestamp = hrt_time_micro();

			// skip the first samples
			if(has_first_samples) {
				if(timestamp > first_timestamp + START_DELAY) {

					// get the latency
					// The latency can be negative for monitors, this means that we got the samples before they were actually played.
					// But for some reason, PulseAudio doesn't like signed integers ...
					pa_usec_t latency_magnitude;
					int latency_negative;
					pa_stream_get_latency(m_pa_stream, &latency_magnitude, &latency_negative);
					int64_t latency = (latency_negative)? -(int64_t) latency_magnitude : latency_magnitude;

					// send the samples to the synchronizer
					int64_t time = timestamp - latency;
					PushAudioSamples(m_sample_rate, m_channels, samples, push_data, AV_SAMPLE_FMT_S16, time);

				}
			} else {
				has_first_samples = true;
				first_timestamp = timestamp;
			}

			// store remaining bytes
			buffer.clear();
			if(bytes_left > 0) {
				buffer.resize(bytes_left);
				memcpy(buffer.data(), (uint8_t*) data + bytes - bytes_left, bytes_left);
			}

			// drop samples
			pa_stream_drop(m_pa_stream);

		}

		Logger::LogInfo("[PulseAudioInput::run] Input thread stopped.");

	} catch(const std::exception& e) {
		m_error_occurred = true;
		Logger::LogError(QString("[PulseAudioInput::run] Exception '") + e.what() + "' in input thread.");
	} catch(...) {
		m_error_occurred = true;
		Logger::LogError("[PulseAudioInput::run] Unknown exception in input thread.");
	}
}
