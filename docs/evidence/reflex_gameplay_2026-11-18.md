# Evidencia: sesión de gameplay con Reflex (18-11-2026, 60 FPS)

- **Cuándo:** 2026-11-18, 22:45–22:50 (Europe/Moscow); build posterior a `b1bfa53`.
- **Configuración:** `fps = 60`, `reflex = on`, `upscaler = fsr3`, ventana 1920x1080, **V-Sync off**
  (`present mode mailbox`; `xcde.ini` con `vsync = 0`).
- **Partida:** guardado cargado desde `save/save_0` (gameplay real, ~133k draws/frame y 458 MB de texturas),
  sin ruta automática.

## Resultados

| Métrica | Valor |
|---|---|
| Frames | 2.709 en 71,9 s |
| FPS medio | 32,1 (tope 60; límite de GPU) |
| `slReflexSleep` | 2.641 OK / 0 fallos |
| Marcadores PCL | 16.175 (6/frame + pings) |
| Pings PCL | 335 |
| PCL proxy (app) | n=321, avg **31,25 ms**, min 7,33, max 248,35, 1 outlier >250 ms |
| Crash / stall | ninguno; sin excepción ni `[stall] no frame`; último crash del proyecto 20:14 |

El PCL proxy es el tramo ping→present de la aplicación, **no** el PCL ETW que muestra RTSS/FrameView; no se
declara ganancia de latencia. El promedio de 32 FPS no alcanza el tope de 60 por límite de la GTX 1050 Ti.

Log completo sin publicar: `build/reflex_probe/last_game_user_2250.log`. Extracto versionado:
`docs/evidence/reflex_gameplay_2026-11-18_run.txt`.
