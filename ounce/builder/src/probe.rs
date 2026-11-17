//! `hookshot-builder probe`: whether this PC's GPU runs the direct back end. Loads libvulkan, picks the device the
//! back end picks (gpu/direct/direct_main.cpp: the first discrete GPU of Vulkan 1.3, else the first of 1.3) and checks
//! what its device creation requires. One line per item, for a front end:
//!
//! `@@probe <key> <pass|fail|warn|info> <value>`, then `@@probe verdict <pass|fail> <failed keys>`.

use std::ffi::{c_char, c_void, CStr};
use std::ptr::{null, null_mut};

type Fp = unsafe extern "C" fn();
type GetProc = unsafe extern "C" fn(*mut c_void, *const c_char) -> Option<Fp>;

const API_1_3: u32 = (1 << 22) | (3 << 12);
const API_1_4: u32 = (1 << 22) | (4 << 12);
const GB: u64 = 1 << 30;
/// below this the game does not keep its textures; from `RECOMMENDED` it has room for its caches
const MIN_VRAM: u64 = 8 * GB;
const RECOMMENDED_VRAM: u64 = 10 * GB;

// (the structure types: vulkan_core.h)
const ST_APP_INFO: u32 = 0;
const ST_INSTANCE_CI: u32 = 1;
const ST_FEATURES2: u32 = 1000059000;
const ST_PROPERTIES2: u32 = 1000059001;
const ST_DRIVER_PROPS: u32 = 1000196000;
const ST_V12_FEATURES: u32 = 51;
const ST_V13_FEATURES: u32 = 53;
const ST_SHADER_OBJECT: u32 = 1000482000;
const ST_DESCRIPTOR_HEAP: u32 = 1000135009;
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
    // SAFETY: dlopen and dlsym with NUL-terminated names; the symbol is the loader's vkGetInstanceProcAddr
    let lib = unsafe { libc::dlopen(c"libvulkan.so.1".as_ptr(), libc::RTLD_NOW | libc::RTLD_LOCAL) };
    if lib.is_null() {
        out.push(item("vulkan", false, "libvulkan.so.1 is not installed"));
        return out;
    }
    // SAFETY: as above
    let sym = unsafe { libc::dlsym(lib, c"vkGetInstanceProcAddr".as_ptr()) };
    if sym.is_null() {
        out.push(item("vulkan", false, "libvulkan.so.1 has no vkGetInstanceProcAddr"));
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
        let app = AppInfo { s_type: ST_APP_INFO, next: null(), app: c"hookshot-probe".as_ptr(), app_version: 0, engine: null(), engine_version: 0, api: API_1_4 };
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
            state: if vram < MIN_VRAM - GB / 2 { "fail" } else if vram < RECOMMENDED_VRAM - GB / 2 { "warn" } else { "pass" },
            value: format!("{gb} (8 GB needed, 10 to 12 GB recommended)"),
        });
        // the extensions
        let mut ne = 0u32;
        exts(d, null(), &mut ne, null_mut());
        let mut eb = vec![0u8; 260 * ne as usize];
        exts(d, null(), &mut ne, eb.as_mut_ptr());
        let names: Vec<String> = eb.chunks_exact(260).map(|e| cstr(&e[..256])).collect();
        let has = |n: &str| names.iter().any(|e| e == n);
        for (key, ext) in [
            ("swapchain", "VK_KHR_swapchain"),
            ("external_memory_fd", "VK_KHR_external_memory_fd"),
            ("dma_buf", "VK_EXT_external_memory_dma_buf"),
            ("shader_object", "VK_EXT_shader_object"),
            ("descriptor_heap", "VK_EXT_descriptor_heap"),
            ("extended_dynamic_state3", "VK_EXT_extended_dynamic_state3"),
            ("maintenance7", "VK_KHR_maintenance7"),
        ] {
            out.push(item(key, has(ext), if has(ext) { ext.to_string() } else { format!("{ext} missing") }));
        }
        // the features the back end turns on and needs
        let mut f12 = Feat::<47>::new(ST_V12_FEATURES);
        let mut f13 = Feat::<15>::new(ST_V13_FEATURES);
        let mut so = Feat::<1>::new(ST_SHADER_OBJECT);
        let mut dh = Feat::<2>::new(ST_DESCRIPTOR_HEAP);
        let mut e3 = Feat::<31>::new(ST_EDS3);
        let mut m7 = Feat::<1>::new(ST_MAINTENANCE7);
        f12.next = (&mut f13 as *mut Feat<15>).cast();
        let mut tail: *mut *mut c_void = &mut f13.next;
        for (ext, s) in [
            ("VK_EXT_shader_object", (&mut so as *mut Feat<1>).cast::<c_void>()),
            ("VK_EXT_descriptor_heap", (&mut dh as *mut Feat<2>).cast()),
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
        if has("VK_EXT_descriptor_heap") {
            out.push(item("descriptor_heap_feature", dh.on[0] != 0, "descriptorHeap"));
        }
        if has("VK_EXT_extended_dynamic_state3") {
            // (direct_heap.cpp's eds3_ok: polygon mode, depth clamp, blend enable, equation, write mask, alpha to
            // coverage, samples, sample mask, alpha to one, logic op)
            let want = [(2, "PolygonMode"), (1, "DepthClampEnable"), (8, "ColorBlendEnable"), (9, "ColorBlendEquation"), (10, "ColorWriteMask"), (5, "AlphaToCoverageEnable"), (3, "RasterizationSamples"), (4, "SampleMask"), (6, "AlphaToOneEnable"), (7, "LogicOpEnable")];
            let missing: Vec<&str> = want.iter().filter(|w| e3.on[w.0] == 0).map(|w| w.1).collect();
            out.push(item("extended_dynamic_state3_features", missing.is_empty(), if missing.is_empty() { "the ten the back end sets".into() } else { format!("missing {}", missing.join(" ")) }));
        }
        if has("VK_KHR_maintenance7") {
            out.push(item("maintenance7_feature", m7.on[0] != 0, "maintenance7"));
        }
        // the queues: graphics and compute in one family; a sparse-binding family without either
        let mut nq = 0u32;
        queues(d, &mut nq, null_mut());
        let mut qb = vec![0u8; 24 * nq as usize];
        queues(d, &mut nq, qb.as_mut_ptr());
        let flags: Vec<u32> = qb.chunks_exact(24).map(|q| u32::from_le_bytes(q[..4].try_into().unwrap())).collect();
        out.push(item("graphics_queue", flags.iter().any(|f| f & 3 == 3), "a graphics and compute family"));
        let sparse = flags.iter().any(|f| f & 8 != 0 && f & 3 == 0);
        out.push(item("sparse_queue", sparse, if sparse { "a sparse-binding family" } else { "no sparse-binding family without graphics and compute" }));
        if let Some(f) = destroy {
            f(vk.instance, null());
        }
    }
}

/// /dev/udmabuf, through which the back end imports the game's memory as dma-bufs
fn udmabuf() -> Item {
    let dev = std::fs::OpenOptions::new().read(true).write(true).open("/dev/udmabuf");
    let limit = std::fs::read_to_string("/sys/module/udmabuf/parameters/size_limit_mb").ok().and_then(|s| s.trim().parse::<u64>().ok());
    match (dev, limit) {
        (Ok(_), Some(mb)) => item("udmabuf", true, format!("/dev/udmabuf, {mb} MB per buffer")),
        (Err(e), _) => item("udmabuf", false, format!("/dev/udmabuf cannot be opened: {e}")),
        (Ok(_), None) => item("udmabuf", false, "udmabuf's size_limit_mb cannot be read"),
    }
}

pub fn command() {
    let mut items = checks();
    items.push(udmabuf());
    let failed: Vec<&str> = items.iter().filter(|i| i.state == "fail").map(|i| i.key).collect();
    for i in &items {
        println!("@@probe {} {} {}", i.key, i.state, i.value);
    }
    println!("@@probe verdict {} {}", if failed.is_empty() { "pass" } else { "fail" }, failed.join(" "));
    std::process::exit(i32::from(!failed.is_empty()));
}
