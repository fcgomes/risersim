/**
 * CameraToolbar.js
 * Liga os 8 botões da barra de câmera/zoom (#view-iso-btn/xy/xz/yz, #zoom-fit/in/out/window-btn)
 * ao CameraViewController e ao ZoomWindowController -- mesmos IDs, mesmo comportamento, nas duas
 * páginas (preprocessor.html/posprocessor.html); só a função "qual step usar" difere entre elas
 * (dados sintéticos de entrada vs. o passo de simulação atualmente selecionado).
 */

/**
 * @param {import('../renderers/CameraViewController.js').CameraViewController} cameraController
 * @param {import('./ZoomWindowController.js').ZoomWindowController} zoomWindow
 * @param {() => object|null} getStep Nós/elementos a enquadrar (formato que CameraViewController espera).
 * @param {() => [number|null, number|null]} getEnvBounds [seabedDepth, waterSurfaceZ].
 */
export function bindCameraToolbar(cameraController, zoomWindow, getStep, getEnvBounds) {
    document.getElementById('view-iso-btn').addEventListener('click', () => cameraController.setView('ISO', getStep(), ...getEnvBounds()));
    document.getElementById('view-xy-btn').addEventListener('click', () => cameraController.setView('XY', getStep(), ...getEnvBounds()));
    document.getElementById('view-xz-btn').addEventListener('click', () => cameraController.setView('XZ', getStep(), ...getEnvBounds()));
    document.getElementById('view-yz-btn').addEventListener('click', () => cameraController.setView('YZ', getStep(), ...getEnvBounds()));

    document.getElementById('zoom-fit-btn').addEventListener('click', () => cameraController.fitToModel(getStep(), ...getEnvBounds()));
    document.getElementById('zoom-in-btn').addEventListener('click', () => cameraController.zoomIn());
    document.getElementById('zoom-out-btn').addEventListener('click', () => cameraController.zoomOut());
    document.getElementById('zoom-window-btn').addEventListener('click', () => zoomWindow.toggle());
}
