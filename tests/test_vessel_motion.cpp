/**
 * @file test_vessel_motion.cpp
 * @brief Validates VesselMotion's JONSWAP spectrum, linear interpolation, and the equivalent-
 * harmonic frequency formula against hand/independently-computed reference values -- see
 * docs/mapa_aml_exemplos_e_web_interface.md for the full derivation each formula mirrors from
 * the real ANFLEX source (trunk/libs/anf_movements).
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "risersim/vessel_motion.hpp"
#include <cmath>

using namespace risersim;
using Catch::Approx;

TEST_CASE("VesselMotion::jonswap_spectrum matches a hand-computed reference", "[vessel_motion]") {
    // Parâmetros reais do Exemplo_01a (Waves/ON no XML).
    const double alpha = 0.015903;
    const double gamma = 2.066236;
    const double period_s = 10.0;
    const double wp = 2.0 * std::numbers::pi / period_s;

    // No pico (w=wp): tau=0.07, var1=0, var2=gamma^1=gamma, aux2=-1.25 -- valor calculado à mão
    // (e conferido em Python com a mesma fórmula) = 9.251870676179982.
    REQUIRE(VesselMotion::jonswap_spectrum(wp, alpha, gamma, period_s) == Approx(9.251870676179982).epsilon(1e-9));

    // Longe do pico, valor pequeno mas positivo (calda de baixa frequência) -- só confere ordem
    // de grandeza/sinal, não um valor exato.
    REQUIRE(VesselMotion::jonswap_spectrum(0.3, alpha, gamma, period_s) < 1.0e-6);
    REQUIRE(VesselMotion::jonswap_spectrum(0.3, alpha, gamma, period_s) > 0.0);

    REQUIRE(VesselMotion::jonswap_spectrum(1.0, alpha, gamma, period_s) == Approx(1.2595302790833383).epsilon(1e-9));

    // Frequência inválida -- retorna 0, não NaN/negativo.
    REQUIRE(VesselMotion::jonswap_spectrum(0.0, alpha, gamma, period_s) == 0.0);
    REQUIRE(VesselMotion::jonswap_spectrum(-1.0, alpha, gamma, period_s) == 0.0);
}

TEST_CASE("VesselMotion::interp_linear clamps at the edges and interpolates in between", "[vessel_motion]") {
    std::vector<double> x = {1.0, 2.0, 4.0};
    std::vector<double> y = {10.0, 20.0, 40.0};

    REQUIRE(VesselMotion::interp_linear(x, y, 0.5) == Approx(10.0)); // abaixo do início -- clampa
    REQUIRE(VesselMotion::interp_linear(x, y, 5.0) == Approx(40.0)); // acima do fim -- clampa
    REQUIRE(VesselMotion::interp_linear(x, y, 1.0) == Approx(10.0));
    REQUIRE(VesselMotion::interp_linear(x, y, 3.0) == Approx(30.0)); // meio do segundo trecho
}

namespace {

/// Monta uma VesselMotionConfig sintética com RAO=1 (fase 0) só no heave, em todas as
/// frequências/headings, e RAO=0 nos outros 5 GDL -- isola a integração espectral/estatística de
/// Rayleigh de qualquer efeito de interpolação de heading ou transferência geométrica (que fica
/// zerada de propósito: mesma posição do CM e do ponto de fixação).
VesselMotionConfig build_synthetic_config(double wini, double wfin, int nwave, double alpha, double gamma, double period_s,
                                           VesselDof dof_with_rao = VesselDof::Heave) {
    VesselMotionConfig cfg;
    cfg.enabled = true;
    cfg.cm_position_m = {0.0, 0.0, 0.0};
    cfg.refsys_angle_deg = 0.0;
    cfg.maximization_dof = VesselDof::Heave;
    cfg.headings_deg = {0.0, 180.0};

    const int n_freq = 50;
    cfg.frequencies_rad_s.resize(n_freq);
    for (int i = 0; i < n_freq; ++i) {
        cfg.frequencies_rad_s[i] = wini + (wfin - wini) * i / (n_freq - 1);
    }

    for (int dof = 0; dof < 6; ++dof) {
        cfg.amplitude[dof].assign(2, std::vector<double>(n_freq, dof == static_cast<int>(dof_with_rao) ? 1.0 : 0.0));
        cfg.phase_deg[dof].assign(2, std::vector<double>(n_freq, 0.0));
    }

    cfg.jonswap_alpha = alpha;
    cfg.jonswap_gamma = gamma;
    cfg.jonswap_period_s = period_s;
    cfg.jonswap_wini_rad_s = wini;
    cfg.jonswap_wfin_rad_s = wfin;
    cfg.jonswap_nwave = nwave;
    return cfg;
}

} // namespace

TEST_CASE("VesselMotion equivalent frequency matches (m4/m0)^0.25 for a unit RAO", "[vessel_motion]") {
    const double alpha = 0.015903, gamma = 2.066236, period_s = 10.0;
    const double wini = 0.2, wfin = 3.0;
    const int nwave = 100;

    auto cfg = build_synthetic_config(wini, wfin, nwave, alpha, gamma, period_s);

    // Referência independente: com RAO=1 constante, o espectro induzido é o próprio JONSWAP.
    // Integra m0/m4 por trapézio na MESMA grade que VesselMotion usa internamente (linspace de
    // nwave pontos entre wini/wfin), usando a função pública jonswap_spectrum -- não reimplementa
    // a classe, só verifica a propriedade omega_eq=(m4/m0)^0.25 (o fator de Rayleigh cancela na
    // razão acel_max/amp_max, ver vessel_motion.cpp).
    //
    // VesselMotion agora clipa esse laço à faixa realmente tabelada em cfg.frequencies_rad_s (ver
    // vessel_motion.cpp, "mov_ini"/"mov_fin", portado de hybrid_movement.cpp:69-81 -- roadmap.md
    // Eixo 2a Atualização 8). Como aqui frequencies_rad_s vai exatamente até `wfin` (mesmo valor
    // de ponto flutuante do último ponto da grade de onda), a comparação estrita "< rao_max" exclui
    // esse último ponto -- integra um ponto a menos aqui pra bater com o que a classe de fato usa.
    std::vector<double> w(nwave);
    for (int i = 0; i < nwave; ++i) w[i] = wini + (wfin - wini) * i / (nwave - 1);
    double m0 = 0.0, m4 = 0.0;
    for (int i = 1; i < nwave - 1; ++i) {
        double s1 = VesselMotion::jonswap_spectrum(w[i - 1], alpha, gamma, period_s);
        double s2 = VesselMotion::jonswap_spectrum(w[i], alpha, gamma, period_s);
        double dw = w[i] - w[i - 1];
        double a = 0.5 * (s1 + s2) * dw;
        double wm = 0.5 * (w[i - 1] + w[i]);
        m0 += a;
        m4 += a * wm * wm * wm * wm;
    }
    double expected_omega_eq = std::pow(m4 / m0, 0.25);

    // Onda vindo do mesmo heading da tabela (0 graus) e sem rotação de referencial, pra isolar
    // o teste de qualquer interpolação de heading ou transferência geométrica.
    VesselMotion vm(cfg, /*wave_heading_deg=*/0.0, /*storm_duration_s=*/10800.0, Eigen::Vector3d::Zero());

    REQUIRE(vm.frequency_rad_s() == Approx(expected_omega_eq).epsilon(1e-6));
    REQUIRE(vm.amplitude(VesselDof::Heave) > 0.0);
    // Sem rotação, sem RAO nos outros GDL, sem offset -- só o heave deveria ter amplitude real.
    REQUIRE(vm.amplitude(VesselDof::Surge) == Approx(0.0).margin(1e-12));
}

TEST_CASE("VesselMotion's geometric transfer couples pitch into heave with the real lever-arm formula",
          "[vessel_motion]") {
    // Confirmado contra o ANFLEX real (model_builder_dat.cpp/anf_movements.cpp/save-dat.cpp):
    // o offset local usado no transfer_local é (posição real do ponto de fixação) - (CM), não um
    // campo pronto -- ver o comentário no construtor de VesselMotion. Este teste isola essa conta:
    // RAO só em pitch (amplitude 1, fase 0), CM na origem, ponto de fixação em x=-10 -> braço de
    // alavanca x_local=-10 -> heave_transferido = heave - x_local*pitch = 0 - (-10)*1 = 10, ou
    // seja, o heave "induzido" deveria ter exatamente o dobro... o quíntuplo... 10x a amplitude do
    // caso base (RAO=1 direto no heave, sem transferência) -- a resposta espectral/Rayleigh escala
    // linearmente com a amplitude da RAO transferida (ver teste anterior).
    const double alpha = 0.015903, gamma = 2.066236, period_s = 10.0;
    const double wini = 0.2, wfin = 3.0;
    const int nwave = 100;

    auto cfg_baseline = build_synthetic_config(wini, wfin, nwave, alpha, gamma, period_s, VesselDof::Heave);
    VesselMotion vm_baseline(cfg_baseline, 0.0, 10800.0, Eigen::Vector3d::Zero());

    auto cfg_pitch = build_synthetic_config(wini, wfin, nwave, alpha, gamma, period_s, VesselDof::Pitch);
    // VesselMotion now converts rotational-DOF RAO amplitude from deg/m (the raw table's real
    // convention, matching model_builder_dat.cpp:242-250) to rad/m before using it -- rescale the
    // synthetic "1.0" here to 1 rad (=180/pi deg) so it still means exactly "1 rad of pitch" and
    // the hand-derived expected value below (10x) stays exact.
    for (auto& row : cfg_pitch.amplitude[static_cast<int>(VesselDof::Pitch)])
        for (double& v : row) v *= 180.0 / std::numbers::pi;
    VesselMotion vm_transferred(cfg_pitch, 0.0, 10800.0, Eigen::Vector3d(-10.0, 0.0, 0.0));

    REQUIRE(vm_transferred.amplitude(VesselDof::Heave) == Approx(10.0 * vm_baseline.amplitude(VesselDof::Heave)).epsilon(1e-6));

    // Sem transferência (ponto de fixação == CM), a resposta de pitch não vaza pro heave.
    VesselMotion vm_no_transfer(cfg_pitch, 0.0, 10800.0, Eigen::Vector3d::Zero());
    REQUIRE(vm_no_transfer.amplitude(VesselDof::Heave) == Approx(0.0).margin(1e-12));
}

TEST_CASE("VesselMotion falls back to zero motion when the RAO table is empty", "[vessel_motion]") {
    VesselMotionConfig cfg; // default: headings_deg/frequencies_rad_s vazios
    VesselMotion vm(cfg, 0.0, 10800.0, Eigen::Vector3d::Zero());

    REQUIRE(vm.frequency_rad_s() == 0.0);
    Eigen::Vector3d disp, rot;
    vm.get_motion(5.0, disp, rot);
    REQUIRE(disp.norm() == 0.0);
    REQUIRE(rot.norm() == 0.0);
}
