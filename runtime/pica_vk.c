/* Renderer da PICA200 em Vulkan 1.3 (alternativa ao OpenGL: lançador "Vulkan" / RENDER=vulkan).
 *
 * Faz o mesmo trabalho de pica_gl.c, com a mesma divisão: a CPU roda o shader de vértice da PICA, monta e
 * recorta os triângulos; a placa rasteriza e roda um único shader de fragmentos que lê os registradores
 * (texturas, iluminação, combinadores, neblina, teste alfa). Os shaders são os mesmos do OpenGL, portados
 * para GLSL do Vulkan em vk/ e embutidos já compilados (vk_shaders.h, gerado por vk/gerar.sh).
 *
 * Tudo acontece fora da tela (sem janela Vulkan): as imagens prontas voltam para a CPU e a janela mostra
 * como no OpenGL (pica_gl.c guarda as imagens em escala e as entrega ao gsp.c).
 *
 * Organização:
 *  - lotes: os comandos vão para um buffer de comandos ("lote"); no fim de cada lista da GPU o lote é
 *    enviado sem esperar. Só se espera a placa quando a CPU precisa do resultado (leitura da memória).
 *  - superfícies (framebuffers do jogo) ficam na placa em imagens sc vezes maiores, sempre no layout
 *    GENERAL, com barreiras completas entre desenhar e copiar/ler. Carga e leitura passam por buffers
 *    da CPU, com a ampliação/redução feita na CPU (vizinho mais próximo, como o blit do OpenGL).
 *  - estado: o que muda muito (profundidade, estêncil, recorte, cor constante) é dinâmico; o resto
 *    (mistura, operação lógica, máscara de cor) escolhe um pipeline num cache.
 */
#include "pica.h"
#include "idioma_ui.h"
#include "pica_int.h"
#include "plat.h"
#define VK_NO_PROTOTYPES
#include "vulkan/vulkan/vulkan_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#include "vk_shaders.h"
#ifdef VK_TEST_SHADER
#include "vk_test.h"
#endif

#define R(x) g_regs[x]
#define VSTRIDE 22
extern const char *g_render_name;
extern int g_sc;
extern volatile int g_res_req;
extern volatile int g_tex_filter, g_tex_filter_req;
void pica_hd_store(uint32_t out, uint32_t *px, int w, int h);   /* pica_gl.c */
void pica_pout_add(uint32_t out, int d);
void pica_gl_out_mark(uint32_t out);
extern volatile int g_pend_done;
extern long g_pend_forced;

/* ------------------------------------------------------------ funções do Vulkan (carregadas na hora) */
#define VK_GLOBAL(X) X(vkCreateInstance) X(vkEnumerateInstanceVersion)
#define VK_INST(X) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceFeatures) \
    X(vkGetPhysicalDeviceFeatures2) X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkGetPhysicalDeviceFormatProperties) X(vkCreateDevice) X(vkGetDeviceProcAddr) X(vkEnumerateDeviceExtensionProperties)
#define VK_DEV(X) X(vkGetDeviceQueue) X(vkCreateCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) \
    X(vkEndCommandBuffer) X(vkResetCommandBuffer) X(vkQueueSubmit) X(vkCreateFence) X(vkWaitForFences) X(vkResetFences) \
    X(vkGetFenceStatus) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) \
    X(vkFreeMemory) X(vkBindBufferMemory) X(vkMapMemory) X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) \
    X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateSampler) X(vkDestroySampler) \
    X(vkCreateShaderModule) X(vkCreateDescriptorSetLayout) X(vkCreatePipelineLayout) X(vkCreateGraphicsPipelines) \
    X(vkCreateDescriptorPool) X(vkResetDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) \
    X(vkCmdPipelineBarrier) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) X(vkCmdBlitImage) X(vkCmdClearColorImage) \
    X(vkCmdClearDepthStencilImage) X(vkCmdBeginRendering) X(vkCmdEndRendering) X(vkCmdBindPipeline) \
    X(vkCmdBindDescriptorSets) X(vkCmdBindVertexBuffers) X(vkCmdDraw) X(vkCmdSetViewport) X(vkCmdSetScissor) \
    X(vkCmdSetBlendConstants) X(vkCmdSetDepthTestEnable) X(vkCmdSetDepthWriteEnable) X(vkCmdSetDepthCompareOp) \
    X(vkCmdSetStencilTestEnable) X(vkCmdSetStencilOp) X(vkCmdSetStencilCompareMask) X(vkCmdSetStencilWriteMask) \
    X(vkCmdSetStencilReference) X(vkDeviceWaitIdle) X(vkCreatePipelineCache) X(vkGetPipelineCacheData) \
    X(vkCreateQueryPool) X(vkCmdResetQueryPool) X(vkCmdWriteTimestamp) X(vkGetQueryPoolResults)
#define VKDECL(n) static PFN_##n n;
VK_GLOBAL(VKDECL) VK_INST(VKDECL) VK_DEV(VKDECL)
#undef VKDECL
static PFN_vkGetInstanceProcAddr p_gipa;

static VkInstance g_inst; static VkPhysicalDevice g_pdev; static VkDevice g_dev; static VkQueue g_queue;
static uint32_t g_qfam; static VkPhysicalDeviceMemoryProperties g_memprops;
static int g_has_logic, g_has_border, g_has_aniso, g_has_clamp, g_blit_c, g_blit_d, g_blit_lin; static float g_max_aniso;
extern volatile int g_hd_need_top, g_hd_need_bot;   /* pica_gl.c */
static VkFormat g_dfmt;                        /* formato da profundidade: D24S8 ou, sem ele (AMD), D32F_S8 */
static int g_vk_state;                         /* 0 = não tentou, 1 = pronto, -1 = falhou */
static int g_in_list;
static unsigned g_list_serial = 1;

static void *vk_lib_open(void) {
#ifdef _WIN32
    HMODULE h = LoadLibraryA("vulkan-1.dll");
    return h ? (void *)GetProcAddress(h, "vkGetInstanceProcAddr") : NULL;
#else
    void *h = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!h) h = dlopen("libvulkan.so", RTLD_NOW);
    return h ? dlsym(h, "vkGetInstanceProcAddr") : NULL;
#endif
}

/* ------------------------------------------------------------ memória */
typedef struct { VkBuffer b; VkDeviceMemory m; uint8_t *p; VkDeviceSize size; } Buf;
static int mem_type(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags prefer) {
    for (int pass = 0; pass < 2; pass++)
        for (uint32_t i = 0; i < g_memprops.memoryTypeCount; i++) {
            VkMemoryPropertyFlags f = g_memprops.memoryTypes[i].propertyFlags;
            if (!(bits & (1u << i)) || (f & want) != want) continue;
            if (pass == 0 && (f & prefer) != prefer) continue;
            return (int)i;
        }
    return -1;
}
static int buf_make(Buf *b, VkDeviceSize size, VkBufferUsageFlags use, int readback) {
    memset(b, 0, sizeof *b);
    VkBufferCreateInfo ci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO }; ci.size = size; ci.usage = use;
    if (vkCreateBuffer(g_dev, &ci, NULL, &b->b)) return 0;
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_dev, b->b, &mr);
    int t = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     readback ? VK_MEMORY_PROPERTY_HOST_CACHED_BIT : 0);
    if (t < 0) return 0;
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO }; ai.allocationSize = mr.size; ai.memoryTypeIndex = (uint32_t)t;
    if (vkAllocateMemory(g_dev, &ai, NULL, &b->m)) { vkDestroyBuffer(g_dev, b->b, NULL); return 0; }
    vkBindBufferMemory(g_dev, b->b, b->m, 0);
    vkMapMemory(g_dev, b->m, 0, VK_WHOLE_SIZE, 0, (void **)&b->p);
    b->size = size;
    return 1;
}
static void buf_free(Buf *b) { if (b->b) vkDestroyBuffer(g_dev, b->b, NULL); if (b->m) vkFreeMemory(g_dev, b->m, NULL); memset(b, 0, sizeof *b); }
/* buffers de leitura reaproveitados (uma leitura de tela por quadro) */
static Buf g_rbfree[8]; static int g_nrbfree;
static int rb_get(Buf *b, VkDeviceSize size) {
    for (int i = 0; i < g_nrbfree; i++) if (g_rbfree[i].size >= size && g_rbfree[i].size <= size * 2) { *b = g_rbfree[i]; g_rbfree[i] = g_rbfree[--g_nrbfree]; return 1; }
    return buf_make(b, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1);
}
static void rb_put(Buf *b) {
    if (g_nrbfree < 8) g_rbfree[g_nrbfree++] = *b; else buf_free(b);
    memset(b, 0, sizeof *b);
}

typedef struct { VkImage img; VkDeviceMemory mem; VkImageView view, dview, sview; } Img;
static int img_make(Img *im, int w, int h, int levels, VkFormat fmt, VkImageUsageFlags use, int depth) {
    memset(im, 0, sizeof *im);
    VkImageCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = fmt; ci.extent.width = (uint32_t)w; ci.extent.height = (uint32_t)h; ci.extent.depth = 1;
    ci.mipLevels = (uint32_t)levels; ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = use; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(g_dev, &ci, NULL, &im->img)) return 0;
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(g_dev, im->img, &mr);
    int t = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    if (t < 0) t = mem_type(mr.memoryTypeBits, 0, 0);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO }; ai.allocationSize = mr.size; ai.memoryTypeIndex = (uint32_t)t;
    if (t < 0 || vkAllocateMemory(g_dev, &ai, NULL, &im->mem)) { vkDestroyImage(g_dev, im->img, NULL); im->img = 0; return 0; }
    vkBindImageMemory(g_dev, im->img, im->mem, 0);
    if (!(use & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))) return 1;   /* (só cópias: sem vistas) */
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = im->img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = fmt;
    vi.subresourceRange.levelCount = (uint32_t)levels; vi.subresourceRange.layerCount = 1;
    vi.subresourceRange.aspectMask = depth ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    vkCreateImageView(g_dev, &vi, NULL, &im->view);
    if (depth) {
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT; vkCreateImageView(g_dev, &vi, NULL, &im->dview);
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT; vkCreateImageView(g_dev, &vi, NULL, &im->sview);
    }
    return 1;
}

/* ------------------------------------------------------------ lotes de comandos */
#define NBATCH 6                    /* lotes na fila da placa: com 3 a CPU esperava a placa liberar um (~20% do tempo) */
#define VTX_RING (16u << 20)        /* (cada lote é uma fração de um quadro; sem espaço, o lote é enviado e outro começa) */
#define UBO_RING (4u << 20)
#define UP_RING (16u << 20)
#define NSETS 4096
typedef struct { int kind; VkImage img; VkImageView v[3]; VkDeviceMemory mem; VkBuffer buf; VkSampler smp; } Dead;
typedef struct {
    VkCommandBuffer cb; VkFence fence; VkDescriptorPool pool;
    Buf vtx, ubo, up; VkDeviceSize vtx_off, ubo_off, up_off; int nsets;
    unsigned long serial; int pending, recording;
    Dead *dead; int ndead, capdead;           /* liberar quando este lote terminar */
    int nq; uint8_t qcat[512];                /* marcas de tempo da placa neste lote e a etapa que cada uma fecha */
} Batch;
static Batch g_b[NBATCH]; static int g_cur;
/* perfil (PICA_PERFIL, a cada ~5 s no registro): tempo da placa medido por ela mesma (marcas de tempo no começo e
 * no fim de cada lote), tempo que a CPU ficou esperando a placa e o que foi pedido a ela */
static VkQueryPool g_qpool; static double g_ts_ns;      /* ns por tique do relógio da placa (0 = sem medição) */
static uint64_t g_pf_gpu_ns, g_pf_wait_ns; static long g_pf_draw, g_pf_vtx, g_pf_rp, g_pf_conv, g_pf_clear, g_pf_bar, g_pf_lotes;
static double g_pf_conv_px, g_pf_clear_px;
/* etapas medidas pela placa: o intervalo entre duas marcas vai para a etapa da segunda */
enum { Et_OUTRO, Et_DESENHO, Et_LIMPEZA, Et_CONV, Et_COPIA, Et_N };
static uint64_t g_pf_et[Et_N]; static int g_et_on;
#define QPB 512
static void ts_mark(int et);
static uint64_t g_pf_wait_anel, g_pf_wait_tudo; static long g_pf_n_tudo, g_pf_esp;   /* espera: lote reaproveitado / tudo o que foi enviado */
static unsigned long g_serial_next = 1, g_serial_done;
static VkCommandPool g_cpool;
static pthread_mutex_t g_vk_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_lock_owner; static int g_lock_depth;

static void batch_reclaim(Batch *b) {
    for (int i = 0; i < b->ndead; i++) {
        Dead *d = &b->dead[i];
        for (int k = 0; k < 3; k++) if (d->v[k]) vkDestroyImageView(g_dev, d->v[k], NULL);
        if (d->img) vkDestroyImage(g_dev, d->img, NULL);
        if (d->buf) vkDestroyBuffer(g_dev, d->buf, NULL);
        if (d->mem) vkFreeMemory(g_dev, d->mem, NULL);
        if (d->smp) vkDestroySampler(g_dev, d->smp, NULL);
    }
    b->ndead = 0;
}
static void batch_wait(Batch *b) {
    if (!b->pending) return;
    uint64_t tw0 = prof_now();
    { static int lg = -1; if (lg < 0) lg = ENVC("VK_LOG") != NULL;
      if (lg) { uint64_t t0 = prof_now(); vkWaitForFences(g_dev, 1, &b->fence, VK_TRUE, UINT64_MAX);
                fprintf(stderr, "[vk] lote %lu: espera %.1f ms\n", b->serial, (prof_now() - t0) / 1e6); } }
    for (int k = 1;; k++) {                     /* (espera em pedaços: se a placa não responder, o registro mostra) */
        VkResult r = vkWaitForFences(g_dev, 1, &b->fence, VK_TRUE, 3000000000ull);
        if (r == VK_SUCCESS) break;
        static int n; if (n++ < 20) { fprintf(stderr, "[vk] a placa não terminou o lote %lu em %d s (resultado %d)\n", b->serial, k * 3, (int)r); fflush(stderr); }
        if (r != VK_TIMEOUT) break;
    }
    b->pending = 0;
    g_pf_wait_ns += prof_now() - tw0;
    if (g_qpool && b->nq >= 2) { static uint64_t ts[QPB]; uint32_t q = (uint32_t)(b - g_b) * QPB;
                   if (vkGetQueryPoolResults(g_dev, g_qpool, q, (uint32_t)b->nq, sizeof(uint64_t) * (size_t)b->nq, ts, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && ts[b->nq - 1] >= ts[0]) {
                       g_pf_gpu_ns += (uint64_t)((double)(ts[b->nq - 1] - ts[0]) * g_ts_ns);
                       for (int i = 1; i < b->nq; i++) if (ts[i] >= ts[i - 1]) g_pf_et[b->qcat[i]] += (uint64_t)((double)(ts[i] - ts[i - 1]) * g_ts_ns);
                   } }
    if (b->serial > g_serial_done) g_serial_done = b->serial;
    batch_reclaim(b);
}
static int batch_poll(Batch *b) {               /* 1 se terminou */
    if (!b->pending) return 1;
    if (vkGetFenceStatus(g_dev, b->fence) != VK_SUCCESS) return 0;
    batch_wait(b);
    return 1;
}
static Batch *cur(void);
static void dead_add(Dead d) {                 /* objeto em uso pelo lote atual (e talvez pelos enviados): libera depois */
    Batch *b = cur();
    if (b->ndead == b->capdead) { b->capdead = b->capdead ? b->capdead * 2 : 64; b->dead = realloc(b->dead, sizeof(Dead) * b->capdead); }
    b->dead[b->ndead++] = d;
}
static Batch *cur(void) {
    Batch *b = &g_b[g_cur];
    if (b->recording) return b;
    { uint64_t t0 = prof_now(); batch_wait(b); g_pf_wait_anel += prof_now() - t0; }
    vkResetCommandBuffer(b->cb, 0);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(b->cb, &bi);
    vkResetDescriptorPool(g_dev, b->pool, 0);
    b->nq = 0;
    if (g_qpool) { uint32_t q = (uint32_t)(b - g_b) * QPB; vkCmdResetQueryPool(b->cb, g_qpool, q, g_et_on ? QPB : 2); vkCmdWriteTimestamp(b->cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_qpool, q); b->qcat[0] = 0; b->nq = 1; }
    b->vtx_off = b->ubo_off = b->up_off = 0; b->nsets = 0;
    b->serial = g_serial_next++;
    b->recording = 1;
    return b;
}
static void ts_mark(int et) {                  /* (deixa 1 vaga para a marca do fim do lote) */
    Batch *b = &g_b[g_cur];
    if (!g_et_on || !g_qpool || !b->recording || b->nq >= QPB - 1) return;
    vkCmdWriteTimestamp(b->cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_qpool, (uint32_t)(b - g_b) * QPB + (uint32_t)b->nq);
    b->qcat[b->nq++] = (uint8_t)et;
}
static void rp_end(void);
/* envia o lote atual (sem esperar); devolve o número de série dele */
static unsigned long batch_submit(void) {
    Batch *b = &g_b[g_cur];
    if (!b->recording) return g_serial_next - 1;
    rp_end();
    if (g_qpool && b->nq < QPB) { vkCmdWriteTimestamp(b->cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_qpool, (uint32_t)(b - g_b) * QPB + (uint32_t)b->nq); b->qcat[b->nq++] = Et_OUTRO; }
    vkEndCommandBuffer(b->cb);
    g_pf_lotes++;
    vkResetFences(g_dev, 1, &b->fence);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO }; si.commandBufferCount = 1; si.pCommandBuffers = &b->cb;
    VkResult r = vkQueueSubmit(g_queue, 1, &si, b->fence);
    if (r) { static int n; if (n++ < 5) fprintf(stderr, "[vk] erro %d ao enviar comandos\n", r); }
    b->recording = 0; b->pending = 1;
    unsigned long s = b->serial;
    g_cur = (g_cur + 1) % NBATCH;
    return s;
}
static void serial_wait(unsigned long s) {
    uint64_t t0 = prof_now(); int w = 0;
    for (int i = 0; i < NBATCH; i++) if (g_b[i].pending && g_b[i].serial <= s) { batch_wait(&g_b[i]); w = 1; }
    if (w && s + 1 >= g_serial_next) { g_pf_wait_tudo += prof_now() - t0; g_pf_n_tudo++; }   /* esperou a placa esvaziar */
}
static int serial_poll(unsigned long s) {
    if (g_serial_done >= s) return 1;
    for (int i = 0; i < NBATCH; i++) if (g_b[i].pending && g_b[i].serial <= s && !batch_poll(&g_b[i])) return 0;
    return g_serial_done >= s;
}
static void submit_wait(void) { unsigned long s = batch_submit(); serial_wait(s); }

/* espaço nos anéis do lote; sem espaço, envia o lote e começa outro */
static void *ring_alloc(int which, VkDeviceSize n, VkDeviceSize align, VkBuffer *buf, VkDeviceSize *off) {
    Batch *b = cur();
    Buf *r = which == 0 ? &b->vtx : which == 1 ? &b->ubo : &b->up;
    VkDeviceSize *o = which == 0 ? &b->vtx_off : which == 1 ? &b->ubo_off : &b->up_off;
    VkDeviceSize at = (*o + align - 1) & ~(align - 1);
    if (at + n > r->size) {
        if (n > r->size) {                     /* maior que o anel: buffer avulso, liberado com o lote */
            Buf t; if (!buf_make(&t, n, which == 2 ? VK_BUFFER_USAGE_TRANSFER_SRC_BIT : VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, 0)) return NULL;
            Dead d = { 0 }; d.buf = t.b; d.mem = t.m; dead_add(d);
            *buf = t.b; *off = 0; return t.p;
        }
        batch_submit(); b = cur();
        r = which == 0 ? &b->vtx : which == 1 ? &b->ubo : &b->up;
        o = which == 0 ? &b->vtx_off : which == 1 ? &b->ubo_off : &b->up_off;
        at = 0;
    }
    *o = at + n; *buf = r->b; *off = at;
    return r->p + at;
}

/* barreira completa: tudo o que veio antes termina (e fica visível) antes do que vem depois */
static void barrier(void) {
    g_pf_bar++;
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cur()->cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}
static void to_general(VkImage img, int levels, int depth) {
    VkImageMemoryBarrier ib = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.image = img; ib.subresourceRange.levelCount = (uint32_t)levels; ib.subresourceRange.layerCount = 1;
    ib.subresourceRange.aspectMask = depth ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    ib.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cur()->cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
}

/* ------------------------------------------------------------ superfícies (framebuffers do jogo) */
typedef struct {
    uint32_t addr, size; int w, h, fmt, depth, sc; Img im; int alive, valid, dirty; unsigned long used; unsigned ver, conv_ver; Img conv;
    int clr_pend; uint32_t clr_val;          /* limpeza adiada: feita no começo da próxima passada (ou antes de qualquer outro uso) */
} Surf;
#define NSURF 48
static Surf g_surf[NSURF]; static unsigned long g_surf_clock;
static Surf *g_cur_c, *g_cur_d;
static int g_rp; static Surf *g_rp_c, *g_rp_d;   /* passada de desenho aberta e seus alvos */
static uint8_t *g_pix; static size_t g_pix_cap;
static int cbpp(int fmt) { return fmt == 0 ? 4 : fmt == 1 ? 3 : 2; }
static int dbpp(int fmt) { return fmt == 0 ? 2 : fmt == 2 ? 3 : 4; }
static int overlap(uint32_t a, uint32_t al, uint32_t b, uint32_t bl) { return a < b + bl && b < a + al; }
static void tex_invalidate_range(uint32_t a, uint32_t len);
static void pend_resolve_range(uint32_t a, uint32_t len);
static void pix_reserve(size_t n) { if (n > g_pix_cap) { g_pix_cap = n; g_pix = realloc(g_pix, n); } }

static uint8_t k_mx[64], k_my[64];
static void morton_init(void) {
    for (int i = 0; i < 64; i++) {
        int x = 0, y = 0;
        for (int b = 0; b < 3; b++) { x |= ((i >> (2 * b)) & 1) << b; y |= ((i >> (2 * b + 1)) & 1) << b; }
        k_mx[i] = (uint8_t)x; k_my[i] = (uint8_t)y;
    }
}
static Fb surf_fb(const Surf *s) {
    Fb f; memset(&f, 0, sizeof f); f.w = s->w; f.h = s->h;
    if (s->depth) { f.depth = s->addr; f.dfmt = s->fmt; } else { f.color = s->addr; f.cfmt = s->fmt; }
    return f;
}
/* memória <-> buffer linear (linha 0 = y 0 da placa), igual ao pica_gl.c: cor RGBA em bytes, profundidade
 * (d24 << 8) | estêncil */
static void mem_to_linear(const Surf *s, uint32_t *out) {
    int w = s->w, h = s->h;
    if ((!s->depth && s->fmt == 0) || (s->depth && s->fmt == 3)) {
        const uint8_t *m = g_mem + s->addr;
        for (int ty = 0; ty < h / 8; ty++) for (int tx = 0; tx < w / 8; tx++) {
            const uint32_t *blk = (const uint32_t *)(m + ((size_t)ty * (w / 8) + tx) * 256);
            for (int i = 0; i < 64; i++) {
                int x = tx * 8 + k_mx[i], y = h - 1 - (ty * 8 + k_my[i]);
                uint32_t v = blk[i];
                out[(size_t)y * w + x] = s->depth ? (v << 8) | (v >> 24) : __builtin_bswap32(v);
            }
        }
        return;
    }
    Fb f = surf_fb(s);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        uint32_t *o = &out[(size_t)y * w + x];
        if (!s->depth) fb_read(&f, x, y, (uint8_t *)o);
        else {
            uint32_t d = depth_read(&f, x, y);
            uint32_t d24 = s->fmt == 0 ? (d << 8 | d >> 8) : d;
            *o = d24 << 8 | (s->fmt == 3 ? stencil_read(&f, x, y) : 0);
        }
    }
}
static void linear_to_mem(const Surf *s, const uint32_t *in) {
    int w = s->w, h = s->h;
    if ((!s->depth && s->fmt == 0) || (s->depth && s->fmt == 3)) {
        uint8_t *m = g_mem + s->addr;
        for (int ty = 0; ty < h / 8; ty++) for (int tx = 0; tx < w / 8; tx++) {
            uint32_t *blk = (uint32_t *)(m + ((size_t)ty * (w / 8) + tx) * 256);
            for (int i = 0; i < 64; i++) {
                int x = tx * 8 + k_mx[i], y = h - 1 - (ty * 8 + k_my[i]);
                uint32_t v = in[(size_t)y * w + x];
                blk[i] = s->depth ? (v >> 8) | (v << 24) : __builtin_bswap32(v);
            }
        }
        return;
    }
    Fb f = surf_fb(s);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        uint32_t v = in[(size_t)y * w + x];
        if (!s->depth) fb_write(&f, x, y, (const uint8_t *)&v, 0xF);
        else {
            uint32_t d24 = v >> 8;
            depth_write(&f, x, y, s->fmt == 0 ? d24 >> 8 : d24);
            if (s->fmt == 3) stencil_write(&f, x, y, (uint8_t)v);
        }
    }
}

/* passada de desenho (renderização dinâmica) no alvo atual */
static void rp_end(void) {
    if (!g_rp) return;
    vkCmdEndRendering(cur()->cb);
    ts_mark(Et_DESENHO);
    barrier();
    g_rp = 0; g_rp_c = g_rp_d = NULL;
}
static void clr_valor(const Surf *s, uint32_t value, VkClearValue *cv);
static void clr_flush(Surf *s);
static long g_pf_clear_rp;
static void rp_begin(Surf *c, Surf *d) {
    if (g_rp && g_rp_c == c && g_rp_d == d) return;
    rp_end();
    /* limpezas adiadas: viram o "limpar ao começar" da passada (de graça na placa), se cobrem a área inteira */
    if (d && d->clr_pend && (d->w * d->sc != c->w * c->sc || d->h * d->sc != c->h * c->sc)) clr_flush(d);
    cur(); ts_mark(Et_OUTRO);
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO }, da = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    ca.imageView = c->im.view; ca.imageLayout = VK_IMAGE_LAYOUT_GENERAL; ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    if (c->clr_pend) { ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; clr_valor(c, c->clr_val, &ca.clearValue); c->clr_pend = 0; g_pf_clear_rp++; }
    if (d) { da.imageView = d->im.view; da.imageLayout = VK_IMAGE_LAYOUT_GENERAL; da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
             if (d->clr_pend) { da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; clr_valor(d, d->clr_val, &da.clearValue); d->clr_pend = 0; g_pf_clear_rp++; } }
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea.extent.width = (uint32_t)(c->w * c->sc); ri.renderArea.extent.height = (uint32_t)(c->h * c->sc);
    ri.layerCount = 1; ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca;
    if (d) { ri.pDepthAttachment = &da; ri.pStencilAttachment = &da; }
    vkCmdBeginRendering(cur()->cb, &ri);
    g_rp = 1; g_rp_c = c; g_rp_d = d; g_pf_rp++;
}

static void surf_free(Surf *s) {
    if (g_rp && (g_rp_c == s || g_rp_d == s)) rp_end();
    if (s->alive) {
        Dead d = { 0 }; d.img = s->im.img; d.mem = s->im.mem; d.v[0] = s->im.view; d.v[1] = s->im.dview; d.v[2] = s->im.sview; dead_add(d);
        if (s->conv.img) { Dead e = { 0 }; e.img = s->conv.img; e.mem = s->conv.mem; e.v[0] = s->conv.view; dead_add(e); }
    }
    if (g_cur_c == s) g_cur_c = NULL;
    if (g_cur_d == s) g_cur_d = NULL;
    memset(s, 0, sizeof *s);
}
/* imagem auxiliar em tamanho nativo (a escala entre ela e a superfície é feita na placa, com vkCmdBlitImage) */
static Img g_nt[2]; static int g_nt_w[2], g_nt_h[2];
static Img *native_tmp(int w, int h, int depth) {
    Img *im = &g_nt[depth];
    if (im->img && g_nt_w[depth] >= w && g_nt_h[depth] >= h) return im;
    if (im->img) { Dead d = { 0 }; d.img = im->img; d.mem = im->mem; d.v[0] = im->view; d.v[1] = im->dview; d.v[2] = im->sview; dead_add(d); memset(im, 0, sizeof *im); }
    int W = w > g_nt_w[depth] ? w : g_nt_w[depth], H = h > g_nt_h[depth] ? h : g_nt_h[depth];
    if (!img_make(im, W, H, 1, depth ? g_dfmt : VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, depth)) { memset(im, 0, sizeof *im); return NULL; }
    g_nt_w[depth] = W; g_nt_h[depth] = H;
    rp_end(); to_general(im->img, 1, depth);
    return im;
}
static int surf_blit_ok(const Surf *s) { return s->sc > 1 && (s->depth ? g_blit_d : g_blit_c); }
/* blit (vizinho mais próximo) entre a superfície (w*sc x h*sc) e a auxiliar (w x h); down = superfície -> auxiliar */
static void surf_blit(Surf *s, Img *nt, int down) {
    if (down) clr_flush(s); else s->clr_pend = 0;
    VkImageBlit b; memset(&b, 0, sizeof b);
    VkImageAspectFlags a = s->depth ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    b.srcSubresource.aspectMask = b.dstSubresource.aspectMask = a; b.srcSubresource.layerCount = b.dstSubresource.layerCount = 1;
    VkOffset3D big = { s->w * s->sc, s->h * s->sc, 1 }, small = { s->w, s->h, 1 };
    b.srcOffsets[1] = down ? big : small; b.dstOffsets[1] = down ? small : big;
    barrier();
    vkCmdBlitImage(cur()->cb, down ? s->im.img : nt->img, VK_IMAGE_LAYOUT_GENERAL, down ? nt->img : s->im.img, VK_IMAGE_LAYOUT_GENERAL, 1, &b, VK_FILTER_NEAREST);
    barrier();
}
/* imagens auxiliares da redução da tela em escala (3: duas para as metades sucessivas e a final) */
static Img g_dt[3]; static int g_dt_w[3], g_dt_h[3];
static Img *down_tmp(int i, int w, int h) {
    Img *im = &g_dt[i];
    if (im->img && g_dt_w[i] >= w && g_dt_h[i] >= h) return im;
    if (im->img) { Dead d = { 0 }; d.img = im->img; d.mem = im->mem; dead_add(d); memset(im, 0, sizeof *im); }
    int W = w > g_dt_w[i] ? w : g_dt_w[i], H = h > g_dt_h[i] ? h : g_dt_h[i];
    if (!img_make(im, W, H, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0)) { memset(im, 0, sizeof *im); g_dt_w[i] = g_dt_h[i] = 0; return NULL; }
    g_dt_w[i] = W; g_dt_h[i] = H;
    rp_end(); to_general(im->img, 1, 0);
    return im;
}
static void blit_lin(VkImage src, int sx, int sy, int sw, int sh, VkImage dst, int dw, int dh) {
    VkImageBlit b; memset(&b, 0, sizeof b);
    b.srcSubresource.aspectMask = b.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; b.srcSubresource.layerCount = b.dstSubresource.layerCount = 1;
    b.srcOffsets[0] = (VkOffset3D){ sx, sy, 0 }; b.srcOffsets[1] = (VkOffset3D){ sx + sw, sy + sh, 1 };
    b.dstOffsets[1] = (VkOffset3D){ dw, dh, 1 };
    vkCmdBlitImage(cur()->cb, src, VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_GENERAL, 1, &b, VK_FILTER_LINEAR);
    barrier();
}
/* reduz na placa a região (sx, sy, sw x sh) da superfície para W x H: metades sucessivas (cada uma com filtro
 * linear = média de 2x2) e o último passo direto; devolve a imagem com o resultado em (0,0) ou NULL */
static Img *reduzir(VkImage src, int sx, int sy, int sw, int sh, int W, int H) {
    int k = 0; Img *t = NULL;
    while (sw > 2 * W && sh > 2 * H) {
        int nw = sw / 2, nh = sh / 2;
        Img *n = down_tmp(k, nw, nh); if (!n) return NULL;
        barrier(); blit_lin(src, sx, sy, sw, sh, n->img, nw, nh);
        src = n->img; sx = sy = 0; sw = nw; sh = nh; t = n; k ^= 1;
    }
    Img *f = down_tmp(2, W, H); if (!f) return NULL;
    (void)t; barrier(); blit_lin(src, sx, sy, sw, sh, f->img, W, H);
    return f;
}
/* cópia imagem -> buffer da CPU (região inteira, na escala da superfície); espera a placa */
static void surf_read_scaled(Surf *s, Buf *rb, VkDeviceSize *doff, VkDeviceSize *soff) {
    int W = s->w * s->sc, H = s->h * s->sc;
    clr_flush(s);
    rp_end();
    VkDeviceSize dsz = (VkDeviceSize)W * H * 4, ssz = s->depth ? (VkDeviceSize)W * H : 0;
    rb_get(rb, dsz + ssz + 16);
    VkBufferImageCopy r[2]; memset(r, 0, sizeof r);
    r[0].imageSubresource.aspectMask = s->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    r[0].imageSubresource.layerCount = 1; r[0].imageExtent.width = (uint32_t)W; r[0].imageExtent.height = (uint32_t)H; r[0].imageExtent.depth = 1;
    r[1] = r[0]; r[1].imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT; r[1].bufferOffset = dsz;
    vkCmdCopyImageToBuffer(cur()->cb, s->im.img, VK_IMAGE_LAYOUT_GENERAL, rb->b, s->depth ? 2 : 1, r);
    barrier();
    submit_wait();
    *doff = 0; *soff = dsz;
}
static inline uint32_t dval_from(const uint8_t *dp, size_t i) {      /* texel da profundidade copiado -> d24 */
    if (g_dfmt == VK_FORMAT_D24_UNORM_S8_UINT) { uint32_t v; memcpy(&v, dp + i * 4, 4); return v & 0xFFFFFF; }
    float f; memcpy(&f, dp + i * 4, 4); if (f < 0) f = 0; if (f > 1) f = 1;
    return (uint32_t)lrintf(f * 16777215.0f);
}
/* placa -> memória do jogo */
static long g_st_wb[2], g_st_up[2]; static uint64_t g_st_wb_ns;   /* estatística: devoluções/envios (cor, prof.) */
static void surf_writeback(Surf *s) {
    if (!s->alive || !s->valid || !s->dirty) return;
    uint64_t t0 = prof_now();
    g_st_wb[s->depth ? 1 : 0]++;
    Buf rb; VkDeviceSize doff, soff;
    Img *nt = surf_blit_ok(s) ? native_tmp(s->w, s->h, s->depth) : NULL;
    { static int lg = -1; if (lg < 0) lg = ENVC("VK_LOG2") != NULL; if (lg) { fprintf(stderr, "[vk2] devolve lista %u: %08x %dx%d fmt %d prof %d blit %d\n", g_list_serial, s->addr, s->w, s->h, s->fmt, s->depth, nt != NULL); fflush(stderr); } }
    int W, k, c;
    if (nt) {                                    /* reduz na placa e lê só o tamanho nativo */
        rp_end();
        surf_blit(s, nt, 1);
        VkDeviceSize dsz = (VkDeviceSize)s->w * s->h * 4, ssz = s->depth ? (VkDeviceSize)s->w * s->h : 0;
        rb_get(&rb, dsz + ssz + 16);
        VkBufferImageCopy r[2]; memset(r, 0, sizeof r);
        r[0].imageSubresource.aspectMask = s->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        r[0].imageSubresource.layerCount = 1; r[0].imageExtent.width = (uint32_t)s->w; r[0].imageExtent.height = (uint32_t)s->h; r[0].imageExtent.depth = 1;
        r[1] = r[0]; r[1].imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT; r[1].bufferOffset = dsz;
        vkCmdCopyImageToBuffer(cur()->cb, nt->img, VK_IMAGE_LAYOUT_GENERAL, rb.b, s->depth ? 2 : 1, r);
        barrier();
        submit_wait();
        doff = 0; soff = dsz; W = s->w; k = 1; c = 0;
    } else { surf_read_scaled(s, &rb, &doff, &soff); W = s->w * s->sc; k = s->sc; c = s->sc / 2; }
    pix_reserve((size_t)s->w * s->h * 4);
    uint32_t *lin = (uint32_t *)g_pix;
    for (int y = 0; y < s->h; y++) for (int x = 0; x < s->w; x++) {
        size_t i = (size_t)(y * k + c) * W + x * k + c;
        if (!s->depth) memcpy(&lin[(size_t)y * s->w + x], rb.p + i * 4, 4);
        else lin[(size_t)y * s->w + x] = dval_from(rb.p + doff, i) << 8 | rb.p[soff + i];
    }
    rb_put(&rb);
    plat_touch(g_mem + s->addr, s->size);
    linear_to_mem(s, lin);
    tex_invalidate_range(s->addr, s->size);
    s->dirty = 0;
    g_prof_ns[3] += prof_now() - t0; g_st_wb_ns += prof_now() - t0;
}
/* memória -> placa (ampliada na CPU) */
static void surf_upload(Surf *s) {
    uint64_t t0 = prof_now();
    g_st_up[s->depth ? 1 : 0]++;
    pix_reserve((size_t)s->w * s->h * 4);
    plat_touch(g_mem + s->addr, s->size);
    uint32_t *lin = (uint32_t *)g_pix;
    mem_to_linear(s, lin);
    rp_end();
    Img *nt = surf_blit_ok(s) ? native_tmp(s->w, s->h, s->depth) : NULL;
    { static int lg = -1; if (lg < 0) lg = ENVC("VK_LOG2") != NULL; if (lg) { fprintf(stderr, "[vk2] carrega lista %u: %08x %dx%d fmt %d prof %d blit %d\n", g_list_serial, s->addr, s->w, s->h, s->fmt, s->depth, nt != NULL); fflush(stderr); } }
    /* envia o tamanho nativo e amplia na placa */
    int W = nt ? s->w : s->w * s->sc, H = nt ? s->h : s->h * s->sc, k = nt ? 1 : s->sc;
    VkDeviceSize dsz = (VkDeviceSize)W * H * 4, ssz = s->depth ? (VkDeviceSize)W * H : 0;
    VkBuffer sb; VkDeviceSize so;
    uint8_t *p = ring_alloc(2, dsz + ssz, 16, &sb, &so);
    if (!p) return;
    if (!s->depth) {                              /* cor: amplia uma linha e copia para as k linhas dela */
        for (int y = 0; y < s->h; y++) {
            const uint32_t *row = lin + (size_t)y * s->w;
            uint32_t *o = (uint32_t *)(p + (size_t)y * k * W * 4);
            if (k == 1) memcpy(o, row, (size_t)W * 4);
            else for (int x = 0; x < s->w; x++) { uint32_t v = row[x]; for (int j = 0; j < k; j++) o[x * k + j] = v; }
            for (int j = 1; j < k; j++) memcpy(o + (size_t)j * W, o, (size_t)W * 4);
        }
    } else for (int Y = 0; Y < H; Y++) {
        const uint32_t *row = lin + (size_t)(Y / k) * s->w;
        for (int X = 0; X < W; X++) {
            uint32_t v = row[X / k]; size_t i = (size_t)Y * W + X;
            uint32_t d24 = v >> 8;
            if (g_dfmt == VK_FORMAT_D24_UNORM_S8_UINT) memcpy(p + i * 4, &d24, 4);
            else { float f = (float)d24 / 16777215.0f; memcpy(p + i * 4, &f, 4); }
            p[dsz + i] = (uint8_t)v;
        }
    }
    VkBufferImageCopy r[2]; memset(r, 0, sizeof r);
    r[0].bufferOffset = so; r[0].imageSubresource.aspectMask = s->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    r[0].imageSubresource.layerCount = 1; r[0].imageExtent.width = (uint32_t)W; r[0].imageExtent.height = (uint32_t)H; r[0].imageExtent.depth = 1;
    r[1] = r[0]; r[1].bufferOffset = so + dsz; r[1].imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
    barrier();
    s->clr_pend = 0;                                       /* (o envio cobre a superfície inteira) */
    vkCmdCopyBufferToImage(cur()->cb, sb, nt ? nt->img : s->im.img, VK_IMAGE_LAYOUT_GENERAL, s->depth ? 2 : 1, r);
    barrier();
    if (nt) surf_blit(s, nt, 0);
    s->valid = 1; s->dirty = 0; s->ver++;
    g_prof_ns[4] += prof_now() - t0;
}
/* escala de uma superfície: a escolhida, mas sem passar dos pixels do maior framebuffer de tela (512x256) nessa
 * escala. Em 2K/4K,
 * superfícies auxiliares maiores que a tela (efeitos, texturas desenhadas pelo jogo) ficariam enormes (512x512 em 9x
 * = 4608x4608) sem ganho visível. A cor e a profundidade de um desenho têm o mesmo tamanho, então a mesma escala. */
static int surf_sc(int w, int h) {
    int sc = g_sc; if (sc <= 4 || ENVC("VK_SUPERFICIE_CHEIA")) return sc;
    double lim = 512.0 * 256.0 * sc * sc;
    while (sc > 4 && (double)w * h * sc * sc > lim * 1.01) sc--;
    return sc;
}
static Surf *surf_get(uint32_t addr, int w, int h, int fmt, int depth) {
    uint32_t size = (uint32_t)(w * h * (depth ? dbpp(fmt) : cbpp(fmt)));
    Surf *hit = NULL;
    for (int i = 0; i < NSURF; i++) {
        Surf *s = &g_surf[i];
        if (!s->alive) continue;
        if (s->addr == addr && s->w == w && s->h == h && s->fmt == fmt && s->depth == depth) { hit = s; continue; }
        if (overlap(s->addr, s->size, addr, size)) { surf_writeback(s); surf_free(s); }
    }
    if (!hit) {
        for (int i = 0; i < NSURF && !hit; i++) if (!g_surf[i].alive) hit = &g_surf[i];
        if (!hit) { hit = &g_surf[0]; for (int i = 1; i < NSURF; i++) if (g_surf[i].used < hit->used) hit = &g_surf[i]; surf_writeback(hit); surf_free(hit); }
        hit->addr = addr; hit->size = size; hit->w = w; hit->h = h; hit->fmt = fmt; hit->depth = depth; hit->sc = surf_sc(w, h);
        VkImageUsageFlags use = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
        if (!img_make(&hit->im, w * hit->sc, h * hit->sc, 1, depth ? g_dfmt : VK_FORMAT_R8G8B8A8_UNORM, use, depth)) {
            fprintf(stderr, "[vk] sem memória para a superfície %08x %dx%d\n", addr, w, h);
            memset(hit, 0, sizeof *hit); return NULL;
        }
        hit->alive = 1;
        { static int lg = -1, nn; if (lg < 0) lg = ENVC("VK_LOG2") != NULL; if (lg && nn++ < 300) fprintf(stderr, "[vk2] nova sup lista %u: %08x %dx%d fmt %d prof %d\n", g_list_serial, addr, w, h, fmt, depth); }
        rp_end(); to_general(hit->im.img, 1, depth);
        hit->valid = 0;
    }
    if (!hit->valid) surf_upload(hit);
    hit->used = ++g_surf_clock;
    return hit;
}
static void clr_valor(const Surf *s, uint32_t value, VkClearValue *cv) {
    memset(cv, 0, sizeof *cv);
    if (!s->depth) {
        VkClearColorValue *c = &cv->color;
        if (s->fmt == 0) { c->float32[0] = (value >> 24) / 255.0f; c->float32[1] = ((value >> 16) & 0xFF) / 255.0f; c->float32[2] = ((value >> 8) & 0xFF) / 255.0f; c->float32[3] = (value & 0xFF) / 255.0f; }
        else { c->float32[0] = ((value >> 16) & 0xFF) / 255.0f; c->float32[1] = ((value >> 8) & 0xFF) / 255.0f; c->float32[2] = (value & 0xFF) / 255.0f; c->float32[3] = 1; }
    } else {
        cv->depthStencil.depth = s->fmt == 0 ? (float)((value & 0xFFFF) / 65535.0) : (float)((value & 0xFFFFFF) / 16777215.0);
        cv->depthStencil.stencil = s->fmt == 3 ? value >> 24 : 0;
    }
}
/* limpeza de verdade (comando separado): quando a superfície vai ser lida/copiada antes de virar alvo de desenho */
static void clr_flush(Surf *s) {
    if (!s->clr_pend) return;
    s->clr_pend = 0;
    rp_end(); cur(); ts_mark(Et_OUTRO);
    VkImageSubresourceRange rr = { 0 }; rr.levelCount = 1; rr.layerCount = 1;
    VkClearValue cv; clr_valor(s, s->clr_val, &cv);
    if (!s->depth) { rr.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; vkCmdClearColorImage(cur()->cb, s->im.img, VK_IMAGE_LAYOUT_GENERAL, &cv.color, 1, &rr); }
    else { rr.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT; vkCmdClearDepthStencilImage(cur()->cb, s->im.img, VK_IMAGE_LAYOUT_GENERAL, &cv.depthStencil, 1, &rr); }
    barrier(); ts_mark(Et_LIMPEZA);
    g_pf_clear++; g_pf_clear_px += (double)s->w * s->sc * s->h * s->sc;
}
/* limpeza pedida pelo jogo: comando separado na hora. VK_LIMPEZA_ADIADA=1 (experimental) a adia para o "limpar ao
 * começar" da próxima passada; no renderizador de software de teste (SwiftShader) isso ficou mais lento e não deu
 * para medir numa placa de verdade, então fica desligado. */
static void surf_clear(Surf *s, uint32_t value) {
    static int direta = -1; if (direta < 0) direta = ENVC("VK_LIMPEZA_ADIADA") == NULL;
    if (g_rp && (g_rp_c == s || g_rp_d == s)) rp_end();   /* (o "limpar ao começar" só vale numa passada nova) */
    s->clr_pend = 1; s->clr_val = value;
    s->valid = 1; s->dirty = 1; s->ver++;
    if (direta) clr_flush(s);
}

/* ------------------------------------------------------------ acesso de fora (gsp.c), com a trava do Vulkan */
static int ctx_enter(void) {
    if (g_vk_state <= 0) return 0;
    if (g_lock_depth && pthread_equal(g_lock_owner, pthread_self())) { g_lock_depth++; return 1; }
    pthread_mutex_lock(&g_vk_lock); g_lock_owner = pthread_self(); g_lock_depth = 1;
    return 2;
}
static void ctx_leave(int c) {
    if (!c) return;
    if (--g_lock_depth == 0) {
        if (!g_in_list && g_b[g_cur].recording) batch_submit();   /* (o que foi gravado fora de uma lista segue já) */
        pthread_mutex_unlock(&g_vk_lock);
    }
}
static int any_overlap(uint32_t a, uint32_t len, int need_dirty) {
    for (int i = 0; i < NSURF; i++)
        if (g_surf[i].alive && overlap(g_surf[i].addr, g_surf[i].size, a, len) && (!need_dirty || (g_surf[i].valid && g_surf[i].dirty))) return 1;
    return 0;
}
void pica_vk_flush(uint32_t a, uint32_t len) {
    if (g_vk_state <= 0) return;
    pend_resolve_range(a, len);
    if (!any_overlap(a, len, 1)) return;
    int c = ctx_enter(); if (!c) return;
    for (int i = 0; i < NSURF; i++) if (g_surf[i].alive && overlap(g_surf[i].addr, g_surf[i].size, a, len)) surf_writeback(&g_surf[i]);
    ctx_leave(c);
}
void pica_vk_will_write(uint32_t a, uint32_t len) {
    if (g_vk_state <= 0) return;
    pend_resolve_range(a, len);
    if (!any_overlap(a, len, 1)) return;
    int c = ctx_enter(); if (!c) return;
    for (int i = 0; i < NSURF; i++) {
        Surf *s = &g_surf[i];
        if (s->alive && overlap(s->addr, s->size, a, len) && !(a <= s->addr && a + len >= s->addr + s->size)) surf_writeback(s);
    }
    ctx_leave(c);
}
void pica_vk_written(uint32_t a, uint32_t len) {             /* (só marca: como no OpenGL, sem a trava) */
    if (g_vk_state <= 0) return;
    for (int i = 0; i < NSURF; i++) {
        Surf *s = &g_surf[i];
        if (s->alive && overlap(s->addr, s->size, a, len)) { s->valid = 0; s->dirty = 0; }
    }
    tex_invalidate_range(a, len);
}
int pica_vk_fill(uint32_t start, uint32_t end, uint32_t value, int width) {
    if (g_vk_state <= 0) return 0;
    int c = ctx_enter(); if (!c) return 0;
    Surf *s = NULL;
    for (int i = 0; i < NSURF; i++)
        if (g_surf[i].alive && g_surf[i].addr == start && g_surf[i].addr + g_surf[i].size == end) { s = &g_surf[i]; break; }
    int ok = s && ((!s->depth && s->fmt == 0 && width >= 2) || (!s->depth && s->fmt == 1 && width == 1) ||
             (s->depth && s->fmt == 3 && width >= 2) || (s->depth && s->fmt == 2 && width == 1) || (s->depth && s->fmt == 0 && width == 0));
    if (ok) { surf_clear(s, value); tex_invalidate_range(s->addr, s->size); }
    { static int lg = -1, nn; if (lg < 0) lg = ENVC("VK_LOG2") != NULL;
      if (lg && (!ok || g_list_serial % 97 == 0) && nn++ < 400) fprintf(stderr, "[vk2] fill lista %u: %08x-%08x val %08x larg %d sup %d ok %d\n", g_list_serial, start, end, value, width, s ? 1 : 0, ok); }
    ctx_leave(c);
    return ok;
}

/* ------------------------------------------------------------ texturas (decodificadas na CPU) */
typedef struct { uint32_t addr, fmt, size; int w, h, levels; uint64_t hash; Img im; int iw, ih, ilv, gen; unsigned checked; unsigned long used; } GTex;   /* iw/ih/ilv: a imagem (maior com textura personalizada) */
#define NGT 768
static GTex g_gt[NGT]; static int g_ngt; static unsigned long g_use_clock;
static const int k_tex_bits[14] = { 32, 24, 16, 16, 16, 16, 16, 8, 8, 8, 4, 4, 4, 8 };
static uint64_t hash_mem(uint32_t addr, uint32_t len) {
    uint64_t h = 0xcbf29ce484222325ull; const uint8_t *p = g_mem + addr; uint32_t i = 0;
    for (; i + 8 <= len; i += 8) { uint64_t w; memcpy(&w, p + i, 8); h = (h ^ w) * 0x100000001b3ull; h ^= h >> 29; }
    for (; i < len; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}
static void tex_invalidate_range(uint32_t a, uint32_t len) {
    for (int i = 0; i < g_ngt; i++) if (overlap(g_gt[i].addr, g_gt[i].size, a, len)) g_gt[i].checked = 0;
}
/* texturas personalizadas (texpack.c) */
int texpack_ativo(void); uint64_t texpack_hash(const uint8_t *src, int w, int h, int fmt);
int texpack_get(uint64_t hash, const uint8_t **px, int *pw, int *ph); extern volatile int g_texpack_gen;
static VkImageView tex_get(uint32_t addr, uint32_t fmt, int w, int h, int levels) {
    uint32_t size = 0;
    for (int l = 0; l < levels; l++) {
        int lw = w >> l, lh = h >> l;
        if (lw < 8 || lh < 8) { levels = l; break; }
        size += (uint32_t)((size_t)lw * lh * k_tex_bits[fmt] / 8);
    }
    if (levels < 1) { levels = 1; size = (uint32_t)((size_t)w * h * k_tex_bits[fmt] / 8); }
    GTex *e = NULL;
    for (int i = 0; i < g_ngt; i++)
        if (g_gt[i].addr == addr && g_gt[i].fmt == fmt && g_gt[i].w == w && g_gt[i].h == h && g_gt[i].levels == levels) { e = &g_gt[i]; break; }
    if (e && e->checked == g_list_serial) { e->used = ++g_use_clock; return e->im.view; }
    pend_resolve_range(addr, size);
    uint64_t t0 = prof_now();
    uint64_t hs = hash_mem(addr, size);
    if (e && e->hash == hs && e->gen == g_texpack_gen) { e->checked = g_list_serial; e->used = ++g_use_clock; g_prof_ns[5] += prof_now() - t0; return e->im.view; }
    const uint8_t *cpx = NULL; int cw = 0, ch = 0, cst = 0;
    if (texpack_ativo()) cst = texpack_get(texpack_hash(g_mem + addr, w, h, (int)fmt), &cpx, &cw, &ch);
    if (!e) {
        if (g_ngt < NGT) e = &g_gt[g_ngt++];
        else {
            e = &g_gt[0]; for (int i = 1; i < NGT; i++) if (g_gt[i].used < e->used) e = &g_gt[i];
            Dead d = { 0 }; d.img = e->im.img; d.mem = e->im.mem; d.v[0] = e->im.view; dead_add(d); memset(&e->im, 0, sizeof e->im);
        }
        e->addr = addr; e->fmt = fmt; e->w = w; e->h = h; e->size = size; e->levels = levels; e->iw = e->ih = e->ilv = 0;
    }
    int dw = w, dh = h, dlv = levels;                  /* tamanho da imagem: o do jogo ou o da substituta do pacote */
    if (cst == 1) { dw = cw; dh = ch; dlv = 1; if (levels > 1) { int m = cw > ch ? cw : ch; while (m > 1 && dlv < 15) { m >>= 1; dlv++; } } }
    if (!e->im.img || e->iw != dw || e->ih != dh || e->ilv != dlv) {
        if (e->im.img) { Dead d = { 0 }; d.img = e->im.img; d.mem = e->im.mem; d.v[0] = e->im.view; dead_add(d); memset(&e->im, 0, sizeof e->im); }
        if (!img_make(&e->im, dw, dh, dlv, VK_FORMAT_R8G8B8A8_UNORM,
                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | (dlv > 1 && cst == 1 ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0), 0)) {
            memset(e, 0, sizeof *e); return VK_NULL_HANDLE;
        }
        e->iw = dw; e->ih = dh; e->ilv = dlv;
        rp_end(); to_general(e->im.img, dlv, 0);
    }
    e->gen = g_texpack_gen;
    if (cst == 1) {                                    /* substituta do pacote (coordenadas normalizadas: o desenho é o mesmo) */
        size_t n = (size_t)cw * ch * 4;
        rp_end();
        VkBuffer sb; VkDeviceSize so;
        uint8_t *p = ring_alloc(2, n, 16, &sb, &so);
        if (!p) return e->im.view;
        memcpy(p, cpx, n);
        VkBufferImageCopy r; memset(&r, 0, sizeof r);
        r.bufferOffset = so; r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; r.imageSubresource.layerCount = 1;
        r.imageExtent.width = (uint32_t)cw; r.imageExtent.height = (uint32_t)ch; r.imageExtent.depth = 1;
        barrier();
        vkCmdCopyBufferToImage(cur()->cb, sb, e->im.img, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
        barrier();
        for (int l = 1; l < dlv; l++) {                /* níveis menores (a textura original tinha mipmaps) */
            VkImageBlit b; memset(&b, 0, sizeof b);
            b.srcSubresource.aspectMask = b.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            b.srcSubresource.layerCount = b.dstSubresource.layerCount = 1;
            b.srcSubresource.mipLevel = (uint32_t)l - 1; b.dstSubresource.mipLevel = (uint32_t)l;
            int sw = cw >> (l - 1), sh = ch >> (l - 1), tw = cw >> l, th = ch >> l;
            b.srcOffsets[1].x = sw > 1 ? sw : 1; b.srcOffsets[1].y = sh > 1 ? sh : 1; b.srcOffsets[1].z = 1;
            b.dstOffsets[1].x = tw > 1 ? tw : 1; b.dstOffsets[1].y = th > 1 ? th : 1; b.dstOffsets[1].z = 1;
            vkCmdBlitImage(cur()->cb, e->im.img, VK_IMAGE_LAYOUT_GENERAL, e->im.img, VK_IMAGE_LAYOUT_GENERAL, 1, &b, VK_FILTER_LINEAR);
            barrier();
        }
        e->hash = hs; e->checked = g_list_serial; e->used = ++g_use_clock;
        g_prof_ns[5] += prof_now() - t0;
        return e->im.view;
    }
    size_t total = 0; for (int l = 0; l < levels; l++) total += (size_t)(w >> l) * (h >> l) * 4;
    rp_end();
    VkBuffer sb; VkDeviceSize so;
    uint8_t *p = ring_alloc(2, total, 16, &sb, &so);
    if (!p) return e->im.view;
    VkBufferImageCopy r[16]; memset(r, 0, sizeof r);
    uint32_t la = addr; size_t po = 0;
    for (int l = 0; l < levels && l < 16; l++) {
        int lw = w >> l, lh = h >> l;
        for (int y = 0; y < lh; y++) for (int x = 0; x < lw; x++) texel(la, fmt, lw, x, y, p + po + ((size_t)y * lw + x) * 4);
        r[l].bufferOffset = so + po; r[l].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; r[l].imageSubresource.mipLevel = (uint32_t)l;
        r[l].imageSubresource.layerCount = 1; r[l].imageExtent.width = (uint32_t)lw; r[l].imageExtent.height = (uint32_t)lh; r[l].imageExtent.depth = 1;
        po += (size_t)lw * lh * 4;
        la += (uint32_t)((size_t)lw * lh * k_tex_bits[fmt] / 8);
    }
    barrier();
    vkCmdCopyBufferToImage(cur()->cb, sb, e->im.img, VK_IMAGE_LAYOUT_GENERAL, (uint32_t)levels, r);
    barrier();
    e->hash = cst < 0 ? hs ^ 1 : hs;                   /* (substituta ainda sendo lida: confere de novo na próxima lista) */
    e->checked = g_list_serial; e->used = ++g_use_clock;
    g_prof_ns[5] += prof_now() - t0;
    return e->im.view;
}

/* ------------------------------------------------------------ amostradores (cache) */
typedef struct { uint64_t key; uint32_t border; VkSampler s; } SmpE;
#define NSMP 1024
static SmpE g_smp[NSMP]; static int g_nsmp;
static VkSampler sampler_get(int minl, int magl, int mipl, int mips, int wrap_s, int wrap_t, float bias, float minlod, float maxlod, int aniso, uint32_t border) {
    static const VkSamplerAddressMode k_wrap[8] = { VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
        VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE };
    int bord = (wrap_s & 7) == 1 || (wrap_t & 7) == 1;
    if (!bord) border = 0;
    uint64_t key = (uint64_t)minl | (uint64_t)magl << 1 | (uint64_t)mipl << 2 | (uint64_t)mips << 3 | (uint64_t)(wrap_s & 7) << 4 |
                   (uint64_t)(wrap_t & 7) << 7 | (uint64_t)aniso << 10 | (uint64_t)((int)(bias * 256) & 0xFFFF) << 16 |
                   (uint64_t)((int)minlod & 15) << 32 | (uint64_t)((int)maxlod & 15) << 36;
    for (int i = 0; i < g_nsmp; i++) if (g_smp[i].key == key && g_smp[i].border == border) return g_smp[i].s;
    if (g_nsmp == NSMP) {                      /* cheio: descarta todos (depois que os lotes terminarem) */
        for (int i = 0; i < g_nsmp; i++) { Dead d = { 0 }; d.smp = g_smp[i].s; dead_add(d); }
        g_nsmp = 0;
    }
    VkSamplerCreateInfo ci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    ci.magFilter = magl ? VK_FILTER_LINEAR : VK_FILTER_NEAREST; ci.minFilter = minl ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.mipmapMode = mipl ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = k_wrap[wrap_s & 7]; ci.addressModeV = k_wrap[wrap_t & 7]; ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.mipLodBias = bias; ci.minLod = mips ? minlod : 0; ci.maxLod = mips ? maxlod : 0.25f;
    ci.anisotropyEnable = aniso && g_has_aniso; ci.maxAnisotropy = aniso && g_has_aniso ? g_max_aniso : 1;
    VkSamplerCustomBorderColorCreateInfoEXT cb = { VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT };
    if (bord && g_has_border) {
        cb.customBorderColor.float32[0] = (border & 0xFF) / 255.0f; cb.customBorderColor.float32[1] = ((border >> 8) & 0xFF) / 255.0f;
        cb.customBorderColor.float32[2] = ((border >> 16) & 0xFF) / 255.0f; cb.customBorderColor.float32[3] = (border >> 24) / 255.0f;
        cb.format = VK_FORMAT_UNDEFINED;
        ci.pNext = &cb; ci.borderColor = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
    } else ci.borderColor = border >> 24 ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    VkSampler s = VK_NULL_HANDLE;
    if (vkCreateSampler(g_dev, &ci, NULL, &s)) return VK_NULL_HANDLE;
    g_smp[g_nsmp].key = key; g_smp[g_nsmp].border = border; g_smp[g_nsmp].s = s; g_nsmp++;
    return s;
}

/* ------------------------------------------------------------ pipelines */
static VkDescriptorSetLayout g_dsl, g_cdsl; static VkPipelineLayout g_pl, g_cpl;
static VkShaderModule g_sm_vs, g_sm_fs, g_sm_fvs, g_sm_cfs;
static VkPipeline g_conv_pipe; static VkPipelineCache g_pcache;
typedef struct { uint64_t key; VkPipeline p; } PipeE;
#define NPIPE 2048
static PipeE g_pipe[NPIPE]; static int g_npipe;
static VkShaderModule shader(const uint32_t *code, size_t bytes) {
    VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO }; ci.codeSize = bytes; ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE; vkCreateShaderModule(g_dev, &ci, NULL, &m); return m;
}
/* chave: bit 0 profundidade presente, 1 mistura, 2-4 eq. cor, 5-7 eq. alfa, 8-23 fatores, 24 op. lógica, 25-28 op,
 * 29-32 máscara de cor */
/* cria o pipeline; com `esp` (28 valores: 6 estágios x fonte/operando/combinação/escala, atualização do buffer e
 * neblina, texturas, iluminação, bicúbico) cria a versão especializada do shader de fragmentos */
static VkPipeline pipeline_make(uint64_t key, const uint32_t *esp) {
    static const VkBlendOp k_beq[8] = { VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_REVERSE_SUBTRACT, VK_BLEND_OP_MIN, VK_BLEND_OP_MAX,
                                        VK_BLEND_OP_ADD, VK_BLEND_OP_ADD, VK_BLEND_OP_ADD };
    VkPipelineShaderStageCreateInfo st[2] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO }, { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = g_sm_vs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = g_sm_fs; st[1].pName = "main";
    struct { VkBool32 alfa, esp; uint32_t v[28]; } sd; VkSpecializationMapEntry sme[30];   /* (bit 34 da chave: teste de alfa) */
    sd.alfa = (key >> 34) & 1; sd.esp = esp != NULL; if (esp) memcpy(sd.v, esp, sizeof sd.v); else memset(sd.v, 0, sizeof sd.v);
    for (uint32_t i = 0; i < 30; i++) { sme[i].constantID = i; sme[i].offset = i * 4; sme[i].size = 4; }
    VkSpecializationInfo si = { esp ? 30u : 1u, sme, esp ? sizeof sd : 4, &sd }; st[1].pSpecializationInfo = &si;
    VkVertexInputBindingDescription vb = { 0, VSTRIDE * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX };
    static const struct { int n, off; } at[6] = { { 4, 0 }, { 4, 4 }, { 4, 8 }, { 3, 12 }, { 4, 15 }, { 3, 19 } };
    VkVertexInputAttributeDescription va[6];
    for (int i = 0; i < 6; i++) { va[i].location = (uint32_t)i; va[i].binding = 0; va[i].format = at[i].n == 4 ? VK_FORMAT_R32G32B32A32_SFLOAT : VK_FORMAT_R32G32B32_SFLOAT; va[i].offset = (uint32_t)(at[i].off * sizeof(float)); }
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &vb; vi.vertexAttributeDescriptionCount = 6; vi.pVertexAttributeDescriptions = va;
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO }; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO }; vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.depthClampEnable = (VkBool32)g_has_clamp; rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO }; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState ba = { 0 };
    ba.blendEnable = (key >> 1) & 1;
    ba.colorBlendOp = k_beq[(key >> 2) & 7]; ba.alphaBlendOp = k_beq[(key >> 5) & 7];
    ba.srcColorBlendFactor = (VkBlendFactor)((key >> 8) & 15); ba.dstColorBlendFactor = (VkBlendFactor)((key >> 12) & 15);
    ba.srcAlphaBlendFactor = (VkBlendFactor)((key >> 16) & 15); ba.dstAlphaBlendFactor = (VkBlendFactor)((key >> 20) & 15);
    for (int i = 8; i < 24; i += 4) if (((key >> i) & 15) == 15) {           /* (15 = 14 na PICA: SRC_ALPHA_SATURATE) */
        if (i == 8) ba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA_SATURATE; else if (i == 12) ba.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        else if (i == 16) ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA_SATURATE; else ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA_SATURATE; }
    ba.colorWriteMask = (VkColorComponentFlags)((key >> 29) & 15);
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.logicOpEnable = ((key >> 24) & 1) && g_has_logic; cb.logicOp = (VkLogicOp)((key >> 25) & 15);
    cb.attachmentCount = 1; cb.pAttachments = &ba;
    static const VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
        VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
        VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
        VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO }; dy.dynamicStateCount = sizeof dyn / sizeof *dyn; dy.pDynamicStates = dyn;
    VkFormat cf = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo pr = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    pr.colorAttachmentCount = 1; pr.pColorAttachmentFormats = &cf;
    if (key & 1) { pr.depthAttachmentFormat = g_dfmt; pr.stencilAttachmentFormat = g_dfmt; }
    VkGraphicsPipelineCreateInfo ci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    ci.pNext = &pr; ci.stageCount = 2; ci.pStages = st; ci.pVertexInputState = &vi; ci.pInputAssemblyState = &ia; ci.pViewportState = &vp;
    ci.pRasterizationState = &rs; ci.pMultisampleState = &ms; ci.pDepthStencilState = &ds; ci.pColorBlendState = &cb; ci.pDynamicState = &dy;
    ci.layout = g_pl;
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(g_dev, g_pcache, 1, &ci, NULL, &p)) { static int n; if (n++ < 5) fprintf(stderr, "[vk] falha ao criar pipeline %llx\n", (unsigned long long)key); return VK_NULL_HANDLE; }
    return p;
}
static VkPipeline pipeline_get(uint64_t key) {                /* o genérico (sem especialização), na hora */
    for (int i = 0; i < g_npipe; i++) if (g_pipe[i].key == key) return g_pipe[i].p;
    VkPipeline p = pipeline_make(key, NULL);
    if (p && g_npipe < NPIPE) { g_pipe[g_npipe].key = key; g_pipe[g_npipe].p = p; g_npipe++; }
    return p;
}
/* ---- pipelines especializados, compilados numa thread à parte (como no Dolphin e no Citra). O shader genérico
 * decide pixel a pixel toda a configuração do 3DS (6 estágios de cor, texturas, luz); a versão especializada já
 * nasce com ela fixa e roda várias vezes mais rápido. Enquanto não fica pronta, o desenho usa o genérico. Ficam
 * guardados no disco (vk_pipelines.bin), então da segunda vez em diante já estão prontos. VK_SEM_ESPECIALIZAR=1 desliga. */
typedef struct { uint64_t key; uint32_t v[28]; } EspK;
typedef struct { EspK k; VkPipeline p; volatile int st; } EspE;   /* st: 0 vazio, 1 compilando, 2 pronto, 3 falhou */
#define NESP 16384
static EspE *g_esp; static int g_esp_on = -1; static volatile long g_esp_prontos;
static int g_esp_fila[1024]; static int g_esp_ini, g_esp_n;
static pthread_mutex_t g_esp_mx = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_esp_cv = PTHREAD_COND_INITIALIZER;
static const char *k_pcache_arq = "vk_pipelines.bin";
/* vk_pipelines.lista: placa e driver da última vez + as configurações já especializadas. Na abertura, todas voltam
 * para a fila (do cache ficam prontas na hora); se o driver mudou, o cache não vale mais e elas são recompiladas de
 * novo em segundo plano, com um aviso na tela explicando os engasgos. */
static const char *k_lista_arq = "vk_pipelines.lista";
typedef struct { char magic[4]; uint32_t vendor, device, driver; uint8_t uuid[16]; uint32_t n; } ListaCab;
static VkPhysicalDeviceProperties g_props;
static int *g_pre; static int g_npre, g_ipre;                 /* configurações da lista ainda por compilar */
static char g_aviso_vk[2][256]; static volatile int g_aviso_vk_pend[2], g_recompilando;   /* [0] o aviso, [1] o "pronto" */
static uint64_t g_aviso_vk_t;                                 /* quando o primeiro apareceu (o segundo espera ele sumir) */
extern void (*g_ui_aviso)(const char *msg);
static void driver_versao(uint32_t vendor, uint32_t v, char *o, size_t n) {
    if (vendor == 0x10DE) snprintf(o, n, "%u.%02u", (v >> 22) & 0x3FF, (v >> 14) & 0xFF);          /* NVIDIA */
    else if (vendor == 0x8086) snprintf(o, n, "%u.%u", v >> 14, v & 0x3FFF);                        /* Intel (Windows) */
    else snprintf(o, n, "%u.%u.%u", VK_VERSION_MAJOR(v), VK_VERSION_MINOR(v), VK_VERSION_PATCH(v));
}
static void aviso_vk(int qual, const char *m) {
    snprintf(g_aviso_vk[qual], sizeof g_aviso_vk[qual], "%s", m); __atomic_store_n(&g_aviso_vk_pend[qual], 1, __ATOMIC_RELEASE);
    char l[256]; snprintf(l, sizeof l, "%s", m); for (char *q = l; *q; q++) if (*q == '\n') *q = ' ';
    fprintf(stderr, "[vk] %s\n", l);
}
static void lista_salvar(void) {
    if (!g_esp) return;
    FILE *f = fopen("vk_pipelines.lista.tmp", "wb"); if (!f) return;
    ListaCab c; memset(&c, 0, sizeof c); memcpy(c.magic, "PXL1", 4);
    c.vendor = g_props.vendorID; c.device = g_props.deviceID; c.driver = g_props.driverVersion; memcpy(c.uuid, g_props.pipelineCacheUUID, 16);
    fwrite(&c, sizeof c, 1, f);
    uint32_t n = 0;
    for (int i = 0; i < NESP; i++) if (__atomic_load_n(&g_esp[i].st, __ATOMIC_ACQUIRE) == 2) { fwrite(&g_esp[i].k, sizeof g_esp[i].k, 1, f); n++; }
    c.n = n; fseek(f, 0, SEEK_SET); fwrite(&c, sizeof c, 1, f);
    int ok = !ferror(f); fclose(f);
    if (ok) { remove(k_lista_arq); rename("vk_pipelines.lista.tmp", k_lista_arq); }
}
static void pcache_salvar(void) {
    lista_salvar();
    size_t n = 0; if (!g_pcache || vkGetPipelineCacheData(g_dev, g_pcache, &n, NULL) != VK_SUCCESS || !n) return;
    void *d = malloc(n); if (!d) return;
    if (vkGetPipelineCacheData(g_dev, g_pcache, &n, d) == VK_SUCCESS) {
        FILE *f = fopen("vk_pipelines.tmp", "wb");
        if (f) { int ok = fwrite(d, 1, n, f) == n; fclose(f); if (ok) { remove(k_pcache_arq); rename("vk_pipelines.tmp", k_pcache_arq); } }
    }
    free(d);
}
static void *esp_thread(void *arg) {
    (void)arg; long salvos = 0;
    for (;;) {
        pthread_mutex_lock(&g_esp_mx);
        while (!g_esp_n && g_ipre >= g_npre) pthread_cond_wait(&g_esp_cv, &g_esp_mx);
        int i;
        if (g_esp_n) { i = g_esp_fila[g_esp_ini]; g_esp_ini = (g_esp_ini + 1) % 1024; g_esp_n--; }   /* o que está na tela primeiro */
        else i = g_pre[g_ipre++];                       /* depois, as da lista (vezes anteriores) */
        int ultimo_pre = !g_esp_n && g_ipre == g_npre && g_npre > 0;
        pthread_mutex_unlock(&g_esp_mx);
        EspE *e = &g_esp[i];
        VkPipeline p = pipeline_make(e->k.key, e->k.v);
        e->p = p; __atomic_store_n(&e->st, p ? 2 : 3, __ATOMIC_RELEASE);
        if (p) { long n = __atomic_add_fetch(&g_esp_prontos, 1, __ATOMIC_RELAXED); if (n - salvos >= 64) { salvos = n; pcache_salvar(); } }
        if (ultimo_pre) {
            g_npre = 0;
            if (g_recompilando) { char m[200]; snprintf(m, sizeof m, "%s", TR("Shaders recompilados para o novo driver de vídeo. Pronto!", "Shaders rebuilt for the new video driver. Done!",
                                                                  "Shaders recompilados para el nuevo controlador de vídeo. ¡Listo!", "新しいビデオドライバー用のシェーダーを再構築しました。完了！")); aviso_vk(1, m); g_recompilando = 0; }
        }
        if (!g_esp_n && g_ipre >= g_npre && g_esp_prontos != salvos) { salvos = g_esp_prontos; pcache_salvar(); }   /* fila vazia: guarda o que já tem */
    }
    return NULL;
}
static uint32_t esp_hash(const EspK *k) {
    const uint8_t *b = (const uint8_t *)k; uint32_t h = 2166136261u;
    for (size_t i = 0; i < sizeof *k; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}
/* lê a lista da última vez: põe as configurações na tabela (estado "compilando") e na fila de pré-carga */
static void lista_carregar(int cache_mudou) {
    FILE *f = fopen(k_lista_arq, "rb"); ListaCab c; int tem = 0;
    if (f && fread(&c, sizeof c, 1, f) == 1 && !memcmp(c.magic, "PXL1", 4) && c.n <= NESP) tem = 1;
    int mudou = tem ? (c.vendor != g_props.vendorID || c.device != g_props.deviceID || c.driver != g_props.driverVersion ||
                       memcmp(c.uuid, g_props.pipelineCacheUUID, 16)) : cache_mudou;
    if (tem) {
        g_pre = malloc(sizeof *g_pre * (c.n ? c.n : 1));
        EspK k;
        for (uint32_t r = 0; g_pre && r < c.n && fread(&k, sizeof k, 1, f) == 1; r++) {
            uint32_t h = esp_hash(&k);
            for (int tries = 0; tries < 64; tries++) {
                EspE *e = &g_esp[(h + (uint32_t)tries) & (NESP - 1)];
                if (e->st == 0) { e->k = k; e->st = 1; g_pre[g_npre++] = (int)(e - g_esp); break; }
                if (!memcmp(&e->k, &k, sizeof k)) break;
            }
        }
    }
    if (f) fclose(f);
    if (mudou) {
        char de[32] = "?", para[32]; char m[256];
        if (tem) driver_versao(c.vendor, c.driver, de, sizeof de);
        driver_versao(g_props.vendorID, g_props.driverVersion, para, sizeof para);
        if (g_npre) { snprintf(m, sizeof m, TR("Driver de vídeo atualizado (%s -> %s)\nRecompilando %d shaders em segundo plano: pode engasgar um pouco até terminar.",
                                     "Video driver updated (%s -> %s)\nRebuilding %d shaders in the background: it may stutter a little until it's done.",
                                     "Controlador de vídeo actualizado (%s -> %s)\nRecompilando %d shaders en segundo plano: puede trabarse un poco hasta que termine.",
                                     "ビデオドライバーが更新されました（%s → %s）\n%d個のシェーダーをバックグラウンドで再構築中：終わるまで少しカクつくことがあります。"), de, para, g_npre); g_recompilando = 1; }
        else snprintf(m, sizeof m, TR("Driver de vídeo atualizado (versão %s)\nOs shaders serão recompilados conforme aparecem: pode engasgar um pouco no começo.",
                                          "Video driver updated (version %s)\nShaders will be rebuilt as they appear: it may stutter a little at first.",
                                          "Controlador de vídeo actualizado (versión %s)\nLos shaders se recompilarán a medida que aparezcan: puede trabarse un poco al principio.",
                                          "ビデオドライバーが更新されました（バージョン %s）\nシェーダーは表示されるたびに再構築されます：最初は少しカクつくことがあります。"), para);
        aviso_vk(0, m);
    } else if (g_npre) fprintf(stderr, "[vk] %d shaders especializados da última vez, carregando do cache\n", g_npre);
}
/* o pipeline especializado desta configuração, se já estiver pronto; senão pede e devolve NULL */
static int g_cache_mudou;
static VkPipeline esp_get(uint64_t key, const uint32_t *v) {
    if (g_ui_aviso) {                                   /* (a janela já existe) */
        if (g_aviso_vk_pend[0]) { g_aviso_vk_pend[0] = 0; g_ui_aviso(g_aviso_vk[0]); g_aviso_vk_t = prof_now(); }
        else if (__atomic_load_n(&g_aviso_vk_pend[1], __ATOMIC_ACQUIRE) && prof_now() - g_aviso_vk_t > 10500000000ull) { g_aviso_vk_pend[1] = 0; g_ui_aviso(g_aviso_vk[1]); }
    }
    if (g_esp_on < 0) {
        g_esp_on = !ENVC("VK_SEM_ESPECIALIZAR");
        if (g_esp_on) { g_esp = calloc(NESP, sizeof *g_esp); pthread_t t;
                        if (g_esp) lista_carregar(g_cache_mudou);
                        if (!g_esp || pthread_create(&t, NULL, esp_thread, NULL)) g_esp_on = 0; else pthread_detach(t); }
        fprintf(stderr, "[vk] shaders especializados: %s\n", g_esp_on ? "sim (compilados em segundo plano)" : "não");
    }
    if (!g_esp_on) return VK_NULL_HANDLE;
    EspK k; memset(&k, 0, sizeof k); k.key = key; memcpy(k.v, v, sizeof k.v);
    uint32_t h = esp_hash(&k);
    for (int tries = 0; tries < 64; tries++) {
        EspE *e = &g_esp[(h + (uint32_t)tries) & (NESP - 1)];
        int st = __atomic_load_n(&e->st, __ATOMIC_ACQUIRE);
        if (st == 0) {                                  /* novo: entra na fila da thread */
            pthread_mutex_lock(&g_esp_mx);
            if (g_esp_n < 1024) { e->k = k; e->st = 1; g_esp_fila[(g_esp_ini + g_esp_n) % 1024] = (int)(e - g_esp); g_esp_n++; pthread_cond_signal(&g_esp_cv); }
            pthread_mutex_unlock(&g_esp_mx);
            return VK_NULL_HANDLE;
        }
        if (memcmp(&e->k, &k, sizeof k)) continue;
        return st == 2 ? e->p : VK_NULL_HANDLE;
    }
    return VK_NULL_HANDLE;                              /* (tabela cheia nessa região: fica no genérico) */
}
static VkPipeline conv_pipeline(void) {
    if (g_conv_pipe) return g_conv_pipe;
    VkPipelineShaderStageCreateInfo st[2] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO }, { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = g_sm_fvs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = g_sm_cfs; st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO }; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO }; vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO }; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO }; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState ba = { 0 }; ba.colorWriteMask = 15;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO }; cb.attachmentCount = 1; cb.pAttachments = &ba;
    static const VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO }; dy.dynamicStateCount = 2; dy.pDynamicStates = dyn;
    VkFormat cf = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo pr = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO }; pr.colorAttachmentCount = 1; pr.pColorAttachmentFormats = &cf;
    VkGraphicsPipelineCreateInfo ci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    ci.pNext = &pr; ci.stageCount = 2; ci.pStages = st; ci.pVertexInputState = &vi; ci.pInputAssemblyState = &ia; ci.pViewportState = &vp;
    ci.pRasterizationState = &rs; ci.pMultisampleState = &ms; ci.pColorBlendState = &cb; ci.pDynamicState = &dy; ci.layout = g_cpl;
    vkCreateGraphicsPipelines(g_dev, g_pcache, 1, &ci, NULL, &g_conv_pipe);
    return g_conv_pipe;
}

/* ------------------------------------------------------------ início */
static Img g_black, g_lut; static VkSampler g_lut_smp, g_conv_smp;
static int vk_setup(void) {
    p_gipa = (PFN_vkGetInstanceProcAddr)vk_lib_open();
    if (!p_gipa) { fprintf(stderr, "[vk] vulkan-1 não encontrado\n"); return 0; }
#define LG(n) n = (PFN_##n)p_gipa(NULL, #n);
    VK_GLOBAL(LG)
#undef LG
    uint32_t iv = VK_API_VERSION_1_0; if (vkEnumerateInstanceVersion) vkEnumerateInstanceVersion(&iv);
    if (iv < VK_API_VERSION_1_3) { fprintf(stderr, "[vk] Vulkan %u.%u: precisa do 1.3\n", VK_VERSION_MAJOR(iv), VK_VERSION_MINOR(iv)); return 0; }
    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO }; ai.pApplicationName = "XY Project"; ai.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO }; ici.pApplicationInfo = &ai;
    if (vkCreateInstance(&ici, NULL, &g_inst)) { fprintf(stderr, "[vk] falha ao criar a instância\n"); return 0; }
#define LI(n) n = (PFN_##n)p_gipa(g_inst, #n); if (!n) { fprintf(stderr, "[vk] função %s não encontrada\n", #n); return 0; }
    VK_INST(LI)
#undef LI
    uint32_t n = 0; vkEnumeratePhysicalDevices(g_inst, &n, NULL);
    if (!n) { fprintf(stderr, "[vk] nenhuma placa com Vulkan\n"); return 0; }
    VkPhysicalDevice pd[16]; if (n > 16) n = 16; vkEnumeratePhysicalDevices(g_inst, &n, pd);
    int best = -1, bscore = -1;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pd[i], &p);
        if (p.apiVersion < VK_API_VERSION_1_3) continue;
        int sc = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
        if (sc > bscore) { bscore = sc; best = (int)i; }
    }
    if (best < 0) { fprintf(stderr, "[vk] nenhuma placa com Vulkan 1.3\n"); return 0; }
    g_pdev = pd[best];
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(g_pdev, &props); g_props = props;
    VkPhysicalDeviceFeatures f; vkGetPhysicalDeviceFeatures(g_pdev, &f);
    VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 }; f2.pNext = &f13;
    vkGetPhysicalDeviceFeatures2(g_pdev, &f2);
    if (!f13.dynamicRendering) { fprintf(stderr, "[vk] sem renderização dinâmica\n"); return 0; }
    uint32_t ne = 0; vkEnumerateDeviceExtensionProperties(g_pdev, NULL, &ne, NULL);
    VkExtensionProperties *ex = calloc(ne ? ne : 1, sizeof *ex); vkEnumerateDeviceExtensionProperties(g_pdev, NULL, &ne, ex);
    for (uint32_t i = 0; i < ne; i++) if (!strcmp(ex[i].extensionName, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME)) g_has_border = 1;
    free(ex);
    VkPhysicalDeviceCustomBorderColorFeaturesEXT fb = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT };
    if (g_has_border) { VkPhysicalDeviceFeatures2 q = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 }; q.pNext = &fb; vkGetPhysicalDeviceFeatures2(g_pdev, &q);
                        g_has_border = fb.customBorderColors && fb.customBorderColorWithoutFormat; }
    g_has_logic = f.logicOp; g_has_aniso = f.samplerAnisotropy; g_has_clamp = f.depthClamp;
    g_max_aniso = props.limits.maxSamplerAnisotropy < 16 ? props.limits.maxSamplerAnisotropy : 16;
    { VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(g_pdev, VK_FORMAT_D24_UNORM_S8_UINT, &fp);
      VkFormatFeatureFlags need = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
      g_dfmt = (fp.optimalTilingFeatures & need) == need ? VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_D32_SFLOAT_S8_UINT; }
    { VkFormatFeatureFlags bl = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT; VkFormatProperties fp;   /* escala na placa */
      vkGetPhysicalDeviceFormatProperties(g_pdev, VK_FORMAT_R8G8B8A8_UNORM, &fp); g_blit_c = (fp.optimalTilingFeatures & bl) == bl;
      g_blit_lin = g_blit_c && (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) && !ENVC("VK_SEM_REDUCAO");   /* (o travamento era do vídeo da abertura, não disto) */
      vkGetPhysicalDeviceFormatProperties(g_pdev, g_dfmt, &fp); g_blit_d = (fp.optimalTilingFeatures & bl) == bl && !ENVC("VK_SEM_BLIT_PROF");
      if (ENVC("VK_SEM_BLIT")) g_blit_c = g_blit_d = 0;
      fprintf(stderr, "[vk] escala na placa: cor %s, profundidade %s\n", g_blit_c ? "sim" : "não", g_blit_d ? "sim" : "não"); }
    uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(g_pdev, &nq, NULL);
    VkQueueFamilyProperties qf[16]; if (nq > 16) nq = 16; vkGetPhysicalDeviceQueueFamilyProperties(g_pdev, &nq, qf);
    g_qfam = UINT32_MAX;
    for (uint32_t i = 0; i < nq; i++) if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { g_qfam = i; break; }
    if (g_qfam != UINT32_MAX && qf[g_qfam].timestampValidBits && props.limits.timestampPeriod > 0 && ENVC("PICA_PERFIL")) g_ts_ns = props.limits.timestampPeriod;
    g_et_on = ENVC("VK_PERFIL") != NULL;                 /* tempo por etapa (marcas no meio dos lotes): só para depurar */
    if (g_qfam == UINT32_MAX) return 0;
    float prio = 1;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO }; qci.queueFamilyIndex = g_qfam; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    VkPhysicalDeviceVulkan13Features e13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES }; e13.dynamicRendering = VK_TRUE;
    VkPhysicalDeviceCustomBorderColorFeaturesEXT eb = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT };
    eb.customBorderColors = VK_TRUE; eb.customBorderColorWithoutFormat = VK_TRUE;
    if (g_has_border) e13.pNext = &eb;
    VkPhysicalDeviceFeatures ef = { 0 }; ef.logicOp = (VkBool32)g_has_logic; ef.depthClamp = f.depthClamp; ef.samplerAnisotropy = (VkBool32)g_has_aniso;
    const char *exts[1] = { VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO }; dci.pNext = &e13;
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci; dci.pEnabledFeatures = &ef;
    dci.enabledExtensionCount = g_has_border ? 1 : 0; dci.ppEnabledExtensionNames = exts;
    if (vkCreateDevice(g_pdev, &dci, NULL, &g_dev)) { fprintf(stderr, "[vk] falha ao criar o dispositivo\n"); return 0; }
#define LD(n) n = (PFN_##n)vkGetDeviceProcAddr(g_dev, #n); if (!n) { fprintf(stderr, "[vk] função %s não encontrada\n", #n); return 0; }
    VK_DEV(LD)
#undef LD
    vkGetDeviceQueue(g_dev, g_qfam, 0, &g_queue);
    vkGetPhysicalDeviceMemoryProperties(g_pdev, &g_memprops);
    VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO }; cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; cpi.queueFamilyIndex = g_qfam;
    vkCreateCommandPool(g_dev, &cpi, NULL, &g_cpool);
    VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, NSETS }, { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, NSETS * 4 } };
    if (g_ts_ns > 0) { VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO }; qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = NBATCH * QPB;
                       if (vkCreateQueryPool(g_dev, &qi, NULL, &g_qpool) != VK_SUCCESS) g_qpool = VK_NULL_HANDLE; }
    for (int i = 0; i < NBATCH; i++) {
        Batch *b = &g_b[i];
        VkCommandBufferAllocateInfo ca = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO }; ca.commandPool = g_cpool; ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount = 1;
        vkAllocateCommandBuffers(g_dev, &ca, &b->cb);
        VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO }; vkCreateFence(g_dev, &fi, NULL, &b->fence);
        VkDescriptorPoolCreateInfo pi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO }; pi.maxSets = NSETS; pi.poolSizeCount = 2; pi.pPoolSizes = ps;
        vkCreateDescriptorPool(g_dev, &pi, NULL, &b->pool);
        if (!buf_make(&b->vtx, VTX_RING, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, 0) || !buf_make(&b->ubo, UBO_RING, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0) ||
            !buf_make(&b->up, UP_RING, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 0)) { fprintf(stderr, "[vk] sem memória para os buffers\n"); return 0; }
    }
    VkDescriptorSetLayoutBinding lb[5];
    memset(lb, 0, sizeof lb);
    lb[0].binding = 0; lb[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC; lb[0].descriptorCount = 1; lb[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    for (int i = 1; i < 5; i++) { lb[i].binding = (uint32_t)i; lb[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; lb[i].descriptorCount = 1; lb[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT; }
    VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO }; li.bindingCount = 5; li.pBindings = lb;
    vkCreateDescriptorSetLayout(g_dev, &li, NULL, &g_dsl);
    lb[0] = lb[1]; lb[0].binding = 0; lb[1].binding = 1; li.bindingCount = 2;
    vkCreateDescriptorSetLayout(g_dev, &li, NULL, &g_cdsl);
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO }; pli.setLayoutCount = 1; pli.pSetLayouts = &g_dsl;
    vkCreatePipelineLayout(g_dev, &pli, NULL, &g_pl);
    pli.pSetLayouts = &g_cdsl; vkCreatePipelineLayout(g_dev, &pli, NULL, &g_cpl);
    g_sm_vs = shader(k_spv_main_vert, sizeof k_spv_main_vert); g_sm_fs = shader(k_spv_main_frag, sizeof k_spv_main_frag);
#ifdef VK_TEST_SHADER                          /* (só no teste local: shader reduzido para o SwiftShader) */
    if (ENVC("VK_TESTE_FS")) g_sm_fs = shader(k_spv_test_frag, sizeof k_spv_test_frag);
#endif
    g_sm_fvs = shader(k_spv_full_vert, sizeof k_spv_full_vert); g_sm_cfs = shader(k_spv_conv_frag, sizeof k_spv_conv_frag);
    if (!g_sm_vs || !g_sm_fs || !g_sm_fvs || !g_sm_cfs) { fprintf(stderr, "[vk] shaders recusados\n"); return 0; }
    VkPipelineCacheCreateInfo pci = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
    { FILE *f = fopen(k_pcache_arq, "rb"); void *d = NULL; long n = 0;   /* pipelines guardados de outras vezes */
      if (f) { fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET); if (n > 32 && n < (256l << 20) && (d = malloc((size_t)n)) && fread(d, 1, (size_t)n, f) == (size_t)n) { pci.initialDataSize = (size_t)n; pci.pInitialData = d; } fclose(f); }
      if (pci.pInitialData) {                          /* cabeçalho do cache: placa e versão de driver que o gravaram */
          const uint8_t *h = d; uint32_t ven, dev; memcpy(&ven, h + 8, 4); memcpy(&dev, h + 12, 4);
          if (ven != g_props.vendorID || dev != g_props.deviceID || memcmp(h + 16, g_props.pipelineCacheUUID, 16)) {
              g_cache_mudou = 1; pci.initialDataSize = 0; pci.pInitialData = NULL;   /* (não serve: começa um novo) */
          }
      }
      if (vkCreatePipelineCache(g_dev, &pci, NULL, &g_pcache) != VK_SUCCESS) { pci.initialDataSize = 0; pci.pInitialData = NULL; vkCreatePipelineCache(g_dev, &pci, NULL, &g_pcache); }
      else if (pci.initialDataSize) fprintf(stderr, "[vk] pipelines guardados: %ld KB\n", n / 1024);
      free(d); }
    /* textura preta 1x1 (unidades sem textura) e tabela da iluminação 256x24 */
    img_make(&g_black, 1, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0);
    img_make(&g_lut, 256, 24, 1, VK_FORMAT_R32G32_SFLOAT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0);
    to_general(g_black.img, 1, 0); to_general(g_lut.img, 1, 0);
    { VkClearColorValue z = { { 0 } }; VkImageSubresourceRange rr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
      vkCmdClearColorImage(cur()->cb, g_black.img, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &rr);
      vkCmdClearColorImage(cur()->cb, g_lut.img, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &rr); barrier(); }
    VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; sci.maxLod = 0.25f;
    vkCreateSampler(g_dev, &sci, NULL, &g_lut_smp);
    vkCreateSampler(g_dev, &sci, NULL, &g_conv_smp);
    submit_wait();
    morton_init();
    fprintf(stderr, "[vk] %s | Vulkan %u.%u.%u | profundidade %s%s%s\n", props.deviceName, VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion),
            VK_VERSION_PATCH(props.apiVersion), g_dfmt == VK_FORMAT_D24_UNORM_S8_UINT ? "D24S8" : "D32S8", g_has_logic ? "" : " | sem operação lógica",
            g_has_border ? "" : " | sem cor de borda");
    g_render_name = "Vulkan";
    return 1;
}
/* chamada pelo pica_gl.c uma vez, antes da primeira lista: 1 = Vulkan pronto */
int pica_vk_init(void) {
    if (g_vk_state) return g_vk_state > 0;
    pthread_mutex_lock(&g_vk_lock); g_lock_owner = pthread_self(); g_lock_depth = 1;
    g_vk_state = 1;                            /* (vk_setup grava comandos) */
    int ok = vk_setup();
    g_vk_state = ok ? 1 : -1;
    g_lock_depth = 0; pthread_mutex_unlock(&g_vk_lock);
    if (!ok) fprintf(stderr, "[vk] Vulkan indisponível: voltando ao OpenGL\n");
    return ok;
}

/* ------------------------------------------------------------ texturas de um desenho */
static int tex_filter(void) {
    if (g_tex_filter < 0) { const char *e = ENVC("FILTRO_TEXTURA"); int v = e ? atoi(e) : 0; g_tex_filter = v >= 0 && v <= 3 ? v : 0; }
    if (g_tex_filter_req >= 0) { g_tex_filter = g_tex_filter_req & 3; g_tex_filter_req = -1; }
    return g_tex_filter;
}
/* profundidade+estêncil lida como RGBA8 (contornos, máscaras): convertida na placa para uma cópia */
static VkImageView depth_as_rgba(Surf *s) {
    if (s->conv.img && s->conv_ver == s->ver) return s->conv.view;
    clr_flush(s);
    VkPipeline p = conv_pipeline(); if (!p) return VK_NULL_HANDLE;
    if (!s->conv.img) {
        if (!img_make(&s->conv, s->w * s->sc, s->h * s->sc, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 0)) return VK_NULL_HANDLE;
        rp_end(); to_general(s->conv.img, 1, 0);
    }
    Batch *b = cur();
    if (b->nsets >= NSETS) { batch_submit(); b = cur(); }
    VkDescriptorSet ds; VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = b->pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &g_cdsl;
    if (vkAllocateDescriptorSets(g_dev, &ai, &ds)) return VK_NULL_HANDLE;
    b->nsets++;
    VkDescriptorImageInfo ii[2] = { { g_conv_smp, s->im.dview, VK_IMAGE_LAYOUT_GENERAL }, { g_conv_smp, s->im.sview, VK_IMAGE_LAYOUT_GENERAL } };
    VkWriteDescriptorSet w[2] = { { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }, { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET } };
    for (int i = 0; i < 2; i++) { w[i].dstSet = ds; w[i].dstBinding = (uint32_t)i; w[i].descriptorCount = 1; w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[i].pImageInfo = &ii[i]; }
    vkUpdateDescriptorSets(g_dev, 2, w, 0, NULL);
    rp_end(); ts_mark(Et_OUTRO);
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    ca.imageView = s->conv.view; ca.imageLayout = VK_IMAGE_LAYOUT_GENERAL; ca.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    int W = s->w * s->sc, H = s->h * s->sc;
    ri.renderArea.extent.width = (uint32_t)W; ri.renderArea.extent.height = (uint32_t)H; ri.layerCount = 1; ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca;
    VkCommandBuffer cb = b->cb;
    vkCmdBeginRendering(cb, &ri);
    VkViewport vp = { 0, 0, (float)W, (float)H, 0, 1 }; VkRect2D sr = { { 0, 0 }, { (uint32_t)W, (uint32_t)H } };
    vkCmdSetViewport(cb, 0, 1, &vp); vkCmdSetScissor(cb, 0, 1, &sr);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_cpl, 0, 1, &ds, 0, NULL);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
    barrier(); ts_mark(Et_CONV);
    s->conv_ver = s->ver; g_pf_conv++; g_pf_conv_px += (double)W * H;
    return s->conv.view;
}
static VkImageView surface_texture(uint32_t addr, int w, int h, uint32_t fmt, const Fb *f) {
    for (int i = 0; i < NSURF; i++) {
        Surf *s = &g_surf[i];
        if (!s->alive || !s->valid || s->addr != addr || s->w != w || s->h != h) continue;
        if (!s->depth && (uint32_t)s->fmt == fmt && addr != f->color) { clr_flush(s); return s->im.view; }
        if (s->depth && s->fmt == 3 && fmt == 0) return depth_as_rgba(s);
    }
    return VK_NULL_HANDLE;
}
static void bind_textures(const Fb *f, uint32_t direct[3], VkImageView views[3], VkSampler smps[3], uint32_t *bicubic) {
    static const uint32_t cfg_reg[3] = { 0x81, 0x91, 0x99 }, fmt_reg[3] = { 0x8E, 0x96, 0x9E };
    uint32_t tcfg = R(0x80);
    int tf = tex_filter();
    *bicubic = 0;
    for (int unit = 0; unit < 3; unit++) {
        views[unit] = g_black.view; smps[unit] = g_lut_smp; direct[unit] = 0;
        if (!((tcfg >> unit) & 1)) continue;
        uint32_t b = cfg_reg[unit], size = R(b + 1), cfg = R(b + 2), addr = p2v(R(b + 4) * 8), fmt = R(fmt_reg[unit]) & 0xF;
        int w = (size >> 16) & 0x7FF, h = size & 0x7FF;
        if (!(w && h && fmt <= 13 && w <= 1024 && h <= 1024)) continue;
        uint32_t tb = (uint32_t)((size_t)w * h * k_tex_bits[fmt] / 8);
        VkImageView st = surface_texture(addr, w, h, fmt, f);
        if (st) { views[unit] = st; direct[unit] = 1; }
        for (int i = 0; i < NSURF && !st; i++)                /* desenhado na placa e lido como textura: vai para a memória antes */
            if (g_surf[i].alive && g_surf[i].dirty && overlap(addr, tb, g_surf[i].addr, g_surf[i].size)) surf_writeback(&g_surf[i]);
        uint32_t lod = R(b + 3);
        int levels = st ? 1 : (int)((lod >> 16) & 0xF) + 1;
        if (!st) { VkImageView v = tex_get(addr, fmt, w, h, levels); if (v) views[unit] = v; }
        int magl = (cfg >> 1) & 1, lin = (cfg >> 2) & 1, mlin = (cfg >> 24) & 1;
        if (tf == 1 && !st) { lin = 1; mlin = 1; magl = 1; }
        int an = tf >= 2 && !st && lin;
        if (tf == 3 && !st && magl) *bicubic |= 1u << unit;
        float bias = (float)((int32_t)(lod << 19) >> 19) / 256.0f, minl = (float)((lod >> 24) & 0xF), maxl = (float)((lod >> 16) & 0xF);
        /* (o número de níveis da textura limita o máximo; sem mipmap, só o nível 0) */
        if (!st) { int lv = 0; for (int l = 0; l < levels; l++) { if ((w >> l) < 8 || (h >> l) < 8) break; lv = l; } if (maxl > lv) maxl = (float)lv; }
        smps[unit] = sampler_get(lin, magl, mlin, levels > 1, (cfg >> 12) & 7, (cfg >> 8) & 7, bias, minl, maxl, an, R(b));
        if (!smps[unit]) smps[unit] = g_lut_smp;
    }
}

/* ------------------------------------------------------------ desenho */
static float *g_vb; static size_t g_vb_cap;
static float g_fog[128][2]; static int g_fog_init;
static const VkCompareOp k_cmp[8] = { VK_COMPARE_OP_NEVER, VK_COMPARE_OP_ALWAYS, VK_COMPARE_OP_EQUAL, VK_COMPARE_OP_NOT_EQUAL,
                                      VK_COMPARE_OP_LESS, VK_COMPARE_OP_LESS_OR_EQUAL, VK_COMPARE_OP_GREATER, VK_COMPARE_OP_GREATER_OR_EQUAL };
static struct { uint32_t c, d, key, dcm, sc; int w, h, nd, nv; float z, wv; } g_lsum[16]; static int g_lsum_n, g_lsum_on = -1;
void pica_vk_draw(const Fb *f, const Tri *t, int n) {
    if (!n) return;
    uint32_t direct[3], bicubic; VkImageView views[3]; VkSampler smps[3];
    bind_textures(f, direct, views, smps, &bicubic);
    /* tabela da iluminação mudou: copia para a placa (fora da passada de desenho) */
    float L[49][3], LD[8][2]; uint32_t lu[20];
    if (g_light_on) {
        light_gl_params(lu, L, LD);
        if (g_light_lut_dirty) {
            rp_end();
            VkBuffer sb; VkDeviceSize so; size_t bytes = 256 * 24 * 2 * sizeof(float);
            void *p = ring_alloc(2, bytes, 16, &sb, &so);
            if (p) {
                memcpy(p, light_lut_data(), bytes);
                VkBufferImageCopy r; memset(&r, 0, sizeof r);
                r.bufferOffset = so; r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; r.imageSubresource.layerCount = 1;
                r.imageExtent.width = 256; r.imageExtent.height = 24; r.imageExtent.depth = 1;
                barrier();
                vkCmdCopyBufferToImage(cur()->cb, sb, g_lut.img, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
                barrier();
            }
            g_light_lut_dirty = 0;
        }
    } else memset(lu, 0, sizeof lu);
    { extern uint32_t g_fog_lut[128]; extern volatile int g_fog_lut_dirty; extern const uint32_t *g_fog_lut_override;
      if ((R(0xE0) & 7) == 5 && (g_fog_lut_dirty || !g_fog_init)) {
          const uint32_t *fog = g_fog_lut_override ? g_fog_lut_override : g_fog_lut;
          for (int i = 0; i < 128; i++) { uint32_t e = fog[i]; g_fog[i][0] = (float)((e >> 13) & 0x7FF) / 2047.0f; g_fog[i][1] = (float)((int32_t)(e << 19) >> 19) / 2047.0f; }
          g_fog_lut_dirty = 0; g_fog_init = 1;
      } }
    g_cur_c = surf_get(f->color, f->w, f->h, f->cfmt, 0);
    g_cur_d = f->depth ? surf_get(f->depth, f->w, f->h, f->dfmt, 1) : NULL;
    if (g_cur_c && g_cur_d && !g_cur_c->alive) g_cur_c = surf_get(f->color, f->w, f->h, f->cfmt, 0);
    if (!g_cur_c) return;
    /* vértices: descarte de faces como no software, tela -> coordenadas normalizadas, grade de 1/16 de pixel */
    size_t need = (size_t)n * 3 * VSTRIDE;
    if (need > g_vb_cap) { g_vb_cap = need; g_vb = realloc(g_vb, need * sizeof(float)); }
    uint32_t cull = R(0x40) & 3;
    float sx = 2.0f / f->w, sy = 2.0f / f->h;
    int nv = 0;
    for (int i = 0; i < n; i++) {
        const SV *v0 = &t[i].v[0], *v1 = &t[i].v[1], *v2 = &t[i].v[2];
        float area = (v1->x - v0->x) * (v2->y - v0->y) - (v1->y - v0->y) * (v2->x - v0->x);
        if (area == 0 || (cull == 1 && area > 0) || (cull == 2 && area < 0)) continue;
        float qs[3] = { 1, 1, 1 };
        if (g_light_on) {
            const float *q0 = v0->v.quat, *q1 = v1->v.quat, *q2 = v2->v.quat;
            if (q0[0] * q1[0] + q0[1] * q1[1] + q0[2] * q1[2] + q0[3] * q1[3] < 0) qs[1] = -1;
            if (q0[0] * q2[0] + q0[1] * q2[1] + q0[2] * q2[2] + q0[3] * q2[3] < 0) qs[2] = -1;
        }
        const SV *vv[3] = { v0, v1, v2 };
        for (int k = 0; k < 3; k++) {
            const SV *s = vv[k]; const Vertex *v = &s->v;
            float *o = g_vb + (size_t)nv * VSTRIDE;
            float qx = floorf(s->x * 16.0f + 0.5f) * (1.0f / 16.0f), qy = floorf(s->y * 16.0f + 0.5f) * (1.0f / 16.0f);
            o[0] = qx * sx - 1.0f; o[1] = qy * sy - 1.0f; o[2] = s->z * 2.0f - 1.0f; o[3] = 1.0f / s->invw;
            memcpy(o + 4, v->color, 4 * sizeof(float));
            o[8] = v->tc0[0]; o[9] = v->tc0[1]; o[10] = v->tc1[0]; o[11] = v->tc1[1]; o[12] = v->tc2[0]; o[13] = v->tc2[1]; o[14] = v->tc0w;
            for (int j = 0; j < 4; j++) o[15 + j] = v->quat[j] * qs[k];
            memcpy(o + 19, v->view, 3 * sizeof(float));
            nv++;
        }
    }
    g_pica_tris += nv / 3;
    if (!nv) return;
    /* estado */
    uint32_t dcm = R(0x107);
    int allow_ds = (R(0x115) & 3) != 0, allow_c = (R(0x113) & 0xF) != 0;
    int dtest = dcm & 1, dwrite = (dcm >> 12) & 1;
    uint64_t key = g_cur_d ? 1 : 0;
    if ((R(0x104) & 1) || ENVC("VK_SEMPRE_ALFA")) key |= (uint64_t)1 << 34;   /* teste de alfa (o shader só descarta pixels com ele) */
    uint32_t cm = allow_c ? (dcm >> 8) & 0xF : 0;
    key |= (uint64_t)cm << 29;
    if (R(0x100) & 0x100) {
        uint32_t bl = R(0x101);
        key |= 2 | (uint64_t)(bl & 7) << 2 | (uint64_t)((bl >> 8) & 7) << 5 | (uint64_t)((bl >> 16) & 0xFFFF) << 8;
    } else {
        static const uint8_t k_lop[16] = { 0, 1, 2, 3, 15, 12, 5, 10, 14, 7, 8, 6, 9, 4, 11, 13 };   /* PICA -> VkLogicOp */
        uint32_t lop = k_lop[R(0x102) & 0xF];
        if (lop != VK_LOGIC_OP_COPY && g_has_logic) key |= (uint64_t)1 << 24 | (uint64_t)lop << 25;
    }
    VkPipeline pipe = VK_NULL_HANDLE;
    { uint32_t v[28]; static const uint32_t tb[6] = { 0xC0, 0xC8, 0xD0, 0xD8, 0xF0, 0xF8 };
      for (int s2 = 0; s2 < 6; s2++) { v[s2 * 4] = R(tb[s2]); v[s2 * 4 + 1] = R(tb[s2] + 1); v[s2 * 4 + 2] = R(tb[s2] + 2); v[s2 * 4 + 3] = R(tb[s2] + 4); }
      v[24] = R(0xE0); v[25] = R(0x80); v[26] = lu[0]; v[27] = bicubic;
      pipe = esp_get(key, v); if (pipe) g_pf_esp++; }
    if (!pipe) pipe = pipeline_get(key);
    if (!pipe) return;
    /* dados do shader de fragmentos */
    static const uint32_t k_tev_base[6] = { 0xC0, 0xC8, 0xD0, 0xD8, 0xF0, 0xF8 };
    struct { uint32_t u[64]; float L[49][4]; float LD[8][2]; float fog[128][2]; } U;
    memset(U.u, 0, sizeof U.u);
    for (int st = 0; st < 6; st++) for (int k = 0; k < 5; k++) U.u[st * 5 + k] = R(k_tev_base[st] + k);
    U.u[30] = R(0xFD); U.u[31] = R(0xE0); U.u[32] = R(0x80); U.u[33] = R(0x104);
    memcpy(U.u + 34, lu, sizeof lu);
    U.u[54] = direct[0]; U.u[55] = direct[1]; U.u[56] = direct[2]; U.u[57] = R(0xE1); U.u[58] = R(0x83); U.u[59] = bicubic;
    for (int i = 0; i < 49; i++) { if (g_light_on) memcpy(U.L[i], L[i], 12); else memset(U.L[i], 0, 12); U.L[i][3] = 0; }
    if (g_light_on) memcpy(U.LD, LD, sizeof LD); else memset(U.LD, 0, sizeof U.LD);
    memcpy(U.fog, g_fog, sizeof U.fog);
    /* espaço no lote para os dados do desenho (senão envia este e começa outro: tudo no mesmo lote) */
    VkDeviceSize vbytes = (VkDeviceSize)nv * VSTRIDE * sizeof(float);
    { Batch *c0 = cur();
      if (c0->nsets >= NSETS || ((c0->ubo_off + 255) & ~(VkDeviceSize)255) + sizeof U > c0->ubo.size ||
          (vbytes <= c0->vtx.size && ((c0->vtx_off + 15) & ~(VkDeviceSize)15) + vbytes > c0->vtx.size)) batch_submit(); }
    Batch *b = cur();
    VkBuffer ub, vbuf; VkDeviceSize uo, vo;
    void *up = ring_alloc(1, sizeof U, 256, &ub, &uo);
    void *vp_ = ring_alloc(0, vbytes, 16, &vbuf, &vo);
    if (!up || !vp_) return;
    memcpy(up, &U, sizeof U);
    memcpy(vp_, g_vb, (size_t)vbytes);
    VkDescriptorSet ds; VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = b->pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &g_dsl;
    if (vkAllocateDescriptorSets(g_dev, &ai, &ds)) return;
    b->nsets++;
    VkDescriptorBufferInfo bi = { ub, 0, sizeof U };
    VkDescriptorImageInfo ii[4];
    for (int i = 0; i < 3; i++) { ii[i].sampler = smps[i]; ii[i].imageView = views[i]; ii[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL; }
    ii[3].sampler = g_lut_smp; ii[3].imageView = g_lut.view; ii[3].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet w[5]; memset(w, 0, sizeof w);
    for (int i = 0; i < 5; i++) { w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstSet = ds; w[i].dstBinding = (uint32_t)i; w[i].descriptorCount = 1; }
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC; w[0].pBufferInfo = &bi;
    for (int i = 1; i < 5; i++) { w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[i].pImageInfo = &ii[i - 1]; }
    vkUpdateDescriptorSets(g_dev, 5, w, 0, NULL);
    /* destino e estado dinâmico */
    rp_begin(g_cur_c, g_cur_d);
    VkCommandBuffer cb = cur()->cb;
    int W = f->w * g_cur_c->sc, H = f->h * g_cur_c->sc;
    VkViewport vp = { 0, 0, (float)W, (float)H, 0, 1 };
    vkCmdSetViewport(cb, 0, 1, &vp);
    VkRect2D sr = { { 0, 0 }, { (uint32_t)W, (uint32_t)H } };
    if ((R(0x65) & 3) == 3) {
        int x0 = R(0x66) & 0x3FF, y0 = (R(0x66) >> 16) & 0x3FF, x1 = R(0x67) & 0x3FF, y1 = (R(0x67) >> 16) & 0x3FF, k = g_cur_c->sc;
        int ax = x0 * k, ay = y0 * k, aw = x1 >= x0 ? (x1 - x0 + 1) * k : 0, ah = y1 >= y0 ? (y1 - y0 + 1) * k : 0;
        if (ax > W) ax = W; if (ay > H) ay = H; if (ax + aw > W) aw = W - ax; if (ay + ah > H) ah = H - ay;
        sr.offset.x = ax; sr.offset.y = ay; sr.extent.width = (uint32_t)aw; sr.extent.height = (uint32_t)ah;
    }
    vkCmdSetScissor(cb, 0, 1, &sr);
    { uint32_t bc = R(0x103); float c[4] = { (bc & 0xFF) / 255.0f, ((bc >> 8) & 0xFF) / 255.0f, ((bc >> 16) & 0xFF) / 255.0f, (bc >> 24) / 255.0f };
      vkCmdSetBlendConstants(cb, c); }
    if (g_cur_d && (dtest || dwrite)) {
        vkCmdSetDepthTestEnable(cb, VK_TRUE);
        vkCmdSetDepthCompareOp(cb, dtest ? k_cmp[(dcm >> 4) & 7] : VK_COMPARE_OP_ALWAYS);
        vkCmdSetDepthWriteEnable(cb, dwrite && allow_ds);
    } else { vkCmdSetDepthTestEnable(cb, VK_FALSE); vkCmdSetDepthWriteEnable(cb, VK_FALSE); vkCmdSetDepthCompareOp(cb, VK_COMPARE_OP_ALWAYS); }
    uint32_t st = R(0x105), sop = R(0x106);
    if ((st & 1) && g_cur_d && f->dfmt == 3) {
        vkCmdSetStencilTestEnable(cb, VK_TRUE);
        vkCmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_AND_BACK, (VkStencilOp)(sop & 7), (VkStencilOp)((sop >> 8) & 7), (VkStencilOp)((sop >> 4) & 7), k_cmp[(st >> 4) & 7]);
        vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, (st >> 16) & 0xFF);
        vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, (st >> 24) & 0xFF);
        vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, allow_ds ? (st >> 8) & 0xFF : 0);
    } else {
        vkCmdSetStencilTestEnable(cb, VK_FALSE);
        vkCmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS);
        vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0); vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
        vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    }
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    uint32_t dynoff = (uint32_t)uo;
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pl, 0, 1, &ds, 1, &dynoff);
    vkCmdBindVertexBuffers(cb, 0, 1, &vbuf, &vo);
    vkCmdDraw(cb, (uint32_t)nv, 1, 0, 0); g_pf_draw++; g_pf_vtx += nv;
    { static int lg = -1; if (lg < 0) lg = ENVC("VK_LOG") != NULL; static long nd; static long from = -1; if (from < 0) { const char *e = ENVC("VK_LOG_FROM"); from = e ? atol(e) : 0; }
      if (lg && (long)g_list_serial >= from && nd++ < 400) fprintf(stderr, "[vk] desenho %ld: lote %lu, %d vértices, chave %llx, alvo %08x\n", nd, b->serial, nv, (unsigned long long)key, f->color); }
    if (g_lsum_on) { int k; for (k = 0; k < g_lsum_n; k++) if (g_lsum[k].c == f->color && g_lsum[k].d == f->depth && g_lsum[k].key == (uint32_t)key) break;
        if (k == g_lsum_n && k < 16) { g_lsum_n++; g_lsum[k].c = f->color; g_lsum[k].d = f->depth; g_lsum[k].key = (uint32_t)key; g_lsum[k].nd = g_lsum[k].nv = 0; g_lsum[k].w = f->w; g_lsum[k].h = f->h;
                                       g_lsum[k].dcm = dcm; g_lsum[k].sc = sr.extent.width << 16 | sr.extent.height; g_lsum[k].z = g_vb[2]; g_lsum[k].wv = g_vb[3]; }
        if (k < 16) { g_lsum[k].nd++; g_lsum[k].nv += nv; } }
    if (cm && (R(0x113) & 0xF)) { g_cur_c->dirty = 1; g_cur_c->ver++; }
    if (g_cur_d && allow_ds && (dwrite || ((st & 1) && f->dfmt == 3))) { g_cur_d->dirty = 1; g_cur_d->ver++; }
}

/* ------------------------------------------------------------ listas */
int pica_vk_active(void) { return g_in_list; }
static void pend_poll(int upto);
int pica_vk_list_begin(void) {
    if (g_vk_state <= 0) return 0;
    pthread_mutex_lock(&g_vk_lock); g_lock_owner = pthread_self(); g_lock_depth = 1;
    { static int init; if (!init) { init = 1; const char *e = ENVC("VK_RES"); if (!e) e = ENVC("RESOLUCAO"); if (e && !g_res_req) g_res_req = atoi(e); } }
    if (g_res_req && g_res_req != g_sc && g_res_req >= 1 && g_res_req <= 9) {   /* até 9x (4K) */
        for (int i = 0; i < NSURF; i++) if (g_surf[i].alive) { surf_writeback(&g_surf[i]); surf_free(&g_surf[i]); }
        g_sc = g_res_req;
        fprintf(stderr, "[gl] resolução interna %dx\n", g_sc);
    }
    g_res_req = 0;
    g_in_list = 1; g_list_serial++; if (!g_list_serial) g_list_serial = 1;
    if (g_lsum_on < 0) g_lsum_on = ENVC("VK_LOG2") != NULL;
    g_lsum_n = 0;
    pend_poll(0);
    return 1;
}
void pica_vk_list_end(void) {
    if (!g_in_list) return;
    { static int pf = -1; static uint64_t t_last; if (pf < 0) pf = ENVC("PICA_PERFIL") != NULL;   /* a cada ~5 s, com o perfil */
      uint64_t now = prof_now();
      if (pf && now - t_last > 5000000000ull) {
          if (t_last) fprintf(stderr, "[vk] cópias em 5 s: placa->memória %ld cor + %ld prof. (%.0f ms), memória->placa %ld cor + %ld prof.\n",
                              g_st_wb[0], g_st_wb[1], g_st_wb_ns / 1e6, g_st_up[0], g_st_up[1]);
          if (t_last) {
              double seg = (now - t_last) / 1e9; char sup[600] = ""; size_t o = 0; double tot = 0;
              for (int i = 0; i < NSURF; i++) { Surf *s = &g_surf[i]; if (!s->alive) continue;
                  tot += (double)s->w * s->sc * s->h * s->sc;
                  if (o < sizeof sup - 40) o += (size_t)snprintf(sup + o, sizeof sup - o, " %dx%d%s", s->w * s->sc, s->h * s->sc, s->depth ? "p" : ""); }
              fprintf(stderr, "[vk] placa ocupada %.0f%% (%.1f ms/s) | CPU esperando a placa %.1f ms/s | por segundo: %.0f lotes, %.0f desenhos (%.0f mil vértices), %.0f passadas, %.0f barreiras, %.0f limpezas (%.0f Mpx), %.0f conversões de profundidade (%.0f Mpx), %.0f limpezas no começo da passada\n",
                      g_pf_gpu_ns / 1e7 / seg, g_pf_gpu_ns / 1e6 / seg, g_pf_wait_ns / 1e6 / seg, g_pf_lotes / seg, g_pf_draw / seg, g_pf_vtx / 1e3 / seg, g_pf_rp / seg, g_pf_bar / seg,
                      g_pf_clear / seg, g_pf_clear_px / 1e6 / seg, g_pf_conv / seg, g_pf_conv_px / 1e6 / seg, g_pf_clear_rp / seg);
              fprintf(stderr, "[vk] superfícies (%.0f Mpx):%s\n", tot / 1e6, sup);
              if (g_et_on) fprintf(stderr, "[vk] tempo da placa por etapa (ms/s): desenhos %.1f, limpezas %.1f, conversões de profundidade %.1f, cópias de tela %.1f, resto %.1f\n",
                      g_pf_et[Et_DESENHO] / 1e6 / seg, g_pf_et[Et_LIMPEZA] / 1e6 / seg, g_pf_et[Et_CONV] / 1e6 / seg, g_pf_et[Et_COPIA] / 1e6 / seg, g_pf_et[Et_OUTRO] / 1e6 / seg);
              fprintf(stderr, "[vk] desenhos com shader especializado: %.0f%% (%ld pipelines prontos)\n", g_pf_draw ? 100.0 * g_pf_esp / g_pf_draw : 0.0, g_esp_prontos);
              fprintf(stderr, "[vk] esperas: lote para reaproveitar %.1f ms/s, placa inteira %.1f ms/s (%.0f vezes/s)\n",
                      g_pf_wait_anel / 1e6 / seg, g_pf_wait_tudo / 1e6 / seg, g_pf_n_tudo / seg);
          }
          g_pf_gpu_ns = g_pf_wait_ns = 0; g_pf_wait_anel = g_pf_wait_tudo = 0; g_pf_n_tudo = 0; g_pf_esp = 0; memset(g_pf_et, 0, sizeof g_pf_et); g_pf_draw = g_pf_vtx = g_pf_rp = g_pf_conv = g_pf_clear = g_pf_bar = g_pf_lotes = 0; g_pf_conv_px = g_pf_clear_px = 0; g_pf_clear_rp = 0;
          t_last = now; g_st_wb[0] = g_st_wb[1] = g_st_up[0] = g_st_up[1] = 0; g_st_wb_ns = 0;
      } }
    if (g_lsum_on > 0 && g_list_serial % 97 == 0)
        for (int k = 0; k < g_lsum_n; k++) fprintf(stderr, "[vk2] lista %u: cor %08x prof %08x %dx%d chave %x dcm %x tesoura %dx%d z %.3f w %.3f: %d desenhos %d vértices\n",
            g_list_serial, g_lsum[k].c, g_lsum[k].d, g_lsum[k].w, g_lsum[k].h, g_lsum[k].key, g_lsum[k].dcm, g_lsum[k].sc >> 16, g_lsum[k].sc & 0xFFFF, g_lsum[k].z, g_lsum[k].wv, g_lsum[k].nd, g_lsum[k].nv);
    batch_submit();
    g_in_list = 0;
    g_lock_depth = 0; pthread_mutex_unlock(&g_vk_lock);
}

/* ------------------------------------------------------------ transferências de tela (leituras assíncronas)
 * Como no OpenGL: a cópia da superfície vai para um buffer da CPU e só é usada quando a placa termina (ou na
 * hora, se alguém precisar daquela memória antes). Uma leitura em escala serve às duas saídas: a imagem
 * grande da janela e a memória da tela (reduzida na CPU). */
#define NPEND 16
typedef struct { uint32_t out, len; int w, h, sc, out_fmt, hd, mem; Buf buf; unsigned long serial; } Pend;
static Pend g_pend[NPEND]; static int g_pend_head, g_pend_n;
static void pend_finish_oldest(void) {
    Pend *p = &g_pend[g_pend_head];
    serial_wait(p->serial);
    int W = p->w * p->sc, H = p->h * p->sc;
    const uint8_t *m = p->buf.p;
    { static int dm = -1, nn; if (dm < 0) dm = ENVC("VK_DUMP") != NULL;
      if (dm && nn < 40 && p->h == 400) { char fn[64]; snprintf(fn, sizeof fn, "vkdump_%02d.ppm", nn++); FILE *fp = fopen(fn, "wb");
          if (fp) { fprintf(fp, "P6 %d %d 255\n", W, H); for (int i = 0; i < W * H; i++) fwrite(m + (size_t)i * 4, 1, 3, fp); fclose(fp); } } }
    if (p->hd) { uint32_t *px = malloc((size_t)W * H * 4); memcpy(px, m, (size_t)W * H * 4); pica_hd_store(p->out, px, W, H); }
    if (p->mem) {
        uint32_t bpp = p->out_fmt == 0 ? 4 : p->out_fmt == 1 ? 3 : 2;
        int k = p->sc, c = p->sc / 2;
        plat_touch(g_mem + p->out, p->len);
        for (int y = 0; y < p->h; y++) {
            const uint8_t *row = m + (size_t)((p->h - 1 - y) * k + c) * W * 4;
            uint8_t *d = g_mem + p->out + (size_t)y * p->w * bpp;
            for (int x = 0; x < p->w; x++, d += bpp) {
                const uint8_t *q = row + (size_t)(x * k + c) * 4;
                uint8_t r = q[0], g = q[1], b = q[2];
                switch (p->out_fmt) {
                case 0: d[0] = 255; d[1] = b; d[2] = g; d[3] = r; break;
                case 1: d[0] = b; d[1] = g; d[2] = r; break;
                case 2: { uint16_t v = (r >> 3) << 11 | (g >> 2) << 5 | b >> 3; d[0] = v; d[1] = v >> 8; break; }
                case 3: { uint16_t v = (r >> 3) << 11 | (g >> 3) << 6 | (b >> 3) << 1 | 1; d[0] = v; d[1] = v >> 8; break; }
                default: { uint16_t v = (r >> 4) << 12 | (g >> 4) << 8 | (b >> 4) << 4 | 15; d[0] = v; d[1] = v >> 8; break; }
                }
            }
        }
        tex_invalidate_range(p->out, p->len);
        pica_gl_out_mark(p->out);
    }
    rb_put(&p->buf);
    if (p->out) pica_pout_add(p->out, -1);
    g_pend_done = 1;
    g_pend_head = (g_pend_head + 1) % NPEND; g_pend_n--;
}
static void pend_poll(int upto) {
    while (g_pend_n) {
        Pend *p = &g_pend[g_pend_head];
        if (upto <= 0) { if (!serial_poll(p->serial)) break; }
        else g_pend_forced++;
        pend_finish_oldest();
        upto--;
    }
}
static void pend_resolve_range(uint32_t a, uint32_t len) {
    if (!g_pend_n) return;
    int last = -1;
    for (int k = 0; k < g_pend_n; k++) { Pend *p = &g_pend[(g_pend_head + k) % NPEND]; if (p->mem && overlap(p->out, p->len, a, len)) last = k; }
    if (last < 0) return;
    int c = ctx_enter(); if (!c) return;
    if (g_b[g_cur].recording) batch_submit();
    pend_poll(last + 1);
    ctx_leave(c);
}
void pica_vk_poll(void) { if (g_pend_n) pend_poll(0); }
int pica_vk_pending(void) { return g_pend_n; }
void pica_vk_pending_wait(int us) {
    if (!g_pend_n) return;
    int c = ctx_enter(); if (!c) return;
    Pend *p = &g_pend[g_pend_head];
    for (int i = 0; i < NBATCH; i++)
        if (g_b[i].pending && g_b[i].serial <= p->serial) vkWaitForFences(g_dev, 1, &g_b[i].fence, VK_TRUE, (uint64_t)us * 1000);
    pend_poll(0);
    ctx_leave(c);
}
void pica_vk_finish_hd(void) {
    if (!g_pend_n) return;
    int c = ctx_enter(); if (!c) return;
    if (g_b[g_cur].recording) batch_submit();
    while (g_pend_n) pend_finish_oldest();
    ctx_leave(c);
}
int pica_vk_transfer(uint32_t in, int iw, int ih, uint32_t out, int ow, int oh, uint32_t flags, int in_fmt, int out_fmt) {
    (void)ih;
    if (g_vk_state <= 0) return 0;
    static int off = -1; if (off < 0) off = ENVC("GL_LEITURA_SINCRONA") != NULL;
    if (flags & 0x03000023) return 0;
    if (in_fmt != 0 || out_fmt > 4) return 0;
    int c = ctx_enter(); if (!c) return 0;
    Surf *s = NULL; int sy0 = 0;
    for (int i = 0; i < NSURF; i++) {
        Surf *q = &g_surf[i];
        if (!q->alive || !q->valid || q->depth || q->fmt != 0 || in < q->addr || in >= q->addr + q->size || q->w != iw) continue;
        uint32_t off2 = in - q->addr, trow = (uint32_t)q->w * 8 * 4;
        if (off2 % trow) continue;
        sy0 = (int)(off2 / trow) * 8; s = q;
    }
    if (!s || ow > s->w || sy0 + oh > s->h) { ctx_leave(c); return 0; }
    if (g_pend_n) pend_poll(0);
    uint32_t len = (uint32_t)ow * oh * (out_fmt == 0 ? 4 : out_fmt == 1 ? 3 : 2);
    int hd = s->sc > 1, mem = !off && !any_overlap(out, len, 0);
    if (!hd && !mem) { ctx_leave(c); return 0; }
    if (mem) pend_resolve_range(out, len);
    if (g_pend_n == NPEND) pend_poll(1);
    Pend *p = &g_pend[(g_pend_head + g_pend_n) % NPEND];
    memset(p, 0, sizeof *p);
    int osc = s->sc;                                      /* escala da imagem que volta: a da janela, se for menor */
    if (hd && g_blit_lin) { int need = oh == 400 ? g_hd_need_top : g_hd_need_bot; if (need > 0 && need < osc) osc = need; }   /* (1 = tela escondida ou pequena) */
    p->out = out; p->len = len; p->w = ow; p->h = oh; p->sc = osc; p->out_fmt = out_fmt; p->hd = hd; p->mem = mem;
    int W = ow * osc, H = oh * osc;
    if (!rb_get(&p->buf, (VkDeviceSize)W * H * 4)) { ctx_leave(c); return 0; }
    rp_end();
    VkBufferImageCopy r; memset(&r, 0, sizeof r);
    r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; r.imageSubresource.layerCount = 1;
    r.imageOffset.y = (s->h - sy0 - oh) * s->sc;          /* (linha 0 da imagem = linha de baixo da tela, como no OpenGL) */
    r.imageExtent.width = (uint32_t)W; r.imageExtent.height = (uint32_t)H; r.imageExtent.depth = 1;
    clr_flush(s);
    VkImage from = s->im.img;
    cur(); ts_mark(Et_OUTRO);
    if (osc < s->sc) {
        Img *red = reduzir(s->im.img, 0, r.imageOffset.y, ow * s->sc, oh * s->sc, W, H);
        if (red) { from = red->img; r.imageOffset.y = 0; }
        else { osc = p->sc = s->sc; W = ow * osc; H = oh * osc; r.imageExtent.width = (uint32_t)W; r.imageExtent.height = (uint32_t)H;
               rb_put(&p->buf); if (!rb_get(&p->buf, (VkDeviceSize)W * H * 4)) { ctx_leave(c); return 0; } }
    }
    vkCmdCopyImageToBuffer(cur()->cb, from, VK_IMAGE_LAYOUT_GENERAL, p->buf.b, 1, &r);
    barrier(); ts_mark(Et_COPIA);
    p->serial = batch_submit();
    { static int lg = -1; if (lg < 0) lg = ENVC("VK_LOG") != NULL;
      if (lg) fprintf(stderr, "[vk] transf lista %lu: %08x %dx%d (sup %08x %dx%d sy0 %d) -> %08x %dx%d fmt %d hd %d mem %d\n", (unsigned long)g_list_serial, in, iw, ih, s->addr, s->w, s->h, sy0, out, ow, oh, out_fmt, hd, mem); }
    if (out) pica_pout_add(out, +1);
    g_pend_n++;
    ctx_leave(c);
    return (hd ? 2 : 0) | (mem ? 1 : 0);
}

void pica_vk_debug_dump(void) {             /* (cão de guarda: sem travas, só leitura) */
    fprintf(stderr, "[watchdog] vulkan: em lista %d, trava %d, cópias pendentes %d, lote atual %d (gravando %d), séries %lu/%lu\n",
            g_in_list, g_lock_depth, g_pend_n, g_cur, g_b[g_cur].recording, g_serial_done, g_serial_next);
    for (int i = 0; i < NBATCH; i++) fprintf(stderr, "[watchdog]   lote %d: série %lu, enviado %d, gravando %d\n", i, g_b[i].serial, g_b[i].pending, g_b[i].recording);
}
