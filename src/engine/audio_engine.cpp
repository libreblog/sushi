/*
 * Copyright 2017-2019 Modern Ancient Instruments Networked AB, dba Elk
 *
 * SUSHI is free software: you can redistribute it and/or modify it under the terms of
 * the GNU Affero General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * SUSHI is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License along with
 * SUSHI.  If not, see http://www.gnu.org/licenses/
 */

/**
 * @brief Real time audio processing engine
 * @copyright 2017-2019 Modern Ancient Instruments Networked AB, dba Elk, Stockholm
 */

#include <fstream>
#include <iomanip>
#include <functional>

#include "twine/src/twine_internal.h"

#include "audio_engine.h"
#include "logging.h"
#include "plugins/passthrough_plugin.h"
#include "plugins/gain_plugin.h"
#include "plugins/lfo_plugin.h"
#include "plugins/equalizer_plugin.h"
#include "plugins/arpeggiator_plugin.h"
#include "plugins/sample_player_plugin.h"
#include "plugins/peak_meter_plugin.h"
#include "plugins/transposer_plugin.h"
#include "plugins/step_sequencer_plugin.h"
#include "plugins/cv_to_control_plugin.h"
#include "plugins/control_to_cv_plugin.h"
#include "library/vst2x_wrapper.h"
#include "library/vst3x_wrapper.h"
#include "library/lv2/lv2_wrapper.h"

namespace sushi {
namespace engine {

constexpr auto RT_EVENT_TIMEOUT = std::chrono::milliseconds(200);
constexpr char TIMING_FILE_NAME[] = "timings.txt";
constexpr auto CLIPPING_DETECTION_INTERVAL = std::chrono::milliseconds(500);

SUSHI_GET_LOGGER_WITH_MODULE_NAME("engine");


void ClipDetector::set_sample_rate(float samplerate)
{
    _interval = samplerate * CLIPPING_DETECTION_INTERVAL.count() / 1000 - AUDIO_CHUNK_SIZE;
}

void ClipDetector::set_input_channels(int channels)
{
    _input_clip_count = std::vector<unsigned int>(channels, _interval);
}

void ClipDetector::set_output_channels(int channels)
{
    _output_clip_count = std::vector<unsigned int>(channels, _interval);
}

void ClipDetector::detect_clipped_samples(const ChunkSampleBuffer& buffer, RtSafeRtEventFifo& queue, bool audio_input)
{
    auto& counter = audio_input? _input_clip_count : _output_clip_count;
    for (int i = 0; i < buffer.channel_count(); ++i)
    {
        if (buffer.count_clipped_samples(i, 1) > 0 && counter[i] >= _interval)
        {
            queue.push(RtEvent::make_clip_notification_event(0, i, audio_input? ClipNotificationRtEvent::ClipChannelType::INPUT:
                                                                   ClipNotificationRtEvent::ClipChannelType::OUTPUT));
            counter[i] = 0;
        }
        else
        {
            counter[i] += AUDIO_CHUNK_SIZE;
        }
    }
}

AudioEngine::AudioEngine(float sample_rate, int rt_cpu_cores) : BaseEngine::BaseEngine(sample_rate),
                                                                _multicore_processing(rt_cpu_cores > 1),
                                                                _rt_cores(rt_cpu_cores),
                                                                _transport(sample_rate),
                                                                _clip_detector(sample_rate)
{
    this->set_sample_rate(sample_rate);
    _event_dispatcher.run();
    if (_multicore_processing)
    {
        _worker_pool = twine::WorkerPool::create_worker_pool(_rt_cores);
    }
}

AudioEngine::~AudioEngine()
{
    _event_dispatcher.stop();
    if (_process_timer.enabled())
    {
        _process_timer.enable(false);
        print_timings_to_file(TIMING_FILE_NAME);
    }
}

void AudioEngine::set_sample_rate(float sample_rate)
{
    BaseEngine::set_sample_rate(sample_rate);
    for (auto& node : _processors)
    {
        node.second->configure(sample_rate);
    }
    _transport.set_sample_rate(sample_rate);
    _process_timer.set_timing_period(sample_rate, AUDIO_CHUNK_SIZE);
    _clip_detector.set_sample_rate(sample_rate);
}

void AudioEngine::set_audio_input_channels(int channels)
{
    _clip_detector.set_input_channels(channels);
    BaseEngine::set_audio_input_channels(channels);
}

void AudioEngine::set_audio_output_channels(int channels)
{
    _clip_detector.set_output_channels(channels);
    BaseEngine::set_audio_output_channels(channels);
}

EngineReturnStatus AudioEngine::set_cv_input_channels(int channels)
{
    if (channels > MAX_ENGINE_CV_IO_PORTS)
    {
        return EngineReturnStatus::INVALID_N_CHANNELS;
    }
    return BaseEngine::set_cv_input_channels(channels);
}

EngineReturnStatus AudioEngine::set_cv_output_channels(int channels)
{
    if (channels > MAX_ENGINE_CV_IO_PORTS)
    {
        return EngineReturnStatus::INVALID_N_CHANNELS;
    }
    return BaseEngine::set_cv_output_channels(channels);
}

EngineReturnStatus AudioEngine::connect_audio_input_channel(int input_channel, int track_channel, const std::string& track_name)
{
    auto processor_node = _processors.find(track_name);
    if(processor_node == _processors.end())
    {
        return EngineReturnStatus::INVALID_TRACK;
    }
    auto track = static_cast<Track*>(processor_node->second.get());
    if (input_channel >= _audio_inputs || track_channel >= track->input_channels())
    {
        return EngineReturnStatus::INVALID_CHANNEL;
    }
    AudioConnection con = {input_channel, track_channel, track->id()};
    _in_audio_connections.push_back(con);
    SUSHI_LOG_INFO("Connected inputs {} to channel {} of track \"{}\"", input_channel, track_channel, track_name);
    return EngineReturnStatus::OK;
}

EngineReturnStatus AudioEngine::connect_audio_output_channel(int output_channel, int track_channel,
                                                             const std::string& track_name)
{
    auto processor_node = _processors.find(track_name);
    if(processor_node == _processors.end())
    {
        return EngineReturnStatus::INVALID_TRACK;
    }
    auto track = static_cast<Track*>(processor_node->second.get());
    if (output_channel >= _audio_outputs || track_channel >= track->output_channels())
    {
        if (track_channel > track->max_output_channels())
        {
            return EngineReturnStatus::INVALID_CHANNEL;
        }
        track->set_output_channels(track_channel + 1);
    }
    AudioConnection con = {output_channel, track_channel, track->id()};
    _out_audio_connections.push_back(con);
    SUSHI_LOG_INFO("Connected channel {} of track \"{}\" to output {}", track_channel, track_name, output_channel);
    return EngineReturnStatus::OK;
}

EngineReturnStatus AudioEngine::connect_audio_input_bus(int input_bus, int track_bus, const std::string& track_name)
{
    auto status = connect_audio_input_channel(input_bus * 2, track_bus * 2, track_name);
    if (status != EngineReturnStatus::OK)
    {
        return status;
    }
    return connect_audio_input_channel(input_bus * 2 + 1, track_bus * 2 + 1, track_name);
}

EngineReturnStatus AudioEngine::connect_audio_output_bus(int output_bus, int track_bus, const std::string& track_name)
{
    auto status = connect_audio_output_channel(output_bus * 2, track_bus * 2, track_name);
    if (status != EngineReturnStatus::OK)
    {
        return status;
    }
    return connect_audio_output_channel(output_bus * 2 + 1, track_bus * 2 + 1, track_name);
}

EngineReturnStatus AudioEngine::connect_cv_to_parameter(const std::string& processor_name,
                                                        const std::string& parameter_name,
                                                        int cv_input_id)
{
    if (cv_input_id >= _cv_inputs)
    {
        return EngineReturnStatus::INVALID_CHANNEL;
    }
    auto processor_node = _processors.find(processor_name);
    if(processor_node == _processors.end())
    {
        return EngineReturnStatus::INVALID_PROCESSOR;
    }
    auto param = processor_node->second->parameter_from_name(parameter_name);
    if (param == nullptr)
    {
        return EngineReturnStatus::INVALID_PARAMETER;
    }
    CvConnection con;
    con.processor_id = processor_node->second->id();
    con.parameter_id = param->id();
    con.cv_id = cv_input_id;
    _cv_in_routes.push_back(con);
    SUSHI_LOG_INFO("Connected cv input {} to parameter {} on {}", cv_input_id, parameter_name, processor_name);
    return EngineReturnStatus::OK;
}

EngineReturnStatus AudioEngine::connect_cv_from_parameter(const std::string& processor_name,
                                                          const std::string& parameter_name,
                                                          int cv_output_id)
{
    if (cv_output_id >= _cv_outputs)
    {
        return EngineReturnStatus::ERROR;
    }
    auto processor_node = _processors.find(processor_name);
    if (processor_node == _processors.end())
    {
        return EngineReturnStatus::INVALID_PROCESSOR;
    }
    auto param = processor_node->second->parameter_from_name(parameter_name);
    if (param == nullptr)
    {
        return EngineReturnStatus::INVALID_PARAMETER;
    }
    auto res = processor_node->second->connect_cv_from_parameter(param->id(), cv_output_id);
    if (res != ProcessorReturnCode::OK)
    {
        return EngineReturnStatus::ERROR;
    }
    SUSHI_LOG_INFO("Connected parameter {} on {} to cv output {}", parameter_name, processor_name, cv_output_id);
    return EngineReturnStatus::OK;
}

EngineReturnStatus AudioEngine::connect_gate_to_processor(const std::string& processor_name,
                                                          int gate_input_id,
                                                          int note_no,
                                                          int channel)
{
    if (gate_input_id >= MAX_ENGINE_GATE_PORTS || note_no > MAX_ENGINE_GATE_NOTE_NO)
    {
        return EngineReturnStatus::ERROR;
    }
    auto processor_node = _processors.find(processor_name);
    if(processor_node == _processors.end())
    {
        return EngineReturnStatus::INVALID_PROCESSOR;
    }
    GateConnection con;
    con.processor_id = processor_node->second->id();
    con.note_no = note_no;
    con.channel = channel;
    con.gate_id = gate_input_id;
    _gate_in_routes.push_back(con);
    SUSHI_LOG_INFO("Connected gate input {} to processor {} on channel {}", gate_input_id, processor_name, channel);
    return EngineReturnStatus::OK;
}

EngineReturnStatus AudioEngine::connect_gate_from_processor(const std::string& processor_name,
                                                            int gate_output_id,
                                                            int note_no,
                                                            int channel)
{
    if (gate_output_id >= MAX_ENGINE_GATE_PORTS || note_no > MAX_ENGINE_GATE_NOTE_NO)
    {
        return EngineReturnStatus::ERROR;
    }
    auto processor_node = _processors.find(processor_name);
    if(processor_node == _processors.end())
    {
        return EngineReturnStatus::INVALID_PROCESSOR;
    }
    auto res = processor_node->second->connect_gate_from_processor(gate_output_id, channel, note_no);
    if (res != ProcessorReturnCode::OK)
    {
        return EngineReturnStatus::ERROR;
    }
    SUSHI_LOG_INFO("Connected processor {} to gate output {} from channel {}", gate_output_id, processor_name, channel);
    return EngineReturnStatus::OK;
}

EngineReturnStatus AudioEngine::connect_gate_to_sync(int /*gate_input_id*/, int /*ppq_ticks*/)
{
    // TODO -  Needs implementing
    return EngineReturnStatus::OK;
}

EngineReturnStatus AudioEngine::connect_sync_to_gate(int /*gate_output_id*/, int /*ppq_ticks*/)
{
    // TODO -  Needs implementing
    return EngineReturnStatus::OK;
}

bool AudioEngine::realtime()
{
    return _state.load() != RealtimeState::STOPPED;
}

void AudioEngine::enable_realtime(bool enabled)
{
    if (enabled)
    {
        _state.store(RealtimeState::STARTING);
    }
    else
    {
        if (realtime())
        {
            auto event = RtEvent::make_stop_engine_event();
            send_async_event(event);
        } else
        {
            _state.store(RealtimeState::STOPPED);
        }
    }
};

int AudioEngine::n_channels_in_track(int track)
{
    if (track <= static_cast<int>(_audio_graph.size()))
    {
        return _audio_graph[track]->input_channels();
    }
    return 0;
}

Processor* AudioEngine::_make_internal_plugin(const std::string& uid)
{
    Processor* instance = nullptr;
    if (uid == "sushi.testing.passthrough")
    {
        instance = new passthrough_plugin::PassthroughPlugin(_host_control);
    }
    else if (uid == "sushi.testing.gain")
    {
        instance = new gain_plugin::GainPlugin(_host_control);
    }
    else if (uid == "sushi.testing.lfo")
    {
        instance = new lfo_plugin::LfoPlugin(_host_control);
    }
    else if (uid == "sushi.testing.equalizer")
    {
        instance = new equalizer_plugin::EqualizerPlugin(_host_control);
    }
    else if (uid == "sushi.testing.sampleplayer")
    {
        instance = new sample_player_plugin::SamplePlayerPlugin(_host_control);
    }
    else if (uid == "sushi.testing.arpeggiator")
    {
        instance = new arpeggiator_plugin::ArpeggiatorPlugin(_host_control);
    }
    else if (uid == "sushi.testing.peakmeter")
    {
        instance = new peak_meter_plugin::PeakMeterPlugin(_host_control);
    }
    else if (uid == "sushi.testing.transposer")
    {
        instance = new transposer_plugin::TransposerPlugin(_host_control);
    }
    else if (uid == "sushi.testing.step_sequencer")
    {
        instance = new step_sequencer_plugin::StepSequencerPlugin(_host_control);
    }
    else if (uid == "sushi.testing.cv_to_control")
    {
        instance = new cv_to_control_plugin::CvToControlPlugin(_host_control);
    }
    else if (uid == "sushi.testing.control_to_cv")
    {
        instance = new control_to_cv_plugin::ControlToCvPlugin(_host_control);
    }
    return instance;
}

EngineReturnStatus AudioEngine::_register_processor(Processor* processor, const std::string& name)
{
    if(name.empty())
    {
        SUSHI_LOG_ERROR("Plugin name is not specified");
        return EngineReturnStatus::INVALID_PLUGIN_NAME;
    }
    if(_processor_exists(name))
    {
        SUSHI_LOG_WARNING("Processor with this name already exists");
        return EngineReturnStatus::INVALID_PROCESSOR;
    }
    processor->set_name(name);
    _processors[name] = std::move(std::unique_ptr<Processor>(processor));
    SUSHI_LOG_DEBUG("Succesfully registered processor {}.", name);
    return EngineReturnStatus::OK;
}

EngineReturnStatus AudioEngine::_deregister_processor(const std::string &name)
{
    auto processor_node = _processors.find(name);
    if (processor_node == _processors.end())
    {
        return EngineReturnStatus::INVALID_PLUGIN_NAME;
    }
    _processors.erase(processor_node);
    return EngineReturnStatus::OK;
}

bool AudioEngine::_processor_exists(const std::string& processor_name)
{
    auto processor_node = _processors.find(processor_name);
    if(processor_node == _processors.end())
    {
        return false;
    }
    return true;
}

bool AudioEngine::_processor_exists(const ObjectId uid)
{
    if(uid >= _realtime_processors.size() || _realtime_processors[uid] == nullptr)
    {
        return false;
    }
    return true;
}

bool AudioEngine::_insert_processor_in_realtime_part(Processor* processor)
{
    if (processor->id() > _realtime_processors.size())
    {
        /* TODO - When non-rt callbacks for events are ready we can have the
         * rt processor list re-allocated outside the rt domain
         * In the meantime, put a limit on the number of processors */
        SUSHI_LOG_ERROR("Realtime processor list full");
        assert(false);
    }
    if(_realtime_processors[processor->id()] != nullptr)
    {
        return false;
    }
    _realtime_processors[processor->id()] = processor;
    return true;
}

bool AudioEngine::_remove_processor_from_realtime_part(ObjectId processor)
{
    if(_realtime_processors[processor] == nullptr)
    {
        return false;
    }
    _realtime_processors[processor] = nullptr;
    return true;
}

void AudioEngine::process_chunk(SampleBuffer<AUDIO_CHUNK_SIZE>* in_buffer,
                                SampleBuffer<AUDIO_CHUNK_SIZE>* out_buffer,
                                ControlBuffer* in_controls,
                                ControlBuffer* out_controls,
                                Time timestamp,
                                int64_t samplecount)
{
    /* Signal that this is a realtime audio processing thread */
    twine::ThreadRtFlag rt_flag;

    auto engine_timestamp = _process_timer.start_timer();

    _transport.set_time(timestamp, samplecount);

    RtEvent in_event;
    while (_internal_control_queue.pop(in_event))
    {
        send_rt_event(in_event);
    }
    while (_main_in_queue.pop(in_event))
    {
        send_rt_event(in_event);
    }

    if (_cv_inputs > 0)
    {
        _route_cv_gate_ins(*in_controls);
    }

    _event_dispatcher.set_time(_transport.current_process_time());
    auto state = _state.load();

    if (_input_clip_detection_enabled)
    {
        _clip_detector.detect_clipped_samples(*in_buffer, _main_out_queue, true);
    }
    _copy_audio_to_tracks(in_buffer);

    if (_multicore_processing)
    {
        _worker_pool->wakeup_workers();
        _retrieve_events_from_tracks(*out_controls);
    }
    else
    {
        for (auto& track : _audio_graph)
        {
            track->render();
        }
        _process_outgoing_events(*out_controls, _processor_out_queue);
    }

    if (_multicore_processing)
    {
        _worker_pool->wait_for_workers_idle();
    }

    _main_out_queue.push(RtEvent::make_synchronisation_event(_transport.current_process_time()));
    _copy_audio_from_tracks(out_buffer);
    _state.store(update_state(state));

    if (_output_clip_detection_enabled)
    {
        _clip_detector.detect_clipped_samples(*out_buffer, _main_out_queue, false);
    }
    _process_timer.stop_timer(engine_timestamp, ENGINE_TIMING_ID);
}

void AudioEngine::set_tempo(float tempo)
{
    bool realtime_running = _state != RealtimeState::STOPPED;
    _transport.set_tempo(tempo, realtime_running);
    if (realtime_running)
    {
        auto e = RtEvent::make_tempo_event(0, tempo);
        send_async_event(e);
    }
}

void AudioEngine::set_time_signature(TimeSignature signature)
{
    bool realtime_running = _state != RealtimeState::STOPPED;
    _transport.set_time_signature(signature, realtime_running);
    if (realtime_running)
    {
        auto e = RtEvent::make_time_signature_event(0, signature);
        send_async_event(e);
    }
}

void AudioEngine::set_transport_mode(PlayingMode mode)
{
    bool realtime_running = _state != RealtimeState::STOPPED;
    _transport.set_playing_mode(mode, realtime_running);
    if (realtime_running)
    {
        auto e = RtEvent::make_playing_mode_event(0, mode);
        send_async_event(e);
    }
}

void AudioEngine::set_tempo_sync_mode(SyncMode mode)
{
    bool realtime_running = _state != RealtimeState::STOPPED;
    _transport.set_sync_mode(mode, realtime_running);
    if (realtime_running)
    {
        auto e = RtEvent::make_sync_mode_event(0, mode);
        send_async_event(e);
    }
}

EngineReturnStatus AudioEngine::send_rt_event(RtEvent& event)
{
    if (_handle_internal_events(event))
    {
        return EngineReturnStatus::OK;
    }
    if (event.processor_id() > _realtime_processors.size())
    {
        SUSHI_LOG_WARNING("Invalid processor id {}.", event.processor_id());
        return EngineReturnStatus::INVALID_PROCESSOR;
    }
    auto processor_node = _realtime_processors[event.processor_id()];
    if (processor_node == nullptr)
    {
        SUSHI_LOG_WARNING("Invalid processor id {}.", event.processor_id());
        return EngineReturnStatus::INVALID_PROCESSOR;
    }
    processor_node->process_event(event);
    return EngineReturnStatus::OK;
}

EngineReturnStatus AudioEngine::send_async_event(RtEvent& event)
{
    std::lock_guard<std::mutex> lock(_in_queue_lock);
    if (_internal_control_queue.push(event))
    {
        return EngineReturnStatus::OK;
    }
    return EngineReturnStatus::QUEUE_FULL;
}


std::pair<EngineReturnStatus, ObjectId> AudioEngine::processor_id_from_name(const std::string& name)
{
    auto processor_node = _processors.find(name);
    if (processor_node == _processors.end())
    {
        return std::make_pair(EngineReturnStatus::INVALID_PROCESSOR, 0);
    }
    return std::make_pair(EngineReturnStatus::OK, processor_node->second->id());
}

std::pair<EngineReturnStatus, ObjectId> AudioEngine::parameter_id_from_name(const std::string& processor_name,
                                                                            const std::string& parameter_name)
{
    auto processor_node = _processors.find(processor_name);
    if (processor_node == _processors.end())
    {
        return std::make_pair(EngineReturnStatus::INVALID_PROCESSOR, 0);
    }
    auto param = processor_node->second->parameter_from_name(parameter_name);
    if (param)
    {
        return std::make_pair(EngineReturnStatus::OK, param->id());
    }
    return std::make_pair(EngineReturnStatus::INVALID_PARAMETER, 0);
};

std::pair<EngineReturnStatus, const std::string> AudioEngine::processor_name_from_id(const ObjectId uid)
{
    if (!_processor_exists(uid))
    {
        return std::make_pair(EngineReturnStatus::INVALID_PROCESSOR, std::string(""));
    }
    return std::make_pair(EngineReturnStatus::OK, _realtime_processors[uid]->name());
}

std::pair<EngineReturnStatus, const std::string> AudioEngine::parameter_name_from_id(const std::string &processor_name,
                                                                                     const ObjectId id)
{
    auto processor_node = _processors.find(processor_name);
    if (processor_node == _processors.end())
    {
        return std::make_pair(EngineReturnStatus::INVALID_PROCESSOR, "");
    }
    auto param = processor_node->second->parameter_from_id(id);
    if (param)
    {
        return std::make_pair(EngineReturnStatus::OK, param->name());
    }
    return std::make_pair(EngineReturnStatus::INVALID_PARAMETER, "");
}

EngineReturnStatus AudioEngine::create_multibus_track(const std::string& name, int input_busses, int output_busses)
{
    if((input_busses > TRACK_MAX_BUSSES && output_busses > TRACK_MAX_BUSSES))
    {
        SUSHI_LOG_ERROR("Invalid number of busses for new track");
        return EngineReturnStatus::INVALID_N_CHANNELS;
    }
    Track* track = new Track(_host_control, input_busses, output_busses, &_process_timer);
    return _register_new_track(name, track);
}

EngineReturnStatus AudioEngine::create_track(const std::string &name, int channel_count)
{
    if((channel_count < 0 || channel_count > 2))
    {
        SUSHI_LOG_ERROR("Invalid number of channels for new track");
        return EngineReturnStatus::INVALID_N_CHANNELS;
    }
    Track* track = new Track(_host_control, channel_count, &_process_timer);
    return _register_new_track(name, track);
}

EngineReturnStatus AudioEngine::delete_track(const std::string &track_name)
{
    // TODO - Until it's decided how tracks report what processors they have,
    // we assume that the track has no processors before deleting
    auto track_node = _processors.find(track_name);
    if (track_node == _processors.end())
    {
        SUSHI_LOG_ERROR("Couldn't delete track {}, not found", track_name);
        return EngineReturnStatus::INVALID_TRACK;
    }
    auto track = track_node->second.get();
    if (realtime())
    {
        auto remove_track_event = RtEvent::make_remove_track_event(track->id());
        auto delete_event = RtEvent::make_remove_processor_event(track->id());
        send_async_event(remove_track_event);
        send_async_event(delete_event);
        bool removed = _event_receiver.wait_for_response(remove_track_event.returnable_event()->event_id(), RT_EVENT_TIMEOUT);
        bool deleted = _event_receiver.wait_for_response(delete_event.returnable_event()->event_id(), RT_EVENT_TIMEOUT);
        if (!removed || !deleted)
        {
            SUSHI_LOG_ERROR("Failed to remove processor {} from processing part", track_name);
        }
        return _deregister_processor(track_name);
    }
    else
    {
        for (auto track_in_graph = _audio_graph.begin(); track_in_graph != _audio_graph.end(); ++track)
        {
            if (*track_in_graph == track)
            {
                _audio_graph.erase(track_in_graph);
                _remove_processor_from_realtime_part(track->id());
                return _deregister_processor(track_name);
            }
            SUSHI_LOG_WARNING("Plugin track {} was not in the audio graph", track_name);
        }
        return EngineReturnStatus::INVALID_TRACK;
    }
}

EngineReturnStatus AudioEngine::add_plugin_to_track(const std::string &track_name,
                                                    const std::string &plugin_uid,
                                                    const std::string &plugin_name,
                                                    const std::string &plugin_path,
                                                    PluginType plugin_type)
{
    auto track_node = _processors.find(track_name);
    if (track_node == _processors.end())
    {
        SUSHI_LOG_ERROR("Track named {} does not exist in processor list", track_name);
        return EngineReturnStatus::INVALID_TRACK;
    }
    auto track = static_cast<Track*>(track_node->second.get());
    Processor* plugin{nullptr};
    switch (plugin_type)
    {
        case PluginType::INTERNAL:
            plugin = _make_internal_plugin(plugin_uid);
            if(plugin == nullptr)
            {
                SUSHI_LOG_ERROR("Unrecognised internal plugin \"{}\"", plugin_uid);
                return EngineReturnStatus::INVALID_PLUGIN_UID;
            }
            break;

        case PluginType::VST2X:
            plugin = new vst2::Vst2xWrapper(_host_control, plugin_path);
            break;

        case PluginType::VST3X:
            plugin = new vst3::Vst3xWrapper(_host_control, plugin_path, plugin_uid);
            break;

        case PluginType::LV2:
            plugin = new lv2::LV2_Wrapper(_host_control, plugin_path);
            break;
    }

    auto processor_status = plugin->init(_sample_rate);
    if(processor_status != ProcessorReturnCode::OK)
    {
        SUSHI_LOG_ERROR("Failed to initialize plugin {}", plugin_name);
        return EngineReturnStatus::INVALID_PLUGIN_UID;
    }
    EngineReturnStatus status = _register_processor(plugin, plugin_name);
    if(status != EngineReturnStatus::OK)
    {
        SUSHI_LOG_ERROR("Failed to register plugin {}", plugin_name);
        delete plugin;
        return status;
    }
    plugin->set_enabled(true);
    if (realtime())
    {
        // In realtime mode we need to handle this in the audio thread
        auto insert_event = RtEvent::make_insert_processor_event(plugin);
        auto add_event = RtEvent::make_add_processor_to_track_event(plugin->id(), track->id());
        send_async_event(insert_event);
        send_async_event(add_event);
        bool inserted = _event_receiver.wait_for_response(insert_event.returnable_event()->event_id(), RT_EVENT_TIMEOUT);
        bool added = _event_receiver.wait_for_response(add_event.returnable_event()->event_id(), RT_EVENT_TIMEOUT);
        if (!inserted || !added)
        {
            SUSHI_LOG_ERROR("Failed to insert/add processor {} to processing part", plugin_name);
            return EngineReturnStatus::INVALID_PROCESSOR;
        }
    }
    else
    {
        // If the engine is not running in realtime mode we can add the processor directly
        _insert_processor_in_realtime_part(plugin);
        if (track->add(plugin) == false)
        {
            return EngineReturnStatus::ERROR;
        }
    }
    return EngineReturnStatus::OK;
}

/* TODO - In the future it should be possible to remove plugins without deleting them
 * and consequentally to add them to a different track or have plugins not associated
 * to a particular track. */
EngineReturnStatus AudioEngine::remove_plugin_from_track(const std::string &track_name, const std::string &plugin_name)
{
    auto track_node = _processors.find(track_name);
    if (track_node == _processors.end())
    {
        return EngineReturnStatus::INVALID_TRACK;
    }
    auto processor_node = _processors.find(plugin_name);
    if (processor_node == _processors.end())
    {
        return EngineReturnStatus::INVALID_PLUGIN_NAME;
    }
    auto processor = processor_node->second.get();
    Track* track = static_cast<Track*>(track_node->second.get());
    if (realtime())
    {
        // Send events to handle this in the rt domain
        auto remove_event = RtEvent::make_remove_processor_from_track_event(processor->id(), track->id());
        auto delete_event = RtEvent::make_remove_processor_event(processor->id());
        send_async_event(remove_event);
        send_async_event(delete_event);
        bool remove_ok = _event_receiver.wait_for_response(remove_event.returnable_event()->event_id(), RT_EVENT_TIMEOUT);
        bool delete_ok = _event_receiver.wait_for_response(delete_event.returnable_event()->event_id(), RT_EVENT_TIMEOUT);
        if (!remove_ok || !delete_ok)
        {
            SUSHI_LOG_ERROR("Failed to remove/delete processor {} from processing part", plugin_name);
        }
    }
    else
    {
        if (!track->remove(processor->id()))
        {
            SUSHI_LOG_ERROR("Failed to remove processor {} from track {}", plugin_name, track_name);
        }
        _remove_processor_from_realtime_part(processor->id());
    }
    return _deregister_processor(processor->name());
}

const Processor* AudioEngine::processor(ObjectId processor_id) const
{
    return const_cast<AudioEngine*>(this)->mutable_processor(processor_id);
}

Processor* AudioEngine::mutable_processor(ObjectId processor_id)
{
    if (processor_id >= _realtime_processors.size())
    {
        return nullptr;
    }
    return _realtime_processors[processor_id];
}

EngineReturnStatus AudioEngine::_register_new_track(const std::string& name, Track* track)
{
    track->init(_sample_rate);
    auto status = _register_processor(track, name);
    if (status != EngineReturnStatus::OK)
    {
        delete track;
        return status;
    }
    if (_multicore_processing)
    {
        // Have tracks buffer their events internally as outputting directly might not be thread safe
        track->set_event_output_internal();
    }
    else
    {
        track->set_event_output(&_processor_out_queue);
    }
    if (realtime())
    {
        auto insert_event = RtEvent::make_insert_processor_event(track);
        auto add_event = RtEvent::make_add_track_event(track->id());
        send_async_event(insert_event);
        send_async_event(add_event);
        bool inserted = _event_receiver.wait_for_response(insert_event.returnable_event()->event_id(), RT_EVENT_TIMEOUT);
        bool added = _event_receiver.wait_for_response(add_event.returnable_event()->event_id(), RT_EVENT_TIMEOUT);
        if (!inserted || !added)
        {
            SUSHI_LOG_ERROR("Failed to insert/add track {} to processing part", name);
            return EngineReturnStatus::INVALID_PROCESSOR;
        }
    } else
    {
        _insert_processor_in_realtime_part(track);
        _audio_graph.push_back(track);
    }
    if (_multicore_processing)
    {
        _worker_pool->add_worker(Track::ext_render_function, track);
    }
    SUSHI_LOG_INFO("Track {} successfully added to engine", name);
    return EngineReturnStatus::OK;
}

bool AudioEngine::_handle_internal_events(RtEvent& event)
{
    switch (event.type())
    {
        case RtEventType::STOP_ENGINE:
        {
            auto typed_event = event.returnable_event();
            _state.store(RealtimeState::STOPPING);
            typed_event->set_handled(true);
            break;
        }
        case RtEventType::INSERT_PROCESSOR:
        {
            auto typed_event = event.processor_operation_event();
            bool ok = _insert_processor_in_realtime_part(typed_event->instance());
            typed_event->set_handled(ok);
            break;
        }
        case RtEventType::REMOVE_PROCESSOR:
        {
            auto typed_event = event.processor_reorder_event();
            bool ok = _remove_processor_from_realtime_part(typed_event->processor());
            typed_event->set_handled(ok);
            break;
        }
        case RtEventType::ADD_PROCESSOR_TO_TRACK:
        {
            auto typed_event = event.processor_reorder_event();
            Track* track = static_cast<Track*>(_realtime_processors[typed_event->track()]);
            Processor* processor = static_cast<Processor*>(_realtime_processors[typed_event->processor()]);
            if (track && processor)
            {
                auto ok = track->add(processor);
                typed_event->set_handled(ok);
            }
            else
            {
                typed_event->set_handled(false);
            }
            break;
        }
        case RtEventType::REMOVE_PROCESSOR_FROM_TRACK:
        {
            auto typed_event = event.processor_reorder_event();
            Track* track = static_cast<Track*>(_realtime_processors[typed_event->track()]);
            if (track)
            {
                bool ok = track->remove(typed_event->processor());
                typed_event->set_handled(ok);
            }
            else
                typed_event->set_handled(true);
            break;
        }
        case RtEventType::ADD_TRACK:
        {
            auto typed_event = event.processor_reorder_event();
            Track* track = static_cast<Track*>(_realtime_processors[typed_event->track()]);
            if (track)
            {
                _audio_graph.push_back(track);
                typed_event->set_handled(true);
            }
            else
                typed_event->set_handled(false);
            break;
        }
        case RtEventType::REMOVE_TRACK:
        {
            auto typed_event = event.processor_reorder_event();
            Track* track = static_cast<Track*>(_realtime_processors[typed_event->track()]);
            if (track)
            {
                for (auto i = _audio_graph.begin(); i != _audio_graph.end(); ++i)
                {
                    if ((*i)->id() == typed_event->track())
                    {
                        _audio_graph.erase(i);
                        typed_event->set_handled(true);
                        break;
                    }
                }
            }
            else
                typed_event->set_handled(false);
            break;
        }
        case RtEventType::TEMPO:
        case RtEventType::TIME_SIGNATURE:
        case RtEventType::PLAYING_MODE:
        case RtEventType::SYNC_MODE:
        {
            _transport.process_event(event);
            break;
        }

        default:
            return false;
    }
    _control_queue_out.push(event); // Send event back to non-rt domain
    return true;
}

void AudioEngine::_retrieve_events_from_tracks(ControlBuffer& buffer)
{
    for (auto& track : _audio_graph)
    {
        auto& event_buffer = track->output_event_buffer();
        _process_outgoing_events(buffer, event_buffer);
    }
}

void AudioEngine::_copy_audio_to_tracks(ChunkSampleBuffer* input)
{
    for (const auto& c : _in_audio_connections)
    {
        auto engine_in = ChunkSampleBuffer::create_non_owning_buffer(*input, c.engine_channel, 1);
        auto track_in = static_cast<Track*>(_realtime_processors[c.track])->input_channel(c.track_channel);
        track_in = engine_in;
    }
}

void AudioEngine::_copy_audio_from_tracks(ChunkSampleBuffer* output)
{
    output->clear();
    for (const auto& c : _out_audio_connections)
    {
        auto track_out = static_cast<Track*>(_realtime_processors[c.track])->output_channel(c.track_channel);
        auto engine_out = ChunkSampleBuffer::create_non_owning_buffer(*output, c.engine_channel, 1);
        engine_out.add(track_out);
    }
}

void AudioEngine::print_timings_to_log()
{
    if (_process_timer.enabled())
    {
        for (const auto& processor : _processors)
        {
            auto id = processor.second->id();
            auto timings = _process_timer.timings_for_node(id);
            if (timings.has_value())
            {
                SUSHI_LOG_INFO("Processor: {} ({}), avg: {}%, min: {}%, max: {}%", id, processor.second->name(),
                              timings->avg_case * 100.0f, timings->min_case * 100.0f, timings->max_case * 100.0f);
            }
        }
        auto timings = _process_timer.timings_for_node(ENGINE_TIMING_ID);
        if (timings.has_value())
        {
            SUSHI_LOG_INFO("Engine total: avg: {}%, min: {}%, max: {}%",
                          timings->avg_case * 100.0f, timings->min_case * 100.0f, timings->max_case * 100.0f);
        }
    }
}


void print_single_timings_for_node(std::fstream& f, performance::PerformanceTimer& timer, int id)
{
    auto timings = timer.timings_for_node(id);
    if (timings.has_value())
    {
        f << std::setw(16) << timings.value().avg_case * 100.0
          << std::setw(16) << timings.value().min_case * 100.0
          << std::setw(16) << timings.value().max_case * 100.0 <<"\n";
    }
}

void AudioEngine::print_timings_to_file(const std::string& filename)
{
    std::fstream file;
    file.open(filename, std::ios_base::out);
    if (!file.is_open())
    {
        SUSHI_LOG_WARNING("Couldn't write timings to file");
        return;
    }
    file.setf(std::ios::left);
    file << "Performance timings for all processors in percentages of audio buffer (100% = "<< 1000000.0 / _sample_rate * AUDIO_CHUNK_SIZE
         << "us)\n\n" << std::setw(24) << "" << std::setw(16) << "average(%)" << std::setw(16) << "minimum(%)"
         << std::setw(16) << "maximum(%)" << std::endl;

    for (const auto& track : _audio_graph)
    {
        file << std::setw(0) << "Track: " << track->name() << "\n";
        auto processors = track->process_chain();
        for (auto& p : processors)
        {
            file << std::setw(8) << "" << std::setw(16) << p->name();
            print_single_timings_for_node(file, _process_timer, p->id());
        }
        file << std::setw(8) << "" << std::setw(16) << "Track total";
        print_single_timings_for_node(file, _process_timer, track->id());
        file << "\n";
    }

    file << std::setw(24) << "Engine total";
    print_single_timings_for_node(file, _process_timer, ENGINE_TIMING_ID);
    file.close();
}

void AudioEngine::_route_cv_gate_ins(ControlBuffer& buffer)
{
    for (const auto& r : _cv_in_routes)
    {
        float value = buffer.cv_values[r.cv_id];
        auto ev = RtEvent::make_parameter_change_event(r.processor_id, 0, r.parameter_id, value);
        send_rt_event(ev);
    }
    // Get gate state changes by xor:ing with previous states
    auto gate_diffs = _prev_gate_values ^ buffer.gate_values;
    if (gate_diffs.any())
    {
        for (const auto& r : _gate_in_routes)
        {
            if (gate_diffs[r.gate_id])
            {
                auto gate_high = buffer.gate_values[r.gate_id];
                if (gate_high)
                {
                    auto ev = RtEvent::make_note_on_event(r.processor_id, 0, r.channel, r.note_no, 1.0f);
                    send_rt_event(ev);
                }
                else
                {
                    auto ev = RtEvent::make_note_off_event(r.processor_id, 0, r.channel, r.note_no, 1.0f);
                    send_rt_event(ev);
                }
            }
        }
    }
    _prev_gate_values = buffer.gate_values;
}

void AudioEngine::_process_outgoing_events(ControlBuffer& buffer, RtSafeRtEventFifo& source_queue)
{
    RtEvent event;
    while (source_queue.pop(event))
    {
        switch (event.type())
        {
            case RtEventType::CV_EVENT:
            {
                auto typed_event = event.cv_event();
                buffer.cv_values[typed_event->cv_id()] = typed_event->value();
                break;
            }

            case RtEventType::GATE_EVENT:
            {
                auto typed_event = event.gate_event();
                _outgoing_gate_values[typed_event->gate_no()] = typed_event->value();
                break;
            }

            default:
                _main_out_queue.push(event);
        }
    }
    buffer.gate_values = _outgoing_gate_values;
}

RealtimeState update_state(RealtimeState current_state)
{
    if (current_state == RealtimeState::STARTING)
    {
        return RealtimeState::RUNNING;
    }
    if (current_state == RealtimeState::STOPPING)
    {
        return RealtimeState::STOPPED;
    }
    return current_state;
}

} // namespace engine
} // namespace sushi
