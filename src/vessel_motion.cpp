/**
 * @file vessel_motion.cpp
 * @brief VesselMotion: real RAO + JONSWAP "equivalent harmonic" top-motion computation.
 *
 * Algorithm and formulas verified line-by-line against the real ANFLEX source
 * (`trunk/libs/anf_movements`): `jonswap_spectrum.cpp` (spectrum), `rao_table.cpp`/`rao.cpp`
 * (RAO interpolation + rigid-body transfer), `spectrum.cpp`/`hybrid_movement.cpp` (spectral
 * moments + Rayleigh narrow-band extremes), `equivalent_harmonic.cpp` (equivalent frequency +
 * per-DOF signal). See docs/mapa_aml_exemplos_e_web_interface.md for the full derivation.
 */
#include "risersim/vessel_motion.hpp"
#include <algorithm>
#include <cmath>

namespace risersim {

namespace {

double deg2rad(double deg) { return deg * std::numbers::pi / 180.0; }

/// Heading-interpolates the raw RAO table (linear, no wraparound needed -- `headings_deg` spans
/// a full 0-360 circle) at every native frequency point, for all 6 DOFs, into real/imaginary
/// components (amp*cos(phase), amp*sin(phase)) -- same representation `rao_table.cpp` uses
/// internally, chosen so phase wraparound never produces a spurious jump.
void interpolate_heading(const VesselMotionConfig& cfg, double heading_deg,
                          std::array<std::vector<double>, 6>& re, std::array<std::vector<double>, 6>& im) {
    const auto& headings = cfg.headings_deg;
    size_t n_h = headings.size();
    size_t n_f = cfg.frequencies_rad_s.size();

    size_t h0 = 0, h1 = (n_h > 1) ? 1 : 0;
    double frac = 0.0;
    if (n_h > 1) {
        if (heading_deg <= headings.front()) {
            h0 = h1 = 0;
        } else if (heading_deg >= headings.back()) {
            h0 = h1 = n_h - 1;
        } else {
            for (size_t i = 1; i < n_h; ++i) {
                if (heading_deg <= headings[i]) {
                    h0 = i - 1;
                    h1 = i;
                    double span = headings[h1] - headings[h0];
                    frac = (span > 0.0) ? (heading_deg - headings[h0]) / span : 0.0;
                    break;
                }
            }
        }
    }

    for (int dof = 0; dof < 6; ++dof) {
        re[dof].resize(n_f);
        im[dof].resize(n_f);
        // Rotational DOFs (roll/pitch/yaw, dof>=3) store amplitude as deg/m in the raw RAO table
        // (same convention as the phase columns), same as every other reader of this file format
        // (real ANFLEX's own `model_builder_dat.cpp:242-250`: "Converte as amplitudes dos graus
        // de liberdade de rotacao para radiano"). Translational DOFs (surge/sway/heave) are
        // already m/m, no conversion. Without this, the rotational amplitude feeding the
        // point-transfer and spectral-moment math below is ~57x too large (deg instead of rad),
        // which -- squared inside the spectral moments -- inflated the "equivalent harmonic"
        // amplitude by up to ~150x for cases where the wave spectrum peak coincides with a roll
        // RAO peak (see docs/mapa_aml_exemplos_e_web_interface.md, Far load case investigation).
        double amp_scale = (dof >= 3) ? std::numbers::pi / 180.0 : 1.0;
        const auto& amp0 = cfg.amplitude[dof][h0];
        const auto& amp1 = cfg.amplitude[dof][h1];
        const auto& ph0 = cfg.phase_deg[dof][h0];
        const auto& ph1 = cfg.phase_deg[dof][h1];
        for (size_t j = 0; j < n_f; ++j) {
            double a0 = amp0[j] * amp_scale;
            double a1 = amp1[j] * amp_scale;
            double re0 = a0 * std::cos(deg2rad(ph0[j]));
            double im0 = a0 * std::sin(deg2rad(ph0[j]));
            double re1 = a1 * std::cos(deg2rad(ph1[j]));
            double im1 = a1 * std::sin(deg2rad(ph1[j]));
            re[dof][j] = re0 + frac * (re1 - re0);
            im[dof][j] = im0 + frac * (im1 - im0);
        }
    }
}

/// Rigid-body linearized transfer of the (already heading-selected) RAO curves from the vessel
/// CM to a local point `(x,y,z)` -- mirrors `rao_table.cpp::transfer_local()`. Only the 3
/// translational DOFs change; rotations are the same everywhere on a rigid body.
void transfer_to_point(double x, double y, double z, std::array<std::vector<double>, 6>& re,
                        std::array<std::vector<double>, 6>& im) {
    size_t n_f = re[0].size();
    std::vector<double> re_t[3], im_t[3];
    for (int k = 0; k < 3; ++k) {
        re_t[k].resize(n_f);
        im_t[k].resize(n_f);
    }
    for (size_t j = 0; j < n_f; ++j) {
        // Indices: 0=surge,1=sway,2=heave,3=roll,4=pitch,5=yaw.
        re_t[0][j] = re[0][j] - y * re[5][j] + z * re[4][j];
        re_t[1][j] = re[1][j] + x * re[5][j] - z * re[3][j];
        re_t[2][j] = re[2][j] - x * re[4][j] + y * re[3][j];
        im_t[0][j] = im[0][j] - y * im[5][j] + z * im[4][j];
        im_t[1][j] = im[1][j] + x * im[5][j] - z * im[3][j];
        im_t[2][j] = im[2][j] - x * im[4][j] + y * im[3][j];
    }
    for (int k = 0; k < 3; ++k) {
        re[k] = re_t[k];
        im[k] = im_t[k];
    }
}

/// Interpolates the (already heading-selected, already transferred) RAO curve for one DOF at an
/// arbitrary frequency, via its real/imaginary components -- returns (amplitude, phase_rad).
std::pair<double, double> curve_at(const std::vector<double>& freqs, const std::vector<double>& re,
                                    const std::vector<double>& im, double omega) {
    double r = VesselMotion::interp_linear(freqs, re, omega);
    double i = VesselMotion::interp_linear(freqs, im, omega);
    return {std::sqrt(r * r + i * i), std::atan2(i, r)};
}

} // namespace

double VesselMotion::interp_linear(const std::vector<double>& x, const std::vector<double>& y, double xq) {
    const size_t n = x.size();
    if (n == 0) return 0.0;
    if (n == 1 || xq <= x.front()) return y.front();
    if (xq >= x.back()) return y.back();
    for (size_t i = 1; i < n; ++i) {
        if (xq <= x[i]) {
            double t = (x[i] > x[i - 1]) ? (xq - x[i - 1]) / (x[i] - x[i - 1]) : 0.0;
            return y[i - 1] + t * (y[i] - y[i - 1]);
        }
    }
    return y.back();
}

double VesselMotion::jonswap_spectrum(double omega, double alpha, double gamma, double period_s) {
    if (omega <= 0.0 || period_s <= 0.0) return 0.0;
    constexpr double g = 9.81;
    double wp = 2.0 * std::numbers::pi / period_s;
    double tau = (omega <= wp) ? 0.07 : 0.09;
    double aux1 = alpha * g * g / std::pow(omega, 5.0);
    double aux2 = -1.25 * std::pow(wp / omega, 4.0);
    if (aux2 <= -40.0) return 0.0;
    double var1 = ((omega - wp) * (omega - wp)) / (2.0 * tau * tau * wp * wp);
    double var2 = (var1 >= 100.0) ? 1.0 : std::pow(gamma, std::exp(-var1));
    return aux1 * std::exp(aux2) * var2;
}

VesselMotion::VesselMotion(const VesselMotionConfig& config, double wave_heading_deg, double storm_duration_s,
                            const Eigen::Vector3d& attachment_point_global) {
    refsys_angle_rad_ = deg2rad(config.refsys_angle_deg);

    if (config.headings_deg.empty() || config.frequencies_rad_s.empty()) {
        return; // Sem tabela RAO real -- omega_eq_/amplitude_/phase_rad_ ficam nos defaults (zero).
    }

    // Direção relativa onda-flutuante, "positivada" pra dentro de [0,360) -- mesma convenção de
    // hybrid_movement.cpp (`rao_dir = wave_angle - floating_angle`); a tabela real já cobre o
    // círculo completo, então não precisa de wraparound na interpolação em si, só aqui na
    // entrada.
    double rao_dir_deg = std::fmod(wave_heading_deg - config.refsys_angle_deg, 360.0);
    if (rao_dir_deg < 0.0) rao_dir_deg += 360.0;

    std::array<std::vector<double>, 6> re, im;
    interpolate_heading(config, rao_dir_deg, re, im);

    // Transferência geométrica CM->ponto de fixação, confirmada linha a linha contra o ANFLEX
    // real (`model_builder_dat.cpp:4373-4386`, `anf_movements.cpp:66-84`, `save-dat.cpp`):
    // cm_position (posição global do CM) = movement_center + offset (somados -- `offset` é a
    // mesma grandeza que o loader real chama de `static_offset`); a posição local do ponto de
    // fixação, alimentada em `transfer_local`, é `(posição global real do nó pós-estática) -
    // cm_position`, rotacionada pela inversa de `refsys_angle` (`cMatrixTransform::inv_transform`
    // = R_z(-theta), verificado em matrix_transform.cpp). Ver VesselMotionConfig::offset_m e
    // docs/mapa_aml_exemplos_e_web_interface.md para o histórico da investigação (uma versão
    // anterior deste módulo interpretou `offset` como já sendo esse delta local pronto, o que
    // subestimava o braço de alavanca e levou a desligar a transferência temporariamente antes
    // dessa confirmação).
    Eigen::Vector3d cm_global(config.cm_position_m[0] + config.offset_m[0],
                               config.cm_position_m[1] + config.offset_m[1],
                               config.cm_position_m[2] + config.offset_m[2]);
    Eigen::Vector3d delta = attachment_point_global - cm_global;
    double cz = std::cos(refsys_angle_rad_), sz = std::sin(refsys_angle_rad_);
    double x_local = cz * delta.x() + sz * delta.y();
    double y_local = -sz * delta.x() + cz * delta.y();
    double z_local = delta.z();

    transfer_to_point(x_local, y_local, z_local, re, im);

    // Espectro JONSWAP nos pontos reais de discretização (linspace(wini,wfin,nwave), igual ao
    // grid de componentes de onda real -- irregular_wave.cpp) e momentos espectrais por GDL
    // (m0, m2, m4) via trapézio -- mesma fórmula de spectrum.cpp::compute_spectrum_moments,
    // exceto que integramos exatamente sobre [wini,wfin] (n-1 trapézios pra n pontos) em vez do
    // grid ligeiramente mais curto que o código real usa (um artefato de `dw=(wf-wi)/n` com `n`
    // = contagem de pontos, não de intervalos, ali) -- diferença desprezível (~1% pra n=100),
    // documentada em vez de replicada bit-a-bit.
    int nwave = std::max(2, config.jonswap_nwave);
    std::vector<double> w(nwave);
    for (int i = 0; i < nwave; ++i) {
        w[i] = config.jonswap_wini_rad_s + (config.jonswap_wfin_rad_s - config.jonswap_wini_rad_s) * i / (nwave - 1);
    }

    std::array<double, 6> amp_max{}, acel_max{};
    for (int dof = 0; dof < 6; ++dof) {
        double m0 = 0.0, m2 = 0.0, m4 = 0.0;
        double s_prev = 0.0;
        for (int i = 0; i < nwave; ++i) {
            double amp = curve_at(config.frequencies_rad_s, re[dof], im[dof], w[i]).first;
            double s_jonswap = jonswap_spectrum(w[i], config.jonswap_alpha, config.jonswap_gamma, config.jonswap_period_s);
            double s_dof = amp * amp * s_jonswap;
            if (i > 0) {
                double dw = w[i] - w[i - 1];
                double a = 0.5 * (s_prev + s_dof) * dw;
                double wm = 0.5 * (w[i - 1] + w[i]);
                m0 += a;
                m2 += a * wm * wm;
                m4 += a * wm * wm * wm * wm;
            }
            s_prev = s_dof;
        }

        double amp_std = std::sqrt(std::max(0.0, m0));
        double acel_std = std::sqrt(std::max(0.0, m4));
        double amp_sig = 2.0 * amp_std;
        double acel_sig = 2.0 * acel_std;

        double rayleigh_factor = 0.0;
        if (m4 > 0.0) {
            double Tm = 2.0 * std::numbers::pi * std::sqrt(m2 / m4);
            double n_cycles = (Tm > 0.0) ? (storm_duration_s / Tm) : 0.0;
            // N de ciclos precisa ser >1 pro fator de Rayleigh (log) fazer sentido -- sempre
            // verdade com a duração real de tormenta (10800s, ver doc de storm_duration_s em
            // vessel_motion.hpp), guarda aqui só por robustez (ex. espectro induzido quase nulo).
            if (n_cycles > 1.0) rayleigh_factor = std::sqrt(0.5 * std::log(n_cycles));
        }

        amp_max[dof] = amp_sig * rayleigh_factor;
        acel_max[dof] = acel_sig * rayleigh_factor;
    }

    int max_dof = static_cast<int>(config.maximization_dof);
    omega_eq_ = (amp_max[max_dof] > 0.0) ? std::sqrt(std::max(0.0, acel_max[max_dof] / amp_max[max_dof])) : 0.0;

    for (int dof = 0; dof < 6; ++dof) {
        amplitude_[dof] = amp_max[dof];
        phase_rad_[dof] = curve_at(config.frequencies_rad_s, re[dof], im[dof], omega_eq_).second;
    }
}

void VesselMotion::get_motion(double time, Eigen::Vector3d& disp, Eigen::Vector3d& rot) const {
    double ramp_factor = (time < ramp_time_s_) ? 0.5 * (1.0 - std::cos(std::numbers::pi * time / ramp_time_s_)) : 1.0;

    std::array<double, 6> local{};
    for (int dof = 0; dof < 6; ++dof) {
        local[dof] = ramp_factor * amplitude_[dof] * std::sin(omega_eq_ * time + phase_rad_[dof]);
    }

    // Gira do referencial local do flutuante (mesmo eixo da tabela RAO) pro global -- rotação
    // direta de refsys_angle, espelhando hybrid_movement.cpp::get_movement()
    // (`mt.set_z_matrix(m_ref_sys_angle); mt.transform(...)`), inversa da rotação usada no
    // construtor pra levar o offset do ponto pro referencial local.
    double c = std::cos(refsys_angle_rad_), s = std::sin(refsys_angle_rad_);
    disp.x() = local[0] * c - local[1] * s;
    disp.y() = local[0] * s + local[1] * c;
    disp.z() = local[2];
    rot.x() = local[3] * c - local[4] * s;
    rot.y() = local[3] * s + local[4] * c;
    rot.z() = local[5];
}

} // namespace risersim
