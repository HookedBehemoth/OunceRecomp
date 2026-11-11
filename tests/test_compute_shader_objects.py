"""Compute programs as shader objects (RX_VK_SHADER_OBJECTS=1, gpu/vk/vk_shader_object.cpp).

A dispatch binds its program's compute VkShaderEXT, made from the driver's binary an earlier run kept, and takes the
compute pipeline only when shader objects are off or the driver refused the stage. The mesh fallback's task and mesh
dispatches do the same, except under the task/mesh fallback, whose pipelines are built with DISABLE_OPTIMIZATION (a
shader object cannot ask for that; optimized, they hung an RX 6950 XT). The compute variants made are noted in the
binary cache under a tag of their own, and the prefetch thread makes them all from their binaries when it starts.

python -I -B tests/test_compute_shader_objects.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"


def block(text, marker):
    start = text.index(marker)
    depth, i = 0, text.index("{", start)
    while True:
        c = text[i]
        depth += c == "{"
        depth -= c == "}"
        i += 1
        if not depth:
            return text[start:i]


def main():
    so = (VK / "vk_shader_object.cpp").read_text(encoding="utf-8")
    draw = (VK / "vk_draw.cpp").read_text(encoding="utf-8")

    stage = block(so, "bool so_stage(const Program& p, const Stage& st) {")
    if "p.compute && so_compute_on()" not in stage or "p.fallback && so_fallback_on()" not in stage:
        raise SystemExit("compute and fallback stages are not made as shader objects")
    if "VK_SHADER_CREATE_REQUIRE_FULL_SUBGROUPS_BIT_EXT" not in so or "full_subgroups(p, st) ? 2 : 0" not in so:
        raise SystemExit("a compute shader object does not ask for full subgroups as its pipeline does (or its binary key misses it)")
    fallback = block(so, "bool so_fallback_on() {")
    if "!dev.task_mesh_fallback" not in fallback:
        raise SystemExit("the fallback's dispatches are optimized shader objects under the task/mesh fallback (GPU hang on RDNA2)")
    if "RX_VK_NOOPT" not in block(so, "bool so_compute_on() {"):
        raise SystemExit("RX_VK_NOOPT, which a shader object cannot express, does not keep the compute pipelines")
    if "kTagCompute" not in block(so, "void cache_init_locked() {"):
        raise SystemExit("the compute programs noted in the binary cache are not read back")
    if "prefetch_computes();" not in block(so, "void prefetch_thread() {"):
        raise SystemExit("the prefetch does not make the kept compute programs when it starts")
    if "so_make(*so, *p, so_push_size(), false)" not in block(so, "void prefetch_computes() {"):
        raise SystemExit("the compute prefetch compiles on its one thread instead of leaving a missing binary to the dispatch")

    # (the dispatch: the shader object when made, else the pipeline, never neither)
    if "SoProg* cso = nullptr;" not in draw or "vkCmdBindShadersEXT(cmd, 1, &stage, &cso->cs[0]);" not in draw:
        raise SystemExit("a dispatch does not bind its compute shader object")
    if "if (!cso && !p.compute_pipeline) SKIP_HERE();" not in draw or \
            "vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.compute_pipeline);" not in draw:
        raise SystemExit("a dispatch whose shader object was not made does not take its pipeline")
    if "its pipeline instead" not in draw:
        raise SystemExit("a compute shader object refused is not logged")
    record = block(draw, "bool fallback_record(VkCommandBuffer cmd, Program& p, const DrawArgs& d) {")
    if "(!so && !fallback_pipelines_ready(p))" not in record or "vkCmdBindShadersEXT(cmd, 1, &cs, &so->cs[stage]);" not in record:
        raise SystemExit("the fallback's dispatches do not bind their shader objects, or wait for pipelines they do not use")
    if "fallback_prefetch(const_cast<Program&>(*p))" not in so:
        raise SystemExit("the fallback programs' pipelines are not begun at the prefetch (left out at their first draws)")
    print("compute shader objects ok")


if __name__ == "__main__":
    main()
