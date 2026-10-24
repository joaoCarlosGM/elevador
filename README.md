# Elevador — ESP32-C3 Mini + L293D

Firmware experimental para um **elevador de bancada**, com motor DC, dois fins de curso, botão local, LED onboard e painel de telemetria Wi-Fi. Não usar para transportar pessoas/animais nem operar carga suspensa sem retenção mecânica independente.

> **Endereços:** `http://192.168.4.1/` abre o **painel de controle** com botões SUBIR, DESCER e PARAR. `http://192.168.4.1/dashboard` continua sendo o **dashboard de diagnóstico**. Os botões **não acionam o motor real**: ficam desabilitados quando a página roda no ESP32 e são interativos apenas na demonstração offline do arquivo `main/control.html`. Não existe endpoint HTTP de movimento; o botão físico continua responsável pela atuação.

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

Os arquivos `main/control.html` e `main/index.html` podem ser abertos localmente no navegador, em demonstração explícita. Nenhum botão remoto é habilitado quando servidos pelo ESP32.

## 5. Compilar (ESP-IDF 5.3.1)

```sh
idf.py set-target esp32c3
idf.py menuconfig
idf.py build
idf.py -p COM5 flash monitor
```

Substituir `COM5` pela porta do seu dispositivo. Em checkout já configurado, evitar executar `set-target` desnecessariamente para não perder `sdkconfig`.

**Valores iniciais seguros:** tempos de percurso mínimos/máximos começam em zero e motor fica bloqueado até configurar limites físicos válidos, confirmar a pinagem, e realizar comissionamento supervisionado conforme `AGENTS.md`. A operação automática requer calibração NVS válida e habilitação explícita.

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

- Comandos da página: **simulação**, não movimentam o motor real.
- Controle real do motor: **botão físico e máquina de estados local**; sem controle via HTTP.
- Nenhum teste em hardware está implícito pela compilação ou pelo runner de mocks.
- A segurança de uma carga suspensa não pode depender exclusivamente do ESP32 e do L293D, principalmente com EN fixo HIGH.
