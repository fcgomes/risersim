/**
 * TimeHistoryChartController.js
 * OO controller for the "time history" chart (Plotly.js) -- a selected node's or element's
 * value(s) plotted across the whole active-mode time series, mirroring the real ANFLEX's
 * time-history concept (see `anf_analysis/src/post_processor.cpp`: series per user-selected
 * node/element, opt-in per point) -- computed client-side since the full step history is already
 * resident in memory (see FEASimulation.activeSteps).
 */
export class TimeHistoryChartController {
    /**
     * @param {string} chartDivId
     */
    constructor(chartDivId = 'history-chart') {
        this.chartDiv = chartDivId;
    }

    /**
     * @param {import('../models/FEASimulation.js').FEASimulation|null} simulation
     * @param {{type:'node'|'element', id:number}|null} selectedPoint
     * @param {string} scalarField - 'tension' | 'moment' | 'curvature' | 'vonmises' | 'mbr' (only used for element selections)
     * @param {number} currentStepIdx
     * @param {'dark'|'light'} currentTheme
     */
    updateChart(simulation, selectedPoint, scalarField = 'tension', currentStepIdx = 0, currentTheme = 'dark') {
        if (typeof Plotly === 'undefined') return;

        const bgColor = currentTheme === 'dark' ? '#100f18' : '#f7f6fb';
        const textColor = currentTheme === 'dark' ? '#f1f0f6' : '#17151f';
        const gridColor = currentTheme === 'dark' ? '#2a2a38' : '#e4e1f1';

        // Title/legend both pinned to "container" coordinates so title always renders above the
        // legend (see ProfileChartsController for the same fix/reasoning), and margin.t reserves
        // room for both stacked above the plot area.
        const layoutBase = {
            autosize: true,
            margin: { l: 70, r: 30, t: 95, b: 55 },
            paper_bgcolor: bgColor,
            plot_bgcolor: bgColor,
            font: { color: textColor, family: 'Segoe UI, sans-serif', size: 13 }
        };
        const titleLayout = (text) => ({ text, font: { size: 16, color: textColor }, x: 0.5, xanchor: 'center', y: 0.98, yanchor: 'top', yref: 'container' });

        if (!simulation || !selectedPoint) {
            const layout = {
                ...layoutBase,
                title: titleLayout('<b>Histórico no Tempo</b>'),
                xaxis: { visible: false },
                yaxis: { visible: false },
                annotations: [{
                    text: 'Selecione uma linha na aba "📊 Tabela" (Elementos ou Nós) para ver o<br>histórico no tempo deste ponto.',
                    showarrow: false, x: 0.5, y: 0.5, xref: 'paper', yref: 'paper',
                    font: { size: 14, color: textColor }
                }]
            };
            Plotly.react(this.chartDiv, [], layout, { responsive: true, displayModeBar: false });
            return;
        }

        const steps = simulation.activeSteps;
        const isDynamic = simulation.mode === 'dynamic';
        // Same dt/labeling convention already used by app.js::render() for the step label.
        const xVals = steps.map(s => isDynamic ? s.stepIndex * 0.05 : s.loadFactor * 100);
        const xTitle = isDynamic ? 'Tempo (s)' : 'Fator de Carga / Offset (%)';
        const idx = selectedPoint.id - 1; // ids are contiguous 1-indexed ordinals (simulation_exporter.cpp)

        let traces = [];
        let titleText = '';
        let yTitle = '';

        if (selectedPoint.type === 'node') {
            const xs = steps.map(s => (s.nodes[idx] ? s.nodes[idx].x : null));
            const ys = steps.map(s => (s.nodes[idx] ? s.nodes[idx].y : null));
            const zs = steps.map(s => (s.nodes[idx] ? s.nodes[idx].z : null));
            traces = [
                { x: xVals, y: xs, type: 'scatter', mode: 'lines', name: 'X (m)', line: { color: '#3b82f6', width: 2 } },
                { x: xVals, y: ys, type: 'scatter', mode: 'lines', name: 'Y (m)', line: { color: '#10b981', width: 2 } },
                { x: xVals, y: zs, type: 'scatter', mode: 'lines', name: 'Z (m)', line: { color: '#f59e0b', width: 2 } }
            ];
            titleText = `<b>Histórico no Tempo — Posição do Nó ${selectedPoint.id}</b>`;
            yTitle = 'Posição (m)';
        } else {
            const fieldMeta = {
                tension: { label: 'Tração Efetiva (kN)', color: '#3b82f6' },
                moment: { label: 'Momento Fletor (kN·m)', color: '#f59e0b' },
                curvature: { label: 'Curvatura (1/m)', color: '#10b981' },
                vonmises: { label: 'Tensão von Mises (MPa)', color: '#ef4444' },
                mbr: { label: 'Fator de Segurança MBR', color: '#7c6ce0' }
            };
            const meta = fieldMeta[scalarField] || fieldMeta.tension;
            const vals = steps.map(s => (s.elements[idx] ? simulation.getElementScalar(s.elements[idx], scalarField) : null));
            traces = [{ x: xVals, y: vals, type: 'scatter', mode: 'lines', name: meta.label, line: { color: meta.color, width: 2.5 } }];
            titleText = `<b>Histórico no Tempo — ${meta.label} do Elemento ${selectedPoint.id}</b>`;
            yTitle = meta.label;
        }

        // Vertical marker at the currently active step's time/load position -- links this chart
        // back to the playback bar's position, same idea as scrubbing a video timeline.
        const currentStep = steps[currentStepIdx];
        const shapes = [];
        if (currentStep) {
            const xCurrent = isDynamic ? currentStep.stepIndex * 0.05 : currentStep.loadFactor * 100;
            shapes.push({
                type: 'line', x0: xCurrent, x1: xCurrent, y0: 0, y1: 1, yref: 'paper',
                line: { color: textColor, width: 1.5, dash: 'dash' }, opacity: 0.5
            });
        }

        const layout = {
            ...layoutBase,
            title: titleLayout(titleText),
            xaxis: { title: xTitle, gridcolor: gridColor, zerolinecolor: gridColor, font: { size: 13 } },
            yaxis: { title: yTitle, gridcolor: gridColor, zerolinecolor: gridColor, font: { size: 13 } },
            legend: { orientation: 'h', x: 0.5, xanchor: 'center', xref: 'container', y: 0.9, yanchor: 'top', yref: 'container' },
            shapes
        };

        Plotly.react(this.chartDiv, traces, layout, { responsive: true, displayModeBar: true });
    }

    resize() {
        if (typeof Plotly === 'undefined') return;
        const el = document.getElementById(this.chartDiv);
        if (el && el.style.display !== 'none') Plotly.Plots.resize(el);
    }
}
