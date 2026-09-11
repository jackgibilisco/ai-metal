# AI Metal Renderer

This project is mainly a way for me to experiment with my Claude setup to try to get it to generate human-readable code without comment spam.
I have also worked on maximizing token efficiency by clearing and compacting when necessary, using efficiency pre-tool hooks, and several skill files.
I've also been playing around with how much I have to specify in the prompt vs how much it can figure out on its own.

Here was the initial prompt to get a basic renderer:

```
Start a new project here.
Create a barebones, lightweight c-style c++ renderer that uses metal to draw cubes to the screen.
The api should look like this: platform layer calls Init, FrameUpdate, and FrameRender functions.
It passes a memory arena to those functions that will hold all of the program memory.
They should be allocated at the start of the program and de-allocated by operating system when the program closes,
with no memory allocated while the program is running.
FrameUpdate should be completely platform agnostic, and FrameRender should be operating system agnostic, but not rendering api agnostic.
For testing purposes, spawn 3 cubes on the first frame and rotate them every frame in FrameUpdate.
```

So far I've written no code by hand for this project, and I intend to keep it that way as long as I can.

# Build Instructions

Clone the repository

`git clone https://github.com/jackgibilisco/ai-metal.git`

Enter the project directory

`cd ai-metal`

Build and run the program

`make run`
