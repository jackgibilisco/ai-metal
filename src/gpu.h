#pragma once

// The graphics-API boundary. app.h, renderer.h, and ui_render.h name these
// two types only as opaque pointers, so the app, game, and UI cores never see
// Metal (or, on a Windows port, D3D). The active backend defines them:
// gpu_metal.h for the Metal backend.

struct GpuContext;   // device plus the swapchain formats a backend needs at init
struct RenderTarget; // one frame's command buffer plus its drawable
