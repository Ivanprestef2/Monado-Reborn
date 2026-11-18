# Evidencia: NVIDIA Reflex (T272), implementación opt-in (2026-11-18)

**Pregunta:** ¿funciona la integración de Reflex añadida en esta sesión (runtime de Streamline compartido con DLSS,
token por frame, marcadores, *sleep*, ajuste `reflex`, DLLs)? **Resultado:** compila, enlaza y arranca en el juego
real: Streamline carga Reflex + PCL, `slReflexGetState` da baja latencia disponible y el bucle de frame entra en
marcha (`[reflex] frame loop active`). Queda la medición de latencia y una anomalía de arranque con Streamline
cargado (abajo). Diseño y límites: [`../reflex_research.md`](../reflex_research.md).

## Equipo y build

- Windows 10.0.19044; **NVIDIA GeForce GTX 1050 Ti** (Pascal, sin RTX), driver **582.78**, Vulkan **1.4.312**,
  8 CPUs lógicas.
- Rama `feat/nvidia-reflex`, creada desde `main` (`474f616`) con la integración de T272. Sin datos del juego ni código generado modificados.
- Los objetos de `build/` se habían borrado: se recompilaron los 320 `.c` generados y los 65 objetos del runtime con
  las mismas banderas de `scripts/build_boot.sh` / `scripts/build_rt_gfx.sh` (zig cc/c++), y se enlazó con el comando
  de `scripts/link_gfx.sh` (385 objetos), ejecutable final de esta sesión. El entorno de esta sesión bloquea los scripts
  bash/ninja del proyecto, por eso el build se hizo con un driver Python equivalente (archivos de log en `build/`,
  ignorados).

## Método

Runs ocultos cortos (`LARECOMP_HIDDEN=1`, captura y salida en el frame 240), sin tocar el teclado/ratón:

- **A - solo Reflex:** `xcde.ini` con `upscaler = fsr`, `reflex = on`; `XCDE_INI=<repo>\build\reflex_probe\reflex_only.ini`.
- **B - base:** el mismo archivo con `reflex = off` (Streamline no se carga).
- **C - DLSS sin RTX:** `upscaler = auto`, `reflex = off`; Streamline se carga para DLSS, que no es compatible.

## Resultados

| Comprobación | Resultado |
|---|---|
| Compilación del runtime y del juego | 320 + 65 unidades, **0 fallos**; enlace de 385 objetos OK |
| Run A: features cargadas | `loaded (requirements: dlss 0, reflex 1, pcl 1; 2 feature(s))` |
| Run A: estado | `[reflex] Reflex disponible (baja latencia) en la GPU activa` (dispositivo y superficie) |
| Run A: bucle de frame | `[reflex] frame loop active (frame 1)` |
| Run A: DLLs | `sl.reflex.dll`, `sl.pcl.dll` y `NvLowLatencyVk.dll` cargadas con firma NVIDIA verificada |
| Run A: ejecución | 240 frames, `build/window.png` guardado, salida **0** |
| Run B (sin Streamline) | `[reflex] Reflex no solicitado`; 240 frames, salida 0, sin avisos de stall |
| Run C (DLSS no soportado) | `[dlss] DLSS no compatible...`; muestra los mismos stalls de arranque que A |
| `test_settings_roundtrip` | pasa (clave `reflex` incluida) |
| `tests/unit/test_reflex_contracts.py` | 4/4 OK |
| `tests/unit/test_install_dlss.py` | 5/5 OK |
| `tools/validate_port.py` | todas las fuentes compilan; 5 tests no enlazan en este entorno por falta de `libwinpthread.dll.a` |

## Hallazgo abierto: arranque con Streamline cargado

Con Streamline cargado (runs A y C) el watchdog del juego avisa de **3 paradas de 5-8 s** en los frames 7-14,
durante la construcción de pipelines; el frame 8 llega a t=11.9 s (máx. 8162 ms). Sin Streamline (run B) esa misma
fase termina en t=5.5 s (máx. 2003 ms) y no hay avisos. Se aisló:

- no es `slReflexSleep`: con `XCDE_REFLEX_NOSLEEP=1` los stalls siguen;
- no es el proxy de swapchain: con la superficie/hooks desactivados (diagnóstico `XCDE_REFLEX_NO_PROXY`) siguen;
- no son los plugins DLSS/NGX no solicitados: con un directorio de plugins solo Reflex (`XCDE_SL_PLUGIN_DIR`) siguen;
- se reproduce pidiendo **solo DLSS** en esta GPU (run C), sin Reflex.

Es decir, cargar el runtime de Streamline (interposer/plugins) hace más lento el prewarm de pipelines del puerto.
Los runs de prueba salen con `_exit(0)` (captura), así que **no guardan** `cache/vk_pipelines.bin`: cada run vuelve a
construir ~290 programas. En un cierre normal el juego guarda la caché, y el paso de precompilación del proyecto
puede dejar el arranque caliente; hay que medirlo en un arranque normal antes de plantear `reflex = on` por defecto.
No se declara la ganancia de latencia: falta la HUD de verificación de Reflex / FrameView.

## Límites

- Solo pantalla de título (240 frames), no gameplay ni rutas; una GPU; ventana oculta y presentación mailbox.
- No se midió PCL ni latencia de entrada; no hay RTX para probar DLSS + Reflex a la vez.
- Los logs completos quedan en `build/reflex_probe/` (ignorados); el extracto saneado es
  [`reflex_impl_2026-11-18_run.txt`](reflex_impl_2026-11-18_run.txt).
