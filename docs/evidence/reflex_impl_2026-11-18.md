# Evidencia: NVIDIA Reflex (T272), implementación opt-in (2026-11-18)

> English version: [`reflex_impl_2026-11-18.en.md`](reflex_impl_2026-11-18.en.md)

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
- no es el proxy de swapchain: con la superficie/hooks desactivados (diagnóstico temporal de esta investigación,
  ya retirado del código) siguen;
- no son los plugins DLSS/NGX no solicitados: con un directorio de plugins solo Reflex (diagnóstico temporal,
  retirado) siguen;
- se reproduce pidiendo **solo DLSS** en esta GPU (run C), sin Reflex.

Es decir, cargar el runtime de Streamline (interposer/plugins) hace más lento el prewarm de pipelines del puerto.
Los runs de prueba salen con `_exit(0)` (captura), así que **no guardan** `cache/vk_pipelines.bin`: cada run vuelve a
construir ~290 programas. En un cierre normal el juego guarda la caché, y el paso de precompilación del proyecto
puede dejar el arranque caliente; hay que medirlo en un arranque normal antes de plantear `reflex = on` por defecto.
No se declara la ganancia de latencia: falta la HUD de verificación de Reflex / FrameView.

### Medición en arranque normal (20:40)

Se repitieron lanzamientos visibles normales (ventana FIFO, `fsr3`, `run_speed = 2.00`, mismos ajustes), matando el
proceso a los 20 s (caché caliente) o 45 s (caché de pipelines borrada, caché del driver caliente), y se comparó
`reflex = on` con `reflex = off`:

| Arranque | Frame 8 | Prewarm | `[stall] no frame` | Ritmo al final |
|---|---|---|---|---|
| caliente, Reflex on | t=6.1 s | 1471 ms (2112 builds) | 0 | 579 frames/18.1 s (53.1 fps) |
| caliente, Reflex off | t=6.0 s | 1478 ms (2110 builds) | 0 | 596 frames/18.1 s (56.3 fps) |
| frío, Reflex on | t=6.7 s | 1802 ms (2111 builds) | 0 | 1972 frames/42.9 s (58.4 fps) |
| frío, Reflex off | t=6.7 s | 1836 ms (2112 builds) | 0 | 1968 frames/42.8 s (58.7 fps) |

El "frío" borró `cache/vk_pipelines.bin` (la caché del driver y `variants.bin` seguían); se restauró la caché
caliente del usuario al terminar. **No se reprodujeron las 3 paradas de 5-8 s de los runs ocultos**: el arranque
normal es igual con Reflex on y off (diferencia < 0.1 s hasta el frame 8, dentro del ruido; el FPS final depende de
la escena). La anomalía de arranque queda, por tanto, como artefacto del arnés oculto/salida forzada o de un estado
de caché del driver muy frío, no como regresión de Reflex en juego normal. No se probó un primer arranque en una
máquina sin caché de driver.

## Cierre (20:00): crash de arranque con Reflex y arreglo

El primer arranque real desde el launcher con `reflex = on` (`upscaler = fsr3`, `run_speed = 2.00`,
`party_switch = 0`, V-Sync, ventana visible) **chocó** al frame 8:

- `crash/crash_20261118_200033.txt`: `c0000005` en `f_7000593120+0xb0`, **lectura de 0xda8**, último frame
  presentado 8; antes hubo los mismos stalls de arranque (informe en `crash/stall_1.txt`).
- Pila: `fill_state` → `party_game_tick` → `run_speed_leader` → `field_group` → `guest_call` →
  `f_7000593120+b0`. Desensamblado del huésped: `x20 = *(obj + 0x680)` y después `LD64(x20 + 0xda8)` sin
  comprobar `x20`; con el objeto de campo recién encontrado y `+0x680` todavía a 0, la lectura cae en `0xda8`.
- Es una **carrera preexistente del run-speed**: el escáner de party encuentra el objeto de campo durante la
  carga, antes de que el juego enlace `+0x680`; el stall de arranque de Streamline (arriba) alarga la ventana.
  No es una llamada de la API de Reflex ni del proxy.
- Arreglo en `runtime/hle/party.c` (`field_group`): se leen `*pm` y `+0x680` y se difiere la llamada al huésped
  hasta que el enlace existe (`[party] field manager not linked yet: run-speed lookup deferred`, máx. 3 avisos).
  Cuando el enlace está, el comportamiento es el mismo de antes.

Validación: reconstruido el runtime y enlazado; los mismos ajustes del usuario pasan en runs oculto y visible de
240 frames (sin crash). En esos runs el guard no llegó a saltar (la carrera depende del tiempo), pero la condición
comprobada es exactamente la lectura que falló en el dump. El coste de arranque con Streamline sigue abierto
(sección anterior); este arreglo evita que se convierta en crash con `run_speed` activo.

## Cierre con la X (20:14): crash de teardown y arreglo

El usuario jugó una sesión real (2409 frames presentados) y cerró con la **X** de la ventana; el juego generó
`crash/crash_20261118_201438.txt`:

- `c0000005` de **ejecución** en `0x7ff87e2aba50`, último frame 2409; el hilo que falla es el 0x2018 (host), no un
  hilo de juego. El log termina en `window closed`.
- El minidump lista `sl.dlss.dll` como **módulo descargado** (base `0x7ff87e260000`, tamaño `0x69000`); la
  dirección que falla es `sl.dlss.dll+0x4ba50`. También están descargados `sl.reflex.dll`, `sl.pcl.dll` y
  `sl.common.dll`, mientras `sl.interposer.dll` sigue cargado.
- Causa: `slShutdown()` (llamado desde el `atexit` de `dlss_shutdown`) descarga los plugins mientras un hilo
  trabajador de NGX/DLSS sigue ejecutando código de `sl.dlss`; al volver, el hilo salta a código ya descargado.
  No es un fallo del juego ni de la partida: solo del camino de cierre ordenado. La X ejecuta
  `ui_run` → `SDL_EVENT_QUIT` → `exit(0)` → `atexit`; el menú interno puede salir por otra vía.

Arreglo: `dlss_shutdown` ya **no llama a `slShutdown()`** (ni a `vkDeviceWaitIdle`) al salir del proceso; deja el
runtime mapeado y Windows lo reclama al terminar. Es seguro porque el proceso está saliendo y no quedan llamadas
a Vulkan/Streamline después.

Validación: reconstruido y enlazado (`build/xcde.exe`, 20:22); un cierre por `WM_CLOSE` directo no generó ningún
informe nuevo (0 crash reports) y el proceso terminó. **Confirmado por el usuario:** el arranque normal y el cierre
con la X ya no generan informe.

## Siguiente: HUD de verificación de Reflex (requiere administrador)

El zip del SDK (`streamline-sdk-v2.14.16.zip`, descargado por el usuario) trae `utils/reflex/`:
`ReflexTestEnable.exe`, `ReflexTest.exe`, `PrintPCL.exe` y `FVSDKSetup.exe`. Se extrajo `utils/reflex` a
`build/reflex_probe/reflex_utils/` (ignorado). `ReflexTestEnable.exe 1` responde **"Please run with
elevated/admin privileges"**; ejecutado elevado devuelve 0. Se añadió `build/xcde.exe` al perfil de NVIDIA Control
Panel y se probó ventana y fullscreen, pero el HUD **no aparece en la captura de escritorio**.

Para no depender del overlay se añadió el diagnóstico `XCDE_REFLEX_STATS=1`, que registra `slReflexGetState` cada
120 frames. Resultado de una sesión de 12 s (ventana, `fsr3`, Reflex on):

```
[reflex] stats frame=241 sleep_ok=241 fail=0 pcl_markers=1440 low=1 report=1 msg=49692
[reflex] report frame=236 input=0 sim=... submit=... present=... driver=... os=... gpu=...
           gpu_us=17011 active_us=2669
```

- `sleep_ok=241 fail=0`: `slReflexSleep` se llamó y devolvió `eOk` en **todos** los frames (el `App Called
  Sleep` del HUD sería 1).
- `pcl_markers=1440`: 6 marcadores por frame (simulación, submit y present, inicio/fin) aceptados por el plugin
  (`eOk`); el `PCL` del HUD no sería 0.
- `report=1`, `low=1`: hay informe de latencia y baja latencia disponible; los timestamps de driver/OS/GPU son no
  nulos y `gpu_us ≈ 16.4-17.0 ms` a 60 FPS.
- `input=0` porque no se marca el muestreo de entrada (haría falta un marcador de input; opcional).
- El campo `present` de este ejemplo tiene inicio=fin (sección de duración cero en el frame de carga); el resto de
  secciones tiene inicio<fin. El HUD real y FrameView siguen pendientes; si el overlay no aparece, el siguiente
  paso de NVIDIA es instalar `FVSDKSetup.exe` (administrador, reinicio) o usar fullscreen exclusivo.

## Límites

- Los runs automatizados son de pantalla de título (240 frames) y no de gameplay; la sesión real del usuario
  (2409 frames) no se midió con HUD ni FrameView.
- No se midió PCL ni latencia de entrada; no hay RTX para probar DLSS + Reflex a la vez.
- Los logs completos quedan en `build/reflex_probe/` (ignorados); el extracto saneado es
  [`reflex_impl_2026-11-18_run.txt`](reflex_impl_2026-11-18_run.txt).
