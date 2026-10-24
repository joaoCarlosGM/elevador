# AGENTS.md — Elevador com ESP32-C3 Mini e L293D

## Papel do Codex

Você é o agente de desenvolvimento deste repositório. Implemente um firmware real, compilável e auditável para um **pequeno elevador experimental de bancada**, usando **ESP-IDF 5.3.1**, alvo **esp32c3**, ponte H **L293D**, dois fins de curso, um botão físico, LED onboard e uma interface web em modo SoftAP.

**Não se limite a explicar:** implemente os arquivos, compile, revise o comportamento e relate evidências. Trabalhe incrementalmente, preserve o que existir no repositório e não faça push ou reescrita de histórico sem autorização explícita.

### Restrições de arquitetura

1. Toda a **lógica de aplicação** deve residir em `main/main.c`, em **C**, e não em C++.
2. Todo HTML, CSS e JavaScript da interface deve residir em **um único** `main/index.html`, abrível diretamente no navegador em modo de demonstração.
3. Não crie outros arquivos `.c`, `.cpp`, `.h`, `.js`, `.css` nem bibliotecas externas da aplicação.
4. Preserve somente os arquivos auxiliares obrigatórios do ESP-IDF: `CMakeLists.txt` da raiz, `main/CMakeLists.txt`, `main/Kconfig.projbuild` e `AGENTS.md`; `sdkconfig`, `sdkconfig.defaults`, `.gitignore` e saídas de build podem existir conforme as convenções do ESP-IDF. **É impossível oferecer configurações Kconfig sem um arquivo Kconfig e compilar sem CMakeLists; esses arquivos são exceções necessárias à restrição de arquivo único.**
   Para testes reproduzíveis do PR, permite-se também tests/validate.py: runner sem lógica de aplicação, que gera mocks e executáveis fora do checkout. Não adicionar outras fontes da aplicação.
5. Use apenas bibliotecas nativas do ESP-IDF (GPIO, Wi-Fi, NVS, esp_timer, esp_http_server, logs, etc.), sem Arduino e sem bibliotecas de terceiros.
6. Não crie tarefas FreeRTOS próprias. O próprio ESP-IDF, Wi-Fi, `esp_timer` e servidor HTTP usam FreeRTOS internamente, o que não pode ser evitado. Prefira ISR mínima + callbacks curtos do `esp_timer` (por exemplo a cada 10 ms) + handlers HTTP de leitura.
7. Implementação pequena, legível, com nomes explícitos, funções estáticas bem delimitadas, documentação breve e macros `#define` apenas quando agregarem clareza.
8. Firmware do elevador é para mecanismo de bancada **sem transporte de pessoas ou animais**. Não presuma capacidade de retenção de carga pelo motor, pela engrenagem ou pelo L293D. Exigir proteção mecânica independente e supervisão nos testes de comissionamento.

## 1. Hardware e GPIOs: defaults sujeitos à confirmação física

Placa declarada: **ESP32-C3 Mini**. No formato *SuperMini* usual há LED azul em GPIO8 ativo em nível BAIXO; porém existem outras placas comercializadas como “C3 Mini”. Antes de confiar no LED, verificar serigrafia/esquema. Não presumir LED RGB/WS2812.

| Nome funcional | Símbolo Kconfig desejado | GPIO padrão | Tipo | Ligação |
|---|---|---:|---|---|
| Sensor fim de curso inferior | `CONFIG_ELEVATOR_LIMIT_BOTTOM_GPIO` | 1 | Entrada IRQ | Microinterruptor NC para GND, pull-up 3,3 V |
| Sentido do motor A | `CONFIG_ELEVATOR_MOTOR_IN1_GPIO` | 3 | Saída | L293D pino 2 (1A) |
| Sentido do motor B | `CONFIG_ELEVATOR_MOTOR_IN2_GPIO` | 4 | Saída | L293D pino 7 (2A) |
| LED de status onboard | `CONFIG_ELEVATOR_STATUS_LED_GPIO` | 8 | Saída | LED integrado, **se confirmado** |
| Sensor fim de curso superior | `CONFIG_ELEVATOR_LIMIT_TOP_GPIO` | 10 | Entrada IRQ | Microinterruptor NC para GND, pull-up 3,3 V |
| Botão único | `CONFIG_ELEVATOR_BUTTON_GPIO` | 20 | Entrada IRQ | Botão NO para GND, pull-up 3,3 V |
| Reservado | — | 2 | — | GPIO de *strapping*; não usar por padrão |

O ESP32-C3 usa GPIO20/GPIO21 por padrão para **UART0 RX/TX**. Nesta proposta, GPIO20 é botão e GPIO21 não é utilizado pela aplicação; programar/depurar pela **USB Serial/JTAG nativa nos GPIO18/GPIO19**, se disponível. Não usar UART0 nesses pinos enquanto conflitar com o botão; validar também qualquer remapeamento de IN1/IN2. Evite GPIO2, GPIO8 e GPIO9 para novos botões/sensores por serem pinos de strapping; GPIO8 apenas LED onboard confirmado. Não usar 18/19 para outra função.

**L293D, primeira ponte:** pino 1 EN1,2 permanentemente em HIGH no hardware, sem conexão a GPIO; pino 2 IN1 -> GPIO3; pino 7 IN2 -> GPIO4; pinos 3/6 -> motor; pino 16 VCC1 -> lógica 5 V; pino 8 VCC2 -> alimentação apropriada do motor; pinos 4/5/12/13 -> GND comum. Desabilitar segunda ponte (pino 9 em LOW) e fixar entradas IN3/IN4 em nível definido. **VCC2 é obrigatório mesmo usando só uma ponte.** Os sinais de 3,3 V do ESP32-C3 são compatíveis com entradas lógicas TTL do L293D conforme datasheet, mas **nunca aplicar 5 V nos GPIOs ESP32-C3**. Validar corrente de partida/travamento do motor, aquecimento e perdas da ponte antes dos testes.

**Proteção elétrica:** IN1 e IN2 precisam de pull-downs externos para GND (dimensionados para o circuito, tipicamente ~10 kΩ) para estados baixos durante reset, antes de o firmware assumir os pads. O firmware pré-carrega ambos os latches em zero e configura IN1/IN2 como saídas com pull-down interno; isso não comprova o estado elétrico durante ROM/reset, falha de alimentação ou pino avariado. Com ENABLE fixo em HIGH, **IN1=IN2=0 comanda frenagem elétrica**, sem alta impedância ou desenergização independente por software. Essa frenagem não garante retenção estática da carga, principalmente sem alimentação. Exigir retenção mecânica, proteção independente de fim de curso e corte físico da potência da ponte acessível ao operador. Adicionar desacoplamento adequado e nunca aplicar 5 V nos GPIOs. Sensores/botão usam 3,3 V; avaliar proteção contra ruído. Validar corrente de partida/travamento e aquecimento do L293D.


## 2. Kconfig editável via `idf.py menuconfig`

Criar em `main/Kconfig.projbuild` um menu `Elevador — Configuração do Hardware`. Todos os nomes de GPIO, lógica ativa e parâmetros relevantes precisam ser visíveis e editáveis. Organizar em seções descritivas:

- **Pinos**: os cinco GPIOs de entradas/motor e o LED onboard; habilitar/desabilitar LED, e permitir LED ativo em nível LOW/HIGH.
- **Entradas**: sensores NC por padrão; estados lógicos ativos parametrizáveis; botão ativo LOW por padrão.
- **Antirruído**: debounce de fins de curso (ex.: default 35 ms) e botão (ex.: default 50 ms), tempo de amostragem (ex.: 10 ms).
- **Temporizações**: espera inferior antes de subir (default **10000 ms**), tempos mínimos permitidos independentes por sentido, tempos máximos independentes por sentido, intervalo mínimo entre inversões do motor.
- **Conectividade**: SSID `Elevador`, senha `12345678` (mínimo 8 caracteres WPA2), número máximo de clientes, intervalo de atualização web.
- **Comissionamento**: modo de teste supervisionado e opção para habilitar operação automática **somente após** calibração validada. Expor claramente na UI quando ainda não calibrado.

Os `#define` em `main.c` devem padronizar aliases para `CONFIG_ELEVATOR_...`, nomes de estados, durações e níveis lógicos, sem duplicar os valores mágicos. Validar no boot valores duplicados, GPIOs não suportados, seleção de strapping indevida, tempo máximo <= mínimo, senha inválida e conflitos com USB/JTAG/UART. Em erro de configuração, bloquear movimento e manter IN1=IN2=0 quando o mapa puder ser aplicado com segurança; publicar falha em log/interface se possível.

Documentar como mudar todos os pinos sem editar `main.c`.

## 3. Arquitetura de interrupção, debounce e entradas

- Registrar **interrupções em ambas as bordas** (`GPIO_INTR_ANYEDGE`) para os dois fins de curso e para o botão.
- ISR deve ser **mínima**, sem `printf`, sem alocação, sem Wi-Fi, sem HTTP, sem NVS e sem bloqueios; apenas marcar eventos/flags e registrar, se seguro, timestamp. Não tomar decisão de direção dentro da ISR.
- Uma única lógica central reavalia os níveis brutos e consolida estados **estáveis** após debounce com `esp_timer_get_time()` (tempo monotônico), sem `delay` bloqueante.
- `esp_timer` periódico pode coordenar reamostragem, máquina de estados e LED desde que o callback seja extremamente curto e não execute acesso HTTP/NVS nem bloqueie; quando necessário, deferir operações lentas aos contextos apropriados sem criar tarefas próprias.
- Priorizar comando IN1=IN2=0 (frenagem elétrica) ao receber **sinal de limite potencialmente acionado**, sem aguardar a janela de debounce para continuar forçando um limite. Depois, validar o estado estável.
- Manter `raw_level`, `debounced_active`, `last_change_ms`, `edge_count` e contadores de eventos para cada entrada; exibir níveis brutos e filtrados na API.
- Button de contato normalmente aberto (NO), ligado ao GND: apenas transição debounced para *pressionado* gera comando. Pressionado continuamente não dispara comandos repetidos.
- Fins de curso NC para GND com pull-up: circuito fechado = leitura LOW, circuito aberto (pressionado ou cabo rompido) = leitura HIGH. **Um fio rompido pode parecer um fim de curso acionado; detecção absoluta de falhas requer redundância/hardware adicional.** Não alegar cobertura de falhas que o hardware não detecta.

## 4. Driver do motor — somente IN1 e IN2

O ENABLE do L293D está permanentemente habilitado no hardware. Não existe GPIO, Kconfig, escrita de pad ou cartão para esse sinal.

- IN1=1, IN2=0: direção A.
- IN1=0, IN2=1: direção B.
- IN1=0, IN2=0: **frenagem elétrica comandada**, ambas as saídas da ponte em nível baixo; não representa alta impedância nem corte independente de energia.
- IN1=1, IN2=1: não comandar neste projeto.

Atribuir UP/DOWN de forma configurável em Kconfig. Somente uma função pode comandar movimento direcional, verificando inicialização, origem conhecida, destino livre, falhas, calibração/modo supervisionado, prazos e intervalo mínimo entre comandos. Antes de trocar direção, comandar ambos baixos e aguardar o intervalo configurado, sem delays no callback. Em falha, comandar IN1=IN2=0 no contexto de controle e travar FAULT.

Não existe desenergização independente por software. Falha no MCU, no L293D ou na alimentação pode impedir a frenagem ou provocar movimento. Exigir corte físico de potência e retenção mecânica independente. Frenagem elétrica não garante que uma carga vertical não caia.

## 5. Máquina de estados explícita

Utilizar `typedef enum` e `switch`, com pelo menos:

- `BOOT`
- `NEEDS_COMMISSIONING`
- `IDLE_TOP`
- `IDLE_BOTTOM`
- `WAIT_AUTO_UP` (contagem de 10 segundos)
- `MOVING_UP`
- `MOVING_DOWN`
- `FAULT`

Estados e transições:
1. No reset: inicializar GPIOs mantendo IN1=IN2=0 (frenagem elétrica), carregar parâmetros NVS e validar mapa de pinos, inicializar debounce, LED e AP. Durante boot o LED pode emitir padrão rápido de *startup* sem ligar o motor.
2. **Ambos os fins ativos**: `FAULT_BOTH_LIMITS`, IN1=IN2=0 (frenagem elétrica), travado.
3. **Somente superior ativo**: `IDLE_TOP`, IN1=IN2=0 (frenagem elétrica). Botão pressionado validado -> descer se comissionado, sem erros.
4. **Somente inferior ativo**: entrar em `WAIT_AUTO_UP` e mostrar contagem real **10 -> 0 segundos**; só depois subir automaticamente **se** comissionado, sem falhas e com condições seguras. Se faltar calibração, `NEEDS_COMMISSIONING` (sem partida automática).
5. **Nenhum fim ativo no boot**: localização desconhecida (`FAULT_UNKNOWN_POSITION`), IN1=IN2=0 (frenagem elétrica). Não assumir automaticamente que está no meio do percurso e não iniciar homing cego. A exigência “ir para cima ao ligar” tem precedência apenas quando existe origem segura conhecida; esse compromisso é intencional.
6. Durante movimento: a liberação do fim de partida é esperada; ambos livres é estado normal de trânsito. O acionamento validado do destino encerra o movimento.
7. Pressionar o botão durante movimento deve **ser registrado na UI**, porém não inverter repentinamente nem criar reativação. Definir política simples (ignorar até completar o curso; não fazer reversão no ar).
8. Ao parar no limite, IN1=IN2=0 (frenagem elétrica) e atualizar a posição discreta superior/inferior.
9. Uma nova descida exige pressionamento válido do botão com elevador no topo. Na base, obedecer à espera automática definida.
10. Qualquer falha crítica permanece travada até reinicialização controlada e diagnóstico; o reset não deve produzir partida inesperada se a falha continuar.
11. Não criar endpoints HTTP que acionem motor; a web é **somente monitoramento**, salvo autorização futura para controle remoto com projeto de segurança apropriado.

### Falhas a implementar, pelo menos

- `BOTH_LIMITS_ACTIVE`
- `UNKNOWN_POSITION_AT_BOOT`
- `GPIO_CONFIGURATION`
- `GPIO_WRITE_FAILURE` (escrita recusada pelo driver, comando não confirmado)
- `TRAVEL_TOO_FAST` (destino acionou antes do tempo mínimo plausível)
- `TRAVEL_TIMEOUT` (destino não acionou antes do máximo)
- `UNEXPECTED_LIMIT`
- `LIMIT_SENSOR_INCONSISTENT` (sequência fisicamente incompatível com direção)
- `START_LIMIT_NOT_RELEASED` (não liberou dentro da janela configurada)
- `NVS_OR_CALIBRATION_INVALID`
- `AP_OR_HTTP_FAILURE` (informar; não afetar intertravamento de motor de forma insegura)

Toda falha deve indicar código, descrição curta, estado no momento e tempo em log/API.

## 6. Tempos de percurso, memória e intertravamento

O tempo de percurso é uma informação **experimental**, não um sensor absoluto de posição.

- Cronometrar da ordem de movimento ao acionamento do destino por sentido.
- Usar `esp_timer_get_time()` (microsegundos, monotônico) para todas as medidas.
- Definir pelo Kconfig limites físicos **mínimo e máximo** por direção. Não inventar valores calibrados para este motor.
- Exigir procedimento explícito de comissionamento supervisionado antes de habilitar movimentação automática. Primeiras corridas controladas devem verificar ligação de sentido, endpoints, frenagem física, limite superior de tempo e consistência dos sinais.
- Após **ciclos válidos e supervisionados**, gravar em NVS, separadamente, os menores tempos válidos observados para subida/descida, contagem de ciclos e versão/checksum dos dados. Validar CRC/schema ou formato equivalente antes de usar. Jamais gravar tempo de percurso inválido ou reduzir o limite de segurança com base em uma corrida com falha.
- Tempo mínimo efetivo deverá ser conservador: nunca abaixo do mínimo físico configurado. Não disparar falha de “rápido demais” por bounce antes de consolidar sinal, mas interromper energização prontamente diante do limite bruto.
- Se o curso exceder o máximo ou ocorrer sequência impossível, parar e travar. Dados NVS ausentes/corrompidos impedem automático.
- A interface deve diferenciar claramente **tempo medido**, **mínimo histórico válido**, **limite mínimo de segurança**, **limite máximo de segurança**, **tempo da viagem atual** e **estado de calibração**.
- O tempo de viagem não constitui medição instantânea de posição; animação entre extremos é **estimada**.

## 7. LED onboard

Criar rotina central sem delays bloqueantes, com polaridade configurável:

| Situação | Sinalização |
|---|---|
| Boot | Pulsos rápidos de inicialização, depois padrão do estado |
| Deslocamento entre fins de curso (`MOVING_UP`/`MOVING_DOWN`) | Pisca continuamente (ex.: 250 ms ON, 250 ms OFF) |
| Parado no superior/inferior | Aceso fixo (padrão de descanso; pode ser configurável) |
| Espera de 10 s na base | Pulso curto periódico distinto |
| `FAULT` / `NEEDS_COMMISSIONING` | Código de pisca distinto, jamais confundido com deslocamento |

Não afirmar que “ambos fins livres” = trânsito seguro quando isso ocorrer **sem** movimento ordenado. Se ambos livres no boot, é posição desconhecida e deve haver FAULT, não sinalização de trânsito normal.

## 8. SoftAP Wi-Fi + HTTP (offline)

- `WIFI_MODE_AP` com SSID **Elevador**, senha **12345678** default, WPA2-PSK, IP normalmente **192.168.4.1**; SSID/senha configuráveis via Kconfig.
- Inicializar NVS, `esp_netif`, event loop padrão, driver Wi-Fi e DHCP do AP; sem conexão STA, sem internet, sem serviços externos.
- Servidor HTTP nativo `esp_http_server`; usar no mínimo `GET /`, `GET /api/status`, `GET /api/health`.
- `GET /` -> servir o único `main/index.html` da flash como `text/html; charset=utf-8`.
- `GET /api/status` -> JSON pequeno com valores reais do firmware, sem cache e sem impedir o controle do motor. Exemplo de contrato:
  ```
  {
    "state":"IDLE_BOTTOM",
    "motion":"STOPPED",
    "position":"BOTTOM",
    "calibrated":true,
    "error":{"code":"NONE","message":""},
    "gpio":{
      "limit_top":{"pin":10,"raw":0,"active":false,"edges":5},
      "limit_bottom":{"pin":1,"raw":1,"active":true,"edges":7},
      "button":{"pin":20,"raw":1,"active":false,"edges":12},
      "motor_in1":{"pin":3,"level":0},
      "motor_in2":{"pin":4,"level":0},
      "onboard_led":{"pin":8,"level":1}
    },
    "auto_up_remaining_ms":10000,
    "travel":{"direction":"NONE","elapsed_ms":0,"minimum_up_ms":null,
              "minimum_down_ms":null,"maximum_up_ms":null,"maximum_down_ms":null},
    "uptime_ms":1000
  }
  ```
  A resposta real deve trazer dados verdadeiros, com formatos e estados coerentes, e sem apontar valores medidos fictícios.
- Usar `snprintf` com buffer delimitado e validar tamanho. Evitar heap em ciclo crítico. Não fazer cálculos lentos de JSON/HTTP em ISR ou timer callback.
- Habilitar headers HTTP para evitar cache do status.
- Não habilitar atualização de firmware OTA, upload, comandos de movimento ou configuração por HTTP nesta etapa.

### Inclusão do HTML sem converter a string à mão

No `main/CMakeLists.txt`, usar `idf_component_register(SRCS "main.c" INCLUDE_DIRS "." EMBED_TXTFILES "index.html" PRIV_REQUIRES esp_wifi esp_netif nvs_flash esp_http_server esp_timer driver ... )`, corrigindo a lista exata de dependências para ESP-IDF 5.3.1. Referenciar em `main.c` os símbolos gerados, normalmente `_binary_index_html_start` e `_binary_index_html_end`, validados na compilação. Definir uma macro curta, por exemplo `#define ELEVATOR_INDEX_HTML ((const char *)index_html_start)` e comprimento pelo símbolo end. **Não usar string literal gigante, script de conversão, geração de cabeçalho ou arquivo `web.h`.** `index.html` deve funcionar tanto embutido na ROM quanto aberto isoladamente no navegador.

## 9. UI/UX responsiva e auditoria de GPIOs

Estética de um dashboard moderno com fundo escuro, tipografia legível, botões não enganosos (nenhum botão “subir/descer” se a web não controla o motor), brilho/glow verde indicando sinal ativo, âmbar para espera, vermelho para falha. Tudo em **`main/index.html`**, com CSS em `<style>` e JS em `<script>`, sem CDN, imagens remotas ou dependências.

Áreas:
1. Cabeçalho “Elevador • ESP32-C3 Mini”, conexão AP e saúde da API.
2. Animação central do poço e cabine, andando para cima/baixo. Posição superior/inferior confirmada por sensores; em trânsito animar **estimativa temporal** e exibir “posição estimada, sem encoder”.
3. Estado principal (`BOOT`, `AGUARDANDO`, `SUBINDO`, `DESCENDO`, `SUPERIOR`, `INFERIOR`, `FALHA`, `CALIBRAÇÃO NECESSÁRIA`) e contagem de 10 s com progresso.
4. Painel diagnóstico: cartões DIV para `GPIO_LIMIT_TOP`, `GPIO_LIMIT_BOTTOM`, `GPIO_BUTTON`, `GPIO_MOTOR_IN1`, `GPIO_MOTOR_IN2` e `GPIO_STATUS_LED`, cada um com número do GPIO, nível bruto, estado lógico, timestamp/event count e glow verde **somente quando realmente ativo**.
5. Tempos de viagem e mínimos/máximos; histórico compacto de eventos/sensores na sessão do browser (não fingir logs históricos do firmware).
6. Falhas com explicação legível e motivo do bloqueio de motor.
7. Rodapé com última atualização e indicação se dados são reais ou simulação local.

Integração:
- Atualizar `/api/status` por `fetch` de ~250 a 500 ms, `cache:'no-store'`. Se cair comunicação, mostrar **SEM TELEMETRIA** e congelar ou neutralizar indicadores; nunca manter glow verde como se fosse confirmação atual.
- Ao abrir `index.html` por `file://`, permitir **modo demonstração** claro e rotulado, com dados simulados para revisão visual; os dados não devem ser confundidos com hardware real.
- Animação em CSS/transições suaves; usar `prefers-reduced-motion`.
- Layout responsivo prioritário para celular de 320–430 px, sem rolagem horizontal; painel de diagnóstico legível no touch, com alturas adequadas e bom contraste.
- Escapar textos dinâmicos, evitar HTML injetado e depender apenas da API local.

## 10. Critérios de aceitação / matriz de testes

Codex deve testar ou disponibilizar roteiro reproduzível dos seguintes casos, sem energizar uma carga real durante testes de software:

1. `idf.py set-target esp32c3`, `idf.py menuconfig`, `idf.py build` sem erro na versão ESP-IDF 5.3.1.
2. Projetos/arquivos sem outras fontes da aplicação além de `main/main.c` e `main/index.html`; somente CMake/Kconfig/metadados obrigatórios.
3. `menuconfig` altera todos os GPIOs, polaridades e tempos; valores inválidos travam motor.
4. Pré-carregar IN1/IN2 em zero antes de configurar saídas, ativar pull-downs internos; exigir pull-downs externos e medir estados reais no reset/boot. ENABLE permanece habilitado no hardware.
5. Dois fins acionados => FAULT; nenhum no boot => FAULT; só superior => topo; só inferior => espera se calibrado.
6. Na base, temporizador de **10,000 ms** antes de subida automática; não mover se não calibrado.
7. Bounce em ambas bordas dos sensores não causa partidas múltiplas; borda bruta de limite interrompe a energização prontamente.
8. Botão com bounce gera no máximo 1 comando por pressão; long press não repete, press durante movimento é refletido no painel mas não inverte.
9. Destino antes do mínimo plausível => IN1=IN2=0 (frenagem elétrica) + `TRAVEL_TOO_FAST`; sem destino até máximo => `TRAVEL_TIMEOUT`.
10. Se fim de origem não liberar no limite, falhar; sequência contraditória dos limites também.
11. Frenagem IN1=IN2=0 em repouso/falhas; nunca comandar ambos altos; intervalo entre movimentos sem desenergização independente.
12. LED pisca durante trânsito e usa padrões distintos em idle, espera, falha e boot; LED físico deve ser conferido na placa.
13. AP aparece como **Elevador** com senha **12345678**, interface acessível por `http://192.168.4.1`.
14. Web exibe glow correto para **GPIO bruto versus sinal lógico ativo**, e atualização em tempo real do botão, motor e sensores.
15. Layout renderiza sem dependência de internet num viewport 360 × 800 e também em desktop.
16. `index.html` abre localmente com demo explicitamente indicada; embutido serve dados reais.
17. Desligamento/reinicialização durante movimento não gera retomada cega e automática.
18. NVS corrompida/ausente não libera automático.
19. Comunicação Wi-Fi lenta, muitos refreshes e desconexões não atrasam a proteção do motor.

### Desenvolvimento e Git

- Inspecionar primeiro o estado do repositório, `git status`, arquivos existentes e configuração Git antes de editar.
- Entregar primeiro um projeto mínimo compilável, depois Kconfig/GPIO/ISR, máquina de estados, NVS, Wi-Fi/API e HTML.
- Ao final executar `idf.py build` no ambiente apropriado, registrar resultados. Se o compilador não estiver disponível, declarar **não testado**, jamais afirmar “PASS”.
- Reportar claramente o que é simulado, testado em software e testado fisicamente.
- Não usar `git add .`, não incluir `build/`, `sdkconfig` com configuração local confidencial, nem credenciais adicionais por acidente.
- Não realizar commits/push automaticamente sem aprovação do operador.
- Não inverter pinos, polaridades, sentidos do motor ou retirar bloqueios de segurança para “fazer funcionar”.
- Nunca declarar pronto para uso desacompanhado sem validação elétrica, dos fins de curso e da retenção mecânica.

## 11. Documentação de referência

- GPIO C3: https://docs.espressif.com/projects/esp-idf/en/v5.3.1/esp32c3/api-reference/peripherals/gpio.html
- Build ESP-IDF / EMBED_TXTFILES: https://docs.espressif.com/projects/esp-idf/en/v5.3.1/esp32c3/api-guides/build-system.html
- HTTP Server: https://docs.espressif.com/projects/esp-idf/en/v5.3.1/esp32c3/api-reference/protocols/esp_http_server.html
- ESP Timer: https://docs.espressif.com/projects/esp-idf/en/v5.3.1/esp32c3/api-reference/system/esp_timer.html
- ESP32-C3 datasheet: https://documentation.espressif.com/esp32-c3_datasheet_en.html

## 12. Operação, testes reproduzíveis e auditoria do PR

Aplicação exclusivamente em main/main.c e main/index.html; EMBED_TXTFILES embute
o HTML. Sem tarefas FreeRTOS próprias: esp_timer controla GPIO/debounce/estados;
a tarefa app_main do IDF realiza NVS e logs. tests/validate.py é o único runner,
sem lógica de aplicação; gera todos os mocks/executáveis fora do checkout.

### Preparação e compilação

Usar ESP-IDF 5.3.1, alvo esp32c3. Neste ambiente:

~~~bash
export IDF_TOOLS_PATH=/workspace/.espressif/tools
export PATH=/workspace/.espressif/host-venv/bin:$PATH
. /workspace/.espressif/esp-idf-v5.3.1/export.sh
cd /workspace/elevador
idf.py --version
# Somente em checkout novo, sem configuração local a preservar:
idf.py set-target esp32c3
idf.py menuconfig
idf.py build
~~~

Editar pinos e parâmetros em menuconfig → Elevador — Configuração do Hardware.
Defaults: IN1=3, IN2=4, inferior=1, superior=10, botão=20, LED=8 (desabilitado
até confirmação física). GPIO21 não é utilizado. Tempos físicos mínimo/máximo
começam em zero deliberadamente, provocando GPIO_CONFIGURATION até configuração.
Automático, supervisionado e confirmação dos limites começam desabilitados.

Com mapa válido, pré-carregar ambos os latches IN1/IN2 em zero **antes** de
configurar qualquer dessas saídas. Ambas recebem pull-down interno; entradas,
pull-up e interrupção ANYEDGE. Mesmo com tempos inválidos, o mapa válido permite
observar sensores e manter frenagem comandada. Mapa inválido não é aplicado.
Não confundir comando com medição elétrica: estados antes do firmware exigem
pull-down externo e validação física, inclusive em remapeamentos para pads
usados pela ROM/UART.

### Comissionamento e persistência

1. Com potência desconectada, verificar sensores NC, GND, VCC1/VCC2, ENABLE fixo,
   pull-downs IN1/IN2, polaridades, LED8 e sentido físico.
2. Com retenção mecânica e operador, determinar limites físicos mínimo/máximo
   por sentido e prazo de liberação da origem. Configurar tempos, confirmar
   proteção/sentido/limites e habilitar SUPERVISED_TEST, sem AUTOMATIC_ENABLED.
3. Posicionar manualmente em um fim conhecido, sem potência. Nenhum ou ambos
   os fins no boot travam o sistema; não há homing cego.
4. Segurar botão por ARM_HOLD_MS (3000 ms) arma sem mover; soltar e fazer nova
   pressão para cada percurso, inclusive da base. Pressões em trânsito ou
   antes do intervalo não ficam enfileiradas. A web nunca comanda o motor.
5. Completar REQUIRED_CYCLES por sentido (dois), conferir contagens, mínimos
   e pending_save:false. Apenas viagens válidas e supervisionadas são gravadas,
   com motor parado em frenagem comandada; gravação pendente impede partida.
6. Após confirmação física, desabilitar supervisionado, habilitar automático,
   recompilar e gravar preservando NVS. Esses modos não alteram o fingerprint.
   Na base esperar 10000 ms; no topo uma pressão inicia descida.

Calibração usa **schema versão 2**: dados do circuito anterior são rejeitados,
exigindo novo comissionamento, sem promoção automática. Validar CRC, schema,
fingerprint de pinos/polaridades/sentido/tempos e contagens. NVS ausente permite
apenas comissionamento supervisionado; corrupção, schema antigo ou configuração
incompatível travam NVS_OR_CALIBRATION_INVALID. Não há apagamento automático.
Para recuperação deliberada: desconectar potência, diagnosticar, usar
idf.py -p PORT erase-flash (destrói dados), regravar com
idf.py -p PORT flash monitor e repetir integralmente o comissionamento.

A parada bruta comanda frenagem na primeira observação de destino ou borda
registrada entre amostras, antes do debounce; bounce não provoca religamento.
A latência depende do escalonamento IDF, sem garantia hard real-time. O histórico
mínimo nunca reduz o mínimo físico de segurança.

### API e dashboard

GET / serve HTML; GET /api/status retorna exatamente seis GPIOs: limit_top,
limit_bottom, button, motor_in1, motor_in2 e onboard_led. O objeto motor informa
command (BRAKE/UP/DOWN/UNKNOWN), brake_commanded, inputs_defined e
independent_shutdown:false. BRAKE é comando, sem medição de velocidade,
corrente ou retenção. Entradas não amostradas usam sampled:false; saídas mostram
comandos, eventos e timestamps, sem realimentação elétrica.
GET /api/health retorna 200 saudável ou 503 em falha.
Sem endpoints de movimento, upload, configuração ou OTA.

A interface distingue bruto/ativo, real/simulado, tempos medidos/históricos/
limites, posição confirmada/estimada e perda de telemetria. Identifica 00 como
frenagem elétrica, sem corte independente. Sem cartão fictício de ENABLE.
file:// seleciona demonstração explicitamente.

### Testes reproduzíveis

Pré-requisitos: Python 3.10+, GCC com AddressSanitizer/UBSan, Node.js,
Playwright 1.62.1 e Chromium. Dependências são apenas de teste; exemplo isolado:

~~~bash
mkdir -p /tmp/elevador-test-tools
npm install --prefix /tmp/elevador-test-tools --no-save --no-package-lock playwright@1.62.1
NODE_PATH=/tmp/elevador-test-tools/node_modules CHROMIUM_PATH=/usr/bin/chromium python3 tests/validate.py --browser
~~~

Sem --browser, executa controle, configuração, GPIO/ISR, NVS e handlers.
Com --browser, falha se Node/Playwright/Chromium não estiverem disponíveis.
Para reter evidências fora do checkout, adicionar --output /tmp/elevador-evidence.
Compilação cruzada é check separado: idf.py build.

O harness inclui o mesmo main.c, com GPIO/relógio/ISR/sincronização simulados.
Cobre ambos os sentidos e fins, bounce/pulsos entre amostras, botão longo/
repetido/em trânsito, espera exata, mínimo/máximo, origem presa, sequências
inválidas, intervalo, saídas 00 em falhas e ausência de escrita no GPIO21.
Confere pré-carga, pull-downs/pull-ups e registro ANYEDGE em initialize_gpio.
NVS usa backend simulado para load/set_blob/commit/reinício/CRC/schema antigo
e falhas. HTTP executa handlers reais com transporte simulado; não testa rádio,
flash física nem servidor executando no ESP32.

No Chromium: 320/360/430/1280 px, seis GPIOs, perda de telemetria, contrato
inválido, texto escapado e reduced-motion. Se file:// for bloqueado pela
política, registrar a limitação e testar o ramo demo com seleção simulada,
sem afirmar abertura direta validada.

### Validações físicas pendentes e riscos

**Nenhum teste físico foi realizado.** Não declarar aprovados GPIO elétrico,
LED, rádio/AP/DHCP, NVS real, motor, frenagem ou retenção.

- Medir GPIO3/GPIO4 desde power-on/reset/ROM/boot, sem potência inicialmente.
  Conferir pull-downs externos, polaridades, pulsos e sequência de movimento.
- ENABLE fixo implica ausência de desligamento independente e alta impedância
  por software. Falhas de MCU/L293D/alimentação podem provocar movimento ou
  perda de frenagem. Exigir corte físico de potência e retenção mecânica.
- Validar fins, ruído/bounce, botão, origem, viagem cedo/timeout, reset em
  trânsito, NVS ausente/corrompida e latência de frenagem sob carga Wi-Fi.
- Com retenção/supervisão, validar sentido, corrente/travamento/aquecimento,
  espera de 10 s, AP Elevador/WPA2, GETs, vários clientes e desconexões.
- Fio NC rompido segue indistinguível de fim acionado sem hardware extra.
  Não transportar pessoas/animais; não operar sem supervisão.

O PR deve incluir arquivos, comandos/resultados e limitações. ChatGPT audita
antes do merge; somente o operador realiza merge e inicia os testes físicos.
Não realizar merge automaticamente.

### Resultado da validação de software desta entrega (2026-10-24)

- ESP-IDF 5.3.1 / esp32c3: build aprovado, sem avisos/erros do compilador;
  imagem de aplicação cabe na partição de 1 MiB, com 17% livres.
- Runner versionado com --browser: 36 cenários no automático e 36 no
  supervisionado aprovados, mais seis checks com limites não confirmados,
  um check de tempos zerados e 11 variantes inválidas rejeitadas.
- Inicialização GPIO, interrupções/pulls, NVS load/write/commit/reload/falhas,
  três handlers HTTP, JSON/no-store/CSP/buffer e contrato de seis GPIOs aprovados
  nos mocks descritos acima; ASan/UBSan não reportaram falhas.
- Chromium: quatro larguras sem overflow, perda de telemetria, texto escapado,
  reduced-motion, contrato inválido e ramo demo simulando seleção aprovados.
  Abertura direta por file:// permanece não validada por política do navegador.
- Testes físicos: **não executados**. Esta aprovação é somente de software;
  não comprova funcionamento elétrico, frenagem ou segurança do mecanismo.

### Correção da auditoria do PR #1 — falhas de escrita GPIO

output_level verifica esp_err_t e só atualiza o valor aceito, contadores e
timestamp após ESP_OK. Falha trava FAULT, registra GPIO/erro/contexto e impede
novas partidas até reinicialização. O latch de falha não escreve GPIOs, evitando
recursão. motor_start só retorna sucesso/entra MOVING depois de todas as escritas
necessárias; em partida parcial tenta rollback para 00 uma vez.
motor_brake tenta ambos os pinos mesmo se o primeiro falhar e não retorna
sucesso se houve erro. Uma parada recusada não registra viagem/calibração válida.

Mesmo após uma tentativa posterior aceita, a incerteza da falha permanece
travada até reboot. A API usa UNKNOWN, brake_commanded:false, level:null,
active:false e write_ok:false nas saídas afetadas por essa incerteza; expõe
gpio_write_failed/gpio_pin/gpio_error. A interface não mostra movimento,
frenagem ou glow ativo como confirmados. Sucesso do driver é apenas aceitação
do comando lógico, nunca medição elétrica ou garantia de frenagem/retenção.

O runner injeta erros persistentes/transitórios no preload, pré-partida,
acionamento dos dois sentidos, partida parcial, parada por destino/timeout e
LED, além de verificar os handlers e o dashboard com um pino simulado preso
em HIGH após parada recusada. Validação local de software, não CI aprovado;
evidências da nova rodada são publicadas no PR. Nenhum teste físico realizado.
Uma partida parcial pode produzir energização antes do rollback; sem GPIO de
ENABLE, o firmware não oferece corte independente se a escrita de parada falhar.
