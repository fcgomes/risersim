#ifndef RISERSIM_STATIC_ANALYSIS_HPP
#define RISERSIM_STATIC_ANALYSIS_HPP

#include "risersim/analysis.hpp"
#include "risersim/vessel_offset.hpp"

namespace risersim {

// Controla quando a rigidez artificial (regularização de Tikhonov, ver
// StaticIntegrator) fica ativa dentro de solve_catenary_static:
//   OnlyFirstStep — comportamento histórico do risersim: só no passo 1.
//   EveryStep     — usada na fase "assembly" (Passo 3 do roadmap de
//                   modernização): decai a cada iteração dentro de CADA
//                   passo, dando à malha uma rede de segurança em todo o
//                   carregamento, não só no primeiro incremento.
//   Never         — usada na fase "static" (real, limpa), igual ao ANFLEX
//                   real (m_have_artificial_stiffness=false).
enum class ArtificialStiffnessMode {
    OnlyFirstStep,
    EveryStep,
    Never
};

class StaticAnalysis : public Analysis {
public:
    int load_steps;
    int max_iter_per_step;
    double tol;
    VesselOffset offset;
    bool enable_offset;

    StaticAnalysis()
        : Analysis(),
          load_steps(20),
          max_iter_per_step(300),
          tol(100.0),
          offset(OffsetMode::Far, 0.0),
          enable_offset(false) {}

    bool solve_catenary_static(int steps = 20, int max_iter = 300, double tolerance = 100.0,
                                ArtificialStiffnessMode artif_mode = ArtificialStiffnessMode::OnlyFirstStep);
    bool solve_vessel_offset(const VesselOffset& vessel_offset, int steps = 20, int max_iter = 300, double tolerance = 100.0);

    // Uniform polymorphic solve implementation
    bool solve() override;
};

} // namespace risersim

#endif
