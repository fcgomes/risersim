# Roadmap do `risersim`

> Documento de planejamento, não de investigação (diferente dos três `mapa_*.md`). Consolida os
> seis objetivos que o usuário definiu em 2026-08-07 — resolver o bug da estática, rodar a análise
> dinâmica, implementar itens faltantes, interface de entrada de dados, interface de controle de
> simulação (projetos/disparo/acompanhamento) e pós-processamento — em eixos com dependências e uma
> ordem sugerida. Não é uma decisão fechada; serve de base para decidir, um eixo de cada vez, o que
> planejar em detalhe a seguir.

## Eixo 1 — Confiabilidade do motor (bloqueante para qualquer resultado em que se possa confiar)

### 1a. Fechar o bug estático solo+corrente — ✅ RESOLVIDO

Depois de exaustivamente investigado (rotação total-vs-local, decaimento da rigidez artificial,
atrito elástico-plástico, atrito axial/lateral real, rampa de carga real, critério de convergência
combinado, `%ASSEMBLY.USING` real) sem fechar o gap, e depois de uma tentativa de estabilização
numérica (limitar o passo por norma física) que ajudava casos isolados mas piorava o modelo
completo, a causa raiz acabou sendo um **bug de dados**, não numérico: o perfil de corrente por
profundidade nunca era de fato interpolado — dois bugs encadeados (`xml_h5_reader.py` guardando a
profundidade na convenção errada + `CurrentProfile` assumindo superfície em `z=0` quando o modelo
real tem o leito em `z≈0`) faziam toda consulta cair no valor de **superfície**, aplicando até
~27x mais carga lateral que a real na zona de touchdown. Corrigido — ver
[`mapa_classes_anflex_estatica.md`](mapa_classes_anflex_estatica.md), seção "Bug real encontrado e
corrigido: perfil de corrente sempre avaliado na superfície". **Os 11 passos de carga do
Exemplo_01a completo convergem agora**, sem precisar do step limiting.

Lição para o resto do roadmap: depois de esgotar hipóteses numéricas/algorítmicas razoáveis, uma
auditoria direta de dados (comparar o valor real no XML contra o que de fato chega no C++, campo a
campo) foi o que resolveu — vale aplicar essa mesma disciplina antes de partir para técnicas mais
sofisticadas em qualquer outra frente (ex. 1b abaixo).

**Pendências resolvidas** (ver `mapa_classes_anflex_estatica.md`, seção "Pendências resolvidas"):
step limiting confirmado desnecessário (resultado atual não o usa); `CurrentProfile` confirmado
usado também pela dinâmica, já corrigido automaticamente por herança de valor
(`DynamicAnalysis(const Analysis&)`), sem bug análogo na força de onda (não é dependente de
profundidade neste código). Achado colateral: o sistema convergido está bem perto de uma
bifurcação numérica — a trajetória passo-a-passo (não o resultado físico final) é sensível a
detalhes incidentais, então comparações futuras devem olhar o resultado final, não iterações
por passo.

**Confirmação adicional** (auditoria de nomenclatura/conversão, ver
`mapa_aml_exemplos_e_web_interface.md`, achado 2): eliminar o `rho` fabricado (densidade
"peso-equivalente") em favor da densidade seca real + `water_density` real reforçou o achado
acima -- mesmo uma diferença de ~0,0006% no peso líquido por metro foi suficiente pra fase de
assembly (pré-solve) falhar em convergir dentro do orçamento padrão de 40 iterações nalguns runs,
exigindo mais iterações (150 testado) pra chegar num ponto de partida bom o bastante pra fase
final. O resultado físico final continua correto e estável (T_eff≈217,3-217,4 kN) -- só o
orçamento de iteração da assembly pode precisar de folga maior em modelos reais como este. Vale
considerar, quando o Eixo 1a for revisitado, aumentar o default de `static.max_iterations` da
assembly (ou torná-lo independente do valor do XML) em vez de manter os 40 vindos de
`AnalysisData/Static/num_max_iter`.

### 1b. Investigar a análise dinâmica — 🟡 EM PROGRESSO (achado real, ainda não fechado)

Começada nesta rodada, seguindo a mesma disciplina que resolveu 1a (auditoria de dados antes de
técnica numérica). Rodando o Exemplo_01a completo com dinâmica pela primeira vez desde que a
estática passou a convergir: **todos os 20 passos de tempo falhavam**. Auditoria encontrou um bug
real e concreto: `total_linear_mass()` (massa/inércia dinâmica) reaproveitava `rho` -- que na
verdade é um valor "equivalente de peso submerso" (líquido de empuxo), correto só para a fórmula
de peso estático -- como se fosse densidade estrutural real. Empuxo reduz peso líquido mas nunca
reduz massa inercial; pro Exemplo_01a isso subestimava a massa dinâmica em **~2,4x**. Corrigido
(novo campo `rho_structural`, derivado do peso seco real do XML, já calculado mas antes
descartado) — ver `mapa_classes_anflex_estatica.md`, seção "Pendências resolvidas".

**Resultado**: a dinâmica passa a convergir em **15 dos 20 passos** (antes: 0). Real e
substancial, mas não resolvido -- passos 1 e 5-8 ainda falham (resíduo crescendo, não platô);
testado orçamento maior de iterações, não ajudou (mesma assinatura de "janela de escape desliza
pra pior" já vista na estática).

**Atualização** (ver `mapa_aml_exemplos_e_web_interface.md`, seção "Auditoria de conversões de
valor", achado 4): o amortecimento de Rayleigh JÁ era lido corretamente do JSON pelo C++
(`simulation.cpp:100-101` — a suspeita anterior de "hardcoded no construtor" estava desatualizada);
o gap real era `xml_h5_reader.py` nunca extrair o valor verdadeiro do XML (`mass_damping`/
`stiffness_damping` por material, já calculados pelo ANFLEX real), hardcodando `alpha=0.05,
beta=0.005` sempre. Corrigido -- os 7 XMLs de exemplo disponíveis têm `consider_damping="no"`
(amortecimento real = 0), então o Exemplo_01a hoje roda com o dado fiel. Verificado: não muda o
resultado estático, e o padrão de convergência dinâmico ficou essencialmente igual (resíduos um
pouco maiores sem o amortecimento fabricado que antes ajudava marginalmente) -- ou seja, esse não
era o fator limitante da dinâmica. A contagem exata de passos convergidos variou entre verificações
consecutivas mesmo sem nenhuma mudança de dado de entrada (4/20 nesta rodada vs. 15/20 documentado
antes) -- confirma que é a mesma sensibilidade de "beira de bifurcação" já registrada em 1a, não
uma regressão.

**Atualização 2** (ver `mapa_aml_exemplos_e_web_interface.md`, seção "Movimento de topo real"):
implementado o movimento real de topo (RAO + JONSWAP "equivalent harmonic", novo módulo
`vessel_motion.hpp`/`.cpp`), substituindo a onda regular Z-só que existia antes. Achado colateral:
a suspeita de que faltava migrar o movimento prescrito do topo pra `PrescribedMotion` (a técnica
de penalidade da estática) era falsa pista -- o nó de topo tem todos os GDL eliminados
(`prescribed_dofs: [-1]*6`), então atribuir `disp`/`rot` diretamente já era a técnica certa; o gap
real era a ausência da RAO/JONSWAP em si, não a técnica de imposição. Durante a implementação, um
bug real de interpretação do campo `Rao/offset` do XML (usar a posição real do nó, ~47m de braço
de alavanca, em vez do offset casco→CM real, poucos metros) produzia uma amplitude de heave de
30m -- corrigido pra usar `offset` direto (~10,3m). Usuário questionou se 10m ainda não era muito;
investigação independente (reprodução em Python) achou que a transferência geométrica estava
amplificando um pitch real da tabela RAO (~0,34 rad/m, incomumente alto) via o braço de ~13m --
fórmula conferida e correta, mas a discrepância entre a posição real do nó (X=-47,73m) e
`movement_center` (X=0) sugere que o referencial de `movement_center`/`offset` pode não ser o
mesmo da malha do riser, sem como confirmar contra nenhum consumidor XML real. Decisão do
usuário: desligar a transferência geométrica por enquanto (movimento aplicado no CM) -- caiu pra
~0,85m, ordem de grandeza mais conservadora; fórmula fica implementada e comentada, pronta pra
reativar quando o referencial for confirmado. Resultado: estática inalterada em todas as versões,
resíduos dinâmicos na mesma ordem de grandeza de antes em todas elas (nunca mais a explosão
numérica de 10²⁰ da primeira versão com bug), mas a dinâmica ainda não converge nos 20 passos
completos -- não era (sozinho) o fator limitante.

**Atualização 3** (ver `mapa_aml_exemplos_e_web_interface.md`, seção "Movimento de topo real",
rodada 3): a dúvida de referencial da Atualização 2 foi resolvida lendo o código-fonte real direto
-- `model_builder_dat.cpp`/`anf_movements.cpp`/`save-dat.cpp` confirmam que `cm_position` =
`movement_center + offset` (somados) e que o braço de alavanca vem de `(posição real do nó
pós-estática) - cm_position`, exatamente a abordagem da primeira implementação (revertida antes
por engano) -- o `.dat` não é um formato paralelo desconectado, é a ponte real entre a interface e
o solver, e `model_builder_dat.cpp` é o único lugar em `trunk/src` que constrói
`cEquivalentHarmonic`. Reativada a transferência geométrica com a fórmula correta. Resultado ao
rodar o Exemplo_01a real: heave volta a **30,17m** (praticamente igual à primeira versão -- confirma
que aquele valor era física real do ANFLEX, não um bug) e a dinâmica volta a divergir
catastroficamente (resíduos a 10²⁴, mesma ordem da primeira versão). A fórmula agora bate 100% com
o ANFLEX real, mas isso expõe -- sem mascarar -- que o solver dinâmico do risersim não aguenta um
movimento de topo dessa magnitude nos parâmetros atuais (rampa de 5s dentro de uma janela de
simulação de só 1s/20 passos, Newton sem line search). Ou seja: o "achado 2" (transferência
desligada) era um mascaramento, não uma correção -- a causa real do não-convergimento inclui esse
movimento de topo genuinamente grande, que só ficou visível agora que o dado está fiel.

**Atualização 4**: revisão pedida pelo usuário sobre ordem de translação/rotação achou e corrigiu
um problema real (não a causa raiz, mas uma correção de fidelidade válida): `dynamic_analysis.cpp`
compunha a rotação do nó de topo prescrito via `compose_rotations()` (produto de Rodrigues,
não-linear), enquanto o mecanismo real de movimento prescrito (`integrator.cpp::set_load_dofs:81-93`)
soma componente a componente (`presc_desl[i] += movements[i]`, sem composição não-linear) --
consistente com o método "harmônico equivalente" já ser linearizado do início ao fim. Corrigido
(`top_node->rot = static_rots.front() + vessel_rot`), suíte sem regressão. Resultado ao rerodar:
praticamente idêntico (heave 30,17m, divergência na mesma ordem de grandeza) -- **não era a causa**.
Achado mais importante dessa rodada: o resíduo já explode a partir do passo 2 (t=0,1s), quando a
rampa de 5s ainda deixa o movimento prescrito quase nulo (fator de rampa ~0,001) -- ou seja, a
causa provavelmente NÃO é a magnitude do movimento de topo (~30m), é algo que quebra o solver
mesmo com um deslocamento de topo praticamente zero. Isso muda o foco do próximo passo: não é mais
"por que um movimento de 30m diverge", é "por que o Newton dinâmico diverge quase imediatamente
mesmo com o topo essencialmente parado" -- um problema mais estrutural (massa/rigidez/Newmark-beta
em si), coerente com o histórico desta seção (a dinâmica nunca convergiu nos 20 passos completos em
nenhuma versão testada, mesmo antes de qualquer trabalho de movimento de topo).

**Atualização 5** (causa raiz encontrada e corrigida, parcialmente): isolei experimentalmente
(a pedido do usuário) que topo genuinamente PARADO (`vessel_motion.enabled=false` E
`wave.amplitude_m=0`) converge nos 20 passos completos -- confirmado até sob um build com
AddressSanitizer+UBSan (descartando bug de memória). Qualquer movimento variando no tempo, por
menor que seja, já quebra. Isso apontou pro laço de montagem da matriz de massa
(`dynamic_analysis.cpp:187-205`): o nó de topo tem `eq_numbers=[-1]*6` (GDL eliminado), então
qualquer termo de massa consistente do elemento que o toque -- inclusive o acoplamento inercial
`M_BA·a_topo` com o primeiro nó livre -- é descartado na montagem. Perguntei "e o ANFLEX faz de
que forma?" antes de corrigir: confirmado em `domain.cpp:546-578` que o ANFLEX real usa **método
de penalidade** ("big number") pro movimento prescrito, não eliminação de GDL -- o nó nunca sai do
sistema, então esse termo nunca desaparece lá. Corrigido reaproveitando `PrescribedMotion`
(`prescribed_motion.hpp`), já implementada e testada -- é o mesmo mecanismo que
`StaticAnalysis::solve_vessel_offset` já usa pro offset estático, só que agora também na dinâmica
(nó de topo ganha GDL reais + mola de penalidade, `compose_rotations()` correto pro estado do
Newton, alvo do achado 4 mantido). Suíte sem regressão (361 asserts). **Resultado real**: melhora
genuína, mas parcial -- resíduos ficam limitados (milhares) até o passo 12 (antes: explosão já no
passo 4), mas a partir do passo 13 volta a divergir (10²² no passo 20), ainda sem convergir os 20
passos completos. Ou seja, a causa identificada era real e a correção ajudou de fato, mas não é a
única coisa em jogo -- 30m de heave + 7,6m de surge numa janela de 1s/20 passos (contra um período
de onda de ~11s, nem um ciclo completo) continua sendo um movimento muito agressivo pro passo de
tempo usado.

**Atualização 6** (usuário desconfiou: "estou achando muito sensível essa estática, será que
temos alguma invasão de memória, algum lixo no caminho?"): tinha razão. ASan/UBSan (usados na
Atualização 5) não pegam esse tipo de bug por padrão -- rodei **Valgrind memcheck
--track-origins=yes** contra o caso determinístico de falha estática (`vessel_motion.enabled=
false`, que não deveria sequer tocar a estática) e achou de cara: `analysis.cpp:24-28` fazia
`node_tangent_sum[node] += ex` num `unordered_map<Node3D*, Eigen::Vector3d>` -- `operator[]`
default-constrói a entrada na primeira inserção, e o construtor padrão de `Eigen::Vector3d` **não
zera os coeficientes** (documentado no próprio Eigen, por performance), ao contrário de
`std::array`/`double`. Lixo de memória sendo somado a `ex` na primeira ocorrência de cada nó. Esse
vetor alimenta a decomposição de atrito do solo (axial/lateral), que entra na matriz de rigidez --
o rastro do Valgrind confirmou a contaminação chegando até dentro da fatoração de Cholesky. Só
importa pra nós com `k_seabed>0` (zona de toque do fundo, que o Exemplo_01a tem). **Corrigido**:
`try_emplace(node, Eigen::Vector3d::Zero())` antes de cada `+=`. Suíte sem regressão. **Resultado**:
o caso que sempre falhava deterministicamente agora **converge limpo** sob build Release normal,
estática E dinâmica completas nos 20 passos -- boa parte da sensibilidade "beira de bifurcação"
documentada nesta sessão inteira não era sensibilidade numérica genuína, era lixo de memória
mesmo. Combinando com a Atualização 5 (acoplamento inercial), o caso real de 30m de heave também
melhorou mais: passos 1-14 convergem todos limpos agora (antes: só até o 12), divergência começa
só no 15 -- ainda não fecha os 20 passos, mas o avanço acumulado dos dois bugs reais é substancial.

**Recomendação pro próximo passo**: com massa dinâmica, Rayleigh damping, movimento de topo,
composição de rotação, o acoplamento inercial do nó prescrito E memória não-inicializada já
corrigidos/confirmados, os candidatos de auditoria de dado/formulação/memória conhecidos estão
esgotados. Dois caminhos concretos: (a) isolar um caso mínimo com um movimento de topo mais modesto
(ou uma duração/passo de tempo mais realista, já que 1s/20 passos nem completa um ciclo de onda de
11s) e comparar diretamente contra `cDynamicAnalysis`/`cDynamicIntegrator` do ANFLEX real
(`trunk/src`), mesma metodologia que achou a rotação total-vs-local na estática; (b) investigar se
falta algo tipo line search/sub-passos no Newton dinâmico (o real ANFLEX também não tem line search,
confirmado em `newton_raphson.cpp` -- mas pode ter outra técnica de robustez, ex. redução automática
de `dt` quando o passo não converge, que o risersim hoje não tem).

## Eixo 2 — Pipeline de dados (desbloqueia mais casos de teste reais, baixo risco)

### 2a. Religar `aml_reader.py` ao schema real do `ModelBuilder`

Achado em [`mapa_aml_exemplos_e_web_interface.md`](mapa_aml_exemplos_e_web_interface.md): hoje, 23
dos 30 exemplos disponíveis (todos sem pasta `_analysis/` com XML/H5 pré-exportado) rodam
silenciosamente com o modelo sintético de fallback — `aml_reader.py` produz um schema que
`ModelBuilder` nunca entende. Consertar isso (gerar `model.nodes`/`model.elements` a partir dos
segmentos já extraídos, resolver corrente/solo por ID como `xml_h5_reader.py` já faz) é um trabalho
bem-escopado e de baixo risco (não toca o C++, só o parser Python), e destrava candidatos reais
novos ao bug solo+corrente — em especial `DNV_Check.aml` (solo e corrente confirmados casando por
ID, caminho estático).

### 2b. Suporte a múltiplas zonas de solo por segmento (opcional, avaliar sob demanda)

Achado no `Boiao/P52_Boiao.aml` (uma linha atravessando 3 solos diferentes ao longo do próprio
comprimento) — nem `aml_reader.py` nem `xml_h5_reader.py` correlacionam isso hoje, ambos assumem
solo único global. Só vale a pena se um caso de teste real desse tipo entrar em uso.

## Eixo 3 — Interfaces (podem começar em paralelo aos eixos 1-2, escopadas ao que o motor já suporta)

### 3a. Interface de entrada de dados

Arquitetura já desenhada em `mapa_aml_exemplos_e_web_interface.md`: dois JSONs (um "de interface",
próximo ao modelo lógico — linha com segmentos referenciando material/solo/corrente por nome; um
"de simulação", o schema atual e inalterado que `ModelBuilder` já lê), com um compilador JS
rodando no navegador. Falta: desenhar o formato concreto do JSON de interface, construir o
formulário (reaproveitando a casca de UI já existente — `TabPanel`, `PanelResizer`, `ThemeToggle`,
toolbar de câmera 3D — hoje só usada para visualização, nunca para edição), escrever o compilador.
Escopo inicial deve ficar restrito ao que o motor já resolve hoje: uma linha só, um material de
seção genérico, sem boias/tendões/turret/ruptura (ver Eixo 5).

### 3b. Interface de controle de simulação (projetos, disparo, acompanhamento)

A peça arquitetural mais nova de todo o roadmap — hoje não existe nenhum backend real, só
`tools/dev_server.py` (servidor de arquivos estáticos) e execução manual do binário C++ via
Docker/CLI. Precisa de desenho próprio antes de qualquer código, cobrindo pelo menos:
- Onde e como um "projeto" (modelo + histórico de rodadas) é persistido — sistema de arquivos é
  provavelmente suficiente para começar, sem precisar de banco de dados.
- Como uma simulação é de fato disparada a partir do navegador — precisa de um serviço/API por trás
  (o navegador não pode invocar `risersim_test_main` diretamente); decidir se roda local, em
  Docker, ou outro lugar.
- Como o progresso é acompanhado em tempo real — o solver já imprime por iteração/passo no stdout
  (`std::cout` com `std::endl`, ver `static_analysis.cpp`), então captura/streaming desse output
  (polling de arquivo de log ou algo como Server-Sent Events) é o caminho mais direto, sem precisar
  instrumentar o C++ para emitir eventos estruturados.

**Recomendação**: tratar como uma rodada de planejamento própria (Explore + Plan) quando chegar a
vez, dado que introduz infraestrutura nova (backend) que hoje não existe.

#### Fase 1 — ✅ IMPLEMENTADA: backend de projetos/rodadas + dashboard/project.html

Sistema de arquivos como fonte de verdade (`risersim/projects/<id>/{project.json,
input_simulation.json, runs/<run-id>/{run.json, input_simulation.json, stdout.log,
catenary_results.json/.h5}}`), sem banco de dados — ver `tools/risersim_projects.py`
(`ProjectStore`). Dois processos separados no `docker-compose.yml`, comunicando só pelo volume
compartilhado: `web` (`tools/run_server.py`, API REST + estático, substituto drop-in de
`dev_server.py`) e `worker` (`tools/run_worker.py`, loop serial que roda `risersim_test_main` uma
rodada de cada vez). Frontend: `dashboard.html`/`project.html` (grid de projetos, criação por
exemplo pré-descoberto de `trunk/exemplos/`, disparo de rodada, log ao vivo via polling de
`stdout.log`). Testado end-to-end (Docker build + Catch2 sem regressão + fluxo completo via
headless Chrome) antes desta rodada de trabalho.

#### Fase 2 — ✅ IMPLEMENTADA: proveniência de versões, hash de modelo, acesso a pré/pós, import por upload

Continuação direta da Fase 1, quatro blocos (ver histórico de planejamento — usuário levantou os
quatro pontos numa mesma conversa):

- **A. Proveniência de versões por rodada** — três campos gravados em `run.json`, cada um evoluindo
  independentemente: `solver_fingerprint` (sha256 do binário `risersim_test_main`, calculado só
  pelo `worker` — só ele tem acesso ao binário compilado — em
  `run_worker.py::compute_solver_fingerprint()`, preenchido quando a rodada começa a rodar, não na
  criação); `schema_version` (constante `SCHEMA_VERSION` em `xml_h5_reader.py`, gravada dentro do
  próprio `input_simulation.json` por `to_risersim_json()`, lida de volta do snapshot já copiado
  por `risersim_projects.py::create_run()`); `web_version` (constante única `WEB_VERSION`, novo
  módulo `tools/risersim_version.py`, cobre interface+pré+pós-processador porque são o mesmo
  deploy — exposta em `GET /api/version`, gravada em `project.json` e em todo `run.json` na
  criação). Motivação real: o próprio bug de memória não-inicializada corrigido nesta sessão
  (mesmo config, binário diferente, resultado diferente) é exatamente o cenário que
  `solver_fingerprint` existe para tornar rastreável.
- **B. Hash do modelo + dedupe de rodadas** — `model_hash` (sha256 do `input_simulation.json`
  congelado em cada rodada, calculado em `create_run()`). `POST /api/projects/<id>/runs` ganhou um
  parâmetro `force` (bool); sem ele, se já existir uma rodada TERMINADA
  (`converged`/`failed`) do mesmo projeto com o mesmo `model_hash`
  (`ProjectStore.find_run_by_model_hash()`), a API responde `409` com o `run_id` da duplicata em
  vez de criar outra à toa — evita gastar a fila serial (uma rodada de cada vez) sem necessidade,
  já que o solver é determinístico dado o mesmo binário. `force=true` bypassa. `project.js` trata o
  409 com `confirm()` e reenvia com `force=true` se o usuário confirmar; tabela de rodadas ganhou
  uma badge discreta `= run-X` nas linhas com `model_hash` repetido.
- **C. Acesso direto a pré/pós-processamento a partir do projeto** — pós já existia; pré-processador
  ganhou `preprocessor_app.js::resolveInputUrl()` (mesmo padrão de `app.js::resolveResultsUrl()`):
  `?project=<id>` → `GET /api/projects/<id>/input` (novo endpoint, input do NÍVEL DO PROJETO, "o
  que uma rodada nova usaria agora"); `?project=<id>&run=<run-id>` → rota genérica de resultados
  por rodada já existente, só adicionando `"input_simulation.json"` ao dict
  `RESULT_FILENAMES` de `run_server.py` (o snapshot já é copiado por `create_run()`, só faltava
  servir). `project.html` ganhou link "🔍 Ver entrada" (nível projeto, cabeçalho) e, por linha da
  tabela de rodadas, "🔍 Entrada usada" (sempre disponível, ao contrário de "Ver resultados" que
  exige `status=converged`).
- **D. Import de caso via upload + área de origem unificada** — novo endpoint
  `POST /api/projects/upload` (multipart: `name`, `xml_file`, `h5_file`, `aml_file` opcional),
  reaproveitando `build_config_from_xml_h5()` tal e qual (sem duplicar a lógica de compilação).
  `create_project()` passou a copiar os arquivos de origem (XML+H5+AML) pra dentro de
  `projects/<id>/source/{model.xml,model.h5,model.aml}` via `_store_source_files()` — caminho de
  código único usado tanto pelo fluxo por exemplo pré-descoberto quanto pelo de upload, resolvendo
  o gap onde `source.xml_path` de projetos-por-exemplo apontava pra fora do diretório do projeto
  (dependente do mount de `trunk/exemplos` continuar no mesmo lugar). `dashboard.html` ganhou uma
  segunda aba "📤 Importar arquivo" no modal "Novo Projeto" (file pickers, submit via `FormData`).

**Verificação real** (2026-08-08, Docker Desktop 29.6.2 no Windows, `docker compose build` + `up`):
rebuild de `web`+`worker` OK (C++ do stage `builder` veio do cache, zero mudança); suíte Catch2
rodada explicitamente no stage `builder` — **361 assertions em 15 test cases, todas passando**
(mesmo número já documentado, confirma zero regressão — nenhum arquivo em `src/`/`include/`/
`tests/` foi tocado nesta rodada). Fluxo completo via API real (`curl`) + headless Chrome
(`chrome --headless=new --dump-dom`, mais um harness HTML same-origin pra simular cliques reais
no modal, já que não havia Playwright/Selenium disponíveis no ambiente):
`GET /api/version` → `{"web_version":"1.0.0"}`; projeto criado por exemplo
(`Curso/Exemplo_01/Exemplo_01a`) com `source/model.{xml,h5,aml}` de fato copiados pro diretório do
projeto (confirmado via `docker compose exec`); rodada disparada e processada pelo worker —
`run.json` final com `model_hash`, `schema_version:1`, `web_version:"1.0.0"` e
`solver_fingerprint` (preenchido só depois do worker pegar a rodada, valor batendo com o log de
startup do worker) todos presentes; segunda tentativa de rodada do mesmo projeto sem `force` →
`409` com corpo `{"error":"já existe uma rodada terminada com o mesmo model_hash","run_id":"run-
20260808-204642","status":"failed"}`; com `force:true` → `201`, nova rodada criada, e a badge
`= run-20260808-204642` apareceu na linha da rodada duplicada em `project.html` (confirmado via
dump-dom); `preprocessor.html?project=<id>` e `?project=<id>&run=<run-id>` carregaram os dados
certos nos dois casos (501 nós/500 elementos, sem avisos, em ambos); upload manual de um par
XML+H5+AML de `trunk/exemplos/Curso/Exemplo_02/Exemplo_02a/` via `POST /api/projects/upload`
(`curl -F`) criou o projeto, copiou os três arquivos de fato pra `projects/<id>/source/`, e a
rodada disparada nesse projeto rodou pelo mesmo pipeline do worker (estática convergida, T_eff≈1027
kN, dinâmica em andamento nos mesmos termos de uma rodada por exemplo) — comportamento idêntico ao
fluxo por exemplo, só a origem dos arquivos muda. Aba "📤 Importar arquivo" testada via clique real
simulado num harness same-origin: abre o modal, alterna painéis (`display:none`/`""`), atualiza
classe `active` dos botões e `dashboard.sourceTab`, tudo correto.

### 3c. Pós-processamento

Já existe uma base sólida (`posprocessor.html`, viewer 3D, gráficos Plotly). O trabalho principal
aqui é integrar com o conceito de "projeto/rodada" do item 3b (carregar resultados de uma rodada
específica do histórico, em vez de sempre apontar para um arquivo fixo) — depende de 3b existir
primeiro para fazer sentido completo, mas a base de visualização já não precisa de retrabalho.

## Backlog de recursos faltantes do motor (não bloqueante — puxar sob demanda)

Achados documentados em `mapa_aml_exemplos_e_web_interface.md`, nenhum implementado: múltiplas
linhas com corpo flutuante compartilhado (`Multilinhas.aml`), boias/tendões como entidade própria
(hoje só existe `BuoyancyModule`/`BendRestrictor`, modificadores locais não lidos por
`ModelBuilder::load_from_json`), conexões articuladas tipo flexjoint/drilljoint, turret com
movimento prescrito 6-GDL por caso de carga (`Turret.aml`), ruptura de elemento em tempo de
execução dinâmico (`Ruptura.aml`), verificação de código DNV como pós-processamento
(`DNV_Check.aml`). Cada um só vale a pena quando um caso de teste real concreto precisar dele — não
faz sentido implementar especulativamente.

## Ordem sugerida (não é obrigatória — ponto de partida pra decidir)

1. ~~**1a** (bug estático)~~ — ✅ resolvido (era um bug de dados no perfil de corrente, não
   numérico). Confiança no motor pra Exemplo_01a estático agora estabelecida.
2. **2a** (religar `aml_reader.py`) — barato, baixo risco, abre mais um caso de teste real
   (`DNV_Check`) que pode ajudar a confirmar 1a de forma independente.
3. 🟡 **1b** (dinâmica) — em progresso: achado e corrigido um bug real de massa (`rho_structural`),
   dinâmica foi de 0/20 pra 15/20 passos de tempo convergindo no Exemplo_01a. Continuar com a
   mesma disciplina (Rayleigh damping é o próximo dado ainda não auditado).
4. **3a** (entrada de dados) — pode começar o desenho/construção em paralelo a tudo acima, já que é
   front-end puro e não depende do motor mudar; só precisa ficar escopado ao que o motor já resolve.
5. **3b** (controle de simulação) — a peça que mais se beneficia de vir depois, já que introduz
   infraestrutura nova; faz mais sentido desenhar quando já houver mais clareza sobre 1b (o que
   "acompanhar uma simulação" precisa mostrar depende de quão bem-comportado o solver já está).
6. **3c** (pós-processamento) — reboca 3b.
7. **Backlog de recursos faltantes** — sob demanda, conforme cada item vire necessário pra um caso
   de teste real específico.

## Ver também

- [`mapa_classes_anflex_estatica.md`](mapa_classes_anflex_estatica.md) — investigação do solver e
  do bug de convergência solo+corrente (Eixo 1a).
- [`mapa_classes_anflex_interface.md`](mapa_classes_anflex_interface.md) — mapa da interface
  gráfica real do ANFLEX (base para o Eixo 3a).
- [`mapa_aml_exemplos_e_web_interface.md`](mapa_aml_exemplos_e_web_interface.md) — censo de
  exemplos, lacuna do `aml_reader.py` (Eixo 2a), e arquitetura de dois JSONs (Eixo 3a).
