# Opções de arquitetura e bibliotecas open-source para um sucessor do ANFLEX

> Documento de referência, não uma decisão de arquitetura. Consolida o levantamento feito sobre bibliotecas open-source para um eventual sucessor do ANFLEX (motor de análise de risers/linhas de ancoragem em C++, orientado a objetos, com interface web de pré/pós-processamento e acompanhamento de simulações), para retomar a análise numa sessão futura.

## 1. Bibliotecas físicas/FEM candidatas

| Biblioteca | Licença | Foco | Maturidade | Relevância para o ANFLEX successor |
|---|---|---|---|---|
| **Project Chrono** | BSD-3 (permissiva) | Multibody physics engine completo; elementos de viga/cabo ANCF (`ChElementCableANCF`, `ChElementBeamANCF`) para estruturas esbeltas com grandes deslocamentos; contato, juntas | Muito madura, release estável 10.0.0 (mar/2026), usada em aplicações offshore | Substituiria nosso elemento de viga corrotacional + solver não-linear próprio por uma formulação (ANCF) diferente mas validada há anos; ganha multibody "de graça" (conexão com boias/embarcações) |
| **MAP++** (NREL) | Apache 2.0 (permissiva) | Solver quasi-estático multi-segmento para linhas de ancoragem (Multi-Segmented Quasi-Static) — solo, atrito, forças externas | Madura, mantida pela NREL, usada no ecossistema OpenFAST | Resolveria especificamente o equilíbrio estático inicial — o problema exato do bug de divergência atual do risersim — sem precisar debugar nosso Newton-Raphson próprio |
| **MoorDyn-C** | BSD-3 (permissiva) | Dinâmica de linhas de ancoragem por massas concentradas (lumped-mass); rigidez/amortecimento axial, peso/empuxo, Morison, contato com solo já embutidos | Madura, versão C++ standalone (além da Fortran usada no OpenFAST), ativa | Cobre boa parte da física que já reimplementamos à mão (`seabed.hpp`, hidrodinâmica) com uma formulação mais simples que FEM completo — bom encaixe para linhas de ancoragem especificamente |
| **MoorPy** (NREL) | Apache 2.0 (permissiva) | Análise quasi-estática de linhas de ancoragem/catenária, **Python puro** (sem compilação), atrito com o leito marinho embutido | Madura, ativa (`github.com/NREL/MoorPy`), `pip install moorpy` | **Validado nesta sessão** (ver "Resultado da validação" abaixo) — converge no equilíbrio estático do `Exemplo_01a` real onde o risersim diverge, com risco de integração quase zero (Python puro, sem build C++/Fortran) |
| **OpenSees** | Não-comercial / não-redistribuível (uso interno livre, mas proíbe redistribuição do programa ou derivados por entidades fora do meio acadêmico/pesquisa) | Framework de elementos finitos para engenharia estrutural/geotécnica; elementos de viga corrotacional e "force-based" muito robustos | Muito madura (PEER/Berkeley, desde 1997), C++ | Fisicamente a melhor formulação de viga não-linear disponível, mas a licença é uma restrição real se o sucessor do ANFLEX puder vir a ser redistribuído/comercializado — precisa checar com cuidado antes de considerar |
| **Kratos Multiphysics** | BSD-4 (permissiva) | Framework genérico multi-física (não específico para cabos/risers); módulo de mecânica estrutural com elementos de viga linear/não-linear | Madura, ativa (v10.2.3 em 2025), C++ com interface Python extensa | Mais genérico que Chrono/MoorDyn — exigiria construir a especialização para riser/ancoragem por conta própria; maior custo de aprendizado do framework |
| **CalculiX** | GPL v2+ (copyleft) | Solver FEM geral (estilo Abaqus), vigas 2D/trusses, estático/dinâmico/térmico | Madura, ativa | Licença GPL é viral — se embutido, o produto inteiro precisaria ser GPL. Risco de licenciamento alto para um produto que pode ser fechado/comercial; incluído aqui só por completude |
| **Motor próprio (Eigen)** | N/A (código nosso) | Controle total, fidelidade exata ao ANFLEX/AML | O que já existe hoje (`risersim`) | Sem dependência de licença de terceiros, mas continuamos sozinhos resolvendo problemas já resolvidos por essas bibliotecas (ex. o bug de divergência atual) |

### Observações-chave

- **MAP++ é o candidato mais direto para o problema em aberto**: como ele resolve especificamente o equilíbrio quasi-estático de linhas multi-segmento, adotá-lo (mesmo que só para a fase estática, mantendo dinâmica própria ou do Chrono) contornaria o bug de divergência do elemento corrotacional sem precisar terminar de depurá-lo.
- **Project Chrono é o candidato mais abrangente**: cobre estático (equilíbrio via relaxamento/integração) e dinâmico, tem multibody completo (relevante para "linhas de ancoragem" conectadas a boias/embarcações), licença permissiva. Custo: formulação ANCF é diferente da corrotacional do ANFLEX real, então resultados não seriam numericamente idênticos ao ANFLEX legado (mas podem ser mais robustos).
- **MoorDyn-C é o mais "parecido" com o que já construímos à mão**: lumped-mass com solo/hidrodinâmica embutidos, mapeia quase 1:1 com `seabed.hpp`/`hydrodynamics.hpp` do risersim atual — mais fácil de avaliar rapidamente por familiaridade.
- **OpenSees e CalculiX têm risco de licença** que precisa ser resolvido antes de qualquer adoção (redistribuição/comercialização, GPL viral respectivamente) — não descartar de cara, mas não são "só copiar e usar" como Chrono/MAP++/MoorDyn/Kratos.
- Qualquer uma dessas bibliotecas ainda precisaria da camada própria já desenhada: leitura de `.aml`/`.xml+h5` do ANFLEX (`tools/aml_reader.py`/`tools/xml_h5_reader.py`, mantidos como estão), e a interface web de pré/pós-processamento e monitoramento (seção 3 abaixo, válida independente da escolha do núcleo físico).

### Próximos passos sugeridos (quando for retomar)

Validar 1-2 candidatos rodando o `Exemplo_01a` (ou um caso reduzido) em cada, comparando: (a) se o equilíbrio estático converge, (b) esforço de integração/build, (c) o quão perto o resultado fica do ANFLEX real.

### Resultado da validação (MoorPy, spike em `risersim/spikes/mooring_validation/`)

Executado nesta sessão dentro do ambiente Docker oficial do projeto. Scripts: `build_from_risersim_json.py` (converte `input_simulation.json` num `moorpy.System`), `run_moorpy_static.py` (resolve o equilíbrio e exporta o resultado), `compare_results.py` (compara com o resultado real do ANFLEX, lido diretamente de `exemplos/Curso/Exemplo_01/Exemplo_01a/Exemplo_01a_analysis/Exemplo_01a_A1_Cross_group1_results_static.h5`).

**Resultado: MoorPy converge de forma direta e rápida** no mesmo modelo (mesma geometria de 501 nós, mesmo material EA=360.000 kN / EI=21.700 N·m² / peso submerso 0,4395 kN/m, mesmo leito marinho a 265 m) onde o solver próprio do risersim diverge:

- Tração na âncora (fundo): 10,76 kN; tração no topo: 123,70 kN; peso total submerso da linha: 219,75 kN — ordem de grandeza fisicamente coerente.
- 44% dos pontos da linha ficam apoiados no leito marinho no resultado do MoorPy, contra **46%** dos 501 nós no resultado real convergido do ANFLEX (step_11) — concordância notável, dado que são duas formulações completamente diferentes (catenária analítica com contato vs. elementos finitos corrotacionais).
- **Achado de integração não-óbvio**: `moorpy.Line.staticSolve()` recalcula o peso submerso internamente a partir de `mass` (kg/m) e do diâmetro externo (`d_vol`), ignorando o `w` passado em `setLineType()`. É preciso "engenharia reversa" da massa (`mass = peso_submerso/g + π/4·d_vol²·ρ_água`) para reproduzir o peso submerso real já validado contra a documentação do curso — sem essa correção o MoorPy computa peso submerso **negativo** (linha boiante) e a solução diverge fisicamente (a linha "flutua" contra a superfície em vez de descansar no leito). Documentado em comentário no código (`build_from_risersim_json.py`).

**Refinamento: comparação nó-a-nó exata (`run_moorpy_static.py --exact-top`).** A primeira rodada fixou o topo na posição de referência lida do H5, sem o offset estático da FPSO que o ANFLEX real aplica nesse caso de carga ("Cross" — desloca o topo em ~ -0,9 m em X e -13,2 m em Y). Repetindo com o topo exatamente na posição absoluta final do ANFLEX (lida do próprio resultado do curso, via novo módulo `anflex_reference.py`) e interpolando a linha do MoorPy por comprimento de arco normalizado para comparar contra os 501 nós reais:

- **Erro médio: 4,07 m | RMS: 4,45 m | máximo: 7,37 m**, numa linha de ~500 m de comprimento e 265 m de lâmina d'água — cerca de 1% de erro relativo entre duas formulações fisicamente independentes.
- Fração apoiada no leito: 46% em ambos (19/41 no MoorPy, 231/501 no ANFLEX) — concordância exata depois da correção do topo.
- Confirmado tanto localmente quanto dentro do ambiente Docker oficial do projeto.

**MAP++ como segundo candidato — avaliado, não implementado.** Não existe pacote PyPI para MAP++/pyMAP (testado `pyMAP`, `map-plus-plus`, `pyMAP-fortran`, `nwtc-map`, `mappp` — nenhum corresponde à biblioteca real; `pyMAP` no PyPI é uma lib de e-mail IMAP não relacionada). A única via é compilar a partir do código-fonte (`github.com/WISDEM/pyMAP`, wrapper Python sobre a lib C++ do MAP++, via `setup.py`, exigindo um compilador C++ — sem LAPACK/Fortran, mais simples do que se temia inicialmente) — mas o repositório é antigo (era WISDEM/FAST ~2015-2018) com status de manutenção incerto, risco real de problemas de build por código legado. Dado que o MoorPy já validou a hipótese central com margem de erro pequena (~1%) e esforço de integração quase nulo, **a decisão foi não investir na build do MAP++ agora** — fica registrado como opção de segunda camada se o MoorPy se mostrar insuficiente para algum requisito futuro (ex. múltiplas linhas acopladas, o que o MAP++ modela nativamente e o MoorPy também suporta via `System` com múltiplos `Point`/`Line`, então essa lacuna pode nem existir).

**Conclusão prática**: a hipótese da seção 1 se confirma com dado quantitativo forte — um solver quasi-estático de terceiros (MoorPy) resolve exatamente o problema em que o risersim trava, com ~1% de erro em relação ao resultado real do ANFLEX e esforço de integração baixo (Python puro, ~250 linhas de código de conversão/execução/comparação no total, sem mudar nada no motor C++). Isso não decide a arquitetura final, mas é um dado concreto e quantificado a favor de considerar MoorPy como o solver do equilíbrio estático inicial num eventual sucessor do ANFLEX, mesmo que a dinâmica continue com outra abordagem (motor próprio, Chrono, ou MoorDyn-C).

### Tentativa de integração real: warm start MoorPy → solver estático do risersim (resultado negativo, mas revelador)

Com o MoorPy validado, tentamos a integração real: usar a geometria de equilíbrio do MoorPy como **chute inicial** (warm start) do solver Newton-Raphson próprio do risersim, em vez de rodar o MoorPy como substituto. Implementado em `risersim/tools/moorpy_warm_start.py` (gera uma seção `warm_start.node_positions` no JSON de entrada) e consumido por um novo bloco em `risersim/src/main_test.cpp` que ajusta `node->disp = warm_coords - node->coords` para cada nó (preservando `coords`/comprimento não-esticado, confirmado estruturalmente seguro por exploração prévia de `dynamic_analysis.cpp`/`node.hpp`).

**Resultado: não convergiu — e piorou.** Testado no Exemplo_01a real, tanto com o carregamento em 11 passos (9,1%→100%) quanto em 1 passo único (100% direto): o resíduo na iteração 0 do passo 1 saltou de ~1,3×10³ (sem warm start) para **~1,4×10⁷** (com warm start) — um salto de ~10.000x, idêntico entre as duas estratégias de carregamento (o que descarta rampa de carga como fator, mais uma vez).

**Hipótese mais provável (não confirmada por instrumentação adicional, por ora)**: o warm start só ajustou `disp` (translação) de cada nó, deixando `rot` (rotação nodal) em zero. Para um elemento de viga corrotacional de 6 GDL, a força interna depende fortemente da **consistência entre translação e rotação** — mover os nós para uma nova curva suave sem girar a orientação nodal para acompanhar a nova tangente cria curvatura/momento fletor espúrio muito maior do que a curvatura real da geometria. Isso é coerente com o salto de resíduo ser idêntico independente da fração de carga aplicada (ou seja, o resíduo em excesso vem de força interna/geométrica, não da carga externa).

**O que isso ensina**: reforça — com um mecanismo mais específico do que antes — que a fragilidade está na formulação/implementação do elemento corrotacional (`element_beam.cpp`) reagir de forma desproporcional a desalinhamentos entre translação e rotação, não só a "más estimativas de posição". Um warm start completo precisaria também estimar rotações nodais iniciais consistentes com a nova tangente da curva (não trivial — o MoorPy não fornece isso, seria preciso derivar da própria geometria) — o que começa a se aproximar de reimplementar parte da lógica que hoje só existe dentro do próprio Newton-Raphson.

**Decisão**: não perseguir a estimativa de rotações iniciais agora (esforço crescente para um resultado incerto). O warm start via MoorPy fica registrado como tentativa válida e documentada, mas **não adotado** como mitigação da divergência. Os dois caminhos que continuam de pé: (a) usar o MoorPy como solver estático **substituto** (não warm start) e alimentar `DynamicAnalysis` diretamente a partir dele — estruturalmente viável (confirmado: `DynamicAnalysis` não depende de `StaticAnalysis::solve()` ter rodado, só de `disp`/`rot` estarem povoados quando `solve()` é chamado), mas ainda deixaria `rot`=0 como estado inicial da dinâmica, com o mesmo risco; (b) retomar a investigação original do bug em `element_beam.cpp`, agora com uma pista mais específica (sensibilidade a desalinhamento translação/rotação) em vez de partir do zero.

## 2. Fadiga e "gêmeo digital" do riser

Uma das análises mais importantes para risers é a de fadiga — já estava no roadmap de física (fase de DNV/fadiga), mas como relatório pós-hoc de baixa prioridade. A ideia de um gêmeo digital (impor movimentos/dados medidos e simular o carregamento futuro do riser) muda essa prioridade: **é uma linha de pesquisa e produto comercial já estabelecida** na indústria offshore, não uma ideia nova.

### Panorama de mercado

**Comercial, todos fechados/proprietários:**
- **4Subsea (Noruega) — "Riser Monitoring Solution (RMS)"**: modelo híbrido continuamente alimentado por sensores instalados, com módulo de previsão opcional que estima vida remanescente combinando resposta medida do riser + estatística de clima. Implementado como modelo estrutural no **OrcaFlex**, rodando na plataforma 4insight.io (Azure). Tem casos reais em **FPSOs no Brasil** com foco em fadiga de riser.
- **Baker Hughes — "Online Fatigue Analysis"**: cálculo de dano à fadiga em tempo real de armaduras de tração de duto flexível, via API do OrcaFlex + algoritmo de análise local próprio.
- **2H Offshore — "Riser Fatigue Digital Twin"**: FEA refinada + rede neural (ANN) treinada sobre ela, para acelerar o cálculo o suficiente para rodar quase em tempo real.
- **RIMS (Riser Integrity Monitoring System)**: tecnologia integrada de monitoramento de longo prazo, captura pressão interna e movimento em tempo real e calcula dano à fadiga e vida remanescente automaticamente.
- Base comum: essas soluções são camadas de dados/previsão construídas **em cima** dos grandes softwares comerciais fechados de análise global de riser — **OrcaFlex** (Orcina), **RIFLEX**/**Helica** (DNV/Sesam), **Flexcom** (Wood/MCS Kenny), **Staris**. Nenhum desses quatro é open-source, e são as licenças mais caras desse mercado.

**Open-source**: não foi encontrado nenhum produto open-source integrado de "gêmeo digital de fadiga de riser" — é um nicho dominado por poucos players comerciais. As peças que comporiam um equivalente aberto já existem e são maduras:
- Contagem rainflow (ASTM E1049-85): `pawelkn/rainflow` (C++, header-only, zero dependências), `a-ma72/rainflow` (C99 com bindings Python/MATLAB), `fatpack` e `ffpack` (Python) — cobrem rainflow + regra de Miner + curvas S-N/Wöhler prontos.
- Lado estrutural: Project Chrono / MAP++ / MoorDyn-C (seção 1).
- Ninguém parece ter montado essas peças num pipeline aberto integrado — é uma lacuna real de mercado, não só tecnicamente possível.

**Empresas interessadas**: sem evidência concreta de uma empresa específica avaliando esta ideia hoje. O que dá para observar: o histórico do próprio ANFLEX (ferramenta brasileira, ligada a Petrobras/COPPE) e o fato de a 4Subsea já ter clientes de FPSO no Brasil usando uma solução estrangeira fechada sugerem que o público natural de uma alternativa aberta seria a Petrobras e suas contratadas de engenharia de risers — hipótese de mercado a validar diretamente, não confirmada.

### Como isso se encaixaria na arquitetura (extensão natural, sem redesenho)

- Generalizar a fonte de dados ambientais/de movimento para uma interface plugável (`IEnvironmentSource`, no mesmo espírito do `IModelReader` da seção 3): uma implementação sintética (JONSWAP/Airy, já existente) e uma implementação "replay" que lê série temporal medida (movimento de topo, corrente, eventualmente strain gauges/acelerômetros no riser) — o `DynamicAnalysis` roda igual nos dois casos.
- Um módulo de fadiga **contínuo/incremental**, não só um relatório pós-hoc: rainflow + S-N + Miner por janela de tempo, persistindo dano acumulado por elemento no `ResultsStore`.
- **Modo "replay/reconstrução"**: rodar o motor sobre dados históricos medidos para reconstituir o dano acumulado real até hoje.
- **Modo "previsão"**: rodar o mesmo motor + módulo de fadiga sobre cenários ambientais futuros esperados (sintéticos/estatísticos, ex. Monte Carlo de estados de mar prováveis) para projetar a vida remanescente.
- Tendência da indústria (2H Offshore) de usar um modelo surrogate/ANN treinado sobre a FEA para acelerar o cálculo — otimização futura, depois que o caminho direto (FE completo) estiver funcionando.

## 3. Arquitetura própria (motor C++ + web app), caso esse seja o caminho escolhido

Desenho feito antes da comparação de bibliotecas da seção 1 — continua válido como referência caso o caminho seja "motor próprio" ou um híbrido.

### Núcleo do motor C++ (redesign OOP)
- Hierarquia de elementos: `Element` base polimórfica (mais enxuta que o `cElement` legado do ANFLEX) + `BeamElement` como wrapper fino em volta da matemática corrotacional já validada do `CorotationalBeam3D` — sem alterar essa matemática.
- Camada de modelo: `Model` (evolução de `RiserModel`) + `IModelReader`/`JsonModelReader`, mantendo `aml_reader.py`/`xml_h5_reader.py` como únicos parsers de formato legado.
- Solver: manter `Analysis`/`StaticAnalysis`/`DynamicAnalysis`, com `ILinearSolver`/`IConvergenceCriterion` plugáveis.
- Resultados: `ResultsStore` com escrita incremental em HDF5; módulos `results/fatigue`/`results/dnv_checks` pós-hoc.
- Seam de monitoramento: interface `SimulationObserver` (inspirada no `sStepInfo`/`cStatusWatcher` do ANFLEX real, sem a dependência de UI IUP) — `ProgressLogObserver` (fase 1, sem rede) e `StreamingObserver` (fase 3, via **oatpp** — Apache 2.0, leve, sem trazer Boost inteiro).

### Web app de pré/pós-processamento
- React + Vite + TypeScript; Three.js (evoluindo o `tools/viewer_3d.html` atual) num componente fino; Plotly mantido; h5wasm mantido; UI kit Mantine; estado com Zustand.
- Um app, três rotas: `/model`, `/run`, `/results/:runId`.
- Comunicação motor↔web em fases: fase 1 = arquivo (`progress.json`, CLI batch); fase 3 = backend local opcional com oatpp (HTTP+WebSocket), mesma rota `/run` trocando polling por streaming atrás de um tipo compartilhado.

### Roadmap faseado
1. Fundação OOP do motor (comportamento preservado, validado contra os exemplos existentes)
2. Web app simples de pré/pós (arquivo, sem backend)
3. Monitoramento ao vivo (oatpp + streaming)
4. Profundidade física/resultados (onda real, perfil de corrente completo, solo P-Y/T-Z, DNV/fadiga, modal, elementos rígidos/boia, parser `.dat` nativo) — à luz da seção 2, vale subir fadiga/DNV para logo após onda real, em vez de deixá-la por último

## 4. Confirmação com a arquitetura real do ANFLEX (`trunk/interfaces/src`)

`trunk/interfaces/src` (~662 arquivos) é o código real da interface de pré/pós-processamento do ANFLEX (GUI legada em IUP + Lua/tolua, sem CMake, só `tecmake` `.mak`). Investigação do código confirmou como o modelo é montado e como os dados são exportados, validando e refinando o design da seção 3:

- **Modelo em memória**: classe central `cAppModel` (`anfmodel.h`) — não é uma malha, é um conjunto de coleções tipadas (`colMaterial`, `colLine`, `colBuoy`, `colSoil`, `colCurrent`, `colWave`, `colNode`, `colAnalysis`, etc.), com objetos se referenciando via wrappers tipados `cObjRef<T>` em vez de cópias. Confirma o padrão já proposto (`Model` como agregado + builder), só que o real usa "coleção por conceito de domínio" em vez de um único container de nós/elementos.
- **Geração de malha**: cada tipo de objeto gera sua própria malha FE, não um assembler central: `cLine::create_fe_mesh` delega para um solver de catenária/geometria externo ("TecLine"); `cBargrid::create_fe_mesh` subdivide um grafo de vértices/arestas em elementos de barra lineares (mais simples); `cBuoy` gera sua própria malha curta. `assembly.cpp`/`cb-assembly.cpp` (que pareciam ser sobre "montagem de malha" pelo nome) são na verdade só a configuração de convergência do estágio de equilíbrio estático (`%ASSEMBLY.CRITERIUM/TOTAL_TIME/MAX_ITERATION/ERROR_TOLERANCE` etc.) — o `StaticAnalysis`/tolerâncias do risersim já é o equivalente direto disso.
- **Exportação de dados — confirma a origem exata dos formatos já lidos**:
  - `.aml`/`.pml` (texto, tags `%KEYWORD`) — escrito por `ioSaveProject`/`anfout.cpp`, lido de volta por `aml.cpp` (`cAml`). É exatamente o formato que `aml_reader.py` já lê.
  - `.dat` + includes (`.ppf/.gpf/.hpf/.rpf`) **ou** `.xml`+`.h5` — gerados por `cSaveDatFile` (`save-dat.cpp`, ~12.500 linhas), a partir de `export.cpp`. O modo `save_xml()` monta uma árvore `nAnfStorage::cWriterNode` e escreve os arrays numéricos pesados via HDF5 — exatamente o par XML+H5 que `xml_h5_reader.py` já lê.
  - Pós-processamento (`anfpos.cpp`) lê os resultados de volta pela mesma biblioteca `nAnfStorage`, fechando o ciclo: pré-processador escreve modelo+config → solver roda → pós-processador lê resultados com a mesma lib de storage.
- **Conclusão prática**: os leitores Python (`aml_reader.py`/`xml_h5_reader.py`) já mapeiam corretamente para o lado de escrita real — não há surpresa estrutural, o que valida mantê-los como a "fonte da verdade" de conversão de formato legado. O padrão "cada tipo de objeto gera sua própria malha" (`cLine`/`cBargrid`/`cBuoy`) é uma referência útil para o desenho do `Element`/`Model`/`ModelBuilder` do sucessor: talvez faça mais sentido um `IMeshGenerator` por tipo de linha/estrutura em vez de um único builder genérico.

## 5. Investigação anterior em aberto (bug de divergência do solver estático)

A investigação da divergência do solver estático no `Exemplo_01a` real (501 nós, elemento de viga corrotacional) segue pausada, sem solução. Resumo para quando for retomada:
- **Descartado como causa**: carregamento mais fino, contato com o solo (testado desligado), modelo de solo, atrito, escolha do eixo local do elemento.
- **Pista não confirmada**: escala da rigidez artificial (EA/L vs 12EI/L³) afeta *quando* diverge, não *se* diverge.
- **Conclusão**: causa isolada no comportamento do próprio elemento de viga corrotacional (`element_beam.cpp`) para malha fina (500 elementos de 1m) e flexível (EI=21700 N·m²).
- **Próximo passo recomendado**: isolar o elemento corrotacional num teste mínimo (2-3 elementos), ou — à luz da seção 1 — considerar validar o mesmo caso no MAP++ (equilíbrio estático) como atalho para contornar o bug em vez de depurá-lo.
