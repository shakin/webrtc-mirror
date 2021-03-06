/*
 *  Copyright (c) 2015 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "webrtc/audio/audio_send_stream.h"

#include <string>

#include "webrtc/audio/audio_state.h"
#include "webrtc/audio/conversion.h"
#include "webrtc/audio/scoped_voe_interface.h"
#include "webrtc/base/checks.h"
#include "webrtc/base/event.h"
#include "webrtc/base/logging.h"
#include "webrtc/base/task_queue.h"
#include "webrtc/modules/congestion_controller/include/congestion_controller.h"
#include "webrtc/modules/pacing/paced_sender.h"
#include "webrtc/modules/rtp_rtcp/include/rtp_rtcp_defines.h"
#include "webrtc/voice_engine/channel_proxy.h"
#include "webrtc/voice_engine/include/voe_audio_processing.h"
#include "webrtc/voice_engine/include/voe_codec.h"
#include "webrtc/voice_engine/include/voe_rtp_rtcp.h"
#include "webrtc/voice_engine/include/voe_volume_control.h"
#include "webrtc/voice_engine/voice_engine_impl.h"

namespace webrtc {

namespace {

constexpr char kOpusCodecName[] = "opus";

// TODO(minyue): Remove |LOG_RTCERR2|.
#define LOG_RTCERR2(func, a1, a2, err)                      \
  LOG(LS_WARNING) << "" << #func << "(" << a1 << ", " << a2 \
                  << ") failed, err=" << err

// TODO(minyue): Remove |LOG_RTCERR3|.
#define LOG_RTCERR3(func, a1, a2, a3, err)                                \
  LOG(LS_WARNING) << "" << #func << "(" << a1 << ", " << a2 << ", " << a3 \
                  << ") failed, err=" << err

std::string ToString(const webrtc::CodecInst& codec) {
  std::stringstream ss;
  ss << codec.plname << "/" << codec.plfreq << "/" << codec.channels << " ("
     << codec.pltype << ")";
  return ss.str();
}

bool IsCodec(const webrtc::CodecInst& codec, const char* ref_name) {
  return (_stricmp(codec.plname, ref_name) == 0);
}

}  // namespace

std::string AudioSendStream::Config::Rtp::ToString() const {
  std::stringstream ss;
  ss << "{ssrc: " << ssrc;
  ss << ", extensions: [";
  for (size_t i = 0; i < extensions.size(); ++i) {
    ss << extensions[i].ToString();
    if (i != extensions.size() - 1) {
      ss << ", ";
    }
  }
  ss << ']';
  ss << ", nack: " << nack.ToString();
  ss << ", c_name: " << c_name;
  ss << '}';
  return ss.str();
}

std::string AudioSendStream::Config::ToString() const {
  std::stringstream ss;
  ss << "{rtp: " << rtp.ToString();
  ss << ", voe_channel_id: " << voe_channel_id;
  // TODO(solenberg): Encoder config.
  ss << ", cng_payload_type: " << send_codec_spec.cng_payload_type;
  ss << '}';
  return ss.str();
}

namespace internal {
AudioSendStream::AudioSendStream(
    const webrtc::AudioSendStream::Config& config,
    const rtc::scoped_refptr<webrtc::AudioState>& audio_state,
    rtc::TaskQueue* worker_queue,
    CongestionController* congestion_controller,
    BitrateAllocator* bitrate_allocator,
    RtcEventLog* event_log)
    : worker_queue_(worker_queue),
      config_(config),
      audio_state_(audio_state),
      bitrate_allocator_(bitrate_allocator) {
  LOG(LS_INFO) << "AudioSendStream: " << config_.ToString();
  RTC_DCHECK_NE(config_.voe_channel_id, -1);
  RTC_DCHECK(audio_state_.get());
  RTC_DCHECK(congestion_controller);

  VoiceEngineImpl* voe_impl = static_cast<VoiceEngineImpl*>(voice_engine());
  channel_proxy_ = voe_impl->GetChannelProxy(config_.voe_channel_id);
  channel_proxy_->SetRtcEventLog(event_log);
  channel_proxy_->RegisterSenderCongestionControlObjects(
      congestion_controller->pacer(),
      congestion_controller->GetTransportFeedbackObserver(),
      congestion_controller->packet_router());
  channel_proxy_->SetRTCPStatus(true);
  channel_proxy_->SetLocalSSRC(config.rtp.ssrc);
  channel_proxy_->SetRTCP_CNAME(config.rtp.c_name);
  // TODO(solenberg): Config NACK history window (which is a packet count),
  // using the actual packet size for the configured codec.
  channel_proxy_->SetNACKStatus(config_.rtp.nack.rtp_history_ms != 0,
                                config_.rtp.nack.rtp_history_ms / 20);

  channel_proxy_->RegisterExternalTransport(config.send_transport);

  for (const auto& extension : config.rtp.extensions) {
    if (extension.uri == RtpExtension::kAbsSendTimeUri) {
      channel_proxy_->SetSendAbsoluteSenderTimeStatus(true, extension.id);
    } else if (extension.uri == RtpExtension::kAudioLevelUri) {
      channel_proxy_->SetSendAudioLevelIndicationStatus(true, extension.id);
    } else if (extension.uri == RtpExtension::kTransportSequenceNumberUri) {
      channel_proxy_->EnableSendTransportSequenceNumber(extension.id);
    } else {
      RTC_NOTREACHED() << "Registering unsupported RTP extension.";
    }
  }
  if (!SetupSendCodec()) {
    LOG(LS_ERROR) << "Failed to set up send codec state.";
  }
}

AudioSendStream::~AudioSendStream() {
  RTC_DCHECK(thread_checker_.CalledOnValidThread());
  LOG(LS_INFO) << "~AudioSendStream: " << config_.ToString();
  channel_proxy_->DeRegisterExternalTransport();
  channel_proxy_->ResetCongestionControlObjects();
  channel_proxy_->SetRtcEventLog(nullptr);
}

void AudioSendStream::Start() {
  RTC_DCHECK(thread_checker_.CalledOnValidThread());
  if (config_.min_bitrate_kbps != -1 && config_.max_bitrate_kbps != -1) {
    RTC_DCHECK_GE(config_.max_bitrate_kbps, config_.min_bitrate_kbps);
    rtc::Event thread_sync_event(false /* manual_reset */, false);
    worker_queue_->PostTask([this, &thread_sync_event] {
      bitrate_allocator_->AddObserver(this, config_.min_bitrate_kbps * 1000,
                                      config_.max_bitrate_kbps * 1000, 0, true);
      thread_sync_event.Set();
    });
    thread_sync_event.Wait(rtc::Event::kForever);
  }

  ScopedVoEInterface<VoEBase> base(voice_engine());
  int error = base->StartSend(config_.voe_channel_id);
  if (error != 0) {
    LOG(LS_ERROR) << "AudioSendStream::Start failed with error: " << error;
  }
}

void AudioSendStream::Stop() {
  RTC_DCHECK(thread_checker_.CalledOnValidThread());
  rtc::Event thread_sync_event(false /* manual_reset */, false);
  worker_queue_->PostTask([this, &thread_sync_event] {
    bitrate_allocator_->RemoveObserver(this);
    thread_sync_event.Set();
  });
  thread_sync_event.Wait(rtc::Event::kForever);

  ScopedVoEInterface<VoEBase> base(voice_engine());
  int error = base->StopSend(config_.voe_channel_id);
  if (error != 0) {
    LOG(LS_ERROR) << "AudioSendStream::Stop failed with error: " << error;
  }
}

bool AudioSendStream::SendTelephoneEvent(int payload_type, int event,
                                         int duration_ms) {
  RTC_DCHECK(thread_checker_.CalledOnValidThread());
  return channel_proxy_->SetSendTelephoneEventPayloadType(payload_type) &&
         channel_proxy_->SendTelephoneEventOutband(event, duration_ms);
}

void AudioSendStream::SetMuted(bool muted) {
  RTC_DCHECK(thread_checker_.CalledOnValidThread());
  channel_proxy_->SetInputMute(muted);
}

webrtc::AudioSendStream::Stats AudioSendStream::GetStats() const {
  RTC_DCHECK(thread_checker_.CalledOnValidThread());
  webrtc::AudioSendStream::Stats stats;
  stats.local_ssrc = config_.rtp.ssrc;
  ScopedVoEInterface<VoEAudioProcessing> processing(voice_engine());
  ScopedVoEInterface<VoECodec> codec(voice_engine());
  ScopedVoEInterface<VoEVolumeControl> volume(voice_engine());

  webrtc::CallStatistics call_stats = channel_proxy_->GetRTCPStatistics();
  stats.bytes_sent = call_stats.bytesSent;
  stats.packets_sent = call_stats.packetsSent;
  // RTT isn't known until a RTCP report is received. Until then, VoiceEngine
  // returns 0 to indicate an error value.
  if (call_stats.rttMs > 0) {
    stats.rtt_ms = call_stats.rttMs;
  }
  // TODO(solenberg): [was ajm]: Re-enable this metric once we have a reliable
  //                  implementation.
  stats.aec_quality_min = -1;

  webrtc::CodecInst codec_inst = {0};
  if (codec->GetSendCodec(config_.voe_channel_id, codec_inst) != -1) {
    RTC_DCHECK_NE(codec_inst.pltype, -1);
    stats.codec_name = codec_inst.plname;

    // Get data from the last remote RTCP report.
    for (const auto& block : channel_proxy_->GetRemoteRTCPReportBlocks()) {
      // Lookup report for send ssrc only.
      if (block.source_SSRC == stats.local_ssrc) {
        stats.packets_lost = block.cumulative_num_packets_lost;
        stats.fraction_lost = Q8ToFloat(block.fraction_lost);
        stats.ext_seqnum = block.extended_highest_sequence_number;
        // Convert samples to milliseconds.
        if (codec_inst.plfreq / 1000 > 0) {
          stats.jitter_ms =
              block.interarrival_jitter / (codec_inst.plfreq / 1000);
        }
        break;
      }
    }
  }

  // Local speech level.
  {
    unsigned int level = 0;
    int error = volume->GetSpeechInputLevelFullRange(level);
    RTC_DCHECK_EQ(0, error);
    stats.audio_level = static_cast<int32_t>(level);
  }

  bool echo_metrics_on = false;
  int error = processing->GetEcMetricsStatus(echo_metrics_on);
  RTC_DCHECK_EQ(0, error);
  if (echo_metrics_on) {
    // These can also be negative, but in practice -1 is only used to signal
    // insufficient data, since the resolution is limited to multiples of 4 ms.
    int median = -1;
    int std = -1;
    float dummy = 0.0f;
    error = processing->GetEcDelayMetrics(median, std, dummy);
    RTC_DCHECK_EQ(0, error);
    stats.echo_delay_median_ms = median;
    stats.echo_delay_std_ms = std;

    // These can take on valid negative values, so use the lowest possible level
    // as default rather than -1.
    int erl = -100;
    int erle = -100;
    int dummy1 = 0;
    int dummy2 = 0;
    error = processing->GetEchoMetrics(erl, erle, dummy1, dummy2);
    RTC_DCHECK_EQ(0, error);
    stats.echo_return_loss = erl;
    stats.echo_return_loss_enhancement = erle;
  }

  internal::AudioState* audio_state =
      static_cast<internal::AudioState*>(audio_state_.get());
  stats.typing_noise_detected = audio_state->typing_noise_detected();

  return stats;
}

void AudioSendStream::SignalNetworkState(NetworkState state) {
  RTC_DCHECK(thread_checker_.CalledOnValidThread());
}

bool AudioSendStream::DeliverRtcp(const uint8_t* packet, size_t length) {
  // TODO(solenberg): Tests call this function on a network thread, libjingle
  // calls on the worker thread. We should move towards always using a network
  // thread. Then this check can be enabled.
  // RTC_DCHECK(!thread_checker_.CalledOnValidThread());
  return channel_proxy_->ReceivedRTCPPacket(packet, length);
}

uint32_t AudioSendStream::OnBitrateUpdated(uint32_t bitrate_bps,
                                           uint8_t fraction_loss,
                                           int64_t rtt) {
  RTC_DCHECK_GE(bitrate_bps,
                static_cast<uint32_t>(config_.min_bitrate_kbps * 1000));
  // The bitrate allocator might allocate an higher than max configured bitrate
  // if there is room, to allow for, as example, extra FEC. Ignore that for now.
  const uint32_t max_bitrate_bps = config_.max_bitrate_kbps * 1000;
  if (bitrate_bps > max_bitrate_bps)
    bitrate_bps = max_bitrate_bps;

  channel_proxy_->SetBitrate(bitrate_bps);

  // The amount of audio protection is not exposed by the encoder, hence
  // always returning 0.
  return 0;
}

const webrtc::AudioSendStream::Config& AudioSendStream::config() const {
  RTC_DCHECK(thread_checker_.CalledOnValidThread());
  return config_;
}

VoiceEngine* AudioSendStream::voice_engine() const {
  internal::AudioState* audio_state =
      static_cast<internal::AudioState*>(audio_state_.get());
  VoiceEngine* voice_engine = audio_state->voice_engine();
  RTC_DCHECK(voice_engine);
  return voice_engine;
}

// Apply current codec settings to a single voe::Channel used for sending.
bool AudioSendStream::SetupSendCodec() {
  ScopedVoEInterface<VoEBase> base(voice_engine());
  ScopedVoEInterface<VoECodec> codec(voice_engine());

  const int channel = config_.voe_channel_id;

  // Disable VAD and FEC unless we know the other side wants them.
  codec->SetVADStatus(channel, false);
  codec->SetFECStatus(channel, false);

  const auto& send_codec_spec = config_.send_codec_spec;

  // Set the codec immediately, since SetVADStatus() depends on whether
  // the current codec is mono or stereo.
  LOG(LS_INFO) << "Send channel " << channel << " selected voice codec "
               << ToString(send_codec_spec.codec_inst)
               << ", bitrate=" << send_codec_spec.codec_inst.rate;

  // If codec is already configured, we do not it again.
  // TODO(minyue): check if this check is really needed, or can we move it into
  // |codec->SetSendCodec|.
  webrtc::CodecInst current_codec = {0};
  if (codec->GetSendCodec(channel, current_codec) != 0 ||
      (send_codec_spec.codec_inst != current_codec)) {
    if (codec->SetSendCodec(channel, send_codec_spec.codec_inst) == -1) {
      LOG_RTCERR2(SetSendCodec, channel, ToString(send_codec_spec.codec_inst),
                  base->LastError());
      return false;
    }
  }

  // FEC should be enabled after SetSendCodec.
  if (send_codec_spec.enable_codec_fec) {
    LOG(LS_INFO) << "Attempt to enable codec internal FEC on channel "
                 << channel;
    if (codec->SetFECStatus(channel, true) == -1) {
      // Enable codec internal FEC. Treat any failure as fatal internal error.
      LOG_RTCERR2(SetFECStatus, channel, true, base->LastError());
      return false;
    }
  }

  if (IsCodec(send_codec_spec.codec_inst, kOpusCodecName)) {
    // DTX and maxplaybackrate should be set after SetSendCodec. Because current
    // send codec has to be Opus.

    // Set Opus internal DTX.
    LOG(LS_INFO) << "Attempt to "
                 << (send_codec_spec.enable_opus_dtx ? "enable" : "disable")
                 << " Opus DTX on channel " << channel;
    if (codec->SetOpusDtx(channel, send_codec_spec.enable_opus_dtx)) {
      LOG_RTCERR2(SetOpusDtx, channel, send_codec_spec.enable_opus_dtx,
                  base->LastError());
      return false;
    }

    // If opus_max_playback_rate <= 0, the default maximum playback rate
    // (48 kHz) will be used.
    if (send_codec_spec.opus_max_playback_rate > 0) {
      LOG(LS_INFO) << "Attempt to set maximum playback rate to "
                   << send_codec_spec.opus_max_playback_rate
                   << " Hz on channel " << channel;
      if (codec->SetOpusMaxPlaybackRate(
              channel, send_codec_spec.opus_max_playback_rate) == -1) {
        LOG_RTCERR2(SetOpusMaxPlaybackRate, channel,
                    send_codec_spec.opus_max_playback_rate, base->LastError());
        return false;
      }
    }
  }

  // Set the CN payloadtype and the VAD status.
  if (send_codec_spec.cng_payload_type != -1) {
    // The CN payload type for 8000 Hz clockrate is fixed at 13.
    if (send_codec_spec.cng_plfreq != 8000) {
      webrtc::PayloadFrequencies cn_freq;
      switch (send_codec_spec.cng_plfreq) {
        case 16000:
          cn_freq = webrtc::kFreq16000Hz;
          break;
        case 32000:
          cn_freq = webrtc::kFreq32000Hz;
          break;
        default:
          RTC_NOTREACHED();
          return false;
      }
      if (codec->SetSendCNPayloadType(channel, send_codec_spec.cng_payload_type,
                                      cn_freq) == -1) {
        LOG_RTCERR3(SetSendCNPayloadType, channel,
                    send_codec_spec.cng_payload_type, cn_freq,
                    base->LastError());

        // TODO(ajm): This failure condition will be removed from VoE.
        // Restore the return here when we update to a new enough webrtc.
        //
        // Not returning false because the SetSendCNPayloadType will fail if
        // the channel is already sending.
        // This can happen if the remote description is applied twice, for
        // example in the case of ROAP on top of JSEP, where both side will
        // send the offer.
      }
    }

    // Only turn on VAD if we have a CN payload type that matches the
    // clockrate for the codec we are going to use.
    if (send_codec_spec.cng_plfreq == send_codec_spec.codec_inst.plfreq &&
        send_codec_spec.codec_inst.channels == 1) {
      // TODO(minyue): If CN frequency == 48000 Hz is allowed, consider the
      // interaction between VAD and Opus FEC.
      LOG(LS_INFO) << "Enabling VAD";
      if (codec->SetVADStatus(channel, true) == -1) {
        LOG_RTCERR2(SetVADStatus, channel, true, base->LastError());
        return false;
      }
    }
  }
  return true;
}

}  // namespace internal
}  // namespace webrtc
