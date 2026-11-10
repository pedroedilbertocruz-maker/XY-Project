// Mixagem de áudio do DSP do 3DS em alto nível (HLE), usando o núcleo de áudio do Azahar
// (audio/audio_core, GPL-2.0-or-later; ver audio/LICENSE.txt). A cada quadro de áudio
// (160 amostras a 32728 Hz) lê as configurações que o jogo escreveu na memória compartilhada do DSP,
// decodifica e mistura as 24 vozes e devolve o estado e as amostras finais.
#include "audio_core/hle/mixers.h"
#include "audio_core/hle/source.h"
#include <array>
#include <cstring>
#include <vector>

extern "C" {
extern uint8_t *g_mem;
}

namespace {
constexpr uint32_t kRegion0 = 0x1FF50000u, kRegion1 = 0x1FF70000u;

// endereço físico (o que o jogo passa ao DSP) -> memória do jogo
const uint8_t *resolve(uint32_t p, size_t n) {
    (void)n;
    if (p >= 0x18000000u && p < 0x18600000u) return g_mem + (p + 0x07000000u);
    if (p >= 0x20000000u && p < 0x28000000u) return g_mem + (p - 0x0C000000u);
    if (p >= 0x1FF00000u && p < 0x1FF80000u) return g_mem + p;
    return nullptr;
}

struct Dsp {
    std::vector<AudioCore::HLE::Source> sources;
    AudioCore::HLE::Mixers mixers;
    Dsp() {
        sources.reserve(AudioCore::HLE::num_sources);
        for (size_t i = 0; i < AudioCore::HLE::num_sources; i++) {
            sources.emplace_back(i);
            sources.back().SetMemoryResolver(resolve);
        }
    }
};
Dsp *g_dsp;

size_t current_region(const AudioCore::HLE::SharedMemory &r0, const AudioCore::HLE::SharedMemory &r1) {
    uint16_t f0 = r0.frame_counter, f1 = r1.frame_counter;
    if (f0 == 0xFFFF && f1 != 0xFFFE) return 1;
    if (f1 == 0xFFFF && f0 != 0xFFFE) return 0;
    return f0 > f1 ? 0 : 1;
}
} // namespace

extern "C" int dsp_hle_frame(int16_t *out) {
    if (!g_dsp) g_dsp = new Dsp();
    static AudioCore::HLE::SharedMemory r0, r1;
    std::memcpy(&r0, g_mem + kRegion0, sizeof r0);
    std::memcpy(&r1, g_mem + kRegion1, sizeof r1);
    size_t ri = current_region(r0, r1);
    auto &read = ri == 0 ? r0 : r1;
    auto &write = ri == 0 ? r1 : r0;
    std::array<AudioCore::QuadFrame32, 3> mixes{};
    for (size_t i = 0; i < g_dsp->sources.size(); i++) {
        write.source_statuses.status[i] =
            g_dsp->sources[i].Tick(read.source_configurations.config[i], read.adpcm_coefficients.coeff[i]);
        for (size_t m = 0; m < mixes.size(); m++) g_dsp->sources[i].MixInto(mixes[m], m);
    }
    write.dsp_status = g_dsp->mixers.Tick(read.dsp_configuration, read.intermediate_mix_samples,
                                          write.intermediate_mix_samples, mixes);
    const auto output = g_dsp->mixers.GetOutput();
    int n = 0;
    for (size_t s = 0; s < output.size(); s++)
        for (size_t ch = 0; ch < output[s].size(); ch++) {
            write.final_samples.pcm16[s][ch] = output[s][ch];
            out[n++] = output[s][ch];
        }
    std::memcpy(g_mem + (ri == 0 ? kRegion0 : kRegion1), &read, sizeof read);
    std::memcpy(g_mem + (ri == 0 ? kRegion1 : kRegion0), &write, sizeof write);
    return n / 2;   // quadros estéreo
}
