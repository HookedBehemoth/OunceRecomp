# The transient pool

This page explains how the game's transient render targets get device memory: one constant allocation in which the
targets alias each other, as they do on the console. Read it before you change `games/oot/patches/rt_slots.c`, the pool code in
`renderer/vk_vma.cpp` and `renderer/vk_image.cpp`, or anything that decides when an image's memory may be
reused. What the targets are is in [render targets](render-targets.md).

## Why one allocation

On the console, every transient target of the game's allocator aliases one memory pool, and a freed target's memory is
some other target's the moment after. On a PC, an image per (address, shape) would grow with every new address a target
lands on, and the game keeps descriptor ids registered for freed targets for a long time. Binding every transient
target into one fixed allocation at its slot's offset gives the console's behavior: the targets cost a constant amount
of VRAM, made once, and nothing is allocated or freed for them in play.

## How it works

1. **The host names the range once.** In direct mode the allocator's slots lie past the end of the game's pool, at
   GPU addresses `runtime/nvn_native.c` leaves unused. The slot range is twice the game's pool, and at least 512 MB
   (`RX_RT_SLOTS_MB` overrides it). The host hands the range to the back end (`gpu_transient_pool`), which allocates it
   with one `vkAllocateMemory`.
2. **Slots are sized by the device's need.** Before placing a target, the host asks the back end how much memory an
   image of that format and shape needs (`gpu_image_need`), and tells it each slot's bytes before a texture is
   initialized there.
3. **An image is bound at its slot's offset.** A texture initialized inside the range gets a `VkImage` with no memory of
   its own, bound in the pool's allocation. Images whose ranges overlap share memory.
4. **The allocator tells the back end who owns a range.** Every entry the game's allocator gives out is told to the back
   end by its own texture object (`gpu_target_given`). The image of that object claims the range.

### Claims and the discard

When an image claims a range another image owned, the back end ends the old ownership and initializes the new image: a
barrier from `UNDEFINED` and a clear to zero. That is the discard Vulkan requires before an alias is used after another
wrote the memory, and it resets the driver's compression metadata. The discard is sent at once, on the claiming
thread's upload buffer.

That is correct only if no unsent list still uses the old owner. The host guarantees it: a slot's memory goes to
another image only 3 presents after its last use (`REUSE_AFTER`), because a list can reach the device up to 2 presents
after it was begun.

### Image objects are kept

A pooled image holds no memory, so it is not destroyed when its texture goes. It stays for the next texture of the same
storage and shape. Past 1,024 objects, the ones longest unclaimed go.

## What the strict events mean

Each of these is a strict event (`HOS_DBG=strict` stops at it) and should count 0:

| Event | Meaning |
|---|---|
| a transient target does not fit its slot | the image needs more than the slot's bytes; it gets the placeholder |
| a transient target placed outside the slots | the slot range is full; the target gets memory of its own |
| a transient target's memory taken while it may be in use | a claim within 2 presents of the old owner's last use |
| a transient target sampled after another took its memory | a draw binds an image that no longer owns its range |
| the transient pool was not made | the back end could not allocate the range |

If "placed outside the slots" appears, the range is too small for the scene: raise `RX_RT_SLOTS_MB` and report the
scene. Scene loads are the peak, because the dynamic resolution steps through several sizes in a few presents and each
size is a full set of targets.

## Log lines

Every 600 presents:

- `direct: transient pool: N MB of device memory (constant), ...`: image objects kept, claims, targets given out, and
  the counts behind the strict events since the start.
- `lists handed to the queue 0, 1, 2, 3+ presents after they were begun`: the basis of
  `REUSE_AFTER`. A count in the "3+" column means 3 presents is no longer safe.
- `rt slots: transient pool: N MB of M held at a present at most ...`: the peak use of the range.

`HOS_RT_SLOTS_LOG=1` adds the slots' state at the first failed placements. `RX_NVN_LOG_TEX=<w>x<h>` logs every
initialization and finalize of textures of one size.

## Parking

When the game frees a transient target that it will ask for again, the host keeps the whole entry (texture object and
descriptor) and hands it back at the next request with the same key, oldest first (`HOS_RT_PARK=0` turns this off). An
entry not handed back for a few presents is freed for real. Parking keeps the game from initializing and registering
the same target again each frame.

## Limits

- Persistent targets of the same allocator are outside the slots and have memory of their own.
- The pool's size is measured on a few routes only; see the open work in
  [the direct back end plan](../direct-backend-plan.md).
