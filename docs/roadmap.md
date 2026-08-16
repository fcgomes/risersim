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

### 1b. Investigar a análise dinâmica — ✅ RESOLVIDO (Exemplo_01a converge os 20 passos completos)

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

**Atualização 7** (fechado -- os 20 passos completos convergem, pela primeira vez em toda essa
investigação): segui o caminho (a) acima -- plano completo aprovado pelo usuário, comparando o
tratamento de superfície livre do risersim contra o ANFLEX real (`trunk/src/nl_hidrostatic.{h,cpp}`,
`trunk/src/morison.cpp`) pra achar o gatilho exato do passo 15 (t=0,75s). Achados e correções, em
ordem:

- **Empuxo constante sem rigidez tangente** (gap real, confirmado por comparação de código): o
  risersim aplicava empuxo constante por elemento, independente de Z, em 4 lugares duplicados
  (`static_integrator.cpp`, `static_analysis.cpp` ×2, `dynamic_analysis.cpp`) -- sem nenhuma rigidez
  associada. Portei `cNL_Hidrostatics` quase 1:1 como `hydrostatics.hpp` (força + rigidez tangente
  por extremidade, 5 regimes de submersão) e pluguei em todos os 4 call-sites de força mais um novo
  `Analysis::assemble_buoyancy_stiffness()` (rigidez, somada em `K_global` nos loops de Newton
  estático e dinâmico, mesmo padrão do `C_global = alpha*M + beta*K`). Simplificação assumida (fase
  1 do plano, documentada no header): superfície plana (`water_surface_z` único, não por-extremidade
  variando com a onda) e clearance vertical de centro-de-linha em vez da distância perpendicular ao
  eixo inclinado que o ANFLEX real usa. Melhoria real e validada (zero regressão, 361 asserts), mas
  **não era a causa da divergência do passo 15** -- ver próximo item.
- **Erro meu de diagnóstico, corrigido antes de seguir**: assumi a convenção Z do risersim sintético
  (superfície em Z=0) pra interpretar a trajetória do nó 274. Depois de implementar e testar o fix de
  empuxo acima e ver resíduo bit-a-bit idêntico ao baseline, chequei os dados reais do modelo:
  `water_surface_z≈265`, `seabed_depth≈0` -- convenção oposta (a nativa do ANFLEX, que é a real pra
  qualquer modelo vindo de XML/H5). O nó 274 estava cruzando o **leito**, não a superfície. Reportei
  o erro ao usuário antes de continuar; o fix de empuxo foi mantido (correto e validado por conta
  própria), só reclassificado como não sendo a causa deste bug específico.
- **Causa raiz real** (isolada com instrumentação de debug temporária, depois removida): no passo 15,
  uma correção de Newton sem limite algum, calculada enquanto um nó ainda estava ~1,75cm FORA do
  contato com o leito (`k_seabed=0` ali -- sem resistência local nenhuma pro Newton), empurrou esse nó
  1,6m através do leito numa única iteração. A resposta de força interna elementar/estrutural
  resultante (não a mola do leito em si) explode o resíduo. É um problema de globalização/tamanho de
  passo do Newton, comum a qualquer modelo simples de contato unilateral -- o próprio ANFLEX real tem
  a mesma descontinuidade de rigidez de contato na mola linear dele (confirmado via
  `newton_raphson.cpp`: sem adaptação de `dt`/retry nenhuma) -- ou seja, **não é um gap de física
  faltando em relação ao ANFLEX**, é um problema numérico do solver.
- **Correção**: portei pro loop dinâmico o mesmo mecanismo já existente no estático
  (`StaticAnalysis::enable_step_limiting`/`apply_newton_step_with_line_search`) -- (1) limitador de
  passo (`max_translation_step_m=0.5`, `max_rotation_step_rad=0.3`) e (2) busca de linha por
  backtracking residual, até 5 cortes pela metade. A tolerância do backtracking teve que ser mais
  apertada que a do estático: `norm_trial <= res_norm*100.0` (a mesma do estático) testada e
  confirmada **ineficaz** aqui -- deixava passar exatamente o passo que explode; `norm_trial <=
  res_norm*2.0` testado e confirmado suficiente. Refatorei o loop de Newton dinâmico em torno de uma
  lambda `assemble_at(U_trial)` reutilizável (monta F_ext/K_global/F_int a partir de um vetor de
  estado absoluto, não incremental -- diferente do estático, não precisa snapshot/restore entre
  tentativas de backtracking).
- **Instrução explícita do usuário, implementada**: "não faz sentido progredir a simulação se temos
  um passo que falha" -- `stop_on_first_non_convergence` (`DynamicAnalysis`/`AnalysisOptionsConfig`)
  mudou o default de `false` pra `true`: um passo não-convergido pára o laço de tempo inteiro em vez
  de continuar rodando passos seguintes sobre um estado fisicamente sem sentido.

**Resultado final**: rodando o `Exemplo_01a` completo e real (XML+H5 reais, config real do ANFLEX --
20 iterações, 1s de duração, dt=0,05s, tolerância 1e-3, sem nenhum afrouxamento artificial), estática
E dinâmica convergem os 20 passos completos pela primeira vez em toda essa investigação. Catch2 361
asserts / 15 casos, zero regressão, verificado a cada incremento.

**Em aberto, não decidido ainda**: as fases 4 (força de Morison, já escrita em `hydrodynamics.hpp`
mas nunca instanciada) e 5 (massa adicionada escalando com comprimento molhado) do plano original
ficaram pendentes -- eram contingentes no checkpoint da fase 3, que seguiu por outro caminho (leito/
limitador de passo/backtracking) em vez do previsto. Como a divergência que motivou o plano já está
resolvida, vale decidir com o usuário se ainda compensa perseguir Morison/massa molhada agora (physics
gap real, mas não bloqueante) ou se o Eixo 1b deve ser considerado fechado por ora.

## Eixo 2 — Pipeline de dados (desbloqueia mais casos de teste reais, baixo risco)

### 2a. Religar `aml_reader.py` ao schema real do `ModelBuilder` — 🟡 EM PROGRESSO

Achado em [`mapa_aml_exemplos_e_web_interface.md`](mapa_aml_exemplos_e_web_interface.md): hoje, 23
dos 30 exemplos disponíveis (todos sem pasta `_analysis/` com XML/H5 pré-exportado) rodam
silenciosamente com o modelo sintético de fallback — `aml_reader.py` produz um schema que
`ModelBuilder` nunca entende. Consertar isso (gerar `model.nodes`/`model.elements` a partir dos
segmentos já extraídos, resolver corrente/solo por ID como `xml_h5_reader.py` já faz) é um trabalho
bem-escopado e de baixo risco (não toca o C++, só o parser Python), e destrava candidatos reais
novos ao bug solo+corrente — em especial `DNV_Check.aml` (solo e corrente confirmados casando por
ID, caminho estático).

**✅ Resolvido**: `to_risersim_json()` agora gera o schema real (nós/elementos/seção sintetizados a
partir dos segmentos), com corrente/solo/onda resolvidos por ID real (não mais "primeiro do
arquivo"). Um AML pode ter vários `%LOAD_CASE` (ex. Near/Far/Transverse/Cross do `Exemplo_01a`),
cada um com sua própria corrente+onda — `_resolve_load_case(load_case_id)` seleciona pelo ID real
(`--load-case-id` no `run_from_aml.py`), com transparência (nome/ID resolvido impresso). Como só
"Cross" tem XML+H5 real exportado neste exemplo, `--force-aml-path` força o caminho `.aml` puro
(malha sintética) mesmo quando existe uma pasta `_analysis/`, necessário pra rodar os outros load
cases (antes disso, `--load-case-id` era silenciosamente ignorado sempre que havia XML+H5 --
descoberto rodando Near/Far/Transverse pela primeira vez).

**Atualização 1** (movimento real de topo RAO+JONSWAP, ligado no caminho `.aml` puro): até aqui,
`vessel_motion` (o mecanismo real de topo já portado e validado pro caminho XML+H5,
`vessel_motion.hpp`) nunca era populado por `aml_reader.py` -- fallback sempre em onda regular só
em Z. Pesquisei o pré-processador real (`trunk/interfaces/src`, não `trunk/src`/`trunk/libs` --
esses só consomem o `.dat`/XML já traduzido, o parser de texto `%KEYWORD` só existe no
pré-processador) pra portar: posição global do ponto de conexão (`%CONNECTION.LOCAL_COORDINATES` +
`%FLOATING.SHIP`, rotação `90-azimute` + translação pela origem do FPSO --
`connection.cpp:189-281`), offset estático real (`%FLOATING.LOADS.STATIC`'s `%TIME_SERIES_LOAD`,
avaliado num parser genérico de tabela de função no tempo real da análise estática), e a tabela RAO
**embutida** no próprio `.aml` (não é arquivo externo -- `%RAO 'FPSO.RAO'` é só um rótulo, a tabela
inteira vem inline, formato confirmado contra `interfaces/src/rao.cpp`). Validação forte: a fórmula
da posição da conexão, calculada à mão antes de escrever código, bateu **exatamente** com o
`[TOP] X=-47.73 m, Z=257 m` já observado no caso real "Cross" (XML+H5).

**Atualização 2** (malha inicial via catenária real, usando MoorPy): o `.aml` não tem coordenadas de
nó, mas tem os parâmetros de um problema de contorno de catenária real
(`%LINE.CATENARY.ANGLE`+`%LINE.AZIMUTH`+comprimento+profundidade) -- o pré-processador real resolve
isso com uma biblioteca externa fechada (`tec_line`, não está neste repo). `moorpy.Catenary.catenary(
XF,ZF,L,EA,W)` (já usado e validado em `spikes/mooring_validation/`, ~1% de erro vs. ANFLEX real) só
resolve "dado o vão, qual a tração" -- não tem modo "dado o ângulo, ache o vão". Implementei
`_solve_catenary_geometry()`: usa `catenary()` como solver interno de uma busca (bisseção) no vão
horizontal até bater o ângulo real no topo. **Validação forte**: a âncora prevista bateu com a âncora
real exportada (XML+H5, caso "Cross") a nível de **centímetro** num vão de ~296m
(`(-271.44,-248.97,0.00)` previsto vs. `(-271.40,-248.94,~0)` real).

**Bug real achado e corrigido durante a validação**: a fórmula de `movement_center` (Atualização 1)
subtraía a posição do nó de topo direto; o certo é subtrair o **gap** entre a posição real da conexão
e onde o nó 1 efetivamente está (`connection_global - top_node_position`), não `top_node_position`
isolado -- sem isso, ao mudar o nó de topo pra posição real (Atualização 2), o braço de alavanca
dobrava em vez de cancelar. Pego checando os amplitudes impressos (100-762m de heave, fisicamente
absurdo), não só "convergiu com sucesso" -- convergência sozinha não bastava pra pegar esse bug.

**Atualização 3** (causa raiz do "Far mostra amplitude muito maior" da lista acima, achada e
corrigida): não era ressonância real nem bug de busca de frequência -- era falta de conversão de
unidade. O ANFLEX real (`model_builder_dat.cpp:242-250`) converte a **amplitude** (não só a fase)
dos GDL rotacionais da RAO (roll/pitch/yaw) de graus/m pra radiano/m antes de usar; `vessel_motion.cpp`
convertia a fase mas nunca a amplitude. Como os momentos espectrais (`m0` etc.) envolvem
amplitude², o erro (~57x, `180/π`) compõe quadraticamente -- e é pior justamente quando o pico do
espectro de onda coincide com um pico de RAO rotacional, o que acontece quase exatamente para "Far"
(ambos perto de ω≈0,40 rad/s), explicando por que só esse caso mostrava números absurdos. Confirmado
comparando contra o `.SAI` real gerado rodando o Fortran legado via WSL (`anf_i`/`anf_s`/`anf_d`,
binários Linux em `anf_analysis/fortran/bin/`) para o caso Far do `Exemplo_01c`: a tabela real
"RESPONSE AMPLITUDE OPERATOR (TRANSFERRED)" do `.SAI` está explicitamente em unidades **"(M/M,
RAD/M)"**, com valores pequenos (~0,049 rad/m) muito diferentes do valor bruto do arquivo `.RAO`
(graus/m, ~6,77). Corrigido em `interpolate_heading()` (`vessel_motion.cpp`): amplitude dos GDL
rotacionais (`dof>=3`) agora escalada por `π/180` antes de montar a curva complexa usada na
transferência de ponto e nos momentos espectrais. Um teste (`test_vessel_motion.cpp`, o caso do
braço de alavanca pitch→heave) precisou de ajuste porque seu RAO sintético assumia amplitude=1.0
como "1 rad" direto -- reescalado por `180/π` no próprio teste pra preservar a mesma intenção.
Suíte: 361/361 sem regressão.

**Achado de processo, sem relação com o bug acima** (pego só ao rodar o pipeline real de ponta a
ponta, não pela suíte Catch2): depois do fix e da suíte passando, rodar `run_from_aml.py` no caso
Far pela linha de comando continuava mostrando o heave de ~1400m antigo. Causa: `risersim_test_main.exe`
(o binário que `run_from_aml.py` de fato invoca) não tinha sido recompilado depois do fix em
`vessel_motion.cpp` -- só `risersim_tests.exe` (a suíte) tinha sido reconstruído nesta sessão. Ou
seja, a suíte verde mascarava um binário de produção desatualizado. Recompilado via `MSBuild` da
instalação "Visual Studio 18" (não a "2022" -- o projeto usa toolset `v145`, que só a instalação 18
tem). Depois do rebuild, Far real: heave 1381m → **26,9m**, roll 19,1 rad (absurdo, >2π) → **0,33
rad**, surge → **3,49m** -- ordem de grandeza correta, ~51x de melhora, batendo com o que o harness
Python standalone já tinha indicado antes do fix chegar no binário real. **Lição**: ao validar um
fix de C++ pelo pipeline real (não só pela suíte de testes), confirmar a data de modificação do
binário específico invocado, não assumir que "a suíte passou" implica "o binário usado pelo restante
do sistema já reflete o fix".

**Atualização 4** (causa raiz do gap residual de ~3x e da não-convergência dinâmica do Far, ambos
achados e corrigidos -- investigação por comparação direta, ponto a ponto, contra as tabelas reais
"RESPONSE AMPLITUDE OPERATOR (TRANSFERRED)" e "LOCAL DISTANCE (USED FOR TRANSFERENCE)" de um `.SAI`
real, geradas rodando o Fortran legado via WSL tanto pro Far quanto pro Cross do `Exemplo_01c`):
dois bugs genuínos e independentes na transferência geométrica CM→ponto de fixação de
`interpolate_heading()`/construtor de `VesselMotion`, nenhum dos dois relacionado ao bug de
graus→radianos da Atualização 3.

1. **`offset` não deveria entrar nesta conta.** O código somava `config.offset_m` a
   `cm_position_m` antes de calcular o braço de alavanca, por analogia direta com
   `model_builder_dat.cpp:4373-4386` (que de fato faz `cm_position[i] += cm_offset[i]`) -- mas essa
   leitura ignorava que, no MESMO trecho real (`model_builder_dat.cpp:4335-4359`), o
   `node_position` do OUTRO lado da subtração recebe o EXATO MESMO offset somado
   (`node_position[i] += offset_coord[i] + offset_coord_assembly[i]`) antes de entrar na mesma
   conta -- no real, o offset é somado nos dois lados do delta e cancela. No pipeline do risersim,
   `attachment_point_global` (a posição real pós-estática do nó de topo) não carrega esse offset da
   mesma forma (é idêntica em todos os load cases), então somar offset só do lado do CM introduzia
   um desbalanceamento que o real nunca tem.
2. **A rotação usava o ângulo com o sinal trocado.** A fórmula em si já batia com
   `cMatrixTransform::inv_transform` (`matrix_transform.cpp:248-259`, aplica R(-theta), a
   transposta de `set_z_matrix`) -- o problema era o `theta` usado: `refsys_angle_deg` (105° pro
   Exemplo_01a) é o valor já validado contra o campo `refsys_angle` exportado no XML real, mas o
   `m_ref_sys_angle`/`floating_angle` que `cAnfMovements`/`cHybridMovement` de fato passam pra
   `inv_transform` internamente (`anf_movements.cpp:66-84`, `hybrid_movement.cpp:59`) tem o sinal
   OPOSTO a esse valor exportado. Como `cEquivalentHarmonic` repassa esse mesmo `floating_angle`
   sem modificar tanto pra escolher a direção da RAO quanto pra girar o braço de alavanca
   (`equivalent_harmonic.cpp:45-54`), a correção precisou negar o ângulo nos dois lugares de uma
   vez (não só na rotação).

Os dois bugs juntos foram achados comparando `x_local`/`y_local` calculados (antes: variavam por
load case, sem bater com nada reconhecível) contra a "LOCAL DISTANCE" real, que é **idêntica nos
dois load cases** (Far e Cross) e bate byte-a-byte com as `%CONNECTION.LOCAL_COORDINATES` cruas do
próprio `.aml`: `(65.0000, -32.0000, -8.0000)`. Corrigido: `cm_global` não soma mais `offset_m`;
`refsys_angle_rad_` agora guarda o `floating_angle` já negado (`-config.refsys_angle_deg`), usado
consistentemente nos três pontos do arquivo que giram entre o referencial local da RAO e o global
(seleção de heading, braço de alavanca no construtor, e a rotação final local→global em
`get_motion()`). Suíte: 361/361 sem regressão (o teste do braço de alavanca usa `refsys_angle_deg
=0`, insensível ao sinal).

**Resultado, comparando contra as tabelas reais "JOINT MOVEMENTS" (STD final por GDL) do `.SAI`**:

| GDL (unidade) | Cross real | Cross calculado (razão) | Far real | Far calculado (razão) |
|---|---|---|---|---|
| surge (m) | 0,3139 | 0,3149 (1,00x) | 2,567 | 2,57 (1,00x) |
| sway (m) | 0,19502 | 0,1879 (0,96x) | 1,8743 | 1,867 (1,00x) |
| heave (m) | 0,97067 | 0,9458 (0,97x) | 9,2905 | 7,96 (0,86x) |
| roll (°) | 0,37943 | 0,3584 (0,94x) | 9,7258 | 7,821 (0,80x) |
| pitch (°) | 1,0613 | 1,0307 (0,97x) | 3,2506 | 3,210 (0,99x) |
| yaw (°) | 0,1270 | 0,1233 (0,97x) | 1,1368 | 1,111 (0,98x) |

Cross agora bate em todos os 6 GDL dentro de ~3-6%; Far bate muito melhor que antes em surge/sway/
pitch/yaw (~1-2%), com heave/roll ainda ~15-20% subestimados (razão não uniforme entre GDL, gap bem
menor que o ~3x/~2x anterior, causa não identificada). **Efeito colateral direto**: a dinâmica do
Far, que antes parava de convergir em t≈9,65s mesmo com a amplitude já saneada pela Atualização 3,
agora **converge os 60s/1200 passos completos** sem nenhuma mudança no solver -- confirma que o
gap residual (heave ~27m, ~3x acima do real) era agressivo o bastante pra ainda quebrar o Newton
dinâmico. **Transverse (load case 135), que divergia na dinâmica (passo 4/t=0,2s) segundo o achado
anterior desta seção, também converge limpo agora** -- mesmo efeito colateral, sem investigação
separada necessária.

**Achado de processo, sem relação com os bugs acima** (pego só ao rodar o pipeline real de ponta a
ponta, não pela suíte Catch2): depois do fix da Atualização 3 e da suíte passando, rodar
`run_from_aml.py` no caso Far pela linha de comando continuava mostrando o heave de ~1400m antigo.
Causa: `risersim_test_main.exe` (o binário que `run_from_aml.py` de fato invoca) não tinha sido
recompilado depois do fix em `vessel_motion.cpp` -- só `risersim_tests.exe` (a suíte) tinha sido
reconstruído nesta sessão. Ou seja, a suíte verde mascarava um binário de produção desatualizado.
Recompilado via `MSBuild` da instalação "Visual Studio 18" (não a "2022" -- o projeto usa toolset
`v145`, que só a instalação 18 tem). Depois do rebuild, Far real: heave 1381m → **26,9m**, roll
19,1 rad (absurdo, >2π) → **0,33 rad**, surge → **3,49m** -- ordem de grandeza correta, ~51x de
melhora, batendo com o que o harness Python standalone já tinha indicado antes do fix chegar no
binário real. **Lição**: ao validar um fix de C++ pelo pipeline real (não só pela suíte de testes),
confirmar a data de modificação do binário específico invocado, não assumir que "a suíte passou"
implica "o binário usado pelo restante do sistema já reflete o fix".

**Em aberto, não resolvido ainda**:
- **Gap residual de ~15-20% no heave/roll do Far** (ver tabela acima) -- razão não uniforme entre
  GDL (surge/sway/pitch/yaw batem em ~1-2%), então não parece ser um terceiro bug de escala; pode
  ser o mesmo ~1% de diferença de grid já documentado (`dw=(wf-wi)/n` vs `/(n-1)`) composto de
  forma não-linear onde a RAO tem um pico mais agudo (roll do Far, ver
  `mapa_aml_exemplos_e_web_interface.md`), ou outra coisa ainda não identificada. Testado
  convergência de `nwave` (100→20000, já usando o código corrigido desta rodada): resultado já
  está convergido em ~500, então NÃO é falta de resolução do grid JONSWAP -- descarta essa
  hipótese especificamente.
  **Tentativa feita e revertida**: reli `movext.f:159-198` (a fórmula real do extremo de
  Rayleigh) esperando achar um terceiro bug de escala -- `STDPM=sqrt(AREAMOV/2)` (não
  `sqrt(AREAMOV)` puro) e `FMAX=sqrt(2*ln(TINTV*XFZ))` com `XFZ` baseado em m0/m2 (não m2/m4).
  Reescrevi `vessel_motion.cpp` pra bater literalmente com isso -- resultado **piorou**: as
  razões (antes 0,76-1,25 no Far, 0,94-1,00 no Cross) viraram uniformemente ~0,68-0,71 nos dois
  casos (~1/√2, todos os 6 GDL igualmente subestimados). Como a versão anterior já estava muito
  mais perto do real (principalmente no Cross), revertido (`git checkout`). A uniformidade exata
  do erro introduzido (1/√2 em todos os GDL) sugere que a leitura de `movext.f` tem um fator de 2
  errado em algum lugar -- provavelmente `AREAMOV`/`XM0` (o "m0" do Fortran, de `movgharfloa.f`,
  ainda não lido nesta rodada) não é a mesma grandeza que o `m0` deste código calcula por
  trapézio, e por isso o "/2" de `STDPM=sqrt(AREAMOV/2)` não se aplica do mesmo jeito aqui. Não
  investigado mais a fundo -- próxima tentativa, se valer a pena, deveria ler `movgharfloa.f`
  primeiro pra confirmar a definição exata de `AREAMOV` antes de mexer na fórmula de novo.

**Mudança de metodologia (achado nesta rodada, antes de continuar o gap acima)**: `trunk/anf_analysis`
contém o C++ REAL do ANFLEX (histórico SVN próprio, `$Revision$`/`$LastChangedBy$` nos headers,
last-changed 2015-2016) -- não é um wrapper fino sobre o Fortran, é uma reimplementação C++ completa
e ativamente mantida, em duas árvores: `anf_analysis/src` (motor: `domain.cpp`, `dynamic_analysis.cpp`,
`static_integrator.cpp`, `dynamic_integrator.cpp`, etc., praticamente os mesmos nomes de arquivo já
citados via `trunk/src` nesta investigação inteira) e `anf_analysis/anf_movements/src` (biblioteca de
movimento de topo/RAO/onda: `hybrid_movement.cpp`, `equivalent_harmonic.cpp`, `jonswap_spectrum.cpp`,
`rao.cpp`, `matrix_transform.cpp`). `trunk/src` (usado até aqui) é uma cópia mais antiga (datada de
2016-10-03 vs. 2016-11-10 do `anf_analysis/src`) com os MESMOS nomes de arquivo mas conteúdo diferente
-- funcionalmente equivalente pro que já foi lido até agora, mas `anf_analysis` é a árvore mais
completa e mais nova. **Decisão do usuário**: priorizar o C++ de `anf_analysis` daqui pra frente;
Fortran (`anf_analysis/fortran/*.f`) só quando o C++ não tiver a rotina ou o caso for complexo demais
pra entender só pelo C++.

**Atualização 8** (aplicando a mudança acima ao gap do Far -- achado inicial promissor, mas
descartado depois de implementado e testado, ver abaixo):
lendo `anf_analysis/anf_movements/src/hybrid_movement.cpp:100-136` (o construtor de `cHybridMovement`,
onde os momentos espectrais e o extremo de Rayleigh são calculados de fato) em vez do Fortran: a
fórmula em si (`m_amp_std=sqrt(m0)`, `m_amp_s=2*m_amp_std`, `Tm=2π·sqrt(m2/m4)`,
`N=t/Tm`, `rayleigh_factor=sqrt(0.5·ln(N))`, `amp_max=amp_s·rayleigh_factor`) bate **exatamente** com
o que `vessel_motion.cpp:240-256` já calcula hoje -- confirma, com a fonte real (não mais inferência
via Fortran), que a versão atual (pós-reversão da Atualização 4) estava certa e a tentativa revertida
(acima) é que introduzia o erro 1/√2. Mas o construtor real tem uma diferença estrutural que
`vessel_motion.cpp` NÃO replica: `hybrid_movement.cpp:69-81` restringe o laço de componentes de onda
usado no cálculo dos momentos (m0/m2/m4) ao intervalo `[rao_min, rao_max]` -- a faixa de frequência
REALMENTE tabelada na RAO (`m_rao_table->get_min_fre()/get_max_fre()`), descartando qualquer
componente de onda fora dela. `vessel_motion.cpp:215-238` integra sobre a faixa CONFIGURADA inteira
(`%WAVE.FIRST_FREQUENCY`/`LAST_FREQUENCY`, 0,2-3,0 rad/s no Exemplo_01a) e usa `interp_linear()`
(`vessel_motion.cpp:118-130`), que EXTRAPOLA como constante (`y.front()`/`y.back()`) fora da faixa da
RAO, em vez de excluir esses pontos. Conferido contra o `.aml` real: a tabela RAO do Exemplo_01a vai
só até **1,20 rad/s** (`exemplos/Curso/Exemplo_01/Exemplo_01a/Exemplo_01a.aml:756-760`), bem abaixo do
teto de integração configurado (3,0 rad/s) -- ou seja, hoje incluímos ~60% do domínio de integração
(1,2 a 3,0 rad/s) com um valor de RAO extrapolado (constante) que o código real simplesmente não usa
ali. Como `m4` pesa por `ω⁴`, essa cauda espúria pesa desproporcionalmente mais nela do que em `m0`,
e como o efeito depende de quão achatada/alta é a última amostra tabelada de cada GDL (heave/roll do
Far, notados como tendo um pico mais agudo -- ver achado anterior nesta seção), é consistente com o
gap ser não-uniforme entre GDL e maior no Far.

**Implementado e testado -- hipótese DESCARTADA, gap continua aberto**: clipei o laço de
`vessel_motion.cpp` à faixa `[frequencies_rad_s.front(), frequencies_rad_s.back()]` da própria RAO
(`mov_ini`/`mov_fin`, mesmo algoritmo de `hybrid_movement.cpp:69-81`, inclusive o mesmo off-by-one de
fronteira quando um ponto da grade de onda cai EXATAMENTE no limite da RAO -- exigiu ajustar a
referência independente de `test_vessel_motion.cpp` pra bater; suíte 361/361 sem regressão).
Recompilei `risersim_test_main.exe` (não só a suíte) e rodei o Far real com um debug print temporário
(`RISERSIM_VESSEL_MOTION_DEBUG`, removido depois) pra imprimir os 6 GDL antes/depois do fix.
**Resultado: byte-idêntico ao valor pré-fix** (surge=2,57 sway=1,867 heave=7,96 roll=7,82° pitch=3,21°
yaw=1,111° -- mesmos números da tabela da Atualização 4). Causa: minha leitura anterior do `.aml` só
cobriu as primeiras ~48 das 59 linhas de frequência da tabela RAO (`Exemplo_01a.aml:712`, cabeçalho
"25 59") -- a tabela real do heading 0° vai até **10,0 rad/s**, bem além do teto de integração
JONSWAP configurado (3,0 rad/s), confirmado lendo `frequencies_rad_s` de volta do
`input_simulation.json` gerado (min 0,0628, max **10,0**, não 1,2 como eu tinha lido antes). Ou seja,
a RAO tabelada é mais LARGA que o domínio de integração, não mais estreita -- o clip nunca exclui
nada na prática pra este caso real, e por isso o fix não muda o resultado. **Mantido mesmo assim**
(código correto, fiel ao algoritmo real, zero regressão, zero mudança de comportamento nos casos já
validados) -- só não é a causa do gap. Gap do heave/roll do Far **continua sem causa identificada**;
próxima hipótese, se valer a pena perseguir, precisa ser outra (a suspeita de grid JONSWAP já foi
descartada antes; frequência de corte da RAO agora também descartada com evidência direta).

**Atualização 5** (causa raiz do `T_eff` estático do "Cross" via `.aml` puro, ~994 kN vs. 217 kN
real, achada e corrigida -- ✅ **RESOLVIDO**): não era o formato do chute inicial por si só (testado
diretamente: rodar com o `warm_start` já existente, aplicado como `disp` por cima da malha reta
-- `model_builder.cpp:200-221` -- deu o MESMO `T_eff`, 994,2 kN, provando que a forma inicial não
era a causa). A causa real: `model_builder.cpp:149` deriva `L_unstretched` (comprimento natural/
não-esticado de cada elemento, usado no cálculo de deformação axial) da distância EUCLIDIANA entre
as coordenadas de entrada dos nós -- correto quando a malha de entrada já é a geometria real
instalada (caminho XML+H5), mas `_synthesize_mesh()` (caminho `.aml` puro) coloca os nós numa
RETA entre o topo e a âncora reais, e uma corda reta é sempre mais curta que o cabo real (que tem
sag) -- medido: corda de 392,3m pra um cabo real de 500m (~22% mais curto). Cada elemento achava
que seu comprimento natural era ~0,785m em vez de ~1,0m -- esticar o espaçamento real disso gerava
uma deformação axial artificial enorme e, com EA=360 MN, uma tração muitas vezes maior que a real.
`moorpy_warm_start.py`/`build_from_risersim_json.py` (o spike MoorPy já validado) tinham o MESMO
bug, por sinal: também inferiam o comprimento da linha somando distâncias nó-a-nó da malha reta de
entrada (documentado no próprio spike como assumindo "geometria já instalada", nunca validado pro
caminho `.aml`) -- por isso corrigir só a malha inicial (sem consertar o spike) não bastava. Duas
correções: (1) `aml_reader.py::to_risersim_json()` agora inclui um campo `model.total_length_m`
real (soma dos segmentos declarados, não re-derivável de coordenadas de uma corda reta); (2)
`build_from_risersim_json.py` usa esse campo quando presente, em vez de sempre somar distâncias
nó-a-nó; (3) `run_from_aml.py` agora aplica a correção MoorPy automaticamente no caminho `.aml`
puro (antes: só disponível como script manual separado) -- e, diferente do `warm_start` (que só
sobrepõe `disp`), sobrescreve as PRÓPRIAS coordenadas dos nós antes de `model_builder.cpp` calcular
`L_unstretched`, corrigindo o comprimento na raiz. **Resultado**: malha corrigida soma ~499,9m
(era 392,3m); `T_eff` do Cross = **217,5 kN** (real: 217,3 kN, ~0,1% de erro) -- Near/Far/
Transverse também convergem pra `T_eff`~218-220 kN cada (antes: ~950-1050 kN, bem mais dispersos
entre si, outro sinal de que eram artefato de malha, não física real). Zero mudança em C++.

**Achado novo, em aberto**: com o `T_eff` estático agora correto (bem mais baixo, ~218 kN em vez de
~1000 kN), a dinâmica do Near e do Far pararam de convergir (Far: não converge no passo 79/t=3,95s,
`res_norm=430`; antes desta correção, ambos convergiam completos) -- só o Transverse continua
convergindo limpo. Não investigado ainda -- pode ser sensibilidade do Newton dinâmico a uma
mudança real e substancial na configuração de equilíbrio (mesma família de problema do Eixo 1b,
robustez do solver dinâmico), não necessariamente um bug novo introduzido por esta correção.

**Verificação de regressão feita nesta rodada** (todos os 4 load cases do `Exemplo_01a`, binário
`risersim_test_main.exe` recompilado a cada mudança): Near/Far/Transverse (`--force-aml-path`,
estática) convergem, com posição global da conexão consistente `(-47,73, -54,50, 257,00)` m e
`refsys=105,0°` nos três (offsets com sinal correto por caso). Cross (caminho real XML+H5, sem
`--force-aml-path`, que na verdade está em `Exemplo_01a_analysis/Exemplo_01a_A1.{xml,h5}`, não em
`Exemplo_01c_analysis` como uma tentativa inicial supôs por engano) segue sem regressão: T_eff
estático convergido = **217,3 kN**, batendo o valor real já documentado. Dinâmica: Near/Far/
Transverse/Cross convergem os passos completos (Far: 60s/1200 passos; antes da Atualização 4, Far
parava em t≈9,65s e Transverse divergia no passo 4). Catch2: 361/361.

**Atualização 6** (investigação da não-convergência dinâmica do Near/Far achada na Atualização 5 --
dois bugs reais de robustez do Newton dinâmico achados e corrigidos, um terceiro achado documentado
e deixado em aberto por ser física, não bug de solver): reproduzi o caso Far (`--duration 30
--dt 0.05`, 600 passos) e instrumentei o loop (`RISERSIM_DYN_DEBUG`, removido antes do commit final)
pra imprimir, por iteração de Newton, o DOF de maior resíduo e a posição/atrito do nó dono dele --
mesma técnica já usada pro solver estático (`RISERSIM_DEBUG_RESIDUAL`, `static_analysis.cpp:401`).

1. **Passo 79 (t=3,95s): "chattering" de contato, não divergência.** O resíduo ficava preso num
   ciclo de período 2 EXATO (430,229 ↔ 179,177, repetindo indefinidamente até estourar o
   orçamento de 20 iterações) -- o Z do nó 300 cruzava o leito marinho por frações de milímetro a
   cada iteração (contato liga/desliga), exatamente o mesmo padrão "chattering" já documentado pro
   solver ESTÁTICO (`seabed.hpp:106`, `static_analysis.hpp:33-48`) mas nunca replicado no
   dinâmico. Causa raiz específica do dinâmico: o *line search* de backtracking (`docs/roadmap.md`
   item 1b, commit `c6a517d`) reutiliza a matriz de amortecimento `C_global` do TOPO da iteração
   pra avaliar cada tentativa (`Reuses this iteration's M_global/C_global for every trial`,
   documentado como "an accepted approximation") -- mas exatamente no instante em que o contato
   liga/desliga, `K_global` (e portanto `C_global = alpha*M + beta*K`) muda bastante, então o
   resíduo que o backtracking mede pra decidir aceitar/rejeitar o passo já está desatualizado em
   relação ao resíduo real que a próxima iteração vai encontrar -- deixando passar exatamente a
   correção que cruza o contato inteiro de uma vez. **Corrigido**: recalcula `C_trial` a partir do
   `K_global` fresco de cada tentativa (`trial_state.K_global`), em vez de reusar o `C_global`
   antigo. Isso sozinho já reduziu bastante o resíduo (472→184→76→49→35→32→29 em 7 iterações) mas
   não bastou sozinho pra convergir de vez -- a posição do nó, porém, já tinha convergido pra um
   ponto fixo fisicamente desprezível (250µm → 113µm → 43µm → 8,5µm → 4,2µm → 0,35µm de erro), só o
   critério de resíduo relativo (o ÚNICO critério de parada que o solver dinâmico tinha) continuava
   oscilando. Por isso um segundo fix: um critério de convergência por INCREMENTO (não força),
   idêntico em espírito ao critério translação/rotação que `ConvergenceTest` já usa no estático mas
   que o dinâmico nunca teve -- se a correção aceita numa iteração é menor que 0,1mm/0,0001rad em
   todo DOF, aceita como convergido mesmo que o resíduo de força não tenha zerado (a posição já não
   está mais se movendo, então continuar iterando é só ruído numérico). Com os dois fixes, o passo
   79 converge limpo.
2. **Passo 87 (t=4,35s): mesmo padrão de "chattering", amplitude maior (~1mm), sem decair.** Nó 297
   preso num ciclo de período 2 estável só que desta vez SEM convergir gradualmente (os dois
   estados se repetiam quase exatamente a partir da iteração 8, sem encolher) -- o fix de
   incremento acima não pega esse caso porque a oscilação não é pequena o bastante. Reconhecido
   como o caso clássico de ponto fixo de Newton oscilando entre dois estados que "cercam" a raiz
   verdadeira (o mesmo problema que motiva bisseção/regula-falsi em vez de Newton puro quando a
   raiz está entre dois pontos que o método visita repetidamente). **Corrigido**: detector de ciclo
   de período 2 -- guarda o resíduo/estado de 2 iterações atrás; se o resíduo atual bate (±0,01%)
   com o de 2 iterações atrás depois de pelo menos 5 iterações (evita disparo em coincidências
   iniciais), aceita a MÉDIA dos dois estados `U` que alternam como a solução -- na prática, uma
   bisseção do ciclo. Resultado: "período-2 chattering detectado no passo 87 iter 8 -- aceitando o
   estado com bisseção" e a simulação segue adiante.
3. **Passo 109 (t=5,45s): tração efetiva NEGATIVA em vários elementos (`T_eff` até -877 kN),
   achado real e diferente dos dois acima, NÃO CORRIGIDO** -- o resíduo cresce de forma irregular
   (não é mais um ciclo de período 2 limpo; os valores mudam a cada iteração: 14501→20172→16923→
   5506→9388→...→11278) com o pior DOF alternando de nó mas sempre em elementos com tração quase
   zero ou negativa (linha "afrouxando"/"slack" sob a excursão grande de heave/surge do topo real,
   já que o `T_eff` deste caso ficou bem mais baixo depois da Atualização 5, ~218kN vs. ~1000kN
   antes -- linha mais frouxa = mais fácil de ficar com tração negativa sob movimento dinâmico
   grande). Fisicamente, tração efetiva perto de zero ou negativa faz o termo de rigidez
   geométrica (rigidez "de corda", proporcional à tração) da barra ficar perto de zero ou negativo,
   e como a rigidez à flexão deste cabo é muito pequena (`EI=21,7 kN.m²`), o elemento fica quase
   sem resistência transversal -- um problema conhecido de dinâmica de linhas de ancoragem frouxas
   ("slack line snap"), não um artefato numérico do solver como os dois casos acima. NÃO é o mesmo
   tipo de bug -- resolver de verdade provavelmente exige uma técnica dedicada (piso mínimo de
   tração, amortecimento artificial perto de T≈0, ou uma formulação de cabo que trate afrouxamento
   explicitamente), não uma correção pontual do Newton. Deixado em aberto.

**Resultado**: Near passa a convergir os 600 passos completos (antes: não investigado, falhava);
Transverse e Cross seguem convergindo limpo (sem regressão); Far avança do passo 79 pro passo 109
(~40% a mais de simulação) antes de esbarrar no problema de tração negativa (item 3 acima, não
relacionado aos bugs de chattering corrigidos). Catch2: 361/361, sem regressão. Instrumentação de
debug (`RISERSIM_DYN_DEBUG`) foi removida antes do commit -- só ficaram os três fixes em si.

**Atualização 7** (item 3 acima, tentativa feita e revertida): antes de tentar qualquer fix,
pesquisei o Fortran real (`anf_analysis/fortran/ANFLEX_COMUNS/bpngkn.f`, `bpngknl.f`, `bpmatg.f` --
a rotina real de rigidez geométrica de viga) pra confirmar se o ANFLEX real trata compressão/tração
negativa de algum jeito especial antes de portar qualquer coisa às cegas. **Achado**: não trata --
`bpngkn.f` usa a força axial (`P1`) igual ao risersim, sem piso, sem `IF (P1.LT.0)`, sem trocar de
formulação -- ou seja, esta instabilidade não é uma lacuna do risersim vs. o real, é uma
característica da própria formulação de viga que o ANFLEX real também tem (só o elemento "cabo"
puro, sem rigidez a flexão, tem uma checagem -- e essa checagem é um `STOP` fatal quando a rigidez
axial some, `arigm3.f:223-228`, não uma correção graciosa). O único mecanismo genérico de ajuda ao
Newton que o real tem é uma "rigidez artificial" (`bpflex.f:852-909`): decai exponencialmente com o
número da iteração dentro de cada passo, não é disparada por tração negativa especificamente -- e o
risersim já porta uma versão disso pro solver ESTÁTICO (`StaticIntegrator`,
`static_integrator.cpp:107-142`, mesma forma: `k = avg(EA/L) * exp(-iter/1.25)`), mas o DINÂMICO
nunca teve. Portei a mesma técnica pro loop dinâmico (rigidez artificial adicionada em
`assemble_at()`, decaindo por iteração) e testei no Far -- **piorou**: passou a falhar no passo 104
em vez do 109 (antes desta tentativa). Revertido (`git checkout`). Diferente da tentativa
documentada na Atualização 4 (que tinha um motivo claro pro erro, 1/√2 uniforme), aqui não ficou
óbvio por que piorou -- possivelmente a rigidez artificial, adicionada em TODA iteração de TODO
passo (não só nos que precisam), muda a trajetória de convergência dos passos anteriores ao 109 o
suficiente pra deixar o passo 109 numa condição inicial pior, já que cada passo dinâmico herda o
estado exato do anterior (diferente do estático, onde artificial stiffness só é usada na fase de
"assembly" isolada, nunca na fase "real" limpa -- `ArtificialStiffnessMode::Never`). Não investigado
mais a fundo. Próxima tentativa, se valer a pena, deveria escopar a rigidez artificial SÓ aos passos/
elementos com tração já negativa (não global/sempre-ligada como o port direto fez), ou considerar
alguma das outras técnicas já listadas no item 3 (piso mínimo de tração, formulação de cabo com
afrouxamento explícito).

**Atualização 8** (item 3, segunda tentativa -- implementada, testada, também sem efeito):
seguindo a mudança de metodologia desta sessão (priorizar `anf_analysis/src` C++ real em vez de
Fortran, ver Eixo 2a), li `dynamic_integrator.cpp:516-517`/`static_integrator.cpp:206-207` do
ANFLEX real em vez do `bpflex.f` já lido antes -- achado concreto: o real só aplica rigidez
artificial no **PASSO 1** (`if(m_have_artificial_stiffness && step == 1) apply_artificial_stiffness();`,
mesmo gate em estático e dinâmico), nunca nos passos seguintes -- bem mais restrito que a tentativa
da Atualização 7 (toda iteração de todo passo). Extraí a fórmula de decaimento já portada em
`StaticIntegrator` pra uma função livre (`add_artificial_stiffness()`, `static_integrator.hpp/cpp`)
e portei pro `assemble_at()` do dinâmico, gated exatamente por `step == 1` -- suíte 361/361 sem
regressão. **Resultado real (Far, `--duration 30 --dt 0.05`): idêntico byte-a-byte ao baseline**
(mesmo chattering período-2 no passo 87/iter 8 com `res=1759.62`, mesma falha no passo 109 com
`res_norm=7730.44`). Causa: o movimento de topo real (`vessel_motion.cpp:286`) tem sua PRÓPRIA
rampa suave (mesma função `0.5*(1-cos(...))` do fallback) -- no passo 1 (t=0,05s) o fator de rampa
já é essencially zero, então o passo 1 converge trivial em pouquíssimas iterações sem nunca precisar
de correção do Newton na prática (a checagem de convergência usa só o resíduo de força, que não
depende de `K_global`/rigidez artificial nenhuma) -- ou seja, o mecanismo real de rigidez artificial
no passo 1 existe pra outra coisa (ajudar a transição estática→dinâmica em geral), não tem
oportunidade de influenciar este caso específico. **Mantido mesmo assim** (fiel ao real, zero
regressão, zero mudança de comportamento) -- é a segunda tentativa seguida sem efeito no item 3;
pausado aqui, aguardando decisão do usuário sobre se vale continuar investigando esse item
específico ou seguir pra outra coisa.

**Atualização 9** (achado decisivo + terceira tentativa, melhoria real mas parcial): antes de
tentar mais nada, comparei contra uma rodada REAL do Fortran via WSL já feita numa sessão anterior
(`exemplos/Curso/Exemplo_01/Exemplo_01c/Exemplo_01c_analysis/Exemplo_01c_A1_FarDD.SAI`, dinâmica
completa 70s/dt=0,05, mesma config) -- nunca conferida contra este item específico até agora.
**O ANFLEX real não tem NENHUMA dificuldade no passo 109/t=5,45s**: converge em 3 iterações, como
todos os passos vizinhos (2-5 iterações, sem chattering) do início ao fim dos 70s/1400 passos.
Descarta de vez a hipótese "talvez seja um limite físico/numérico que o real também tem" -- não é;
o `risersim` tem uma fragilidade real e específica que o real não tem.

Isso motivou a terceira tentativa: instanciar o Morison (`hydrodynamics.hpp`, escrito há tempos mas
nunca ligado ao solver dinâmico -- só o movimento de topo via RAO entra dinamicamente hoje, a linha
em si só recebe arrasto de CORRENTE, constante, nenhuma força hidrodinâmica de ONDA). Implementado
em `dynamic_analysis.cpp::assemble_at()`: gerado um estado de mar JONSWAP real (uma realização de
fase aleatória por análise, não por passo) via `JONSWAPSpectrum::generate_wave_components()` +
`AiryWaveKinematics`, aplicado por elemento via `MorisonForce` (arrasto + inércia). Detalhe de
correção importante: a aceleração estrutural passada pra `calculate_force_per_length()` é
deliberadamente ZERO, não a real -- o termo `-(Cm-1)*rho*A*a_struct` do Morison é matematicamente a
MESMA contribuição de massa adicionada que `mass_matrix()` já bota no lado esquerdo da equação
(`m_added = rho_water*outer_area()*props.Ca`, com `Ca=Cm-1`); usar a aceleração real duplicaria essa
massa adicionada (uma vez via `M_global`, outra via `F_ext`). Suíte 361/361 sem regressão.

**Resultado real (Far)**: passa do passo 109 pro **116** (t=5,45s → 5,80s) -- melhoria real, mas
pequena, longe do comportamento limpo do real (2-5 iterações até o passo 1400). Testei com uma
segunda semente de fase aleatória (7 em vez do default 42): passo **118** (t=5,90s) -- na mesma
faixa, confirma que não é ruído de fase específico (não é "essa fase em particular ajuda", é uma
melhoria real mas modesta e consistente entre realizações). **Suspeita não confirmada de por que a
melhoria é pequena**: o movimento de topo (RAO "harmônico equivalente", frequência única
determinística) e o Morison na linha (decomposição de fase aleatória multi-componente) usam DUAS
realizações INDEPENDENTES do "mesmo" mar JONSWAP -- fisicamente deveriam vir da mesma onda real
(correlacionadas), mas aqui não estão, o que pode fazer a força de onda na linha brigar com o
movimento do topo em vez de reforçá-lo em alguns instantes. Não investigado mais a fundo -- exigiria
repensar a representação do mar (uma realização única alimentando tanto o RAO equivalente quanto o
Morison), mudança maior. **Mantido mesmo assim** (fisicamente real, correto na formulação, melhoria
mensurável, zero regressão) -- terceira tentativa seguida sem fechar o item 3 por completo, mas a
primeira que de fato mudou o resultado na direção certa. Pausado aqui.

**Atualização 10** (2026-08-16, quarta tentativa -- bug real achado e corrigido, mais dois
mitigadores parciais explorados e deixados como decisão do usuário): antes de perseguir as duas
pistas já anotadas (correlacionar a onda do topo com a do Morison; mapear os binários `.HIS`/`.MOV`
reais), investiguei as duas primeiro -- ambas esbarraram em complicação antes de virar trabalho
concreto (`.MOV` acabou sendo só um eco de texto da config do corpo flutuante, não histórico;
`.HIS` é binário de verdade mas a rotina achada que escreve nele, `bpwrde.f`, é do módulo de
análise em domínio da FREQUÊNCIA, não do domínio do tempo -- não serve pra comparar tração por
passo do caso Far transiente). Por decisão do usuário, troquei pra instrumentar o próprio
`risersim` no passo que falha.

- **Achado**: o sinal do passo que falha é mais grave do que documentado até aqui -- tração
  efetiva em elementos perto do touchdown chega a **±1-10 MN** entre iterações (a tração real de
  topo é ~218 kN), fisicamente absurdo. Com `EA=360 MN` e elementos de ~1m, poucos centímetros de
  deslocamento relativo entre nós vizinhos já geram isso.
- **Bug real achado e corrigido**: `node->friction_force` (`seabed.hpp`) é estado PERSISTENTE
  atualizado incrementalmente (`f_state += k*du`) -- mas o laço de backtracking do solver dinâmico
  (`dynamic_analysis.cpp`, a mesma técnica que já existe no estático) chama `assemble_at()` uma vez
  por tentativa de `alpha`, e cada chamada muta `friction_force` em definitivo, INCLUSIVE tentativas
  REJEITADAS -- sem tirar/restaurar um snapshot antes de cada tentativa (o estático já faz isso,
  via `NodeStateSnapshot`/`restore()`; o dinâmico nunca teve o equivalente). O `du` de uma tentativa
  rejeitada é relativo ao estado deixado pela tentativa ANTERIOR (também rejeitada), não ao estado
  realmente aceito no início da iteração -- cada backtrack composto contamina ainda mais o estado de
  atrito. Corrigido: snapshot de `friction_force` de todos os nós antes do laço de backtracking,
  restaurado no início de cada tentativa. Sem regressão (405/405; casos já validados como Cross
  seguem convergindo idênticos -- a correção é matematicamente um no-op quando a primeira tentativa
  já é aceita, só muda comportamento quando o backtracking de fato tenta mais de uma vez).
  **Resultado isolado** (config padrão, sem mais nada): passo 116 → **128** (t=5,8s → 6,4s).
- **Dois mitigadores parciais adicionais, testados mas NÃO mantidos no código** (efeito real, mas
  com trade-offs que ficam pra decisão do usuário antes de tornar permanentes):
  1. Cap de translação do limitador de passo dinâmico (hoje hardcoded 0,5m, nunca configurável)
     mais apertado (0,01m) + orçamento de iteração maior (20→200-400): sozinho leva de 116 pro
     **145**: mas combinado com a correção do atrito acima, NÃO soma (144, quase igual) --
     indício de que o cap apertado já limita o dano que o bug de atrito causava, então corrigir os
     dois juntos é redundante, não aditivo.
  2. Tolerância do detector de chattering período-2 (`res_hist`, hoje `1e-4` relativo, nunca
     configurável) mais frouxa: combinada com a correção do atrito (cap padrão, mais iterações),
     `5%` leva de 156 pro passo **210** (t=10,5s, quase o dobro do baseline original) -- mas o
     colapso final vira catastrófico (resíduo ~1e92) em vez de um estouro comum; `1%` dá resultado
     PIOR (175) e também colapsa catastroficamente (~1e100) -- não é monotônico, sugere que uma
     tolerância frouxa demais pode aceitar uma média de dois estados genuinamente diferentes (não
     um par período-2 de verdade), plantando uma semente ruim que floresce em outro passo adiante.
- **Estado atual do código**: só a correção do bug de atrito (item acima) ficou -- é a única das
  três mudanças que é uma correção sem ambiguidade, não um trade-off. Os dois mitigadores foram
  revertidos (ficam documentados aqui, com os números, caso o usuário queira reativá-los como
  parâmetros configuráveis via JSON, no mesmo espírito do `enable_step_limiting` do estático).
- **Ainda em aberto**: mesmo só com a correção do atrito, o passo 128 falha com um padrão de
  chattering quase-período-2 (resíduo oscilando ~30mil↔226mil em DOFs laterais da zona de
  touchdown) apertado demais pro detector de 1e-4 pegar -- não é mais o "slack line" simples
  documentado antes (tração levemente negativa), é uma dinâmica de contato mais rica que ainda não
  tem causa raiz identificada. Próxima hipótese, se valer a pena: expandir o detector de
  período-2 pra período-N genérico.

**Atualização 11** (2026-08-16, quinta tentativa -- correção real de consistência, resultado nulo
neste caso): pedido do usuário pra testar se `C_trial` recomputado por tentativa (a técnica que já
fechou o chattering dos passos 79/87, Atualização 6) também se aplicava aqui. Ao reler o código com
calma: **já se aplica** -- `C_trial` já é recalculado do zero a partir do `K_global` fresco de cada
tentativa de backtracking desde a Atualização 6; o comentário logo acima é que tinha ficado
desatualizado (dizia "reusa M_global/C_global... aproximação aceita", sem refletir mais o código
abaixo). Mas essa releitura revelou um paralelo real que ninguém tinha notado: `M_global`
(matriz de massa) era montada UMA VEZ no topo de cada iteração e reaproveitada em TODAS as
tentativas de backtracking daquela iteração, com um comentário afirmando "não depende de Z" --
falso: `local_mass_matrix()` (`element_beam.cpp`) usa `current_length()`, que muda com a posição
dos nós tanto quanto a rigidez geométrica que já motivou recalcular `K`/`C` por tentativa. Extraído
`assemble_mass()` (lambda, mesmo padrão de `assemble_at()`) e chamado tanto no topo da iteração
quanto dentro do laço de backtracking, produzindo `M_trial` fresco por tentativa. Suíte sem
regressão (405/405); Cross seguiu convergindo idêntico. **Resultado no caso Far**: nenhuma
mudança -- passo de falha e resíduo final ficaram byte-idênticos ao baseline só-com-o-fix-de-atrito
(passo 128, `res_norm=9.81756e6`), confirmando que a massa nunca diverge o bastante entre
tentativas pra mudar uma decisão de aceitar/rejeitar neste caso específico (o comprimento dos
elementos na zona de touchdown varia pouco demais, mesmo sob uma tentativa ruim, pra M pesar).
**Mantida mesmo assim** (correção de consistência real e barata, sem regressão, mesmo padrão já
validado pra K/C) -- resultado nulo aqui, mas pode importar em outro caso com deformação mais
extrema por tentativa. O comentário desatualizado foi corrigido pra refletir que K/C/M são todos
recalculados por tentativa agora.

**Atualização 12** (2026-08-16, sexta tentativa -- achado grande, causa raiz provável do gap
inteiro): usuário pediu pra verificar os dados/cálculo do movimento em vez de continuar mexendo no
solver. Reli `vessel_motion.cpp::get_motion()`: a rampa que traz o movimento de zero até a
amplitude plena usa `ramp_time_s_ = 5.0` FIXO, nunca ligado a nenhum dado real.

- **Achado real**: a tabela real "JOINT MOVEMENTS - WAVE AND MOVEMENT CORRELATED" de um `.SAI`
  real (`Exemplo_01c_A1_FarDI.SAI:811-818`) imprime, por GDL, `AMPLITUDE`, `PERIOD`, `PHASE` E
  **`RAMP`** -- uma coluna nunca conferida até agora. Pro Far: `PERIOD=15,348s` (nosso cálculo:
  15,24s, ~0,7% de diferença -- confirma que a frequência equivalente em si já estava correta),
  `RAMP=30,696s`. Conferido nos outros 3 casos reais também (Cross: período 11,022/rampa 22,044;
  Near: 10,717/21,434; Transverse: 10,325/20,650) -- em TODOS, **rampa = 2 × período**, exato.
- **Tentativa 1, descartada**: ler o C++ real (`model_builder_dat.cpp:461-480/5223-5225`,
  `cWave::get_wave_ramp()`) mostrou que o default real É `0,10 × duração total da análise
  dinâmica` -- implementei isso (`ramp_time_s = 0.10 * duration_s`, threaded desde
  `dynamic_analysis.cpp` até o construtor de `VesselMotion`). **Quebrou o caso Cross** (que sempre
  convergiu limpo): `%ANALYSIS_CASE.TIME_DOMAIN.TOTAL_TIME` do `Exemplo_01a.aml` é só 1,0s (valor
  de teste/exemplo), então a rampa virou 0,1s -- amplitude plena em 2 passos de tempo, choque
  imediato na estrutura. Revertido antes de qualquer commit.
- **Investigação da duração real por trás do `.SAI`**: achei `TOTAL TIME = 70.0000` impresso no
  próprio `FarDI.SAI` -- mas veio de `Exemplo_01c.aml` (não `Exemplo_01a.aml`, que só tem o valor
  de 1,0s de brinquedo -- são dois arquivos de exemplo diferentes sobre o mesmo modelo físico).
  Com duração real = 70s, a fórmula "10% da duração" daria rampa=7,0s -- não bate com os 30,696s
  reais (30,7/70=0,44, não 0,10). **A fórmula "10% da duração" está confirmada no código real, mas
  não é o que gerou ESSES dados específicos** -- não achei o campo/cálculo exato de onde
  `get_wave_ramp()` tirou 30,696s pra esse caso (não é um campo exportado no `.aml`, que não tem
  nenhuma ocorrência de "RAMP" em lugar nenhum). O padrão "2×período" continua batendo exato nos 4
  casos reais, então é essa relação (baseada no período do próprio movimento equivalente, não na
  duração total escolhida pra rodar a simulação) que foi implementada.
- **Implementado**: `ramp_time_s_` agora é calculado DENTRO do construtor de `VesselMotion`
  (`2 * (2*pi/omega_eq_)`), depois que a frequência equivalente é conhecida -- não depende mais de
  nenhum parâmetro externo (`dynamic_analysis.cpp` não passa mais rampa nenhuma, só o construtor
  resolve sozinho). Suíte sem regressão (405/405). Cross convergeu limpo com rampa calculada
  =22,17s (bate com o real 22,044s). Near e Transverse também sem regressão.
- **Resultado no caso Far**: o passo de falha foi de **t=5,8s pro passo 442, t=22,1s** -- quase
  4x mais simulação, com um resíduo final pequeno (776 N, não mais explosões de milhões) --
  claramente o maior salto de todo esse eixo de investigação, e resultado IDÊNTICO rodando com
  duração total de 30s ou 70s (confirma que a rampa agora é intrínseca ao movimento, não à duração
  escolhida pra rodar). Testado orçamento maior de iterações (20→100): melhora um pouco mais
  (passo 453, t=22,65s) mas não fecha -- resíduo na explosão final ficou maior (100687 N), sinal
  de que o que resta pode ser um problema genuinamente diferente do que já foi caçado até aqui,
  não mais o mesmo padrão de chattering pequeno.
- **Em aberto**: ainda não converge o run completo, mas a causa provável do gap inteiro (rampa
  fixa de 5s completamente desconectada da física real, aplicando o movimento pleno rápido demais)
  parece ter sido a principal — o restante que falta agora, no novo ponto de falha (t≈22s, perto
  de onde a excursão do topo atinge seu primeiro extremo, dado T≈15,3s), precisa de investigação
  própria, sem a distorção anterior.

### 2b. Suporte a múltiplas zonas de solo por segmento (opcional, avaliar sob demanda)

Achado no `Boiao/P52_Boiao.aml` (uma linha atravessando 3 solos diferentes ao longo do próprio
comprimento) — nem `aml_reader.py` nem `xml_h5_reader.py` correlacionam isso hoje, ambos assumem
solo único global. Só vale a pena se um caso de teste real desse tipo entrar em uso.

### 2c. Suporte a múltiplas linhas (corpo flutuante compartilhado) — ✅ IMPLEMENTADO, convergência real verificada

Pedido do usuário (prioridade explícita), movido do backlog. Caso real: `Multilinhas.aml` -- 7
linhas presas ao MESMO corpo flutuante (`%FLOATING.SHIP`, 6 GDL) via 7 pontos de conexão locais
distintos, não acoplamento linha-linha. Pesquisa prévia (2 agentes Explore + 1 Plan) mapeou tanto
a arquitetura real do ANFLEX (`interfaces/src/`: `cReference`/`cConnection`/`cLine`, corpo
flutuante é um SUBTIPO de referência, não entidade separada) quanto o estado do risersim dos dois
lados -- decisão do usuário: espelhar fielmente essa hierarquia no schema JSON (`references`/
`connections`/`lines`), não uma versão minimalista; frontend/web run-manager explicitamente fora
de escopo desta rodada.

**Fase 1 -- mecânica C++ multi-anexo** (`model.hpp`: novos `ReferenceInfo`/`ConnectionInfo`/
`LineInfo` + `RiserModel::resolve_line_attachments()`; `simulation.cpp`/`dynamic_analysis.cpp`/
`static_analysis.cpp`: os 3 pontos hardcoded em `nodes.front()` como "o" nó de topo viraram loops
por linha). Achado no caminho: plumbing profundo do solver (numeração de equação RCM,
`compute_rcm_order()`) já era 100% genérico pra componentes desconexos -- zero mudança necessária
ali, só ganhou teste dedicado. Achado um bug de correção física real (não relacionado à
numeração): `compute_stress_and_curvature()`'s vizinho prev/next (usado pra curvatura/momento/von
Mises/MBR) era escolhido só por posição no array (`elements[i-1]`/`[i+1]`), não por adjacência
topológica real -- com múltiplas linhas, elementos de linhas diferentes ficam lado a lado no array
plano sem se tocar; corrigido pra checar `node2==node1` de verdade antes de usar como vizinho (4
pontos: `capture_snapshot()`, `solve_vessel_offset()`, `dynamic_analysis.cpp`, todos compartilhando
a mesma lógica antes errada). Novo `tests/test_multiline.cpp`: duas correntes catenárias
totalmente desconexas resolvidas juntas batem, ponto a ponto, contra cada uma resolvida sozinha
(prova zero cross-talk); modelo sem `lines[]` continua com comportamento idêntico ao de sempre.

**Fase 2 -- parsing JSON no `ModelBuilder`** (`model_builder.cpp`): resolução de ID generalizada
de índice-direto-no-array (`node1_id - 1` como posição) pra mapa `id -> Node3D*` (mesmo padrão já
usado pelo bloco de warm-start) -- necessário pra IDs globalmente únicos mas não densos entre
linhas. Extraída `parse_vessel_motion_config()` (antes inline só em `environmental.vessel_motion`)
pra ficar reutilizável também em `references[].vessel_motion`. Fixture JSON de 2 linhas parseia e
converge; fixture single-line (sem `lines[]`) prova retrocompatibilidade total.

**Fase 3 -- síntese multi-linha em `aml_reader.py`, testada contra o `Multilinhas.aml` real**:
`_extract_all()`/`_parse_floating_ships()`/`_parse_connections()` já parseavam tudo multi-entrada
(achado surpreendente -- não começava do zero); o gargalo estava só downstream
(`to_risersim_json(line_index=0)` sempre resolvia UMA linha). Novo `to_risersim_json_multiline()`
(`tools/aml_reader.py`) + `build_config_from_aml_multiline()` (`risersim_runner.py`) +
`run_from_aml.py --all-lines`. Achado e resolvido no caminho: a correção de malha via MoorPy
(`_apply_moorpy_mesh_correction()`, corrige a corda reta pra geometria de catenária real --
sem ela `L_unstretched` sai curto e a tração estática fica várias vezes maior que a real, mesmo
bug já documentado no Eixo 2a) assume que `model.elements` forma UMA corrente contínua
(`node_order = [elements[0].node1_id] + [e.node2_id for e in elements]`) -- quebraria correndo
através de fronteiras entre linhas no array plano mesclado. Resolvido aplicando a correção POR
LINHA (`ANFLEXAMLReader._moorpy_correct_line_mesh()`, nova, roda a mesma lógica MoorPy numa
fatia JSON de uma linha só, isolada, dentro do loop) em vez de tentar ensinar a ferramenta
existente sobre fronteiras de linha.

**Verificação real** (2026-08-16): `exemplos/Multilinhas/Multilinhas.aml` real, via `run_from_aml.py
--all-lines --static-only`: as 7 linhas resolvem conexão+catenária reais (moorpy) com sucesso,
schema gerado estruturalmente correto (14497 nós/14490 elementos, IDs globalmente únicos e sem
sobreposição entre linhas, confirmado inspecionando o JSON gerado -- linha N vai de node_id
X a Y sem invadir a faixa da linha seguinte), `references`/`connections`/`lines` corretos (1
corpo flutuante compartilhado, 7 conexões de topo + 7 de âncora). H5 exportado sem crash mesmo
no modelo grande.

**Achado inicial: a estática não convergia** -- resíduo divergia já no 1º passo de carga. Isolado
via teste A/B: rodando a MESMA linha 1 sozinha pelo caminho single-line já existente e não tocado
(`to_risersim_json()`, sem nenhuma mudança desta rodada) reproduz a *mesma* divergência -- prova
que não era um bug do multi-linha, e sim algo pré-existente no motor pra esse tipo de geometria
(águas muito profundas, 2200m vs. 100-265m dos exemplos já validados; linha muito longa, 5265m;
EI muito baixo, 6,27 kN·m²; malha com ~2070 elementos/linha, bem além de qualquer caso já testado).

**✅ Investigado e resolvido, sem mudança de C++** (2026-08-16): instrumentação de debug temporária
(removida depois) achou a causa real -- não é o "chattering" de contato/atrito já documentado (o
DOF de maior resíduo, no trecho onde a explosão começa, é sempre uma ROTAÇÃO, em nós longe do
leito, com atrito zerado). É mal-condicionamento: EI tão baixo com malha tão fina deixa a rigidez
rotacional quase singular, e o passo de Newton cheio "chuta" a rotação longe demais. O mecanismo
que resolve já existia (`StaticAnalysis::enable_step_limiting`, desligado por padrão) -- só
precisava de calibração: cap de rotação bem mais apertado que seu próprio default (0,01 rad vs.
0,3 rad) elimina a explosão por completo; a tolerância de força/momento desbalanceados (usada como
"escape hatch" perto do limite de iteração, ver `convergence_test.cpp`) também precisou afrouxar
(5→30 N) pra não ficar perseguindo um resíduo já fisicamente desprezível por centenas de
iterações. Confirmado convergindo as 7 linhas individualmente E o modelo combinado (7 linhas, 11
passos de carga cada) -- todas batendo no mesmo `T_eff=2361 kN` (forte prova de consistência
física, já que as 7 linhas são geometricamente equivalentes, só giradas). Testado também que essas
configurações são inofensivas num caso já validado (Exemplo_01a/Cross, EI=21,7 kN·m²): T_eff
idêntico ao baseline (217,3 kN), convergindo em MENOS iterações -- ou seja, o gatilho da
auto-detecção não precisa ser cirúrgico, só separar os dois pontos reais já encontrados.

**Implementado**: `ANFLEXAMLReader._static_robustness_overrides(ei_nm2)` (`tools/aml_reader.py`),
chamado tanto em `to_risersim_json()` quanto em `to_risersim_json_multiline()` -- se
`EI < 15.000 N·m²` (limiar entre os dois pontos reais, 6.270 instável / 21.700 inofensivo), liga
`enable_step_limiting`+cap 0,01 rad, afrouxa `unbalanced_force_tol`/`moment_tol` pra 30 N, e sobe
`max_iterations` pro maior entre o valor real do `.aml` e 400 (cobre o pior caso observado, o
passo 1 partindo da malha reta inicial, que precisou de 387). Zero mudança em C++/`model.hpp`
(os defaults do mecanismo continuam os mesmos pra todo o resto). Reverificado ponta a ponta pelo
pipeline real (`run_from_aml.py --all-lines`, sem nenhuma edição manual de JSON): converge os 11
passos, mesmo `T_eff=2361 kN` de antes; `Exemplo_01a` via caminho `.aml` puro confirmado SEM
disparar a auto-detecção (EI=21,7 kN·m² > limiar), comportamento idêntico ao de sempre. Catch2:
405/405 (mudança só em Python).

**Fora de escopo desta rodada** (confirmado com o usuário antes de começar): frontend (`Riser3DRenderer.js`/`DataLoaderService.js`/tabela de resultados -- hoje assumem uma corrente única contígua, precisam de conectividade real exportada no H5 primeiro); seleção de linha no gerenciador de rodadas web (`risersim_projects.py`/`run_server.py`, sem conceito de "linha" hoje); caminho XML+H5 (`xml_h5_reader.py`, sem export real multi-linha existente).

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

#### Fase 3 — ✅ IMPLEMENTADA: caso de carregamento por rodada, projeto a partir de `.aml` puro, limpeza de UI

Rodada de trabalho puxada pelo usuário navegando a interface real (Docker) e apontando lacunas
concretas, não planejada de antemão.

- **Caso de carregamento por rodada** — até aqui, "projeto" era 1:1 com um caso já resolvido (uma
  pasta XML+H5 = um caso); o `--load-case-id` do `run_from_aml.py` nunca tinha sido ligado ao
  backend web. Decisão do usuário (entre duas arquiteturas propostas): **projeto = modelo físico
  (o `.aml`), rodada escolhe o `%LOAD_CASE`** — o mesmo modelo mental já usado via CLI a sessão
  inteira, agora navegável dentro do MESMO projeto. Implementado extraindo
  `build_config_from_aml()`/`list_aml_load_cases()`/a correção de malha MoorPy pra
  `risersim_runner.py` (compartilhadas entre `run_from_aml.py` e o backend web, zero mudança de
  comportamento no CLI); `ProjectStore.create_run()` ganhou `load_case_id` opcional, monta e
  hasheia o config certo por rodada (`DuplicateRunError` substitui a checagem de dedupe que antes
  só olhava o projeto); nova rota `GET /api/projects/<id>/load-cases`; seletor de caso na criação
  de rodada (`project.html`/`preprocessor.html`) e coluna "Caso" nas tabelas de rodada.
- **Projeto a partir de `.aml` puro** — perguntado pelo usuário ao notar que só 7 dos 31 exemplos
  (os com pasta `_analysis/` já exportada) podiam virar projeto. `discover_aml_only_examples()`
  (`risersim_runner.py`) varre `trunk/exemplos/` por `.aml` sem XML+H5 correspondente (24 dos 31);
  `create_project()`/`_store_source_files()` (`risersim_projects.py`) e as rotas de criação
  (`run_server.py`) passaram a aceitar `aml_path` sozinho; `dashboard.html`/`dashboard.js` marcam
  exemplos aml-only na lista e mostram o seletor de caso já na criação do projeto.
- **Limpeza de UI do pós-processador** — aba "📁 Carregar" (upload manual, redundante com o caminho
  `?project=&run=` principal) removida; controles de animação (play/pause/slider) movidos da aba
  lateral pra uma barra flutuante compacta sobre o canvas 3D (estilo player de vídeo, só ícones,
  cor no gradiente laranja da marca — `--brand-accent`/`--brand-accent-strong`), reaproveitando o
  padrão de overlay HTML já usado por `#colorbar-legend`/`#zoom-rect`.
- `WEB_VERSION`: `1.0.0` → `1.3.1`.

**Verificação real** (2026-08-15, mesmo stack Docker): rebuild de `web` OK a cada mudança; todos os
4 casos de carregamento do Exemplo_01a criados como rodadas do mesmo projeto, T_eff consistente com
o já validado via CLI (~217-220 kN); projeto criado a partir de `Exemplo_01b.aml` (aml puro) —
rodada falhou na estática por limitação pré-existente e não-relacionada (EI≈0,01 kN·m², reproduzida
identicamente via CLI); projeto criado a partir de `DNV_Check.aml` (aml puro) — estática convergiu
limpa (18 iterações), dinâmica confirmada avançando via log ao vivo antes de abortada (rodada grande
demais pra esperar terminar, não necessário pra validar o pipeline). Markup/JS servidos conferidos
via `curl` contra o container real a cada mudança.

### 3c. Pós-processamento

Já existe uma base sólida (`posprocessor.html`, viewer 3D, gráficos Plotly). O trabalho principal
aqui é integrar com o conceito de "projeto/rodada" do item 3b (carregar resultados de uma rodada
específica do histórico, em vez de sempre apontar para um arquivo fixo) — depende de 3b existir
primeiro para fazer sentido completo, mas a base de visualização já não precisa de retrabalho.

#### Fase 1 — ✅ IMPLEMENTADA: envoltórias + histórico no tempo

Puxada pelo usuário comparando com o que o ANFLEX real oferece no pós-processamento. Pesquisa no
C++ real (`anf_analysis/src/post_processor.cpp`/`results_structs.h`/`results_variables.h`)
confirmou que envoltória (mín/máx por nó de elemento, sobre um range de passos) e histórico no
tempo (série completa por nó/elemento escolhido pelo usuário) são recursos de primeira classe lá —
e que toda a série temporal do `risersim` já vive na memória do navegador
(`FEASimulation.staticSteps`/`.dynamicSteps`, populados por `DataLoaderService.js`), então os dois
recursos foram implementados **só no frontend**, sem tocar em C++/exportação.

- **Envoltória**: `FEASimulation::getElementEnvelope(field)` (novo método) itera `activeSteps` e
  retorna mín/máx por posição ordinal do elemento. Checkbox "📉 Mostrar envoltória" (aba
  Visualização) sobrepõe 2 traces tracejados (mín/máx) aos 3 gráficos de perfil já existentes
  (Tração/Momento/von Mises) — decisão do usuário: overlay nos gráficos existentes, não uma aba
  separada.
- **Histórico no tempo**: nova aba de viewport "🕒 Histórico no Tempo" +
  `TimeHistoryChartController.js` (novo). Seleção do ponto de interesse é feita clicando numa linha
  da tabela de Elementos/Nós já existente (aba "📊 Tabela") — decisão do usuário, em vez de um
  dropdown — o que também destaca o nó/elemento escolhido na cena 3D
  (`Riser3DRenderer::updateSelectionHighlight()`, reaproveitando o `nodesGroup`, existente mas
  nunca antes populado). Nó selecionado mostra X/Y/Z vs. tempo; elemento selecionado mostra o campo
  escalar atualmente escolhido em "🎯 Grandeza para Colorir" vs. tempo, com uma linha vertical
  marcando o passo ativo do slider.
- `WEB_VERSION`: `1.3.1` → `1.4.0`.

**Verificação real** (2026-08-16, Docker): rebuild de `web` OK; testado via um harness HTML
same-origin (mesma técnica já usada na Fase 2 do Eixo 3b, já que não há Playwright/Selenium no
ambiente) contra uma rodada real convergida do caso Far (`Exemplo_01a`, 21 passos dinâmicos, 501
nós/500 elementos): clique em linha de elemento → `selectedPoint` correto, classe `selected-row`
aplicada, marcador 3D criado; clique em linha de nó → histórico com 3 traces (X/Y/Z); clique de
novo na mesma linha → deseleciona; toggle de envoltória → Tração 1→3 traces, Momento+Curvatura
3→4, von Mises 2→4, todos os traces extras corretos. Zero erros de console capturados em todo o
fluxo. Confirmado visualmente por screenshot (tema escuro) que o layout/cores seguem o padrão
visual já estabelecido.

#### Fase 2 — ✅ IMPLEMENTADA: correções de layout dos gráficos + tabela de Elementos compacta

Puxada pelo usuário usando a Fase 1 na prática. Três achados de layout, seguidos de uma
reorganização da tabela de Resultados.

- **Título/legenda sobrepostos pelas barras flutuantes**: os 4 `div`s de gráfico (`.viewport-layer`)
  preenchiam 100% da altura do `#canvas-container` desde y=0, ficando por baixo de
  `#viewport-tabs` (topo) e `#playback-overlay` (base), ambos `position:absolute`. Tentativa 1
  (`padding-top`/`padding-bottom` no CSS) não funcionou -- `clientHeight`, que o Plotly lê pra se
  dimensionar, INCLUI padding, então o gráfico continuava se desenhando no tamanho cheio e o
  excesso (onde ficava o eixo X) era cortado pelo `overflow:hidden` do container, fazendo o eixo
  sumir por completo. Corrigido de vez trocando pra `position:absolute` com `top`/`bottom` (mesmo
  padrão dos outros overlays) -- isso sim encolhe a caixa real antes do Plotly medir.
- **Legenda acima do título** (ordem invertida): a legenda usava coordenadas `paper` (relativas à
  área do gráfico, `y:1.12`) enquanto o título usava posição automática do Plotly -- a legenda
  acabava renderizando mais alto que o título. Corrigido fixando os dois em coordenadas
  `container` (`yref:'container'`) com o título mais perto do topo e a legenda abaixo dele, em
  `ProfileChartsController.js` e `TimeHistoryChartController.js`.
- **Envoltória mín. "invisível"**: as curvas de mín./máx. usavam a mesma cor da série principal (só
  com opacidade reduzida) -- quando o valor mínimo histórico de um elemento coincidia com o do
  passo atualmente exibido (comum logo após trocar de modo estático/dinâmico), a curva tracejada
  ficava desenhada exatamente por baixo da sólida, parecendo ausente. Corrigido com cores fixas e
  distintas (`#ec4899` máx./`#06b6d4` mín.) em vez de derivadas da cor de cada série, garantindo
  contraste mesmo quando os valores coincidem.
- **Tabela de Elementos compacta (Proposta C, escolhida entre 3 propostas apresentadas)**: a tabela
  de 8 colunas (ID/Nó/Status/Tração/Momento/Curvatura/von Mises/MBR) já exigia scroll horizontal no
  painel lateral. Reduzida a 4 colunas "manchete" (ID/Nó/Status/Tração); as 4 grandezas restantes
  (Momento/Curvatura/von Mises/MBR) passam a aparecer num painel de detalhe (`#element-detail-card`,
  reaproveitando as classes `control-group`/`stat-card` já existentes) que se popula ao clicar numa
  linha -- o mesmo mecanismo de seleção (`selectPoint()`) já usado pelo destaque 3D e pelo histórico
  no tempo da Fase 1. Para não perder a capacidade de achar rapidamente "o pior elemento" que a
  tabela larga dava de graça, os 3 cabeçalhos restantes ganharam ordenação por clique (▲/▼/⇅),
  persistente durante a navegação entre passos. Decisão do usuário: uma "tabelão com tudo" pode ser
  adicionada depois, sob demanda -- não faz parte deste incremento.
- `WEB_VERSION`: `1.4.0` → `1.4.1`.

**Verificação real** (2026-08-16, mesmo harness HTML same-origin contra a mesma rodada Far):
gráficos conferidos visualmente por screenshot em cada etapa (título acima da legenda, eixo X
visível, barra de playback sem sobrepor nada; envoltória mín./máx. com cores distintas em dois
passos diferentes, inclusive um onde mín.==passo atual). Tabela: 4 colunas confirmadas, ordenação
ascendente/descendente testada na coluna Tração (elemento 1, maior tração, ordena primeiro em
descendente; elementos da zona de touchdown, menor tração, primeiro em ascendente) com indicador
correto; clique em linha popula o painel de detalhe com os valores corretos, mantém destaque 3D e
histórico no tempo funcionando; painel de detalhe some ao trocar para a visão de Nós. Zero erros de
console em todo o fluxo.

#### Fase 3 — ✅ IMPLEMENTADA: seletor de grandeza dedicado pro histórico no tempo

Usuário perguntou se dava pra escolher a grandeza do histórico. Até aqui, o histórico de um
elemento reaproveitava o valor de "🎯 Grandeza para Colorir" (aba Visualização, campo que também
colore a cena 3D) -- funcionava, mas escondido, e com um bug real: trocar aquele seletor enquanto
a aba "Histórico no Tempo" estava ativa forçava a navegação de volta pra aba de perfil
correspondente (`switchViewportView()`'s condição `activeViewportView !== '3d'` não excluía
`'history'`), tirando o usuário da aba sem pedir.

- Novo seletor `#history-field-select`, independente do de coloração 3D (`this.historyField`,
  novo estado em `app.js`, default `'tension'`), numa barra flutuante (`#history-field-bar`) só
  visível durante a aba Histórico e só quando o selecionado é um ELEMENTO (nó não tem grandeza --
  histórico de nó já é X/Y/Z fixo).
- Bug de navegação corrigido: `activeViewportView !== '3d' && activeViewportView !== 'history'`.
- **Achado de layout, pego só depois de screenshot** (não pela suíte funcional): a barra flutuante
  original, ancorada `position:absolute` no canto direito da mesma linha das abas, colava direto
  em cima do texto "🕒 Histórico no Tempo" assim que a 5ª aba entrou na fileira -- espaço
  insuficiente pra ambos convivirem numa tela de largura comum. Corrigido reestruturando as duas
  (`#viewport-tabs` + `#history-field-bar`) como filhos de um `#viewport-toolbar` com
  `flex-wrap:wrap`, então a barra de grandeza cai pra uma segunda linha em vez de sobrepor. Isso
  por sua vez expôs um segundo problema (mesma categoria da Fase 2: espaço reservado insuficiente
  pra um overlay externo) -- com 2 linhas de toolbar, o topo reservado de 52px não bastava mais,
  a barra invadia o título do gráfico. Corrigido com uma classe `with-field-bar` (alternada via JS
  junto com a visibilidade da própria barra) que só aumenta o `top` do `#history-chart`
  especificamente quando a barra pode estar visível, sem afetar os outros 3 gráficos.
- `WEB_VERSION`: `1.4.1` → `1.4.2`.

**Verificação real** (2026-08-16, mesmo harness): selecionar elemento → trocar grandeza do
histórico (própria barra) → título do gráfico muda, aba continua em "Histórico" (bug corrigido);
trocar a grandeza de COLORAÇÃO 3D (seletor não relacionado) enquanto na aba Histórico → não navega
pra fora, histórico não muda (desacoplamento confirmado); selecionar nó → barra some. Screenshot
confirmou visualmente a quebra de linha sem sobreposição e o título do gráfico totalmente visível
abaixo das duas linhas da toolbar. Zero erros de console.

#### Fase 4 — ✅ IMPLEMENTADA: aposentar JSON de resultados, completar/comprimir o H5, nomear pelo caso

Usuário perguntou se os resultados iam todos pro H5 -- não iam (só posição+tração; momento/
curvatura/von Mises/MBR só existiam no JSON, com o loader JS mascarando a ausência com `0.0`/`5.0`
default). Ao discutir tamanho/performance (JSON de uma rodada pequena já media 5,38 MB nesta
sessão, com os passos dinâmicos gravados EM DOBRO -- `dynamic_steps` + o array `steps` de
compatibilidade), decisão do usuário: aposentar o JSON por completo, completar o schema do H5,
ativar compressão gzip, e nomear o arquivo de resultados pelo projeto+caso em vez de um nome
genérico.

- **C++** (`simulation_exporter.cpp`): `write_hdf5_group()` passa a gravar os 5 campos por
  elemento (antes só tração) e comprime todos os datasets (posições + elementos) com
  `H5::DSetCreatPropList::setChunk()+setDeflate(6)` -- gzip é filtro padrão do HDF5, pesquisa
  prévia confirmou que o h5wasm 0.4.10 (já usado no pós-processador) lê sem plugin nenhum.
  `export_json()`/`write_snapshots_json_array()` removidos inteiros, junto com o binding pybind11
  e a chamada em `Simulation::export_results()`.
- **Nome do arquivo pelo caso**: o binário C++ não tem noção de "projeto"/"caso" (sem campo de
  nome em `RiserModel`), então continua escrevendo `catenary_results.h5` fixo; `run_worker.py`
  RENOMEIA pra `<Projeto>_<Caso>_results.h5` assim que o solver termina
  (`ProjectStore.finalize_results_filename()`, novo, com `_sanitize_filename_component()` --
  espaço vira `_`, preserva maiúsculas ao contrário do `_slugify()` já existente pro ID do
  projeto), gravando o nome real em `run.json["results_filename"]`. Decisão do usuário: a
  INTERFACE busca e usa esse nome real (não um nome estável traduzido por trás no servidor) --
  `app.js::resolveResultsUrl()` virou assíncrona, busca `GET .../runs/<id>` primeiro pra ler
  `results_filename` antes de montar a URL de resultados; `project.js`'s "⬇ Resultados" usa o
  mesmo campo (já disponível, sem fetch extra). `run_server.py::api_run_results()` valida o nome
  pedido contra `run.json["results_filename"]` em vez de um dicionário estático -- funciona como
  controle de acesso también (só o arquivo real da rodada é servível). Fora de escopo, deliberado:
  o workflow manual `run_from_aml.py` (sem `ProjectStore`) continua com nome fixo.
- **JS**: `DataLoaderService.js` perde `loadJSON()`/`parseRawStepsArray()` e a lógica de
  detecção de extensão + fallback pra JSON -- `load()` só chama `loadHDF5()`.
- `WEB_VERSION`: `1.4.2` → `1.5.0`.

**Bug real achado e corrigido durante a verificação, sem relação com o plano acima**: a URL do
`<script>` do h5wasm em `posprocessor.html` sempre apontou pra
`.../h5wasm@0.4.10/dist/h5wasm.js`, que **não existe** no pacote (404) -- o bundle real do browser
fica em `dist/iife/h5wasm.js`. Como o caminho H5 nunca tinha sido de fato exercitado em produção
(nada pedia `?format=h5` por padrão), esse 404 nunca foi notado -- o carregamento H5 estava
quebrado desde sempre, silenciosamente. Só foi pego agora porque H5 virou o único formato e um
harness headless real tentou carregar um resultado de verdade. Corrigido (path certo confirmado
batendo `curl` contra o CDN antes de aplicar).

**Verificação real** (2026-08-16, Docker + MSBuild): Catch2 361/361 sem regressão. CLI
(`run_from_aml.py`, caso Cross): só `.h5` gerado, T_eff=217,29 kN batendo o valor já validado
(217,3 kN); H5 inspecionado via h5py -- 6 datasets presentes (`node_positions` +
5 campos de elemento), todos `compression=gzip`; tamanho **menor** que o H5 antigo mesmo com 4
datasets a mais (533.440 → 404.640 bytes) graças à compressão. Pipeline web (rodada real do Far):
`run.json` com `results_filename="Exemplo_01a_Far_results.h5"`; arquivo já nasce com esse nome
dentro do diretório da rodada (confirmado via `docker compose exec`); API serve o nome real (200),
rejeita o nome genérico antigo e o `.json` (404 nos dois), `Content-Disposition` do download traz
o nome certo. Pós-processador carregado via harness headless real (não só screenshot) -- painel de
detalhe mostra Momento/Curvatura/von Mises/MBR REAIS (antes viriam zerados sob o H5 incompleto),
envoltória e histórico com seletor de grandeza funcionando sobre dado H5 de verdade. H5 desta
rodada (920.787 bytes, 33 passos) vs. o JSON medido antes pra essa mesma rodada (5.382.923 bytes,
com a duplicação) -- **~5,85x menor**, já confirma a direção esperada mesmo numa rodada pequena.
Zero erros de console em todo o fluxo.

#### Fase 5 — ✅ IMPLEMENTADA: escrever com o nome certo de saída, não renomear depois

Usuário questionou o desenho da Fase 4 (renomear `catenary_results.h5` pra
`<Projeto>_<Caso>_results.h5` DEPOIS que o solver termina) -- preferiu que o nome certo já nascesse
junto com o arquivo. Projeto+caso já são conhecidos no momento em que a rodada é CRIADA (antes do
solver sequer começar), então não havia motivo real pra esperar o fim da execução só pra renomear.

- **C++**: `Simulation::export_results(output_dir, filename="catenary_results.h5")` ganhou um
  segundo parâmetro (nome do arquivo, com default que preserva o comportamento do workflow manual
  `run_from_aml.py`, que não passa esse argumento); `main()` (`main.cpp`) lê um 3º `argv` opcional
  e repassa. `risersim_test_main <input.json> <output_dir> [nome_do_arquivo]`.
- **`risersim_projects.py::create_run()`**: passa a calcular e gravar `results_filename` já na
  criação da rodada (mesma fórmula `<Projeto>[_<Caso>]_results.h5` de antes), não mais depois que
  o solver termina. Método `finalize_results_filename()` (Fase 4) removido inteiro -- não existe
  mais rename, só escrita direta.
- **`run_worker.py`**: lê `results_filename` do `run.json` (já preenchido desde a criação) e passa
  como 3º argumento pro binário -- `cmd = [exe, input_json, run_dir, results_filename]`. O arquivo
  nasce com o nome certo; nenhum `catenary_results.h5` intermediário chega a existir no disco.
- **`app.js::resolveResultsUrl()`**: como `results_filename` agora está sempre presente (mesmo
  numa rodada `pending`/`running`, já que o nome é conhecido antes do arquivo existir), a checagem
  de "tá pronto pra carregar" passou a olhar `run.status` (`converged`/`failed`) em vez da
  presença do campo.
- `WEB_VERSION`: sem bump adicional (mesmo `1.5.0` da Fase 4 -- ainda não commitado).

**Verificação real** (2026-08-16, Docker): rebuild C++ local confirmou o binário aceita o 3º
argumento (com ele, escreve direto no nome pedido; sem ele, mantém `catenary_results.h5` -- CLI
manual não quebrou). Catch2 361/361. Rodada real disparada via API (`Near`, `exemplo-01a`):
`results_filename="Exemplo_01a_Near_results.h5"` já presente com `status:"pending"`, antes do
solver começar; `stdout.log` do solver confirma que ele mesmo escreveu direto nesse caminho
(`✅ ... exported to: .../Exemplo_01a_Near_results.h5`); diretório da rodada terminada só tem esse
arquivo, nunca teve um `catenary_results.h5` intermediário. Pós-processador carregado via harness
headless real sobre essa rodada -- 21 passos dinâmicos/12 estáticos, zero erros de console.

#### Fase 6 — ✅ IMPLEMENTADA: entrada sempre obrigatória (sem fallback sintético), saída sempre ao
lado da entrada

Usuário perguntou se `main()` sempre recebe o arquivo de entrada -- na verdade não: sem `argv[1]`
(ou com um JSON que falhasse ao carregar), `Simulation::load()` caía silenciosamente num modelo
sintético de catenária parabólica (geometria de conveniência histórica do risersim, sem
equivalente real de ANFLEX). Pediu pra remover essa opção -- entrada sempre via arquivo real -- e,
já que o `<output_dir>` da Fase 5 sempre foi o mesmo diretório do arquivo de entrada em todo
caminho de chamada existente (`run_worker.py`: `input_json = run_dir / "input_simulation.json"`;
`run_from_aml.py`: `input_json_path = out_dir / "input_simulation.json"`), derivar esse diretório
automaticamente a partir do próprio caminho de entrada em vez de recebê-lo como argumento
separado.

- **C++**: `Simulation::load()` perdeu o branch de fallback inteiro -- se o JSON não existir/não
  parsear, `parsed_from_json` fica `false` (erro já impresso por `ModelBuilder::load_from_json()`)
  e não há mais geometria sintética alternativa. `ModelBuilder::build_synthetic_fallback()` era
  chamado só por esse branch (confirmado via grep -- zero outros call sites em produção ou
  bindings Python) -- removido inteiro (declaração + definição), não só desligado.
  `tests/test_static_analysis.cpp` tem sua própria cópia local da mesma geometria
  (`build_synthetic_catenary_model()`, nunca chamou o método do ModelBuilder), então nenhum teste
  foi afetado.
- **`main.cpp`**: `argc < 2` agora é erro fatal (`Uso: ... <input.json> [nome_arquivo_resultado]`,
  retorna 1) em vez de deixar `input_json_path` vazio pro fallback absorver. O antigo 2º argumento
  (`output_dir`) foi removido -- `main()` deriva o diretório de saída direto do `parent_path()` do
  caminho de entrada (`std::filesystem`); o antigo 3º argumento (nome do arquivo de resultado,
  opcional) virou o 2º. Novo uso: `risersim_test_main <input.json> [nome_arquivo_resultado]`. Se
  `sim.parsed_from_json` for `false` depois de `load()`, `main()` retorna 1 sem chamar
  `run()`/`export_results()`.
- **Python**: todo call site do binário precisou dropar o argumento de `output_dir` (posição
  mudou -- passar a pasta ali agora seria interpretado como nome de arquivo de resultado):
  `run_worker.py` (`cmd = [exe, input_json, results_filename]`, `run_dir` removido -- já era o
  diretório do próprio `input_json`), `run_from_aml.py` (`cmd = [exe, input_json_path]`, já estava
  dentro de `out_dir`), e `risersim_runner.py::run_simulation_subprocess()` (função exportada mas
  sem call sites hoje -- corrigida por consistência, mesmo padrão).

**Verificação real** (2026-08-16): rebuild local de `risersim_test_main` + `risersim_tests`,
Catch2 361/361 sem regressão. CLI: sem argumentos → erro de uso, exit 1, sem geometria sintética;
caminho inexistente → erro de carga, exit 1; caminho de entrada real (`Exemplo_01a`, estático,
`T_eff≈217,3 kN` -- mesmo valor histórico já validado nesta sessão) com e sem nome de arquivo
explícito, ambos gravando `.h5` no mesmo diretório do `input_simulation.json`, incluindo o caso de
caminho relativo sem diretório (`has_parent_path() == false` → usa `.`). Docker rebuild +
`docker compose up -d --build web worker`; rodada real disparada via API (`Near`, `exemplo-01a`,
force=true por já existir uma rodada com o mesmo `model_hash`) convergiu normalmente --
`Exemplo_01a_Near_results.h5` apareceu sozinho no diretório da rodada, junto de
`input_simulation.json`, sem `output_dir` separado. `run_from_aml.py` (workflow manual, fora do
Docker) testado contra o `.aml` real do Exemplo_01a com `--static-only`, reproduzindo
`T_eff≈217,3 kN` e gerando `catenary_results.h5` ao lado do `input_simulation.json` gerado.

#### Fase 7 — ✅ IMPLEMENTADA: nomear também o arquivo de entrada pelo caso (mesma regra do de saída)

Usuário pediu a mesma regra de nome do `results_filename` (Fase 5/6) pro arquivo de entrada
per-rodada -- até aqui o snapshot congelado do config que o solver efetivamente lê continuava
com o nome genérico `input_simulation.json` dentro de `runs/<run_id>/`, enquanto o resultado ao
lado já se chamava `<Projeto>[_<Caso>]_results.h5`. Escopo continua o mesmo da Fase 5/6: só o
arquivo POR RODADA, dentro de `runs/<run_id>/` -- o `input_simulation.json` de NÍVEL DE PROJETO
(`projects/<id>/input_simulation.json`, o "config atual" fora de qualquer rodada específica, sem
caso associado) fica como estava, sem mudança.

- **`risersim_projects.py::create_run()`**: computa `stem = "_".join([projeto, caso])` uma vez só
  e deriva os dois nomes a partir dele -- `input_filename = f"{stem}_input.json"` e
  `results_filename = f"{stem}_results.h5"` (mesma sanitização via `_sanitize_filename_component`
  já usada pra `results_filename`). O snapshot é gravado direto sob `input_filename` (não mais
  `input_simulation.json`); o campo novo `input_filename` é gravado em `run.json` junto do já
  existente `results_filename`.
- **`run_worker.py`**: lê `run.get("input_filename")` (com fallback pra `"input_simulation.json"`
  -- rodadas criadas antes desta fase não têm o campo) em vez do nome fixo, e passa esse caminho
  como `argv[1]` pro binário.
- **`run_server.py`**: `RESULT_FILENAMES` (dict estático que só tinha a entrada
  `"input_simulation.json"`) removido inteiro -- virou código morto assim que o nome parou de ser
  fixo. `api_run_results()` agora valida `filename` contra os DOIS campos dinâmicos do run
  (`input_filename` OU `results_filename`), não mais um allowlist estático + um campo dinâmico.
- **`preprocessor_app.js::resolveInputUrl()`**: virou assíncrona, mesmo padrão de
  `app.js::resolveResultsUrl()` -- no caminho `?project=&run=`, busca `GET .../runs/<run_id>`
  primeiro pra ler o `input_filename` real, então monta a URL com esse nome. Diferença importante
  em relação ao dos resultados: o arquivo de entrada é gravado SÍNCRONA e IMEDIATAMENTE na
  criação da rodada (antes do solver sequer começar), então não precisa checar `status` como
  `resolveResultsUrl()` faz -- só falha se a rodada em si não existir.
- `WEB_VERSION`: `1.5.0` → `1.6.0` (novo campo em `run.json`, nova exigência de nome dinâmico na
  rota de resultados).

**Verificação real** (2026-08-16, Docker): rodada real disparada via API (`Cross`, `exemplo-01a`,
force=true): resposta da criação já veio com `input_filename:"Exemplo_01a_Cross_input.json"` e
`results_filename:"Exemplo_01a_Cross_results.h5"`; após convergir, o diretório da rodada só tinha
esses dois arquivos (mais `run.json`/`stdout.log`) -- nenhum `input_simulation.json`/
`catenary_results.h5` genérico. Rota de resultados: nome real do input → 200; nome real dos
resultados → 200; `input_simulation.json` (nome genérico antigo) → 404; `catenary_results.h5`
(idem) → 404 -- confirma que a rota não aceita mais nomes fixos, só os dois nomes reais desta
rodada. Pós-processador de ENTRADA (`preprocessor.html?project=&run=`) carregado via harness
Chrome headless real sobre essa rodada -- 501 nós / 500 elementos, `inputValid: true`, sem erro no
console.

## Backlog de recursos faltantes do motor (não bloqueante — puxar sob demanda)

Achados documentados em `mapa_aml_exemplos_e_web_interface.md`, nenhum implementado: boias/tendões
como entidade própria (hoje só existe `BuoyancyModule`/`BendRestrictor`, modificadores locais não
lidos por `ModelBuilder::load_from_json`), conexões articuladas tipo flexjoint/drilljoint, turret
com movimento prescrito 6-GDL por caso de carga (`Turret.aml`), ruptura de elemento em tempo de
execução dinâmico (`Ruptura.aml`), verificação de código DNV como pós-processamento
(`DNV_Check.aml`). Cada um só vale a pena quando um caso de teste real concreto precisar dele — não
faz sentido implementar especulativamente. (Múltiplas linhas com corpo flutuante compartilhado —
implementado e com convergência estática resolvida pra águas muito profundas/linhas muito longas,
ver Eixo 2c.)

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
