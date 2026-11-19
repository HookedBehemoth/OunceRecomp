// The host's upscale (gpu_upscale, renderer/api/vk_list_api.h): one compute dispatch recorded into a list where the game's
// own upscaler would run, with the filter as a parameter (shaders/upscale.comp). It reads the source through the pools'
// sampled 2D entry and writes the output through their storage 2D entry, so the game's texture and image ids are all it
// needs. Its rendering is ended first; the colour's writes (a rendering's attachments or a dispatch) are made visible to
// it, and its writes to the work after it as a dispatch's are (kAfterDispatch). Another upscaler records here the same way;
// a temporal one (FSR3, XeSS, DLSS) needs inputs the call does not carry yet: depth, motion vectors and the jitter.
#pragma once

namespace hsd {

/// (after vkCreateDevice, the pools' set made) the shader object
void upscale_device_created();

}  // namespace hsd
