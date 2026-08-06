/**
 * TabPanel.js
 * Alterna qual painel de aba (`.bottom-tab-panel`) fica visível dentro de `#data-panel`, e marca
 * o botão correspondente como ativo -- mesmo algoritmo usado tanto pelas abas do pré-processador
 * (Carregar/Geometria/Ambiental/Análise) quanto pelas do pós-processador
 * (Controles/Visualização/Tabela/Carregar), só o conjunto de abas muda de uma página pra outra.
 */

/**
 * @param {Record<string, string>} tabMap Mapa `{chave: idDoPainel}` -- o botão correspondente é
 * `tab-${chave}-btn`.
 * @param {string} activeKey Chave (do tabMap) a ativar.
 */
export function switchTab(tabMap, activeKey) {
    Object.entries(tabMap).forEach(([key, panelId]) => {
        document.getElementById(panelId).classList.toggle('active', key === activeKey);
        document.getElementById(`tab-${key}-btn`).className = key === activeKey ? 'btn-tab active' : 'btn-tab';
    });
}
