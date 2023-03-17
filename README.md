This is a hack of wwvsim to generate CHU's time signal audio. Although
it generates both English and French announcements it is much less
complete than wwvsim. It only writes PCM audio to stdout; it does not
yet use portaudio to write directly to the system sound device with
minimum delay. The time code bursts are also unimplemented; only a
mark tone is generated.

The audio output is raw 48 kHz PCM, 16 bits per sample, 1 channel, signed integer, little endian. To listen, do something like

chusim | play -t raw -r 48000 -c 1 -b 16 -e signed-integer -

(the 'play' command is implemented by the 'sox' audio package)

