# Nintendo DSP-ADPCM Encoder Tool

This is a free reimplementation of Nintendo's DSPADPCM encoder tool
for generating GC-DSPADPCM data targeting GameCube, Wii and Wii U
systems.

## Compiling steps

1. Clone the Git repository

```bash
git clone https://github.com/jackoalan/gc-dspadpcm-encode.git
```

2. Install GCC for your operating system.

_Linux/Ubuntu_

```bash
sudo apt-get update && sudo apt-get install gcc
```

_Microsoft Windows_

For Microsoft Windows, you can take a look at <https://github.com/devstrons/heloworld/blob/main/lang/cpp.md#-microsoft-windows> that will guide you to install MinGW and GCC.\
Alternatively, you can download and install [Chocolatey](https://chocolatey.org/) and run the following command:

```bash
choco install mingw
```

3. Once you have GCC installed, you can compile the program by running the following command.

```bash
gcc -o dsp_encoder main.c grok.c -lm
```

> **Note**
>
> The `lm` flag is required for the `lround` C function.

4. Run the program in your terminal. The program will print the usage of how to properly run it.

```bash
./dsp_encoder
```
