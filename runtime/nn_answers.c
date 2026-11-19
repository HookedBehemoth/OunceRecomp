/* The SDK functions the game calls that the host answers with a constant, each with the reason that is enough (runtime/
   patch.h PATCH_ANSWER). An OS-facing import neither implemented (HLE) nor answered here stops the game when called,
   naming it (ounce/lift/src/gen.rs gen_stubs, runtime/hos_hostcall.c hos_import_stop). Every answer below was seen called
   by a game, or belongs to a family one was seen calling (an audio effect's Add, Remove, Set and Is). Answers that
   leave a result unwritten (a float in s0, an output struct) are marked so: real HLEs if the game ever needs more. */
#include "hos.h"

/* ---- nn::account */
/* nn::account::LoadCachedNintendoAccountInfo(nn::account::CachedNintendoAccountInfo*, nn::account::UserHandle co... */
PATCH_ANSWER(_ZN2nn7account29LoadCachedNintendoAccountInfoEPNS0_25CachedNintendoAccountInfoERKNS0_10UserHandleEPvm, 0x7C7C,
             "an account error (module 124): no Nintendo Account is linked; the game reads the info only on success");

/* ---- nn::aoc */
/* nn::aoc::CountAddOnContent() */
PATCH_ANSWER(_ZN2nn3aoc17CountAddOnContentEv, 0,
             "0: no add-on content");
/* nn::aoc::ListAddOnContent(int*, int, int) */
PATCH_ANSWER(_ZN2nn3aoc16ListAddOnContentEPiii, 0,
             "0 listed, the list left as it was: no add-on content (CountAddOnContent answers 0)");

/* ---- nn::audio */
/* nn::audio::AcquireAudioOutputDeviceNotification(nn::os::SystemEvent*) */
PATCH_ANSWER(_ZN2nn5audio36AcquireAudioOutputDeviceNotificationEPNS_2os11SystemEventE, 0,
             "0 (success), the event left as it was: the output device never changes");
/* nn::audio::AddBiquadFilter(nn::audio::AudioRendererConfig*, nn::audio::BiquadFilterType*, nn::audio::FinalM... */
PATCH_ANSWER(_ZN2nn5audio15AddBiquadFilterEPNS0_19AudioRendererConfigEPNS0_16BiquadFilterTypeEPNS0_12FinalMixTypeE, 0,
             "0 (success): the effect is never applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::AddBiquadFilter(nn::audio::AudioRendererConfig*, nn::audio::BiquadFilterType*, nn::audio::SubMixType*) */
PATCH_ANSWER(_ZN2nn5audio15AddBiquadFilterEPNS0_19AudioRendererConfigEPNS0_16BiquadFilterTypeEPNS0_10SubMixTypeE, 0,
             "0 (success): the filter is never applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::AddBufferMixer(nn::audio::AudioRendererConfig*, nn::audio::BufferMixerType*, nn::audio::FinalMix... */
PATCH_ANSWER(_ZN2nn5audio14AddBufferMixerEPNS0_19AudioRendererConfigEPNS0_15BufferMixerTypeEPNS0_12FinalMixTypeE, 0,
             "0 (success): the effect is never applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::AddBufferMixer(nn::audio::AudioRendererConfig*, nn::audio::BufferMixerType*, nn::audio::SubMixTy... */
PATCH_ANSWER(_ZN2nn5audio14AddBufferMixerEPNS0_19AudioRendererConfigEPNS0_15BufferMixerTypeEPNS0_10SubMixTypeE, 0,
             "0 (success): the effect is never applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::AddCompressor(nn::audio::AudioRendererConfig*, nn::audio::CompressorType*, nn::audio::FinalMixTy... */
PATCH_ANSWER(_ZN2nn5audio13AddCompressorEPNS0_19AudioRendererConfigEPNS0_14CompressorTypeEPNS0_12FinalMixTypeEi, 0,
             "0 (success): the effect is never applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::AddCompressor(nn::audio::AudioRendererConfig*, nn::audio::CompressorType*, nn::audio::SubMixType... */
PATCH_ANSWER(_ZN2nn5audio13AddCompressorEPNS0_19AudioRendererConfigEPNS0_14CompressorTypeEPNS0_10SubMixTypeEi, 0,
             "0 (success): the effect is never applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::AddDelay(nn::audio::AudioRendererConfig*, nn::audio::DelayType*, void*, unsigned long, nn::audio... */
PATCH_ANSWER(_ZN2nn5audio8AddDelayEPNS0_19AudioRendererConfigEPNS0_9DelayTypeEPvmPNS0_12FinalMixTypeENS_8TimeSpanEi, 0,
             "0 (success): the effect is never applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::AddDelay(nn::audio::AudioRendererConfig*, nn::audio::DelayType*, void*, unsigned long, nn::audio::S...) */
PATCH_ANSWER(_ZN2nn5audio8AddDelayEPNS0_19AudioRendererConfigEPNS0_9DelayTypeEPvmPNS0_10SubMixTypeENS_8TimeSpanEi, 0,
             "0 (success): the delay is never applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::AddLightLimiter(nn::audio::AudioRendererConfig*, nn::audio::LightLimiterType*, void*, unsigned l... */
PATCH_ANSWER(_ZN2nn5audio15AddLightLimiterEPNS0_19AudioRendererConfigEPNS0_16LightLimiterTypeEPvmPNS0_10SubMixTypeENS_8TimeSpanEi, 0,
             "0 (success): the effect is never applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::AddLightLimiter(nn::audio::AudioRendererConfig*, nn::audio::LightLimiterType*, void*, unsigned l... */
PATCH_ANSWER(_ZN2nn5audio15AddLightLimiterEPNS0_19AudioRendererConfigEPNS0_16LightLimiterTypeEPvmPNS0_12FinalMixTypeENS_8TimeSpanEi, 0,
             "0 (success): the effect is never applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::AddReverb(nn::audio::AudioRendererConfig*, nn::audio::ReverbType*, void*, unsigned long, nn::aud... */
PATCH_ANSWER(_ZN2nn5audio9AddReverbEPNS0_19AudioRendererConfigEPNS0_10ReverbTypeEPvmPNS0_12FinalMixTypeEi, 0,
             "0 (success): the effect is never applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::AddReverb(nn::audio::AudioRendererConfig*, nn::audio::ReverbType*, void*, unsigned long, nn::audio:...) */
PATCH_ANSWER(_ZN2nn5audio9AddReverbEPNS0_19AudioRendererConfigEPNS0_10ReverbTypeEPvmPNS0_10SubMixTypeEi, 0,
             "0 (success): the reverb is never applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::GetAudioDeviceOutputVolume(nn::audio::AudioDeviceName const*) */
PATCH_ANSWER(_ZN2nn5audio26GetAudioDeviceOutputVolumeEPKNS0_15AudioDeviceNameE, 0,
             "x0 only: its float result (s0) keeps what was there (as before); the volume is the host's");
/* nn::audio::GetAudioRendererVoiceDropParameter(nn::audio::AudioRendererHandle) */
PATCH_ANSWER(_ZN2nn5audio34GetAudioRendererVoiceDropParameterENS0_19AudioRendererHandleE, 0,
             "x0 only: its float result (s0) keeps what was there (as before); the renderer drops no voices");
/* nn::audio::GetCompressorInputOutput(nn::audio::CompressorType const*, signed char*, signed char*, int*, int) */
PATCH_ANSWER(_ZN2nn5audio24GetCompressorInputOutputEPKNS0_14CompressorTypeEPaS4_Pii, 0,
             "0, outputs unwritten: the effect is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::GetDelayTime(nn::audio::DelayType const*) */
PATCH_ANSWER(_ZN2nn5audio12GetDelayTimeEPKNS0_9DelayTypeE, 0,
             "0 ns: the delay is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::GetLightLimiterInputOutput(nn::audio::LightLimiterType const*, signed char*, signed char*, int*,... */
PATCH_ANSWER(_ZN2nn5audio26GetLightLimiterInputOutputEPKNS0_16LightLimiterTypeEPaS4_Pii, 0,
             "0, outputs unwritten: the effect is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::GetLightLimiterLookAheadTime(nn::audio::LightLimiterType const*) */
PATCH_ANSWER(_ZN2nn5audio28GetLightLimiterLookAheadTimeEPKNS0_16LightLimiterTypeE, 0,
             "0: the effect is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::GetReverbInputOutput(nn::audio::ReverbType const*, signed char*, signed char*, int*, int) */
PATCH_ANSWER(_ZN2nn5audio20GetReverbInputOutputEPKNS0_10ReverbTypeEPaS4_Pii, 0,
             "0, outputs unwritten: the reverb is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::IsBiquadFilterEnabled(nn::audio::BiquadFilterType const*) */
PATCH_ANSWER(_ZN2nn5audio21IsBiquadFilterEnabledEPKNS0_16BiquadFilterTypeE, 0,
             "0 (false): the effect is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::IsBufferMixerEnabled(nn::audio::BufferMixerType const*) */
PATCH_ANSWER(_ZN2nn5audio20IsBufferMixerEnabledEPKNS0_15BufferMixerTypeE, 0,
             "0 (false): the effect is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::IsCompressorEnabled(nn::audio::CompressorType const*) */
PATCH_ANSWER(_ZN2nn5audio19IsCompressorEnabledEPKNS0_14CompressorTypeE, 0,
             "0 (false): the effect is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::IsCompressorRemovable(nn::audio::CompressorType const*) */
PATCH_ANSWER(_ZN2nn5audio21IsCompressorRemovableEPKNS0_14CompressorTypeE, 1,
             "1 (true): nothing to wait for: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::IsDelayEnabled(nn::audio::DelayType const*) */
PATCH_ANSWER(_ZN2nn5audio14IsDelayEnabledEPKNS0_9DelayTypeE, 0,
             "0 (false): the delay is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::IsDelayRemovable(nn::audio::DelayType const*) */
PATCH_ANSWER(_ZN2nn5audio16IsDelayRemovableEPKNS0_9DelayTypeE, 1,
             "1 (true): nothing to wait for: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::IsDeviceOutputVolumeAutoTuneModeEnabled() */
PATCH_ANSWER(_ZN2nn5audio39IsDeviceOutputVolumeAutoTuneModeEnabledEv, 0,
             "0 (false): never enabled");
/* nn::audio::IsReverbEnabled(nn::audio::ReverbType const*) */
PATCH_ANSWER(_ZN2nn5audio15IsReverbEnabledEPKNS0_10ReverbTypeE, 0,
             "0 (false): the reverb is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::RemoveAux(nn::audio::AudioRendererConfig*, nn::audio::AuxType*, nn::audio::FinalMixType*) */
PATCH_ANSWER(_ZN2nn5audio9RemoveAuxEPNS0_19AudioRendererConfigEPNS0_7AuxTypeEPNS0_12FinalMixTypeE, 0,
             "nothing to do: the effect is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::RemoveAux(nn::audio::AudioRendererConfig*, nn::audio::AuxType*, nn::audio::SubMixType*) */
PATCH_ANSWER(_ZN2nn5audio9RemoveAuxEPNS0_19AudioRendererConfigEPNS0_7AuxTypeEPNS0_10SubMixTypeE, 0,
             "nothing to do: the effect is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::RemoveBiquadFilter(nn::audio::AudioRendererConfig*, nn::audio::BiquadFilterType*, nn::audio::Fin... */
PATCH_ANSWER(_ZN2nn5audio18RemoveBiquadFilterEPNS0_19AudioRendererConfigEPNS0_16BiquadFilterTypeEPNS0_12FinalMixTypeE, 0,
             "nothing to do: the effect is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::RemoveBiquadFilter(nn::audio::AudioRendererConfig*, nn::audio::BiquadFilterType*, nn::audio::Sub... */
PATCH_ANSWER(_ZN2nn5audio18RemoveBiquadFilterEPNS0_19AudioRendererConfigEPNS0_16BiquadFilterTypeEPNS0_10SubMixTypeE, 0,
             "nothing to do: the effect is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::RemoveBufferMixer(nn::audio::AudioRendererConfig*, nn::audio::BufferMixerType*, nn::audio::Final... */
PATCH_ANSWER(_ZN2nn5audio17RemoveBufferMixerEPNS0_19AudioRendererConfigEPNS0_15BufferMixerTypeEPNS0_12FinalMixTypeE, 0,
             "nothing to do: the effect is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::RemoveBufferMixer(nn::audio::AudioRendererConfig*, nn::audio::BufferMixerType*, nn::audio::SubMi... */
PATCH_ANSWER(_ZN2nn5audio17RemoveBufferMixerEPNS0_19AudioRendererConfigEPNS0_15BufferMixerTypeEPNS0_10SubMixTypeE, 0,
             "nothing to do: the effect is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::RemoveCompressor(nn::audio::AudioRendererConfig*, nn::audio::CompressorType*, nn::audio::FinalMi... */
PATCH_ANSWER(_ZN2nn5audio16RemoveCompressorEPNS0_19AudioRendererConfigEPNS0_14CompressorTypeEPNS0_12FinalMixTypeE, 0,
             "nothing to do: the effect is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::RemoveCompressor(nn::audio::AudioRendererConfig*, nn::audio::CompressorType*, nn::audio::SubMixT... */
PATCH_ANSWER(_ZN2nn5audio16RemoveCompressorEPNS0_19AudioRendererConfigEPNS0_14CompressorTypeEPNS0_10SubMixTypeE, 0,
             "nothing to do: the effect is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::RemoveDelay(nn::audio::AudioRendererConfig*, nn::audio::DelayType*, nn::audio::FinalMixType*) */
PATCH_ANSWER(_ZN2nn5audio11RemoveDelayEPNS0_19AudioRendererConfigEPNS0_9DelayTypeEPNS0_12FinalMixTypeE, 0,
             "nothing to do: the effect is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::RemoveDelay(nn::audio::AudioRendererConfig*, nn::audio::DelayType*, nn::audio::SubMixType*) */
PATCH_ANSWER(_ZN2nn5audio11RemoveDelayEPNS0_19AudioRendererConfigEPNS0_9DelayTypeEPNS0_10SubMixTypeE, 0,
             "nothing to do: the effect is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::RemoveI3dl2Reverb(nn::audio::AudioRendererConfig*, nn::audio::I3dl2ReverbType*, nn::audio::Final... */
PATCH_ANSWER(_ZN2nn5audio17RemoveI3dl2ReverbEPNS0_19AudioRendererConfigEPNS0_15I3dl2ReverbTypeEPNS0_12FinalMixTypeE, 0,
             "nothing to do: the effect is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::RemoveI3dl2Reverb(nn::audio::AudioRendererConfig*, nn::audio::I3dl2ReverbType*, nn::audio::SubMi... */
PATCH_ANSWER(_ZN2nn5audio17RemoveI3dl2ReverbEPNS0_19AudioRendererConfigEPNS0_15I3dl2ReverbTypeEPNS0_10SubMixTypeE, 0,
             "nothing to do: the effect is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::RemoveLightLimiter(nn::audio::AudioRendererConfig*, nn::audio::LightLimiterType*, nn::audio::Fin... */
PATCH_ANSWER(_ZN2nn5audio18RemoveLightLimiterEPNS0_19AudioRendererConfigEPNS0_16LightLimiterTypeEPNS0_12FinalMixTypeE, 0,
             "nothing to do: the effect is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::RemoveLightLimiter(nn::audio::AudioRendererConfig*, nn::audio::LightLimiterType*, nn::audio::Sub... */
PATCH_ANSWER(_ZN2nn5audio18RemoveLightLimiterEPNS0_19AudioRendererConfigEPNS0_16LightLimiterTypeEPNS0_10SubMixTypeE, 0,
             "nothing to do: the effect is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::RemoveReverb(nn::audio::AudioRendererConfig*, nn::audio::ReverbType*, nn::audio::FinalMixType*) */
PATCH_ANSWER(_ZN2nn5audio12RemoveReverbEPNS0_19AudioRendererConfigEPNS0_10ReverbTypeEPNS0_12FinalMixTypeE, 0,
             "nothing to do: the effect is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::RemoveReverb(nn::audio::AudioRendererConfig*, nn::audio::ReverbType*, nn::audio::SubMixType*) */
PATCH_ANSWER(_ZN2nn5audio12RemoveReverbEPNS0_19AudioRendererConfigEPNS0_10ReverbTypeEPNS0_10SubMixTypeE, 0,
             "nothing to do: the effect is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
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
/* nn::audio::SetBiquadFilterParameter(nn::audio::BiquadFilterType*, nn::audio::TBiquadFilterParameter<short> const&) */
PATCH_ANSWER(_ZN2nn5audio24SetBiquadFilterParameterEPNS0_16BiquadFilterTypeERKNS0_22TBiquadFilterParameterIsEE, 0,
             "nothing to do: the filter is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::SetBufferMixerEnabled(nn::audio::BufferMixerType*, bool) */
PATCH_ANSWER(_ZN2nn5audio21SetBufferMixerEnabledEPNS0_15BufferMixerTypeEb, 0,
             "nothing to do: the effect is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::SetBufferMixerInputOutput(nn::audio::BufferMixerType*, signed char const*, signed char const*, int) */
PATCH_ANSWER(_ZN2nn5audio25SetBufferMixerInputOutputEPNS0_15BufferMixerTypeEPKaS4_i, 0,
             "nothing to do: the effect is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::SetBufferMixerVolume(nn::audio::BufferMixerType*, int, float) */
PATCH_ANSWER(_ZN2nn5audio20SetBufferMixerVolumeEPNS0_15BufferMixerTypeEif, 0,
             "nothing to do: the effect is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::SetCompressorAttackTime(nn::audio::CompressorType*, nn::TimeSpan) */
PATCH_ANSWER(_ZN2nn5audio23SetCompressorAttackTimeEPNS0_14CompressorTypeENS_8TimeSpanE, 0,
             "nothing to do: the effect is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::SetCompressorEnabled(nn::audio::CompressorType*, bool) */
PATCH_ANSWER(_ZN2nn5audio20SetCompressorEnabledEPNS0_14CompressorTypeEb, 0,
             "nothing to do: the effect is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::SetCompressorInputOutput(nn::audio::CompressorType*, signed char const*, signed char const*, int) */
PATCH_ANSWER(_ZN2nn5audio24SetCompressorInputOutputEPNS0_14CompressorTypeEPKaS4_i, 0,
             "nothing to do: the effect is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::SetCompressorMakeupGainEnabled(nn::audio::CompressorType*, bool) */
PATCH_ANSWER(_ZN2nn5audio30SetCompressorMakeupGainEnabledEPNS0_14CompressorTypeEb, 0,
             "nothing to do: the effect is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::SetCompressorOutGain(nn::audio::CompressorType*, float) */
PATCH_ANSWER(_ZN2nn5audio20SetCompressorOutGainEPNS0_14CompressorTypeEf, 0,
             "nothing to do: the effect is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::SetCompressorRatio(nn::audio::CompressorType*, float) */
PATCH_ANSWER(_ZN2nn5audio18SetCompressorRatioEPNS0_14CompressorTypeEf, 0,
             "nothing to do: the effect is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::SetCompressorReleaseTime(nn::audio::CompressorType*, nn::TimeSpan) */
PATCH_ANSWER(_ZN2nn5audio24SetCompressorReleaseTimeEPNS0_14CompressorTypeENS_8TimeSpanE, 0,
             "nothing to do: the effect is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::SetCompressorThreshold(nn::audio::CompressorType*, float) */
PATCH_ANSWER(_ZN2nn5audio22SetCompressorThresholdEPNS0_14CompressorTypeEf, 0,
             "nothing to do: the effect is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
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
/* nn::audio::SetLightLimiterAttackTime(nn::audio::LightLimiterType*, nn::TimeSpan) */
PATCH_ANSWER(_ZN2nn5audio25SetLightLimiterAttackTimeEPNS0_16LightLimiterTypeENS_8TimeSpanE, 0,
             "nothing to do: the effect is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::SetLightLimiterInputOutput(nn::audio::LightLimiterType*, signed char const*, signed char const*,... */
PATCH_ANSWER(_ZN2nn5audio26SetLightLimiterInputOutputEPNS0_16LightLimiterTypeEPKaS4_i, 0,
             "nothing to do: the effect is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::SetLightLimiterLookAheadTime(nn::audio::LightLimiterType*, nn::TimeSpan) */
PATCH_ANSWER(_ZN2nn5audio28SetLightLimiterLookAheadTimeEPNS0_16LightLimiterTypeENS_8TimeSpanE, 0,
             "nothing to do: the effect is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::SetLightLimiterProcessingMode(nn::audio::LightLimiterType*, nn::audio::LightLimiterType::Process... */
PATCH_ANSWER(_ZN2nn5audio29SetLightLimiterProcessingModeEPNS0_16LightLimiterTypeENS1_14ProcessingModeE, 0,
             "nothing to do: the effect is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
/* nn::audio::SetLightLimiterReleaseTime(nn::audio::LightLimiterType*, nn::TimeSpan) */
PATCH_ANSWER(_ZN2nn5audio26SetLightLimiterReleaseTimeEPNS0_16LightLimiterTypeENS_8TimeSpanE, 0,
             "nothing to do: the effect is not applied: the host's mixer has no effects (runtime/nn_audio.c)");
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

/* ---- nn::bcat */
/* nn::bcat::CancelSyncDeliveryCacheRequest() */
PATCH_ANSWER(_ZN2nn4bcat30CancelSyncDeliveryCacheRequestEv, 0,
             "nothing to do: no delivery is ever requested: the host has no network");
/* nn::bcat::Initialize() */
PATCH_ANSWER(_ZN2nn4bcat10InitializeEv, 0,
             "nothing to do: the delivery cache is never mounted");
/* nn::bcat::MountDeliveryCacheStorage() */
PATCH_ANSWER(_ZN2nn4bcat25MountDeliveryCacheStorageEv, 0x47a,
             "ResultNotFound (module 122, description 2): no delivery cache; the game retries only on TargetLocked (3)");
/* nn::bcat::UnmountDeliveryCacheStorage() */
PATCH_ANSWER(_ZN2nn4bcat27UnmountDeliveryCacheStorageEv, 0,
             "nothing to do: the delivery cache is never mounted");

/* ---- nn::friends (the friend list and play history, from the Nintendo Account's online services) */
/* nn::friends::Initialize() */
PATCH_ANSWER(_ZN2nn7friends10InitializeEv, 0,
             "nothing to do: every query answers that no network service account is linked");
/* nn::friends::SetAllocator(void* (*)(unsigned long), void (*)(void*, unsigned long)) */
PATCH_ANSWER(_ZN2nn7friends12SetAllocatorEPFPvmEPFvS1_mE, 0,
             "nothing to do: the host allocates nothing for the friends library");
/* nn::friends::CheckFriendListAvailability(bool*, nn::account::Uid const&) */
PATCH_ANSWER(_ZN2nn7friends27CheckFriendListAvailabilityEPbRKNS_7account3UidE, 0x7C7C,
             "an account error (module 124), the flag left as it was: no network service account; the game reads the flag only on success");
/* nn::friends::GetPlayHistoryRegistrationKey(nn::friends::PlayHistoryRegistrationKey*, nn::account::Uid const&, bool) */
PATCH_ANSWER(_ZN2nn7friends29GetPlayHistoryRegistrationKeyEPNS0_26PlayHistoryRegistrationKeyERKNS_7account3UidEb, 0x7C7C,
             "an account error (module 124), the key left as it was: no network service account; the game keeps a key only on success");

/* ---- nn::hid */
/* nn::hid::FinalizeJcms() */
PATCH_ANSWER(_ZN2nn3hid12FinalizeJcmsEv, 0,
             "nothing to do: the host's controllers have no mouse sensor (Joy-Con 2 as a mouse)");
/* nn::hid::GetJcmsHandle(unsigned int const&, nn::hid::JcmsType const&) */
PATCH_ANSWER(_ZN2nn3hid13GetJcmsHandleERKjRKNS0_8JcmsTypeE, 0,
             "handle 0: the host's controllers have no mouse sensor; the handle is only passed back");
/* nn::hid::GetJcmsStates(nn::hid::JcmsState*, int, nn::hid::JcmsHandle const&) */
PATCH_ANSWER(_ZN2nn3hid13GetJcmsStatesEPNS0_9JcmsStateEiRKNS0_10JcmsHandleE, 0,
             "0 states, the states left as they were: no mouse sensor");
/* nn::hid::InitializeJcms(void*, unsigned long) */
PATCH_ANSWER(_ZN2nn3hid14InitializeJcmsEPvm, 0,
             "nothing to do: the host's controllers have no mouse sensor");
/* nn::hid::MergeSingleJoyAsDualJoy(unsigned int const&, unsigned int const&) */
PATCH_ANSWER(_ZN2nn3hid23MergeSingleJoyAsDualJoyERKjS2_, 0,
             "0 (success): the host's controller is one full-key pad whatever the assignment (runtime/nn_hid.c)");
/* nn::hid::StartJcms(nn::hid::JcmsHandle const&) */
PATCH_ANSWER(_ZN2nn3hid9StartJcmsERKNS0_10JcmsHandleE, 0,
             "0 (success): no mouse sensor; GetJcmsStates reports none");
/* nn::hid::StopJcms(nn::hid::JcmsHandle const&) */
PATCH_ANSWER(_ZN2nn3hid8StopJcmsERKNS0_10JcmsHandleE, 0,
             "0 (success): no mouse sensor");
/* nn::hid::SetNpadJoyAssignmentModeDual(unsigned int const&) */
PATCH_ANSWER(_ZN2nn3hid28SetNpadJoyAssignmentModeDualERKj, 0,
             "nothing to do: the host's controller is one full-key pad whatever the assignment (runtime/nn_hid.c)");
/* nn::hid::SetNpadJoyAssignmentModeSingle(unsigned int const&, nn::hid::NpadJoyDeviceType) */
PATCH_ANSWER(_ZN2nn3hid30SetNpadJoyAssignmentModeSingleERKjNS0_17NpadJoyDeviceTypeE, 0,
             "nothing to do: the host's controller is one full-key pad whatever the assignment (runtime/nn_hid.c)");
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

/* ---- nn::streamplay (GameShare: the console streams the game to other consoles) */
/* nn::streamplay::GetRequiredMemorySize(nn::streamplay::StreamPlayType, nn::streamplay::StreamingMode, int) */
PATCH_ANSWER(_ZN2nn10streamplay21GetRequiredMemorySizeENS0_14StreamPlayTypeENS0_13StreamingModeEi, 0x1000,
             "a page: the game reserves the larger of two sizes at boot; the host never streams, so it stays unused");

/* ---- nn::swkbd (the software keyboard applet) */
/* nn::swkbd::GetRequiredWorkBufferSize(bool) */
PATCH_ANSWER(_ZN2nn5swkbd25GetRequiredWorkBufferSizeEb, 0xD000,
             "the SDK's work buffer size: the game allocates it at boot; the keyboard is shown only when a name is typed");
/* nn::swkbd::GetRequiredStringBufferSize() */
PATCH_ANSWER(_ZN2nn5swkbd27GetRequiredStringBufferSizeEv, 0x7D4,
             "the SDK's string buffer size (500 UTF-16 characters and a header): an allocation size only");
/* nn::swkbd::GetRequiredTextCheckWorkBufferSize() */
PATCH_ANSWER(_ZN2nn5swkbd34GetRequiredTextCheckWorkBufferSizeEv, 0x7D4,
             "the SDK's text check buffer size: an allocation size only");

/* ---- nn::vi */
/* nn::vi::QueryDisplayCurrentRefreshRate(nn::vi::RefreshRate*, bool*, nn::vi::Display const*) */
PATCH_ANSWER(_ZN2nn2vi30QueryDisplayCurrentRefreshRateEPNS0_11RefreshRateEPbPKNS0_7DisplayE, 0,
             "0 (success), the outputs left as they were: the host paces presents (runtime/nn_vi.c)", sdk(22));
/* nn::vi::SetLayerNextHdrContentGamutMetadata(nn::vi::Layer*, nn::vi::HdrContentGamut) */
PATCH_ANSWER(_ZN2nn2vi35SetLayerNextHdrContentGamutMetadataEPNS0_5LayerENS0_15HdrContentGamutE, 0,
             "0 (success): the host presents SDR; HDR metadata has no consumer");
/* nn::vi::SetLayerNextHdrExtendedContentLuminanceRangeMetadata(nn::vi::Layer*, unsigned short, unsigned short) */
PATCH_ANSWER(_ZN2nn2vi52SetLayerNextHdrExtendedContentLuminanceRangeMetadataEPNS0_5LayerEtt, 0,
             "0 (success): the host presents SDR; HDR metadata has no consumer");
/* nn::vi::SetLayerNextHdrStandardContentLuminanceRangeMetadata(nn::vi::Layer*, unsigned short, unsigned short) */
PATCH_ANSWER(_ZN2nn2vi52SetLayerNextHdrStandardContentLuminanceRangeMetadataEPNS0_5LayerEtt, 0,
             "0 (success): the host presents SDR; HDR metadata has no consumer");
/* nn::vi::SetLayerNextHdrToSdrToneMappingMetadata(nn::vi::Layer*, nn::vi::HdrToneMapping) */
PATCH_ANSWER(_ZN2nn2vi39SetLayerNextHdrToSdrToneMappingMetadataEPNS0_5LayerENS0_14HdrToneMappingE, 0,
             "0 (success): the host presents SDR; HDR metadata has no consumer");
/* nn::vi::SetDisplay120HzRefreshRateCapability(nn::vi::Display*, bool) */
PATCH_ANSWER(_ZN2nn2vi36SetDisplay120HzRefreshRateCapabilityEPNS0_7DisplayEb, 0,
             "0 (success): the host paces presents itself (runtime/nn_vi.c)");
