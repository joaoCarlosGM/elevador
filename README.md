# Elevador — ESP32-C3 Mini + L293D

Firmware experimental para um **elevador de bancada**, com motor DC, dois fins de curso, botão local, LED onboard e painel de telemetria Wi-Fi. Não usar para transportar pessoas/animais nem operar carga suspensa sem retenção mecânica independente.

> **Endereços:** `http://192.168.4.1/` mostra a **página de controle real** com SUBIR, DESCER e PARAR; `http://192.168.4.1/dashboard` é o diagnóstico detalhado. O acionamento pela web é opcional e **desabilitado por padrão**. Se o comissionamento e o modo web não estiverem habilitados, SUBIR/DESCER ficam indisponíveis; o arquivo HTML aberto localmente oferece apenas simulação.

## 1. Pinagem padrão (ESP32-C3 Mini)

Os pinos funcionais são editáveis no **`idf.py menuconfig` → `Elevador — Configuração do Hardware`**.

| GPIO ESP32-C3 | Nome funcional | Ligação externa | Comportamento |
|---|---|---|---|
| GPIO1 | `LIMIT_BOTTOM` | Fim de curso inferior, NC → GND | HIGH = acionado/contato aberto |
| GPIO10 | `LIMIT_TOP` | Fim de curso superior, NC → GND | HIGH = acionado/contato aberto |
| GPIO20 | `BUTTON` | Botão momentâneo NO → GND | LOW = pressionado |
| GPIO3 | `MOTOR_IN1` | Entrada 1A do L293D (pino 2) | Sinal direcional |
| GPIO4 | `MOTOR_IN2` | Entrada 2A do L293D (pino 7) | Sinal direcional |
| GPIO8 | `STATUS_LED` | LED integrado **se confirmado** | Usualmente ativo LOW no C3 SuperMini; desabilitado por padrão |
| GPIO21 | **Livre/reservado** | UART0 TX em certas configurações | Sem função de motor |
| GPIO2 | **Reservado** | Strapping de boot | Não usar por padrão |
| GPIO18/19 | **Reservados** | USB Serial/JTAG da placa, se disponível | Programação e monitoramento |

**Importante:** “ESP32-C3 Mini” nomeia várias placas diferentes. Antes de ativar GPIO8 como LED, conferir o esquema/serigrafia da sua versão; em algumas placas o LED não é um LED GPIO convencional.

## 2. L293D: mapa físico do circuito

| L293D DIP-16 | Nome | Destino/função |
|---|---|---|
| 1 | EN1,2 | Habilitação da ponte usada **fixa em HIGH no hardware** — não conectada ao ESP32 |
| 2 | 1A / IN1 | ESP32-C3 GPIO3 |
| 3 | 1Y / OUT1 | Um dos terminais do motor DC |
| 4 e 5 | GND | Terra comum |
| 6 | 2Y / OUT2 | Outro terminal do motor DC |
| 7 | 2A / IN2 | ESP32-C3 GPIO4 |
| 8 | VCC2 | Alimentação adequada ao motor |
| 9 | EN3,4 | LOW para manter a segunda ponte desabilitada |
| 10 e 15 | 3A e 4A | Entradas da segunda ponte em LOW definido |
| 12 e 13 | GND | Terra comum |
| 16 | VCC1 | Alimentação lógica do L293D, usualmente 5 V |

O L293D possui **duas alimentações distintas**: VCC1 (lógica) e VCC2 (motor), ambas necessárias mesmo quando você só usa a primeira ponte. O ESP32-C3 trabalha com lógica de **3,3 V** e seus GPIOs **não toleram 5 V diretamente**. Dimensionar alimentação, capacidade de corrente, dissipação e desacoplamento conforme os datasheets e as medições do motor.

| IN1 | IN2 | Estado da ponte (EN fixo HIGH) |
|---|---|---|
| 0 | 0 | Frenagem elétrica comandada, sem desenergização independente |
| 1 | 0 | Sentido A (subir/descer conforme polaridade física) |
| 0 | 1 | Sentido B |
| 1 | 1 | Frenagem elétrica (não usada pelo firmware) |

**O firmware não possui GPIO de ENABLE.** A frenagem por `IN1=IN2=0` não sustenta com garantia uma carga vertical, principalmente em perda de energia. Utilizar retenção mecânica e proteção independente de fim de curso; testar com a potência do motor desconectada antes de qualquer teste energizado.

## 3. Sensores e botão

- **Fim superior:** contato normalmente fechado (NC) do microinterruptor entre **GPIO10** e **GND**; usar pull-up a 3,3 V (habilitado pelo firmware). Contato aberto/acionado é interpretado como HIGH.
- **Fim inferior:** mesmo circuito, no **GPIO1**.
- **Botão local:** contato normalmente aberto (NO) entre **GPIO20** e **GND**; LOW indica pressão.
- Interrupções em ambas as bordas e debounce são tratados pelo firmware. Fio NC partido pode ser lido como sensor acionado: isso **não** substitui um circuito de segurança independente.

## 4. Wi-Fi e painel

1. Após gravar firmware, conectar o celular ou computador à rede SoftAP **`Elevador`**.
2. Senha padrão de desenvolvimento: **`12345678`** (trocar em `menuconfig` para uso fora de bancada).
3. Abrir **`http://192.168.4.1/`** para ver os controles e o estado do elevador.
4. Abrir **`http://192.168.4.1/dashboard`** para diagnóstico detalhado de GPIOs.
5. `GET /api/status` e `GET /api/health` permanecem disponíveis como APIs de leitura; não são páginas HTML.

O dashboard mostra fins de curso, botão, IN1/IN2, LED, temporizações, calibração, falhas e animação de posição **estimada**, não medição com encoder.

O arquivo `main/control.html` também pode ser aberto isoladamente como demonstração simulada. No ESP32, os comandos reais passam exclusivamente por POST `/api/control/up`, `/api/control/down` e `/api/control/stop`, com validação do estado e dos fins de curso. **Segure** SUBIR/DESCER para manter o movimento. Soltar envia PARAR e a ausência de comandos por mais de 600 ms (padrão configurável) aciona frenagem e trava a falha. O servidor não altera GPIO diretamente.

## 5. Compilar (ESP-IDF 5.3.1)

```sh
idf.py set-target esp32c3
idf.py menuconfig
idf.py build
idf.py -p COM5 flash monitor
```

Substituir `COM5` pela porta do seu dispositivo. Em checkout já configurado, evitar executar `set-target` desnecessariamente para não perder `sdkconfig`.

**Valores iniciais seguros:** tempos de percurso mínimos/máximos começam em zero, impedindo movimento até que limites físicos sejam configurados. Para habilitar o modo remoto, é necessário em `menuconfig`: (1) indicar tempos mínimos e máximos reais por sentido; (2) confirmar sentido do motor e polaridade dos sensores com a fonte do motor desligada; (3) marcar `ELEVATOR_LIMITS_CONFIRMED` **somente após verificação física**; (4) desabilitar os modos automático e supervisionado; (5) alterar a senha padrão do AP para outra senha WPA2 forte; e (6) habilitar `ELEVATOR_WEB_CONTROL_ENABLED`, apenas quando houver proteção física e desligamento independente. O modo web manual não precisa da calibração NVS por ciclos, mas exige NVS inicializável e todos os demais intertravamentos. Uma vez gravada uma falha de movimento ou parada manual durante percurso, não há religamento até reinicialização com origem segura conhecida. Isso não substitui retenção mecânica nem parada de potência.

## 6. Organização

```text
elevador/
├── AGENTS.md
├── README.md
├── CMakeLists.txt
├── sdkconfig.defaults
├── main/
│   ├── CMakeLists.txt
│   ├── Kconfig.projbuild
│   ├── main.c
│   ├── index.html
│   └── control.html
└── tests/
    └── validate.py
```

A lógica do dispositivo reside exclusivamente em `main/main.c`; as duas páginas HTML/CSS/JS independentes são `main/index.html` (diagnóstico) e `main/control.html` (controles), ambas embutidas via `EMBED_TXTFILES`. A pasta `build/` e o `sdkconfig` local são ignorados pelo Git.

## 7. Limitações e validação

- Comandos da página: **reais apenas quando o modo web for habilitado e os intertravamentos permitirem**; no modo local `file://`, somente simulação.
- O controle HTTP não aciona saídas diretamente; somente a máquina de estados temporizada escreve IN1/IN2.
- O botão PARAR comanda **frenagem lógica**, não corte independente da alimentação. Perda de Wi-Fi provoca parada pela lógica, desde que o ESP32 e a ponte estejam funcionando corretamente.
- A maquete não deve transportar pessoas ou animais; movimento inesperado ou falha elétrica exigem desligamento físico da potência antes de novos testes.
- Nenhum teste em hardware está implícito pela compilação ou pelo runner de mocks.
- A segurança de uma carga suspensa não pode depender exclusivamente do ESP32 e do L293D, principalmente com EN fixo HIGH.

## 8. Intertravamentos do controle web

- Posição inicial reconhecida obrigatoriamente em **um dos fins de curso**, não no meio do percurso.
- Os dois fins ativos, nenhum fim ativo na partida, sinal inconsistente, tempo máximo excedido ou escrita GPIO falha: **FAULT travada**.
- SUBIR somente a partir da base, DESCER somente a partir do topo; sem reversão durante a marcha.
- Comunicação perdida, soltura do botão ou PARAR durante movimento: pede `IN1=IN2=0`, registra falha travada e exige nova inspeção.
- Modo web e modo automático são mutuamente exclusivos; não há subida automática após 10 segundos nesse modo.
- **Resistor pull-up/pull-down é escolha elétrica, não inversão da definição lógica**: se a sua fiação está ligada entre GPIO e GND, mantenha `GPIO_PULLUP_ENABLE` e confira `LIMIT_*_ACTIVE_LEVEL` pela leitura real. A lógica padrão NC abre=HIGH; altere o nível ativo somente se a topologia do interruptor exigir. Não faça merge no PR #4 (pull-down para 3V3) sem alterar fisicamente os contatos.
