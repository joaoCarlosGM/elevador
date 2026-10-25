# Maquete de elevador — ESP32-C3 Mini + L293D

Controlador para **maquete sem pessoas/animais**, com um único sensor superior HOME. A posição inferior é um tempo de descida salvo na NVS; não existe sensor inferior, botão físico ou dashboard secundário. Página única: **http://192.168.4.1/**.

## Ligações

| GPIO / pino | Função | Ligação |
|---|---|---|
| ESP32-C3 GPIO3 | Motor IN1 | L293D DIP-16 pino 2 |
| ESP32-C3 GPIO4 | Motor IN2 | L293D DIP-16 pino 7 |
| ESP32-C3 GPIO10 | Fim HOME superior | Microinterruptor NC ligado ao GND |
| GPIO1 e GPIO20 | Não utilizados | Sem sensores ou botões |
| GND ESP32 | Referência lógica | GND comum com L293D |

L293D: pino 1 (EN1,2) = HIGH fixo; pinos 3 e 6 para o motor; pinos 4,5,12,13 = GND; pino 8 (VCC2) = alimentação do motor; pino 16 (VCC1) = alimentação lógica apropriada, em geral 5 V; EN da segunda ponte = LOW, suas entradas em nível definido. Os GPIOs ESP32 nunca recebem 5 V.

O sensor HOME tem pull-up interno: contato NC **fechado = LOW / HOME livre** e **aberto = HIGH / HOME acionado**. O nível ativo é configurável na página, mantendo a topologia de pull-up. O fio rompido pode parecer HOME acionado. O software não identifica essa falha de forma absoluta.

Use pull-downs externos de aproximadamente 10 kΩ em IN1/IN2 para assegurar LOW antes do boot. Com ENABLE fixo em HIGH, IN1=IN2=0 apenas solicita frenagem; não corta a alimentação nem garante retenção. Antes de operar motor, conferir níveis com potência desconectada, ter desligamento de potência independente e batentes/retenção mecânicos.

## Controle web e calibração

1. Conectar ao AP **Elevador** com a senha personalizada no menuconfig. Abrir http://192.168.4.1/.
2. **ENCONTRAR HOME**: um toque inicia subida com limite de tempo até o sensor superior.
3. **CALIBRAR MÍNIMO**: no HOME, habilita medição. Depois tocar em **DESCER**.
4. **SET MÍNIMO**: ao alcançar a posição inferior desejada, para, grava o tempo de descida na NVS, espera o intervalo necessário à inversão do motor e retorna automaticamente ao HOME.
5. **DESCER** normal: desce pelo tempo salvo; ao atingir o mínimo, espera **10 segundos** e sobe ao HOME.
6. **SUBIR**: retorna ao HOME antes dos 10 segundos. **PARAR**: solicita frenagem e, se interromper um trajeto, invalida a estimativa de posição; é necessário reencontrar HOME.

O sentido de rotação para SUBIR (GPIO3 HIGH ou GPIO4 HIGH) e a polaridade ativa do sensor (HIGH/LOW) podem ser editados e **salvos na NVS pela página**. Uma mudança invalida o tempo inferior antigo; repetir HOME e calibração.

A posição por tempo é estimada: tensão, atrito, carga e travamento podem causar erro acumulado. Sem segundo sensor não existe confirmação física de que o ponto inferior foi alcançado. O software limita os tempos de busca HOME e calibração; isso não substitui desligamento de potência independente.

## Build / gravação com ESP-IDF 5.3.1

Com o terminal ESP-IDF aberto na pasta do projeto, execute em sequência:

- git switch main
- git pull --ff-only origin main
- idf.py menuconfig
- idf.py build
- idf.py -p COM5 flash monitor

Troque COM5 pela porta serial. A opção **Elevador — Maquete com HOME superior** permite definir pinos, limites de tempo e senha Wi-Fi. **O firmware impede movimento com senha padrão 12345678** e requer ativar **ELEVATOR_HARDWARE_VERIFIED**, que somente deve ser marcada **após validação real da parada elétrica, do sensor HOME e de proteção mecânica independente**. Essa opção não cria proteção física.

Configuração de GPIO antiga no sdkconfig local precisa ser revisada no menuconfig. Não usar set-target em um checkout já configurado sem necessidade.

## Organização

- main/main.c — único firmware, NVS, Wi-Fi, API e máquina de estados
- main/control.html — única página (CSS/JS incorporados, sem internet)
- main/Kconfig.projbuild — pinos e timeouts de segurança
- tests/validate.py — testes do núcleo de controle com GPIO/relógio simulados
- .github/workflows/host-tests.yml — testes e build no GitHub Actions

**Sem teste físico, não há garantia de parada ou funcionamento elétrico.** Maquete supervisionada apenas. Nunca utilizar em transporte de pessoas ou animais.
