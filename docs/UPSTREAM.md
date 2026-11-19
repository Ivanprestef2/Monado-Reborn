# Cambios locales al runtime heredado

Se conserva la atribución LASRecomp en LICENSE/NOTICE. Esta continuación no incorpora código de proyectos GPL ni datos del juego.

## 2026-11-04 — T247–T255

- T247: `runtime/host/settings_ini.c`, `runtime/hle/bindings.c`; escritor compartido en `runtime/host/ini_writer.h` y `atomic_file.h`.
- T248: `runtime/settings.h`, `runtime/nvn/vulkan/device.c`, `runtime/ui/menu_vk.cpp`; política/probe/selector compartidos con el launcher.
- T249: `runtime/hle/framepace.c`, `runtime/hle/party.c`, `runtime/frame_clock.h`; rutas por milisegundos en `runtime/diag/diag.c`/`.h`.
- T250: `runtime/nvn/vulkan/vk.h`, `dispatch.h`, `device.c`, `imgui_glue.cpp`, `shaders.c`, `dlss.cpp`, `temporal.c`, `temporal_fg.inc`.
- T251: `runtime/nvn/output_scale.c`, `runtime/nvn/vulkan/exec.c`, `precompile.c`, `runtime/resource_policy.h`.
- T252–T254: `runtime/nvn/vulkan/exec.c`, `device.c`, `textures.c`, `shaders.c`, `runtime/cache_validation.h`, `staging_copy.h`; diagnóstico de archivos en `runtime/services/fs.c`.
- T255: `runtime/nvn/objects.c`, `nvn.h`, `runtime/nvn/vulkan/exec.c`, `runtime/window_reuse.h`: espera de reutilización por imagen de ventana.

Pruebas y límites: `PORT_VALIDATION.md`. Los cambios específicos de temporizadores del juego conservan los hooks y parches existentes de `games/xcde`; no se inventaron direcciones ni se generó código del juego.

## T256–T258 — rutas Vulkan, DLSS y HUD FPS

- `runtime/settings.h`, `host/settings_ini.c`, `host/settings_boot.c`, `gpu_policy.h`, `vulkan_policy.h`, `nvn/vulkan/gpu_probe.h`, `device.c`, `vk.h`: preferencia de ruta API, capacidades y escalador automático.
- `runtime/nvn/vulkan/dlss.cpp`, `exec.c`, `temporal.c`, `runtime/dlss_policy.h`, `host/nvidia_signature.h`: runtime NVIDIA oficial, gating por GPU/SDK, colas, hook de superficie, recursos/estado y verificación de firma.
- `runtime/ui/menu_vk.cpp`, `menu.h`, `gpu_picker.h`, `runtime/fps_meter.h`, `host/menu_stub.c`: contador en pantalla y opciones. El launcher comparte las opciones y preserva las claves nuevas.

Detalle y pruebas en `VULKAN_DLSS_FPS.md`. Los binarios NVIDIA viven solo en `sdk/` y `build/`, excluidos de control de versiones.


## T259–T261 — rendimiento conservador, perfiles e interfaz

- `runtime/settings.h`, `quality_policy.h`, `host/settings_ini.c`, `services/romfs_virtual.c`: perfiles, claves compatibles, enlace C/C++ y efectos optativos en memoria.
- `runtime/nvn/output_scale.c`, `nvn/vulkan/exec.c`, `device.c`, `textures.c`, `vk.h`: presupuesto de presentación, tiempo GPU asíncrono, caché de descriptores, scratch de índices, índice de texturas y propiedades GPU.
- Nuevos helpers `render_policy.h`, `descriptor_cache.h`, `native_intervals.h`, `services/romfs_fd_cache.h`, `nvn/vulkan/texture_index.h`; comparten lógica con las pruebas.
- `runtime/ui/menu_vk.cpp`, nuevos helpers UI, `tools/launcher/launcher.cpp`: tema, navegación y configuración compartida. `scripts/native_tests.sh` y `tools/validate_port.py`: comprobaciones sin datos del juego.

Justificación en ADR 0011. Se conserva la atribución y no se incorpora código de otros proyectos. No se ha enviado ningún cambio upstream ni publicado una distribución.

## T271 — tamaño del HUD (GitHub #121)

- `runtime/nvn/vulkan/exec.c`: la clasificación de la caja 16:9 también corre en 16:9 con el HUD escalado; ancla por draw y escala plegada en el mapeo del viewport. `runtime/settings.h`, `host/settings_ini.c`, `ui/menu_vk.cpp`, `tools/launcher/launcher.cpp`: ajuste `ui_scale`. Nuevo helper `runtime/ui_scale.h` (compartido con `tests/native/test_ui_scale.c`). T271b: solo se escala el HUD de los bordes (popups, menús, diálogos y marcadores quedan como los dibuja el juego), grupos estables entre frames (anclas con histéresis) y `ui_backdrop_view` en `exec.c` (copia desplazada de la textura de la ventana para los draws del HUD reducido que leen la escena).

## Corrupción de vértices y efectos con async en Windows (2026-11-10)

Dos fallos distintos del renderer asíncrono de Windows producían el mismo tipo de síntoma: geometría y efectos
dibujados con los datos de vértice de otro frame. Ambos son inertes en Linux, donde el vertex cache y el frame lag
no están activos (RADV usa la ruta dma-buf GTT y `pt_init()` devuelve false sin `MEM_WRITE_WATCH`).

- **T-VCACHE-ASYNC:** `vcache_lookup` (`nvn/vulkan/exec.c`) comparaba la elegibilidad de desalojo contra
  `vk_pending_seq()`. Con el worker asíncrono, los entries del frame N parecían desalojables antes de que su
  secuencia se enviara, y sus buffers device-local podían reutilizarse o liberarse mientras el command buffer del
  worker aún los referenciaba: geometría/texturas incorrectas y corrupción gráfica. Ahora, con `backend_async()`,
  se usa `vk_completed_seq()`; la ruta síncrona no cambia.
- **T-FRAME-LAG-VCACHE:** con async + vertex cache + más de 60 FPS, `vc_flush` registraba la actualización como una
  copia en tiempo de GPU desde memoria del guest; la copia podía ejecutarse después de que el juego reescribiera
  esas páginas, así que los streams de efectos/partículas dibujaban el contenido de otro frame (quad grandes
  estirados). Ahora, mientras el frame lag está activo, las páginas se copian al transient ring en el momento del
  submit; `XCDE_VC_STAGE=0` restaura el comportamiento anterior para un A/B. Las lecturas de host que el frame lag
  sigue haciendo un frame tarde (SSBOs de graphics leídos directamente del heap importado) no están cubiertas.
  `T-FRAME-LAG-HOST-READS` (2026-11-19) extiende el snapshot a la copia `gfast` del path de compute y al llenado
  inicial de los mirrors de vértices.

Detalle, síntomas, diagnósticos y límites: [evidence/windows-vertex-corruption/summary.md](evidence/windows-vertex-corruption/summary.md).

