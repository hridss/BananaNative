// Guest Vulkan on the device's Vulkan (docs/reference/rendering.md "Guest Vulkan passed through to the device's driver"): for games that render with Vulkan
// themselves instead of NVN (Super Mario 3D All-Stars' Super Mario 64 and Super Mario Sunshine, Super Mario Galaxy, Hades, Minecraft, Re:ZERO). The game imports
// the vk* entry points from the SDK's Vulkan loader (subsdk0), which the host does not load; they bind to the functions here through the host's import table
// (hle.h swhle), as on the Mac (dm/gfx/vk/hle_vk.m, Vulkan on Metal). Here they go to the phone's own driver, the device the NVN replay made (vka_dev.c), so the
// game's ARM64 code calls the driver as a native Android app would:
//   · one physical device shaped like the Switch's Tegra X1 where the game reads its identity (vendor, name, API 1.1, one queue family with 16 queues), and the
//     phone's own where a value is a promise the driver must keep (limits, alignments, formats, features masked to what the Tegra has);
//   · the game's VkInstance, VkPhysicalDevice, VkDevice and every VkQueue are the replay's (V.inst, V.pd, V.dev, V.q): the game's handles are real driver handles, so
//     most calls go straight to the driver through a three-instruction trampoline (GVT) that only clears the allocator argument (the driver never calls the game's
//     allocation callbacks);
//   · memory: the Tegra's three memory types (device local, host visible, host cached) stand for three of the phone's, and memory requirements are translated;
//   · the WSI is the host's: the nn::vi surface is a token, a swapchain's images are images made here (vka_img), and a present hands the image to the replay's present
//     thread (vka_present.c vka_present_guest) as the NVN window texture is: the same scaling, rotation, overlay and pacing;
//   · queue submissions take the replay's queue lock (V.qmu), and a present signals the replay's timeline (V.tl) with a fence of its ledger, which the present waits for;
//   · what the Switch's driver lets pass and the phone's does not is fixed on the way: attachments opened or stored with DONT_CARE keep their contents (NVIDIA's driver
//     does not discard), descriptor sets freed after their pool was reset or destroyed, a swapchain's images destroyed by the game, ArrayStride on struct members (Hagi).
// The functions that call into the driver's compiler or wait run on the thread's big host stack (hle_bigstack.c): a game thread's stack may be small.
// Switches: DM_VK_GUEST_LOG=1 logs object creation; DM_VK_GUEST_KEEP=0 leaves DONT_CARE attachments to the driver.
#include "vka.h"
#include "../../host/hle.h"
#include <vulkan/vulkan_vi.h>
#include <string.h>
#include <time.h>

void mtl_present_wait(uint64_t ticket);   // vka_present.c
int mtl_fps_target(void);                 // vka_present.c: 60, or DM_FPS
void dm_vi_layer_crop(int c[4]);          // hle_vi.c: x, y, w, h the game last cropped its layer to (all 0 = none)

#define GLOG(...) LOG("[gvk] " __VA_ARGS__)
#define GRED(...) LOG("[RED] gvk: " __VA_ARGS__)
#define GRED_ONCE(...) do { static _Atomic int once_; if (!atomic_exchange(&once_, 1)) GRED(__VA_ARGS__); } while (0)
static int g_log;
#define OLOG(...) do { if (g_log) GLOG(__VA_ARGS__); } while (0)

// GV: one Vulkan function implemented here, checked against its PFN_ type and registered in the host's import table under its own name
#define GV(name, ret, ...)                                                                       \
    static ret h_##name(__VA_ARGS__);                                                            \
    static const PFN_##name chk_##name __attribute__((unused)) = h_##name;                       \
    PLAT_TABLE(swhle) static const hle_ent gve_##name = {#name, (void *)h_##name};               \
    static ret h_##name(__VA_ARGS__)
// GVB: the same, run on the thread's big host stack (hle_bigstack.c; register arguments only, at most 8)
#define GVB(name, ret, ...)                                                                      \
    static ret h_##name(__VA_ARGS__) __attribute__((used));                                      \
    static const PFN_##name chk_##name __attribute__((unused)) = h_##name;                       \
    extern const char h_##name##_bs[];                                                           \
    __asm__(".text\n.p2align 2\n.globl " PLAT_ASM_SYM(h_##name##_bs) "\n" PLAT_ASM_HIDDEN " " PLAT_ASM_SYM(h_##name##_bs) "\n" PLAT_ASM_SYM(h_##name##_bs) ":\n" \
            "    adrp x16, " PLAT_ASM_PAGE(h_##name) "\n    add x16, x16, " PLAT_ASM_PAGEOFF(h_##name) "\n    b " PLAT_ASM_SYM(dm_bigstack_enter) "\n"); \
    PLAT_TABLE(swhle) static const hle_ent gve_##name = {#name, (void *)(uintptr_t)h_##name##_bs}; \
    static ret h_##name(__VA_ARGS__)

// GVT: a function passed to the driver unchanged but for the allocator argument: the trampoline loads the driver's entry point from its slot (filled when the device
// is up), clears register x<z> (the VkAllocationCallbacks pointer; -1: none) and jumps there, so every argument, those on the stack too, reaches the driver as the
// game passed it. A call before the slot is filled ends in gv_unbound with the function's name (a red line, the call returns 0)
#define GV_Z_NONE ""
#define GV_Z1 "    mov x1, xzr\n"
#define GV_Z2 "    mov x2, xzr\n"
#define GV_Z3 "    mov x3, xzr\n"
#define GV_Z4 "    mov x4, xzr\n"
#define GVT(name, z)                                                                             \
    __attribute__((visibility("hidden"), used)) void *gvs_##name;                                \
    __attribute__((visibility("hidden"), used)) const char gvn_##name[] = #name;                 \
    extern const char gvt_##name[];                                                              \
    __asm__(".text\n.p2align 2\n.globl " PLAT_ASM_SYM(gvt_##name) "\n" PLAT_ASM_HIDDEN " " PLAT_ASM_SYM(gvt_##name) "\n" PLAT_ASM_SYM(gvt_##name) ":\n" \
            "    adrp x16, " PLAT_ASM_PAGE(gvs_##name) "\n    ldr x16, [x16, " PLAT_ASM_PAGEOFF(gvs_##name) "]\n" z \
            "    cbz x16, 1f\n    br x16\n1:\n    adrp x0, " PLAT_ASM_PAGE(gvn_##name) "\n    add x0, x0, " PLAT_ASM_PAGEOFF(gvn_##name) "\n    b " PLAT_ASM_SYM(gv_unbound) "\n"); \
    PLAT_TABLE(swhle) static const hle_ent gve_##name = {#name, (void *)(uintptr_t)gvt_##name};

// The driver call a game thread is in, for a crash inside the driver (gv_fault: one line in the crash report). GV_IN at the top of a function here
static __thread const char *gv_cur;
static void gv_out(const char **p) { gv_cur = *p; }
#define GV_IN(name) const char *gv_prev_ __attribute__((cleanup(gv_out))) = gv_cur; gv_cur = name
static int gv_fault(void *addr, int is_write, void *uc) {
    (void)uc; if (gv_cur) LOG("[gvk] fault at %p (%s) inside %s, called by the game", addr, is_write ? "write" : "read", gv_cur);
    return 0;
}
__attribute__((visibility("hidden"), used)) uint64_t gv_unbound(const char *name);
__attribute__((visibility("hidden"), used)) uint64_t gv_unbound(const char *name) {
    static _Atomic int n; if (atomic_fetch_add(&n, 1) < 16) GRED("%s called before the device was made (or the driver has no such entry point): returns 0", name);
    return 0;
}

// The functions that go to the driver unchanged (X(name, allocator register)). Instance-level ones (I) are filled from V.inst, the others from V.dev
#define GV_PASS_I(X) \
    X(vkGetPhysicalDeviceSparseImageFormatProperties, GV_Z_NONE) X(vkGetPhysicalDeviceSparseImageFormatProperties2, GV_Z_NONE) \
    X(vkGetPhysicalDeviceExternalBufferProperties, GV_Z_NONE) X(vkGetPhysicalDeviceExternalSemaphoreProperties, GV_Z_NONE) X(vkGetPhysicalDeviceExternalFenceProperties, GV_Z_NONE)
#define GV_PASS_D(X) \
    X(vkFreeMemory, GV_Z2) X(vkMapMemory, GV_Z_NONE) X(vkUnmapMemory, GV_Z_NONE) X(vkFlushMappedMemoryRanges, GV_Z_NONE) X(vkInvalidateMappedMemoryRanges, GV_Z_NONE) \
    X(vkGetDeviceMemoryCommitment, GV_Z_NONE) \
    X(vkCreateBuffer, GV_Z2) X(vkDestroyBuffer, GV_Z2) X(vkBindBufferMemory, GV_Z_NONE) X(vkBindBufferMemory2, GV_Z_NONE) \
    X(vkCreateBufferView, GV_Z2) X(vkDestroyBufferView, GV_Z2) \
    X(vkGetImageSubresourceLayout, GV_Z_NONE) X(vkBindImageMemory, GV_Z_NONE) X(vkBindImageMemory2, GV_Z_NONE) \
    X(vkGetImageSparseMemoryRequirements, GV_Z_NONE) X(vkGetImageSparseMemoryRequirements2, GV_Z_NONE) \
    X(vkCreateImageView, GV_Z2) X(vkDestroyImageView, GV_Z2) X(vkCreateSampler, GV_Z2) X(vkDestroySampler, GV_Z2) \
    X(vkCreateSamplerYcbcrConversion, GV_Z2) X(vkDestroySamplerYcbcrConversion, GV_Z2) \
    X(vkCreateDescriptorSetLayout, GV_Z2) X(vkDestroyDescriptorSetLayout, GV_Z2) X(vkGetDescriptorSetLayoutSupport, GV_Z_NONE) \
    X(vkUpdateDescriptorSets, GV_Z_NONE) X(vkCreateDescriptorUpdateTemplate, GV_Z2) X(vkDestroyDescriptorUpdateTemplate, GV_Z2) X(vkUpdateDescriptorSetWithTemplate, GV_Z_NONE) \
    X(vkCreatePipelineLayout, GV_Z2) X(vkDestroyPipelineLayout, GV_Z2) X(vkDestroyRenderPass, GV_Z2) X(vkGetRenderAreaGranularity, GV_Z_NONE) \
    X(vkCreateFramebuffer, GV_Z2) X(vkDestroyFramebuffer, GV_Z2) \
    X(vkCreatePipelineCache, GV_Z2) X(vkDestroyPipelineCache, GV_Z2) X(vkGetPipelineCacheData, GV_Z_NONE) X(vkMergePipelineCaches, GV_Z_NONE) X(vkDestroyPipeline, GV_Z2) \
    X(vkCreateCommandPool, GV_Z2) X(vkDestroyCommandPool, GV_Z2) X(vkResetCommandPool, GV_Z_NONE) X(vkTrimCommandPool, GV_Z_NONE) \
    X(vkAllocateCommandBuffers, GV_Z_NONE) X(vkFreeCommandBuffers, GV_Z_NONE) X(vkBeginCommandBuffer, GV_Z_NONE) X(vkEndCommandBuffer, GV_Z_NONE) X(vkResetCommandBuffer, GV_Z_NONE) \
    X(vkCreateEvent, GV_Z2) X(vkDestroyEvent, GV_Z2) X(vkGetEventStatus, GV_Z_NONE) X(vkSetEvent, GV_Z_NONE) X(vkResetEvent, GV_Z_NONE) \
    X(vkCreateFence, GV_Z2) X(vkDestroyFence, GV_Z2) X(vkResetFences, GV_Z_NONE) X(vkGetFenceStatus, GV_Z_NONE) \
    X(vkCreateSemaphore, GV_Z2) X(vkDestroySemaphore, GV_Z2) X(vkGetSemaphoreCounterValue, GV_Z_NONE) X(vkSignalSemaphore, GV_Z_NONE) \
    X(vkGetBufferDeviceAddress, GV_Z_NONE) X(vkGetBufferOpaqueCaptureAddress, GV_Z_NONE) X(vkGetDeviceMemoryOpaqueCaptureAddress, GV_Z_NONE) \
    X(vkCmdBindPipeline, GV_Z_NONE) X(vkCmdSetViewport, GV_Z_NONE) X(vkCmdSetScissor, GV_Z_NONE) X(vkCmdSetLineWidth, GV_Z_NONE) X(vkCmdSetDepthBias, GV_Z_NONE) \
    X(vkCmdSetBlendConstants, GV_Z_NONE) X(vkCmdSetDepthBounds, GV_Z_NONE) X(vkCmdSetStencilCompareMask, GV_Z_NONE) X(vkCmdSetStencilWriteMask, GV_Z_NONE) \
    X(vkCmdSetStencilReference, GV_Z_NONE) X(vkCmdBindDescriptorSets, GV_Z_NONE) X(vkCmdBindIndexBuffer, GV_Z_NONE) X(vkCmdBindVertexBuffers, GV_Z_NONE) \
    X(vkCmdDraw, GV_Z_NONE) X(vkCmdDrawIndexed, GV_Z_NONE) X(vkCmdDrawIndirect, GV_Z_NONE) X(vkCmdDrawIndexedIndirect, GV_Z_NONE) \
    X(vkCmdDrawIndirectCount, GV_Z_NONE) X(vkCmdDrawIndexedIndirectCount, GV_Z_NONE) \
    X(vkCmdDispatch, GV_Z_NONE) X(vkCmdDispatchIndirect, GV_Z_NONE) X(vkCmdDispatchBase, GV_Z_NONE) X(vkCmdSetDeviceMask, GV_Z_NONE) \
    X(vkCmdCopyBuffer, GV_Z_NONE) X(vkCmdCopyImage, GV_Z_NONE) X(vkCmdBlitImage, GV_Z_NONE) X(vkCmdCopyBufferToImage, GV_Z_NONE) X(vkCmdCopyImageToBuffer, GV_Z_NONE) \
    X(vkCmdUpdateBuffer, GV_Z_NONE) X(vkCmdFillBuffer, GV_Z_NONE) X(vkCmdClearColorImage, GV_Z_NONE) X(vkCmdClearDepthStencilImage, GV_Z_NONE) \
    X(vkCmdClearAttachments, GV_Z_NONE) X(vkCmdResolveImage, GV_Z_NONE) X(vkCmdSetEvent, GV_Z_NONE) X(vkCmdResetEvent, GV_Z_NONE) X(vkCmdWaitEvents, GV_Z_NONE) \
    X(vkCmdPipelineBarrier, GV_Z_NONE) X(vkCmdPipelineBarrier2, GV_Z_NONE) \
    X(vkCmdWriteTimestamp, GV_Z_NONE) X(vkCmdPushConstants, GV_Z_NONE) X(vkCmdBeginRenderPass, GV_Z_NONE) \
    X(vkCmdNextSubpass, GV_Z_NONE) X(vkCmdEndRenderPass, GV_Z_NONE) X(vkCmdExecuteCommands, GV_Z_NONE) \
    X(vkCmdBeginRenderPass2, GV_Z_NONE) X(vkCmdNextSubpass2, GV_Z_NONE) X(vkCmdEndRenderPass2, GV_Z_NONE)
#define GV_TRAMP(name, z) GVT(name, z)
GV_PASS_I(GV_TRAMP)
GV_PASS_D(GV_TRAMP)

// ---------- The driver's entry points the functions here call ----------
#define GV_REAL_D(X) \
    X(vkQueueSubmit) X(vkQueueSubmit2) X(vkQueueWaitIdle) X(vkQueueBindSparse) X(vkAllocateMemory) X(vkGetBufferMemoryRequirements) X(vkGetBufferMemoryRequirements2) \
    X(vkGetImageMemoryRequirements) X(vkGetImageMemoryRequirements2) X(vkCreateImage) X(vkDestroyImage) X(vkCreateRenderPass) X(vkCreateRenderPass2) \
    X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkResetDescriptorPool) X(vkAllocateDescriptorSets) X(vkFreeDescriptorSets) \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreateGraphicsPipelines) X(vkCreateComputePipelines) X(vkWaitForFences) X(vkWaitSemaphores) \
    X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCmdPipelineBarrier) X(vkCmdBeginQuery) X(vkCmdEndQuery) X(vkCmdResetQueryPool) X(vkResetQueryPool) X(vkCreateQueryPool) X(vkDestroyQueryPool) X(vkCmdCopyQueryPoolResults) X(vkGetQueryPoolResults)
#define GV_DECL(f) PFN_##f f;
static struct { GV_REAL_D(GV_DECL) } R;

static _Atomic int g_up;   // 1 the device is up and the slots are filled, -1 no device
static pthread_mutex_t g_up_mu = PTHREAD_MUTEX_INITIALIZER;
static int gv_up(void) {
    int s = atomic_load_explicit(&g_up, memory_order_acquire);
    if (s) return s > 0;
    pthread_mutex_lock(&g_up_mu);
    if (!atomic_load(&g_up)) {
        g_log = VKA_ENV("DM_VK_GUEST_LOG", 0);
        int ok = vka_init();
        if (ok) {
#define FILL_I(name, z) gvs_##name = (void *)vk.vkGetInstanceProcAddr(V.inst, #name);
#define FILL_D(name, z) gvs_##name = (void *)vk.vkGetDeviceProcAddr(V.dev, #name);
#define FILL_R(name) R.name = (PFN_##name)vk.vkGetDeviceProcAddr(V.dev, #name);
            GV_PASS_I(FILL_I) GV_PASS_D(FILL_D) GV_REAL_D(FILL_R)
#undef FILL_I
#undef FILL_D
#undef FILL_R
            dm_fault_hook_register(gv_fault);
            if (V.qf != 0) GRED("the device's graphics queue family is %u, not 0: the game's queue family indices (0) are not translated", V.qf);
        }
        atomic_store_explicit(&g_up, ok ? 1 : -1, memory_order_release);
        GLOG("guest Vulkan on %s: %s", V.props.deviceName, ok ? "the device is up, the game's calls go to its driver" : "no Vulkan device: the game's calls fail");
    }
    pthread_mutex_unlock(&g_up_mu);
    return atomic_load(&g_up) > 0;
}

// ---------- Small tables ----------
// An open-addressing set of 64-bit keys with a value (0 is never a key); under the caller's lock
typedef struct { uint64_t *k, *v; uint32_t cap, n, dead; } gv_map;
static uint64_t gv_hash(uint64_t k) { k ^= k >> 33; k *= 0xff51afd7ed558ccdull; k ^= k >> 33; return k; }
static int gv_map_find(const gv_map *m, uint64_t k) {
    if (!m->cap) return -1;
    for (uint32_t i = (uint32_t)gv_hash(k) & (m->cap - 1);; i = (i + 1) & (m->cap - 1)) {
        if (m->k[i] == k) return (int)i;
        if (!m->k[i]) return -1;
    }
}
static void gv_map_put(gv_map *m, uint64_t k, uint64_t v);
static void gv_map_grow(gv_map *m) {
    gv_map o = *m; m->cap = o.cap ? o.cap * 2 : 256; m->n = m->dead = 0;
    if ((o.n - o.dead) * 4 < o.cap && o.cap) m->cap = o.cap;   // mostly tombstones: rebuild at the same size
    m->k = calloc(m->cap, 8); m->v = calloc(m->cap, 8);
    for (uint32_t i = 0; i < o.cap; i++) if (o.k[i] && o.k[i] != ~0ull) gv_map_put(m, o.k[i], o.v[i]);
    free(o.k); free(o.v);
}
static void gv_map_put(gv_map *m, uint64_t k, uint64_t v) {
    int f = gv_map_find(m, k); if (f >= 0) { m->v[f] = v; return; }
    if ((m->n + 1) * 4 > m->cap * 3) gv_map_grow(m);
    for (uint32_t i = (uint32_t)gv_hash(k) & (m->cap - 1);; i = (i + 1) & (m->cap - 1))
        if (!m->k[i]) { m->k[i] = k; m->v[i] = v; m->n++; return; }
}
static void gv_map_del(gv_map *m, uint64_t k) { int f = gv_map_find(m, k); if (f >= 0) { m->k[f] = ~0ull; m->v[f] = 0; m->dead++; } }

// ---------- Instance ----------
static VkResult fill_names(const char *const *names, uint32_t n, uint32_t *count, VkExtensionProperties *p) {
    if (!p) { *count = n; return VK_SUCCESS; }
    uint32_t k = *count < n ? *count : n;
    for (uint32_t i = 0; i < k; i++) { memset(&p[i], 0, sizeof p[i]); strncpy(p[i].extensionName, names[i], sizeof p[i].extensionName - 1); p[i].specVersion = 1; }
    *count = k; return k < n ? VK_INCOMPLETE : VK_SUCCESS;
}
static const char *const g_inst_ext[] = {"VK_KHR_surface", "VK_NN_vi_surface", "VK_KHR_get_physical_device_properties2", "VK_KHR_get_surface_capabilities2", "VK_EXT_debug_report"};
// The device extensions of the Switch's driver that games were seen to use; all are core in Vulkan 1.1, which the phone's device has
static const char *const g_dev_ext[] = {"VK_KHR_swapchain", "VK_KHR_descriptor_update_template", "VK_KHR_maintenance1", "VK_KHR_get_memory_requirements2",
                                        "VK_KHR_dedicated_allocation", "VK_KHR_bind_memory2"};
static void join_names(char *out, size_t n, const char *const *names, uint32_t count) {
    out[0] = 0; size_t o = 0;
    for (uint32_t i = 0; i < count && o + 2 < n; i++) o += (size_t)snprintf(out + o, n - o, "%s%s", i ? " " : "", names[i]);
    if (!count) snprintf(out, n, "none");
}

GV(vkEnumerateInstanceVersion, VkResult, uint32_t *v) { *v = VK_API_VERSION_1_1; return VK_SUCCESS; }
GV(vkEnumerateInstanceLayerProperties, VkResult, uint32_t *n, VkLayerProperties *p) { (void)p; *n = 0; return VK_SUCCESS; }
GV(vkEnumerateInstanceExtensionProperties, VkResult, const char *layer, uint32_t *n, VkExtensionProperties *p) {
    if (layer) { *n = 0; return VK_ERROR_LAYER_NOT_PRESENT; }
    return fill_names(g_inst_ext, sizeof g_inst_ext / sizeof *g_inst_ext, n, p);
}
GVB(vkCreateInstance, VkResult, const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *a, VkInstance *out) {
    (void)a;
    const VkApplicationInfo *ai = ci->pApplicationInfo; char ext[512];
    join_names(ext, sizeof ext, ci->ppEnabledExtensionNames, ci->enabledExtensionCount);
    GLOG("CreateInstance: application %s, engine %s, API %u.%u; extensions %s; %u layers", ai && ai->pApplicationName ? ai->pApplicationName : "-",
         ai && ai->pEngineName ? ai->pEngineName : "-", ai ? VK_API_VERSION_MAJOR(ai->apiVersion) : 0, ai ? VK_API_VERSION_MINOR(ai->apiVersion) : 0, ext, ci->enabledLayerCount);
    if (!gv_up()) return VK_ERROR_INITIALIZATION_FAILED;
    *out = V.inst; return VK_SUCCESS;
}
GV(vkDestroyInstance, void, VkInstance i, const VkAllocationCallbacks *a) { (void)i; (void)a; }   // the replay's instance stays
// VK_EXT_debug_report: a game that enabled it calls its functions without checking the lookup (Super Mario 64 called the null pointer). Nothing is reported: the
// callback object is a token that is never called
GV(vkCreateDebugReportCallbackEXT, VkResult, VkInstance i, const VkDebugReportCallbackCreateInfoEXT *ci, const VkAllocationCallbacks *a, VkDebugReportCallbackEXT *out) {
    (void)i; (void)ci; (void)a; static _Atomic uint64_t next; *out = (VkDebugReportCallbackEXT)(uintptr_t)(0x1000 + atomic_fetch_add(&next, 1) + 1); return VK_SUCCESS;
}
GV(vkDestroyDebugReportCallbackEXT, void, VkInstance i, VkDebugReportCallbackEXT cb, const VkAllocationCallbacks *a) { (void)i; (void)cb; (void)a; }
GV(vkDebugReportMessageEXT, void, VkInstance i, VkDebugReportFlagsEXT f, VkDebugReportObjectTypeEXT t, uint64_t o, size_t loc, int32_t code, const char *layer, const char *msg) {
    (void)i; (void)f; (void)t; (void)o; (void)loc; (void)code; GLOG("debug report from the game: %s: %s", layer ? layer : "-", msg ? msg : "-");
}

// ---------- Physical device ----------
GV(vkEnumeratePhysicalDevices, VkResult, VkInstance i, uint32_t *n, VkPhysicalDevice *p) {
    (void)i; if (!gv_up()) { *n = 0; return VK_ERROR_INITIALIZATION_FAILED; }
    if (!p) { *n = 1; return VK_SUCCESS; }
    if (!*n) return VK_INCOMPLETE;
    *n = 1; p[0] = V.pd; return VK_SUCCESS;
}
// The game reads the Tegra X1's identity under NVIDIA's Switch driver; the limits are the phone's, which its driver keeps, except that an alignment is never finer than
// the Tegra's (a game may lay out its buffers by the Switch's value)
static void pdev_props(VkPhysicalDeviceProperties *p) {
    VkPhysicalDeviceProperties r = V.props;
    p->apiVersion = VK_API_VERSION_1_1; p->driverVersion = VK_MAKE_API_VERSION(0, 1, 0, 0); p->vendorID = 0x10de; p->deviceID = 0x92ba;
    p->deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU; memset(p->deviceName, 0, sizeof p->deviceName); snprintf(p->deviceName, sizeof p->deviceName, "NVIDIA Tegra X1 (nvgpu)");
    memcpy(p->pipelineCacheUUID, r.pipelineCacheUUID, sizeof p->pipelineCacheUUID);
    p->limits = r.limits; p->sparseProperties = r.sparseProperties;
    VkPhysicalDeviceLimits *l = &p->limits;
#define AT_LEAST(f, v) if (l->f < (v)) l->f = (v)
    AT_LEAST(minUniformBufferOffsetAlignment, 256); AT_LEAST(minStorageBufferOffsetAlignment, 256); AT_LEAST(minTexelBufferOffsetAlignment, 256);
    AT_LEAST(optimalBufferCopyOffsetAlignment, 256); AT_LEAST(optimalBufferCopyRowPitchAlignment, 256); AT_LEAST(nonCoherentAtomSize, 64);
    AT_LEAST(minMemoryMapAlignment, 64); AT_LEAST(bufferImageGranularity, 1024);
#undef AT_LEAST
}
// The Tegra's features (as upstream's layer reports them, plus what NVIDIA's driver has that the phone may have too), each only where the phone has it
static void pdev_feats(VkPhysicalDeviceFeatures *f) {
    VkPhysicalDeviceFeatures r; vk.vkGetPhysicalDeviceFeatures(V.pd, &r);
    VkPhysicalDeviceFeatures t; memset(&t, 0, sizeof t);
    t.robustBufferAccess = t.fullDrawIndexUint32 = t.imageCubeArray = t.independentBlend = t.geometryShader = t.tessellationShader = t.sampleRateShading = t.dualSrcBlend = 1;
    t.logicOp = t.multiDrawIndirect = t.drawIndirectFirstInstance = t.depthClamp = t.depthBiasClamp = t.fillModeNonSolid = t.depthBounds = t.wideLines = t.largePoints = 1;
    t.alphaToOne = t.multiViewport = t.samplerAnisotropy = t.textureCompressionETC2 = t.textureCompressionASTC_LDR = t.textureCompressionBC = t.occlusionQueryPrecise = 1;
    t.pipelineStatisticsQuery = t.vertexPipelineStoresAndAtomics = t.fragmentStoresAndAtomics = t.shaderTessellationAndGeometryPointSize = t.shaderImageGatherExtended = 1;
    t.shaderStorageImageExtendedFormats = t.shaderStorageImageMultisample = t.shaderStorageImageReadWithoutFormat = t.shaderStorageImageWriteWithoutFormat = 1;
    t.shaderUniformBufferArrayDynamicIndexing = t.shaderSampledImageArrayDynamicIndexing = t.shaderStorageBufferArrayDynamicIndexing = t.shaderStorageImageArrayDynamicIndexing = 1;
    t.shaderClipDistance = t.shaderCullDistance = t.shaderFloat64 = t.shaderInt64 = t.shaderInt16 = t.shaderResourceResidency = t.shaderResourceMinLod = 1;
    t.variableMultisampleRate = t.inheritedQueries = 1;
    const VkBool32 *a = (const VkBool32 *)&r, *b = (const VkBool32 *)&t; VkBool32 *o = (VkBool32 *)f;
    for (size_t i = 0; i < sizeof *f / sizeof(VkBool32); i++) o[i] = a[i] && b[i];
}
// Memory: the Tegra's three types on one heap, each standing for a type of the phone's (g_gtype); a resource's memoryTypeBits are translated through it
static uint32_t g_gtype[3];
static VkPhysicalDeviceMemoryProperties g_gmem;
static void mem_model(void) {
    static int done; if (done) return; done = 1;
    const VkMemoryPropertyFlags DL = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, HV = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, HC = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, CA = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    const VkMemoryPropertyFlags BAD = VK_MEMORY_PROPERTY_PROTECTED_BIT | VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT;
    uint32_t t0 = vka_mem_type(~0u, DL, HV | BAD); if (t0 == UINT32_MAX) t0 = vka_mem_type(~0u, DL, BAD);
    uint32_t t1 = vka_mem_type(~0u, DL | HV | HC, CA | BAD); if (t1 == UINT32_MAX) t1 = vka_mem_type(~0u, HV | HC, CA | BAD); if (t1 == UINT32_MAX) t1 = vka_mem_type(~0u, HV | HC, BAD);
    uint32_t t2 = vka_mem_type(~0u, DL | HV | HC | CA, BAD); if (t2 == UINT32_MAX) t2 = vka_mem_type(~0u, HV | HC | CA, BAD); if (t2 == UINT32_MAX) t2 = t1;
    if (t0 == UINT32_MAX) t0 = t1 != UINT32_MAX ? t1 : 0;
    if (t1 == UINT32_MAX) { GRED("the device has no host-visible coherent memory type: the game's mapped memory is not coherent"); t1 = t2 = t0; }
    g_gtype[0] = t0; g_gtype[1] = t1; g_gtype[2] = t2;
    memset(&g_gmem, 0, sizeof g_gmem);
    g_gmem.memoryTypeCount = 3;
    g_gmem.memoryTypes[0] = (VkMemoryType){DL, 0}; g_gmem.memoryTypes[1] = (VkMemoryType){DL | HV | HC, 0}; g_gmem.memoryTypes[2] = (VkMemoryType){DL | HV | HC | CA, 0};
    VkDeviceSize heap = V.memp.memoryHeaps[V.memp.memoryTypes[t0].heapIndex].size;
    g_gmem.memoryHeapCount = 1; g_gmem.memoryHeaps[0] = (VkMemoryHeap){heap, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT};
    GLOG("memory types: the Tegra's device local, host visible and host cached types are the device's %u (%#x), %u (%#x) and %u (%#x); one heap of %llu MiB",
         t0, V.memp.memoryTypes[t0].propertyFlags, t1, V.memp.memoryTypes[t1].propertyFlags, t2, V.memp.memoryTypes[t2].propertyFlags, (unsigned long long)(heap >> 20));
}
static uint32_t bits_g(uint32_t real) {   // a resource's memory types as the Tegra's
    uint32_t g = 0; for (int i = 0; i < 3; i++) if (real >> g_gtype[i] & 1) g |= 1u << i;
    if (!g && real) GRED_ONCE("a resource can live in none of the memory types the Tegra's stand for (device types %#x): reported as type 0", real);
    return g ? g : 1;
}
GV(vkGetPhysicalDeviceProperties, void, VkPhysicalDevice pd, VkPhysicalDeviceProperties *p) { (void)pd; pdev_props(p); }
GV(vkGetPhysicalDeviceProperties2, void, VkPhysicalDevice pd, VkPhysicalDeviceProperties2 *p) {
    (void)pd; vk.vkGetPhysicalDeviceProperties2(V.pd, p);   // the chained structures from the driver
    pdev_props(&p->properties);
}
GV(vkGetPhysicalDeviceFeatures, void, VkPhysicalDevice pd, VkPhysicalDeviceFeatures *f) { (void)pd; pdev_feats(f); }
GV(vkGetPhysicalDeviceFeatures2, void, VkPhysicalDevice pd, VkPhysicalDeviceFeatures2 *f) { (void)pd; vk.vkGetPhysicalDeviceFeatures2(V.pd, f); pdev_feats(&f->features); }
GV(vkGetPhysicalDeviceMemoryProperties, void, VkPhysicalDevice pd, VkPhysicalDeviceMemoryProperties *m) { (void)pd; mem_model(); *m = g_gmem; }
GV(vkGetPhysicalDeviceMemoryProperties2, void, VkPhysicalDevice pd, VkPhysicalDeviceMemoryProperties2 *m) { (void)pd; mem_model(); m->memoryProperties = g_gmem; }
static void qfam(VkQueueFamilyProperties *p) {
    uint32_t nq = 0; vk.vkGetPhysicalDeviceQueueFamilyProperties(V.pd, &nq, NULL);
    VkQueueFamilyProperties q[8]; if (nq > 8) nq = 8; vk.vkGetPhysicalDeviceQueueFamilyProperties(V.pd, &nq, q);
    *p = V.qf < nq ? q[V.qf] : (VkQueueFamilyProperties){0};
    p->queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT; p->queueCount = 16;   // every VkQueue of the game is the replay's one queue
}
GV(vkGetPhysicalDeviceQueueFamilyProperties, void, VkPhysicalDevice pd, uint32_t *n, VkQueueFamilyProperties *p) {
    (void)pd; if (!p) { *n = 1; return; }
    if (!*n) return;
    *n = 1; qfam(&p[0]);
}
GV(vkGetPhysicalDeviceQueueFamilyProperties2, void, VkPhysicalDevice pd, uint32_t *n, VkQueueFamilyProperties2 *p) {
    (void)pd; if (!p) { *n = 1; return; }
    if (!*n) return;
    *n = 1; qfam(&p[0].queueFamilyProperties);
}
GV(vkGetPhysicalDeviceFormatProperties, void, VkPhysicalDevice pd, VkFormat format, VkFormatProperties *p) { (void)pd; vk.vkGetPhysicalDeviceFormatProperties(V.pd, format, p); }
GV(vkGetPhysicalDeviceFormatProperties2, void, VkPhysicalDevice pd, VkFormat format, VkFormatProperties2 *p) {
    (void)pd; PFN_vkGetPhysicalDeviceFormatProperties2 f = (PFN_vkGetPhysicalDeviceFormatProperties2)vk.vkGetInstanceProcAddr(V.inst, "vkGetPhysicalDeviceFormatProperties2");
    if (f) f(V.pd, format, p); else vk.vkGetPhysicalDeviceFormatProperties(V.pd, format, &p->formatProperties);
}
GV(vkGetPhysicalDeviceImageFormatProperties, VkResult, VkPhysicalDevice pd, VkFormat format, VkImageType type, VkImageTiling tiling, VkImageUsageFlags usage,
   VkImageCreateFlags flags, VkImageFormatProperties *p) {
    (void)pd; return vk.vkGetPhysicalDeviceImageFormatProperties(V.pd, format, type, tiling, usage, flags, p);
}
GV(vkGetPhysicalDeviceImageFormatProperties2, VkResult, VkPhysicalDevice pd, const VkPhysicalDeviceImageFormatInfo2 *i, VkImageFormatProperties2 *p) {
    (void)pd; PFN_vkGetPhysicalDeviceImageFormatProperties2 f = (PFN_vkGetPhysicalDeviceImageFormatProperties2)vk.vkGetInstanceProcAddr(V.inst, "vkGetPhysicalDeviceImageFormatProperties2");
    if (f) return f(V.pd, i, p);
    return vk.vkGetPhysicalDeviceImageFormatProperties(V.pd, i->format, i->type, i->tiling, i->usage, i->flags, &p->imageFormatProperties);
}

// ---------- Surface (nn::vi's layer) ----------
typedef struct { void *win; } gv_surf;
static VkExtent2D surf_ext(void) { return dm_opmode() ? (VkExtent2D){1920, 1080} : (VkExtent2D){1280, 720}; }
GV(vkCreateViSurfaceNN, VkResult, VkInstance i, const VkViSurfaceCreateInfoNN *ci, const VkAllocationCallbacks *a, VkSurfaceKHR *out) {
    (void)i; (void)a; gv_surf *s = calloc(1, sizeof *s); if (!s) return VK_ERROR_OUT_OF_HOST_MEMORY;
    s->win = ci->window; *out = (VkSurfaceKHR)(uintptr_t)s;
    GLOG("CreateViSurfaceNN window %p", ci->window); return VK_SUCCESS;
}
GV(vkDestroySurfaceKHR, void, VkInstance i, VkSurfaceKHR s, const VkAllocationCallbacks *a) { (void)i; (void)a; free((void *)(uintptr_t)s); }
GV(vkGetPhysicalDeviceSurfaceSupportKHR, VkResult, VkPhysicalDevice pd, uint32_t fam, VkSurfaceKHR s, VkBool32 *ok) { (void)pd; (void)s; *ok = fam == 0; return VK_SUCCESS; }
GV(vkGetPhysicalDeviceSurfaceCapabilitiesKHR, VkResult, VkPhysicalDevice pd, VkSurfaceKHR s, VkSurfaceCapabilitiesKHR *c) {
    (void)pd; (void)s; memset(c, 0, sizeof *c);
    c->minImageCount = 2; c->maxImageCount = 4; c->currentExtent = surf_ext(); c->minImageExtent = (VkExtent2D){1, 1}; c->maxImageExtent = (VkExtent2D){1920, 1080};
    // INHERIT as well: Super Mario Galaxy creates its swapchain with preTransform INHERIT and asserts when the surface does not list it (the present rotates by itself)
    c->maxImageArrayLayers = 1; c->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR; c->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    c->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR | VK_SURFACE_TRANSFORM_INHERIT_BIT_KHR;
    c->supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    return VK_SUCCESS;
}
GV(vkGetPhysicalDeviceSurfaceCapabilities2KHR, VkResult, VkPhysicalDevice pd, const VkPhysicalDeviceSurfaceInfo2KHR *s, VkSurfaceCapabilities2KHR *c) {
    return h_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, s->surface, &c->surfaceCapabilities);
}
static const VkSurfaceFormatKHR surface_formats[] = {
    {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}, {VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
    {VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}, {VK_FORMAT_R8G8B8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}
};
GV(vkGetPhysicalDeviceSurfaceFormatsKHR, VkResult, VkPhysicalDevice pd, VkSurfaceKHR s, uint32_t *n, VkSurfaceFormatKHR *f) {
    (void)pd; (void)s;
    const uint32_t k = sizeof surface_formats / sizeof *surface_formats;
    if (!f) { *n = k; return VK_SUCCESS; }
    uint32_t m = *n < k ? *n : k; memcpy(f, surface_formats, m * sizeof *f); *n = m; return m < k ? VK_INCOMPLETE : VK_SUCCESS;
}
GV(vkGetPhysicalDeviceSurfaceFormats2KHR, VkResult, VkPhysicalDevice pd, const VkPhysicalDeviceSurfaceInfo2KHR *s, uint32_t *n, VkSurfaceFormat2KHR *f) {
    VkSurfaceFormatKHR formats[sizeof surface_formats / sizeof *surface_formats];
    if (f && *n > sizeof formats / sizeof *formats) *n = sizeof formats / sizeof *formats;
    VkResult result = h_vkGetPhysicalDeviceSurfaceFormatsKHR(pd, s->surface, n, f ? formats : NULL);
    if (f) for (uint32_t i = 0; i < *n; i++) f[i].surfaceFormat = formats[i];
    return result;
}
GV(vkGetPhysicalDeviceSurfacePresentModesKHR, VkResult, VkPhysicalDevice pd, VkSurfaceKHR s, uint32_t *n, VkPresentModeKHR *m) {
    (void)pd; (void)s; if (!m) { *n = 1; return VK_SUCCESS; }
    if (!*n) return VK_INCOMPLETE;
    *n = 1; m[0] = VK_PRESENT_MODE_FIFO_KHR; return VK_SUCCESS;
}

// ---------- Device and queues ----------
GV(vkEnumerateDeviceLayerProperties, VkResult, VkPhysicalDevice pd, uint32_t *n, VkLayerProperties *p) { (void)pd; (void)p; *n = 0; return VK_SUCCESS; }
GV(vkEnumerateDeviceExtensionProperties, VkResult, VkPhysicalDevice pd, const char *layer, uint32_t *n, VkExtensionProperties *p) {
    (void)pd; if (layer) { *n = 0; return VK_ERROR_LAYER_NOT_PRESENT; }
    return fill_names(g_dev_ext, sizeof g_dev_ext / sizeof *g_dev_ext, n, p);
}
// The device the game asks for is the replay's, which has every feature the phone has enabled (vka_dev.c): what the game enables is logged, and a Tegra feature the
// phone lacks is said once
GVB(vkCreateDevice, VkResult, VkPhysicalDevice pd, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *a, VkDevice *out) {
    (void)pd; (void)a;
    if (!gv_up()) return VK_ERROR_INITIALIZATION_FAILED;
    mem_model();
    char ext[512]; join_names(ext, sizeof ext, ci->ppEnabledExtensionNames, ci->enabledExtensionCount);
    uint32_t nq = 0; for (uint32_t i = 0; i < ci->queueCreateInfoCount; i++) nq += ci->pQueueCreateInfos[i].queueCount;
    const VkPhysicalDeviceFeatures *want = ci->pEnabledFeatures;
    for (const VkBaseInStructure *s = ci->pNext; s && !want; s = s->pNext) if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) want = &((const VkPhysicalDeviceFeatures2 *)s)->features;
    char miss[256] = ""; size_t mn = 0;
    if (want) {
        VkPhysicalDeviceFeatures r; vk.vkGetPhysicalDeviceFeatures(V.pd, &r);
        const VkBool32 *w = (const VkBool32 *)want, *h = (const VkBool32 *)&r;
        for (size_t i = 0; i < sizeof r / sizeof(VkBool32) && mn + 8 < sizeof miss; i++) if (w[i] && !h[i]) mn += (size_t)snprintf(miss + mn, sizeof miss - mn, " %zu", i);
    }
    GLOG("CreateDevice: %u queues; extensions %s; features the device lacks (VkPhysicalDeviceFeatures member index):%s", nq, ext, mn ? miss : " none");
    *out = V.dev; return VK_SUCCESS;
}
GV(vkDestroyDevice, void, VkDevice d, const VkAllocationCallbacks *a) { (void)d; (void)a; }   // the replay's device stays (the present keeps using it)
GV(vkGetDeviceQueue, void, VkDevice d, uint32_t fam, uint32_t idx, VkQueue *q) { (void)d; (void)fam; (void)idx; *q = V.q; }
GV(vkGetDeviceQueue2, void, VkDevice d, const VkDeviceQueueInfo2 *i, VkQueue *q) { (void)d; *q = i->flags ? VK_NULL_HANDLE : V.q; }   // (no protected queues)

// ---------- Memory ----------
GVB(vkAllocateMemory, VkResult, VkDevice d, const VkMemoryAllocateInfo *ai, const VkAllocationCallbacks *a, VkDeviceMemory *out) {
    GV_IN("vkAllocateMemory");
    (void)d; (void)a; mem_model();
    VkMemoryAllocateInfo c = *ai;
    if (ai->memoryTypeIndex >= 3) { GRED("vkAllocateMemory: memory type %u of 3", ai->memoryTypeIndex); return VK_ERROR_OUT_OF_DEVICE_MEMORY; }
    c.memoryTypeIndex = g_gtype[ai->memoryTypeIndex];
    VkResult r = R.vkAllocateMemory(V.dev, &c, NULL, out);
    if (r) GRED("vkAllocateMemory of %llu bytes (type %u, the device's %u): %d", (unsigned long long)ai->allocationSize, ai->memoryTypeIndex, c.memoryTypeIndex, r);
    else OLOG("memory %#llx: %llu bytes, type %u", (unsigned long long)(uintptr_t)*out, (unsigned long long)ai->allocationSize, ai->memoryTypeIndex);
    return r;
}
static void dedicated_req(void *pn) {   // a dedicated allocation is never required (the Switch's driver asks for none)
    for (VkBaseOutStructure *s = pn; s; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS) { VkMemoryDedicatedRequirements *m = (void *)s; m->prefersDedicatedAllocation = m->requiresDedicatedAllocation = VK_FALSE; }
}
GV(vkGetBufferMemoryRequirements, void, VkDevice d, VkBuffer b, VkMemoryRequirements *m) {
    (void)d; mem_model(); R.vkGetBufferMemoryRequirements(V.dev, b, m); m->memoryTypeBits = bits_g(m->memoryTypeBits);
}
GV(vkGetBufferMemoryRequirements2, void, VkDevice d, const VkBufferMemoryRequirementsInfo2 *i, VkMemoryRequirements2 *m) {
    (void)d; mem_model(); R.vkGetBufferMemoryRequirements2(V.dev, i, m); m->memoryRequirements.memoryTypeBits = bits_g(m->memoryRequirements.memoryTypeBits);
}
GV(vkGetImageMemoryRequirements, void, VkDevice d, VkImage im, VkMemoryRequirements *m) {
    (void)d; mem_model(); R.vkGetImageMemoryRequirements(V.dev, im, m); m->memoryTypeBits = bits_g(m->memoryTypeBits);
}
GV(vkGetImageMemoryRequirements2, void, VkDevice d, const VkImageMemoryRequirementsInfo2 *i, VkMemoryRequirements2 *m) {
    (void)d; mem_model(); R.vkGetImageMemoryRequirements2(V.dev, i, m); m->memoryRequirements.memoryTypeBits = bits_g(m->memoryRequirements.memoryTypeBits);
    dedicated_req(m->pNext);
}

// ---------- Images ----------
static pthread_mutex_t g_obj_mu = PTHREAD_MUTEX_INITIALIZER;   // the tables below
static gv_map g_swapimg;   // the images of the swapchains (value: 1)
// Formats the Switch's driver takes and the phone's may not (optional in Vulkan: three-component and scaled vertex formats, packed 4- and 5-bit texture formats):
// each format used in a way the device does not support is named once, with what it lacks
static void fmt_check(VkFormat f, int vertex, VkImageTiling tiling, VkImageUsageFlags usage) {
    enum { NFC = 64 }; static _Atomic uint64_t seen[NFC];   // (format, use) pairs already said
    VkFormatProperties p; vk.vkGetPhysicalDeviceFormatProperties(V.pd, f, &p);
    VkFormatFeatureFlags have = vertex ? p.bufferFeatures : tiling == VK_IMAGE_TILING_LINEAR ? p.linearTilingFeatures : p.optimalTilingFeatures, need = 0;
    if (vertex) need = VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;
    else {
        if (usage & VK_IMAGE_USAGE_SAMPLED_BIT) need |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        if (usage & VK_IMAGE_USAGE_STORAGE_BIT) need |= VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
        if (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) need |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
        if (usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) need |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
    }
    VkFormatFeatureFlags miss = need & ~have; if (!miss) return;
    uint64_t key = (uint64_t)f << 8 | (vertex ? 1 : 2 + tiling);
    for (int i = 0; i < NFC; i++) { uint64_t s = atomic_load(&seen[i]); if (s == key) return; if (!s) { uint64_t z = 0; if (atomic_compare_exchange_strong(&seen[i], &z, key)) break; if (atomic_load(&seen[i]) == key) return; } }
    GRED("format %d %s: the device lacks format features %#x (has %#x)", f, vertex ? "as a vertex attribute" : tiling == VK_IMAGE_TILING_LINEAR ? "in a linear image" : "in an optimal image", miss, have);
}
GVB(vkCreateImage, VkResult, VkDevice d, const VkImageCreateInfo *ci, const VkAllocationCallbacks *a, VkImage *out) {
    GV_IN("vkCreateImage");
    (void)d; (void)a; fmt_check(ci->format, 0, ci->tiling, ci->usage);
    VkResult r = R.vkCreateImage(V.dev, ci, NULL, out);
    if (r) {
        static _Atomic int n; if (atomic_fetch_add(&n, 1) < 16)
            GRED("vkCreateImage %ux%ux%u format %d type %d tiling %d usage %#x flags %#x levels %u layers %u samples %d: %d", ci->extent.width, ci->extent.height, ci->extent.depth,
                 ci->format, ci->imageType, ci->tiling, ci->usage, ci->flags, ci->mipLevels, ci->arrayLayers, ci->samples, r);
    } else OLOG("image %#llx: %ux%ux%u format %d tiling %d usage %#x", (unsigned long long)(uintptr_t)*out, ci->extent.width, ci->extent.height, ci->extent.depth, ci->format, ci->tiling, ci->usage);
    return r;
}
// A swapchain's images belong to it: a game that destroys them (Super Mario 64 on its way back to the launcher) is ignored, as the Switch's driver ignores it
GV(vkDestroyImage, void, VkDevice d, VkImage im, const VkAllocationCallbacks *a) {
    (void)d; (void)a; if (!im) return;
    pthread_mutex_lock(&g_obj_mu); int swap = gv_map_find(&g_swapimg, (uint64_t)(uintptr_t)im) >= 0; pthread_mutex_unlock(&g_obj_mu);
    if (swap) { GRED_ONCE("the game destroys an image of its swapchain: left to the swapchain"); return; }
    R.vkDestroyImage(V.dev, im, NULL);
}

// ---------- Render passes ----------
// NVIDIA's Switch driver keeps an attachment's contents when a pass opens it with LOAD_OP_DONT_CARE or ends it with STORE_OP_DONT_CARE (it discards nothing: upstream's
// reference, "Attachments opened with LOAD_OP_DONT_CARE"), and games draw over what such a pass left (Re:ZERO's post chain). A tiler's driver would start the tile
// memory undefined and drop what was drawn: LOAD and STORE instead. DM_VK_GUEST_KEEP=0 leaves them as the game made them
static int keep_on(void) { return VKA_ENV("DM_VK_GUEST_KEEP", 1) != 0; }
GVB(vkCreateRenderPass, VkResult, VkDevice d, const VkRenderPassCreateInfo *ci, const VkAllocationCallbacks *a, VkRenderPass *out) {
    GV_IN("vkCreateRenderPass");
    (void)d; (void)a;
    if (!keep_on() || !ci->attachmentCount) return R.vkCreateRenderPass(V.dev, ci, NULL, out);
    VkAttachmentDescription at[16], *p = ci->attachmentCount <= 16 ? at : malloc(ci->attachmentCount * sizeof *p);
    if (!p) return VK_ERROR_OUT_OF_HOST_MEMORY;
    for (uint32_t i = 0; i < ci->attachmentCount; i++) {
        p[i] = ci->pAttachments[i];
        if (p[i].loadOp == VK_ATTACHMENT_LOAD_OP_DONT_CARE) p[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        if (p[i].stencilLoadOp == VK_ATTACHMENT_LOAD_OP_DONT_CARE) p[i].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        if (p[i].storeOp == VK_ATTACHMENT_STORE_OP_DONT_CARE) p[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        if (p[i].stencilStoreOp == VK_ATTACHMENT_STORE_OP_DONT_CARE) p[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    VkRenderPassCreateInfo c = *ci; c.pAttachments = p;
    VkResult r = R.vkCreateRenderPass(V.dev, &c, NULL, out);
    if (p != at) free(p);
    return r;
}
GVB(vkCreateRenderPass2, VkResult, VkDevice d, const VkRenderPassCreateInfo2 *ci, const VkAllocationCallbacks *a, VkRenderPass *out) {
    (void)d; (void)a;
    if (!R.vkCreateRenderPass2) return VK_ERROR_EXTENSION_NOT_PRESENT;
    if (!keep_on() || !ci->attachmentCount) return R.vkCreateRenderPass2(V.dev, ci, NULL, out);
    VkAttachmentDescription2 at[16], *p = ci->attachmentCount <= 16 ? at : malloc(ci->attachmentCount * sizeof *p);
    if (!p) return VK_ERROR_OUT_OF_HOST_MEMORY;
    for (uint32_t i = 0; i < ci->attachmentCount; i++) {
        p[i] = ci->pAttachments[i];
        if (p[i].loadOp == VK_ATTACHMENT_LOAD_OP_DONT_CARE) p[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        if (p[i].stencilLoadOp == VK_ATTACHMENT_LOAD_OP_DONT_CARE) p[i].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        if (p[i].storeOp == VK_ATTACHMENT_STORE_OP_DONT_CARE) p[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        if (p[i].stencilStoreOp == VK_ATTACHMENT_STORE_OP_DONT_CARE) p[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    VkRenderPassCreateInfo2 c = *ci; c.pAttachments = p;
    VkResult r = R.vkCreateRenderPass2(V.dev, &c, NULL, out);
    if (p != at) free(p);
    return r;
}

// ---------- Descriptor pools and sets ----------
// A game frees descriptor sets whose pool it already reset or destroyed (Super Mario 64 as it leaves); the Switch's driver lets it pass, the phone's would free memory
// it no longer owns. Each pool has a generation (bumped by a reset); a set remembers its pool's and is freed only while both still hold
static gv_map g_pool, g_set;   // pool → generation; set → pool
static gv_map g_setgen;        // set → its pool's generation at allocation
GV(vkCreateDescriptorPool, VkResult, VkDevice d, const VkDescriptorPoolCreateInfo *ci, const VkAllocationCallbacks *a, VkDescriptorPool *out) {
    (void)d; (void)a; VkResult r = R.vkCreateDescriptorPool(V.dev, ci, NULL, out);
    if (!r) { pthread_mutex_lock(&g_obj_mu); gv_map_put(&g_pool, (uint64_t)(uintptr_t)*out, 1); pthread_mutex_unlock(&g_obj_mu); }
    return r;
}
GV(vkDestroyDescriptorPool, void, VkDevice d, VkDescriptorPool p, const VkAllocationCallbacks *a) {
    (void)d; (void)a; if (!p) return;
    pthread_mutex_lock(&g_obj_mu); gv_map_del(&g_pool, (uint64_t)(uintptr_t)p); pthread_mutex_unlock(&g_obj_mu);
    R.vkDestroyDescriptorPool(V.dev, p, NULL);
}
GV(vkResetDescriptorPool, VkResult, VkDevice d, VkDescriptorPool p, VkDescriptorPoolResetFlags f) {
    GV_IN("vkResetDescriptorPool");
    (void)d; pthread_mutex_lock(&g_obj_mu); int k = gv_map_find(&g_pool, (uint64_t)(uintptr_t)p); if (k >= 0) g_pool.v[k]++; pthread_mutex_unlock(&g_obj_mu);
    return R.vkResetDescriptorPool(V.dev, p, f);
}
GV(vkAllocateDescriptorSets, VkResult, VkDevice d, const VkDescriptorSetAllocateInfo *ai, VkDescriptorSet *out) {
    GV_IN("vkAllocateDescriptorSets");
    (void)d; VkResult r = R.vkAllocateDescriptorSets(V.dev, ai, out);
    if (!r) {
        pthread_mutex_lock(&g_obj_mu);
        int k = gv_map_find(&g_pool, (uint64_t)(uintptr_t)ai->descriptorPool); uint64_t gen = k >= 0 ? g_pool.v[k] : 0;
        for (uint32_t i = 0; i < ai->descriptorSetCount; i++) { gv_map_put(&g_set, (uint64_t)(uintptr_t)out[i], (uint64_t)(uintptr_t)ai->descriptorPool); gv_map_put(&g_setgen, (uint64_t)(uintptr_t)out[i], gen); }
        pthread_mutex_unlock(&g_obj_mu);
    }
    return r;
}
GV(vkFreeDescriptorSets, VkResult, VkDevice d, VkDescriptorPool p, uint32_t n, const VkDescriptorSet *sets) {
    GV_IN("vkFreeDescriptorSets");
    (void)d; VkDescriptorSet keep[64]; uint32_t nk = 0, gone = 0; VkResult r = VK_SUCCESS;
    pthread_mutex_lock(&g_obj_mu);
    int pk = gv_map_find(&g_pool, (uint64_t)(uintptr_t)p); uint64_t gen = pk >= 0 ? g_pool.v[pk] : 0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t s = (uint64_t)(uintptr_t)sets[i]; if (!s) continue;
        int k = gv_map_find(&g_set, s), kg = gv_map_find(&g_setgen, s);
        int live = pk >= 0 && k >= 0 && g_set.v[k] == (uint64_t)(uintptr_t)p && kg >= 0 && g_setgen.v[kg] == gen;
        if (k >= 0) gv_map_del(&g_set, s);
        if (kg >= 0) gv_map_del(&g_setgen, s);
        if (!live) { gone++; continue; }
        keep[nk++] = sets[i];
        if (nk == 64) { pthread_mutex_unlock(&g_obj_mu); r = R.vkFreeDescriptorSets(V.dev, p, nk, keep); pthread_mutex_lock(&g_obj_mu); nk = 0; }
    }
    pthread_mutex_unlock(&g_obj_mu);
    if (nk) r = R.vkFreeDescriptorSets(V.dev, p, nk, keep);
    if (gone) GRED_ONCE("the game frees descriptor sets whose pool it reset or destroyed (%u): ignored, as the Switch's driver does", gone);
    return r;
}

// ---------- Shaders and pipelines ----------
// SPIR-V a game made that names ArrayStride on a struct member (OpMemberDecorate) instead of on the array type (Hagi's generated GX shaders: Super Mario Galaxy, Sunshine):
// NVIDIA's driver takes it; the stride is copied onto the array type, after the last such member decoration (upstream's vk_shaders.py fix_strides). NULL: unchanged
static uint32_t *fix_strides(const uint32_t *w, size_t n, size_t *out_n) {
    enum { OP_DECORATE = 71, OP_MEMBER_DECORATE = 72, OP_TYPE_ARRAY = 28, OP_TYPE_RTARRAY = 29, OP_TYPE_STRUCT = 30, DEC_ARRAY_STRIDE = 6 };
    if (n < 5 || w[0] != 0x07230203u) return NULL;
    size_t at = 0, nmem = 0;
    for (size_t i = 5; i < n;) { uint32_t op = w[i] & 0xffff, len = w[i] >> 16; if (!len || i + len > n) return NULL;
        if (op == OP_MEMBER_DECORATE && len >= 5 && w[i + 3] == DEC_ARRAY_STRIDE) { at = i + len; nmem++; }
        i += len; }
    if (!nmem) return NULL;
    // (ids are below the header's bound: small tables by id)
    uint32_t bound = w[3]; if (!bound || bound > (1u << 22)) return NULL;
    uint8_t *is_arr = calloc(bound, 1), *has = calloc(bound, 1); uint32_t *add = calloc(bound, 4); const uint32_t **st = calloc(bound, sizeof *st); uint32_t *stn = calloc(bound, 4);
    uint32_t *res = NULL; size_t nadd = 0;
    if (!is_arr || !has || !add || !st || !stn) goto out;
    for (size_t i = 5; i < n; i += w[i] >> 16) { uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (op == OP_DECORATE && len >= 4 && w[i + 2] == DEC_ARRAY_STRIDE && w[i + 1] < bound) has[w[i + 1]] = 1;
        else if ((op == OP_TYPE_ARRAY || op == OP_TYPE_RTARRAY) && len >= 3 && w[i + 1] < bound) is_arr[w[i + 1]] = 1;
        else if (op == OP_TYPE_STRUCT && len >= 2 && w[i + 1] < bound) { st[w[i + 1]] = w + i + 2; stn[w[i + 1]] = len - 2; } }
    for (size_t i = 5; i < n; i += w[i] >> 16) { uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (op != OP_MEMBER_DECORATE || len < 5 || w[i + 3] != DEC_ARRAY_STRIDE || w[i + 1] >= bound) continue;
        uint32_t s = w[i + 1], m = w[i + 2]; if (!st[s] || m >= stn[s]) continue;
        uint32_t t = st[s][m]; if (t < bound && is_arr[t] && !has[t] && !add[t]) { add[t] = w[i + 4]; nadd++; } }
    if (!nadd) goto out;
    res = malloc((n + nadd * 4) * 4); if (!res) goto out;
    memcpy(res, w, at * 4); size_t o = at;
    for (uint32_t t = 0; t < bound; t++) if (add[t]) { res[o++] = (4u << 16) | OP_DECORATE; res[o++] = t; res[o++] = DEC_ARRAY_STRIDE; res[o++] = add[t]; }
    memcpy(res + o, w + at, (n - at) * 4); *out_n = n + nadd * 4;
out:
    free(is_arr); free(has); free(add); free(st); free(stn);
    return res;
}
// Scaled vertex formats the device cannot fetch (vka_guest_vin.c): a vertex module's code is kept while the module lives, so a pipeline that feeds one of its inputs in
// such a format gets a copy whose input reads the integer format and converts it
uint32_t *vkg_spv_scaled(const uint32_t *w, size_t n, const uint8_t conv[32], size_t *out_n, char *why, size_t wn);   // vka_guest_vin.c
uint32_t *vkg_spv_nonuniform(const uint32_t *w, size_t n, size_t *out_n, unsigned *kinds, char *why, size_t wn);   // vka_guest_vin.c
// The device's non-uniform descriptor indexing (bits as vkg_spv_nonuniform's kinds; the replay's device enables every feature it has, vka_dev.c)
static unsigned nu_have(void) {
    static int v = -1;
    if (v < 0) {
        VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f12};
        vk.vkGetPhysicalDeviceFeatures2(V.pd, &f2);
        v = (f12.shaderUniformBufferArrayNonUniformIndexing ? 1 : 0) | (f12.shaderStorageBufferArrayNonUniformIndexing ? 2 : 0) | (f12.shaderSampledImageArrayNonUniformIndexing ? 4 : 0) |
            (f12.shaderStorageImageArrayNonUniformIndexing ? 8 : 0);
        GLOG("non-uniform descriptor indexing on this device: uniform buffers %d, storage buffers %d, sampled images %d, storage images %d", v & 1, v >> 1 & 1, v >> 2 & 1, v >> 3 & 1);
    }
    return (unsigned)v;
}
static int g_vin_scaled = -1;   // 1: the device fetches every scaled format the check found (nothing kept), 0: some it cannot
static int scaled_of(VkFormat f, VkFormat *ifmt) {   // 1 USCALED, 2 SSCALED (*ifmt: its UINT / SINT twin, two values on in every group), 0 neither
    static const VkFormat us[] = {VK_FORMAT_R8_USCALED, VK_FORMAT_R8G8_USCALED, VK_FORMAT_R8G8B8_USCALED, VK_FORMAT_B8G8R8_USCALED, VK_FORMAT_R8G8B8A8_USCALED,
        VK_FORMAT_B8G8R8A8_USCALED, VK_FORMAT_A8B8G8R8_USCALED_PACK32, VK_FORMAT_A2R10G10B10_USCALED_PACK32, VK_FORMAT_A2B10G10R10_USCALED_PACK32, VK_FORMAT_R16_USCALED,
        VK_FORMAT_R16G16_USCALED, VK_FORMAT_R16G16B16_USCALED, VK_FORMAT_R16G16B16A16_USCALED};
    for (size_t i = 0; i < sizeof us / sizeof *us; i++) {
        if (f == us[i]) { *ifmt = (VkFormat)(f + 2); return 1; }
        if (f == us[i] + 1) { *ifmt = (VkFormat)(f + 2); return 2; }
    }
    return 0;
}
static int vtx_ok(VkFormat f) { VkFormatProperties p; vk.vkGetPhysicalDeviceFormatProperties(V.pd, f, &p); return (p.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) != 0; }
static int vin_scaled_needed(void) {
    if (g_vin_scaled < 0) {
        int all = 1; VkFormat i;
        for (int f = VK_FORMAT_R8_USCALED; f <= VK_FORMAT_R16G16B16A16_SSCALED; f++) if (scaled_of((VkFormat)f, &i) && !vtx_ok((VkFormat)f)) all = 0;
        g_vin_scaled = all;
        GLOG("scaled vertex formats: %s", all ? "the device fetches them" : "the device lacks some: attributes in them are fed as integers and converted in the vertex shader");
    }
    return !g_vin_scaled;
}
static gv_map g_vcode;   // vertex module → its code (malloc'd: u64 word count, then the words)
static int has_vertex_entry(const uint32_t *w, size_t n) {
    for (size_t i = 5; i < n && (w[i] >> 16);) { if ((w[i] & 0xffff) == 15 && (w[i] >> 16) >= 3 && w[i + 1] == 0) return 1; if ((w[i] & 0xffff) == 54) break; i += w[i] >> 16; }
    return 0;
}
GVB(vkCreateShaderModule, VkResult, VkDevice d, const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *a, VkShaderModule *out) {
    GV_IN("vkCreateShaderModule");
    (void)d; (void)a;
    size_t nn = 0; uint32_t *fixed = fix_strides(ci->pCode, ci->codeSize / 4, &nn);
    VkShaderModuleCreateInfo c = *ci; if (fixed) { c.pCode = fixed; c.codeSize = nn * 4; GRED_ONCE("SPIR-V with ArrayStride on struct members (the Switch's driver takes it): the strides copied onto the array types"); }
    {   // descriptor arrays indexed per invocation: declared NonUniform (vka_guest_vin.c)
        size_t nu_n = 0; unsigned kinds = 0; char why[96];
        uint32_t *nu = vkg_spv_nonuniform(c.pCode, c.codeSize / 4, &nu_n, &kinds, why, sizeof why);
        if (nu && (kinds & ~nu_have())) { GRED_ONCE("a shader indexes descriptor arrays per invocation (kinds %#x) where the device cannot (%#x): left as made", kinds, nu_have()); free(nu); nu = NULL; }
        if (nu) { free(fixed); fixed = nu; c.pCode = nu; c.codeSize = nu_n * 4; static _Atomic int told; if (!atomic_exchange(&told, 1)) GLOG("descriptor arrays indexed per invocation (kinds %#x): the accesses decorated NonUniform (and every such shader from now on)", kinds); }
        else if (why[0]) GRED_ONCE("non-uniform descriptor indexing not declared: %s", why);
    }
    { static const char *cap; static int capn; if (!capn) { cap = getenv("DM_VK_GUEST_CAPTURE"); capn = 1; }   // DM_VK_GUEST_CAPTURE=<folder>: every module the game makes, as <hash>.spv
      if (cap && *cap) { uint64_t h = 1469598103934665603ull; const uint8_t *b = (const uint8_t *)c.pCode; for (size_t i = 0; i < c.codeSize; i++) h = (h ^ b[i]) * 1099511628211ull;
          char p[600]; snprintf(p, sizeof p, "%s/%016llx.spv", cap, (unsigned long long)h); FILE *fo = fopen(p, "wb"); if (fo) { fwrite(c.pCode, 1, c.codeSize, fo); fclose(fo); } } }
    uint64_t f = vka_fp_host(NULL); VkResult r = R.vkCreateShaderModule(V.dev, &c, NULL, out); vka_fp_back(f);
    if (!r && vin_scaled_needed() && has_vertex_entry(c.pCode, c.codeSize / 4)) {
        uint64_t *keep = malloc(8 + c.codeSize);
        if (keep) { keep[0] = c.codeSize / 4; memcpy(keep + 1, c.pCode, c.codeSize); pthread_mutex_lock(&g_obj_mu); gv_map_put(&g_vcode, (uint64_t)(uintptr_t)*out, (uint64_t)(uintptr_t)keep); pthread_mutex_unlock(&g_obj_mu); }
    }
    free(fixed);
    static _Atomic uint64_t nmod; uint64_t k = atomic_fetch_add(&nmod, 1) + 1;
    if (r) GRED("vkCreateShaderModule (%zu bytes): %d", ci->codeSize, r);
    else if (k == 1 || !(k & (k - 1))) GLOG("shader modules so far: %llu", (unsigned long long)k);
    return r;
}
GV(vkDestroyShaderModule, void, VkDevice d, VkShaderModule m, const VkAllocationCallbacks *a) {
    (void)d; (void)a; if (!m) return;
    pthread_mutex_lock(&g_obj_mu); int k = gv_map_find(&g_vcode, (uint64_t)(uintptr_t)m); void *keep = k >= 0 ? (void *)(uintptr_t)g_vcode.v[k] : NULL; if (k >= 0) gv_map_del(&g_vcode, (uint64_t)(uintptr_t)m); pthread_mutex_unlock(&g_obj_mu);
    free(keep); R.vkDestroyShaderModule(V.dev, m, NULL);
}
// A pipeline whose vertex input has a scaled format the device cannot fetch: the copies it is made from (its stage array, vertex input state, attributes) and the
// rewritten vertex module, destroyed once the pipeline is made. 0 = nothing to change (or the rewrite failed: said once, the pipeline made as the game asked)
typedef struct { VkPipelineShaderStageCreateInfo st[8]; VkPipelineVertexInputStateCreateInfo vi; VkVertexInputAttributeDescription at[32]; VkShaderModule mod; } vin_fix;
static int pso_fix_scaled(VkGraphicsPipelineCreateInfo *ci, vin_fix *fx) {
    const VkPipelineVertexInputStateCreateInfo *vi = ci->pVertexInputState;
    if (!vi || !vi->vertexAttributeDescriptionCount || vi->vertexAttributeDescriptionCount > 32 || ci->stageCount > 8 || !vin_scaled_needed()) return 0;
    uint8_t conv[32] = {0}; int any = 0; VkFormat ifmt[32];
    for (uint32_t j = 0; j < vi->vertexAttributeDescriptionCount; j++) {
        const VkVertexInputAttributeDescription *d = &vi->pVertexAttributeDescriptions[j];
        int s = d->location < 32 ? scaled_of(d->format, &ifmt[j]) : 0;
        if (s && !vtx_ok(d->format)) { if (!vtx_ok(ifmt[j])) { GRED_ONCE("vertex format %d: neither it nor its integer twin %d can be fetched here", d->format, ifmt[j]); return 0; } conv[d->location] = (uint8_t)s; any = 1; }
    }
    if (!any) return 0;
    int vs = -1; for (uint32_t k = 0; k < ci->stageCount; k++) if (ci->pStages[k].stage == VK_SHADER_STAGE_VERTEX_BIT) vs = (int)k;
    if (vs < 0) return 0;
    pthread_mutex_lock(&g_obj_mu); int k = gv_map_find(&g_vcode, (uint64_t)(uintptr_t)ci->pStages[vs].module); const uint64_t *keep = k >= 0 ? (const uint64_t *)(uintptr_t)g_vcode.v[k] : NULL; pthread_mutex_unlock(&g_obj_mu);
    if (!keep) { GRED_ONCE("a pipeline feeds a scaled vertex format the device lacks, and its vertex module's code was not kept: drawn as made"); return 0; }
    char why[128]; size_t nn = 0; uint32_t *code = vkg_spv_scaled((const uint32_t *)(keep + 1), (size_t)keep[0], conv, &nn, why, sizeof why);
    if (!code) { if (why[0]) GRED_ONCE("scaled vertex input not rewritten: %s (drawn as made)", why); return 0; }
    VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = nn * 4, .pCode = code};
    uint64_t f = vka_fp_host(NULL); VkResult r = R.vkCreateShaderModule(V.dev, &mi, NULL, &fx->mod); vka_fp_back(f);
    free(code);
    if (r) { GRED_ONCE("scaled vertex input: the rewritten module was refused: %d (drawn as made)", r); return 0; }
    memcpy(fx->st, ci->pStages, ci->stageCount * sizeof *fx->st); fx->st[vs].module = fx->mod; ci->pStages = fx->st;
    fx->vi = *vi; memcpy(fx->at, vi->pVertexAttributeDescriptions, vi->vertexAttributeDescriptionCount * sizeof *fx->at); fx->vi.pVertexAttributeDescriptions = fx->at;
    for (uint32_t j = 0; j < vi->vertexAttributeDescriptionCount; j++) if (fx->at[j].location < 32 && conv[fx->at[j].location]) fx->at[j].format = ifmt[j];
    ci->pVertexInputState = &fx->vi;
    static _Atomic int told; if (!atomic_exchange(&told, 1)) GLOG("scaled vertex input: a pipeline's scaled attributes fed as integers and converted in its vertex shader (and every such pipeline from now on)");
    return 1;
}
// Query ranges past the pool: Hagi resets its occlusion pool from the query in use to the pool's end, and when its index wraps it asks for a count of -2169 (first 0,
// count 0xfffff787 of a 2,176-query pool, Super Mario Sunshine at Delfino Airstrip). NVIDIA's driver ignores queries outside the pool; Qualcomm's wrote past its query
// memory (SIGSEGV in vulkan.adreno.so in vkCmdResetQueryPool, ~20 s into play). Queries outside a pool do not exist: every range is cut to the pool, a query outside it skipped
static gv_map g_qpool;   // query pool → its query count
static uint32_t qpool_n(VkQueryPool p) { pthread_mutex_lock(&g_obj_mu); int k = gv_map_find(&g_qpool, (uint64_t)(uintptr_t)p); uint32_t n = k >= 0 ? (uint32_t)g_qpool.v[k] : UINT32_MAX; pthread_mutex_unlock(&g_obj_mu); return n; }
static int qrange(VkQueryPool p, uint32_t *first, uint32_t *n, const char *who) {   // 0: nothing of the range is in the pool
    uint32_t size = qpool_n(p); if (size == UINT32_MAX) return 1;
    uint64_t end = (uint64_t)*first + *n;
    if (*first < size && end <= size) return 1;
    static _Atomic int told; if (atomic_fetch_add(&told, 1) < 4) GRED("%s: queries %u + %u of a pool of %u (the Switch's driver ignores those outside the pool): cut to the pool", who, *first, *n, size);
    if (*first >= size) return 0;
    *n = size - *first; return 1;
}
GV(vkCreateQueryPool, VkResult, VkDevice d, const VkQueryPoolCreateInfo *ci, const VkAllocationCallbacks *a, VkQueryPool *out) {
    (void)d; (void)a; VkResult r = R.vkCreateQueryPool(V.dev, ci, NULL, out);
    if (!r) { pthread_mutex_lock(&g_obj_mu); gv_map_put(&g_qpool, (uint64_t)(uintptr_t)*out, ci->queryCount); pthread_mutex_unlock(&g_obj_mu); }
    OLOG("query pool %#llx: type %d, %u queries: %d", (unsigned long long)(uintptr_t)*out, ci->queryType, ci->queryCount, r);
    return r;
}
GV(vkDestroyQueryPool, void, VkDevice d, VkQueryPool p, const VkAllocationCallbacks *a) {
    (void)d; (void)a; if (!p) return;
    pthread_mutex_lock(&g_obj_mu); gv_map_del(&g_qpool, (uint64_t)(uintptr_t)p); pthread_mutex_unlock(&g_obj_mu);
    R.vkDestroyQueryPool(V.dev, p, NULL);
}
GV(vkCmdResetQueryPool, void, VkCommandBuffer cb, VkQueryPool p, uint32_t first, uint32_t n) {
    GV_IN("vkCmdResetQueryPool"); if (qrange(p, &first, &n, "vkCmdResetQueryPool") && n) R.vkCmdResetQueryPool(cb, p, first, n);
}
GV(vkResetQueryPool, void, VkDevice d, VkQueryPool p, uint32_t first, uint32_t n) {
    (void)d; if (qrange(p, &first, &n, "vkResetQueryPool") && n) R.vkResetQueryPool(V.dev, p, first, n);
}
GV(vkCmdBeginQuery, void, VkCommandBuffer cb, VkQueryPool p, uint32_t q, VkQueryControlFlags f) {
    GV_IN("vkCmdBeginQuery"); uint32_t one = 1; if (qrange(p, &q, &one, "vkCmdBeginQuery")) R.vkCmdBeginQuery(cb, p, q, f);
}
GV(vkCmdEndQuery, void, VkCommandBuffer cb, VkQueryPool p, uint32_t q) {
    GV_IN("vkCmdEndQuery"); uint32_t one = 1; if (qrange(p, &q, &one, "vkCmdEndQuery")) R.vkCmdEndQuery(cb, p, q);
}
GV(vkCmdCopyQueryPoolResults, void, VkCommandBuffer cb, VkQueryPool p, uint32_t first, uint32_t n, VkBuffer b, VkDeviceSize off, VkDeviceSize stride, VkQueryResultFlags f) {
    if (qrange(p, &first, &n, "vkCmdCopyQueryPoolResults") && n) R.vkCmdCopyQueryPoolResults(cb, p, first, n, b, off, stride, f);
}
GV(vkGetQueryPoolResults, VkResult, VkDevice d, VkQueryPool p, uint32_t first, uint32_t n, size_t size, void *data, VkDeviceSize stride, VkQueryResultFlags f) {
    (void)d; if (!qrange(p, &first, &n, "vkGetQueryPoolResults") || !n) return VK_SUCCESS;
    return R.vkGetQueryPoolResults(V.dev, p, first, n, size, data, stride, f);
}
// A pipeline with a logic op where the device has none: drawn without it, said once (Hagi makes a dozen VK_LOGIC_OP_INVERT pipelines at boot)
static int pso_fix_logic(VkGraphicsPipelineCreateInfo *ci, VkPipelineColorBlendStateCreateInfo *cb) {
    if (V.logic_op || !ci->pColorBlendState || !ci->pColorBlendState->logicOpEnable) return 0;
    *cb = *ci->pColorBlendState; cb->logicOpEnable = VK_FALSE; ci->pColorBlendState = cb;
    GRED_ONCE("a pipeline with logic op %d: the device has no logic ops, drawn without", cb->logicOp);
    return 1;
}
GVB(vkCreateGraphicsPipelines, VkResult, VkDevice d, VkPipelineCache c, uint32_t n, const VkGraphicsPipelineCreateInfo *ci, const VkAllocationCallbacks *a, VkPipeline *out) {
    GV_IN("vkCreateGraphicsPipelines");
    (void)d; (void)a;
    VkGraphicsPipelineCreateInfo *cc = malloc(n * sizeof *cc); VkPipelineColorBlendStateCreateInfo *cbs = malloc(n * sizeof *cbs); vin_fix *fx = calloc(n, sizeof *fx);
    if (cc && cbs && fx) {
        memcpy(cc, ci, n * sizeof *cc);
        for (uint32_t j = 0; j < n; j++) { pso_fix_logic(&cc[j], &cbs[j]); pso_fix_scaled(&cc[j], &fx[j]); }
        ci = cc;
    }
    for (uint32_t i = 0; i < n; i++) { const VkPipelineVertexInputStateCreateInfo *vi = ci[i].pVertexInputState;
        for (uint32_t j = 0; vi && j < vi->vertexAttributeDescriptionCount; j++) fmt_check(vi->pVertexAttributeDescriptions[j].format, 1, 0, 0); }
    uint64_t f = vka_fp_host(NULL); VkResult r = R.vkCreateGraphicsPipelines(V.dev, c, n, ci, NULL, out); vka_fp_back(f);
    if (fx) for (uint32_t j = 0; j < n; j++) if (fx[j].mod) R.vkDestroyShaderModule(V.dev, fx[j].mod, NULL);   // (a pipeline does not need its modules once made)
    free(cc); free(cbs); free(fx);
    static _Atomic uint64_t npso; uint64_t k = atomic_fetch_add(&npso, n) + n;
    if (r) { static _Atomic int nr; if (atomic_fetch_add(&nr, 1) < 16) GRED("vkCreateGraphicsPipelines (%u): %d", n, r); }
    else if (k <= n || !(k & (k - 1)) || (k & ~(k - 1)) < n) OLOG("graphics pipelines so far: %llu", (unsigned long long)k);
    return r;
}
GVB(vkCreateComputePipelines, VkResult, VkDevice d, VkPipelineCache c, uint32_t n, const VkComputePipelineCreateInfo *ci, const VkAllocationCallbacks *a, VkPipeline *out) {
    GV_IN("vkCreateComputePipelines");
    (void)d; (void)a;
    uint64_t f = vka_fp_host(NULL); VkResult r = R.vkCreateComputePipelines(V.dev, c, n, ci, NULL, out); vka_fp_back(f);
    if (r) { static _Atomic int nr; if (atomic_fetch_add(&nr, 1) < 16) GRED("vkCreateComputePipelines (%u): %d", n, r); }
    return r;
}

// ---------- Queues ----------
// The game's queues are the replay's one queue, which the present thread and the replay also submit to: every call takes its lock
static uint64_t g_nsubmit;
GVB(vkQueueSubmit, VkResult, VkQueue q, uint32_t n, const VkSubmitInfo *si, VkFence f) {
    GV_IN("vkQueueSubmit");
    (void)q; dm_gfx_enter();
    pthread_mutex_lock(&V.qmu);
    VkResult r = vka_hung() ? VK_ERROR_DEVICE_LOST : R.vkQueueSubmit(V.q, n, si, f);
    g_nsubmit++;
    pthread_mutex_unlock(&V.qmu);
    dm_gfx_leave();
    if (r == VK_ERROR_DEVICE_LOST) vka_device_lost("the game's vkQueueSubmit");
    else if (r) { static _Atomic int k; if (atomic_fetch_add(&k, 1) < 8) GRED("vkQueueSubmit (%u batches): %d", n, r); }
    return r;
}
GVB(vkQueueSubmit2, VkResult, VkQueue q, uint32_t n, const VkSubmitInfo2 *si, VkFence f) {
    GV_IN("vkQueueSubmit2");
    (void)q; if (!R.vkQueueSubmit2) return VK_ERROR_EXTENSION_NOT_PRESENT;
    dm_gfx_enter(); pthread_mutex_lock(&V.qmu);
    VkResult r = vka_hung() ? VK_ERROR_DEVICE_LOST : R.vkQueueSubmit2(V.q, n, si, f); g_nsubmit++;
    pthread_mutex_unlock(&V.qmu); dm_gfx_leave();
    if (r == VK_ERROR_DEVICE_LOST) vka_device_lost("the game's vkQueueSubmit2");
    return r;
}
GVB(vkQueueBindSparse, VkResult, VkQueue q, uint32_t n, const VkBindSparseInfo *bi, VkFence f) {
    (void)q; pthread_mutex_lock(&V.qmu); VkResult r = R.vkQueueBindSparse(V.q, n, bi, f); pthread_mutex_unlock(&V.qmu); return r;
}
GVB(vkQueueWaitIdle, VkResult, VkQueue q) {
    GV_IN("vkQueueWaitIdle");
    (void)q; if (vka_hung()) return VK_ERROR_DEVICE_LOST;
    pthread_mutex_lock(&V.qmu); VkResult r = R.vkQueueWaitIdle(V.q); pthread_mutex_unlock(&V.qmu); return r;
}
GVB(vkDeviceWaitIdle, VkResult, VkDevice d) { (void)d; return h_vkQueueWaitIdle(V.q); }
GVB(vkWaitForFences, VkResult, VkDevice d, uint32_t n, const VkFence *f, VkBool32 all, uint64_t timeout) {
    GV_IN("vkWaitForFences");
    (void)d; if (vka_hung()) return VK_ERROR_DEVICE_LOST;
    return R.vkWaitForFences(V.dev, n, f, all, timeout);
}
GVB(vkWaitSemaphores, VkResult, VkDevice d, const VkSemaphoreWaitInfo *wi, uint64_t timeout) {
    (void)d; if (vka_hung()) return VK_ERROR_DEVICE_LOST;
    return R.vkWaitSemaphores(V.dev, wi, timeout);
}

// ---------- Swapchains and presents ----------
// A swapchain's images are made here as vka_img (the present shows them as it shows the NVN window texture). An image goes round GENERAL (where the present reads it)
// → PRESENT_SRC (acquired: the game's first barrier may take it from PRESENT_SRC or UNDEFINED) → the game's drawing → PRESENT_SRC (presented) → GENERAL: two command
// buffers per image do the transitions, recorded once
enum { GV_NSWAP = 4 };
typedef struct {
    uint32_t n, next, w, h; VkFormat fmt;
    vka_img *im[GV_NSWAP]; VkCommandBuffer acq[GV_NSWAP], hand[GV_NSWAP]; uint64_t ticket[GV_NSWAP];
    VkCommandPool pool;
} gv_swap;
static VkFormat unorm_twin(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32: return VK_FORMAT_A8B8G8R8_UNORM_PACK32;
    default: return f;
    }
}
static VkFormat srgb_twin(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_SRGB;
    case VK_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_SRGB;
    case VK_FORMAT_A8B8G8R8_UNORM_PACK32: return VK_FORMAT_A8B8G8R8_SRGB_PACK32;
    default: return VK_FORMAT_UNDEFINED;
    }
}
static void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .oldLayout = from, .newLayout = to, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    R.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}
static VkCommandBuffer record(VkCommandPool pool, VkImage img, VkImageLayout from, VkImageLayout to, VkCommandBufferUsageFlags use) {
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (R.vkAllocateCommandBuffers(V.dev, &ai, &cb)) return VK_NULL_HANDLE;
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = use};
    R.vkBeginCommandBuffer(cb, &bi); barrier(cb, img, from, to); R.vkEndCommandBuffer(cb);
    return cb;
}
static vka_img *swap_img(const VkSwapchainCreateInfoKHR *ci, uint32_t k) {
    VkFormat fmt = unorm_twin(ci->imageFormat), srgb = srgb_twin(fmt);
    vka_img *im = calloc(1, sizeof *im);
    if (!im) return NULL;
    im->serial = 0xF7000000ull + k; im->refs = 1; im->inited = 1; im->gpu_owned = 1;
    im->f = (vka_fmt){fmt, fmt, 0, 0, 4, 1, 1};
    im->d.target = NVN_TEXTURE_TARGET_2D; im->d.width = ci->imageExtent.width; im->d.height = ci->imageExtent.height;
    VkImageUsageFlags use = ci->imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VkImageCreateFlags fl = srgb ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0;
    VkImageFormatProperties ifp;
    if ((use & VK_IMAGE_USAGE_STORAGE_BIT) && vk.vkGetPhysicalDeviceImageFormatProperties(V.pd, fmt, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, use, fl, &ifp)) {
        GRED_ONCE("swapchain format %d cannot be a storage image here: made without storage use", ci->imageFormat); use &= ~VK_IMAGE_USAGE_STORAGE_BIT; }
    im->usage = use; im->aspect = VK_IMAGE_ASPECT_COLOR_BIT; im->w = ci->imageExtent.width; im->h = ci->imageExtent.height; im->depth = im->layers = im->levels = im->samples = 1;
    VkFormat vf[2] = {fmt, srgb}; VkImageFormatListCreateInfo fli = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, .viewFormatCount = 2, .pViewFormats = vf};
    VkImageCreateInfo ic = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = srgb ? &fli : NULL, .flags = fl, .imageType = VK_IMAGE_TYPE_2D, .format = fmt,
        .extent = {im->w, im->h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = use};
    VkResult r = R.vkCreateImage(V.dev, &ic, NULL, &im->img);
    VkMemoryRequirements mr = {0}; if (!r) R.vkGetImageMemoryRequirements(V.dev, im->img, &mr);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = vka_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0)};
    if (!r && ai.memoryTypeIndex == UINT32_MAX) r = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    if (!r) r = R.vkAllocateMemory(V.dev, &ai, NULL, &im->m.dm);   // its own allocation (size 0 in vka_imem: not in the image memory stats)
    if (!r) r = vk.vkBindImageMemory(V.dev, im->img, im->m.dm, 0);
    if (r) { GRED("swapchain image %ux%u format %d: %d", im->w, im->h, ci->imageFormat, r); vka_tex_drop(im); return NULL; }
    return im;
}
static void swap_free(gv_swap *sc) {
    for (uint32_t i = 0; i < sc->n; i++) if (sc->ticket[i]) mtl_present_wait(sc->ticket[i]);   // the present thread has handed its last copy of each to the queue
    h_vkQueueWaitIdle(V.q);   // ... and the queue has run them, and the transitions (their command buffers go with the pool)
    pthread_mutex_lock(&g_obj_mu);
    for (uint32_t i = 0; i < sc->n; i++) if (sc->im[i]) gv_map_del(&g_swapimg, (uint64_t)(uintptr_t)sc->im[i]->img);
    pthread_mutex_unlock(&g_obj_mu);
    for (uint32_t i = 0; i < sc->n; i++) vka_tex_drop(sc->im[i]);   // destroyed once the fences that used them are done (vka_tex.c unref_locked)
    if (sc->pool) R.vkDestroyCommandPool(V.dev, sc->pool, NULL);
    free(sc);
}
GVB(vkCreateSwapchainKHR, VkResult, VkDevice d, const VkSwapchainCreateInfoKHR *ci, const VkAllocationCallbacks *a, VkSwapchainKHR *out) {
    GV_IN("vkCreateSwapchainKHR");
    (void)d; (void)a; if (!gv_up()) return VK_ERROR_INITIALIZATION_FAILED;
    gv_swap *sc = calloc(1, sizeof *sc); if (!sc) return VK_ERROR_OUT_OF_HOST_MEMORY;
    sc->n = ci->minImageCount < 2 ? 2 : ci->minImageCount > GV_NSWAP ? GV_NSWAP : ci->minImageCount;
    sc->w = ci->imageExtent.width; sc->h = ci->imageExtent.height; sc->fmt = ci->imageFormat;
    VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = V.qf};
    if (R.vkCreateCommandPool(V.dev, &pi, NULL, &sc->pool)) { free(sc); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    VkCommandBuffer init[GV_NSWAP] = {0};
    for (uint32_t i = 0; i < sc->n; i++) {
        if (!(sc->im[i] = swap_img(ci, i))) { sc->n = i; swap_free(sc); return VK_ERROR_OUT_OF_DEVICE_MEMORY; }
        VkImage img = sc->im[i]->img;
        init[i] = record(sc->pool, img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
        sc->acq[i] = record(sc->pool, img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT);
        sc->hand[i] = record(sc->pool, img, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_GENERAL, VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT);
        if (!init[i] || !sc->acq[i] || !sc->hand[i]) { sc->n = i + 1; swap_free(sc); return VK_ERROR_OUT_OF_HOST_MEMORY; }
        pthread_mutex_lock(&g_obj_mu); gv_map_put(&g_swapimg, (uint64_t)(uintptr_t)img, 1); pthread_mutex_unlock(&g_obj_mu);
    }
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = sc->n, .pCommandBuffers = init};
    pthread_mutex_lock(&V.qmu); VkResult r = R.vkQueueSubmit(V.q, 1, &si, VK_NULL_HANDLE); pthread_mutex_unlock(&V.qmu);
    if (r) GRED("swapchain: the images' first transition: %d", r);
    GLOG("swapchain %ux%u format %d, %u images (at least %u asked for), usage %#x, present mode %d, transform %#x", sc->w, sc->h, ci->imageFormat, sc->n, ci->minImageCount,
         ci->imageUsage, ci->presentMode, ci->preTransform);
    *out = (VkSwapchainKHR)(uintptr_t)sc; return VK_SUCCESS;
}
GVB(vkDestroySwapchainKHR, void, VkDevice d, VkSwapchainKHR s, const VkAllocationCallbacks *a) { (void)d; (void)a; if (s) swap_free((gv_swap *)(uintptr_t)s); }
GV(vkGetSwapchainImagesKHR, VkResult, VkDevice d, VkSwapchainKHR s, uint32_t *n, VkImage *out) {
    (void)d; gv_swap *sc = (gv_swap *)(uintptr_t)s;
    if (!out) { *n = sc->n; return VK_SUCCESS; }
    uint32_t k = *n < sc->n ? *n : sc->n; for (uint32_t i = 0; i < k; i++) out[i] = sc->im[i]->img;
    *n = k; return k < sc->n ? VK_INCOMPLETE : VK_SUCCESS;
}
// FIFO: at most one image a display refresh. The acquire waits until the image's previous present has been handed to the queue (its copy queued before the
// transition that gives it back to the game) and paces itself to the host's frame rate (60, DM_FPS, the frame rate cap) on its own clock
static uint64_t g_next_acq;
GVB(vkAcquireNextImageKHR, VkResult, VkDevice d, VkSwapchainKHR s, uint64_t timeout, VkSemaphore sem, VkFence f, uint32_t *idx) {
    GV_IN("vkAcquireNextImageKHR");
    (void)d; (void)timeout; gv_swap *sc = (gv_swap *)(uintptr_t)s;
    uint32_t i = sc->next++ % sc->n;
    int fps = mtl_fps_target(); int nxhost_fps_limit_get(void) __attribute__((weak));
    { int lim = &nxhost_fps_limit_get ? nxhost_fps_limit_get() : 0; if (lim > 0 && lim < fps) fps = lim; }
    uint64_t period = 1000000000ull / (uint64_t)(fps > 0 ? fps : 60), now = now_ns();
    if (g_next_acq > now + 2 * period) g_next_acq = now;   // (a clock jump, a long stop)
    if (g_next_acq > now) { uint64_t w = g_next_acq - now; struct timespec ts = {(time_t)(w / 1000000000ull), (long)(w % 1000000000ull)}; nanosleep(&ts, NULL); }
    now = now_ns(); g_next_acq = g_next_acq && now < g_next_acq + period ? g_next_acq + period : now + period;
    if (sc->ticket[i]) mtl_present_wait(sc->ticket[i]);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &sc->acq[i], .signalSemaphoreCount = sem ? 1 : 0, .pSignalSemaphores = &sem};
    pthread_mutex_lock(&V.qmu); VkResult r = vka_hung() ? VK_ERROR_DEVICE_LOST : R.vkQueueSubmit(V.q, 1, &si, f); pthread_mutex_unlock(&V.qmu);
    if (r) { GRED_ONCE("vkAcquireNextImageKHR: the transition's submission: %d", r); return r; }
    *idx = i; return VK_SUCCESS;
}
uint64_t vka_present_guest(vka_img *im, double period_s, const int crop[4], uint64_t fence);   // vka_present.c
static pthread_mutex_t g_pres_mu = PTHREAD_MUTEX_INITIALIZER;   // a present's fence reserved and signalled in order
static uint64_t g_frames;
GVB(vkQueuePresentKHR, VkResult, VkQueue q, const VkPresentInfoKHR *pi) {
    GV_IN("vkQueuePresentKHR");
    (void)q; dm_gfx_enter();
    int crop[4]; dm_vi_layer_crop(crop);   // the vi layer crop (hle_vi.c): Super Mario Galaxy shows the top-left 1280x720 of its 1920x1080 swapchain in handheld mode
    double period = 1.0 / mtl_fps_target();
    VkResult res = VK_SUCCESS;
    pthread_mutex_lock(&g_pres_mu);
    for (uint32_t i = 0; i < pi->swapchainCount; i++) {
        gv_swap *sc = (gv_swap *)(uintptr_t)pi->pSwapchains[i]; uint32_t k = pi->pImageIndices[i];
        if (k >= sc->n) { if (pi->pResults) pi->pResults[i] = VK_ERROR_OUT_OF_DATE_KHR; res = VK_ERROR_OUT_OF_DATE_KHR; continue; }
        uint32_t nw = i ? 0 : pi->waitSemaphoreCount; VkSemaphore ws[8]; uint64_t wv[8] = {0}; VkPipelineStageFlags st[8];
        if (nw > 8) { GRED_ONCE("a present waits for %u semaphores: 8 taken", nw); nw = 8; }
        for (uint32_t j = 0; j < nw; j++) { ws[j] = pi->pWaitSemaphores[j]; st[j] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT; }
        uint64_t fence = vka_fence_next();
        VkResult r = vka_submit(sc->hand[k], fence, nw, ws, wv, st, VK_NULL_HANDLE);
        if (r) { GRED_ONCE("vkQueuePresentKHR: the hand-over's submission: %d", r); vka_host_signal(V.tl, fence); vka_fence_cpu(fence, NULL, 0, NULL); }
        else vka_fence_post(fence, NULL, 0, NULL);
        int fit = crop[2] > 0 && crop[3] > 0 && (uint32_t)(crop[0] + crop[2]) <= sc->w && (uint32_t)(crop[1] + crop[3]) <= sc->h;
        sc->ticket[k] = vka_present_guest(sc->im[k], period, fit ? crop : NULL, fence);
        if (pi->pResults) pi->pResults[i] = r;
        if (r) res = r;
    }
    pthread_mutex_unlock(&g_pres_mu);
    dm_gfx_leave();
    uint64_t fr = ++g_frames;
    if (fr <= 3 || fr % 600 == 0) GLOG("frame %llu presented (%llu submissions so far)", (unsigned long long)fr, (unsigned long long)g_nsubmit);
    return res == VK_ERROR_DEVICE_LOST ? res : VK_SUCCESS;
}

// ---------- Function lookup ----------
// A name the table here has (a name ending in KHR also without it: extensions promoted to core); otherwise the driver's own entry point for a device function the
// game's real handles can call, logged once by name
static void *gv_lookup(const char *name, int device) {
    (void)device;
    if (!name || strncmp(name, "vk", 2)) return NULL;
    static const hle_ent *sec; static unsigned long sz;
    if (!sec) sec = (const hle_ent *)plat_table("swhle", &sz);
    void *fn = NULL; size_t n = strlen(name);
    for (int pass = 0; pass < 2 && !fn; pass++) {
        size_t len = pass ? n - 3 : n;
        if (pass && (n < 4 || strcmp(name + n - 3, "KHR"))) break;
        for (unsigned long i = 0; sec && i < sz / sizeof *sec && !fn; i++) if (!strncmp(sec[i].name, name, len) && !sec[i].name[len]) fn = sec[i].fn;
    }
    const char *how = fn ? "host" : NULL;
    if (!fn && gv_up()) { fn = (void *)vk.vkGetDeviceProcAddr(V.dev, name); if (!fn) fn = (void *)vk.vkGetInstanceProcAddr(V.inst, name); how = fn ? "the driver's own" : "none (null)"; }
    static const char *seen[512]; static int nseen; static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu);
    int known = 0; for (int i = 0; i < nseen; i++) if (!strcmp(seen[i], name)) { known = 1; break; }
    if (!known && nseen < 512) { seen[nseen++] = strdup(name); if (!fn || strcmp(how, "host")) GLOG("lookup %s: %s", name, how ? how : "none (null)"); else OLOG("lookup %s: host", name); }
    pthread_mutex_unlock(&mu);
    return fn;
}
GV(vkGetInstanceProcAddr, PFN_vkVoidFunction, VkInstance i, const char *name) { (void)i; return (PFN_vkVoidFunction)gv_lookup(name, 0); }
GV(vkGetDeviceProcAddr, PFN_vkVoidFunction, VkDevice d, const char *name) { (void)d; return (PFN_vkVoidFunction)gv_lookup(name, 1); }

// The host has a guest Vulkan layer (main.c lets a game that imports Vulkan run)
int dm_guest_vk_available(void) { return 1; }
