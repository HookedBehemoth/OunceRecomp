//! `ounce probe`: whether this PC's GPU runs the direct back end. Loads libvulkan, picks the device the
//! back end picks (renderer/vk_main.cpp: the first discrete GPU of Vulkan 1.3, else the first of 1.3) and checks
//! what it needs. An item fails only when the back end refuses to start without it, or starts and draws nothing;
//! what it starts without and works around is a warning. One line per item, for a front end:
//!
//! `@@probe <key> <pass|fail|warn|info> <value>`, then `@@probe verdict <pass|fail> <failed keys>`.

use std::ffi::{c_char, c_void, CStr};
use std::ptr::{null, null_mut};

type Fp = unsafe extern "C" fn();
type GetProc = unsafe extern "C" fn(*mut c_void, *const c_char) -> Option<Fp>;

const API_1_3: u32 = (1 << 22) | (3 << 12);
const API_1_4: u32 = (1 << 22) | (4 << 12);
const GB: u64 = 1 << 30;
/// below this the game may not keep its textures (the back end does not check); from `RECOMMENDED` it has room for
/// its caches
const MIN_VRAM: u64 = 8 * GB;
const RECOMMENDED_VRAM: u64 = 10 * GB;
/// the push data a draw writes at most (vk_pool.cpp kPushBytes: the uniform table's offset and its slots)
const PUSH_BYTES: u32 = 144;

// (the structure types: vulkan_core.h)
const ST_APP_INFO: u32 = 0;
const ST_INSTANCE_CI: u32 = 1;
const ST_FEATURES2: u32 = 1000059000;
const ST_PROPERTIES2: u32 = 1000059001;
const ST_DRIVER_PROPS: u32 = 1000196000;
const ST_V12_FEATURES: u32 = 51;
const ST_V13_FEATURES: u32 = 53;
const ST_V12_PROPS: u32 = 52;
const ST_SHADER_OBJECT: u32 = 1000482000;
const ST_ROBUSTNESS2: u32 = 1000286000;
const ST_EDS3: u32 = 1000455000;
const ST_MAINTENANCE7: u32 = 1000562000;

/// A feature structure: its type, the chain, and its VkBool32 fields in order.
#[repr(C)]
struct Feat<const N: usize> {
    s_type: u32,
    next: *mut c_void,
    on: [u32; N],
}

impl<const N: usize> Feat<N> {
    fn new(s_type: u32) -> Self {
        Feat { s_type, next: null_mut(), on: [0; N] }
    }
}

#[repr(C)]
struct AppInfo {
    s_type: u32,
    next: *const c_void,
    app: *const c_char,
    app_version: u32,
    engine: *const c_char,
    engine_version: u32,
    api: u32,
}

#[repr(C)]
struct InstanceCi {
    s_type: u32,
    next: *const c_void,
    flags: u32,
    app: *const AppInfo,
    layers: u32,
    layer_names: *const *const c_char,
    exts: u32,
    ext_names: *const *const c_char,
}

/// One requirement's line.
struct Item {
    key: &'static str,
    state: &'static str,
    value: String,
}

fn item(key: &'static str, ok: bool, value: impl Into<String>) -> Item {
    Item { key, state: if ok { "pass" } else { "fail" }, value: value.into() }
}

/// what the back end starts without
fn optional(key: &'static str, ok: bool, value: impl Into<String>) -> Item {
    Item { key, state: if ok { "pass" } else { "warn" }, value: value.into() }
}

/// The loader's entry points; `None` when libvulkan is not there.
struct Vk {
    gipa: GetProc,
    instance: *mut c_void,
}

impl Vk {
    unsafe fn f<T>(&self, name: &CStr) -> Option<T> {
        // SAFETY: T is the function pointer type of `name` (the caller's)
        unsafe { (self.gipa)(self.instance, name.as_ptr()).map(|p| std::mem::transmute_copy::<Fp, T>(&p)) }
    }
}

/// A shared library by its system name, opened (dlopen; Windows: LoadLibrary): its handle, or null
#[cfg(unix)]
unsafe fn lib_open(name: &CStr) -> *mut c_void {
    libc::dlopen(name.as_ptr(), libc::RTLD_NOW | libc::RTLD_LOCAL)
}

#[cfg(windows)]
unsafe fn lib_open(name: &CStr) -> *mut c_void {
    windows_sys::Win32::System::LibraryLoader::LoadLibraryA(name.as_ptr() as *const u8) as *mut c_void
}

/// A symbol of a library lib_open opened, or null
#[cfg(unix)]
unsafe fn lib_sym(lib: *mut c_void, name: &CStr) -> *mut c_void {
    libc::dlsym(lib, name.as_ptr())
}

#[cfg(windows)]
unsafe fn lib_sym(lib: *mut c_void, name: &CStr) -> *mut c_void {
    windows_sys::Win32::System::LibraryLoader::GetProcAddress(lib as _, name.as_ptr() as *const u8).map_or(null_mut(), |f| f as *mut c_void)
}

#[cfg(unix)]
unsafe fn lib_close(lib: *mut c_void) {
    libc::dlclose(lib);
}

#[cfg(windows)]
unsafe fn lib_close(lib: *mut c_void) {
    windows_sys::Win32::Foundation::FreeLibrary(lib as _);
}

/// The Vulkan loader's name (the one the back end opens, renderer/vk_main.cpp)
#[cfg(unix)]
const VULKAN: &CStr = c"libvulkan.so.1";
#[cfg(windows)]
const VULKAN: &CStr = c"vulkan-1.dll";

/// SDL3's and SDL2's names (runtime/media/hos_sdl.c's)
#[cfg(unix)]
const SDL_NAMES: [(&CStr, &str); 2] = [(c"libSDL3.so.0", "SDL3"), (c"libSDL2-2.0.so.0", "SDL2")];
#[cfg(windows)]
const SDL_NAMES: [(&CStr, &str); 2] = [(c"SDL3.dll", "SDL3"), (c"SDL2.dll", "SDL2")];

fn cstr(b: &[u8]) -> String {
    let n = b.iter().position(|&c| c == 0).unwrap_or(b.len());
    String::from_utf8_lossy(&b[..n]).into_owned()
}

fn version(v: u32) -> String {
    format!("{}.{}.{}", v >> 22 & 0x7f, v >> 12 & 0x3ff, v & 0xfff)
}

/// Every check, in the order printed.
fn checks() -> Vec<Item> {
    let mut out = Vec::new();
    let loader = VULKAN.to_string_lossy();
    // SAFETY: a library opened and a symbol looked up by NUL-terminated names; the symbol is the loader's
    // vkGetInstanceProcAddr
    let lib = unsafe { lib_open(VULKAN) };
    if lib.is_null() {
        out.push(item("vulkan", false, format!("{loader} is not installed")));
        return out;
    }
    // SAFETY: as above
    let sym = unsafe { lib_sym(lib, c"vkGetInstanceProcAddr") };
    if sym.is_null() {
        out.push(item("vulkan", false, format!("{loader} has no vkGetInstanceProcAddr")));
        return out;
    }
    // SAFETY: the symbol is vkGetInstanceProcAddr
    let mut vk = Vk { gipa: unsafe { std::mem::transmute::<*mut c_void, GetProc>(sym) }, instance: null_mut() };
    // SAFETY: each function is called as vulkan_core.h declares it, with structures laid out as it does
    unsafe { device_checks(&mut vk, &mut out) };
    out
}

unsafe fn device_checks(vk: &mut Vk, out: &mut Vec<Item>) {
    type EnumVersion = unsafe extern "C" fn(*mut u32) -> i32;
    type CreateInstance = unsafe extern "C" fn(*const InstanceCi, *const c_void, *mut *mut c_void) -> i32;
    type DestroyInstance = unsafe extern "C" fn(*mut c_void, *const c_void);
    type EnumDevices = unsafe extern "C" fn(*mut c_void, *mut u32, *mut *mut c_void) -> i32;
    type GetProps = unsafe extern "C" fn(*mut c_void, *mut u8);
    type GetFeatures2 = unsafe extern "C" fn(*mut c_void, *mut c_void);
    type EnumExts = unsafe extern "C" fn(*mut c_void, *const c_char, *mut u32, *mut u8) -> i32;
    type GetQueues = unsafe extern "C" fn(*mut c_void, *mut u32, *mut u8);
    unsafe {
        let mut loader = 1 << 22;
        if let Some(f) = vk.f::<EnumVersion>(c"vkEnumerateInstanceVersion") {
            f(&mut loader);
        }
        out.push(item("loader", loader >= API_1_3, version(loader)));
        let Some(create) = vk.f::<CreateInstance>(c"vkCreateInstance") else {
            out.push(item("vulkan", false, "the loader has no vkCreateInstance"));
            return;
        };
        let app = AppInfo { s_type: ST_APP_INFO, next: null(), app: c"ounce-probe".as_ptr(), app_version: 0, engine: null(), engine_version: 0, api: API_1_4 };
        let ci = InstanceCi { s_type: ST_INSTANCE_CI, next: null(), flags: 0, app: &app, layers: 0, layer_names: null(), exts: 0, ext_names: null() };
        let r = create(&ci, null(), &mut vk.instance);
        if r != 0 {
            out.push(item("vulkan", false, format!("vkCreateInstance failed ({r}): no Vulkan driver")));
            return;
        }
        let destroy = vk.f::<DestroyInstance>(c"vkDestroyInstance");
        let (Some(enum_devs), Some(props), Some(props2), Some(mem), Some(feats2), Some(exts), Some(queues)) = (
            vk.f::<EnumDevices>(c"vkEnumeratePhysicalDevices"),
            vk.f::<GetProps>(c"vkGetPhysicalDeviceProperties"),
            vk.f::<GetProps>(c"vkGetPhysicalDeviceProperties2"),
            vk.f::<GetProps>(c"vkGetPhysicalDeviceMemoryProperties"),
            vk.f::<GetFeatures2>(c"vkGetPhysicalDeviceFeatures2"),
            vk.f::<EnumExts>(c"vkEnumerateDeviceExtensionProperties"),
            vk.f::<GetQueues>(c"vkGetPhysicalDeviceQueueFamilyProperties"),
        ) else {
            out.push(item("vulkan", false, "the instance lacks a Vulkan 1.1 function"));
            return;
        };
        let mut n = 0u32;
        enum_devs(vk.instance, &mut n, null_mut());
        let mut devs = vec![null_mut(); n as usize];
        enum_devs(vk.instance, &mut n, devs.as_mut_ptr());
        // (VkPhysicalDeviceProperties: apiVersion, driverVersion, vendorID, deviceID, deviceType, deviceName[256], ...)
        let mut best: Option<(usize, i32)> = None;
        let mut p = vec![0u8; 1024];
        for (i, &d) in devs.iter().enumerate() {
            props(d, p.as_mut_ptr());
            let api = u32::from_le_bytes(p[0..4].try_into().unwrap());
            let score = if api < API_1_3 { -1 } else if u32::from_le_bytes(p[16..20].try_into().unwrap()) == 2 { 2 } else { 0 };
            out.push(Item { key: "device", state: "info", value: format!("{i} {} (Vulkan {})", cstr(&p[20..276]), version(api)) });
            if best.is_none_or(|b| score > b.1) {
                best = Some((i, score));
            }
        }
        let Some((pick, score)) = best.filter(|b| b.1 >= 0) else {
            out.push(item("gpu", false, "no GPU with Vulkan 1.3"));
            if let Some(f) = destroy {
                f(vk.instance, null());
            }
            return;
        };
        let d = devs[pick];
        props(d, p.as_mut_ptr());
        let api = u32::from_le_bytes(p[0..4].try_into().unwrap());
        out.push(Item { key: "gpu", state: if score == 2 { "pass" } else { "warn" }, value: format!("{}{}", cstr(&p[20..276]), if score == 2 { "" } else { " (not a discrete GPU)" }) });
        out.push(item("api", api >= API_1_3, version(api)));
        // (VkPhysicalDeviceProperties2 with VkPhysicalDeviceDriverProperties: driverID, driverName[256], driverInfo[256])
        let mut drv = vec![0u8; 16 + 4 + 512 + 8];
        drv[0..4].copy_from_slice(&ST_DRIVER_PROPS.to_le_bytes());
        let mut p2 = vec![0u8; 16 + 1024];
        p2[0..4].copy_from_slice(&ST_PROPERTIES2.to_le_bytes());
        p2[8..16].copy_from_slice(&(drv.as_mut_ptr() as usize).to_le_bytes());
        props2(d, p2.as_mut_ptr());
        out.push(Item { key: "driver", state: "info", value: format!("{} {}", cstr(&drv[20..276]), cstr(&drv[276..532])) });
        // (VkPhysicalDeviceMemoryProperties: typeCount, 32 types of 8 bytes, heapCount, 16 heaps of {u64 size, u32 flags})
        let mut m = vec![0u8; 528];
        mem(d, m.as_mut_ptr());
        let heaps = u32::from_le_bytes(m[260..264].try_into().unwrap()) as usize;
        let vram = (0..heaps.min(16))
            .map(|h| (u64::from_le_bytes(m[264 + 16 * h..][..8].try_into().unwrap()), u32::from_le_bytes(m[272 + 16 * h..][..4].try_into().unwrap())))
            .filter(|h| h.1 & 1 != 0)
            .map(|h| h.0)
            .max()
            .unwrap_or(0);
        let gb = format!("{:.1} GB", vram as f64 / GB as f64);
        out.push(Item {
            key: "vram",
            state: if vram < RECOMMENDED_VRAM - GB / 2 { "warn" } else { "pass" },
            value: format!("{gb} ({}10 to 12 GB recommended)", if vram < MIN_VRAM - GB / 2 { "under 8 GB the textures may not fit; " } else { "" }),
        });
        // the extensions
        let mut ne = 0u32;
        exts(d, null(), &mut ne, null_mut());
        let mut eb = vec![0u8; 260 * ne as usize];
        exts(d, null(), &mut ne, eb.as_mut_ptr());
        let names: Vec<String> = eb.chunks_exact(260).map(|e| cstr(&e[..256])).collect();
        let has = |n: &str| names.iter().any(|e| e == n);
        // (the device is not made without any of these)
        for (key, ext) in [
            ("swapchain", "VK_KHR_swapchain"),
            ("shader_object", "VK_EXT_shader_object"),
            ("extended_dynamic_state3", "VK_EXT_extended_dynamic_state3"),
        ] {
            out.push(item(key, has(ext), if has(ext) { ext.to_string() } else { format!("{ext} missing") }));
        }
        // (the game's memory is handed over as dma-bufs, on Windows as host pointers (runtime/os/os_guest.h): without
        // these the back end starts with layer 0 off)
        #[cfg(unix)]
        let memory_exts = [("external_memory_fd", "VK_KHR_external_memory_fd"), ("dma_buf", "VK_EXT_external_memory_dma_buf")];
        #[cfg(windows)]
        let memory_exts = [("external_memory_host", "VK_EXT_external_memory_host")];
        for (key, ext) in memory_exts {
            out.push(optional(key, has(ext), if has(ext) { ext.to_string() } else { format!("{ext} missing: the game's memory is not mapped on the GPU (layer 0 off)") }));
        }
        // (null descriptors: VK_KHR_robustness2, or the same structure as VK_EXT_robustness2)
        let robust2_ext = if has("VK_KHR_robustness2") { Some("VK_KHR_robustness2") } else if has("VK_EXT_robustness2") { Some("VK_EXT_robustness2") } else { None };
        out.push(item("robustness2", robust2_ext.is_some(), robust2_ext.unwrap_or("VK_KHR_robustness2 missing")));
        let m7_ext = has("VK_KHR_maintenance7");
        // the features the back end turns on and needs
        let mut f12 = Feat::<47>::new(ST_V12_FEATURES);
        let mut f13 = Feat::<15>::new(ST_V13_FEATURES);
        let mut so = Feat::<1>::new(ST_SHADER_OBJECT);
        let mut rb = Feat::<3>::new(ST_ROBUSTNESS2);
        let mut e3 = Feat::<31>::new(ST_EDS3);
        let mut m7 = Feat::<1>::new(ST_MAINTENANCE7);
        f12.next = (&mut f13 as *mut Feat<15>).cast();
        let mut tail: *mut *mut c_void = &mut f13.next;
        for (ext, s) in [
            ("VK_EXT_shader_object", (&mut so as *mut Feat<1>).cast::<c_void>()),
            (robust2_ext.unwrap_or(""), (&mut rb as *mut Feat<3>).cast()),
            ("VK_EXT_extended_dynamic_state3", (&mut e3 as *mut Feat<31>).cast()),
            ("VK_KHR_maintenance7", (&mut m7 as *mut Feat<1>).cast()),
        ] {
            if has(ext) {
                *tail = s;
                tail = s.cast::<u8>().add(8).cast();
            }
        }
        // (VkPhysicalDeviceFeatures2: type, chain, then VkPhysicalDeviceFeatures' 55 fields)
        let mut f2 = Feat::<55>::new(ST_FEATURES2);
        f2.next = (&mut f12 as *mut Feat<47>).cast();
        feats2(d, (&mut f2 as *mut Feat<55>).cast());
        let core = [("dynamicRendering", f13.on[12]), ("synchronization2", f13.on[9]), ("timelineSemaphore", f12.on[37]), ("bufferDeviceAddress", f12.on[38])];
        let missing: Vec<&str> = core.iter().filter(|c| c.1 == 0).map(|c| c.0).collect();
        out.push(item("core_features", missing.is_empty(), if missing.is_empty() { "dynamicRendering synchronization2 timelineSemaphore bufferDeviceAddress".into() } else { format!("missing {}", missing.join(" ")) }));
        if has("VK_EXT_shader_object") {
            out.push(item("shader_object_feature", so.on[0] != 0, "shaderObject"));
        }
        if robust2_ext.is_some() {
            out.push(item("robustness2_feature", rb.on[2] != 0, "nullDescriptor"));
        }
        // (the pools' set, vk_pool.cpp draw_features_resolved: descriptor indexing with update after bind; and 64-bit
        // integers for the uniform buffers' addresses)
        let indexing = [(29, "runtimeDescriptorArray"), (14, "shaderSampledImageArrayNonUniformIndexing"), (16, "shaderStorageImageArrayNonUniformIndexing"), (21, "descriptorBindingSampledImageUpdateAfterBind"), (22, "descriptorBindingStorageImageUpdateAfterBind"), (26, "descriptorBindingUpdateUnusedWhilePending")];
        let mut missing: Vec<&str> = indexing.iter().filter(|w| f12.on[w.0] == 0).map(|w| w.1).collect();
        if f2.on[40] == 0 {
            missing.push("shaderInt64");
        }
        out.push(item("descriptor_indexing", missing.is_empty(), if missing.is_empty() { "the six the pools' set needs, shaderInt64".into() } else { format!("missing {}", missing.join(" ")) }));
        // (the pools' set's sizes, vk_pool.cpp: 7 view types x 65,536 sampled and storage images, 2 x 4,096 samplers,
        // updated after bind; VkPhysicalDeviceVulkan12Properties' limits at their offsets)
        let mut p12 = vec![0u8; 800];
        p12[0..4].copy_from_slice(&ST_V12_PROPS.to_le_bytes());
        let mut p2 = vec![0u8; 16 + 1024];
        p2[0..4].copy_from_slice(&ST_PROPERTIES2.to_le_bytes());
        p2[8..16].copy_from_slice(&(p12.as_mut_ptr() as usize).to_le_bytes());
        props2(d, p2.as_mut_ptr());
        let at = |o: usize| u32::from_le_bytes(p12[o..o + 4].try_into().unwrap());
        let (images, samplers) = (7 * 65536u32, 2 * 4096u32);
        let limits = [
            (684, images, "set sampled images"),
            (688, images, "set storage images"),
            (664, samplers, "set samplers"),
            (648, images, "stage sampled images"),
            (652, images, "stage storage images"),
            (636, samplers, "stage samplers"),
            (660, 2 * images + 2 * samplers, "stage resources"),
            (604, 2 * images + 2 * samplers, "descriptors in all pools"),
        ];
        let low: Vec<String> = limits.iter().filter(|l| at(l.0) < l.1).map(|l| format!("{} {} < {}", l.2, at(l.0), l.1)).collect();
        out.push(item("descriptor_limits", low.is_empty(), if low.is_empty() { "update-after-bind: 458,752 images and 8,192 samplers".into() } else { format!("update-after-bind limits too low: {}", low.join(", ")) }));
        // (VkPhysicalDeviceProperties' limits.maxPushConstantsSize; a draw pushes up to 144 bytes)
        let push = u32::from_le_bytes(p[328..332].try_into().unwrap());
        out.push(item("push_constants", push >= PUSH_BYTES, format!("{push} bytes{}", if push >= PUSH_BYTES { "" } else { ", under the 144 a draw pushes" })));
        if has("VK_EXT_extended_dynamic_state3") {
            // (vk_pool.cpp's eds3_ok: polygon mode, depth clamp, blend enable, equation, write mask, alpha to
            // coverage, samples, sample mask, alpha to one, logic op)
            let want = [(2, "PolygonMode"), (1, "DepthClampEnable"), (8, "ColorBlendEnable"), (9, "ColorBlendEquation"), (10, "ColorWriteMask"), (5, "AlphaToCoverageEnable"), (3, "RasterizationSamples"), (4, "SampleMask"), (6, "AlphaToOneEnable"), (7, "LogicOpEnable")];
            let missing: Vec<&str> = want.iter().filter(|w| e3.on[w.0] == 0).map(|w| w.1).collect();
            out.push(item("extended_dynamic_state3_features", missing.is_empty(), if missing.is_empty() { "the ten the back end sets".into() } else { format!("missing {}", missing.join(" ")) }));
        }
        let m7_ok = m7_ext && m7.on[0] != 0;
        out.push(optional("maintenance7", m7_ok, if m7_ok { "VK_KHR_maintenance7" } else { "no VK_KHR_maintenance7: the game's command jobs are not executed" }));
        // (VkPhysicalDeviceFeatures: sparseBinding is field 44, sparseResidencyBuffer 45, sparseResidencyAliased 52)
        let sparse_res = f2.on[44] != 0 && f2.on[45] != 0 && f2.on[52] != 0;
        out.push(optional("sparse_residency", sparse_res, if sparse_res { "aliased sparse buffers" } else { "no aliased sparse buffers: the game's memory is not mapped on the GPU (layer 0 off)" }));
        // the queues: graphics and compute in one family; a sparse-binding family without either
        let mut nq = 0u32;
        queues(d, &mut nq, null_mut());
        let mut qb = vec![0u8; 24 * nq as usize];
        queues(d, &mut nq, qb.as_mut_ptr());
        let flags: Vec<u32> = qb.chunks_exact(24).map(|q| u32::from_le_bytes(q[..4].try_into().unwrap())).collect();
        out.push(item("graphics_queue", flags.iter().any(|f| f & 3 == 3), "a graphics and compute family"));
        let sparse = flags.iter().any(|f| f & 8 != 0 && f & 3 == 0);
        let any_sparse = flags.iter().any(|f| f & 8 != 0);
        out.push(optional("sparse_queue", sparse, if sparse { "a sparse-binding family" } else if any_sparse { "no sparse-binding family of its own: binds on a graphics or compute queue" } else { "no sparse-binding family: the game's memory is not mapped on the GPU (layer 0 off)" }));
        if let Some(f) = destroy {
            f(vk.instance, null());
        }
    }
}

/// what a player does when /dev/udmabuf cannot be opened
#[cfg(unix)]
pub const UDMABUF_FIX: &str = "To fix it: load the module (sudo modprobe udmabuf; to keep it, add udmabuf to /etc/modules-load.d/udmabuf.conf) and let your user open it (the group that owns /dev/udmabuf, often kvm: sudo usermod -aG kvm $USER, then log in again)";

/// /dev/udmabuf, through which the back end imports the game's memory as dma-bufs
#[cfg(windows)]
fn udmabuf() -> Item {
    Item { key: "udmabuf", state: "info", value: "not used on Windows: the game's memory reaches the GPU as host pointers".into() }
}

#[cfg(unix)]
fn udmabuf() -> Item {
    let dev = std::fs::OpenOptions::new().read(true).write(true).open("/dev/udmabuf");
    let limit = std::fs::read_to_string("/sys/module/udmabuf/parameters/size_limit_mb").ok().and_then(|s| s.trim().parse::<u64>().ok());
    match (dev, limit) {
        (Ok(_), Some(mb)) => item("udmabuf", true, format!("/dev/udmabuf, {mb} MB per buffer")),
        (Err(e), _) => optional("udmabuf", false, format!("/dev/udmabuf cannot be opened ({e}): the game's memory is not mapped on the GPU (layer 0 off), which costs speed. {}", UDMABUF_FIX)),
        (Ok(_), None) => optional("udmabuf", false, "udmabuf's size_limit_mb cannot be read; the game's memory is not mapped on the GPU (layer 0 off)"),
    }
}

/// Whether the back end would start on this PC: Err(the failed items) when it would not (no Vulkan, no GPU of 1.3)
pub fn device_ready() -> Result<(), String> {
    let failed: Vec<String> = checks().into_iter().filter(|i| i.state == "fail").map(|i| format!("{}: {}", i.key, i.value)).collect();
    if failed.is_empty() { Ok(()) } else { Err(failed.join("; ")) }
}

/// SDL, which the game loads at run time for its window, sound and controllers (runtime/media/hos_sdl.c's names)
fn sdl() -> Item {
    // (a release puts its own beside the game)
    if ounce_core::games::root().ok().and_then(|r| crate::sdl::prebuilt(&r)).is_some() {
        return item("sdl", true, "SDL 3.4.20, the release's own");
    }
    for (name, what) in SDL_NAMES {
        // SAFETY: a library opened by a NUL-terminated name; the handle is closed at once
        let h = unsafe { lib_open(name) };
        if !h.is_null() {
            unsafe { lib_close(h) };
            return item("sdl", true, what);
        }
    }
    optional("sdl", false, "neither SDL3 nor SDL2 is installed: the game runs without a window, sound or controllers. Install your distribution's SDL3 (or SDL2) package")
}

pub fn command() {
    let mut items = checks();
    items.push(udmabuf());
    items.push(sdl());
    let failed: Vec<&str> = items.iter().filter(|i| i.state == "fail").map(|i| i.key).collect();
    for i in &items {
        println!("@@probe {} {} {}", i.key, i.state, i.value);
    }
    println!("@@probe verdict {} {}", if failed.is_empty() { "pass" } else { "fail" }, failed.join(" "));
    std::process::exit(i32::from(!failed.is_empty()));
}
