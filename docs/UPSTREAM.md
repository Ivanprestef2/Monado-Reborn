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

## T272 — NVIDIA Reflex opcional (2026-11-18)

- `runtime/nvn/vulkan/dlss.cpp`: el runtime de Streamline deja de depender de DLSS; carga las features que pida el
  ajuste (DLSS y/o Reflex + PCL), fusiona requisitos de todas, y mantiene banderas separadas (`sl_rt_ok`, `dlss_ok`,
  `reflex_ok`). Un DLSS no soportado (GTX sin RTX) ya no desactiva Reflex. Token de frame compartido (anillo de 8)
  entre marcadores/sleep y `dlss_evaluate`, `slReflexSetOptions/GetState/Sleep` y `slPCLSetMarker`.
- `runtime/nvn/vulkan/reflex.h` (nuevo): API C del frame loop (`reflex_frame_begin`, marcadores, frame del present).
- `runtime/hle/framepace.c`: `slReflexSleep` y `eSimulationStart` una vez por frame en el límite del limiter.
  `runtime/nvn/objects.c`: `eSimulationEnd`/`eRenderSubmitStart` al presentar. `runtime/nvn/vulkan/exec.c` y
  `device.c`: el frame del present viaja con el trabajo del render worker y `vk_frame_end` marca alrededor de
  `vkQueuePresentKHR`. `reflex = off | on | boost` en `xcde.ini` (por defecto off), F1 y launcher;
  `XCDE_REFLEX=0/1/2` lo fuerza.
- `tools/install_dlss.py`, `cmake/copy_upscaler_dlls.cmake`, `scripts/copy_upscaler_dlls.sh`, `NOTICE.md`:
  `sl.reflex.dll`, `sl.pcl.dll` y `NvLowLatencyVk.dll` se despliegan con el resto del runtime NVIDIA.
- Pruebas: `tests/native/test_settings_roundtrip.c` (clave `reflex`), `tests/unit/test_reflex_contracts.py` (fuente) y
  `tests/unit/test_install_dlss.py` (DLLs). Investigación y límites: `docs/reflex_research.md` y
  `docs/evidence/reflex_2026-11-18*.md`.
