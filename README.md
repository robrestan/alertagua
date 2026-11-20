# AlertAgua

Sistema IoT de monitoreo y alerta temprana para parámetros de calidad del agua

AlertAgua mide turbidez relativa, sólidos disueltos totales (TDS) y temperatura mediante sensores conectados a un microcontrolador ESP32. Registra las mediciones con fecha y hora en una tarjeta microSD y ofrece una interfaz web local, accesible sin conexión a internet. Cuando una medición supera alguno de los umbrales configurados, el sistema muestra una alerta en la interfaz y activa una señal sonora.

El proyecto facilita el seguimiento preventivo y la detección de variaciones en el agua almacenada

**No sustituye los análisis de laboratorio ni controles sanitarios oficiales**

## Funcionalidades

- Lectura periódica ajustable de temperatura, TDS y turbidez relativa
- Registro de mediciones en la tarjeta microSD
- Registro de eventos, como alertas o lecturas inválidas
- Punto de acceso Wi-Fi propio para consultar el sistema localmente sin conexión a Internet
- Página web con valores actuales, estado del dispositivo e historial gráfico del día
- Descarga del archivo de mediciones en formato CSV
- Ajuste de fecha y hora del reloj RTC desde la interfaz web
- Configuración de umbrales e intervalos desde la tarjeta microSD

## Hardware utilizado

- ESP32
- Sensor de turbidez TSW-20M
- Sensor de TDS V1.0
- Sensor de temperatura DS18B20
- Módulo RTC DS3231 junto a una celda CR2025
- Módulo de tarjeta microSD compatible con SPI
- Buzzer
- Tarjeta microSD

### Asignación de pines

| Componente | Señal | GPIO del ESP32 |
|---|---|---:|
| Sensor de turbidez | Salida analógica | 34 |
| Sensor de TDS | Salida analógica | 35 |
| DS18B20 | Datos OneWire | 32 |
| Buzzer | Salida digital | 25 |
| RTC DS3231 | SDA | 21 |
| RTC DS3231 | SCL | 22 |
| microSD | CS | 5 |
| microSD | SCK | 18 |
| microSD | MISO | 19 |
| microSD | MOSI | 23 |

## Requisitos de software

- Arduino IDE
- Paquete para tarjetas **esp32 by Espressif Systems** instalado desde el Gestor de tarjetas de Arduino IDE
- Librerías externas requeridas:
  - **RTClib**
  - **OneWire**
  - **DallasTemperature**

## Instalación y carga del programa

1. Clona o descarga este repositorio
2. Abre en Arduino IDE el archivo `.ino` del proyecto
3. Instala el soporte para ESP32 desde **Herramientas > Placa > Gestor de tarjetas**, buscando `esp32` e instalando el paquete de Espressif Systems
4. Instala las bibliotecas requeridas desde el gestor de librerías
5. Conecta el ESP32 al computador por USB
6. Selecciona la placa ESP32 que corresponda a tu módulo y el puerto serial correcto
7. Compila y carga el programa con los botones **Verificar** y **Subir**.
8. Abre el monitor serial a **115200 baudios** para revisar los mensajes de inicialización y posibles errores. Si la consola comienza a imprimir mediciones, el proyecto esta funcionando correctamente

## Configuración de la tarjeta microSD

Formatea la tarjeta microSD con un sistema de archivos FAT32. Para cambiar los parámetros sin modificar el programa, crea en la raíz de la tarjeta un archivo llamado `config.txt`.

Ejemplo:

```ini
server_ip=192.168.4.1
limite_tds_ppm=500
limite_turbidez_pct=50
limite_temperatura_c=30
intervalo_lectura_ms=10000
intervalo_historial_ms=30000
```

Si `config.txt` no existe, se utilizan los valores predeterminados del código:

| Parámetro | Valor predeterminado |
|---|---:|
| IP del punto de acceso | `192.168.4.1` |
| Límite de TDS | `500 ppm` |
| Límite de turbidez relativa | `50%` |
| Límite de temperatura | `30°C` |
| Intervalo de lectura | `10000 ms` |
| Intervalo de historial en la página | `30000 ms` |

## Uso

1. Inserta la tarjeta microSD con el archivo `config.txt` opcional
2. Enciende el ESP32 y espera a que inicialice los sensores, el RTC, la microSD y el punto de acceso
3. Desde un teléfono o computador, conéctate a la red Wi-Fi **AlertAgua**
4. Abre un navegador y visita `http://192.168.4.1` o la dirección configurada en `config.txt`
5. La página mostrará el estado del dispositivo y las mediciones disponibles. También permite ajustar la fecha y hora del RTC y descargar los datos obtenidos hasta la fecha

La contraseña del punto de acceso está como `UNAD2026` y pueden ser modificada junto al SSID desde el código fuente

## Archivos de datos

- `mediciones.csv`: Mediciones con las columnas `fecha_hora`, `temperatura_C`, `TDS_ppm` y `turbidez_relativa_pct`.
- `eventos.txt`: Alertas, lecturas inválidas y otros eventos del sistema.

Los gráficos de la interfaz corresponden a las mediciones del día actual. La descarga del archivo CSV entrega el archivo completo de mediciones disponible en la tarjeta.

El sistema marca una alerta cuando una medición **supera** el umbral configurado. La indicación visual se presenta en la página web y la alerta sonora se genera con el buzzer.

## Consideraciones de diseño

- Las mediciones requieren que los sensores estén conectados y funcionen correctamente
- La fecha y hora dependen del módulo RTC DS3231. Si el RTC no está disponible o no tiene una fecha válida, las lecturas no se guardan como mediciones válidas
- El acceso a la interfaz se realiza mediante la red local creada por el ESP32, por lo que no se requiere internet
- La función de ajuste del RTC del código valida años desde 2026 en adelante. Si se necesita manejar fechas anteriores, esa condición debe modificarse en el programa
