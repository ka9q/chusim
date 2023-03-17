This is a hack of wwvsim to generate CHU's time signal audio. Although
it generates both English and French announcements it is much less
complete than wwvsim. It only writes PCM audio to stdout; it does not
yet use portaudio to write directly to the system sound device with
minimum delay. The time code bursts are also unimplemented; only a
mark tone is generated.

I'm also told that the male French voice on MacOS speaks with a
metropolitan (European) French accent, not a Canadian French accent.
