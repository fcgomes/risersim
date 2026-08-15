/**
 * TabPanel.js
 * Switches which tab panel (`.bottom-tab-panel`) is visible inside `#data-panel`, and marks the
 * corresponding button as active -- the same algorithm used by both the preprocessor's tabs
 * (Load/Geometry/Environmental/Analysis) and the postprocessor's tabs
 * (Visualization/Table), only the set of tabs changes between pages.
 */

/**
 * @param {Record<string, string>} tabMap `{key: panelId}` map -- the corresponding button is
 * `tab-${key}-btn`.
 * @param {string} activeKey Key (from tabMap) to activate.
 */
export function switchTab(tabMap, activeKey) {
    Object.entries(tabMap).forEach(([key, panelId]) => {
        document.getElementById(panelId).classList.toggle('active', key === activeKey);
        document.getElementById(`tab-${key}-btn`).className = key === activeKey ? 'btn-tab active' : 'btn-tab';
    });
}
