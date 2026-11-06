# Record Source

Plugin para **OBS Studio 33.0+** que graba una fuente concreta en un archivo propio, con su propia pista de audio y su propio panel de control. No comparte el sistema de salida con la transmisión, la grabación principal ni el búfer de repetición.

## Características

- Grabación independiente de una fuente de vídeo (escena, juego, ventana, capturadora, etc.).
- Pista de audio propia, capturada desde la fuente seleccionada.
- Archivo independiente mediante `ffmpeg_muxer`.
- Panel pequeño integrado en OBS (`Docks -> Record Source`).
- Configuración de codificadores de vídeo y audio con las mismas propiedades que expone OBS.
- Ajustes de color por grabación: formato de color (NV12, I420, I444, P010), espacio de color (Rec. 709, Rec. 601, sRGB, Rec. 2100 PQ/HLG) y gama de colores (limitada o completa).
- División automática de la grabación en varios archivos por tiempo o tamaño, como en OBS.
- Interfaz en inglés por defecto y en español cuando OBS está configurado en español.
- Compatible con OBS Studio 33.0.

## Compilación

Requisitos:

- Visual Studio 2022 Build Tools con C++.
- CMake 3.28 o superior.
- Fuentes de OBS Studio 33.0.1.
- Paquete Qt 6 de `obs-deps`.
- OBS Studio 33.0.1 instalado.

Compilación:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build.ps1 -Config Release -Install
```

El script descarga las dependencias si no están disponibles, genera las bibliotecas de importación desde `obs.dll` y `obs-frontend-api.dll`, configura CMake, compila y, con `-Install`, copia el plugin a:

```text
C:\ProgramData\obs-studio\plugins\obs-record-source
```

## Desarrollo

Para probar sin instalar en `ProgramData`, define:

```powershell
$env:OBS_PLUGINS_PATH = "C:\ruta\al\parent\del\bundle"
```

OBS espera el plugin en:

```text
<parent>\obs-record-source\obs-record-source.dll
<parent>\obs-record-source\data\manifest.json
<parent>\obs-record-source\data\locale\en-US.ini
<parent>\obs-record-source\data\locale\es-ES.ini
```

## Uso

1. Abre OBS Studio.
2. Abre el dock `Record Source`.
3. Selecciona la fuente de vídeo.
4. Elige la carpeta de destino, el formato y, si lo necesitas, los ajustes de color y la división de archivos.
5. Selecciona y configura los codificadores de vídeo y audio.
6. Pulsa **Iniciar grabación**.
7. Pulsa **Detener** para finalizar.

Los ajustes se guardan automáticamente en la configuración del plugin.

### Idioma

La interfaz del plugin sigue el idioma de OBS:

- Si OBS está en **español**, el plugin usa español (`es-ES`).
- Con cualquier otro idioma, incluido el inglés, el plugin usa **inglés** (`en-US`).

Los textos están en `data/locale/en-US.ini` y `data/locale/es-ES.ini`. El inglés se carga siempre como base y el español se superpone cuando corresponde, así que si falta una clave en un idioma se muestra el texto en inglés. OBS no permite traducir los metadatos de `manifest.json`, que están en inglés por defecto.

Para añadir otro idioma, crea `data/locale/<locale>.ini`, añádelo a las reglas de `CMakeLists.txt` y amplía la selección de idioma de `obs_module_set_locale` en `src/plugin-main.cpp`.

### Ajustes de color

- **Formato de color**: `NV12` e `I420` son 4:2:0 de 8 bits y los más compatibles. `I444` conserva más detalle de color y `P010` usa 10 bits.
- **Espacio de color**: `Rec. 709` es el estándar SDR. `Rec. 2100 PQ/HLG` usa la gama `Rec. 2020` (HDR) y requiere un formato de 10 bits y un codificador compatible; el x264 de OBS no lo admite.
- **Gama de colores**: `Limitado` (16-235) es lo habitual en YUV; `Completo` (0-255) conserva todo el rango.
- La opción **Automático** de cada ajuste mantiene el valor que OBS tiene configurado en ese momento, así que el comportamiento predeterminado no cambia.

### Audio

El plugin no captura la fuente de audio de forma aislada: al iniciar la grabación, añade temporalmente la fuente seleccionada a la pista de OBS indicada y graba esa pista completa. Por eso, para grabar solo esa fuente, debes desactivar el resto de fuentes de audio en esa misma pista. Al detener la grabación se restauran las pistas originales de la fuente.

### División de archivos

El ajuste **Dividir archivo** hace que la grabación continúe en otro archivo al alcanzar un límite, igual que en OBS (`Ajustes -> Salida -> Grabación`):

- **No dividir**: un único archivo por grabación.
- **Por tiempo**: cada archivo dura los minutos indicados (15 por defecto).
- **Por tamaño**: cada archivo ocupa como máximo los MB indicados (2048 por defecto, mínimo 20 MB).

La división se hace en el siguiente fotograma clave, así que cada parte puede superar ligeramente el límite. Las partes se guardan en la carpeta de destino con el mismo patrón de nombre que el primer archivo, usando la fecha y hora de cuando se crea cada una. El panel **Archivo** muestra siempre la parte que se está grabando en ese momento.