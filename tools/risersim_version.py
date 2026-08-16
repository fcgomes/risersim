"""
risersim_version.py
=====================
Single version constant for the run manager's "web app" (see docs/roadmap.md, Axis 3b): covers
interface + pre-processor + post-processor together, since today they're the same deployment
(the `web` service: `tools/run_server.py` serving dashboard/project/preprocessor/
posprocessor.html + js/*.js) -- versioning the three separately wouldn't mean anything while
that stays true.

Bump by hand when a frontend/API change lands that's worth tracking per run (e.g. a change to
what preprocessor.html/posprocessor.html expect from the JSON). NOT the C++ solver's version
(automatic fingerprint, see run_worker.py::compute_solver_fingerprint()) nor the
input_simulation.json schema version (see xml_h5_reader.py::SCHEMA_VERSION) -- the three evolve
independently, which is why they're three separate fields recorded in project.json/run.json (see
risersim_projects.py).
"""

WEB_VERSION = "1.6.0"
