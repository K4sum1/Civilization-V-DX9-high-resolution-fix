Ever been wanting to play Civ 5 and encountered this?

<img width="640" height="360" alt="CivilizationV_2026_10_27_08_59_54_555" src="https://github.com/user-attachments/assets/adaf84a2-cbde-403c-a22f-681e6f3e7439" />

or perhaps this?

<img width="640" height="400" alt="image" src="https://github.com/user-attachments/assets/9f6ad8c6-5e6d-4743-b83a-ed57078d0864" />
<img width="640" height="400" alt="image" src="https://github.com/user-attachments/assets/ee4210c0-05cd-4395-8c70-31aead703258" />

Then you've come to the right place, with this fix you can play the DirectX 9 version of Civ 5 in whatever resolution you want with this fix. (theoretically up to 16384x16384)

<img width="960" height="400" alt="image" src="https://github.com/user-attachments/assets/4312eb97-d62f-4be0-a971-97a5d35b7598" />

Simply place the d3d9.dll in your game's directory and your resolution issues will be gone.

# What does this do?
Civ 5 in DX9 mode allocates a depth stencil of 2048x2048 for the map, which in 2010 may have been good and all, but nowadays you probably have a monitor with a resolution higher than 1920x1200. DirectX 9 requires the depth stencil to be at least as big as the texture it is meant for. Which, the map is being rendered as a texture with a depth stencil of 2048x2048. So once the resolution goes above 2048 in one axis, it breaks the game. For AMD cards it's not so bad, you just see the grey bar in the first screenshot. However, for Nvidia cards, the map does not render at all and you get whatever that is in the second screenshot. The one exception to all this is VMware's virtual GPU, it just does not care and just renders the map lol.

What this patch does is it injects into the game and increases the size of the depth stencil to correspond to your resolution. 2560x1440 gets a depth stencil of 4096x2048, 3840x2160 gets a depth stencil of 4096x4096*, 5120 × 2880 gets a depth stencil of 6144x4096* and so on.

*Due to my highest resolution display being 3840x1600, I have not tested if this works exactly beyond 4096x2048, but it should™. I have however well tested it at 2560x1440 and 3840x1600 on AMD and Nvidia and encountered no issues stemming from this mod.

I have also designed this to be a bit game agnostic, so if another game has a similar issue, try it, maybe it'll work idk. Other functions games may need from d3d9.dll are exported and get sent to the system d3d9.dll. Civ 5 DX11 requires D3DPERF_BeginEvent, and D3DPERF_EndEvent from d3d9.dll and if this mod exported only what is required for Civ 5 DX9, Direct3DCreate9, it would not work. So to also have the ability to test the DX11 version of the game if needed, and in case it could work for some other game, it exports other d3d9.dll functions that could also be needed.

# How was this made?
Even though I have some other projects, I'm not a coder, and I don't exactly know what I'm doing. I kinda just plug code in from other stuff and if it errors i just tweak and modify it until it fits, or simply give up and make it an issue with the help wanted tag. I'm unable to make a new project like this one by myself.

Except now AI exists so I gave the problem to duck.ai and with some diagnosing and looking around with Apitrace, guided by GPT-6 Luna, I discovered the depth stencil issue, and had it try increasing it, and that was how the fix was made. I also had it make a few other improvements and got a second opinion from Haiku 4.5 to the point where it fills the free chat window up in just a few messages lol. Also yes this means it didn't cost me anything. I'm poor lol.

# How to build?
I wanted to keep XP compatibility, being that Civ 5 had officially supported and ran on this platform, and especially since DX9 is the only option for XP. So I installed the VS2017 build tools with the XP toolchain.

Then using the `x64_x86 Cross Tools Command Prompt for VS 2017`, I cd'd to the source directory and ran this to compile it.

`cl /nologo /EHsc /LD d3d9proxy.cpp d3d9.def /link /OUT:d3d9.dll`

If you want, you can open the `x64 Native Tools Command Prompt for VS 2017`, and run this command to get a x64 d3d9.dll. Due to Civilization 5 being a 32 bit game, this isn't very useful, but if you find a game with a similar issue and it's x64 maybe it could work for you. This also means I have not tested it due to having nothing to test it with, but it compiles fine.

`cl /nologo /EHsc /LD d3d9proxy.cpp d3d964.def /link /OUT:d3d9.dll`
