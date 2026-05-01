# Disclaimer

This was vibe coded for myself, I'm not a beginer tho, works great.

# Loudness Equalizer

A program for PipeWire that keeps your volume at a steady level. It automatically brings up quiet voices and lowers loud explosions so you can hear everything clearly without constantly reaching for the volume knob.

## Features

*   **No more sudden spikes:** It sees loud sounds coming and lowers the volume just in time to stop them from blasting your ears.
*   **Focuses on voices:** It anchors the volume to the mid-range where people talk, so heavy bass in music or movies doesn't accidentally make the vocals too quiet.
*   **Smart and natural:** It automatically detects when you are listening to something like a podcast and slows down its adjustments to keep the sound natural and stop any "breathing" or "pumping" noise.
*   **Perfectly synced:** It tells your computer how much delay it adds so that movies and audio always stay perfectly in sync.

## Dependencies

### Debian
`sudo apt install libpipewire-0.3-dev libspa-0.2-dev pkg-config make gcc pulseaudio-utils pipewire-bin`

### Arch Linux
`sudo pacman -S pipewire pkgconf make gcc`

## How to Use

1.  **Build the program:**
    ```bash
    make
    ```

2.  **Start it:**
    ```bash
    ./reload.sh
    ```
    This will create a new "Virtual Sink" and route all your computer's audio through the equalizer. Press **Ctrl+C** in the terminal to stop it and go back to normal.

## Configuration

If you want to change how it behaves, you can set these in your terminal before running it:
*   `TARGET`: How loud you want the output to be (Default is 0.25).
*   `GATE`: Ignores very quiet background noise/hiss (Default is 0.001).
