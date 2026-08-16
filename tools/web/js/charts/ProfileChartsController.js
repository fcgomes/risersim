/**
 * ProfileChartsController.js
 * OO controller for interactive 2D engineering profile charts (Plotly.js).
 */
export class ProfileChartsController {
    /**
     * @param {string} tensionChartDivId
     * @param {string} momentCurvChartDivId
     * @param {string} vonMisesChartDivId
     */
    constructor(tensionChartDivId = 'tension-chart', momentCurvChartDivId = 'moment-curv-chart', vonMisesChartDivId = 'vonmises-chart') {
        this.tensionDiv = tensionChartDivId;
        this.momentCurvDiv = momentCurvChartDivId;
        this.vonMisesDiv = vonMisesChartDivId;
        this.isInitialized = false;
    }

    /**
     * Updates all 3 profile charts for the active step.
     * @param {SimulationStep} step
     * @param {'dark'|'light'} currentTheme
     * @param {{tension: {min:number[],max:number[]}, moment: {min:number[],max:number[]}, vonmises: {min:number[],max:number[]}}|null} [envelope]
     *   Min/max-over-time per element ordinal (see FEASimulation::getElementEnvelope), aligned
     *   index-for-index with this step's own elements -- when present, overlaid as dashed
     *   min/max traces on top of the current step's profile.
     */
    updateCharts(step, currentTheme = 'dark', envelope = null) {
        if (typeof Plotly === 'undefined' || !step || !step.elements || step.elements.length === 0) return;

        // 1. Compute the cumulative arc length s along the line
        const arcLengths = [];
        let cumulativeS = 0;
        arcLengths.push(0);

        for (let i = 0; i < step.elements.length; ++i) {
            const n1 = step.nodes[i];
            const n2 = step.nodes[i + 1] || n1;
            const dx = n2.x - n1.x;
            const dy = n2.y - n1.y;
            const dz = n2.z - n1.z;
            const len = Math.sqrt(dx * dx + dy * dy + dz * dz);
            cumulativeS += len;
            arcLengths.push(cumulativeS);
        }

        // Points at element centers
        const elementS = [];
        for (let i = 0; i < step.elements.length; ++i) {
            elementS.push(0.5 * (arcLengths[i] + arcLengths[i + 1]));
        }

        const tensions = step.elements.map(e => e.tensionEffectiveKn !== undefined ? e.tensionEffectiveKn : (e.tension_effective_kN || 0));
        const moments = step.elements.map(e => e.bendingMomentKnm !== undefined ? e.bendingMomentKnm : (e.bending_moment_kNm || 0));
        const curvatures = step.elements.map(e => e.curvature !== undefined ? e.curvature : 0);
        const vonMises = step.elements.map(e => e.vonMisesMpa !== undefined ? e.vonMisesMpa : (e.von_mises_MPa || 0));
        const mbrSF = step.elements.map(e => e.mbrSafetyFactor !== undefined ? e.mbrSafetyFactor : (e.mbr_safety_factor || 1.0));

        // Chart chrome (background/text/gridlines) follows the page theme -- kept in sync by hand
        // here because Plotly reads plain color strings, not CSS custom properties (see
        // css/shared.css's --bg/--text/--border tokens, same values). The data-series colors
        // below (tension/moment/curvature/von Mises/MBR line colors) are deliberately NOT tied to
        // the theme -- they're physics quantities, not UI chrome, same reasoning as the results
        // colorbar gradient.
        const bgColor = currentTheme === 'dark' ? '#100f18' : '#f7f6fb';
        const textColor = currentTheme === 'dark' ? '#f1f0f6' : '#17151f';
        const gridColor = currentTheme === 'dark' ? '#2a2a38' : '#e4e1f1';

        // Envelope overlay traces (dashed min/max) -- optional, built once per field when
        // `envelope` is provided. Fixed colors, deliberately NOT derived from the main series'
        // own color (as an earlier version did): the envelope's min (or max) can legitimately
        // equal the currently-displayed step's own value (e.g. in dynamic mode, right after
        // switching modes the visible step is often exactly where the historical minimum occurs)
        // -- same-color traces drawn on top of each other in that case become visually
        // indistinguishable from the main curve, making the overlay look "missing" even though the
        // data is correct. Magenta/cyan are picked to stay distinct from every main-series color
        // used across the 3 charts (tension blue, moment orange, curvature green, von Mises red).
        const ENVELOPE_MAX_COLOR = '#ec4899';
        const ENVELOPE_MIN_COLOR = '#06b6d4';
        const envelopeTraces = (fieldEnvelope, label) => {
            if (!fieldEnvelope) return [];
            return [
                {
                    x: elementS, y: fieldEnvelope.max, type: 'scatter', mode: 'lines',
                    name: `${label} máx. (envoltória)`, line: { color: ENVELOPE_MAX_COLOR, width: 1.5, dash: 'dot' }
                },
                {
                    x: elementS, y: fieldEnvelope.min, type: 'scatter', mode: 'lines',
                    name: `${label} mín. (envoltória)`, line: { color: ENVELOPE_MIN_COLOR, width: 1.5, dash: 'dot' }
                }
            ];
        };

        // Title and legend both pinned to "container" coordinates (the whole chart div, not just
        // the plot area) so their stacking order is predictable -- title nearest the top, legend
        // just below it -- instead of Plotly's default "paper" (plot-area-relative) coordinates,
        // where a legend y > 1 could render ABOVE the title depending on the title's own auto
        // position. margin.t reserves room for both stacked above the plot area.
        const layoutBase = {
            autosize: true,
            margin: { l: 70, r: 70, t: 95, b: 55 },
            paper_bgcolor: bgColor,
            plot_bgcolor: bgColor,
            font: { color: textColor, family: 'Segoe UI, sans-serif', size: 13 },
            xaxis: { title: 'Comprimento da Linha s (m)', gridcolor: gridColor, zerolinecolor: gridColor, font: { size: 13 } },
            yaxis: { gridcolor: gridColor, zerolinecolor: gridColor, font: { size: 13 } },
            legend: { orientation: 'h', x: 0.5, xanchor: 'center', xref: 'container', y: 0.9, yanchor: 'top', yref: 'container' }
        };
        const titleLayout = (text) => ({ text, font: { size: 16, color: textColor }, x: 0.5, xanchor: 'center', y: 0.98, yanchor: 'top', yref: 'container' });

        // --- CHART 1: EFFECTIVE TENSION vs s ---
        const traceTension = {
            x: elementS,
            y: tensions,
            type: 'scatter',
            mode: 'lines+markers',
            name: 'Tração Efetiva (kN)',
            line: { color: '#3b82f6', width: 3 },
            marker: { size: 6, color: '#2563eb' }
        };

        const layoutTension = {
            ...layoutBase,
            title: titleLayout(`<b>Perfil de Tração Efetiva T<sub>eff</sub> (kN) ao Longo do Riser</b>`),
            yaxis: { ...layoutBase.yaxis, title: 'Tração Efetiva (kN)' }
        };

        const tensionTraces = [traceTension, ...envelopeTraces(envelope && envelope.tension, 'Tração')];
        Plotly.react(this.tensionDiv, tensionTraces, layoutTension, { responsive: true, displayModeBar: true });

        // --- CHART 2: BENDING MOMENT & CURVATURE vs s ---
        const traceMoment = {
            x: elementS,
            y: moments,
            type: 'scatter',
            mode: 'lines+markers',
            name: 'Momento Fletor (kN.m)',
            line: { color: '#f59e0b', width: 3 },
            marker: { size: 5 }
        };

        const traceCurvature = {
            x: elementS,
            y: curvatures,
            type: 'scatter',
            mode: 'lines+markers',
            name: 'Curvatura Geometrica (1/m)',
            yaxis: 'y2',
            line: { color: '#10b981', width: 2.5, dash: 'dot' },
            marker: { size: 5 }
        };

        const layoutMomentCurv = {
            ...layoutBase,
            title: titleLayout(`<b>Distribuição de Momento Fletor (kN.m) & Curvatura 3D (1/m)</b>`),
            yaxis: { ...layoutBase.yaxis, title: 'Momento Fletor (kN.m)' },
            yaxis2: {
                title: 'Curvatura (1/m)',
                overlaying: 'y',
                side: 'right',
                gridcolor: gridColor,
                font: { color: '#10b981', size: 13 }
            }
        };

        // Envelope overlay applies only to moment (primary y-axis) -- curvature has no field key
        // in FEASimulation::getElementEnvelope's scalar set here, so it's left off the overlay.
        const momentTraces = [traceMoment, traceCurvature, ...envelopeTraces(envelope && envelope.moment, 'Momento')];
        Plotly.react(this.momentCurvDiv, momentTraces, layoutMomentCurv, { responsive: true, displayModeBar: true });

        // --- CHART 3: VON MISES STRESS vs s ---
        const traceVonMises = {
            x: elementS,
            y: vonMises,
            type: 'scatter',
            mode: 'lines+markers',
            name: 'Tensão de von Mises (MPa)',
            line: { color: '#ef4444', width: 3 },
            marker: { size: 6 }
        };

        // 350 MPa matches the solver's real default (CorotationalBeam3D::compute_stress_and_curvature,
        // yield_stress_MPa=350.0 in element_beam.hpp) used to normalize the MBR safety factor --
        // it's not a value exported per model (the exporter doesn't record the yield stress used),
        // so this is an assumption aligned with the code's default, not real data from this simulation.
        const yieldStressMPa = 350;
        const traceYield = {
            x: [0, Math.max(...elementS)],
            y: [yieldStressMPa, yieldStressMPa],
            type: 'scatter',
            mode: 'lines',
            name: `Limite de Escoamento σ<sub>yield</sub> (${yieldStressMPa} MPa, default do solver)`,
            line: { color: '#dc2626', width: 2, dash: 'dash' }
        };

        const layoutVonMises = {
            ...layoutBase,
            title: titleLayout(`<b>Envelope de Tensão Combinada de von Mises σ<sub>vm</sub> (MPa)</b>`),
            yaxis: { ...layoutBase.yaxis, title: 'von Mises (MPa)' }
        };

        const vonMisesTraces = [traceVonMises, traceYield, ...envelopeTraces(envelope && envelope.vonmises, 'von Mises')];
        Plotly.react(this.vonMisesDiv, vonMisesTraces, layoutVonMises, { responsive: true, displayModeBar: true });
    }

    resizeCharts() {
        if (typeof Plotly === 'undefined') return;
        [this.tensionDiv, this.momentCurvDiv, this.vonMisesDiv].forEach(divId => {
            const el = document.getElementById(divId);
            if (el && el.style.display !== 'none') {
                Plotly.Plots.resize(el);
            }
        });
    }
}
