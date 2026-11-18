/* The SDK functions the game calls that the host answers with a constant, each with the reason that is enough (runtime/
   patch.h PATCH_ANSWER). An OS-facing import neither implemented (HLE) nor answered here stops the game when called,
   naming it (ounce/lift/src/gen.rs gen_stubs, runtime/hos_hostcall.c hos_import_stop). Every answer below was seen called
   in play and in the smoke and benchmark runs (they returned 0 as generated stubs before 2026-11-15). Answers that
   leave a result unwritten (a float in s0, an output struct) are marked so: real HLEs if the game ever needs more. */
#include "hos.h"

/* ---- nn::aoc */
/* nn::aoc::CountAddOnContent() */
PATCH_ANSWER(_ZN2nn3aoc17CountAddOnContentEv, 0,
             "0: no add-on content");

/* ---- nn::audio */
/* nn::audio::AcquireAudioOutputDeviceNotification(nn::os::SystemEvent*) */
PATCH_ANSWER(_ZN2nn5audio36AcquireAudioOutputDeviceNotificationEPNS_2os11SystemEventE, 0,
             "0 (success), the event left as it was: the output device never changes");
/* nn::audio::AddBiquadFilter(nn::audio::AudioRendererConfig*, nn::audio::BiquadFilterType*, nn::audio::SubMixType*) */
PATCH_ANSWER(_ZN2nn5audio15AddBiquadFilterEPNS0_19AudioRendererConfigEPNS0_16BiquadFilterTypeEPNS0_10SubMixTypeE, 0,
             "0 (success): the filter is never applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::AddDelay(nn::audio::AudioRendererConfig*, nn::audio::DelayType*, void*, unsigned long, nn::audio::S...) */
PATCH_ANSWER(_ZN2nn5audio8AddDelayEPNS0_19AudioRendererConfigEPNS0_9DelayTypeEPvmPNS0_10SubMixTypeENS_8TimeSpanEi, 0,
             "0 (success): the delay is never applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::AddReverb(nn::audio::AudioRendererConfig*, nn::audio::ReverbType*, void*, unsigned long, nn::audio:...) */
PATCH_ANSWER(_ZN2nn5audio9AddReverbEPNS0_19AudioRendererConfigEPNS0_10ReverbTypeEPvmPNS0_10SubMixTypeEi, 0,
             "0 (success): the reverb is never applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::GetAudioDeviceOutputVolume(nn::audio::AudioDeviceName const*) */
PATCH_ANSWER(_ZN2nn5audio26GetAudioDeviceOutputVolumeEPKNS0_15AudioDeviceNameE, 0,
             "x0 only: its float result (s0) keeps what was there (as before); the volume is the host's");
/* nn::audio::GetAudioRendererVoiceDropParameter(nn::audio::AudioRendererHandle) */
PATCH_ANSWER(_ZN2nn5audio34GetAudioRendererVoiceDropParameterENS0_19AudioRendererHandleE, 0,
             "x0 only: its float result (s0) keeps what was there (as before); the renderer drops no voices");
/* nn::audio::GetDelayTime(nn::audio::DelayType const*) */
PATCH_ANSWER(_ZN2nn5audio12GetDelayTimeEPKNS0_9DelayTypeE, 0,
             "0 ns: the delay is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::GetReverbInputOutput(nn::audio::ReverbType const*, signed char*, signed char*, int*, int) */
PATCH_ANSWER(_ZN2nn5audio20GetReverbInputOutputEPKNS0_10ReverbTypeEPaS4_Pii, 0,
             "0, outputs unwritten: the reverb is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::IsDelayEnabled(nn::audio::DelayType const*) */
PATCH_ANSWER(_ZN2nn5audio14IsDelayEnabledEPKNS0_9DelayTypeE, 0,
             "0 (false): the delay is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::IsDeviceOutputVolumeAutoTuneModeEnabled() */
PATCH_ANSWER(_ZN2nn5audio39IsDeviceOutputVolumeAutoTuneModeEnabledEv, 0,
             "0 (false): never enabled");
/* nn::audio::IsReverbEnabled(nn::audio::ReverbType const*) */
PATCH_ANSWER(_ZN2nn5audio15IsReverbEnabledEPKNS0_10ReverbTypeE, 0,
             "0 (false): the reverb is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::SetAudioRendererVoiceDropParameter(nn::audio::AudioRendererHandle, float) */
PATCH_ANSWER(_ZN2nn5audio34SetAudioRendererVoiceDropParameterENS0_19AudioRendererHandleEf, 0,
             "nothing to do: the host's renderer drops no voices");
/* nn::audio::SetAuxEnabled(nn::audio::AuxType*, bool) */
PATCH_ANSWER(_ZN2nn5audio13SetAuxEnabledEPNS0_7AuxTypeEb, 0,
             "nothing to do: the aux is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::SetAuxInputOutput(nn::audio::AuxType*, signed char const*, signed char const*, int) */
PATCH_ANSWER(_ZN2nn5audio17SetAuxInputOutputEPNS0_7AuxTypeEPKaS4_i, 0,
             "nothing to do: the aux is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::SetBiquadFilterEnabled(nn::audio::BiquadFilterType*, bool) */
PATCH_ANSWER(_ZN2nn5audio22SetBiquadFilterEnabledEPNS0_16BiquadFilterTypeEb, 0,
             "nothing to do: the filter is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::SetBiquadFilterInputOutput(nn::audio::BiquadFilterType*, signed char const*, signed char const*, int) */
PATCH_ANSWER(_ZN2nn5audio26SetBiquadFilterInputOutputEPNS0_16BiquadFilterTypeEPKaS4_i, 0,
             "nothing to do: the filter is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::SetBiquadFilterParameter(nn::audio::BiquadFilterType*, nn::audio::TBiquadFilterParameter<float> const&) */
PATCH_ANSWER(_ZN2nn5audio24SetBiquadFilterParameterEPNS0_16BiquadFilterTypeERKNS0_22TBiquadFilterParameterIfEE, 0,
             "nothing to do: the filter is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::SetDelayChannelSpread(nn::audio::DelayType*, float) */
PATCH_ANSWER(_ZN2nn5audio21SetDelayChannelSpreadEPNS0_9DelayTypeEf, 0,
             "nothing to do: the delay is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::SetDelayDryGain(nn::audio::DelayType*, float) */
PATCH_ANSWER(_ZN2nn5audio15SetDelayDryGainEPNS0_9DelayTypeEf, 0,
             "nothing to do: the delay is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::SetDelayEnabled(nn::audio::DelayType*, bool) */
PATCH_ANSWER(_ZN2nn5audio15SetDelayEnabledEPNS0_9DelayTypeEb, 0,
             "nothing to do: the delay is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::SetDelayFeedbackGain(nn::audio::DelayType*, float) */
PATCH_ANSWER(_ZN2nn5audio20SetDelayFeedbackGainEPNS0_9DelayTypeEf, 0,
             "nothing to do: the delay is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::SetDelayInGain(nn::audio::DelayType*, float) */
PATCH_ANSWER(_ZN2nn5audio14SetDelayInGainEPNS0_9DelayTypeEf, 0,
             "nothing to do: the delay is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::SetDelayInputOutput(nn::audio::DelayType*, signed char const*, signed char const*, int) */
PATCH_ANSWER(_ZN2nn5audio19SetDelayInputOutputEPNS0_9DelayTypeEPKaS4_i, 0,
             "nothing to do: the delay is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::SetDelayLowPassAmount(nn::audio::DelayType*, float) */
PATCH_ANSWER(_ZN2nn5audio21SetDelayLowPassAmountEPNS0_9DelayTypeEf, 0,
             "nothing to do: the delay is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::SetDelayTime(nn::audio::DelayType*, nn::TimeSpan) */
PATCH_ANSWER(_ZN2nn5audio12SetDelayTimeEPNS0_9DelayTypeENS_8TimeSpanE, 0,
             "nothing to do: the delay is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::SetDeviceOutputVolumeAutoTuneModeEnabled(bool) */
PATCH_ANSWER(_ZN2nn5audio40SetDeviceOutputVolumeAutoTuneModeEnabledEb, 0,
             "nothing to do: the output volume is the host's");
/* nn::audio::SetReverbColoration(nn::audio::ReverbType*, float) */
PATCH_ANSWER(_ZN2nn5audio19SetReverbColorationEPNS0_10ReverbTypeEf, 0,
             "nothing to do: the reverb is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::SetReverbDecayTime(nn::audio::ReverbType*, nn::TimeSpan) */
PATCH_ANSWER(_ZN2nn5audio18SetReverbDecayTimeEPNS0_10ReverbTypeENS_8TimeSpanE, 0,
             "nothing to do: the reverb is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::SetReverbDryGain(nn::audio::ReverbType*, float) */
PATCH_ANSWER(_ZN2nn5audio16SetReverbDryGainEPNS0_10ReverbTypeEf, 0,
             "nothing to do: the reverb is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::SetReverbEarlyGain(nn::audio::ReverbType*, float) */
PATCH_ANSWER(_ZN2nn5audio18SetReverbEarlyGainEPNS0_10ReverbTypeEf, 0,
             "nothing to do: the reverb is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::SetReverbEarlyMode(nn::audio::ReverbType*, nn::audio::ReverbType::EarlyMode) */
PATCH_ANSWER(_ZN2nn5audio18SetReverbEarlyModeEPNS0_10ReverbTypeENS1_9EarlyModeE, 0,
             "nothing to do: the reverb is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::SetReverbEnabled(nn::audio::ReverbType*, bool) */
PATCH_ANSWER(_ZN2nn5audio16SetReverbEnabledEPNS0_10ReverbTypeEb, 0,
             "nothing to do: the reverb is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::SetReverbHighFrequencyDecayRatio(nn::audio::ReverbType*, float) */
PATCH_ANSWER(_ZN2nn5audio32SetReverbHighFrequencyDecayRatioEPNS0_10ReverbTypeEf, 0,
             "nothing to do: the reverb is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::SetReverbInputOutput(nn::audio::ReverbType*, signed char const*, signed char const*, int) */
PATCH_ANSWER(_ZN2nn5audio20SetReverbInputOutputEPNS0_10ReverbTypeEPKaS4_i, 0,
             "nothing to do: the reverb is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::SetReverbLateGain(nn::audio::ReverbType*, float) */
PATCH_ANSWER(_ZN2nn5audio17SetReverbLateGainEPNS0_10ReverbTypeEf, 0,
             "nothing to do: the reverb is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::SetReverbLateMode(nn::audio::ReverbType*, nn::audio::ReverbType::LateMode) */
PATCH_ANSWER(_ZN2nn5audio17SetReverbLateModeEPNS0_10ReverbTypeENS1_8LateModeE, 0,
             "nothing to do: the reverb is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::SetReverbOutGain(nn::audio::ReverbType*, float) */
PATCH_ANSWER(_ZN2nn5audio16SetReverbOutGainEPNS0_10ReverbTypeEf, 0,
             "nothing to do: the reverb is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::SetReverbPredelayTime(nn::audio::ReverbType*, nn::TimeSpan) */
PATCH_ANSWER(_ZN2nn5audio21SetReverbPredelayTimeEPNS0_10ReverbTypeENS_8TimeSpanE, 0,
             "nothing to do: the reverb is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::SetReverbReverbGain(nn::audio::ReverbType*, float) */
PATCH_ANSWER(_ZN2nn5audio19SetReverbReverbGainEPNS0_10ReverbTypeEf, 0,
             "nothing to do: the reverb is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::SetSplitterBiquadFilterParameter(nn::audio::SplitterType*, int, nn::audio::FinalMixType*, int, nn::...) */
PATCH_ANSWER(_ZN2nn5audio32SetSplitterBiquadFilterParameterEPNS0_12SplitterTypeEiPNS0_12FinalMixTypeEiRKNS0_22TBiquadFilterParameterIfEE, 0,
             "nothing to do: a splitter's filter is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::SetSplitterBiquadFilterParameter(nn::audio::SplitterType*, int, nn::audio::SubMixType*, int, nn::au...) */
PATCH_ANSWER(_ZN2nn5audio32SetSplitterBiquadFilterParameterEPNS0_12SplitterTypeEiPNS0_10SubMixTypeEiRKNS0_22TBiquadFilterParameterIfEE, 0,
             "nothing to do: a splitter's filter is not applied: the host's mixer has no effects (runtime/nn_audio.c)");

/* ---- nn::hid */
/* nn::hid::SetGyroscopeZeroDriftMode(nn::hid::SixAxisSensorHandle const&, nn::hid::GyroscopeZeroDriftMode const&) */
PATCH_ANSWER(_ZN2nn3hid25SetGyroscopeZeroDriftModeERKNS0_19SixAxisSensorHandleERKNS0_22GyroscopeZeroDriftModeE, 0,
             "nothing to do: the host's controllers report no motion (runtime/nn_hid.c)");

/* ---- nn::movie */
/* nn::movie::Finalize() */
PATCH_ANSWER(_ZN2nn5movie8FinalizeEv, 0,
             "0: the SDK's movie library is not used (runtime/nn_video.c)");
/* nn::movie::Initialize() */
PATCH_ANSWER(_ZN2nn5movie10InitializeEv, 0,
             "0 (success): the SDK's movie library is not used (runtime/nn_video.c plays the movies)");
/* nn::movie::IsInitialized() */
PATCH_ANSWER(_ZN2nn5movie13IsInitializedEv, 0,
             "0 (false): the SDK's movie library is not used (runtime/nn_video.c)");

/* ---- nn::nfp */
/* nn::nfp::AttachAvailabilityChangeEvent(nn::os::SystemEventType*) */
PATCH_ANSWER(_ZN2nn3nfp29AttachAvailabilityChangeEventEPNS_2os15SystemEventTypeE, 0,
             "nothing to do: no amiibo reader, the event is never signalled");
/* nn::nfp::Initialize() */
PATCH_ANSWER(_ZN2nn3nfp10InitializeEv, 0,
             "nothing to do: no amiibo reader");

/* ---- nn::nifm */
/* nn::nifm::GetCurrentProxySetting(nn::nifm::ProxySetting*) */
PATCH_ANSWER(_ZN2nn4nifm22GetCurrentProxySettingEPNS0_12ProxySettingE, 0,
             "0 (success), the setting left as it was: the host has no network");
/* nn::nifm::Initialize() */
PATCH_ANSWER(_ZN2nn4nifm10InitializeEv, 0,
             "0 (success): the host has no network");
/* nn::nifm::IsNetworkAvailable() */
PATCH_ANSWER(_ZN2nn4nifm18IsNetworkAvailableEv, 0,
             "0 (false): the host has no network");
/* nn::nifm::SubmitNetworkRequestAndWait() */
PATCH_ANSWER(_ZN2nn4nifm27SubmitNetworkRequestAndWaitEv, 0,
             "nothing to do: the request fails (IsNetworkAvailable answers false)");

/* ---- nn::prepo */
/* nn::prepo::AddInternetConnectionStatus(nn::prepo::PlayReport*, char const*, char const*, char const*) */
PATCH_ANSWER(_ZN2nn5prepo27AddInternetConnectionStatusEPNS0_10PlayReportEPKcS4_S4_, 0,
             "0 (success): play reports are never sent");
/* nn::prepo::GetOverSaveFrequencyLimitEvent() */
PATCH_ANSWER(_ZN2nn5prepo30GetOverSaveFrequencyLimitEventEv, 0,
             "0 (no event): play reports are never sent");
/* nn::prepo::RequestImmediateTransmission() */
PATCH_ANSWER(_ZN2nn5prepo28RequestImmediateTransmissionEv, 0,
             "0 (success): play reports are never sent");

/* ---- nn::ro */
/* nn::ro::ProtectRelro(void const*, void const*, void const*, void const*, void const*) */
PATCH_ANSWER(_ZN2nn2ro12ProtectRelroEPKvS2_S2_S2_S2_, 0,
             "0 (success): the host keeps the image writable; nothing to protect");

/* ---- nn::socket */
/* nn::socket::Close(int) */
PATCH_ANSWER(_ZN2nn6socket5CloseEi, 0,
             "0 (success): nothing was opened");
/* nn::socket::EventFd(unsigned long, nn::socket::EventFdFlags) */
PATCH_ANSWER(_ZN2nn6socket7EventFdEmNS0_12EventFdFlagsE, 0,
             "fd 0: only closed (EventFdRead / EventFdWrite stop the game if called)");
/* nn::socket::Initialize(nn::socket::Config const&) */
PATCH_ANSWER(_ZN2nn6socket10InitializeERKNS0_6ConfigE, 0,
             "0 (success): no network; the game then makes only event fds (EventFd, Close)");
/* nn::socket::IsInitialized() */
PATCH_ANSWER(_ZN2nn6socket13IsInitializedEv, 0,
             "0 (false): asked before Initialize, which is answered");

/* ---- nn::ssl */
/* nn::ssl::Initialize() */
PATCH_ANSWER(_ZN2nn3ssl10InitializeEv, 0,
             "0 (success): the host has no network");

/* ---- nn::vi */
/* nn::vi::QueryDisplayCurrentRefreshRate(nn::vi::RefreshRate*, bool*, nn::vi::Display const*) */
PATCH_ANSWER(_ZN2nn2vi30QueryDisplayCurrentRefreshRateEPNS0_11RefreshRateEPbPKNS0_7DisplayE, 0,
             "0 (success), the outputs left as they were: the host paces presents (runtime/nn_vi.c)");
/* nn::vi::SetDisplay120HzRefreshRateCapability(nn::vi::Display*, bool) */
PATCH_ANSWER(_ZN2nn2vi36SetDisplay120HzRefreshRateCapabilityEPNS0_7DisplayEb, 0,
             "0 (success): the host paces presents itself (runtime/nn_vi.c)");
