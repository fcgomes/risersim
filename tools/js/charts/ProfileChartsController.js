/**
 * ProfileChartsController.js
 * Controlador OO de gráficos 2D interativos de perfil de engenharia (Plotly.js).
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
     * Atualiza todos os 3 gráficos de perfil para o passo ativo
     * @param {SimulationStep} step 
     * @param {'dark'|'light'} currentTheme 
     */
    updateCharts(step, currentTheme = 'dark') {
        if (typeof Plotly === 'undefined' || !step || !step.elements || step.elements.length === 0) return;

        // 1. Calcula o comprimento acumulado do arco s (arc-length s) ao longo da linha
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

        // Pontos nos centros dos elementos
        const elementS = [];
        for (let i = 0; i < step.elements.length; ++i) {
            elementS.push(0.5 * (arcLengths[i] + arcLengths[i + 1]));
        }

        const tensions = step.elements.map(e => e.tensionEffectiveKn !== undefined ? e.tensionEffectiveKn : (e.tension_effective_kN || 0));
        const moments = step.elements.map(e => e.bendingMomentKnm !== undefined ? e.bendingMomentKnm : (e.bending_moment_kNm || 0));
        const curvatures = step.elements.map(e => e.curvature !== undefined ? e.curvature : 0);
        const vonMises = step.elements.map(e => e.vonMisesMpa !== undefined ? e.vonMisesMpa : (e.von_mises_MPa || 0));
        const mbrSF = step.elements.map(e => e.mbrSafetyFactor !== undefined ? e.mbrSafetyFactor : (e.mbr_safety_factor || 1.0));

        const bgColor = currentTheme === 'dark' ? '#11111b' : '#ffffff';
        const textColor = currentTheme === 'dark' ? '#cdd6f4' : '#0f172a';
        const gridColor = currentTheme === 'dark' ? '#313244' : '#e2e8f0';

        const layoutBase = {
            autosize: true,
            margin: { l: 70, r: 70, t: 55, b: 55 },
            paper_bgcolor: bgColor,
            plot_bgcolor: bgColor,
            font: { color: textColor, family: 'Segoe UI, sans-serif', size: 13 },
            xaxis: { title: 'Comprimento da Linha s (m)', gridcolor: gridColor, zerolinecolor: gridColor, font: { size: 13 } },
            yaxis: { gridcolor: gridColor, zerolinecolor: gridColor, font: { size: 13 } }
        };

        // --- GRÁFICO 1: TRAÇÃO EFETIVA vs s ---
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
            title: { text: `<b>Perfil de Tração Efetiva T<sub>eff</sub> (kN) ao Longo do Riser</b>`, font: { size: 16, color: textColor } },
            yaxis: { ...layoutBase.yaxis, title: 'Tração Efetiva (kN)' }
        };

        Plotly.react(this.tensionDiv, [traceTension], layoutTension, { responsive: true, displayModeBar: true });

        // --- GRÁFICO 2: MOMENTO FLETOR & CURVATURA vs s ---
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
            title: { text: `<b>Distribuição de Momento Fletor (kN.m) & Curvatura 3D (1/m)</b>`, font: { size: 16, color: textColor } },
            yaxis: { ...layoutBase.yaxis, title: 'Momento Fletor (kN.m)' },
            yaxis2: {
                title: 'Curvatura (1/m)',
                overlaying: 'y',
                side: 'right',
                gridcolor: gridColor,
                font: { color: '#10b981', size: 13 }
            },
            legend: { orientation: 'h', x: 0.2, y: 1.12 }
        };

        Plotly.react(this.momentCurvDiv, [traceMoment, traceCurvature], layoutMomentCurv, { responsive: true, displayModeBar: true });

        // --- GRÁFICO 3: TENSÃO VON MISES vs s ---
        const traceVonMises = {
            x: elementS,
            y: vonMises,
            type: 'scatter',
            mode: 'lines+markers',
            name: 'Tensão de von Mises (MPa)',
            line: { color: '#ef4444', width: 3 },
            marker: { size: 6 }
        };

        // 350 MPa casa com o default real do solver (CorotationalBeam3D::compute_stress_and_curvature,
        // yield_stress_MPa=350.0 em element_beam.hpp) usado para normalizar o fator de segurança MBR --
        // não é um valor exportado por modelo (o exportador não grava a tensão de escoamento usada),
        // então isso é uma suposição alinhada ao default do código, não um dado real desta simulação.
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
            title: { text: `<b>Envelope de Tensão Combinada de von Mises σ<sub>vm</sub> (MPa)</b>`, font: { size: 16, color: textColor } },
            yaxis: { ...layoutBase.yaxis, title: 'von Mises (MPa)' },
            legend: { orientation: 'h', x: 0.2, y: 1.12 }
        };

        Plotly.react(this.vonMisesDiv, [traceVonMises, traceYield], layoutVonMises, { responsive: true, displayModeBar: true });
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
