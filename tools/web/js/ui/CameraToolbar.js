/**
 * CameraToolbar.js
 * Wires the 8 camera/zoom toolbar buttons (#view-iso-btn/xy/xz/yz, #zoom-fit/in/out/window-btn)
 * to CameraViewController and ZoomWindowController -- same IDs, same behavior, on both pages
 * (preprocessor.html/posprocessor.html); only the "which step to use" function differs between
 * them (synthetic input data vs. the currently selected simulation step).
 */

/**
 * @param {import('../renderers/CameraViewController.js').CameraViewController} cameraController
 * @param {import('./ZoomWindowController.js').ZoomWindowController} zoomWindow
 * @param {() => object|null} getStep Nodes/elements to frame (the shape CameraViewController expects).
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
