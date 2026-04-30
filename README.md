# Furmark-NX
![Bad logo](Screenshots/FRNXL2.jpeg)

Heavily based off [Furmark by StanislavPetrovV](https://github.com/StanislavPetrovV/FurMark/tree/main})

- A Stress Testing Utility for the Nintendo Switch.
- This tool currently has 4 stress tests avaliable, with 2 more in development.
- Given its purpose, if used on high enough clockspeeds and timespans, this will cause hardware damage.
- While Furmark can be used to find instability, it is generally not good at it and thus will only crash once much larger issues are present.

## Furmark
![Furmark 48STEP screnshot](/Screenshots/Furmark.png)

- Uses an OpenGL shader to draw as much power as possible
- Renders at 1280x720, using a path tracing setup
- The path tracer allows rays to take 48 steps before quitting.

## Furmark-RAM Stress
![Furmark RAM Screenshot](/Screenshots/Furmark-RAM.png)

- Uses the same core shader as Furmark, however this copies 4 textures every frame to stress memory.
- This also reduces the ray stepping to 32 steps, and changes the perspective of the render.

## GPU Path Tracing
![GPUPT Screenshot](/Screenshots/GPUPT.png)

- This uses a standard cornel box scene at 1280x720.
- Rays can take 80 steps, although this is more of a hard limit as rasing it simply produced lower results.
- Reducing the step count meanwhile only introduced visual glitches.
- Rays bounce 3 times before stopping.
- The image is then constantly sampled to denoise.

## CPU Ray Tracing
![CPURT Sceenshot](/Screenshots/CPUPT.png)

- Uses the same scene as GPUPT, however it now runs on the CPU.
- The same parameters are used, with necesarry changes made to run on CPU.
- This test heavily uses NEON piplines while using the GPU to display the final image.
- This runs on all avaliable threads (3) on stock atmosphere.
- Denoising is similar to GPUPT.
