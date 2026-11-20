/*
  AlertAgua
  Sistema de monitoreo y alerta temprana para parámetros de calidad del agua.
*/

#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <RTClib.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <SPI.h>
#include <SD.h>
#include <math.h>

// Asignacion de pines GPIO
const int PIN_TURBIDEZ = 34, PIN_TDS = 35;
const int PIN_TEMPERATURA = 32, PIN_BUZZER = 25;
const int PIN_SD_CS = 5, PIN_SD_SCK = 18;
const int PIN_SD_MISO = 19, PIN_SD_MOSI = 23;

// Configuracion de punto de acceso
const char* AP_SSID = "AlertAgua";
const char* AP_PASS = "UNAD2026";
IPAddress IP_SERVIDOR(192, 168, 4, 1);
IPAddress MASCARA_RED(255, 255, 255, 0);

// Limites por defecto, modificables desde la memoria microSD
float LIMITE_TDS_PPM = 500.0;
float LIMITE_TURBIDEZ_PCT = 50.0;
float LIMITE_TEMPERATURA_C = 30.0;

unsigned long INTERVALO_MS = 10000;
unsigned long INTERVALO_HISTORIAL_MS = 30000;

// Umbrales internos y configuracion
const float VOLTAJE_AGUA_LIMPIA = 3.0;
const float VOLTAJE_MAX_TURBIDEZ = 0.5;

WebServer server(80);
RTC_DS3231 rtc;
OneWire oneWire(PIN_TEMPERATURA);
DallasTemperature sensorTemp(&oneWire);

bool rtcOK = false, sdOK = false, hayUltimaMedicion = false;
unsigned long ultimaLectura = 0;

struct Medicion {
  String fecha;
  float temperatura, ppm, turbidezPct;
};

Medicion ultimaMedicion;

void aplicarConfiguracion(const String& elemento, const String& valor) {
  if (elemento == "server_ip") {
    IPAddress ip;
    if (ip.fromString(valor)) IP_SERVIDOR = ip;
    else Serial.println("IP inválida; se conserva la IP por defecto.");
    return;
  }

  float n = valor.toFloat();

  if (elemento == "limite_tds_ppm" && isfinite(n) && n > 0 && n <= 1000000)
    LIMITE_TDS_PPM = n;
  else if (elemento == "limite_turbidez_pct" && isfinite(n) && n >= 0 && n <= 100)
    LIMITE_TURBIDEZ_PCT = n;
  else if (elemento == "limite_temperatura_c" && isfinite(n) && n > -50 && n < 125)
    LIMITE_TEMPERATURA_C = n;
  else if (elemento == "intervalo_lectura_ms") {
    long ms = valor.toInt();
    if (ms >= 1000 && ms <= 3600000) INTERVALO_MS = ms;
  } else if (elemento == "intervalo_historial_ms") {
    long ms = valor.toInt();
    if (ms >= 5000 && ms <= 3600000) INTERVALO_HISTORIAL_MS = ms;
  }
}

void cargarConfiguracion() {
  if (!sdOK) {
    Serial.println("No se pudo leer config.txt, microSD no disponible.");
    return;
  }
  if (!SD.exists("/config.txt")) {
    Serial.println("No existe /config.txt, se usarán los valores por defecto.");
    return;
  }

  File archivo = SD.open("/config.txt", FILE_READ);
  if (!archivo) {
    Serial.println("No se pudo abrir /config.txt.");
    return;
  }

  while (archivo.available()) {
    String linea = archivo.readStringUntil('\n');
    linea.trim();

    if (linea.isEmpty() || linea.startsWith("#") || linea.startsWith(";"))
      continue;

    int igual = linea.indexOf('=');
    if (igual < 1) continue;

    String elemento = linea.substring(0, igual);
    String valor = linea.substring(igual + 1);
    elemento.trim();
    valor.trim();
    aplicarConfiguracion(elemento, valor);
  }
  archivo.close();

  Serial.println("Configuración cargada:");
  Serial.printf("IP del servidor web: %s\n", IP_SERVIDOR.toString().c_str());
  Serial.printf("Límite TDS: %.1f ppm\n", LIMITE_TDS_PPM);
  Serial.printf("Límite turbidez: %.1f %%\n", LIMITE_TURBIDEZ_PCT);
  Serial.printf("Límite temperatura: %.1f °C\n", LIMITE_TEMPERATURA_C);
  Serial.printf("Intervalo de lectura: %lu ms\n", INTERVALO_MS);
  Serial.printf("Intervalo de historial: %lu ms\n", INTERVALO_HISTORIAL_MS);
}

float leerPromedio(int pin) {
  uint32_t suma = 0;
  for (int i = 0; i < 20; i++) {
    suma += analogRead(pin);
    delay(3);
  }
  return suma / 20.0;
}

String fechaHoraActual() {
  if (!rtcOK) return "RTC_NO_VALIDO";

  DateTime ahora = rtc.now();
  char texto[20];
  snprintf(texto, sizeof(texto), "%02d-%02d-%04d %02d:%02d:%02d",
           ahora.day(), ahora.month(), ahora.year(),
           ahora.hour(), ahora.minute(), ahora.second());
  return String(texto);
}

String fechaActual() {
  if (!rtcOK) return "";

  DateTime ahora = rtc.now();
  char texto[11];
  snprintf(texto, sizeof(texto), "%02d-%02d-%04d",
           ahora.day(), ahora.month(), ahora.year());
  return String(texto);
}

float calcularTurbidezRelativa(float voltaje) {
  float diferencia = VOLTAJE_MAX_TURBIDEZ - VOLTAJE_AGUA_LIMPIA;
  if (fabs(diferencia) < 0.001) return 0;

  float porcentaje =
      (voltaje - VOLTAJE_AGUA_LIMPIA) / diferencia * 100.0;
  return constrain(porcentaje, 0.0, 100.0);
}

// Calculo de TDS utilizando la formula sugerida por el fabricante
float calcularTDS(float voltaje, float temperatura) {
  float compensacion = 1.0 + 0.02 * (temperatura - 25.0);
  if (compensacion <= 0) compensacion = 1.0;

  float v = voltaje / compensacion;
  float ppm = (133.42 * pow(v, 3) - 255.86 * pow(v, 2) + 857.39 * v) * 0.5;
  return max(0.0f, ppm);
}

// Verifica que los datos a entregar sean coherentes
// Para esto se hace una comprobacion de que la cadena
// sea 19 o mas y que los valores de fecha no sean nulos
bool analizarFilaCSV(const String& linea, Medicion& m) {
  int c1 = linea.indexOf(',');
  int c2 = linea.indexOf(',', c1 + 1);
  int c3 = linea.indexOf(',', c2 + 1);
  if (c1 < 0 || c2 < 0 || c3 < 0) return false;

  m.fecha = linea.substring(0, c1);
  m.temperatura = linea.substring(c1 + 1, c2).toFloat();
  m.ppm = linea.substring(c2 + 1, c3).toFloat();
  m.turbidezPct = linea.substring(c3 + 1).toFloat();

  return m.fecha.length() >= 19 &&
         isfinite(m.temperatura) &&
         isfinite(m.ppm) &&
         isfinite(m.turbidezPct);
}

// Revisa en la microSD si existe una medicion del mismo dia y la carga
void cargarUltimaMedicionDelDia() {
  if (!sdOK || !rtcOK || !SD.exists("/mediciones.csv")) return;

  String hoy = fechaActual();
  File archivo = SD.open("/mediciones.csv", FILE_READ);
  if (!archivo) return;

  while (archivo.available()) {
    String linea = archivo.readStringUntil('\n');
    linea.trim();

    if (!linea.startsWith(hoy)) continue;

    Medicion m;
    if (analizarFilaCSV(linea, m)) {
      ultimaMedicion = m;
      hayUltimaMedicion = true;
    }
  }
  archivo.close();
}

// Si un sensor pasa el umbral, se llama esta funcion para registrar el hecho
void guardarEvento(const String& evento) {
  if (!sdOK) return;

  File archivo = SD.open("/eventos.txt", FILE_APPEND);
  if (!archivo) return;

  archivo.print(fechaHoraActual());
  archivo.print(" - ");
  archivo.println(evento);
  archivo.close();
}

void guardarMedicion(const Medicion& m) {
  if (!sdOK) return;

  bool nueva = !SD.exists("/mediciones.csv");
  File archivo = SD.open("/mediciones.csv", FILE_APPEND);

  if (!archivo) {
    guardarEvento("No se pudo abrir mediciones.csv");
    return;
  }

  if (nueva)
    archivo.println("fecha_hora,temperatura_C,TDS_ppm,turbidez_relativa_pct");

  archivo.print(m.fecha);
  archivo.print(",");
  archivo.print(m.temperatura, 2);
  archivo.print(",");
  archivo.print(m.ppm, 1);
  archivo.print(",");
  archivo.println(m.turbidezPct, 1);
  archivo.close();
}

// Determina si se ha pasado el umbral
bool hayAlerta(const Medicion& m) {
  return m.temperatura > LIMITE_TEMPERATURA_C ||
         m.ppm > LIMITE_TDS_PPM ||
         m.turbidezPct > LIMITE_TURBIDEZ_PCT;
}

void tomarMedicion() {
  sensorTemp.requestTemperatures();
  float temperatura = sensorTemp.getTempCByIndex(0);

  float adcTurbidez = leerPromedio(PIN_TURBIDEZ);
  float adcTDS = leerPromedio(PIN_TDS);
  float voltTurbidez = adcTurbidez * (3.3 / 4095.0);
  float voltTDS = adcTDS * (3.3 / 4095.0);

  Medicion m;
  m.fecha = fechaHoraActual();
  m.temperatura = temperatura;
  m.ppm = calcularTDS(voltTDS, temperatura);
  m.turbidezPct = calcularTurbidezRelativa(voltTurbidez);

  bool valida = rtcOK && temperatura > 10 && temperatura < 60 &&
                isfinite(m.ppm) && isfinite(m.turbidezPct);

  if (!valida) {
    guardarEvento("Lectura inválida o fecha del RTC no disponible");
    return;
  }

  ultimaMedicion = m;
  hayUltimaMedicion = true;
  guardarMedicion(m);

  if (hayAlerta(m)) {
  tone(PIN_BUZZER, 2200, 300);

  char detalle[150];
  snprintf(detalle, sizeof(detalle),
           "Umbral superado | Temperatura: %.2f C | TDS: %.1f ppm | Turbidez relativa: %.1f %%",
           m.temperatura, m.ppm, m.turbidezPct);

  guardarEvento(String(detalle));
}
  Serial.printf(
      "%s | Temperatura: %.2f °C | TDS: %.1f ppm | Turbidez relativa: %.1f %%\n",
      m.fecha.c_str(), m.temperatura, m.ppm, m.turbidezPct);
}

String paginaHTML() {
  return R"rawliteral(
<!doctype html>
<html lang="es">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>AlertAgua</title>

  <style>
    :root {
      --azul: #075985;
      --oscuro: #0c3048;
      --fondo: #eef6f8;
      --texto: #183243;
      --borde: #d8e7ec;
    }

    * { box-sizing: border-box; }
    body { margin: 0; font-family: Arial, sans-serif; background: var(--fondo); color: var(--texto); }
    header { padding: 24px 20px; color: white; background: linear-gradient(120deg, var(--oscuro), var(--azul)); }
    main, .contenedor { max-width: 900px; margin: auto; }
    main { padding: 0 14px; }
    h1 { margin: 0; font-size: 25px; }
    h2 { margin: 0 0 14px; color: var(--oscuro); font-size: 18px; }
    .subtitulo { margin: 7px 0 0; color: #d8f3f5; }

    .card {
      margin: 12px 0;
      padding: 16px;
      background: white;
      border: 1px solid var(--borde);
      border-radius: 12px;
      box-shadow: 0 3px 12px #0c30480d;
    }

    .estado { line-height: 1.7; white-space: pre-line; }
    .estado-titulo { margin-bottom: 4px; color: var(--oscuro); font-weight: bold; }
    .alerta-agua { margin-bottom: 14px; padding: 14px 16px; border: 1px solid; border-radius: 9px; font-weight: bold; line-height: 1.5; }
    .alerta-normal { color: #14532d; background: #f0fdf4; border-color: #22c55e; }
    .alerta-roja { color: #991b1b; background: #fef2f2; border-color: #dc2626; }
    .alerta-neutra { color: #334155; background: #f1f5f9; border-color: #94a3b8; }

    .mediciones { display: grid; grid-template-columns: repeat(auto-fit, minmax(190px, 1fr)); gap: 12px; }
    .medicion { padding: 14px; background: white; border: 1px solid var(--borde); border-radius: 10px; }
    .medicion.superado { background: #fee2e2; border-color: #dc2626; }
    .medicion-nombre, .medicion-limite, .nota, footer { color: #58717e; }
    .medicion-nombre { font-size: 14px; }
    .medicion-valor { margin-top: 6px; color: var(--azul); font-size: 25px; font-weight: bold; }
    .medicion.superado .medicion-valor { color: #991b1b; }
    .medicion-limite { margin-top: 6px; font-size: 12px; }

    canvas { display: block; width: 100%; height: 260px; }
    input, button { margin: 4px; padding: 10px; border-radius: 7px; }
    input { border: 1px solid #b8cbd3; }
    button, .boton { display: inline-block; padding: 10px 14px; color: white; background: var(--azul); border: 0; border-radius: 7px; text-decoration: none; font-weight: 600; cursor: pointer; }
    button:hover, .boton:hover { background: var(--oscuro); }
    .nota, footer { font-size: 13px; }
    footer { padding: 18px; text-align: center; }
  </style>
</head>

<body>
  <header>
    <div class="contenedor">
      <h1>AlertAgua</h1>
      <p class="subtitulo">Sistema de monitoreo y alerta temprana para tanques de almacenamiento de agua</p>
    </div>
  </header>

  <main>
    <section class="card">
      <div class="estado-titulo">Estado del dispositivo</div>
      <div id="alerta-agua" class="alerta-agua alerta-neutra" role="status" aria-live="polite">
        Esperando una medición válida...
      </div>
      <div id="estado" class="estado">Cargando estado del sistema...</div>
    </section>

    <section class="card">
      <h2>Mediciones actuales</h2>
      <div class="mediciones">
        <div class="medicion" id="tarjeta-temp">
          <div class="medicion-nombre">Temperatura</div>
          <div class="medicion-valor" id="medicion-temp">—</div>
          <div class="medicion-limite" id="limite-temp"></div>
        </div>
        <div class="medicion" id="tarjeta-tds">
          <div class="medicion-nombre">Sólidos disueltos (TDS)</div>
          <div class="medicion-valor" id="medicion-tds">—</div>
          <div class="medicion-limite" id="limite-tds"></div>
        </div>
        <div class="medicion" id="tarjeta-turbidez">
          <div class="medicion-nombre">Turbidez relativa</div>
          <div class="medicion-valor" id="medicion-turbidez">—</div>
          <div class="medicion-limite" id="limite-turbidez"></div>
        </div>
      </div>
    </section>

    <section class="card">
      <h2>Historial de temperatura (°C)</h2>
      <canvas id="g0" aria-label="Gráfica de temperatura por hora"></canvas>
    </section>

    <section class="card">
      <h2>Historial de sólidos disueltos (ppm)</h2>
      <canvas id="g1" aria-label="Gráfica de sólidos disueltos por hora"></canvas>
    </section>

    <section class="card">
      <h2>Historial de turbidez relativa (%)</h2>
      <canvas id="g2" aria-label="Gráfica de turbidez relativa por hora"></canvas>
    </section>

    <section class="card">
      <h2>Configuración del reloj</h2>
      <p>Ajusta la fecha y hora del reloj interno:</p>
      <form action="/ajustar" method="get">
        <input name="fecha" type="datetime-local" required>
        <button type="submit">Guardar fecha y hora</button>
      </form>
    </section>

    <section class="card">
      <h2>Historial de mediciones</h2>
      <p>Descarga las mediciones guardadas en la tarjeta microSD.</p>
      <a class="boton" href="/csv">Descargar CSV</a>
    </section>
  </main>

  <script>
    const $ = id => document.getElementById(id);
    let ultimaConsulta = 0;
    let intervaloHistorial = 30000;

    function actualizarTarjeta(id, superado) {
      $(id).classList.toggle('superado', Boolean(superado));
    }

    function actualizarIndicadores(d) {
      const alerta = $('alerta-agua');
      alerta.classList.remove('alerta-normal', 'alerta-roja', 'alerta-neutra');

      if (!d.hayMedicion) {
        alerta.classList.add('alerta-neutra');
        alerta.textContent = 'Aún no hay una medición válida para evaluar los parámetros del agua.';
        return;
      }

      actualizarTarjeta('tarjeta-temp', d.temp > d.limiteTemp);
      actualizarTarjeta('tarjeta-tds', d.ppm > d.limiteTds);
      actualizarTarjeta('tarjeta-turbidez', d.turbidezPct > d.limiteTurbidez);

      alerta.classList.add(d.alerta ? 'alerta-roja' : 'alerta-normal');
      alerta.textContent = d.alerta
        ? 'ALERTA: Se superó uno o más umbrales configurados'
        : 'No se han encontrado parámetros problemáticos según los umbrales configurados';
    }

    function precisionEjes(min, max, divisiones, inicial) {
      for (let p = inicial; p <= 6; p++) {
        const etiquetas = Array.from({ length: divisiones + 1 }, (_, i) =>
          (max - i / divisiones * (max - min)).toFixed(p)
        );
        if (new Set(etiquetas).size === etiquetas.length) return p;
      }
      return 6;
    }

    function dibujar(id, valores, horas, color, precision, minFijo, maxFijo) {
      const canvas = $(id);
      const ctx = canvas.getContext('2d');
      const escala = window.devicePixelRatio || 1;
      const ancho = canvas.clientWidth;
      const alto = 260;

      canvas.width = Math.max(1, Math.floor(ancho * escala));
      canvas.height = Math.floor(alto * escala);
      ctx.setTransform(escala, 0, 0, escala, 0, 0);
      ctx.clearRect(0, 0, ancho, alto);

      const x0 = 64, x1 = ancho - 12, y0 = 12, y1 = alto - 48;
      const graficaAncho = x1 - x0, graficaAlto = y1 - y0;
      if (graficaAncho <= 0 || graficaAlto <= 0) return;

      let min = minFijo ?? 0;
      let max = maxFijo ?? 1;

      if (minFijo === null && valores.length) {
        min = Math.min(...valores);
        max = Math.max(...valores);
        const margen = max === min ? Math.abs(max) * 0.1 || 1 : (max - min) * 0.1;
        min -= margen;
        max += margen;
      }

      const divisiones = 4;
      ctx.font = '11px Arial';
      ctx.textAlign = 'right';
      ctx.textBaseline = 'middle';

      for (let i = 0; i <= divisiones; i++) {
        const y = y0 + i / divisiones * graficaAlto;
        const valor = max - i / divisiones * (max - min);

        ctx.strokeStyle = '#e2ebef';
        ctx.beginPath();
        ctx.moveTo(x0, y);
        ctx.lineTo(x1, y);
        ctx.stroke();

        ctx.fillStyle = '#526b78';
        ctx.fillText(valor.toFixed(precisionEjes(min, max, divisiones, precision)), x0 - 7, y);
      }

      ctx.strokeStyle = '#8da4af';
      ctx.beginPath();
      ctx.moveTo(x0, y0);
      ctx.lineTo(x0, y1);
      ctx.lineTo(x1, y1);
      ctx.stroke();

      if (valores.length) {
        const marcas = Math.min(5, valores.length);
        ctx.textAlign = 'center';
        ctx.textBaseline = 'top';
        ctx.fillStyle = '#526b78';

        for (let i = 0; i < marcas; i++) {
          const indice = marcas === 1 ? 0 : Math.round(i * (valores.length - 1) / (marcas - 1));
          const x = valores.length === 1 ? x0 : x0 + indice * graficaAncho / (valores.length - 1);

          ctx.beginPath();
          ctx.moveTo(x, y1);
          ctx.lineTo(x, y1 + 4);
          ctx.strokeStyle = '#8da4af';
          ctx.stroke();
          ctx.fillText(horas[indice] || '--:--:--', x, y1 + 6);
        }
      }

      if (valores.length < 2) return;

      ctx.strokeStyle = color;
      ctx.lineWidth = 2;
      ctx.beginPath();

      valores.forEach((valor, i) => {
        const x = x0 + i * graficaAncho / (valores.length - 1);
        const y = y0 + (max - valor) * graficaAlto / (max - min);
        i === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y);
      });

      ctx.stroke();
    }

    function actualizarGraficas(historial) {
      const horas = historial.map(fila => fila.h);
      dibujar('g0', historial.map(fila => fila.t), horas, '#e87519', 1, null, null);
      dibujar('g1', historial.map(fila => fila.p), horas, '#2374ab', 0, null, null);
      dibujar('g2', historial.map(fila => fila.r), horas, '#0ea5a4', 0, 0, 100);
    }

    async function actualizarHistorial() {
      const respuesta = await fetch('/historial', { cache: 'no-store' });
      if (!respuesta.ok) throw new Error('No se pudo cargar el historial');
      actualizarGraficas(await respuesta.json());
      ultimaConsulta = Date.now();
    }

    async function actualizar() {
      try {
        const respuesta = await fetch('/datos', { cache: 'no-store' });
        if (!respuesta.ok) throw new Error('No se pudieron cargar los datos');

        const d = await respuesta.json();
        intervaloHistorial = d.intervaloHistorialMs || 30000;
        $('estado').textContent = d.estado;

        $('limite-temp').textContent = 'Umbral máximo: ' + d.limiteTemp.toFixed(1) + ' °C';
        $('limite-tds').textContent = 'Umbral máximo: ' + d.limiteTds.toFixed(1) + ' ppm';
        $('limite-turbidez').textContent = 'Umbral máximo: ' + d.limiteTurbidez.toFixed(1) + ' %';

        const mediciones = [
          ['medicion-temp', d.temp, 2, ' °C'],
          ['medicion-tds', d.ppm, 1, ' ppm'],
          ['medicion-turbidez', d.turbidezPct, 1, ' %']
        ];

        mediciones.forEach(([id, valor, decimales, unidad]) => {
          $(id).textContent = d.hayMedicion ? valor.toFixed(decimales) + unidad : '—';
        });

        if (!d.hayMedicion) {
          ['tarjeta-temp', 'tarjeta-tds', 'tarjeta-turbidez']
            .forEach(id => actualizarTarjeta(id, false));
        }

        actualizarIndicadores(d);

        if (Date.now() - ultimaConsulta >= intervaloHistorial) {
          await actualizarHistorial();
        }
      } catch (e) {
        $('alerta-agua').className = 'alerta-agua alerta-neutra';
        $('alerta-agua').textContent = 'No se pudo obtener una medición del sistema.';
        $('estado').textContent = 'No se pudo conectar con el sistema.';
      }
    }

    actualizar();
    setInterval(actualizar, 5000);
    window.addEventListener('resize', () => actualizarHistorial().catch(() => {}));
  </script>
</body>
</html>
)rawliteral";
}

void enviarDatos() {
  bool alerta = hayUltimaMedicion && hayAlerta(ultimaMedicion);

  String json = "{\"estado\":\"Fecha y hora: ";
  json += rtcOK ? fechaHoraActual() : "No disponible";
  json += "\\nReloj interno: ";
  json += rtcOK ? "Operativo" : "No disponible";
  json += "\\nmicroSD: ";
  json += sdOK ? "Activa" : "No disponible";

  json += "\",\"hayMedicion\":";
  json += hayUltimaMedicion ? "true" : "false";
  json += ",\"alerta\":";
  json += alerta ? "true" : "false";

  json += ",\"temp\":" +
          String(hayUltimaMedicion ? ultimaMedicion.temperatura : 0.0, 2);
  json += ",\"ppm\":" +
          String(hayUltimaMedicion ? ultimaMedicion.ppm : 0.0, 1);
  json += ",\"turbidezPct\":" +
          String(hayUltimaMedicion ? ultimaMedicion.turbidezPct : 0.0, 1);

  json += ",\"limiteTemp\":" + String(LIMITE_TEMPERATURA_C, 1);
  json += ",\"limiteTds\":" + String(LIMITE_TDS_PPM, 1);
  json += ",\"limiteTurbidez\":" + String(LIMITE_TURBIDEZ_PCT, 1);
  json += ",\"intervaloHistorialMs\":" + String(INTERVALO_HISTORIAL_MS);
  json += "}";

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

void enviarHistorial() {
  if (!sdOK || !rtcOK || !SD.exists("/mediciones.csv")) {
    server.send(200, "application/json", "[]");
    return;
  }

  File archivo = SD.open("/mediciones.csv", FILE_READ);
  if (!archivo) {
    server.send(200, "application/json", "[]");
    return;
  }

  String hoy = fechaActual();
  String bloque;
  bool primera = true;
  bloque.reserve(1024);

  server.sendHeader("Cache-Control", "no-store");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  server.sendContent("[");

  while (archivo.available()) {
    String linea = archivo.readStringUntil('\n');
    linea.trim();

    if (!linea.startsWith(hoy)) continue;

    Medicion m;
    if (!analizarFilaCSV(linea, m)) continue;

    if (!primera) bloque += ",";
    primera = false;

    bloque += "{\"t\":" + String(m.temperatura, 2);
    bloque += ",\"p\":" + String(m.ppm, 1);
    bloque += ",\"r\":" + String(m.turbidezPct, 1);
    bloque += ",\"h\":\"" + m.fecha.substring(11, 19) + "\"}";

    if (bloque.length() >= 1024) {
      server.sendContent(bloque);
      bloque = "";
    }
  }

  archivo.close();

  if (bloque.length()) server.sendContent(bloque);
  server.sendContent("]");
  server.sendContent("");
}

void descargarCSV() {
  if (!sdOK || !SD.exists("/mediciones.csv")) {
    server.send(404, "text/plain", "No hay archivo CSV disponible");
    return;
  }

  File archivo = SD.open("/mediciones.csv", FILE_READ);
  if (!archivo) {
    server.send(500, "text/plain", "No se pudo abrir el archivo CSV");
    return;
  }

  server.sendHeader("Content-Disposition",
                    "attachment; filename=mediciones.csv");
  server.streamFile(archivo, "text/csv");
  archivo.close();
}

void ajustarRTC() {
  if (!rtcOK || !server.hasArg("fecha")) {
    server.send(400, "text/plain", "RTC no disponible o fecha faltante");
    return;
  }

  String s = server.arg("fecha");
  int anno = s.substring(0, 4).toInt();
  int mes = s.substring(5, 7).toInt();
  int dia = s.substring(8, 10).toInt();
  int hora = s.substring(11, 13).toInt();
  int minuto = s.substring(14, 16).toInt();

  if (anno < 2026 || mes < 1 || mes > 12 || dia < 1 || dia > 31 ||
      hora > 23 || minuto > 59) {
    server.send(400, "text/plain", "Fecha inválida");
    return;
  }

  rtc.adjust(DateTime(anno, mes, dia, hora, minuto, 0));
  guardarEvento("Fecha y hora del RTC ajustadas manualmente");

  server.sendHeader("Location", "/");
  server.send(303);
}

// Inicio y ciclo principal
void setup() {
  Serial.begin(115200);
  Serial.println("< AlertAgua >");

  analogReadResolution(12);
  analogSetPinAttenuation(PIN_TURBIDEZ, ADC_11db);
  analogSetPinAttenuation(PIN_TDS, ADC_11db);
  pinMode(PIN_BUZZER, OUTPUT);

  Wire.begin(21, 22);
  rtcOK = rtc.begin();

  if (!rtcOK) {
    Serial.println("Error al iniciar el RTC.");
  } else if (rtc.lostPower()) {
    Serial.println("La bateria del reloj interno se ha cambiado, ajuste nuevamente la fecha desde la página.");
  }

  sensorTemp.begin();

  SPI.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI);
  sdOK = SD.begin(PIN_SD_CS, SPI);

  if (!sdOK) {
    Serial.println("Error al iniciar microSD.");
  } else {
    cargarConfiguracion();
    cargarUltimaMedicionDelDia();
  }

  WiFi.mode(WIFI_AP);

  if (!WiFi.softAPConfig(IP_SERVIDOR, IP_SERVIDOR, MASCARA_RED))
    Serial.println("No se pudo configurar la IP del punto de acceso.");

  if (!WiFi.softAP(AP_SSID, AP_PASS))
    Serial.println("Error al iniciar el punto de acceso.");

  Serial.print("Conectese a la red ");
  Serial.print(AP_SSID);
  Serial.print(" y abra http://");
  Serial.print(IP_SERVIDOR);
  Serial.println(" desde su navegador.");

  server.on("/", []() {
    server.send(200, "text/html", paginaHTML());
  });

  server.on("/datos", enviarDatos);
  server.on("/historial", enviarHistorial);
  server.on("/csv", descargarCSV);
  server.on("/ajustar", ajustarRTC);
  server.begin();
}

void loop() {
  server.handleClient();

  unsigned long ahora = millis();
  if (ahora - ultimaLectura >= INTERVALO_MS) {
    ultimaLectura = ahora;
    tomarMedicion();
  }
}
