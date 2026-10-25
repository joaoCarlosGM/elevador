# Contrato do projeto — Elevador maquete ESP32-C3 / L293D

## Escopo
- Um HOME superior NC para GND em GPIO10, com pull-up; nível ativo padrão HIGH.
- GPIO3 L293D IN1 e GPIO4 IN2, ambos baixos para frenagem comandada.
- Remover fim inferior GPIO1 e botão físico GPIO20 da lógica.
- Uma única página HTTP GET / em main/control.html; não criar dashboard extra.
- Firmware único main/main.c, ESP-IDF 5.3.1 C.
- Limite inferior virtual definido por **tempo de descida**, não sensor.
- Comandos web de um toque SUBIR, DESCER e PARAR.
- CALIBRAR MÍNIMO apenas a partir do HOME; DESCER mede até SET MÍNIMO.
- SET MÍNIMO grava NVS e automaticamente retorna HOME com dwell entre sentidos.
- Ao atingir mínimo normalmente, retorna ao HOME após 10 s.
- Se parar em posição intermediária, o software marca UNKNOWN; novo movimento normal requer HOME.
- Direção do motor e estado ativo do sensor ajustáveis na página e persistidos na NVS.
- Mudar configuração de motor ou sensor invalida o mínimo antigo.

## Segurança técnica
- HTTP envia intenções; somente controller_tick escreve GPIO.
- Sem movimento automático no boot com HOME desconhecido.
- HOMING e calibração possuem timeouts e sensor deve liberar ao descer.
- Falhas e erros de GPIO travam estado FAULT.
- L293D EN fixo HIGH significa que IN1=IN2=0 não equivale a corte de potência independente.
- Manter seleção ELEVATOR_HARDWARE_VERIFIED OFF até avaliação real de parada elétrica, batentes e retenção mecânica.
- Não sugerir que tempo inferior é sensor de fim físico.
- Não transportar pessoas/animais.

## Entrega
Revisar CI e testes host antes de merge. Não commitar build/ nem sdkconfig local. Usar commits específicos por arquivo e não alegar testes físicos inexistentes.
