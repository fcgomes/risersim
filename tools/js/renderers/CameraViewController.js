/**
 * CameraViewController.js
 * Controlador OO de vistas predefinidas de câmera CAD (Isométrica, XY, XZ, YZ).
 */
export class CameraViewController {
    /**
     * @param {THREE.Camera} camera 
     * @param {OrbitControls} controls 
     */
    constructor(camera, controls) {
        this.camera = camera;
        this.controls = controls;
    }

    /**
     * Define a vista da câmera baseada na opção selecionada
     * @param {'ISO'|'XY'|'XZ'|'YZ'} viewType 
     * @param {SimulationStep} currentStep 
     */
    setView(viewType, currentStep) {
        if (!this.camera || !this.controls) return;

        let minX = 0, maxX = 120, minY = -100, maxY = 0;
        if (currentStep && currentStep.nodes && currentStep.nodes.length > 0) {
            minX = Math.min(...currentStep.nodes.map(n => n.x));
            maxX = Math.max(...currentStep.nodes.map(n => n.x));
            minY = Math.min(...currentStep.nodes.map(n => n.z));
            maxY = Math.max(...currentStep.nodes.map(n => n.z));
        }

        const centerX = (minX + maxX) / 2.0;
        const centerY = (minY + maxY) / 2.0;
        const target = new THREE.Vector3(centerX, centerY, 0);

        if (viewType === 'ISO') {
            this.camera.position.set(centerX + 60, centerY + 60, 160);
        } else if (viewType === 'XY') {
            // Vista Superior
            this.camera.position.set(centerX, centerY + 220, 0.001);
        } else if (viewType === 'XZ') {
            // Vista de Elevação / Perfil
            this.camera.position.set(centerX, centerY, 220);
        } else if (viewType === 'YZ') {
            // Vista Frontal Transversal
            this.camera.position.set(centerX + 260, centerY, 0);
        }

        this.controls.target.copy(target);
        this.controls.update();
    }
}
