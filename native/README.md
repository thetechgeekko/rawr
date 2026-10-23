# native/

Image-processing libraries. The app (`app/src/main/cpp`) orchestrates them.

## Boundary

| Layer | Owns | Must not |
|---|---|---|
| `native/<lib>` | Shaders, pipelines, and a host API: create from borrowed device handles (`rawr::vk::GpuContext` or `VkPhysicalDevice`/`VkDevice`), then `record(VkCommandBuffer, inputs, outputs, params)`. Its own validation. | Include anything from `app/`. Own queues, submission, threads, frame slots or settings. |
| `app/src/main/cpp` | Camera session, AHB import, frame lifetimes, scheduling and submission, JNI, mapping settings to library params, diagnostics. | Contain image-processing math or shaders (diagnostic probes excepted). |

A realtime variant of an algorithm (for example the fused video demosaic) is
still a native library, not app code.

## Stage map

| Stage | Preview | Still (single) | Still (multiframe) | Video | Renderer (DNG) |
|---|---|---|---|---|---|
| Clip classification + CFA pack | `raw_preview` (cfa_state) | `raw_demosaic/common` (sensor_clip_pack) | `multiframe/output` (prepare_rgb) | `raw_ingress` | GPU camera ingress: RAW10 (MIPI packed) to R16 unpack from the imported camera buffer. |
| `video_pipeline` | CPU normalize (`RendererEngine`) |
| Demosaic | `raw_preview` (2×2 cell) | `raw_demosaic` (RCD / VNG4 / Dual) | `multiframe/merge_wronski` (merge outputs RGB) | `video_pipeline` (MHC 5×5) | `raw_demosaic` (+ quadfix) |
| Capture sharpening | — | `capture_sharpening` (planned) | — | — | `capture_sharpening` (planned) |
| Post-demosaic denoise | — | `raw_denoise` | — | `raw_denoise` | `raw_denoise` |
| WB, highlight, FCC, defringe | `raw_preview` + `raw_highlight` | `post_demosaic` + `raw_highlight` + `false_color_correction` | same as single | same as single | same as single |
| Tone / look | `tonemap_engine` or `spektrafilm` | same, then `gainmap` | same | `tonemap_engine` | same as still |

Highlight reconstruction has two methods, colour propagation and Inpaint
Opposed, and every pipeline runs the selected one; see
`raw_highlight/README.md`.

## Packages

| Package | Purpose |
|---|---|
| `vk_common` | Borrowed-handle Vulkan helpers (`GpuContext`, `OwnedImage`), `embed_spirv()` CMake, host test helpers (`testing/`). |
| `shader_common` | Shared GLSL: CFA conventions (`cfa_common.glsl`), Camera2 lens shading (`lens_shading.glsl`), and the matching host layout. |
| `raw_preview` | Viewfinder RAW development: 2×2 demosaic, LSC, WB, clip state, highlight dispatch. |
| `raw_highlight` | Highlight reconstruction for both methods: shaders, `GuideChain` (colour propagation), `ColoroppProcessor` (Inpaint Opposed), shared Coloropp parameters. |
| `raw_demosaic` | Still demosaicers (RCD, VNG4, Dual blend, quadfix pre-filter) and the shared input stage (`common/`). |
| `video_pipeline` | Recording-resolution RAW stage: fused MHC demosaic, crop / 2× reduction, LSC, clip state. |
| `post_demosaic` | Post-demosaic still/video stages (`rawr::post::PostDemosaicProcessor`). |
| `raw_denoise` | Profiled wavelet denoise (darktable denoiseprofile port). |
| `false_color_correction` | RawTherapee false-colour suppression. |
| `capture_sharpening` | RawTherapee capture sharpening (Richardson-Lucy deconvolution) on demosaiced pre-WB RGB. |
| `multiframe` | Multiframe burst: `alignment`, `merge_wronski`, `pipeline` (executor, recorder, burst coordinator), `output` (CFA projection, JPEG-path RGB), `sharpness` (reference-frame scoring). |
| `tonemap_engine` | Scene-linear to display rendering, LUTs, colour transforms. |
| `spektrafilm` | Film simulation. |
| `gainmap` | UltraHDR gain map compute and muxing. |
| `image_scopes`, `monitoring_overlays` | Scopes and exposure/focus overlays. |
| `tinydng` | DNG writing (streaming writer, LosslessJPEG default) and reading. |
| `zsl_codec`, `zsl_ring`, `zsl_container` | Zero-shutter-lag RAW compression and ring buffer. |
| `mcraw` | MotionCam `.mcraw` RAW video: lossless codec (compression type 7) and container writer/recovery. |
| `libjpeg-turbo` | Vendored JPEG codec. |
