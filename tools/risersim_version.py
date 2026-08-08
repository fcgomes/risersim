"""
risersim_version.py
=====================
Constante única de versão da "web app" do gerenciador de rodadas (ver docs/roadmap.md Eixo 3b):
cobre interface + pré-processador + pós-processador porque hoje são o mesmo deploy (`web`,
tools/run_server.py servindo dashboard/project/preprocessor/posprocessor.html + js/*.js) -- não
faz sentido versionar os três separadamente enquanto isso continuar verdade.

Bumpar à mão quando uma mudança relevante entrar no frontend/API que valha a pena rastrear por
rodada (ex.: mudança no que preprocessor.html/posprocessor.html esperam do JSON). NÃO é a versão
do solver C++ (fingerprint automático, ver run_worker.py::compute_solver_fingerprint) nem do
schema do input_simulation.json (ver xml_h5_reader.py::SCHEMA_VERSION) -- os três evoluem de
forma independente, por isso três campos separados gravados em project.json/run.json (ver
risersim_projects.py).
"""

WEB_VERSION = "1.0.0"
